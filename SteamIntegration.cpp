#include "SteamIntegration.h"
#include <DynamicOutput/DynamicOutput.hpp>
#include <chrono>

using namespace RC;

namespace IslandsMP
{
    static SessionState s_state = SessionState::Disconnected;
    static CSteamID s_lobbyID;
    static bool s_isHost = false;
    static std::vector<PlayerInfo> s_players;
    static MessageCallback s_messageCallback;
    static bool s_useNetworkingMessages = false; // true if ISteamNetworkingMessages available
    static SessionMode s_mode = SessionMode::Solo;
    static std::vector<std::string> s_sessionEvents;
    static int s_publicPlayers = -1;
    static double s_nextMergeCheck = 0;  // public host alone: look for a busier lobby
    static double s_retrySearchAt = 0;   // public: search again after a failed join

    static double NowSec()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static double s_nextSteamCheck = 0;

    // Whether the Steam client is connected to Steam's servers (lobbies need it).
    static bool SteamLoggedOn()
    {
        return SteamUser() && SteamUser()->BLoggedOn();
    }

    static void WaitForSteam()
    {
        if (s_state != SessionState::WaitingForSteam)
            Output::send<LogLevel::Warning>(STR("[IslandsMP] Steam is offline; the session will start when it reconnects\n"));
        s_state = SessionState::WaitingForSteam;
        s_nextSteamCheck = NowSec() + 2.0;
    }

    static constexpr int NET_CHANNEL = 0;
    static constexpr const char* MOD_VERSION = "0.6.8";

    static void RefreshPlayerList();
    static void ProcessIncomingMessages();
    static void JoinLobby(CSteamID lobby);
    static void InviteIfPending();
    static void CreateLobby(ELobbyType type);
    static void RequestPublicLobbies(bool merge);

    class SteamLobbyCallbacks
    {
    public:
        void OnLobbyCreated(LobbyCreated_t* result, bool ioFailure)
        {
            if (ioFailure || result->m_eResult != k_EResultOK)
            {
                Output::send<LogLevel::Warning>(STR("[IslandsMP] Lobby creation failed: {}\n"),
                    ioFailure ? -1 : static_cast<int>(result->m_eResult));
                // No connection to Steam's servers (outage, maintenance, network): wait for it
                // to come back and try again instead of giving up.
                if (ioFailure || result->m_eResult == k_EResultNoConnection || !SteamLoggedOn())
                    WaitForSteam();
                else
                    s_state = SessionState::Disconnected;
                return;
            }
            s_lobbyID = CSteamID(result->m_ulSteamIDLobby);
            s_isHost = true;
            s_state = SessionState::Hosting;

            SteamMatchmaking()->SetLobbyData(s_lobbyID, "mod_name", "IslandsMultiplayerMod");
            SteamMatchmaking()->SetLobbyData(s_lobbyID, "mod_version", MOD_VERSION);
            SteamMatchmaking()->SetLobbyData(s_lobbyID, "mode", s_mode == SessionMode::Public ? "public" : "friends");
            s_nextMergeCheck = NowSec() + 20.0;

            // Lets friends use "Join Game" from the Steam friends list.
            SteamFriends()->SetRichPresence("connect", ("+connect_lobby " + std::to_string(result->m_ulSteamIDLobby)).c_str());

            Output::send<LogLevel::Default>(STR("[IslandsMP] Lobby created! ID: {}. Invite friends via Steam overlay.\n"),
                std::to_wstring(result->m_ulSteamIDLobby));
            RefreshPlayerList();
            InviteIfPending();
        }

