#pragma once

#include <steam/steam_api.h>
#include <steam/isteammatchmaking.h>
#include <steam/isteamnetworkingmessages.h>
#include <steam/isteamnetworkingsockets.h>
#include "NetworkProtocol.h"
#include <string>
#include <vector>
#include <functional>

namespace IslandsMP
{
    enum class SessionState
    {
        Disconnected,
        CreatingSession,
        Hosting,
        JoiningSession,
        Connected,
        Searching,      // looking for a public session
        WaitingForSteam // Steam is offline; the session starts once it reconnects
    };

    // What kind of session to be in once we're in the world.
    enum class SessionMode
    {
        Solo,     // no session (invites still work)
        Friends,  // friends-only lobby; friends join from Steam
        Public    // join any public session with room, or start one
    };

    constexpr int MAX_PLAYERS = 10;

    struct PlayerInfo
    {
        CSteamID steamID;
        std::string displayName;
        bool isHost;
    };

    // All of these must be called from the game thread.
    bool InitSteamIntegration();
    void ShutdownSteamIntegration();
    void HostLobby(); // friends-only
    // Friends: host a friends-only lobby. Public: join the fullest public lobby with room,
    // or host a public one. Solo: leave any lobby.
    void StartSession(SessionMode mode);
    SessionMode GetSessionMode();
    const char* GetSessionModeString(SessionMode mode);
    void LeaveLobby();
    void OpenInviteDialog();
    // Invite button: Steam invite dialog for our lobby; starts a friends session first if needed.
    void InviteFriends();
    // "X joined" / "X left" lines (UTF-8) since the last call.
    std::vector<std::string> TakeSessionEvents();
    // Players in public sessions running this mod version (title screen). -1 until known.
    void RefreshPublicPlayerCount();
    int GetPublicPlayerCount();
    SessionState GetSessionState();
    const char* GetStatusString();
    bool IsHost();
    CSteamID GetLobbyID();
    CSteamID GetLocalSteamID();
    const std::vector<PlayerInfo>& GetPlayers();
    bool IsLobbyMember(uint64_t steamID);
    std::string GetPlayerName(uint64_t steamID);

    // P2P messaging
    using MessageCallback = std::function<void(CSteamID sender, const uint8_t* data, size_t len)>;
    void SetMessageCallback(MessageCallback callback);
    void SendToPlayer(CSteamID target, const uint8_t* data, size_t len, int sendFlags);
    void SendToAll(const uint8_t* data, size_t len, int sendFlags);

    // Call every frame
    void Tick();
}
