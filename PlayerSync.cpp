#include "PlayerSync.h"
#include "SteamIntegration.h"
#include "Appearance.h"
#include "UEUtil.h"
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>

using namespace RC;
using namespace RC::Unreal;

namespace IslandsMP
{
    using UE::Call;
    using UE::Handle;
    using UE::ObjProp;
    using UE::Prop;

    // ---- Tuning ---------------------------------------------------------------

    static constexpr float SEND_INTERVAL = 0.1f;          // state packets, 10 Hz
    static constexpr float APPEARANCE_CHECK = 1.0f;      // re-capture our look
    static constexpr float NAMEPLATE_NEAR = 1200.0f;     // full-size name cards within 12 m
    static constexpr float NAMEPLATE_FAR = 4000.0f;      // half size at 40 m, hidden beyond
    static constexpr float NAMEPLATE_FADE = 800.0f;      // fade out over the last 8 m
    static constexpr float APPEARANCE_RESEND = 5.0f;     // resend even if unchanged (late joiners)
    static constexpr float REMOTE_TIMEOUT = 15.0f;
    static constexpr float NOT_IN_LOBBY_GRACE = 3.0f;
    static constexpr float WORLD_GRACE = 2.0f;           // after the pawn appears, before spawning
    static constexpr float APPEARANCE_WAIT = 3.0f;       // spawn with default look if nothing arrives
    static constexpr float LOOPBACK_LATENCY = 0.12f;
    static constexpr float LOOPBACK_OFFSET = 300.0f;
    static constexpr uint64_t LOOPBACK_ID = 1;
    static constexpr uint64_t STATIC_CLONE_BASE = 1000;
    static constexpr int MAX_REMOTE_PLAYERS = 16;
    static constexpr uint64_t STRESS_ID_FIRST = 2;   // 2..9, local only
    static constexpr int STRESS_COUNT = 8;
    static constexpr float STRESS_RADIUS = 220.0f;
    static constexpr float WING_FADE_TIME = 0.4f;
    static constexpr float GLIDE_STABLE_TIME = 1.0f; // before we trust the wing fade values

    static const wchar_t* kPuppetClass = L"/Script/Engine.SkeletalMeshActor";

    struct AnimDef { const wchar_t* path; bool loop; float refSpeed; };
    static const AnimDef kAnims[] = {
        /* Idle    */ {L"/Game/ASophia/Characters/BaseMeshes/Animations/Idle.Idle", true, 0},
        /* Walking */ {L"/Game/ASophia/Characters/BaseMeshes/Animations/ALS_N_Walk_F.ALS_N_Walk_F", true, 200},
        /* Running */ {L"/Game/ASophia/Characters/BaseMeshes/Animations/ALS_N_Run_F.ALS_N_Run_F", true, 600},
        /* Jumping */ {L"/Game/ASophia/Characters/BaseMeshes/Animations/ThirdPerson_Jump.ThirdPerson_Jump", false, 0},
        /* Falling */ {L"/Game/ASophia/Characters/BaseMeshes/Animations/Hover_Idle.Hover_Idle", true, 0},
        /* Gliding */ {L"/Game/ASophia/Characters/BaseMeshes/Animations/FastFlight.FastFlight", true, 0},
        /* Crouch  */ {L"/Game/ASophia/Characters/BaseMeshes/Animations/ALS_CLF_Pose.ALS_CLF_Pose", true, 0},
        /* CrouchW */ {L"/Game/ASophia/Characters/BaseMeshes/Animations/ALS_CLF_Walk_F.ALS_CLF_Walk_F", true, 150},
    };
    static_assert(sizeof(kAnims) / sizeof(kAnims[0]) == static_cast<size_t>(AnimState::Count));

    static double Now()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    struct Vec3f { float x, y, z; };

    // ---- State ----------------------------------------------------------------

    struct RemotePlayer
    {
        uint64_t id = 0;
        bool fake = false;      // static debug clone, never times out
        bool localDebug = false; // driven by our own packets (loopback / stress), not a lobby member
        PlayerStateData from{}, to{}, shown{};
        float interpT = 1.0f, interpDur = SEND_INTERVAL;
        double lastRecv = 0, firstSeen = 0, notInLobbySince = 0;
        bool hasState = false;

        AppearanceDesc appearance;
        uint32_t appearanceHash = 0;
        bool hasAppearance = false;

        Handle actor, leader, visual; // leader = animated (hidden) body, visual = what you see
        uint32_t builtHash = 0;
        bool builtWithAppearance = false;
        double nextSpawnAttempt = 0;
        int spawnFailures = 0;

        AnimState playing = AnimState::Count;
        float playingRate = 1.0f;

        std::vector<GlidePart> glideParts; // wings etc., shown only while gliding
        Handle nameplate;
        bool nameplateFilled = false;
        bool nameplateShown = true;
        float nameplateScale = 1.0f, nameplateAlpha = 1.0f;
        bool placed = false;          // last transform sent to the actor
        float placedLoc[3] = {}, placedYaw = 0;

        // Map/minimap marker: a player state the map widget draws like an online player.
        Handle mapState;
        std::string mapName, mapSubtitle;

        // Shared puzzle cursor
        PuzzleCursorData cursor{};
        double cursorTime = 0;
        float cursorShown[3] = {0, 0, 0};
        bool cursorActive = false;         // cursorShown holds a point (panel frame)
        Handle cursorActor, cursorWidget;
        bool cursorTextSet = false;
        float glideBlend = 0.0f;
    };

    static std::map<uint64_t, RemotePlayer> s_remotes;

    static Handle s_pc, s_localPawn;
    static PlayerStateData s_localState{};
    static double s_worldReadyAt = 1e300;
    static bool s_autoSessionStarted = false;
    static SessionMode s_menuMode = SessionMode::Solo; // picked on the title screen
    static float s_sendTimer = 0, s_appearanceTimer = 0, s_appearanceResendTimer = 0, s_pawnCheckTimer = 0;
    static AppearanceDesc s_localAppearance;
    static uint32_t s_localAppearanceHash = 0;
    static bool s_haveLocalAppearance = false;
    static float s_remoteScale = 1.0f;
    static bool s_mapMarkers = true;
    static PuzzleCursorData s_localCursor{}; // our open grid puzzle + cursor point
    static LookTuning s_lookTuning;

    struct DelayedPacket { double due; std::vector<uint8_t> data; };
    static bool s_loopback = false;
    static bool s_stress = false;
    static uint32_t s_stressSeed = 1;
    static std::deque<DelayedPacket> s_loopbackQueue;
    static uint64_t s_nextStaticClone = STATIC_CLONE_BASE;

