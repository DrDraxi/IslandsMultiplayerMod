#pragma once

#include "NetworkProtocol.h"
#include <Unreal/UObject.hpp>
#include "SteamIntegration.h"
#include <steam/steam_api.h>
#include <cstdint>
#include <string>
#include <vector>

namespace IslandsMP
{
    // ---- Game thread only ------------------------------------------------------

    void InitPlayerSync();
    void TickPlayerSync(float deltaTime);
    void ShutdownPlayerSync();

    // A new map is about to load: every actor we know about is about to be destroyed.
    void OnMapLoadStarting();

    void OnNetworkMessage(CSteamID sender, const uint8_t* data, size_t len);

    // Session
    void HostSession();         // friends-only
    void FindPublicSession();   // join or start a public one
    void LeaveSession();
    // Title-screen buttons: the session to start once the world has loaded.
    void ChooseSessionFromMenu(SessionMode mode);
    bool IsInWorld();

    // Chat: send a UTF-8 line to everyone in the session (false if not in one).
    bool SendChat(const std::string& text);
    std::string LocalDisplayName();
    RC::Unreal::UObject* LocalPlayerController();

    // Debug tools
    void ToggleLoopbackClone();        // puppet driven by our own packets, through the full network path
    void ToggleStressClones();         // 8 extra local puppets in a ring, random looks
    void SpawnStaticClone(float distance);
    void ClearStaticClones();
    void ForceRespawnRemotes();
    void SetRemoteScale(float scale);
    void SetNameplateLayoutAll(float pivotX, float pivotY, float height);
    void SetLookTuning(bool enabled, bool invertYaw, bool invertPitch);
    void SetMapMarkers(bool enabled);
    void TeleportToPlayer(int index);
    void DumpLocalAppearance();
    void DumpLocalPlayerDetails();

    // ---- Any thread (ImGui) -----------------------------------------------------

    struct UIRemote
    {
        uint64_t steamID = 0;
        std::string name;
        float posX = 0, posY = 0, posZ = 0;
        float distance = 0;
        AnimState anim = AnimState::Idle;
        bool hasActor = false;
        bool hasAppearance = false;
        bool fake = false;
    };

    struct UISnapshot
    {
        std::string status = "Starting";
        bool inSession = false;
        std::string mode = "Solo";
        bool localFound = false;
        PlayerStateData local{};
        std::vector<std::string> lobby;
        std::vector<UIRemote> remotes;
        bool loopbackActive = false;
        bool stressActive = false;
    };

    UISnapshot GetUISnapshot();
}
