#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <imgui.h>
#include "GameDiscovery.h"
#include "SteamIntegration.h"
#include "PlayerSync.h"
#include "MainMenu.h"
#include <steam/steam_api.h>
#include <cmath>
#include <functional>
#include <mutex>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

// Threading model
// ---------------
// UE4SS calls on_update() and draws ImGui on its own threads, not the game thread.
// Touching UObjects from those threads races the engine (GC, physics, world teardown)
// and was the cause of the random crashes in 0.1.0. So:
//   * all game/Steam logic runs from a UEngine::Tick post-hook (game thread),
//   * the ImGui tab only reads a snapshot and queues commands for the game thread.

namespace
{
    std::mutex g_cmdMutex;
    std::vector<std::function<void()>> g_commands;

    void Post(std::function<void()> fn)
    {
        std::lock_guard lock(g_cmdMutex);
        g_commands.push_back(std::move(fn));
    }

    void RunCommands()
    {
        std::vector<std::function<void()>> cmds;
        {
            std::lock_guard lock(g_cmdMutex);
            cmds.swap(g_commands);
        }
        for (auto& c : cmds) c();
    }
}

class IslandsMultiplayerMod : public RC::CppUserModBase
{
public:
    IslandsMultiplayerMod() : CppUserModBase()
    {
        ModName = STR("IslandsMultiplayerMod");
        ModVersion = STR("0.5.1");
        ModDescription = STR("Adds multiplayer presence to Islands of Insight");
        ModAuthors = STR("Draxi");

        register_tab(STR("Islands MP"), [](CppUserModBase*) { DrawTab(); });
    }

    ~IslandsMultiplayerMod() override
    {
        for (auto id : m_hooks) Hook::UnregisterCallback(id);
        IslandsMP::ShutdownPlayerSync();
        IslandsMP::ShutdownSteamIntegration();
    }

    auto on_unreal_init() -> void override
    {
        Hook::FCallbackOptions opts{};
        opts.OwnerModName = STR("IslandsMultiplayerMod");

        opts.HookName = STR("GameThreadTick");
        m_hooks.push_back(Hook::RegisterEngineTickPostCallback(
            [this](Hook::TCallbackIterationData<void>&, UEngine*, float dt, bool) { GameThreadTick(dt); }, opts));

        opts.HookName = STR("LoadMapPre");
        m_hooks.push_back(Hook::RegisterLoadMapPreCallback(
            [this](Hook::TCallbackIterationData<bool>&, UEngine*, FWorldContext&, FURL, UPendingNetGame*, FString&) {
                if (m_ready) IslandsMP::OnMapLoadStarting();
            }, opts));

        Output::send<LogLevel::Default>(STR("[IslandsMP] Hooks registered\n"));
    }

    auto on_ui_init() -> void override
    {
        UE4SS_ENABLE_IMGUI();
    }

private:
    std::vector<Hook::GlobalCallbackId> m_hooks;
    float m_initDelay = 0.0f;
    bool m_ready = false;

    void GameThreadTick(float dt)
    {
        if (dt > 1.0f) dt = 0.016f; // map loads

        if (!m_ready)
        {
            // Give the game time to bring Steam and the first world up.
            m_initDelay += dt;
            if (m_initDelay < 5.0f) return;
            if (ISteamUser* user = SteamUser())
                Output::send<LogLevel::Default>(STR("[IslandsMP] Steam OK, SteamID {}\n"),
                    std::to_wstring(user->GetSteamID().ConvertToUint64()));
            IslandsMP::InitSteamIntegration();
            IslandsMP::InitPlayerSync();
            m_ready = true;
            Output::send<LogLevel::Default>(STR("[IslandsMP] Ready\n"));
            return;
        }

        RunCommands();
        IslandsMP::Tick();
        IslandsMP::TickMainMenu(dt);
        IslandsMP::TickPlayerSync(dt);
    }