        void OnLobbyEnter(LobbyEnter_t* result, bool ioFailure)
        {
            if (ioFailure || result->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess)
            {
                Output::send<LogLevel::Warning>(STR("[IslandsMP] Failed to join lobby\n"));
                s_state = SessionState::Disconnected;
                // Public: the lobby filled up or closed in the meantime; look again.
                if (s_mode == SessionMode::Public) { s_state = SessionState::Searching; s_retrySearchAt = NowSec() + 2.0; }
                return;
            }

            CSteamID lobbyID(result->m_ulSteamIDLobby);

            const char* hostVersion = SteamMatchmaking()->GetLobbyData(lobbyID, "mod_version");
            if (!hostVersion || std::string(hostVersion) != MOD_VERSION)
            {
                std::string hv = hostVersion ? hostVersion : "none";
                Output::send<LogLevel::Warning>(STR("[IslandsMP] Version mismatch (host {}, us {}). Leaving lobby.\n"),
                    std::wstring(hv.begin(), hv.end()), STR("0.6.8"));
                SteamMatchmaking()->LeaveLobby(lobbyID);
                s_state = SessionState::Disconnected;
                return;
            }

            s_lobbyID = lobbyID;
            s_isHost = SteamMatchmaking()->GetLobbyOwner(lobbyID) == SteamUser()->GetSteamID();
            s_state = s_isHost ? SessionState::Hosting : SessionState::Connected;

            if (!s_isHost)
            {
                std::string host = SteamFriends()->GetFriendPersonaName(SteamMatchmaking()->GetLobbyOwner(lobbyID));
                s_sessionEvents.push_back("You joined " + host + "'s session");
            }
            Output::send<LogLevel::Default>(STR("[IslandsMP] Joined lobby! ID: {}\n"),
                std::to_wstring(result->m_ulSteamIDLobby));
            RefreshPlayerList();
            InviteIfPending();
        }

        // Public lobby search results. merge = we already host a public lobby on our own and
        // only move if there's somewhere better to be.
        void OnLobbyList(LobbyMatchList_t* result, bool ioFailure)
        {
            bool merge = m_listIsMerge;
            if (s_mode != SessionMode::Public) return;
            if (merge && !(s_state == SessionState::Hosting && s_players.size() <= 1)) return;
            if (!merge && s_state != SessionState::Searching) return;
            // The search can't succeed while Steam is offline; don't create a lobby that will
            // fail too, just wait for the connection.
            if (!merge && (ioFailure || !SteamLoggedOn())) { WaitForSteam(); return; }

            CSteamID best;
            int bestMembers = 0;
            int found = ioFailure ? 0 : static_cast<int>(result->m_nLobbiesMatching);
            for (int i = 0; i < found; i++)
            {
                CSteamID lobby = SteamMatchmaking()->GetLobbyByIndex(i);
                if (lobby == s_lobbyID) continue;
                int members = SteamMatchmaking()->GetNumLobbyMembers(lobby);
                if (members <= 0 || members >= MAX_PLAYERS) continue;
                if (members > bestMembers || (members == bestMembers && lobby.ConvertToUint64() < best.ConvertToUint64()))
                {
                    best = lobby;
                    bestMembers = members;
                }
            }
            Output::send<LogLevel::Default>(STR("[IslandsMP] Public search: {} lobbies, best has {} players\n"), found, bestMembers);

            if (merge)
            {
                // Two players who searched at the same moment each made a lobby: the one with the
                // higher lobby ID moves over, so they don't swap places.
                if (best.IsValid() && (bestMembers > 1 || best.ConvertToUint64() < s_lobbyID.ConvertToUint64()))
                {
                    Output::send<LogLevel::Default>(STR("[IslandsMP] Moving to a busier public session\n"));
                    JoinLobby(best);
                }
                return;
            }
            if (best.IsValid()) JoinLobby(best);
            else CreateLobby(k_ELobbyTypePublic);
        }

        CCallResult<SteamLobbyCallbacks, LobbyCreated_t> m_lobbyCreated;
        CCallResult<SteamLobbyCallbacks, LobbyEnter_t> m_lobbyEntered;
        CCallResult<SteamLobbyCallbacks, LobbyMatchList_t> m_lobbyList;