    static std::mutex s_uiMutex;
    static UISnapshot s_ui;

    // ---- Helpers ----------------------------------------------------------------

    static uint64_t LocalID()
    {
        return GetLocalSteamID().ConvertToUint64();
    }

    static bool WorldReady()
    {
        return s_localPawn.Get() && Now() >= s_worldReadyAt;
    }

    static void DestroyCursor(RemotePlayer& r)
    {
        if (auto* a = r.cursorActor.As<AActor>()) a->K2_DestroyActor();
        r.cursorActor.Reset();
        r.cursorWidget.Reset();
        r.cursorTextSet = false;
    }

    // Show the remote's puzzle cursor while we both have the same puzzle open.
    static void UpdateRemoteCursor(RemotePlayer& r, uint64_t id, float dt, double now)
    {
        bool show = r.cursor.puzzleHash != 0 && r.cursor.puzzleHash == s_localCursor.puzzleHash
                    && now - r.cursorTime < 1.0 && WorldReady();
        UObject* actor = r.cursorActor.Get();
        if (!show)
        {
            if (actor) MoveCursorMarker(actor, 0, 0, 0, false);
            r.cursorActive = false;
            return;
        }
        // The point is in the sender's puzzle-panel frame; place it on our own panel.
        const float target[3] = {r.cursor.x, r.cursor.y, r.cursor.z};
        if (!r.cursorActive) { std::copy(target, target + 3, r.cursorShown); r.cursorActive = true; }
        // Smooth toward the latest point (packets come at ~15 Hz).
        float k = std::min(1.0f, dt * 20.0f);
        for (int i = 0; i < 3; i++) r.cursorShown[i] += (target[i] - r.cursorShown[i]) * k;
        float world[3];
        if (!PuzzleLocalToWorld(r.cursorShown, world))
        {
            if (actor) MoveCursorMarker(actor, 0, 0, 0, false);
            return;
        }
        if (!actor)
        {
            UObject* widget = nullptr;
            actor = CreateCursorMarker(s_localPawn.Get(), world[0], world[1], world[2], &widget);
            if (!actor) return;
            r.cursorActor.Set(actor);
            r.cursorWidget.Set(widget);
            r.cursorTextSet = false;
            Output::send<LogLevel::Default>(STR("[IslandsMP] Puzzle cursor shown for {}\n"), id);
        }
        MoveCursorMarker(actor, world[0], world[1], world[2], true);
        if (!r.cursorTextSet && r.cursorWidget.Get())
        {
            std::string name = r.appearance.displayName.empty() ? GetPlayerName(id) : r.appearance.displayName;
            r.cursorTextSet = SetCursorMarkerText(r.cursorWidget.Get(), UE::Widen(name));
        }
    }

    static void DestroyMapMarker(RemotePlayer& r)
    {
        DestroyCursor(r);
        // The emitter is a component of the puppet actor and goes away with it.
        r.mapState.Reset();
        r.mapName.clear();
    }

    static void UpdatePuppetMapMarker(RemotePlayer& r, AActor* actor, const std::string& name)
    {
        UObject* emitter = r.mapState.Get();
        if (!emitter)
        {
            if (!s_mapMarkers) return;
            emitter = CreateMapMarker(actor, UE::Widen(name));
            r.mapState.Set(emitter);
            r.mapName.clear(); // fill name + subtitle below
            if (!emitter) return;
        }
        // Name only; a blank second line (an empty one makes the map show "Quest").
        std::string subtitle = " ";
        if (name != r.mapName || subtitle != r.mapSubtitle)
        {
            SetMapMarkerName(emitter, UE::Widen(name));
            SetMapMarkerSubtitle(emitter, UE::Widen(subtitle));
            r.mapName = name;
            r.mapSubtitle = subtitle;
        }
        UpdateMapMarker(emitter, r.shown.posX, r.shown.posY, r.shown.posZ, s_mapMarkers);
    }

    static void DestroyPuppet(RemotePlayer& r)
    {
        if (auto* a = r.actor.As<AActor>())
            a->K2_DestroyActor();
        r.actor.Reset();
        r.leader.Reset();
        r.visual.Reset();
        r.mapState.Reset();
        r.mapName.clear();
        r.glideParts.clear();
        r.glideBlend = 0.0f;
        r.playing = AnimState::Count;
        r.placed = false;
    }

    static void ApplyState(RemotePlayer& r, const PlayerStateData& s, double now)
    {
        if (!r.hasState)
        {
            r.from = r.to = r.shown = s;
            r.interpT = 1.0f;
            r.hasState = true;
        }
        else
        {
            float dx = s.posX - r.to.posX, dy = s.posY - r.to.posY, dz = s.posZ - r.to.posZ;
            bool teleport = dx * dx + dy * dy + dz * dz > 1500.0f * 1500.0f;
            r.from = teleport ? s : r.shown;
            r.to = s;
            r.interpT = teleport ? 1.0f : 0.0f;
            r.interpDur = std::clamp(static_cast<float>(now - r.lastRecv), 0.05f, 0.3f);
        }
        r.lastRecv = now;
    }

    // ---- Local player -----------------------------------------------------------

    static bool IsMenuPawn(UObject* pawn)
    {
        std::wstring cls = UE::ClassNameOf(pawn);
        return cls.find(L"Login") != std::wstring::npos || cls.find(L"FreeCam") != std::wstring::npos;
    }

    static void ResolveLocalPawn(float dt)
    {
        s_pawnCheckTimer += dt;
        bool haveValid = s_localPawn.Get() != nullptr;
        if (haveValid && s_pawnCheckTimer < 1.0f) return;
        s_pawnCheckTimer = 0;

        UObject* pc = s_pc.Get();
        if (!pc)
        {
            pc = UObjectGlobals::FindFirstOf(STR("PlayerController"));
            s_pc.Set(pc);
            pc = s_pc.Get(); // drops a controller that is already pending kill
            if (!pc) { s_pc.Reset(); s_localPawn.Reset(); return; }
        }

        UObject* pawn = ObjProp(pc, L"Pawn");
        if (!UE::IsLive(pawn) || IsMenuPawn(pawn))
        {
            s_localPawn.Reset();
            return;
        }
        if (s_localPawn.Get() == pawn) return;

        s_localPawn.Set(pawn);
        s_worldReadyAt = Now() + WORLD_GRACE;
        s_haveLocalAppearance = false;
        s_appearanceTimer = APPEARANCE_CHECK; // capture right away
        Output::send<LogLevel::Default>(STR("[IslandsMP] Local player: {}\n"), pawn->GetFullName());

        // Start the session picked on the title screen (Solo: none).
        if (!s_autoSessionStarted && GetSessionState() == SessionState::Disconnected)
        {
            s_autoSessionStarted = true;
            if (s_menuMode != SessionMode::Solo) StartSession(s_menuMode);
        }
    }

