#pragma once

// Appearance sync: the sender describes its character as a list of mesh parts with asset
// paths, sockets, relative transforms and materials (including dynamic material parameters
// such as skin and hair colour). The receiver rebuilds that on a plain SkeletalMeshActor.
// Only content asset paths go over the wire, so both sides resolve the same assets.

#include "NetworkProtocol.h"
#include <Unreal/UObject.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace IslandsMP
{
    struct MatScalar { std::string name; float value = 0; };
    struct MatVector { std::string name; float rgba[4] = {0, 0, 0, 1}; };
    struct MatTexture { std::string name; std::string path; }; // e.g. tattoo / marking masks

    struct MaterialDesc
    {
        std::string path;   // asset, or the parent asset of a dynamic instance
        bool dynamic = false;
        std::vector<MatScalar> scalars;
        std::vector<MatVector> vectors;
        std::vector<MatTexture> textures;
        bool HasParams() const { return !scalars.empty() || !vectors.empty() || !textures.empty(); }
    };

    // A Niagara user parameter (e.g. trail length / colour) as raw bytes.
    struct FxParam { std::string name; std::vector<uint8_t> bytes; };

    enum class PartKind : uint8_t
    {
        Leader   = 0, // the animated body; everything else follows it
        Follower = 1, // skeletal mesh that copies the leader's pose
        Attached = 2, // static mesh attached to a socket (hair, scalp, wings)
        Effect   = 3, // Niagara system attached to a socket (wing particles)
    };

    enum PartFlags : uint8_t
    {
        PART_GLIDE_ONLY = 1, // only shown while the player glides (wings)
    };

    struct PartDesc
    {
        PartKind kind = PartKind::Follower;
        uint8_t flags = 0;
        std::string name;   // component name on the sender, for logging
        std::string mesh;
        std::string socket;
        float loc[3] = {0, 0, 0};
        float rot[3] = {0, 0, 0}; // pitch, yaw, roll
        float scale[3] = {1, 1, 1};
        std::vector<MaterialDesc> materials;
        // Glide-only meshes fade with custom primitive data the game animates; values seen
        // while gliding (on) and while not (off), so the puppet can run the same fade.
        std::vector<float> cpdOn, cpdOff;   // CustomPrimitiveDataInternal (runtime, animated)
        std::vector<float> cpdDefault;      // CustomPrimitiveData (defaults set once by the game)
        std::vector<FxParam> fxParams;      // Effect parts: user parameter overrides
    };

    struct AppearanceDesc
    {
        bool female = true;
        // Body mesh offset relative to the capsule (actor location), so the puppet's feet
        // land where the sender's feet are.
        float meshOffset[3] = {0, 0, -90};
        float meshYaw = -90;
        std::vector<PartDesc> parts;
        // Dynamic materials to apply to any slot whose current material is overrides[i].path.
        std::vector<MaterialDesc> overrides;
        // Nameplate text.
        std::string displayName;
        std::string title;
        int32_t level = 0;

        uint32_t Hash() const;
        std::vector<uint8_t> Serialize() const;
        bool Deserialize(ByteReader& r);
    };

    // Read the local character's look. Returns false if the character isn't ready.
    // glideState: 1 = has been gliding for a while, 0 = has not, -1 = in transition.
    bool CaptureAppearance(RC::Unreal::UObject* character, AppearanceDesc& out, int glideState = -1);

    // Fallback when we have nothing from the remote yet (or capture failed).
    AppearanceDesc DefaultAppearance(bool female);

    // Random sex/face/hair/costume from the game's customization data (stress testing).
    // `reference` (the local look) supplies hair placement and mesh offsets.
    AppearanceDesc RandomAppearance(uint32_t seed, const AppearanceDesc& reference);

    // Build the parts onto a freshly spawned SkeletalMeshActor. Returns the leader component.
    // Components flagged PART_GLIDE_ONLY are created hidden and returned in glideParts.
    struct GlidePart
    {
        RC::Unreal::UObject* comp = nullptr;
        bool effect = false;
        bool active = false;
        std::vector<float> on, off;
    };
    // Returns the animated (hidden) body component. *visual receives the visible body: a
    // PoseableMeshComponent that copies the animated pose each frame and can then turn the
    // head/spine (look direction). Falls back to the animated component itself.
    RC::Unreal::UObject* ApplyAppearance(RC::Unreal::UObject* puppetActor, const AppearanceDesc& desc,
                                         std::vector<GlidePart>* glideParts = nullptr,
                                         RC::Unreal::UObject** visual = nullptr);

    // Per frame: copy the animated pose to the visual mesh and turn spine/neck/head by the
    // given look offset (degrees, relative to the body). No-op when visual == anim.
    struct LookTuning { float yawSign = 1, pitchSign = 1; bool enabled = true; };
    void ApplyLook(RC::Unreal::UObject* anim, RC::Unreal::UObject* visual, float yawDelta, float pitchDelta,
                   const LookTuning& tuning);

    // Fade glide-only parts (wings, trail) in or out: 0 = hidden, 1 = fully shown.
    void SetGlideBlend(GlidePart& part, float blend);

    // Debug: log the wing components' state (visibility, material, parameters).
    void LogWingDebug(RC::Unreal::UObject* character, const wchar_t* label);
    void LogGlideDebug(RC::Unreal::UObject* character, const wchar_t* label);
    void LogGlideParts(const std::vector<GlidePart>& parts, const wchar_t* label);

    // Nameplate: the game's player info widget (name + title) in screen space above the head.
    RC::Unreal::UObject* CreateNameplate(RC::Unreal::UObject* puppetActor, RC::Unreal::UObject* leader);
    // Fills in the text once the widget exists. Returns false if it isn't ready yet.
    bool SetNameplateText(RC::Unreal::UObject* nameplate, const std::wstring& name, const std::wstring& title, int32_t level);
    // Nameplate placement (live-tunable from the UI): widget pivot and height above the head.
    void SetNameplateLayout(float pivotX, float pivotY, float height);
    void ApplyNameplateLayout(RC::Unreal::UObject* nameplate);
    // Map/minimap marker: a UMarkerEmitter on the puppet with the game's "other player" icon
    // and the player's name (how online players and map pins were shown).
    RC::Unreal::UObject* CreateMapMarker(RC::Unreal::UObject* puppetActor, const std::wstring& name);
    void UpdateMapMarker(RC::Unreal::UObject* emitter, float x, float y, float z, bool visible);
    void SetMapMarkerName(RC::Unreal::UObject* emitter, const std::wstring& name);
    // Second line under the name (the game shows "Quest" when this is empty).
    void SetMapMarkerSubtitle(RC::Unreal::UObject* emitter, const std::wstring& text);

    // Shared puzzle cursor: an invisible actor carrying a screen-space widget (the game's
    // player location marker: pointer icon + name) that we move to the remote cursor point.
    RC::Unreal::UObject* CreateCursorMarker(RC::Unreal::UObject* worldContextActor, float x, float y, float z,
                                            RC::Unreal::UObject** widgetComp);
    bool SetCursorMarkerText(RC::Unreal::UObject* widgetComp, const std::wstring& name);
    void MoveCursorMarker(RC::Unreal::UObject* actor, float x, float y, float z, bool visible);

    // The local player's open grid puzzle: puzzle ID hash and cursor point, in the frame of
    // our puzzle panel's grid (see PuzzleLocalToWorld). False if none.
    bool ReadLocalPuzzleCursor(RC::Unreal::UObject* pawn, uint32_t& puzzleHash, float pos[3]);
    // Grid-panel frame <-> world, using the local player's own puzzle panel.
    bool PuzzleWorldToLocal(const float world[3], float local[3]);
    bool PuzzleLocalToWorld(const float local[3], float world[3]);
    // Hooks the grid puzzle enter/exit Blueprint events. Returns false until they can be hooked.
    bool InstallPuzzleHooks();
    bool IsLocalInPuzzle();

    // The local player's level (0 if not found).
    int32_t ReadLocalLevel();
    // The local player's mastery title as shown on the HUD ("" if not found).
    std::wstring ReadLocalTitle();
    // All player titles the game knows about (for stress clones).
    std::vector<std::wstring> ReadAllTitles();

    // Human-readable dump to the UE4SS log.
    void LogAppearance(const AppearanceDesc& desc, const wchar_t* label);
}