        // Player count for the title screen: everyone in public lobbies of this version.
        void OnCountList(LobbyMatchList_t* result, bool ioFailure)
        {
            m_countPending = false;
            if (ioFailure) return;
            int players = 0;
            for (uint32 i = 0; i < result->m_nLobbiesMatching; i++)
                players += SteamMatchmaking()->GetNumLobbyMembers(SteamMatchmaking()->GetLobbyByIndex(static_cast<int>(i)));
            s_publicPlayers = players;
        }
        CCallResult<SteamLobbyCallbacks, LobbyMatchList_t> m_countList;
        bool m_countPending = false;
        bool m_listIsMerge = false;

        STEAM_CALLBACK(SteamLobbyCallbacks, OnLobbyChatUpdate, LobbyChatUpdate_t)
        {
            if (CSteamID(pParam->m_ulSteamIDLobby) != s_lobbyID) return;
            CSteamID who(pParam->m_ulSteamIDUserChanged);
            if (who != SteamUser()->GetSteamID())
            {
                std::string name = SteamFriends()->GetFriendPersonaName(who);
                if (pParam->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered)
                    s_sessionEvents.push_back(name + " joined");
                else
                    s_sessionEvents.push_back(name + " left");
            }
            RefreshPlayerList();
            // The host may have left; ownership moves to someone else.
            s_isHost = SteamMatchmaking()->GetLobbyOwner(s_lobbyID) == SteamUser()->GetSteamID();
            if (s_state == SessionState::Connected && s_isHost) s_state = SessionState::Hosting;
        }

        STEAM_CALLBACK(SteamLobbyCallbacks, OnGameLobbyJoinRequested, GameLobbyJoinRequested_t)
        {
            Output::send<LogLevel::Default>(STR("[IslandsMP] Join request for lobby {}\n"),
                std::to_wstring(pParam->m_steamIDLobby.ConvertToUint64()));
            JoinLobby(pParam->m_steamIDLobby);
        }

        // "Join Game" from the friends list while we advertise a "connect" rich presence.
        STEAM_CALLBACK(SteamLobbyCallbacks, OnRichPresenceJoinRequested, GameRichPresenceJoinRequested_t)
        {
            std::string connect(pParam->m_rgchConnect);
            auto pos = connect.find("+connect_lobby ");
            if (pos == std::string::npos) return;
            uint64_t id = std::strtoull(connect.c_str() + pos + 15, nullptr, 10);
            if (id) JoinLobby(CSteamID(id));
        }

        // Accept Steam networking sessions from people in our lobby.
        STEAM_CALLBACK(SteamLobbyCallbacks, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t)
        {
            CSteamID peer = pParam->m_identityRemote.GetSteamID();
            if (IsLobbyMember(peer.ConvertToUint64()))
                SteamNetworkingMessages()->AcceptSessionWithUser(pParam->m_identityRemote);
        }
    };

    static SteamLobbyCallbacks* s_callbacks = nullptr;

    static void RefreshPlayerList()
    {
        s_players.clear();
        if (!s_lobbyID.IsValid()) return;

        int count = SteamMatchmaking()->GetNumLobbyMembers(s_lobbyID);
        CSteamID hostID = SteamMatchmaking()->GetLobbyOwner(s_lobbyID);

        for (int i = 0; i < count; i++)
        {
            CSteamID memberID = SteamMatchmaking()->GetLobbyMemberByIndex(s_lobbyID, i);
            PlayerInfo info;
            info.steamID = memberID;
            info.displayName = SteamFriends()->GetFriendPersonaName(memberID);
            info.isHost = (memberID == hostID);
            s_players.push_back(info);
        }
        Output::send<LogLevel::Default>(STR("[IslandsMP] {} players in lobby\n"), count);
    }

    static void JoinLobby(CSteamID lobby)
    {
        if (!s_callbacks) return;
        if (s_lobbyID.IsValid())
        {
            if (s_lobbyID == lobby) return;
            LeaveLobby(); // we auto-host on load, so drop that lobby before joining a friend's
        }
        s_state = SessionState::JoiningSession;
        SteamAPICall_t call = SteamMatchmaking()->JoinLobby(lobby);
        s_callbacks->m_lobbyEntered.Set(call, s_callbacks, &SteamLobbyCallbacks::OnLobbyEnter);
    }