    static void UpdateLocalState(UObject* pawn)
    {
        if (UObject* root = ObjProp(pawn, L"RootComponent"))
        {
            if (auto* loc = Prop<float>(root, L"RelativeLocation"))
            {
                s_localState.posX = loc[0];
                s_localState.posY = loc[1];
                s_localState.posZ = loc[2];
            }
            if (auto* rot = Prop<float>(root, L"RelativeRotation"))
                s_localState.yaw = rot[1];
        }
        if (auto* control = Prop<float>(s_pc.Get(), L"ControlRotation"))
        {
            s_localState.lookPitch = control[0] > 180.0f ? control[0] - 360.0f : control[0];
            s_localState.lookYaw = control[1];
        }
        if (UObject* mesh = ObjProp(pawn, L"Mesh"))
            if (auto* rel = Prop<float>(mesh, L"RelativeLocation"))
                s_localState.meshZ = rel[2];

        uint8_t moveMode = 1, customMode = 0xFF;
        bool crouching = false;
        UObject* move = ObjProp(pawn, L"CharMoveComp");
        if (!move) move = ObjProp(pawn, L"CharacterMovement");
        if (move)
        {
            if (auto* vel = Prop<float>(move, L"Velocity"))
            {
                s_localState.velX = vel[0];
                s_localState.velY = vel[1];
                s_localState.velZ = vel[2];
            }
            if (auto* m = Prop<uint8_t>(move, L"MovementMode")) moveMode = *m;
            if (auto* c = Prop<uint8_t>(move, L"CustomMovementMode")) customMode = *c;
            Call crouch(move, L"IsCrouching");
            crouching = crouch.Invoke() && crouch.Ret<bool>();
        }

        float speed = std::sqrt(s_localState.velX * s_localState.velX + s_localState.velY * s_localState.velY);
        constexpr uint8_t MOVE_Falling = 3, MOVE_Flying = 5, MOVE_Custom = 6;
        constexpr uint8_t CUSTOM_Glide = 0;

        if ((moveMode == MOVE_Custom && customMode == CUSTOM_Glide) || moveMode == MOVE_Flying)
            s_localState.animState = AnimState::Gliding;
        else if (moveMode == MOVE_Falling)
            s_localState.animState = s_localState.velZ > 50.0f ? AnimState::Jumping : AnimState::Falling;
        else if (crouching)
            s_localState.animState = speed > 10.0f ? AnimState::CrouchWalking : AnimState::Crouching;
        else if (speed > 400.0f)
            s_localState.animState = AnimState::Running;
        else if (speed > 10.0f)
            s_localState.animState = AnimState::Walking;
        else
            s_localState.animState = AnimState::Idle;

    }

    // How long the local player has been continuously gliding / not gliding.
    static bool s_wasGliding = false;
    static float s_glideStateTime = 0.0f;

    static void UpdateGlideTimer(UObject* pawn, float dt)
    {
        bool gliding = s_localState.animState == AnimState::Gliding;
        if (gliding != s_wasGliding) { s_wasGliding = gliding; s_glideStateTime = 0.0f; }
        else
        {
            float before = s_glideStateTime;
            s_glideStateTime += dt;
            // Debug: one snapshot of every effect on the character, mid-glide and on the ground.
            static int glideLogs = 0, groundLogs = 0;
            if (before < 1.5f && s_glideStateTime >= 1.5f)
            {
                if (gliding && glideLogs < 3) { glideLogs++; LogGlideDebug(pawn, L"gliding"); }
                if (!gliding && groundLogs < 2) { groundLogs++; LogGlideDebug(pawn, L"on ground"); }
            }
        }
    }

    static int GlideStateForCapture()
    {
        if (s_glideStateTime < GLIDE_STABLE_TIME) return -1;
        return s_wasGliding ? 1 : 0;
    }

    // ---- Sending ----------------------------------------------------------------

    // Queue a copy of an outgoing packet for a local debug puppet, re-addressed to `id` and
    // shifted by (dx, dy) so it stands next to us.
    static void QueueLocalCopy(const uint8_t* data, size_t len, uint64_t id, float dx, float dy)
    {
        DelayedPacket p{Now() + LOOPBACK_LATENCY, std::vector<uint8_t>(data, data + len)};
        WriteHeader(p.data.data(), GetMessageType(data, len), id);
        if (GetMessageType(data, len) == MessageType::PlayerState && len >= PLAYER_STATE_MSG_SIZE)
        {
            PlayerStateData s;
            std::memcpy(&s, p.data.data() + sizeof(MessageHeader), sizeof(s));
            s.posX += dx;
            s.posY += dy;
            std::memcpy(p.data.data() + sizeof(MessageHeader), &s, sizeof(s));
        }
        s_loopbackQueue.push_back(std::move(p));
    }

    static void Loopback(const uint8_t* data, size_t len)
    {
        if (s_loopback)
            QueueLocalCopy(data, len, LOOPBACK_ID, LOOPBACK_OFFSET, 0);

        // Stress clones get our movement in a ring around us, but keep their own random look.
        if (s_stress && GetMessageType(data, len) == MessageType::PlayerState)
        {
            for (int i = 0; i < STRESS_COUNT; i++)
            {
                float a = i * 6.2831853f / STRESS_COUNT;
                QueueLocalCopy(data, len, STRESS_ID_FIRST + i, std::cos(a) * STRESS_RADIUS, std::sin(a) * STRESS_RADIUS);
            }
        }
    }

    static void SendState()
    {
        uint8_t buf[PLAYER_STATE_MSG_SIZE];
        WriteHeader(buf, MessageType::PlayerState, LocalID());
        std::memcpy(buf + sizeof(MessageHeader), &s_localState, sizeof(s_localState));
        if (GetSessionState() != SessionState::Disconnected)
            SendToAll(buf, sizeof(buf), k_nSteamNetworkingSend_UnreliableNoDelay);
        Loopback(buf, sizeof(buf));
    }

    static float s_cursorTimer = 0.0f;
    static constexpr float CURSOR_SEND_INTERVAL = 1.0f / 15.0f;