    static void DrawTab()
    {
        using namespace IslandsMP;
        UISnapshot s = GetUISnapshot();

        ImGui::Text("Islands of Insight Multiplayer v0.5.1");
        ImGui::Separator();

        ImGui::Text("Status: %s (%s)", s.status.c_str(), s.mode.c_str());
        ImGui::SameLine();
        if (!s.inSession)
        {
            if (ImGui::Button("Friends session")) Post([] { HostSession(); });
            ImGui::SameLine();
            if (ImGui::Button("Public session")) Post([] { FindPublicSession(); });
        }
        else
        {
            if (ImGui::Button("Invite")) Post([] { OpenInviteDialog(); });
            ImGui::SameLine();
            if (ImGui::Button("Leave")) Post([] { LeaveSession(); });
        }

        ImGui::Text("Lobby:");
        for (const auto& name : s.lobby) ImGui::BulletText("%s", name.c_str());

        ImGui::Separator();
        if (s.localFound)
        {
            const char* animNames[] = {"Idle", "Walk", "Run", "Jump", "Fall", "Glide", "Crouch", "Crouch walk"};
            int ai = static_cast<int>(s.local.animState);
            float speed = std::sqrt(s.local.velX * s.local.velX + s.local.velY * s.local.velY);
            ImGui::Text("You: %.0f, %.0f, %.0f  Yaw %.0f  Speed %.0f  %s", s.local.posX, s.local.posY,
                s.local.posZ, s.local.yaw, speed, (ai >= 0 && ai < 8) ? animNames[ai] : "?");
        }
        else
        {
            ImGui::Text("Local player: searching...");
        }

        ImGui::Separator();
        ImGui::Text("Remote players: %d", static_cast<int>(s.remotes.size()));
        for (int i = 0; i < static_cast<int>(s.remotes.size()); i++)
        {
            const auto& r = s.remotes[i];
            ImGui::Text("  %s  %.0fm  %s%s", r.name.c_str(), r.distance / 100.0f,
                r.hasActor ? "" : "(no puppet) ", r.hasAppearance ? "" : "(default look)");
            ImGui::SameLine();
            char label[32];
            snprintf(label, sizeof(label), "TP##%d", i);
            if (ImGui::SmallButton(label)) Post([i] { TeleportToPlayer(i); });
        }

        static float npPivotX = 0.57f, npPivotY = 1.00f, npHeight = 0.0f;
        bool npChanged = ImGui::SliderFloat("Name card pivot X", &npPivotX, -1.0f, 2.0f, "%.2f");
        npChanged |= ImGui::SliderFloat("Name card pivot Y", &npPivotY, -1.0f, 2.0f, "%.2f");
        npChanged |= ImGui::SliderFloat("Name card height", &npHeight, -50.0f, 150.0f, "%.0f");
        if (npChanged)
        {
            float px = npPivotX, py = npPivotY, h = npHeight;
            Post([px, py, h] { SetNameplateLayoutAll(px, py, h); });
        }

        static bool mapMarkers = true;
        if (ImGui::Checkbox("Map markers", &mapMarkers))
        {
            bool v = mapMarkers;
            Post([v] { SetMapMarkers(v); });
        }

        static bool lookOn = true, lookInvYaw = false, lookInvPitch = false;
        bool lookChanged = ImGui::Checkbox("Head look", &lookOn);
        ImGui::SameLine();
        lookChanged |= ImGui::Checkbox("Invert look yaw", &lookInvYaw);
        ImGui::SameLine();
        lookChanged |= ImGui::Checkbox("Invert look pitch", &lookInvPitch);
        if (lookChanged)
        {
            bool a = lookOn, b = lookInvYaw, c = lookInvPitch;
            Post([a, b, c] { SetLookTuning(a, b, c); });
        }

        static float remoteScale = 1.0f;
        if (ImGui::SliderFloat("Remote scale", &remoteScale, 0.5f, 5.0f, "%.1fx"))
        {
            float v = remoteScale;
            Post([v] { SetRemoteScale(v); });
        }

        ImGui::Separator();
        ImGui::Text("Local testing");
        if (ImGui::Button(s.loopbackActive ? "Stop loopback clone" : "Start loopback clone"))
            Post([] { ToggleLoopbackClone(); });
        ImGui::SameLine();
        ImGui::TextDisabled("(your own packets, 3m to the side, 120ms late)");
        if (ImGui::Button(s.stressActive ? "Stop stress test" : "Stress test: 8 random clones"))
            Post([] { ToggleStressClones(); });
        ImGui::SameLine();
        ImGui::TextDisabled("(ring around you, random looks, follow your moves)");

        static float spawnDist = 500.0f;
        ImGui::SetNextItemWidth(100);
        ImGui::InputFloat("##dist", &spawnDist, 100.0f, 500.0f, "%.0f");
        ImGui::SameLine();
        if (ImGui::Button("Spawn static clone"))
        {
            float d = spawnDist;
            Post([d] { SpawnStaticClone(d); });
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear clones")) Post([] { ClearStaticClones(); });
        if (ImGui::Button("Respawn all puppets")) Post([] { ForceRespawnRemotes(); });

        ImGui::Separator();
        if (ImGui::Button("Log my appearance")) Post([] { DumpLocalAppearance(); });
        ImGui::SameLine();
        if (ImGui::Button("Dump player")) Post([] { DumpLocalPlayerDetails(); });

        static char dumpLabel[64] = "default";
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("##dumpLabel", dumpLabel, sizeof(dumpLabel));
        ImGui::SameLine();
        if (ImGui::Button("Dump objects"))
        {
            std::string label = dumpLabel;
            Post([label] { DumpAllObjectsToFile(label.c_str()); });
        }
    }
};

#define ISLANDS_MP_API __declspec(dllexport)
extern "C"
{
    ISLANDS_MP_API RC::CppUserModBase* start_mod()
    {
        return new IslandsMultiplayerMod();
    }

    ISLANDS_MP_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