    bool InitSteamIntegration()
    {
        if (!SteamUser())
        {
            Output::send<LogLevel::Warning>(STR("[IslandsMP] Steam not available\n"));
            return false;
        }

        if (!s_callbacks)
            s_callbacks = new SteamLobbyCallbacks();

        s_useNetworkingMessages = SteamNetworkingMessages() != nullptr;
        Output::send<LogLevel::Default>(s_useNetworkingMessages
            ? STR("[IslandsMP] Using ISteamNetworkingMessages API\n")
            : STR("[IslandsMP] ISteamNetworkingMessages unavailable, using ISteamNetworking P2P fallback\n"));

        // Launched through "Join Game" while the game was closed: +connect_lobby <id>
        char cmd[256]{};
        if (SteamApps()->GetLaunchCommandLine(cmd, sizeof(cmd)) > 0)
        {
            std::string line(cmd);
            auto pos = line.find("+connect_lobby ");
            if (pos != std::string::npos)
            {
                uint64_t id = std::strtoull(line.c_str() + pos + 15, nullptr, 10);
                if (id) JoinLobby(CSteamID(id));
            }
        }

        Output::send<LogLevel::Default>(STR("[IslandsMP] Steam integration initialized\n"));
        return true;
    }

    void ShutdownSteamIntegration()
    {
        LeaveLobby();
        delete s_callbacks;
        s_callbacks = nullptr;
    }

    static void CreateLobby(ELobbyType type)
    {
        if (!s_callbacks) return;
        s_state = SessionState::CreatingSession;
        SteamAPICall_t call = SteamMatchmaking()->CreateLobby(type, MAX_PLAYERS);
        s_callbacks->m_lobbyCreated.Set(call, s_callbacks, &SteamLobbyCallbacks::OnLobbyCreated);
    }

    static void RequestPublicLobbies(bool merge)
    {
        if (!s_callbacks) return;
        ISteamMatchmaking* mm = SteamMatchmaking();
        mm->AddRequestLobbyListStringFilter("mod_name", "IslandsMultiplayerMod", k_ELobbyComparisonEqual);
        mm->AddRequestLobbyListStringFilter("mod_version", MOD_VERSION, k_ELobbyComparisonEqual);
        mm->AddRequestLobbyListStringFilter("mode", "public", k_ELobbyComparisonEqual);
        mm->AddRequestLobbyListFilterSlotsAvailable(1);
        mm->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
        mm->AddRequestLobbyListResultCountFilter(50);
        s_callbacks->m_listIsMerge = merge;
        s_callbacks->m_lobbyList.Set(mm->RequestLobbyList(), s_callbacks, &SteamLobbyCallbacks::OnLobbyList);
    }

    void HostLobby()
    {
        StartSession(SessionMode::Friends);
    }

    void StartSession(SessionMode mode)
    {
        if (!s_callbacks) return;
        LeaveLobby();
        s_mode = mode;
        Output::send<LogLevel::Default>(STR("[IslandsMP] Starting session: {}\n"),
            std::wstring(GetSessionModeString(mode), GetSessionModeString(mode) + std::strlen(GetSessionModeString(mode))));
        if (mode != SessionMode::Solo && !SteamLoggedOn()) { WaitForSteam(); return; }
        switch (mode)
        {
        case SessionMode::Friends:
            CreateLobby(k_ELobbyTypeFriendsOnly);
            break;
        case SessionMode::Public:
            s_state = SessionState::Searching;
            RequestPublicLobbies(false);
            break;
        default:
            break;
        }
    }

    SessionMode GetSessionMode() { return s_mode; }