    static void UpdateAndSendCursor(UObject* pawn, float dt)
    {
        static float hookRetry = 0;
        if ((hookRetry += dt) > 1.0f) { hookRetry = 0; InstallPuzzleHooks(); }
        s_cursorTimer += dt;
        bool wasInPuzzle = s_localCursor.puzzleHash != 0;
        // Cheap check first; the mouse trace only runs when a packet is due (15 Hz).
        if (!IsLocalInPuzzle() && !wasInPuzzle) return;
        if (IsLocalInPuzzle() && wasInPuzzle && s_cursorTimer < CURSOR_SEND_INTERVAL) return;
        PuzzleCursorData c{};
        float pos[3];
        if (ReadLocalPuzzleCursor(pawn, c.puzzleHash, pos)) { c.x = pos[0]; c.y = pos[1]; c.z = pos[2]; }
        else c.puzzleHash = 0; // no point on the puzzle yet; don't send a cursor at the origin
        s_localCursor = c;

        // Send while in a puzzle, plus one "left the puzzle" packet.
        if ((c.puzzleHash == 0 && !wasInPuzzle) || (s_cursorTimer < CURSOR_SEND_INTERVAL && c.puzzleHash != 0 && wasInPuzzle))
            return;
        s_cursorTimer = 0;
        uint8_t buf[PUZZLE_CURSOR_MSG_SIZE];
        WriteHeader(buf, MessageType::PuzzleCursor, LocalID());
        std::memcpy(buf + sizeof(MessageHeader), &c, sizeof(c));
        if (GetSessionState() != SessionState::Disconnected)
            SendToAll(buf, sizeof(buf), k_nSteamNetworkingSend_UnreliableNoDelay);
        Loopback(buf, sizeof(buf));
    }

    static void SendAppearance()
    {
        if (!s_haveLocalAppearance) return;
        std::vector<uint8_t> blob = s_localAppearance.Serialize();
        std::vector<uint8_t> msg(sizeof(MessageHeader) + blob.size());
        WriteHeader(msg.data(), MessageType::Appearance, LocalID());
        std::memcpy(msg.data() + sizeof(MessageHeader), blob.data(), blob.size());
        if (GetSessionState() != SessionState::Disconnected)
            SendToAll(msg.data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        Loopback(msg.data(), msg.size());
    }

    static void UpdateLocalAppearance(UObject* pawn, float dt)
    {
        s_appearanceTimer += dt;
        s_appearanceResendTimer += dt;
        if (s_appearanceTimer < APPEARANCE_CHECK) return;
        s_appearanceTimer = 0;

        AppearanceDesc desc;
        if (!CaptureAppearance(pawn, desc, GlideStateForCapture())) return;
        if (SteamFriends()) desc.displayName = SteamFriends()->GetPersonaName();
        desc.title = UE::Narrow(ReadLocalTitle());
        desc.level = ReadLocalLevel();
        uint32_t hash = desc.Hash();
        bool changed = !s_haveLocalAppearance || hash != s_localAppearanceHash;
        // Name/title/level aren't part of the rebuild hash but still need to go out promptly.
        bool labelChanged = s_haveLocalAppearance && (desc.displayName != s_localAppearance.displayName
            || desc.title != s_localAppearance.title || desc.level != s_localAppearance.level);
        // Always keep the latest capture (parameter values, labels); the hash only decides
        // whether receivers rebuild.
        s_localAppearance = std::move(desc);
        s_localAppearanceHash = hash;
        s_haveLocalAppearance = true;
        if (changed || labelChanged) LogAppearance(s_localAppearance, L"local");
        if (labelChanged)
        {
            // Static clones are copies of us: keep their name card in step.
            for (auto& [id, r] : s_remotes)
            {
                if (!r.fake) continue;
                r.appearance.displayName = s_localAppearance.displayName;
                r.appearance.title = s_localAppearance.title;
                r.appearance.level = s_localAppearance.level;
                r.nameplateFilled = false;
            }
        }
        if (changed || labelChanged || s_appearanceResendTimer >= APPEARANCE_RESEND)
        {
            s_appearanceResendTimer = 0;
            SendAppearance();
        }
    }

    // ---- Receiving --------------------------------------------------------------

    static RemotePlayer* GetOrAddRemote(uint64_t id)
    {
        auto it = s_remotes.find(id);
        if (it != s_remotes.end()) return &it->second;
        if (static_cast<int>(s_remotes.size()) >= MAX_REMOTE_PLAYERS) return nullptr;
        RemotePlayer& r = s_remotes[id];
        r.id = id;
        r.firstSeen = Now();
        Output::send<LogLevel::Default>(STR("[IslandsMP] New remote player: {}\n"), id);
        return &r;
    }

    static void HandlePacket(uint64_t origin, const uint8_t* data, size_t len)
    {
        MessageHeader h;
        if (!ReadHeader(data, len, h)) return;
        const uint8_t* body = data + sizeof(MessageHeader);
        size_t bodyLen = len - sizeof(MessageHeader);

        switch (h.type)
        {
        case MessageType::PlayerState:
        {
            if (bodyLen < sizeof(PlayerStateData)) return;
            RemotePlayer* r = GetOrAddRemote(origin);
            if (!r) return;
            PlayerStateData s;
            std::memcpy(&s, body, sizeof(s));
            ApplyState(*r, s, Now());
            break;
        }
        case MessageType::Appearance:
        {
            RemotePlayer* r = GetOrAddRemote(origin);
            if (!r) return;
            ByteReader reader(body, bodyLen);
            AppearanceDesc desc;
            if (!desc.Deserialize(reader))
            {
                Output::send<LogLevel::Warning>(STR("[IslandsMP] Bad appearance packet from {}\n"), origin);
                return;
            }
            uint32_t hash = desc.Hash();
            if (r->hasAppearance && hash == r->appearanceHash)
            {
                // Same outfit; just take the latest mesh offset (crouch etc.) and nameplate text.
                std::copy(desc.meshOffset, desc.meshOffset + 3, r->appearance.meshOffset);
                r->appearance.meshYaw = desc.meshYaw;
                if (desc.displayName != r->appearance.displayName || desc.title != r->appearance.title
                    || desc.level != r->appearance.level)
                {
                    r->appearance.displayName = desc.displayName;
                    r->appearance.title = desc.title;
                    r->appearance.level = desc.level;
                    r->nameplateFilled = false;
                }
                return;
            }
            r->appearance = std::move(desc);
            r->appearanceHash = hash;
            r->hasAppearance = true;
            LogAppearance(r->appearance, L"remote");
            break;
        }
        case MessageType::PuzzleCursor:
        {
            if (bodyLen < sizeof(PuzzleCursorData)) return;
            RemotePlayer* r = GetOrAddRemote(origin);
            if (!r) return;
            std::memcpy(&r->cursor, body, sizeof(PuzzleCursorData));
            r->cursorTime = Now();
            break;
        }
        case MessageType::PlayerLeave:
        {
            auto it = s_remotes.find(origin);
            if (it == s_remotes.end()) return;
            Output::send<LogLevel::Default>(STR("[IslandsMP] Remote player {} left\n"), origin);
            DestroyPuppet(it->second);
            DestroyMapMarker(it->second);
            s_remotes.erase(it);
            break;
        }
        default:
            break;
        }
    }

    void OnNetworkMessage(CSteamID sender, const uint8_t* data, size_t len)
    {
        MessageHeader h;
        if (!ReadHeader(data, len, h)) return;
        // Our own id, or ids we reserve for local debug puppets.
        if (h.origin == LocalID() || h.origin < STATIC_CLONE_BASE + 1000)
            return;
        // Only the host may speak for someone else (relay); clients speak for themselves.
        uint64_t senderID = sender.ConvertToUint64();
        if (h.origin != senderID && senderID != SteamMatchmaking()->GetLobbyOwner(GetLobbyID()).ConvertToUint64())
            return;
        HandlePacket(h.origin, data, len);
    }

    // ---- Puppets ----------------------------------------------------------------

    static bool SpawnPuppet(RemotePlayer& r)
    {
        auto* pawn = s_localPawn.As<AActor>();
        UWorld* world = pawn ? pawn->GetWorld() : nullptr;
        auto* cls = static_cast<UClass*>(UE::FindClass(kPuppetClass));
        if (!world || !cls)
        {
            Output::send<LogLevel::Warning>(STR("[IslandsMP] Cannot spawn puppet (world {}, class {})\n"),
                world != nullptr, cls != nullptr);
            return false;
        }

        FVector loc{static_cast<double>(r.shown.posX), static_cast<double>(r.shown.posY), static_cast<double>(r.shown.posZ)};
        FRotator rot{0.0, static_cast<double>(r.shown.yaw), 0.0};
        AActor* actor = world->SpawnActor(cls, &loc, &rot);
        if (!actor)
        {
            Output::send<LogLevel::Warning>(STR("[IslandsMP] SpawnActor failed\n"));
            return false;
        }
        actor->SetActorEnableCollision(false);

        const AppearanceDesc look = r.hasAppearance ? r.appearance
                                                    : DefaultAppearance(true);
        std::vector<GlidePart> glideParts;
        UObject* visual = nullptr;
        UObject* leader = ApplyAppearance(actor, look, &glideParts, &visual);
        if (!leader)
        {
            actor->K2_DestroyActor();
            return false;
        }

        if (s_remoteScale != 1.0f)
        {
            double s = s_remoteScale;
            actor->SetActorScale3D(FVector{s, s, s});
        }

        r.actor.Set(actor);
        r.leader.Set(leader);
        r.visual.Set(visual ? visual : leader);
        r.glideParts = std::move(glideParts);
        r.glideBlend = 0.0f;
        r.nameplate.Set(CreateNameplate(actor, visual ? visual : leader));
        r.nameplateShown = true;
        r.nameplateScale = r.nameplateAlpha = 1.0f;
        r.nameplateFilled = false;
        r.builtHash = r.appearanceHash;
        r.builtWithAppearance = r.hasAppearance;
        r.playing = AnimState::Count;
        Output::send<LogLevel::Default>(STR("[IslandsMP] Spawned puppet for {}\n"), r.id);
        return true;
    }

    static void PlayAnim(RemotePlayer& r, UObject* leader)
    {
        AnimState want = r.to.animState;
        if (want >= AnimState::Count) want = AnimState::Idle;
        const AnimDef& def = kAnims[static_cast<int>(want)];

        float speed = std::sqrt(r.to.velX * r.to.velX + r.to.velY * r.to.velY);
        float rate = def.refSpeed > 0 ? std::clamp(speed / def.refSpeed, 0.5f, 1.6f) : 1.0f;

        if (want != r.playing)
        {
            UObject* anim = UE::FindByPath(def.path);
            if (!anim && want != AnimState::Idle)
                anim = UE::FindByPath(kAnims[0].path);
            if (!anim) return;

            // Playing an animation for a different skeleton asserts in some builds; check first.
            UObject* meshSkel = ObjProp(ObjProp(leader, L"SkeletalMesh"), L"Skeleton");
            UObject* animSkel = ObjProp(anim, L"Skeleton");
            if (meshSkel && animSkel && meshSkel != animSkel)
            {
                static std::wstring warned;
                if (warned != def.path)
                {
                    warned = def.path;
                    Output::send<LogLevel::Warning>(STR("[IslandsMP] Skeleton mismatch: {} vs {}\n"),
                        UE::PathOf(animSkel), UE::PathOf(meshSkel));
                }
                r.playing = want; // don't retry every frame
                return;
            }

            Call play(leader, L"PlayAnimation");
            play.Arg<UObject*>(0, anim);
            play.Arg<bool>(1, def.loop);
            play.Invoke();
            r.playing = want;
            r.playingRate = -1;
        }

        if (std::fabs(rate - r.playingRate) > 0.05f)
        {
            Call setRate(leader, L"SetPlayRate");
            setRate.Arg<float>(0, rate);
            setRate.Invoke();
            r.playingRate = rate;
        }
    }

    static void MovePuppet(RemotePlayer& r, AActor* actor, float dt)
    {
        if (r.interpT < 1.0f)
            r.interpT = std::min(1.0f, r.interpT + dt / r.interpDur);
        float t = r.interpT;

        r.shown = r.to;
        r.shown.posX = r.from.posX + t * (r.to.posX - r.from.posX);
        r.shown.posY = r.from.posY + t * (r.to.posY - r.from.posY);
        r.shown.posZ = r.from.posZ + t * (r.to.posZ - r.from.posZ);
        float yawDelta = std::fmod(r.to.yaw - r.from.yaw + 540.0f, 360.0f) - 180.0f;
        r.shown.yaw = r.from.yaw + t * yawDelta;
        float lookYawDelta = std::fmod(r.to.lookYaw - r.from.lookYaw + 540.0f, 360.0f) - 180.0f;
        r.shown.lookYaw = r.from.lookYaw + t * lookYawDelta;
        r.shown.lookPitch = r.from.lookPitch + t * (r.to.lookPitch - r.from.lookPitch);

        // The capsule centre is what we sync; the mesh (feet) sits meshZ below it. Crouching
        // moves both at once, so interpolate the feet rather than the capsule centre.
        const AppearanceDesc& look = r.hasAppearance ? r.appearance : s_localAppearance;
        float fromZ = r.from.meshZ != 0 ? r.from.meshZ : look.meshOffset[2];
        float toZ = r.to.meshZ != 0 ? r.to.meshZ : look.meshOffset[2];
        float sc = s_remoteScale;
        float feetFrom = r.from.posZ + fromZ * sc, feetTo = r.to.posZ + toZ * sc;
        Vec3f loc{r.shown.posX, r.shown.posY, feetFrom + t * (feetTo - feetFrom)};
        Vec3f rot{0, r.shown.yaw + look.meshYaw, 0};
        if (r.placed && std::memcmp(&loc, r.placedLoc, sizeof(loc)) == 0 && rot.y == r.placedYaw) return;
        r.placed = true;
        std::memcpy(r.placedLoc, &loc, sizeof(loc));
        r.placedYaw = rot.y;

        Call move(actor, L"K2_SetActorLocationAndRotation");
        move.Arg(0, loc);
        move.Arg(1, rot);
        move.Arg<bool>(2, false); // bSweep
        move.Arg<bool>(4, true);  // bTeleport
        move.Invoke();
    }

    static void UpdateRemotes(float dt)
    {
        double now = Now();
        bool inSession = GetSessionState() != SessionState::Disconnected;
        std::vector<uint64_t> remove;

        for (auto& [id, r] : s_remotes)
        {
            if (!r.fake)
            {
                if (now - r.lastRecv > REMOTE_TIMEOUT && r.hasState)
                {
                    Output::send<LogLevel::Default>(STR("[IslandsMP] Remote player {} timed out\n"), id);
                    remove.push_back(id);
                    continue;
                }
                bool member = id == LOOPBACK_ID ? s_loopback
                    : r.localDebug ? s_stress
                    : (inSession && IsLobbyMember(id));
                if (!member)
                {
                    if (r.notInLobbySince == 0) r.notInLobbySince = now;
                    if (now - r.notInLobbySince > NOT_IN_LOBBY_GRACE) { remove.push_back(id); continue; }
                }
                else r.notInLobbySince = 0;
            }

            UpdateRemoteCursor(r, id, dt, now);

            if (!r.hasState) continue;

            AActor* actor = r.actor.As<AActor>();
            if (r.actor.IsSet() && !actor)
            {
                // The world or the game destroyed it; respawn below.
                r.actor.Reset();
                r.leader.Reset();
            }

            // Rebuild when a real appearance arrives or changes.
            if (actor && r.hasAppearance && (!r.builtWithAppearance || r.builtHash != r.appearanceHash)
                && now >= r.nextSpawnAttempt)
            {
                DestroyPuppet(r);
                actor = nullptr;
            }

            if (!actor && WorldReady() && now >= r.nextSpawnAttempt
                && (r.hasAppearance || now - r.firstSeen > APPEARANCE_WAIT))
            {
                if (SpawnPuppet(r))
                {
                    actor = r.actor.As<AActor>();
                    r.spawnFailures = 0;
                    r.nextSpawnAttempt = now + 1.0;
                }
                else
                {
                    r.spawnFailures++;
                    r.nextSpawnAttempt = now + std::min(30.0, 2.0 * r.spawnFailures);
                }
            }

            if (!actor) continue;
            MovePuppet(r, actor, dt);
            {
                std::string markerName = r.appearance.displayName.empty() ? GetPlayerName(id) : r.appearance.displayName;
                UpdatePuppetMapMarker(r, actor, markerName);
            }
            if (UObject* leader = r.leader.Get())
            {
                PlayAnim(r, leader);
                // Head/spine follow the camera, relative to where the body faces. Gliding is
                // already a full-body pose, so leave it alone.
                float yaw = std::fmod(r.shown.lookYaw - r.shown.yaw + 540.0f, 360.0f) - 180.0f;
                float pitch = r.shown.lookPitch;
                if (r.to.animState == AnimState::Gliding) yaw = pitch = 0;
                ApplyLook(leader, r.visual.Get(), std::clamp(yaw, -75.0f, 75.0f), std::clamp(pitch, -60.0f, 60.0f),
                          s_lookTuning);
            }

            // Name cards would float over the puzzle; hide them while we're solving one.
            // Further away they shrink and fade, and past NAMEPLATE_FAR they're gone.
            float dx = r.shown.posX - s_localState.posX, dy = r.shown.posY - s_localState.posY,
                  dz = r.shown.posZ - s_localState.posZ;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (UObject* np = r.nameplate.Get())
            {
                bool want = !IsLocalInPuzzle() && dist < NAMEPLATE_FAR;
                if (want)
                {
                    float t = std::clamp((dist - NAMEPLATE_NEAR) / (NAMEPLATE_FAR - NAMEPLATE_NEAR), 0.0f, 1.0f);
                    float scale = 1.0f - 0.5f * t;
                    float alpha = std::clamp((NAMEPLATE_FAR - dist) / NAMEPLATE_FADE, 0.0f, 1.0f);
                    if (std::fabs(scale - r.nameplateScale) > 0.02f || std::fabs(alpha - r.nameplateAlpha) > 0.03f)
                    {
                        Call get(np, L"GetUserWidgetObject");
                        UObject* w = get.Invoke() ? get.Ret<UObject*>() : nullptr;
                        if (UE::IsLive(w))
                        {
                            struct Vec2f { float x, y; };
                            Call sc(w, L"SetRenderScale");
                            sc.Arg(0, Vec2f{scale, scale});
                            sc.Invoke();
                            Call op(w, L"SetRenderOpacity");
                            op.Arg<float>(0, alpha);
                            op.Invoke();
                            r.nameplateScale = scale;
                            r.nameplateAlpha = alpha;
                        }
                    }
                }
                if (want != r.nameplateShown)
                {
                    Call vis(np, L"SetVisibility");
                    vis.Arg<bool>(0, want);
                    vis.Arg<bool>(1, true);
                    vis.Invoke();
                    r.nameplateShown = want;
                }
            }

            if (!r.nameplateFilled && r.nameplate.Get())
            {
                std::string name = r.appearance.displayName.empty() ? GetPlayerName(id) : r.appearance.displayName;
                if (id == LOOPBACK_ID) name += " (loopback)";
                r.nameplateFilled = SetNameplateText(r.nameplate.Get(), UE::Widen(name), UE::Widen(r.appearance.title),
                                                     r.appearance.level);
            }

            // Wings fade in/out like the game's own WingFadeTimeline.
            float target = r.to.animState == AnimState::Gliding ? 1.0f : 0.0f;
            if (r.glideBlend != target)
            {
                float step = dt / WING_FADE_TIME;
                r.glideBlend = target > r.glideBlend ? std::min(target, r.glideBlend + step)
                                                     : std::max(target, r.glideBlend - step);
                for (auto& part : r.glideParts) SetGlideBlend(part, r.glideBlend);
                static int puppetGlideLogs = 0;
                if (r.glideBlend >= 1.0f && puppetGlideLogs < 2)
                {
                    puppetGlideLogs++;
                    LogGlideParts(r.glideParts, L"fully shown");
                }
            }
        }

        for (uint64_t id : remove)
        {
            DestroyPuppet(s_remotes[id]);
            DestroyMapMarker(s_remotes[id]);
            s_remotes.erase(id);
        }
    }

    static void PumpLoopback()
    {
        double now = Now();
        while (!s_loopbackQueue.empty() && s_loopbackQueue.front().due <= now)
        {
            auto& p = s_loopbackQueue.front();
            MessageHeader h;
            if (ReadHeader(p.data.data(), p.data.size(), h)) // loopback or stress clone id
                HandlePacket(h.origin, p.data.data(), p.data.size());
            s_loopbackQueue.pop_front();
        }
    }

    // ---- UI snapshot ------------------------------------------------------------

    static void PublishSnapshot()
    {
        UISnapshot snap;
        snap.status = GetStatusString();
        snap.inSession = GetSessionState() != SessionState::Disconnected;
        snap.mode = GetSessionModeString(GetSessionMode());
        snap.localFound = s_localPawn.Get() != nullptr;
        snap.local = s_localState;
        snap.loopbackActive = s_loopback;
        snap.stressActive = s_stress;
        for (const auto& p : GetPlayers())
            snap.lobby.push_back(p.displayName + (p.isHost ? " (Host)" : ""));
        for (const auto& [id, r] : s_remotes)
        {
            UIRemote u;
            u.steamID = id;
            u.name = id == LOOPBACK_ID ? "Loopback clone" : r.localDebug ? "Stress clone"
                : r.fake ? "Static clone" : GetPlayerName(id);
            u.posX = r.to.posX; u.posY = r.to.posY; u.posZ = r.to.posZ;
            float dx = r.to.posX - s_localState.posX, dy = r.to.posY - s_localState.posY, dz = r.to.posZ - s_localState.posZ;
            u.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            u.anim = r.to.animState;
            u.hasActor = r.actor.IsSet();
            u.hasAppearance = r.hasAppearance;
            u.fake = r.fake || r.localDebug || id == LOOPBACK_ID;
            snap.remotes.push_back(std::move(u));
        }
        std::lock_guard lock(s_uiMutex);
        s_ui = std::move(snap);
    }

    UISnapshot GetUISnapshot()
    {
        std::lock_guard lock(s_uiMutex);
        return s_ui;
    }

    // ---- Lifecycle --------------------------------------------------------------

    void InitPlayerSync()
    {
        SetMessageCallback([](CSteamID sender, const uint8_t* data, size_t len) {
            OnNetworkMessage(sender, data, len);
        });
    }

    void TickPlayerSync(float dt)
    {
        ResolveLocalPawn(dt);
        UObject* pawn = s_localPawn.Get();

        if (pawn)
        {
            UpdateLocalState(pawn);
            UpdateGlideTimer(pawn, dt);
            UpdateLocalAppearance(pawn, dt);

            s_sendTimer += dt;
            if (s_sendTimer >= SEND_INTERVAL)
            {
                s_sendTimer = 0;
                SendState();
            }
            UpdateAndSendCursor(pawn, dt);
        }

        PumpLoopback();
        UpdateRemotes(dt);
        static float snapTimer = 0;
        if ((snapTimer += dt) >= 0.1f) { snapTimer = 0; PublishSnapshot(); }
    }

    void OnMapLoadStarting()
    {
        // Every actor in the old world is about to go away. Forget them without touching them.
        Output::send<LogLevel::Default>(STR("[IslandsMP] Map load: dropping world references\n"));
        s_pc.Reset();
        s_localPawn.Reset();
        s_worldReadyAt = 1e300;
        UE::ClearCaches();
        for (auto& [id, r] : s_remotes)
        {
            r.actor.Reset();
            r.leader.Reset();
            r.visual.Reset();
            r.mapState.Reset();
            r.mapName.clear();
            r.cursorActor.Reset();
            r.cursorWidget.Reset();
            r.cursorTextSet = false;
            r.glideParts.clear();
            r.glideBlend = 0.0f;
            r.playing = AnimState::Count;
        }
    }

    void ShutdownPlayerSync()
    {
        // Called during unload; the world may already be gone, so don't touch actors.
        s_remotes.clear();
        s_loopbackQueue.clear();
        s_pc.Reset();
        s_localPawn.Reset();
    }

    // ---- Commands ---------------------------------------------------------------

    void HostSession()
    {
        StartSession(SessionMode::Friends);
    }

    void FindPublicSession()
    {
        StartSession(SessionMode::Public);
    }

    void ChooseSessionFromMenu(SessionMode mode)
    {
        if (GetSessionState() != SessionState::Disconnected) LeaveSession();
        s_menuMode = mode;
        s_autoSessionStarted = false; // started once we're in the world
        Output::send<LogLevel::Default>(STR("[IslandsMP] Title screen: {} chosen\n"),
            UE::Widen(GetSessionModeString(mode)));
    }

    bool IsInWorld() { return s_localPawn.Get() != nullptr; }

    void LeaveSession()
    {
        if (GetSessionState() != SessionState::Disconnected)
        {
            uint8_t buf[sizeof(MessageHeader)];
            WriteHeader(buf, MessageType::PlayerLeave, LocalID());
            SendToAll(buf, sizeof(buf), k_nSteamNetworkingSend_Reliable);
        }
        LeaveLobby();
        for (auto it = s_remotes.begin(); it != s_remotes.end();)
        {
            if (it->second.fake || it->first == LOOPBACK_ID) { ++it; continue; }
            DestroyPuppet(it->second);
            DestroyMapMarker(it->second);
            it = s_remotes.erase(it);
        }
    }

    void ToggleStressClones()
    {
        s_stress = !s_stress;
        for (int i = 0; i < STRESS_COUNT; i++)
        {
            uint64_t id = STRESS_ID_FIRST + i;
            auto it = s_remotes.find(id);
            if (it != s_remotes.end())
            {
                DestroyPuppet(it->second);
                DestroyMapMarker(it->second);
                s_remotes.erase(it);
            }
            if (!s_stress) continue;

            RemotePlayer* r = GetOrAddRemote(id);
            if (!r) break;
            r->localDebug = true;
            r->appearance = RandomAppearance(s_stressSeed++, s_haveLocalAppearance ? s_localAppearance
                                                                                    : DefaultAppearance(true));
            static const char* kNames[] = {"Aurelia", "Bram", "Cyrene", "Dax", "Elowen", "Fenn", "Galen",
                                           "Hesper", "Ilya", "Juno", "Kael", "Lyra", "Mire", "Nox", "Orin", "Pell"};
            r->appearance.displayName = std::string(kNames[(s_stressSeed * 7) % 16]) + " (stress)";
            // Mastery titles are "<rank> of <puzzle type>", the rank growing with that type's
            // mastery level (names as shown in the game's Mastery Title menu).
            static const char* kRanks[] = {"Enthusiast", "Practitioner", "Adept", "Expert", "Master",
                                           "Supreme Master", "Ascendent"};
            static const char* kTypes[] = {"Logic Grid", "Skydrop", "Matchbox", "Hidden Ring", "Sightseer",
                                           "Hidden Archway", "Armillary Rings", "Hidden Cube", "Shifting Mosaic",
                                           "Light Motif", "Hidden Pentad", "Flow Orbs", "Pattern Grid"};
            uint32_t h = s_stressSeed * 2654435761u;
            r->appearance.title = std::string(kRanks[(h >> 4) % 7]) + " of " + kTypes[(h >> 12) % 13];
            r->appearance.level = 1 + static_cast<int32_t>((s_stressSeed * 40503u) % 120u);
            r->appearanceHash = r->appearance.Hash();
            r->hasAppearance = true;
            LogAppearance(r->appearance, L"stress");
        }
        if (!s_stress)
        {
            // Drop queued packets for the stress ids; they would recreate the clones.
            std::deque<DelayedPacket> keep;
            for (auto& p : s_loopbackQueue)
            {
                MessageHeader h;
                if (ReadHeader(p.data.data(), p.data.size(), h)
                    && (h.origin < STRESS_ID_FIRST || h.origin >= STRESS_ID_FIRST + STRESS_COUNT))
                    keep.push_back(std::move(p));
            }
            s_loopbackQueue.swap(keep);
        }
        Output::send<LogLevel::Default>(STR("[IslandsMP] Stress clones {}\n"), s_stress ? STR("on") : STR("off"));
    }

    void ToggleLoopbackClone()
    {
        s_loopback = !s_loopback;
        if (!s_loopback)
        {
            s_loopbackQueue.clear();
            auto it = s_remotes.find(LOOPBACK_ID);
            if (it != s_remotes.end())
            {
                DestroyPuppet(it->second);
                DestroyMapMarker(it->second);
                s_remotes.erase(it);
            }
        }
        else
        {
            s_appearanceResendTimer = APPEARANCE_RESEND; // push our look through the loop now
            s_appearanceTimer = APPEARANCE_CHECK;
        }
        Output::send<LogLevel::Default>(STR("[IslandsMP] Loopback clone {}\n"), s_loopback ? STR("on") : STR("off"));
    }

    void SpawnStaticClone(float distance)
    {
        if (!s_localPawn.Get()) return;
        uint64_t id = s_nextStaticClone++;
        RemotePlayer* r = GetOrAddRemote(id);
        if (!r) return;
        r->fake = true;
        float rad = s_localState.yaw * 3.14159265f / 180.0f;
        PlayerStateData s = s_localState;
        s.posX += std::cos(rad) * distance;
        s.posY += std::sin(rad) * distance;
        s.yaw += 180.0f;
        s.velX = s.velY = s.velZ = 0;
        s.animState = AnimState::Idle;
        ApplyState(*r, s, Now());
        if (s_haveLocalAppearance)
        {
            r->appearance = s_localAppearance;
            r->appearanceHash = s_localAppearanceHash;
            r->hasAppearance = true;
        }
    }

    void ClearStaticClones()
    {
        for (auto it = s_remotes.begin(); it != s_remotes.end();)
        {
            if (!it->second.fake) { ++it; continue; }
            DestroyPuppet(it->second);
            DestroyMapMarker(it->second);
            it = s_remotes.erase(it);
        }
    }

    void ForceRespawnRemotes()
    {
        for (auto& [id, r] : s_remotes)
        {
            DestroyPuppet(r);
            r.nextSpawnAttempt = 0;
        }
    }

    void SetMapMarkers(bool enabled)
    {
        s_mapMarkers = enabled; // applied to VisibleOnMap on the next update
    }

    void SetLookTuning(bool enabled, bool invertYaw, bool invertPitch)
    {
        s_lookTuning.enabled = enabled;
        s_lookTuning.yawSign = invertYaw ? -1.0f : 1.0f;
        s_lookTuning.pitchSign = invertPitch ? -1.0f : 1.0f;
    }

    void SetNameplateLayoutAll(float pivotX, float pivotY, float height)
    {
        SetNameplateLayout(pivotX, pivotY, height);
        for (auto& [id, r] : s_remotes)
            if (UObject* np = r.nameplate.Get()) ApplyNameplateLayout(np);
    }

    void SetRemoteScale(float scale)
    {
        s_remoteScale = scale;
        double s = scale;
        for (auto& [id, r] : s_remotes)
            if (auto* a = r.actor.As<AActor>())
                a->SetActorScale3D(FVector{s, s, s});
    }

    void TeleportToPlayer(int index)
    {
        auto* pawn = s_localPawn.As<AActor>();
        if (!pawn) return;
        int i = 0;
        for (auto& [id, r] : s_remotes)
        {
            if (i++ != index) continue;
            FVector loc{static_cast<double>(r.to.posX + 200.0f), static_cast<double>(r.to.posY),
                        static_cast<double>(r.to.posZ)};
            FRotator rot{0.0, static_cast<double>(r.to.yaw), 0.0};
            pawn->K2_TeleportTo(loc, rot);
            return;
        }
    }

    void DumpLocalAppearance()
    {
        UObject* pawn = s_localPawn.Get();
        if (!pawn) return;
        AppearanceDesc d;
        if (CaptureAppearance(pawn, d))
            LogAppearance(d, L"dump");
    }

    void DumpLocalPlayerDetails()
    {
        UObject* pawn = s_localPawn.Get();
        if (!pawn)
        {
            Output::send<LogLevel::Warning>(STR("[IslandsMP] No local player to dump\n"));
            return;
        }
        Output::send<LogLevel::Default>(STR("\n=== LOCAL PLAYER DUMP: {} ===\n"), pawn->GetFullName());
        for (FProperty* prop : TFieldRange<FProperty>(pawn->GetClassPrivate(), EFieldIterationFlags::IncludeSuper))
            Output::send<LogLevel::Default>(STR("  {} ({})\n"), prop->GetName(), prop->GetClass().GetName());
        DumpLocalAppearance();
        Output::send<LogLevel::Default>(STR("=== DUMP COMPLETE ===\n"));
    }
}