    void RefreshPublicPlayerCount()
    {
        // Don't overlap with a session search (lobby filters are shared per request).
        if (!s_callbacks || s_callbacks->m_countPending || s_state == SessionState::Searching || !SteamLoggedOn()) return;
        ISteamMatchmaking* mm = SteamMatchmaking();
        mm->AddRequestLobbyListStringFilter("mod_name", "IslandsMultiplayerMod", k_ELobbyComparisonEqual);
        mm->AddRequestLobbyListStringFilter("mod_version", MOD_VERSION, k_ELobbyComparisonEqual);
        mm->AddRequestLobbyListStringFilter("mode", "public", k_ELobbyComparisonEqual);
        mm->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
        mm->AddRequestLobbyListResultCountFilter(50);
        s_callbacks->m_countPending = true;
        s_callbacks->m_countList.Set(mm->RequestLobbyList(), s_callbacks, &SteamLobbyCallbacks::OnCountList);
    }

    int GetPublicPlayerCount() { return s_publicPlayers; }

    std::vector<std::string> TakeSessionEvents()
    {
        std::vector<std::string> out;
        out.swap(s_sessionEvents);
        return out;
    }

    const char* GetSessionModeString(SessionMode mode)
    {
        switch (mode)
        {
        case SessionMode::Friends: return "Friends only";
        case SessionMode::Public:  return "Public";
        default:                   return "Solo";
        }
    }

    void LeaveLobby()
    {
        if (s_lobbyID.IsValid())
        {
            SteamMatchmaking()->LeaveLobby(s_lobbyID);
            s_lobbyID = CSteamID();
            if (SteamFriends()) SteamFriends()->SetRichPresence("connect", nullptr);
            Output::send<LogLevel::Default>(STR("[IslandsMP] Left lobby\n"));
        }
        s_players.clear();
        s_isHost = false;
        s_state = SessionState::Disconnected;
    }

    void OpenInviteDialog()
    {
        if (s_lobbyID.IsValid() && SteamFriends())
            SteamFriends()->ActivateGameOverlayInviteDialog(s_lobbyID);
    }

    // Opened once a lobby exists (InviteFriends started one).
    static bool s_inviteWhenReady = false;

    void InviteFriends()
    {
        if (s_lobbyID.IsValid()) { OpenInviteDialog(); return; }
        // Solo, or a session that failed: start a friends session, invite when it's up.
        s_inviteWhenReady = true;
        if (s_state == SessionState::Disconnected) StartSession(SessionMode::Friends);
    }

    static void InviteIfPending()
    {
        if (!s_inviteWhenReady || !s_lobbyID.IsValid()) return;
        s_inviteWhenReady = false;
        OpenInviteDialog();
    }

    SessionState GetSessionState() { return s_state; }
    bool IsHost() { return s_isHost; }
    CSteamID GetLobbyID() { return s_lobbyID; }
    CSteamID GetLocalSteamID() { return SteamUser() ? SteamUser()->GetSteamID() : CSteamID(); }
    const std::vector<PlayerInfo>& GetPlayers() { return s_players; }

    bool IsLobbyMember(uint64_t steamID)
    {
        for (const auto& p : s_players)
            if (p.steamID.ConvertToUint64() == steamID) return true;
        return false;
    }

    std::string GetPlayerName(uint64_t steamID)
    {
        for (const auto& p : s_players)
            if (p.steamID.ConvertToUint64() == steamID) return p.displayName;
        return std::to_string(steamID);
    }

    const char* GetStatusString()
    {
        switch (s_state)
        {
        case SessionState::Disconnected:    return "Disconnected";
        case SessionState::CreatingSession: return "Creating Session...";
        case SessionState::Hosting:         return "Hosting";
        case SessionState::JoiningSession:  return "Joining...";
        case SessionState::Connected:       return "Connected";
        case SessionState::Searching:       return "Looking for a public session...";
        case SessionState::WaitingForSteam: return "Steam is offline, waiting to reconnect...";
        default:                            return "Unknown";
        }
    }

    void SetMessageCallback(MessageCallback callback) { s_messageCallback = callback; }

    void SendToPlayer(CSteamID target, const uint8_t* data, size_t len, int sendFlags)
    {
        if (s_useNetworkingMessages)
        {
            SteamNetworkingIdentity identity;
            identity.SetSteamID(target);
            SteamNetworkingMessages()->SendMessageToUser(identity, data, static_cast<uint32>(len),
                sendFlags | k_nSteamNetworkingSend_AutoRestartBrokenSession, NET_CHANNEL);
        }
        else
        {
            EP2PSend sendType = (sendFlags & k_nSteamNetworkingSend_Reliable)
                ? k_EP2PSendReliable : k_EP2PSendUnreliable;
            SteamNetworking()->SendP2PPacket(target, data, static_cast<uint32>(len), sendType);
        }
    }

    void SendToAll(const uint8_t* data, size_t len, int sendFlags)
    {
        if (!s_lobbyID.IsValid()) return;
        CSteamID myID = SteamUser()->GetSteamID();

        if (!s_isHost)
        {
            // Client: send only to host, who relays to everyone else.
            CSteamID hostID = SteamMatchmaking()->GetLobbyOwner(s_lobbyID);
            if (hostID != myID)
                SendToPlayer(hostID, data, len, sendFlags);
            return;
        }

        for (const auto& player : s_players)
        {
            if (player.steamID == myID) continue;
            SendToPlayer(player.steamID, data, len, sendFlags);
        }
    }

    static void HandleIncoming(CSteamID sender, const uint8_t* data, size_t len)
    {
        if (s_messageCallback)
            s_messageCallback(sender, data, len);

        // Host relays client packets to the other clients. Packets carry their origin
        // SteamID in the header, so receivers attribute them correctly.
        if (!s_isHost) return;
        MessageType type = GetMessageType(data, len);
        int flags = (type == MessageType::PlayerState) ? k_nSteamNetworkingSend_UnreliableNoDelay
                                                        : k_nSteamNetworkingSend_Reliable;
        CSteamID myID = SteamUser()->GetSteamID();
        for (const auto& player : s_players)
        {
            if (player.steamID == sender || player.steamID == myID) continue;
            SendToPlayer(player.steamID, data, len, flags);
        }
    }

    static void ProcessIncomingMessages()
    {
        if (s_useNetworkingMessages)
        {
            SteamNetworkingMessage_t* messages[64];
            int numMessages;
            while ((numMessages = SteamNetworkingMessages()->ReceiveMessagesOnChannel(NET_CHANNEL, messages, 64)) > 0)
            {
                for (int i = 0; i < numMessages; i++)
                {
                    SteamNetworkingMessage_t* msg = messages[i];
                    HandleIncoming(msg->m_identityPeer.GetSteamID(),
                        static_cast<const uint8_t*>(msg->m_pData), static_cast<size_t>(msg->m_cbSize));
                    msg->Release();
                }
            }
        }
        else
        {
            uint32 msgSize;
            while (SteamNetworking()->IsP2PPacketAvailable(&msgSize))
            {
                std::vector<uint8_t> buf(msgSize);
                uint32 bytesRead;
                CSteamID sender;
                if (SteamNetworking()->ReadP2PPacket(buf.data(), msgSize, &bytesRead, &sender))
                    HandleIncoming(sender, buf.data(), bytesRead);
            }
        }
    }

    void Tick()
    {
        // Steam came back after an outage: start the session that was waiting for it.
        if (s_state == SessionState::WaitingForSteam && s_callbacks && NowSec() >= s_nextSteamCheck)
        {
            s_nextSteamCheck = NowSec() + 2.0;
            if (SteamLoggedOn())
            {
                Output::send<LogLevel::Default>(STR("[IslandsMP] Steam is back online\n"));
                StartSession(s_mode);
            }
        }
        if (s_mode == SessionMode::Public && s_callbacks)
        {
            double now = NowSec();
            if (s_state == SessionState::Searching && s_retrySearchAt > 0 && now >= s_retrySearchAt)
            {
                s_retrySearchAt = 0;
                RequestPublicLobbies(false);
            }
            // Alone in our own public lobby: someone else may have started one at the same time.
            if (s_state == SessionState::Hosting && s_players.size() <= 1 && now >= s_nextMergeCheck)
            {
                s_nextMergeCheck = now + 20.0;
                RequestPublicLobbies(true);
            }
        }
        if (s_state == SessionState::Disconnected) return;
        ProcessIncomingMessages();
    }
}
