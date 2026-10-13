#include "Appearance.h"
#include "UEUtil.h"
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <algorithm>
#include <map>
#include <chrono>
#include <array>
#include <cmath>

using namespace RC;
using namespace RC::Unreal;

namespace IslandsMP
{
    using UE::Call;
    using UE::ObjProp;
    using UE::Prop;

    static constexpr size_t MAX_PARTS = 24;
    static constexpr size_t MAX_MATERIALS = 16;
    static constexpr size_t MAX_PARAMS = 64;

    // ---- Serialization --------------------------------------------------------

    static void PutMaterial(ByteWriter& w, const MaterialDesc& m)
    {
        w.PutString(m.path);
        w.Put<uint8_t>(m.dynamic ? 1 : 0);
        w.Put<uint8_t>(static_cast<uint8_t>(std::min(m.scalars.size(), MAX_PARAMS)));
        for (size_t i = 0; i < m.scalars.size() && i < MAX_PARAMS; i++)
        {
            w.PutString(m.scalars[i].name);
            w.Put(m.scalars[i].value);
        }
        w.Put<uint8_t>(static_cast<uint8_t>(std::min(m.vectors.size(), MAX_PARAMS)));
        for (size_t i = 0; i < m.vectors.size() && i < MAX_PARAMS; i++)
        {
            w.PutString(m.vectors[i].name);
            for (float f : m.vectors[i].rgba) w.Put(f);
        }
        w.Put<uint8_t>(static_cast<uint8_t>(std::min(m.textures.size(), MAX_PARAMS)));
        for (size_t i = 0; i < m.textures.size() && i < MAX_PARAMS; i++)
        {
            w.PutString(m.textures[i].name);
            w.PutString(m.textures[i].path);
        }
    }

    static bool GetMaterial(ByteReader& r, MaterialDesc& m)
    {
        uint8_t dyn = 0, ns = 0, nv = 0;
        if (!r.GetString(m.path) || !r.Get(dyn) || !r.Get(ns)) return false;
        m.dynamic = dyn != 0;
        m.scalars.resize(ns);
        for (auto& s : m.scalars)
            if (!r.GetString(s.name) || !r.Get(s.value)) return false;
        if (!r.Get(nv)) return false;
        m.vectors.resize(nv);
        for (auto& v : m.vectors)
        {
            if (!r.GetString(v.name)) return false;
            for (float& f : v.rgba) if (!r.Get(f)) return false;
        }
        uint8_t nt = 0;
        if (!r.Get(nt)) return false;
        m.textures.resize(nt);
        for (auto& t : m.textures)
            if (!r.GetString(t.name) || !r.GetString(t.path)) return false;
        return true;
    }

    std::vector<uint8_t> AppearanceDesc::Serialize() const
    {
        ByteWriter w;
        w.Put<uint8_t>(female ? 1 : 0);
        for (float f : meshOffset) w.Put(f);
        w.Put(meshYaw);
        uint8_t n = static_cast<uint8_t>(std::min(parts.size(), MAX_PARTS));
        w.Put(n);
        for (size_t i = 0; i < n; i++)
        {
            const PartDesc& p = parts[i];
            w.Put(static_cast<uint8_t>(p.kind));
            w.Put(p.flags);
            w.PutString(p.name);
            w.PutString(p.mesh);
            w.PutString(p.socket);
            for (float f : p.loc) w.Put(f);
            for (float f : p.rot) w.Put(f);
            for (float f : p.scale) w.Put(f);
            uint8_t nm = static_cast<uint8_t>(std::min(p.materials.size(), MAX_MATERIALS));
            w.Put(nm);
            for (size_t m = 0; m < nm; m++) PutMaterial(w, p.materials[m]);
            for (const auto* cpd : {&p.cpdOn, &p.cpdOff, &p.cpdDefault})
            {
                uint8_t nc = static_cast<uint8_t>(std::min<size_t>(cpd->size(), 16));
                w.Put(nc);
                for (size_t k = 0; k < nc; k++) w.Put((*cpd)[k]);
            }
            uint8_t nf = static_cast<uint8_t>(std::min<size_t>(p.fxParams.size(), 32));
            w.Put(nf);
            for (size_t k = 0; k < nf; k++)
            {
                w.PutString(p.fxParams[k].name);
                uint8_t sz = static_cast<uint8_t>(std::min<size_t>(p.fxParams[k].bytes.size(), 16));
                w.Put(sz);
                for (size_t b = 0; b < sz; b++) w.Put(p.fxParams[k].bytes[b]);
            }
        }
        uint8_t no = static_cast<uint8_t>(std::min(overrides.size(), MAX_MATERIALS));
        w.Put(no);
        for (size_t i = 0; i < no; i++) PutMaterial(w, overrides[i]);
        w.PutString(displayName);
        w.PutString(title);
        w.Put(level);
        return std::move(w.data);
    }

    bool AppearanceDesc::Deserialize(ByteReader& r)
    {
        uint8_t fem = 0, n = 0;
        if (!r.Get(fem)) return false;
        female = fem != 0;
        for (float& f : meshOffset) if (!r.Get(f)) return false;
        if (!r.Get(meshYaw) || !r.Get(n)) return false;
        parts.clear();
        parts.resize(n);
        for (auto& p : parts)
        {
            uint8_t kind = 0, nm = 0;
            if (!r.Get(kind) || !r.Get(p.flags)) return false;
            if (kind > static_cast<uint8_t>(PartKind::Effect)) return false;
            p.kind = static_cast<PartKind>(kind);
            if (!r.GetString(p.name) || !r.GetString(p.mesh) || !r.GetString(p.socket)) return false;
            for (float& f : p.loc) if (!r.Get(f)) return false;
            for (float& f : p.rot) if (!r.Get(f)) return false;
            for (float& f : p.scale) if (!r.Get(f)) return false;
            if (!r.Get(nm)) return false;
            p.materials.resize(nm);
            for (auto& m : p.materials) if (!GetMaterial(r, m)) return false;
            for (auto* cpd : {&p.cpdOn, &p.cpdOff, &p.cpdDefault})
            {
                uint8_t nc = 0;
                if (!r.Get(nc) || nc > 16) return false;
                cpd->resize(nc);
                for (float& f : *cpd) if (!r.Get(f)) return false;
            }
            uint8_t nf = 0;
            if (!r.Get(nf) || nf > 32) return false;
            p.fxParams.resize(nf);
            for (auto& fx : p.fxParams)
            {
                uint8_t sz = 0;
                if (!r.GetString(fx.name) || !r.Get(sz) || sz > 16) return false;
                fx.bytes.resize(sz);
                for (auto& b : fx.bytes) if (!r.Get(b)) return false;
            }
        }
        uint8_t no = 0;
        if (!r.Get(no)) return false;
        overrides.resize(no);
        for (auto& m : overrides) if (!GetMaterial(r, m)) return false;
        if (!r.GetString(displayName) || !r.GetString(title) || !r.Get(level)) return false;
        return r.Ok();
    }

    // Structural hash: meshes, sockets and material assets, not parameter values. The game
    // animates some material parameters (e.g. etheric hair fades), which must not trigger a
    // puppet rebuild every second.
    uint32_t AppearanceDesc::Hash() const
    {
        AppearanceDesc s = *this;
        s.displayName.clear(); // nameplate text is updated live, not by rebuilding
        s.title.clear();
        s.level = 0;
        auto strip = [](MaterialDesc& m) { m.scalars.clear(); m.vectors.clear(); };
        for (auto& p : s.parts)
        {
            for (auto& m : p.materials) strip(m);
            // Fade values: whether we know them matters (triggers a resend), not the values.
            for (float& f : p.cpdOn) f = 0;
            for (float& f : p.cpdOff) f = 0;
            for (auto& fx : p.fxParams) std::fill(fx.bytes.begin(), fx.bytes.end(), 0);
        }
        for (auto& m : s.overrides) strip(m);
        // Mesh offset changes with crouching etc.; it's applied live, not by rebuilding.
        for (float& f : s.meshOffset) f = 0;
        s.meshYaw = 0;
        uint32_t h = 2166136261u;
        for (uint8_t b : s.Serialize()) { h ^= b; h *= 16777619u; }
        return h;
    }

    // ---- Capture --------------------------------------------------------------

    template<typename F>
    static void ForEachMaterialParam(UObject* mat, const wchar_t* arrayName, F&& fn)
    {
        auto* arrProp = CastField<FArrayProperty>(mat->GetPropertyByNameInChain(arrayName));
        if (!arrProp) return;
        auto* structProp = CastField<FStructProperty>(arrProp->GetInner());
        if (!structProp) return;
        UScriptStruct* st = structProp->GetStruct();
        if (!st) return;
        FProperty* infoProp = st->FindProperty(FName(STR("ParameterInfo"), FNAME_Find));
        FProperty* valueProp = st->FindProperty(FName(STR("ParameterValue"), FNAME_Find));
        auto* arr = Prop<UE::RawArray>(mat, arrayName);
        if (!infoProp || !valueProp || !arr || !arr->Data) return;

        int32_t elem = structProp->GetElementSize();
        int32_t count = std::min<int32_t>(arr->Num, static_cast<int32_t>(MAX_PARAMS));
        for (int32_t i = 0; i < count; i++)
        {
            uint8_t* e = arr->Data + static_cast<size_t>(i) * elem;
            // FMaterialParameterInfo starts with the parameter FName.
            auto* name = reinterpret_cast<FName*>(e + infoProp->GetOffset_Internal());
            fn(UE::Narrow(name->ToString()), e + valueProp->GetOffset_Internal(), valueProp->GetSize());
        }
    }

    static bool CaptureMaterial(UObject* mat, MaterialDesc& out)
    {
        if (!mat) return false;
        std::wstring path = UE::PathOf(mat);
        if (UE::IsSharedAssetPath(path))
        {
            out.path = UE::Narrow(path);
            return true;
        }

        // Runtime-created instance (skin/hair colour). Send its parent plus parameter values.
        UObject* parent = ObjProp(mat, L"Parent");
        for (int depth = 0; parent && depth < 4 && !UE::IsSharedAssetPath(UE::PathOf(parent)); depth++)
            parent = ObjProp(parent, L"Parent");
        if (!parent) return false;

        out.path = UE::Narrow(UE::PathOf(parent));
        out.dynamic = true;
        ForEachMaterialParam(mat, L"ScalarParameterValues", [&](std::string name, const uint8_t* v, int32_t size) {
            if (size >= 4) { MatScalar s{name}; std::memcpy(&s.value, v, 4); out.scalars.push_back(s); }
        });
        ForEachMaterialParam(mat, L"VectorParameterValues", [&](std::string name, const uint8_t* v, int32_t size) {
            if (size >= 16) { MatVector c{name}; std::memcpy(c.rgba, v, 16); out.vectors.push_back(c); }
        });
        // Texture parameters carry the skin markings (face/body tattoos).
        ForEachMaterialParam(mat, L"TextureParameterValues", [&](std::string name, const uint8_t* v, int32_t size) {
            if (size < 8) return;
            UObject* tex = nullptr;
            std::memcpy(&tex, v, 8);
            std::wstring path = UE::PathOf(tex);
            if (UE::IsSharedAssetPath(path)) out.textures.push_back({name, UE::Narrow(path)});
        });
        return true;
    }

    static void CaptureMaterials(UObject* comp, PartDesc& part)
    {
        Call num(comp, L"GetNumMaterials");
        if (!num.Invoke()) return;
        int32_t n = std::min<int32_t>(num.Ret<int32_t>(), static_cast<int32_t>(MAX_MATERIALS));
        for (int32_t i = 0; i < n; i++)
        {
            Call get(comp, L"GetMaterial");
            get.Arg<int32_t>(0, i);
            if (!get.Invoke()) break;
            MaterialDesc m;
            CaptureMaterial(get.Ret<UObject*>(), m); // empty path = leave slot default
            part.materials.push_back(std::move(m));
        }
    }

    static void CaptureTransform(UObject* comp, PartDesc& part)
    {
        if (auto* v = Prop<float>(comp, L"RelativeLocation")) std::copy(v, v + 3, part.loc);
        if (auto* v = Prop<float>(comp, L"RelativeRotation")) std::copy(v, v + 3, part.rot);
        if (auto* v = Prop<float>(comp, L"RelativeScale3D")) std::copy(v, v + 3, part.scale);
        if (auto* s = Prop<FName>(comp, L"AttachSocketName")) part.socket = UE::Narrow(s->ToString());
        if (part.socket == "None") part.socket.clear();
    }

    static void AddOverridesFrom(UObject* comp, AppearanceDesc& out)
    {
        if (!comp) return;
        PartDesc tmp;
        CaptureMaterials(comp, tmp);
        for (auto& m : tmp.materials)
        {
            if (!m.dynamic) continue;
            bool dup = false;
            for (auto& o : out.overrides) dup |= o.path == m.path;
            if (!dup && out.overrides.size() < MAX_MATERIALS) out.overrides.push_back(std::move(m));
        }
    }

    static bool HasMesh(const AppearanceDesc& d, const std::string& mesh)
    {
        for (auto& p : d.parts) if (p.mesh == mesh) return true;
        return false;
    }

    static void AddMeshPart(AppearanceDesc& out, UObject* mesh, PartKind kind, const std::string& name,
                            const char* socket = "")
    {
        std::wstring path = UE::PathOf(mesh);
        if (!UE::IsSharedAssetPath(path)) return;
        std::string p = UE::Narrow(path);
        if (HasMesh(out, p) || out.parts.size() >= MAX_PARTS) return;
        PartDesc part;
        part.kind = kind;
        part.name = name;
        part.mesh = p;
        part.socket = socket;
        out.parts.push_back(std::move(part));
    }

    // Custom primitive data of glide-only meshes, remembered per component for the stable
    // gliding (on) and not-gliding (off) states.
    static std::map<std::wstring, std::vector<float>> s_cpdOn, s_cpdOff;
    static int s_captureGlideState = -1;

    // FCustomPrimitiveData { TArray<float> Data; }. "CustomPrimitiveData" holds the defaults,
    // "CustomPrimitiveDataInternal" the runtime values SetCustomPrimitiveData* writes.
    static std::vector<float> ReadCustomPrimitiveData(UObject* comp, const wchar_t* prop = L"CustomPrimitiveDataInternal")
    {
        std::vector<float> out;
        auto* arr = Prop<UE::RawArray>(comp, prop);
        for (int32_t i = 0; arr && arr->Data && i < arr->Num && i < 16; i++)
            out.push_back(reinterpret_cast<float*>(arr->Data)[i]);
        return out;
    }

    // Niagara user parameters live in UNiagaraComponent::OverrideParameters
    // (FNiagaraUserRedirectionParameterStore : FNiagaraParameterStore, 4.27 layout):
    //   +0x10 TArray<FNiagaraVariableWithOffset> SortedParameterOffsets  (16 B: FName, handle, int32 Offset)
    //   +0x20 TArray<uint8> ParameterData
    // Sizes come from the gap to the next offset. Only plain values (<= 16 bytes) are kept.
    static std::vector<FxParam> ReadNiagaraUserParams(UObject* comp)
    {
        std::vector<FxParam> out;
        auto* store = Prop<uint8_t>(comp, L"OverrideParameters");
        if (!store) return out;
        auto* vars = reinterpret_cast<UE::RawArray*>(store + 0x10);
        auto* data = reinterpret_cast<UE::RawArray*>(store + 0x20);
        if (!vars->Data || !data->Data || vars->Num <= 0 || vars->Num > 256) return out;

        struct Entry { std::wstring name; int32_t offset; };
        std::vector<Entry> entries;
        for (int32_t i = 0; i < vars->Num; i++)
        {
            uint8_t* e = vars->Data + static_cast<size_t>(i) * 16;
            auto* name = reinterpret_cast<FName*>(e);
            int32_t offset = *reinterpret_cast<int32_t*>(e + 0xC);
            if (offset >= 0 && offset < data->Num) entries.push_back({name->ToString(), offset});
        }
        std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.offset < b.offset; });
        for (size_t i = 0; i < entries.size() && out.size() < 32; i++)
        {
            int32_t end = i + 1 < entries.size() ? entries[i + 1].offset : data->Num;
            int32_t size = end - entries[i].offset;
            if (size != 4 && size != 8 && size != 12 && size != 16) continue;
            FxParam fx;
            fx.name = UE::Narrow(entries[i].name);
            fx.bytes.assign(data->Data + entries[i].offset, data->Data + end);
            out.push_back(std::move(fx));
        }
        return out;
    }

    // A static mesh component on the character (hair, scalp), captured whether or not the
    // game is currently showing it: first-person view hides and shows these constantly.
    static bool AddComponentPart(AppearanceDesc& out, UObject* character, const wchar_t* compName,
                                 PartKind kind = PartKind::Attached, uint8_t flags = 0)
    {
        UObject* comp = ObjProp(character, compName);
        if (!comp || out.parts.size() >= MAX_PARTS) return false;
        const wchar_t* assetProp = kind == PartKind::Effect ? L"Asset" : L"StaticMesh";
        std::wstring assetPath = UE::PathOf(ObjProp(comp, assetProp));
        if (!UE::IsSharedAssetPath(assetPath)) return false;
        PartDesc p;
        p.kind = kind;
        p.flags = flags;
        p.name = UE::Narrow(compName);
        p.mesh = UE::Narrow(assetPath);
        CaptureTransform(comp, p);
        if (kind != PartKind::Effect) CaptureMaterials(comp, p);
        if ((flags & PART_GLIDE_ONLY) && kind != PartKind::Effect)
        {
            if (s_captureGlideState == 1) s_cpdOn[compName] = ReadCustomPrimitiveData(comp);
            if (s_captureGlideState == 0) s_cpdOff[compName] = ReadCustomPrimitiveData(comp);
            p.cpdOn = s_cpdOn[compName];
            p.cpdOff = s_cpdOff[compName];
            p.cpdDefault = ReadCustomPrimitiveData(comp, L"CustomPrimitiveData");
            // Whatever transform the fade left it in, the puppet fades it itself.
            if (std::fabs(p.scale[0]) < 0.01f) p.scale[0] = p.scale[1] = p.scale[2] = 1.0f;
        }
        out.parts.push_back(std::move(p));
        return true;
    }

    // ---- Face lookup --------------------------------------------------------------
    //
    // UCharacterCustomizationDataAsset (Female/MaleCustomizationData) holds parallel arrays
    // Body[], Face[], ... of FCustomization { ..., TArray<USkeletalMesh*> SkeletalMeshes, ... }.
    // The selected face index isn't exposed directly, so: find which Body[] entry holds the
    // body mesh GetBodyInfo returned and use the same index into Face[] (presets pair a body
    // with its face). Everything found is logged once so the rule can be checked in game.

    struct StructArray
    {
        FStructProperty* inner = nullptr;
        UE::RawArray* arr = nullptr;
        int32_t Num() const { return arr && arr->Data ? arr->Num : 0; }
        uint8_t* At(int32_t i) const { return arr->Data + static_cast<size_t>(i) * inner->GetElementSize(); }
    };

    static StructArray GetStructArray(UObject* obj, const wchar_t* name)
    {
        StructArray out;
        auto* ap = CastField<FArrayProperty>(obj->GetPropertyByNameInChain(name));
        if (!ap) return out;
        out.inner = CastField<FStructProperty>(ap->GetInner());
        out.arr = Prop<UE::RawArray>(obj, name);
        if (!out.inner) out.arr = nullptr;
        return out;
    }

    static std::vector<UObject*> ObjectsOf(const StructArray& a, int32_t i, const wchar_t* member)
    {
        std::vector<UObject*> out;
        auto* list = static_cast<UE::RawArray*>(UE::StructMember(a.inner, a.At(i), member));
        for (int32_t k = 0; list && list->Data && k < list->Num && k < 8; k++)
            out.push_back(reinterpret_cast<UObject**>(list->Data)[k]);
        return out;
    }

    static std::vector<UObject*> SkeletalMeshesOf(const StructArray& a, int32_t i)
    {
        return ObjectsOf(a, i, L"SkeletalMeshes");
    }

    static std::wstring FStringAt(void* p)
    {
        struct Raw { wchar_t* data; int32_t num; int32_t max; };
        auto* s = static_cast<Raw*>(p);
        return s && s->data && s->num > 0 ? std::wstring(s->data, s->num - 1) : L"";
    }

    static UObject* FindFaceMesh(UObject* character, const AppearanceDesc& look)
    {
        static bool logged = false;
        const wchar_t* assetPath = look.female
            ? L"/Game/ASophia/Data/FemaleCustomizationData.FemaleCustomizationData"
            : L"/Game/ASophia/Data/MaleCustomizationData.MaleCustomizationData";
        UObject* data = UE::FindByPath(assetPath);
        if (!data) return nullptr;

        StructArray bodies = GetStructArray(data, L"Body");
        StructArray faces = GetStructArray(data, L"Face");
        std::string bodyMesh = look.parts.empty() ? "" : look.parts[0].mesh;

        int32_t bodyIndex = -1;
        for (int32_t i = 0; i < bodies.Num() && bodyIndex < 0; i++)
            for (UObject* m : SkeletalMeshesOf(bodies, i))
                if (UE::Narrow(UE::PathOf(m)) == bodyMesh) { bodyIndex = i; break; }

        if (!logged)
        {
            logged = true;
            Output::send<LogLevel::Default>(STR("[IslandsMP] Face probe: {} bodies, {} faces, body index {}\n"),
                bodies.Num(), faces.Num(), bodyIndex);
            auto* names = Prop<UE::RawArray>(data, L"FaceNames");
            for (int32_t i = 0; i < faces.Num() && i < 16; i++)
            {
                auto meshes = SkeletalMeshesOf(faces, i);
                std::wstring name = names && i < names->Num ? FStringAt(names->Data + i * 16) : L"";
                Output::send<LogLevel::Default>(STR("  Face[{}] '{}' -> {}\n"), i, name,
                    meshes.empty() ? std::wstring(L"(none)") : UE::PathOf(meshes[0]));
            }
            for (const wchar_t* prop : {L"CustomizationData", L"LocalCustomizationData"})
            {
                if (auto* b = Prop<uint8_t>(character, prop))
                {
                    std::wstring hex;
                    wchar_t tmp[4];
                    for (int i = 0; i < 0x27; i++) { swprintf(tmp, 4, L"%02X", b[i]); hex += tmp; hex += L' '; }
                    Output::send<LogLevel::Default>(STR("  {}: {}\n"), prop, hex);
                }
            }
            if (auto* skin = Prop<int32_t>(character, L"curSkinIndex"))
                Output::send<LogLevel::Default>(STR("  curSkinIndex: {}\n"), *skin);
        }

        // There is a single Body[] entry and six Face[] presets, so the face is picked by
        // FCustomizationData::FaceMorph1 (0x23). Unconfirmed: verify by switching face preset.
        int32_t faceIndex = 0;
        const wchar_t* dataProp = look.female ? L"CustomizationData" : L"MaleCustomizationData";
        if (auto* b = Prop<uint8_t>(character, dataProp)) faceIndex = b[0x23];
        if (faceIndex >= faces.Num()) faceIndex = 0;
        if (faces.Num() == 0) return nullptr;
        auto meshes = SkeletalMeshesOf(faces, faceIndex);
        return meshes.empty() ? nullptr : meshes[0];
    }

    // In first person the character's own Mesh is only arms + gloves, so the third-person look
    // is read from the game's customization getters (Blueprint functions on the character that
    // return the selected body/costume assets; they don't build or merge anything).
    // Local player's SkinColor / EyeColor / EyebrowColor indices (FCustomizationData 0x12/0x11/0x10).
    static int s_localColorIdx[3] = {-1, -1, -1};

    bool CaptureAppearance(UObject* character, AppearanceDesc& out, int glideState)
    {
        s_captureGlideState = glideState;
        {
            auto* sexFemale = Prop<bool>(character, L"Sex");
            auto* data = Prop<uint8_t>(character, (sexFemale && !*sexFemale) ? L"MaleCustomizationData" : L"CustomizationData");
            if (data)
            {
                s_localColorIdx[0] = data[0x12];
                s_localColorIdx[1] = data[0x11];
                s_localColorIdx[2] = data[0x10];
            }
        }
        out = {};
        UObject* body = ObjProp(character, L"Mesh");
        if (!body) return false;

        if (auto* sex = Prop<bool>(character, L"Sex")) out.female = *sex; // true = female (verified in game)

        // Only the height is stable; X wobbles with the first-person camera.
        out.meshOffset[0] = out.meshOffset[1] = 0;
        if (auto* v = Prop<float>(body, L"RelativeLocation")) out.meshOffset[2] = v[2];
        if (auto* v = Prop<float>(body, L"RelativeRotation")) out.meshYaw = v[1];

        // Body: skeletal pieces of the selected body (body, head, ...). First one leads.
        {
            Call c(character, L"GetBodyInfo");
            auto* arr = static_cast<UE::RawArray*>(c.ArgPtr(0, sizeof(UE::RawArray)));
            if (arr && c.Invoke())
            {
                for (int32_t i = 0; i < arr->Num && i < 8 && arr->Data; i++)
                {
                    UObject* mesh = reinterpret_cast<UObject**>(arr->Data)[i];
                    AddMeshPart(out, mesh, out.parts.empty() ? PartKind::Leader : PartKind::Follower,
                        "Body" + std::to_string(i));
                }
            }
        }
        if (out.parts.empty())
        {
            Output::send<LogLevel::Warning>(STR("[IslandsMP] GetBodyInfo gave no body; using default body\n"));
            out.parts.push_back(DefaultAppearance(out.female).parts[0]);
        }

        // Face: not returned by any getter; look it up in the customization data asset.
        if (UObject* face = FindFaceMesh(character, out))
            AddMeshPart(out, face, PartKind::Follower, "Face");

        // Costume: clothes, head piece, gloves.
        {
            Call c(character, L"GetCostumeInfo");
            if (c.Valid() && c.Invoke())
            {
                const char* names[] = {"Clothes", "HeadPiece", "Gloves"};
                for (int i = 0; i < 3; i++)
                {
                    void* v = c.ArgPtr(i);
                    auto** sk = static_cast<UObject**>(UE::StructMember(c.ArgProp(i), v, L"SkeletalMesh"));
                    auto** sm = static_cast<UObject**>(UE::StructMember(c.ArgProp(i), v, L"StaticMesh"));
                    if (sk && *sk) AddMeshPart(out, *sk, PartKind::Follower, names[i]);
                    else if (sm && *sm) AddMeshPart(out, *sm, PartKind::Attached, names[i], "head");
                }
            }
        }
        // The live Clothes component as a fallback if the getter returned nothing for it.
        if (UObject* clothes = ObjProp(character, L"Clothes"))
            AddMeshPart(out, ObjProp(clothes, L"SkeletalMesh"), PartKind::Follower, "ClothesComp");

        AddComponentPart(out, character, L"HairMesh");
        AddComponentPart(out, character, L"Scalp");

        // Wings: the mesh plus its particle effect. The game only shows
        // these while gliding, so they are flagged and toggled on the puppet the same way.
        // The foot trails (Streamers) stay on all the time, walking included.
        AddComponentPart(out, character, L"Wings", PartKind::Attached, PART_GLIDE_ONLY);
        AddComponentPart(out, character, L"wingsComponent", PartKind::Effect, PART_GLIDE_ONLY);
        if (auto* streamers = Prop<UE::RawArray>(character, L"Streamers"))
        {
            for (int32_t i = 0; streamers->Data && i < streamers->Num && i < 4 && out.parts.size() < MAX_PARTS; i++)
            {
                UObject* comp = reinterpret_cast<UObject**>(streamers->Data)[i];
                if (!UE::IsLive(comp)) continue;
                std::wstring asset = UE::PathOf(ObjProp(comp, L"Asset"));
                if (!UE::IsSharedAssetPath(asset)) continue;
                PartDesc p;
                p.kind = PartKind::Effect;
                p.name = "Streamer" + std::to_string(i);
                p.mesh = UE::Narrow(asset);
                CaptureTransform(comp, p);
                p.fxParams = ReadNiagaraUserParams(comp); // trail length / colour
                out.parts.push_back(std::move(p));
            }
        }

        // Colours: the game applies them as dynamic material instances on the live meshes.
        AddOverridesFrom(body, out);
        AddOverridesFrom(ObjProp(character, L"Clothes"), out);
        AddOverridesFrom(ObjProp(character, L"HairMesh"), out);
        AddOverridesFrom(ObjProp(character, L"Scalp"), out);
        AddOverridesFrom(ObjProp(character, L"Wings"), out);
        return true;
    }

    static bool Contains(const std::string& s, const char* sub);

    // Lowercase, letters/digits only: "Tattoo Face Size" == "TattooFaceSize".
    static std::string NormName(const std::string& s)
    {
        std::string out;
        for (char c : s)
            if (isalnum(static_cast<unsigned char>(c))) out.push_back(static_cast<char>(tolower(c)));
        return out;
    }

    // Values of one tattoo preset (FTattooFaceInfo / FTattooBodyInfo), keyed by normalized
    // member name. The game feeds these into same-named skin material parameters.
    struct TattooValues
    {
        std::map<std::string, float> scalars;
        std::map<std::string, std::array<float, 4>> colors;
        std::map<std::string, std::string> textures;
    };

    static TattooValues RandomTattoo(UObject* tattooAsset, int32_t pick)
    {
        TattooValues out;
        if (!tattooAsset) return out;
        StructArray info = GetStructArray(tattooAsset, L"Info");
        if (info.Num() == 0) return out;
        uint8_t* elem = info.At(pick % info.Num());
        UScriptStruct* st = info.inner->GetStruct();
        for (FProperty* p : TFieldRange<FProperty>(st, EFieldIterationFlags::None))
        {
            std::string name = NormName(UE::Narrow(p->GetName()));
            std::wstring cls = p->GetClass().GetName();
            uint8_t* v = elem + p->GetOffset_Internal();
            if (cls == L"FloatProperty")
                out.scalars[name] = *reinterpret_cast<float*>(v);
            else if (cls == L"StructProperty" && p->GetSize() == 16)
            {
                std::array<float, 4> c{};
                std::memcpy(c.data(), v, 16);
                out.colors[name] = c;
            }
            else if (cls == L"ObjectProperty")
            {
                std::wstring path = UE::PathOf(*reinterpret_cast<UObject**>(v));
                if (UE::IsSharedAssetPath(path)) out.textures[name] = UE::Narrow(path);
            }
        }
        return out;
    }

    // Overwrite matching parameters of a skin material with tattoo preset values. Texture
    // parameters with no match are dropped so the sender's own markings don't carry over.
    static int ApplyTattoo(MaterialDesc& m, const TattooValues& t)
    {
        int matched = 0;
        for (auto& s : m.scalars)
            if (auto it = t.scalars.find(NormName(s.name)); it != t.scalars.end()) { s.value = it->second; matched++; }
        for (auto& v : m.vectors)
            if (auto it = t.colors.find(NormName(v.name)); it != t.colors.end())
            {
                std::memcpy(v.rgba, it->second.data(), 16);
                matched++;
            }
        std::vector<MatTexture> textures;
        for (auto& tex : m.textures)
            if (auto it = t.textures.find(NormName(tex.name)); it != t.textures.end())
            {
                textures.push_back({tex.name, it->second});
                matched++;
            }
        m.textures = std::move(textures);
        return matched;
    }

    AppearanceDesc RandomAppearance(uint32_t seed, const AppearanceDesc& reference)
    {
        uint64_t state = 0x853C49E6748FEA9Bull ^ (static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ull);
        auto rnd = [&](int32_t n) -> int32_t { // splitmix64
            state += 0x9E3779B97F4A7C15ull;
            uint64_t z = state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            return n > 0 ? static_cast<int32_t>(z % static_cast<uint64_t>(n)) : 0;
        };

        bool female = rnd(2) == 0;
        AppearanceDesc d = DefaultAppearance(female);
        d.parts[0].materials.clear(); // keep the body's own materials
        std::copy(reference.meshOffset, reference.meshOffset + 3, d.meshOffset);
        d.meshYaw = reference.meshYaw;

        UObject* data = UE::FindByPath(female
            ? L"/Game/ASophia/Data/FemaleCustomizationData.FemaleCustomizationData"
            : L"/Game/ASophia/Data/MaleCustomizationData.MaleCustomizationData");
        if (!data) return d;

        auto pick = [&](const wchar_t* arrayName, const char* partName, const char* socket) {
            StructArray a = GetStructArray(data, arrayName);
            if (a.Num() == 0) return;
            int32_t i = rnd(a.Num());
            auto sk = ObjectsOf(a, i, L"SkeletalMeshes");
            auto sm = ObjectsOf(a, i, L"StaticMeshes");
            if (!sk.empty() && sk[0]) AddMeshPart(d, sk[0], PartKind::Follower, partName);
            else if (!sm.empty() && sm[0]) AddMeshPart(d, sm[0], PartKind::Attached, partName, socket);
        };

        StructArray bodies = GetStructArray(data, L"Body");
        if (bodies.Num() > 0)
        {
            auto meshes = SkeletalMeshesOf(bodies, 0);
            if (!meshes.empty() && meshes[0]) d.parts[0].mesh = UE::Narrow(UE::PathOf(meshes[0]));
        }
        pick(L"Face", "Face", "");
        pick(L"Clothes", "Clothes", "");
        pick(L"HeadPieces", "HeadPiece", "head");
        pick(L"Gloves", "Gloves", "");
        pick(L"Hair", "HairMesh", "head"); // usually empty: hair lives in the unisex data

        // Hair: unisex Hair[] entries hold StaticMeshes {hair, scalp?} and their materials.
        UObject* unisexData = UE::FindByPath(L"/Game/ASophia/Data/UnisexCustomizationDataAsset.UnisexCustomizationDataAsset");
        bool haveHair = false;
        for (auto& p : d.parts) haveHair |= p.name == "HairMesh";
        if (!haveHair && unisexData)
        {
            StructArray hair = GetStructArray(unisexData, L"Hair");
            if (hair.Num() > 0)
            {
                int32_t i = rnd(hair.Num());
                auto meshes = ObjectsOf(hair, i, L"StaticMeshes");
                if (meshes.size() > 0 && meshes[0]) AddMeshPart(d, meshes[0], PartKind::Attached, "HairMesh", "head");
                if (meshes.size() > 1 && meshes[1]) AddMeshPart(d, meshes[1], PartKind::Attached, "Scalp", "head");

                // Hair material from the entry, tinted with a random HairColor.
                auto* infos = static_cast<UE::RawArray*>(UE::StructMember(hair.inner, hair.At(i), L"MaterialAndTextures"));
                UObject* hairMat = nullptr;
                if (infos && infos->Data && infos->Num > 0)
                {
                    auto* mats = reinterpret_cast<UE::RawArray*>(infos->Data);
                    if (mats->Data && mats->Num > 0) hairMat = reinterpret_cast<UObject**>(mats->Data)[0];
                }
                float tint[4] = {0, 0, 0, 1};
                bool haveTint = false;
                StructArray colors = GetStructArray(unisexData, L"HairColor");
                if (colors.Num() > 0)
                {
                    // Like the skin/eye lists: one entry whose Colors[] is the whole palette
                    // (the first swatch is white, which every clone got before).
                    auto* c = static_cast<UE::RawArray*>(UE::StructMember(colors.inner, colors.At(0), L"Colors"));
                    if (c && c->Data && c->Num > 0)
                    {
                        const float* swatch = reinterpret_cast<const float*>(c->Data) + 4 * rnd(c->Num);
                        for (int ch = 0; ch < 3; ch++) tint[ch] = std::pow(swatch[ch], 2.2f); // sRGB swatch -> linear
                        tint[3] = 1.0f;
                        haveTint = true;
                    }
                }
                for (auto& p : d.parts)
                {
                    if (p.name != "HairMesh") continue;
                    // Start from the local player's hair material setup (dynamic, with its
                    // parameter names), then swap the asset and colour.
                    for (const auto& ref : reference.parts)
                        if (ref.name == "HairMesh") p.materials = ref.materials;
                    if (p.materials.empty()) p.materials.resize(1);
                    if (UE::IsSharedAssetPath(UE::PathOf(hairMat))) p.materials[0].path = UE::Narrow(UE::PathOf(hairMat));
                    if (haveTint)
                        for (auto& v : p.materials[0].vectors)
                            if (Contains(v.name, "colo")) std::memcpy(v.rgba, tint, 16);
                }
            }
        }

        // Hair placement: borrow the local player's hair transform.
        for (const auto& ref : reference.parts)
            if (ref.name == "HairMesh" || ref.name == "Scalp")
                for (auto& p : d.parts)
                    if (p.name == ref.name)
                    {
                        std::copy(ref.loc, ref.loc + 3, p.loc);
                        std::copy(ref.rot, ref.rot + 3, p.rot);
                        std::copy(ref.scale, ref.scale + 3, p.scale);
                        p.socket = ref.socket;
                    }

        // Wings and foot trails: start from the local player's parts (placement, fade values,
        // trail length) and swap in a random wing mesh/material and trail system from the
        // unisex customization data.
        UObject* unisex = UE::FindByPath(L"/Game/ASophia/Data/UnisexCustomizationDataAsset.UnisexCustomizationDataAsset");
        auto randomObject = [&](const wchar_t* arrayName, const wchar_t* member) -> UObject* {
            if (!unisex) return nullptr;
            StructArray a = GetStructArray(unisex, arrayName);
            if (a.Num() == 0) return nullptr;
            auto objs = ObjectsOf(a, rnd(a.Num()), member);
            return objs.empty() ? nullptr : objs[0];
        };
        // First UMaterialInstance of FCustomization::MaterialAndTextures[0].Materials.
        auto randomMaterial = [&](const wchar_t* arrayName) -> UObject* {
            if (!unisex) return nullptr;
            StructArray a = GetStructArray(unisex, arrayName);
            if (a.Num() == 0) return nullptr;
            auto* infos = static_cast<UE::RawArray*>(UE::StructMember(a.inner, a.At(rnd(a.Num())), L"MaterialAndTextures"));
            if (!infos || !infos->Data || infos->Num <= 0) return nullptr;
            auto* mats = reinterpret_cast<UE::RawArray*>(infos->Data); // FCustomizationMaterialInfo::Materials
            return mats->Data && mats->Num > 0 ? reinterpret_cast<UObject**>(mats->Data)[0] : nullptr;
        };

        // Skin tone: copy the local player's skin material overrides (head/body skin instances)
        // and swap colour-like vector parameters for a random entry of SkinColor[].Colors.
        if (unisex)
        {
            float color[4] = {0, 0, 0, 1};
            bool haveColor = false; // picked below, once randomColor exists
            TattooValues faceTattoo = RandomTattoo(ObjProp(unisex, L"FaceTattoo"), rnd(1 << 20));
            TattooValues bodyTattoo = RandomTattoo(ObjProp(unisex, L"BodyTattoo"), rnd(1 << 20));

            // A random entry's first colour from a FCustomization colour list.
            // Each list is ONE entry whose Colors[] holds every swatch. Swatches are UI colours;
            // the material gets a transformed value, so calibrate per channel against the local
            // player's own pick: material = swatch * (localMaterial / localSwatch).
            auto randomColor = [&](const wchar_t* arrayName, int which, const char* refParam, float out[4]) -> bool {
                StructArray a = GetStructArray(unisex, arrayName);
                if (a.Num() == 0) return false;
                auto* c = static_cast<UE::RawArray*>(UE::StructMember(a.inner, a.At(0), L"Colors"));
                if (!c || !c->Data || c->Num <= 0) return false;
                const float* swatches = reinterpret_cast<const float*>(c->Data);
                const float* pick = swatches + 4 * rnd(c->Num);

                const float* localValue = nullptr;
                for (const auto& o : reference.overrides)
                    for (const auto& v : o.vectors)
                        if (NormName(v.name) == NormName(refParam)) localValue = v.rgba;
                int li = s_localColorIdx[which];
                if (localValue && li >= 0 && li < c->Num)
                {
                    const float* localSwatch = swatches + 4 * li;
                    for (int ch = 0; ch < 3; ch++)
                    {
                        float k = localSwatch[ch] > 1e-3f ? localValue[ch] / localSwatch[ch] : 1.0f;
                        out[ch] = std::clamp(pick[ch] * k, 0.0f, 4.0f);
                    }
                    out[3] = localValue[3];
                }
                else
                {
                    // No calibration: treat swatches as sRGB.
                    for (int ch = 0; ch < 3; ch++) out[ch] = std::pow(pick[ch], 2.2f);
                    out[3] = 1.0f;
                }
                return true;
            };

            // Debug (once): what the colour lists actually contain, and the local skin/eye params.
            static bool loggedColorData = false;
            if (!loggedColorData)
            {
                loggedColorData = true;
                for (const wchar_t* listName : {L"SkinColor", L"EyeColor", L"EyebrowColor"})
                {
                    StructArray a = GetStructArray(unisex, listName);
                    Output::send<LogLevel::Default>(STR("[IslandsMP] Colour list {}: {} entries\n"), listName, a.Num());
                    for (int32_t i = 0; i < a.Num() && i < 6; i++)
                    {
                        auto* c = static_cast<UE::RawArray*>(UE::StructMember(a.inner, a.At(i), L"Colors"));
                        auto* c2 = static_cast<UE::RawArray*>(UE::StructMember(a.inner, a.At(i), L"SecondaryColors"));
                        auto* mats = static_cast<UE::RawArray*>(UE::StructMember(a.inner, a.At(i), L"MaterialAndTextures"));
                        float f[4]{};
                        if (c && c->Data && c->Num > 0) std::memcpy(f, c->Data, 16);
                        Output::send<LogLevel::Default>(STR("  [{}] colors={} first=({:.3f},{:.3f},{:.3f},{:.3f}) secondary={} matInfos={}\n"),
                            i, c ? c->Num : -1, f[0], f[1], f[2], f[3], c2 ? c2->Num : -1, mats ? mats->Num : -1);
                    }
                }
                for (const auto& o : reference.overrides)
                {
                    if (!Contains(o.path, "HumanSkin_Female_Body") && !Contains(o.path, "EYE")) continue;
                    Output::send<LogLevel::Default>(STR("[IslandsMP] Params of {}\n"), UE::Widen(o.path));
                    for (auto& s : o.scalars)
                        Output::send<LogLevel::Default>(STR("    scalar {} = {}\n"), UE::Widen(s.name), s.value);
                    for (auto& v : o.vectors)
                        Output::send<LogLevel::Default>(STR("    vector {} = ({:.3f},{:.3f},{:.3f},{:.3f})\n"),
                            UE::Widen(v.name), v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3]);
                }
            }

            // Eyes and eyebrows: the local player's eye/brow material overrides, recoloured.
            for (const auto& o : reference.overrides)
            {
                bool brow = Contains(o.path, "eyebrow");
                bool eye = !brow && Contains(o.path, "eye") && !Contains(o.path, "lash");
                if (!brow && !eye) continue;
                MaterialDesc m = o;
                float c[4];
                if (randomColor(brow ? L"EyebrowColor" : L"EyeColor", brow ? 2 : 1, brow ? "BrowColor" : "IrisHueTint", c))
                    for (auto& v : m.vectors) std::memcpy(v.rgba, c, 16);
                d.overrides.push_back(std::move(m));
            }

            haveColor = randomColor(L"SkinColor", 0, "Skin Base Colour", color);
            for (const auto& o : reference.overrides)
            {
                if (!Contains(o.path, "HumanSkin")) continue;
                MaterialDesc m = o;
                static bool loggedNames = false;
                if (!loggedNames)
                {
                    loggedNames = true;
                    for (auto& v : m.vectors)
                        Output::send<LogLevel::Default>(STR("[IslandsMP] Skin vector param: {}\n"), UE::Widen(v.name));
                }
                if (haveColor)
                    for (auto& v : m.vectors)
                        if (NormName(v.name) == "skinbasecolour" || NormName(v.name) == "skinbasecolor")
                            std::memcpy(v.rgba, color, 16);
                // Random face and body markings from the game's tattoo presets.
                int matched = ApplyTattoo(m, faceTattoo) + ApplyTattoo(m, bodyTattoo);
                Output::send<LogLevel::Default>(STR("[IslandsMP] Stress tattoo: {} params matched on {} ({} face / {} body values)\n"),
                    matched, UE::Widen(m.path), faceTattoo.scalars.size() + faceTattoo.colors.size() + faceTattoo.textures.size(),
                    bodyTattoo.scalars.size() + bodyTattoo.colors.size() + bodyTattoo.textures.size());
                d.overrides.push_back(std::move(m));
            }
        }

        UObject* wingMesh = randomObject(L"WingsMeshes", L"StaticMeshes");
        UObject* wingMat = randomMaterial(L"WingsMaterial");
        UObject* trail = randomObject(L"TrailSystem", L"NiagaraSystem");
        float hue[3] = {rnd(1000) / 1000.0f, rnd(1000) / 1000.0f, rnd(1000) / 1000.0f};
        for (const auto& ref : reference.parts)
        {
            if (!(ref.flags & PART_GLIDE_ONLY) || d.parts.size() >= MAX_PARTS) continue;
            PartDesc p = ref;
            if (ref.kind == PartKind::Attached)
            {
                if (UE::IsSharedAssetPath(UE::PathOf(wingMesh))) p.mesh = UE::Narrow(UE::PathOf(wingMesh));
                if (UE::IsSharedAssetPath(UE::PathOf(wingMat)))
                {
                    MaterialDesc m;
                    m.path = UE::Narrow(UE::PathOf(wingMat));
                    p.materials.assign(1, m);
                }
            }
            else if (ref.kind == PartKind::Effect && ref.name.rfind("Streamer", 0) == 0)
            {
                if (UE::IsSharedAssetPath(UE::PathOf(trail))) p.mesh = UE::Narrow(UE::PathOf(trail));
                // Tint any colour parameter so the clones are easy to tell apart.
                for (auto& fx : p.fxParams)
                    if (fx.bytes.size() == 16 && Contains(fx.name, "colo")) // "Colour" / "Color"
                        std::memcpy(fx.bytes.data(), hue, sizeof(hue));
            }
            d.parts.push_back(std::move(p));
        }
        return d;
    }

    AppearanceDesc DefaultAppearance(bool female)
    {
        AppearanceDesc d;
        d.female = female;
        PartDesc body;
        body.kind = PartKind::Leader;
        body.name = "DefaultBody";
        body.mesh = female ? "/Game/ASophia/Characters/BaseMeshes/F_body_LOD0.F_body_LOD0"
                           : "/Game/ASophia/Characters/BaseMeshes/MaleBody_SK_LOD0.MaleBody_SK_LOD0";
        MaterialDesc skin;
        skin.path = female
            ? "/Game/ASophia/Characters/Shared/SkinMaterials/MasterSkin/Instances/MI_HumanSkin_Female_Body.MI_HumanSkin_Female_Body"
            : "/Game/ASophia/Characters/Shared/SkinMaterials/MasterSkin/Instances/MI_HumanSkin_Male_Body.MI_HumanSkin_Male_Body";
        body.materials.assign(4, skin);
        d.parts.push_back(std::move(body));
        return d;
    }

    // ---- Apply ----------------------------------------------------------------

    struct Vec3f { float x, y, z; };

    // Put a dynamic instance of `mat` on slot `slot` with the captured parameter values.
    static bool ApplyDynamic(UObject* comp, int32_t slot, UObject* mat, const MaterialDesc& m)
    {
        Call create(comp, L"CreateDynamicMaterialInstance");
        create.Arg<int32_t>(0, slot);
        create.Arg<UObject*>(1, mat);
        if (!create.Invoke()) return false;
        UObject* mid = create.Ret<UObject*>();
        if (!mid) return false;
        for (const auto& s : m.scalars)
        {
            Call set(mid, L"SetScalarParameterValue");
            set.Arg(0, FName(UE::Widen(s.name).c_str(), FNAME_Add));
            set.Arg<float>(1, s.value);
            set.Invoke();
        }
        for (const auto& v : m.vectors)
        {
            Call set(mid, L"SetVectorParameterValue");
            set.Arg(0, FName(UE::Widen(v.name).c_str(), FNAME_Add));
            if (void* p = set.ArgPtr(1, 16)) std::memcpy(p, v.rgba, 16);
            set.Invoke();
        }
        for (const auto& t : m.textures)
        {
            UObject* tex = UE::FindByPath(UE::Widen(t.path));
            if (!tex) continue;
            Call set(mid, L"SetTextureParameterValue");
            set.Arg(0, FName(UE::Widen(t.name).c_str(), FNAME_Add));
            set.Arg<UObject*>(1, tex);
            set.Invoke();
        }
        return true;
    }

    static void ApplyMaterials(UObject* comp, const PartDesc& part)
    {
        for (size_t i = 0; i < part.materials.size(); i++)
        {
            const MaterialDesc& m = part.materials[i];
            if (m.path.empty()) continue;
            UObject* mat = UE::FindByPath(UE::Widen(m.path));
            if (!mat)
            {
                Output::send<LogLevel::Warning>(STR("[IslandsMP] Material not loaded: {}\n"), UE::Widen(m.path));
                continue;
            }

            if (m.dynamic && m.HasParams()
                && ApplyDynamic(comp, static_cast<int32_t>(i), mat, m))
                continue;

            Call set(comp, L"SetMaterial");
            set.Arg<int32_t>(0, static_cast<int32_t>(i));
            set.Arg<UObject*>(1, mat);
            set.Invoke();
        }
    }

    // Root (non-instance) material of an instance chain, for matching skin slots.
    static UObject* RootMaterial(UObject* mat)
    {
        for (int depth = 0; mat && depth < 8; depth++)
        {
            UObject* parent = ObjProp(mat, L"Parent");
            if (!parent) return mat;
            mat = parent;
        }
        return mat;
    }

    static bool Contains(const std::string& s, const char* sub)
    {
        std::string a = s, b = sub;
        for (auto& c : a) c = static_cast<char>(tolower(c));
        for (auto& c : b) c = static_cast<char>(tolower(c));
        return a.find(b) != std::string::npos;
    }

    // An override for `current` when its exact path isn't one of ours: an override built on
    // the same master material (the skin master), preferring Head vs Body by name.
    static const MaterialDesc* FindRelatedOverride(UObject* current, const std::string& partName,
                                                   const std::vector<MaterialDesc>& overrides)
    {
        UObject* root = RootMaterial(current);
        if (!root) return nullptr;
        std::string slotPath = UE::Narrow(UE::PathOf(current));
        bool wantHead = Contains(partName, "face") || Contains(slotPath, "head") || Contains(slotPath, "face");
        const MaterialDesc* any = nullptr;
        for (const auto& o : overrides)
        {
            UObject* oMat = UE::FindByPath(UE::Widen(o.path));
            if (!oMat || RootMaterial(oMat) != root) continue;
            if (Contains(o.path, "head") == wantHead) return &o;
            if (!any) any = &o;
        }
        return any;
    }

    static void ApplyOverrides(UObject* comp, const std::vector<MaterialDesc>& overrides, const std::string& partName = "")
    {
        if (overrides.empty()) return;
        Call num(comp, L"GetNumMaterials");
        if (!num.Invoke()) return;
        int32_t n = std::min<int32_t>(num.Ret<int32_t>(), static_cast<int32_t>(MAX_MATERIALS));
        for (int32_t i = 0; i < n; i++)
        {
            Call get(comp, L"GetMaterial");
            get.Arg<int32_t>(0, i);
            if (!get.Invoke()) break;
            UObject* current = get.Ret<UObject*>();
            std::string path = UE::Narrow(UE::PathOf(current));
            const MaterialDesc* match = nullptr;
            for (const auto& o : overrides)
                if (o.path == path) { match = &o; break; }
            if (!match) match = FindRelatedOverride(current, partName, overrides);
            if (!match) continue;
            // Base the new instance on the sender's material asset (skin head/body instance);
            // its colour and marking texture values are then applied on top.
            UObject* base = match->path == path ? current : UE::FindByPath(UE::Widen(match->path));
            ApplyDynamic(comp, i, base ? base : current, *match);
        }
    }

    // We don't know each parameter's Niagara type, only its size, so set it through every
    // setter of that size; a setter whose type doesn't match the parameter has no effect on it.
    static void ApplyFxParams(UObject* comp, const std::vector<FxParam>& params)
    {
        for (const auto& fx : params)
        {
            std::string name = fx.name.rfind("User.", 0) == 0 ? fx.name.substr(5) : fx.name;
            FName fname(UE::Widen(name).c_str(), FNAME_Add);
            std::vector<const wchar_t*> setters;
            switch (fx.bytes.size())
            {
            case 4:  setters = {L"SetVariableFloat", L"SetVariableInt"}; break;
            case 8:  setters = {L"SetVariableVec2"}; break;
            case 12: setters = {L"SetVariableVec3"}; break;
            case 16: setters = {L"SetVariableLinearColor", L"SetVariableVec4"}; break;
            default: continue;
            }
            for (const wchar_t* fn : setters)
            {
                Call set(comp, fn);
                set.Arg(0, fname);
                if (void* v = set.ArgPtr(1, fx.bytes.size())) std::memcpy(v, fx.bytes.data(), fx.bytes.size());
                else continue;
                set.Invoke();
            }
        }
    }

    static bool SetMesh(UObject* comp, const PartDesc& part)
    {
        UObject* mesh = UE::FindByPath(UE::Widen(part.mesh));
        if (!mesh)
        {
            Output::send<LogLevel::Warning>(STR("[IslandsMP] Mesh not loaded: {}\n"), UE::Widen(part.mesh));
            return false;
        }
        if (part.kind == PartKind::Effect)
        {
            Call c(comp, L"SetAsset");
            c.Arg<UObject*>(0, mesh);
            return c.Invoke();
        }
        if (part.kind == PartKind::Attached)
        {
            Call c(comp, L"SetStaticMesh");
            c.Arg<UObject*>(0, mesh);
            return c.Invoke();
        }
        Call c(comp, L"SetSkeletalMesh");
        c.Arg<UObject*>(0, mesh);
        c.Arg<bool>(1, true); // bReinitPose
        return c.Invoke();
    }

    static UObject* AddComponent(UObject* actor, const wchar_t* classPath)
    {
        auto* cls = UE::FindClass(classPath);
        Call add(actor, L"AddComponentByClass");
        if (!cls || !add.Valid()) return nullptr;
        add.Arg<UObject*>(0, cls);
        add.Arg<bool>(1, true); // bManualAttachment: we attach to the body ourselves
        if (void* t = add.ArgPtr(2, 48))
        {
            // FTransform (4.27, float): rotation quat, translation, scale (16-byte aligned rows)
            const float identity[12] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0};
            std::memcpy(t, identity, sizeof(identity));
        }
        add.Arg<bool>(3, false); // bDeferredFinish
        add.Invoke();
        return add.Ret<UObject*>();
    }

    static void AttachToLeader(UObject* comp, UObject* leader, const PartDesc& part)
    {
        Call attach(comp, L"K2_AttachToComponent");
        attach.Arg<UObject*>(0, leader);
        attach.Arg(1, FName(UE::Widen(part.socket.empty() ? std::string("None") : part.socket).c_str(), FNAME_Add));
        attach.Arg<uint8_t>(2, 0); // EAttachmentRule::KeepRelative
        attach.Arg<uint8_t>(3, 0);
        attach.Arg<uint8_t>(4, 0);
        attach.Arg<bool>(5, false);
        attach.Invoke();

        Call rel(comp, L"K2_SetRelativeLocationAndRotation");
        rel.Arg(0, Vec3f{part.loc[0], part.loc[1], part.loc[2]});
        rel.Arg(1, Vec3f{part.rot[0], part.rot[1], part.rot[2]});
        rel.Invoke();

        Call scale(comp, L"SetRelativeScale3D");
        scale.Arg(0, Vec3f{part.scale[0], part.scale[1], part.scale[2]});
        scale.Invoke();
    }

    static void DisableCollision(UObject* comp)
    {
        Call c(comp, L"SetCollisionEnabled");
        c.Arg<uint8_t>(0, 0); // ECollisionEnabled::NoCollision
        c.Invoke();
    }

    void SetGlideBlend(GlidePart& part, float blend)
    {
        UObject* comp = part.comp;
        if (!UE::IsLive(comp)) return;

        bool shown = blend > 0.001f;
        if (shown != part.active)
        {
            part.active = shown;
            Call vis(comp, L"SetVisibility");
            vis.Arg<bool>(0, shown);
            vis.Arg<bool>(1, true);
            vis.Invoke();
            if (part.effect)
            {
                Call act(comp, shown ? L"Activate" : L"Deactivate");
                if (shown) act.Arg<bool>(0, true); // bReset
                act.Invoke();
            }
        }
        if (part.effect) return;

        // Run the game's own fade: blend the custom primitive data between off and on. If we
        // never saw the off state, fade from zero. If we never saw the gliding state either
        // (e.g. stress clones made before the first glide), use the game's known values:
        // CustomPrimitiveDataInternal = [1, 0] while the wings are fully shown.
        static const std::vector<float> kWingsOn = {1.0f, 0.0f};
        const std::vector<float>& on = part.on.empty() ? kWingsOn : part.on;
        for (size_t i = 0; i < on.size(); i++)
        {
            float from = i < part.off.size() ? part.off[i] : 0.0f;
            Call set(comp, L"SetCustomPrimitiveDataFloat");
            set.Arg<int32_t>(0, static_cast<int32_t>(i));
            set.Arg<float>(1, from + (on[i] - from) * blend);
            set.Invoke();
        }
    }

    UObject* ApplyAppearance(UObject* puppetActor, const AppearanceDesc& desc, std::vector<GlidePart>* glideParts,
                             UObject** visualOut)
    {
        UObject* leader = ObjProp(puppetActor, L"SkeletalMeshComponent");
        if (!leader)
        {
            Output::send<LogLevel::Warning>(STR("[IslandsMP] Puppet has no SkeletalMeshComponent\n"));
            return nullptr;
        }

        const PartDesc* leaderPart = nullptr;
        for (const auto& p : desc.parts)
            if (p.kind == PartKind::Leader) { leaderPart = &p; break; }
        AppearanceDesc fallback;
        if (!leaderPart)
        {
            fallback = DefaultAppearance(desc.female);
            leaderPart = &fallback.parts[0];
        }

        if (!SetMesh(leader, *leaderPart) && leaderPart != &fallback.parts[0])
        {
            fallback = DefaultAppearance(desc.female);
            leaderPart = &fallback.parts[0];
            SetMesh(leader, *leaderPart);
        }
        ApplyMaterials(leader, *leaderPart);
        ApplyOverrides(leader, desc.overrides, leaderPart->name);
        DisableCollision(leader);

        // Visible body: a poseable copy of the animated body, so we can turn the head after
        // the animation. The animated body stays hidden but keeps ticking its pose.
        UObject* visual = leader;
        if (UObject* poseable = AddComponent(puppetActor, L"/Script/Engine.PoseableMeshComponent"))
        {
            PartDesc identity;
            AttachToLeader(poseable, leader, identity);
            if (SetMesh(poseable, *leaderPart))
            {
                ApplyMaterials(poseable, *leaderPart);
                ApplyOverrides(poseable, desc.overrides, leaderPart->name);
                DisableCollision(poseable);
                if (auto* tick = Prop<uint8_t>(leader, L"VisibilityBasedAnimTickOption"))
                    *tick = 0; // AlwaysTickPoseAndRefreshBones, even while hidden
                Call hide(leader, L"SetVisibility");
                hide.Arg<bool>(0, false);
                hide.Arg<bool>(1, false); // don't hide the attached parts
                hide.Invoke();
                Call copy(poseable, L"CopyPoseFromSkeletalComponent");
                copy.Arg<UObject*>(0, leader);
                copy.Invoke();
                visual = poseable;
            }
        }
        if (visualOut) *visualOut = visual;

        int added = 0;
        for (const auto& p : desc.parts)
        {
            if (&p == leaderPart || p.kind == PartKind::Leader) continue;
            bool skeletal = p.kind == PartKind::Follower;
            const wchar_t* cls = skeletal ? L"/Script/Engine.SkeletalMeshComponent"
                : p.kind == PartKind::Effect ? L"/Script/Niagara.NiagaraComponent"
                : L"/Script/Engine.StaticMeshComponent";
            UObject* comp = AddComponent(puppetActor, cls);
            if (!comp)
            {
                Output::send<LogLevel::Warning>(STR("[IslandsMP] Could not add component for {}\n"), UE::Widen(p.name));
                continue;
            }
            DisableCollision(comp);
            AttachToLeader(comp, visual, p);
            if (!SetMesh(comp, p)) continue;
            if (p.kind == PartKind::Effect) ApplyFxParams(comp, p.fxParams);
            if (p.kind != PartKind::Effect)
            {
                ApplyMaterials(comp, p);
                ApplyOverrides(comp, desc.overrides, p.name);
            }
            for (size_t k = 0; k < p.cpdDefault.size(); k++)
            {
                Call set(comp, L"SetDefaultCustomPrimitiveDataFloat");
                set.Arg<int32_t>(0, static_cast<int32_t>(k));
                set.Arg<float>(1, p.cpdDefault[k]);
                set.Invoke();
            }
            if (p.flags & PART_GLIDE_ONLY)
            {
                GlidePart gp;
                gp.comp = comp;
                gp.effect = p.kind == PartKind::Effect;
                gp.active = true; // so the hide below applies
                gp.on = p.cpdOn;
                gp.off = p.cpdOff;
                SetGlideBlend(gp, 0.0f);
                if (glideParts) glideParts->push_back(std::move(gp));
            }
            if (skeletal)
            {
                Call follow(comp, L"SetMasterPoseComponent");
                follow.Arg<UObject*>(0, visual);
                follow.Arg<bool>(1, true);
                follow.Invoke();
            }
            added++;
        }

        Output::send<LogLevel::Default>(STR("[IslandsMP] Puppet built: body {} + {} parts\n"),
            UE::Widen(leaderPart->mesh), added);
        return leader;
    }

    static void LogMaterialParams(UObject* mat, int depth = 0)
    {
        if (!mat || depth > 3) return;
        Output::send<LogLevel::Default>(STR("      {}mat {} ({})\n"), depth ? STR("parent ") : STR(""),
            UE::PathOf(mat), UE::ClassNameOf(mat));
        ForEachMaterialParam(mat, L"ScalarParameterValues", [&](std::string name, const uint8_t* v, int32_t size) {
            float f = 0; if (size >= 4) std::memcpy(&f, v, 4);
            Output::send<LogLevel::Default>(STR("        scalar {} = {}\n"), UE::Widen(name), f);
        });
        ForEachMaterialParam(mat, L"VectorParameterValues", [&](std::string name, const uint8_t* v, int32_t size) {
            float c[4]{}; if (size >= 16) std::memcpy(c, v, 16);
            Output::send<LogLevel::Default>(STR("        vector {} = ({}, {}, {}, {})\n"), UE::Widen(name), c[0], c[1], c[2], c[3]);
        });
        if (UObject* parent = ObjProp(mat, L"Parent")) LogMaterialParams(parent, depth + 1);
    }

    static void LogComponentBrief(UObject* comp, const std::wstring& label)
    {
        Call vis(comp, L"IsVisible");
        bool visible = vis.Invoke() && vis.Ret<bool>();
        Call act(comp, L"IsActive");
        bool active = act.Invoke() && act.Ret<bool>();
        UObject* asset = ObjProp(comp, L"Asset");
        if (!asset) asset = ObjProp(comp, L"StaticMesh");
        std::wstring socket;
        if (auto* s = Prop<FName>(comp, L"AttachSocketName")) socket = s->ToString();
        UObject* parent = ObjProp(comp, L"AttachParent");
        float scale[3]{1, 1, 1};
        if (auto* v = Prop<float>(comp, L"RelativeScale3D")) std::copy(v, v + 3, scale);
        Output::send<LogLevel::Default>(STR("  {} [{}] visible={} active={} asset={} parent={} socket={} scale=({}, {}, {})\n"),
            label, UE::ClassNameOf(comp), visible, active, UE::PathOf(asset),
            parent ? parent->GetName() : std::wstring(L"-"), socket, scale[0], scale[1], scale[2]);
        for (const wchar_t* prop : {L"CustomPrimitiveData", L"CustomPrimitiveDataInternal"})
        {
            auto cpd = ReadCustomPrimitiveData(comp, prop);
            if (cpd.empty()) continue;
            std::wstring s;
            for (float f : cpd) s += std::to_wstring(f) + L" ";
            Output::send<LogLevel::Default>(STR("    {}: {}\n"), prop, s);
        }
    }

    void LogGlideParts(const std::vector<GlidePart>& parts, const wchar_t* label)
    {
        Output::send<LogLevel::Default>(STR("[IslandsMP] Puppet glide parts ({}): {}\n"), label, parts.size());
        for (const auto& p : parts)
            if (UE::IsLive(p.comp)) LogComponentBrief(p.comp, p.comp->GetName());
    }

    void LogGlideDebug(UObject* character, const wchar_t* label)
    {
        Output::send<LogLevel::Default>(STR("[IslandsMP] Glide debug ({})\n"), label);
        if (auto* wingsActive = Prop<bool>(character, L"bWingsActive"))
            Output::send<LogLevel::Default>(STR("  bWingsActive={}\n"), *wingsActive);
        if (UObject* wings = ObjProp(character, L"Wings")) LogComponentBrief(wings, L"Wings");
        auto* actor = static_cast<AActor*>(character);
        if (auto* cls = static_cast<UClass*>(UE::FindClass(L"/Script/Niagara.NiagaraComponent")))
            for (UObject* comp : actor->K2_GetComponentsByClass(cls))
                if (comp) LogComponentBrief(comp, comp->GetName());
        for (const wchar_t* arrName : {L"Streamers", L"TempStreamers"})
        {
            auto* arr = Prop<UE::RawArray>(character, arrName);
            Output::send<LogLevel::Default>(STR("  {}: {} entries\n"), arrName, arr ? arr->Num : -1);
            for (int32_t i = 0; arr && arr->Data && i < arr->Num && i < 8; i++)
                if (UObject* c = reinterpret_cast<UObject**>(arr->Data)[i]; UE::IsLive(c))
                    LogComponentBrief(c, std::wstring(arrName) + L"[" + std::to_wstring(i) + L"]");
        }
    }

    void LogWingDebug(UObject* character, const wchar_t* label)
    {
        Output::send<LogLevel::Default>(STR("[IslandsMP] Wing debug ({})\n"), label);
        for (const wchar_t* name : {L"Wings", L"wingsComponent", L"ContrailComponent"})
        {
            UObject* comp = ObjProp(character, name);
            if (!comp) { Output::send<LogLevel::Default>(STR("  {}: missing\n"), name); continue; }
            Call vis(comp, L"IsVisible");
            bool visible = vis.Invoke() && vis.Ret<bool>();
            UObject* asset = ObjProp(comp, L"StaticMesh");
            if (!asset) asset = ObjProp(comp, L"Asset");
            Output::send<LogLevel::Default>(STR("  {} ({}): visible={} asset={}\n"), name, UE::ClassNameOf(comp),
                visible, UE::PathOf(asset));
            Call num(comp, L"GetNumMaterials");
            if (!num.Invoke()) continue;
            for (int32_t i = 0; i < num.Ret<int32_t>() && i < 4; i++)
            {
                Call get(comp, L"GetMaterial");
                get.Arg<int32_t>(0, i);
                if (get.Invoke()) LogMaterialParams(get.Ret<UObject*>());
            }
        }
    }

    UObject* CreateNameplate(UObject* puppetActor, UObject* leader)
    {
        auto* widgetClass = UE::FindClass(L"/Game/ASophia/UI/HUD/Markers/WBP_PlayerInfoWidget.WBP_PlayerInfoWidget_C");
        auto* compClass = UE::FindClass(L"/Script/UMG.WidgetComponent");
        if (!widgetClass || !compClass) return nullptr;
        const float identity[12] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0};

        Call add(puppetActor, L"AddComponentByClass");
        add.Arg<UObject*>(0, compClass);
        add.Arg<bool>(1, true);
        if (void* t = add.ArgPtr(2, 48)) std::memcpy(t, identity, sizeof(identity));
        add.Arg<bool>(3, true); // bDeferredFinish: configure before it registers
        if (!add.Invoke()) return nullptr;
        UObject* comp = add.Ret<UObject*>();
        if (!comp) return nullptr;

        if (auto** wc = Prop<UObject*>(comp, L"WidgetClass")) *wc = widgetClass;
        if (auto* space = Prop<uint8_t>(comp, L"Space")) *space = 1; // EWidgetSpace::Screen

        Call finish(puppetActor, L"FinishAddComponent");
        finish.Arg<UObject*>(0, comp);
        finish.Arg<bool>(1, true);
        if (void* t = finish.ArgPtr(2, 48)) std::memcpy(t, identity, sizeof(identity));
        finish.Invoke();
        DisableCollision(comp);
        PartDesc place;
        place.socket = "head";
        AttachToLeader(comp, leader, place);
        ApplyNameplateLayout(comp);

        Call space(comp, L"SetWidgetSpace");
        space.Arg<uint8_t>(0, 1); // EWidgetSpace::Screen: always faces the camera, fixed size
        space.Invoke();
        Call desired(comp, L"SetDrawAtDesiredSize");
        desired.Arg<bool>(0, true);
        desired.Invoke();
        return comp;
    }

    // ---- Map marker ------------------------------------------------------------------

    // Pointer to a member of the emitter's FLocationMarkerData Marker, plus its property.
    static void* MarkerField(UObject* emitter, const wchar_t* member, FProperty** propOut = nullptr,
                             const wchar_t* structName = L"Marker")
    {
        FProperty* markerProp = UE::FindProp(emitter, structName);
        auto* sp = CastField<FStructProperty>(markerProp);
        if (!sp || !sp->GetStruct()) return nullptr;
        auto* marker = reinterpret_cast<uint8_t*>(emitter) + markerProp->GetOffset_Internal();
        // FLocationMarkerData members (FPlayerLocationMarkerData's base) are found via the chain.
        FProperty* field = UE::FindStructMember(sp->GetStruct(), member);
        if (!field) return nullptr;
        if (propOut) *propOut = field;
        return marker + field->GetOffset_Internal();
    }

    template<typename T>
    static void SetMarker(UObject* emitter, const wchar_t* member, const T& value)
    {
        if (void* p = MarkerField(emitter, member)) std::memcpy(p, &value, sizeof(T));
    }

    static void SetMarkerText(UObject* emitter, const wchar_t* member, const std::wstring& text);

    void SetMapMarkerName(UObject* emitter, const std::wstring& name)
    {
        SetMarkerText(emitter, L"MarkerName", name);
    }

    void SetMapMarkerSubtitle(UObject* emitter, const std::wstring& text)
    {
        SetMarkerText(emitter, L"FunctionText", text);
    }

    static void SetMarkerText(UObject* emitter, const wchar_t* member, const std::wstring& name)
    {
        if (!UE::IsLive(emitter)) return;
        FProperty* textProp = nullptr;
        void* dest = MarkerField(emitter, member, &textProp);
        static UE::Handle lib;
        if (!lib.Get())
            lib.Set(UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary")));
        if (!dest || !textProp || !lib.Get()) return;
        Call conv(lib.Get(), L"Conv_StringToText");
        if (!conv.ArgString(0, name) || !conv.Invoke() || !conv.RetPtr()) return;
        textProp->CopyCompleteValue(dest, conv.RetPtr()); // proper FText copy (ref-counted)
    }

    UObject* CreateMapMarker(UObject* puppetActor, const std::wstring& name)
    {
        auto* cls = UE::FindClass(L"/Script/IslandsofInsight.MarkerEmitter");
        UObject* icon = UE::FindByPath(L"/Game/ASophia/UI/HUD/Map/MinimapOtherPlayerMarker.MinimapOtherPlayerMarker");
        if (!cls) return nullptr;
        const float identity[12] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0};

        Call add(puppetActor, L"AddComponentByClass");
        add.Arg<UObject*>(0, cls);
        add.Arg<bool>(1, true);
        if (void* t = add.ArgPtr(2, 48)) std::memcpy(t, identity, sizeof(identity));
        add.Arg<bool>(3, true); // deferred: fill the marker before it registers
        if (!add.Invoke()) return nullptr;
        UObject* emitter = add.Ret<UObject*>();
        if (!emitter) return nullptr;

        SetMarker<bool>(emitter, L"VisibleOnScreen", false);
        SetMarker<bool>(emitter, L"VisibleOnMap", true);
        SetMarker<bool>(emitter, L"showEverywhere", true);
        SetMarker<bool>(emitter, L"ShowOverFog", true);
        SetMarker<float>(emitter, L"maxDrawWorldDistance", 1.0e9f);
        SetMarker<float>(emitter, L"Opacity", 1.0f);
        SetMarker<float>(emitter, L"sizeMultiplier", 1.0f);
        SetMarker<UObject*>(emitter, L"mapTex", icon);
        SetMarker<bool>(emitter, L"hideGenericMarkerBackground", true);
        SetMarker<bool>(emitter, L"drawOnMapEdge", true);
        SetMarker<bool>(emitter, L"HideCompletionIcon", true);
        SetMarker<bool>(emitter, L"DrawNameOnMap", true);
        SetMarker<bool>(emitter, L"ShowInfoPanel", false); // hover panel shows a "QUEST" header
        const float white[4] = {1, 1, 1, 1};
        if (void* p = MarkerField(emitter, L"NameColor")) std::memcpy(p, white, 16);
        // Name label next to the icon: drawn depending on zoom vs. this threshold. 0 never drew
        // it, so assume "draw while zoom is below the threshold" and make that always true.
        SetMarker<float>(emitter, L"NameZoomThreshold", 1.0e6f);
        SetMapMarkerName(emitter, name);

        Call finish(puppetActor, L"FinishAddComponent");
        finish.Arg<UObject*>(0, emitter);
        finish.Arg<bool>(1, true);
        if (void* t = finish.ArgPtr(2, 48)) std::memcpy(t, identity, sizeof(identity));
        finish.Invoke();
        Output::send<LogLevel::Default>(STR("[IslandsMP] Map marker added ({}), icon {}\n"), name, icon != nullptr);
        return emitter;
    }

    void UpdateMapMarker(UObject* emitter, float x, float y, float z, bool visible)
    {
        if (!UE::IsLive(emitter)) return;
        const float pos[3] = {x, y, z};
        if (void* p = MarkerField(emitter, L"position")) std::memcpy(p, pos, 12);
        SetMarker<bool>(emitter, L"VisibleOnMap", visible);
    }

    // ---- Shared puzzle cursor ------------------------------------------------------

    UObject* CreateCursorMarker(UObject* worldContextActor, float x, float y, float z, UObject** widgetCompOut)
    {
        auto* ctx = static_cast<AActor*>(worldContextActor);
        UWorld* world = ctx ? ctx->GetWorld() : nullptr;
        auto* actorClass = static_cast<UClass*>(UE::FindClass(L"/Script/Engine.SkeletalMeshActor")); // movable root
        auto* widgetClass = UE::FindClass(L"/Game/ASophia/UI/HUD/Markers/WBP_PlayerLocationMarkerInfoWidget.WBP_PlayerLocationMarkerInfoWidget_C");
        auto* compClass = UE::FindClass(L"/Script/UMG.WidgetComponent");
        if (!world || !actorClass || !widgetClass || !compClass) return nullptr;

        FVector loc{static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)};
        FRotator rot{0.0, 0.0, 0.0};
        AActor* actor = world->SpawnActor(actorClass, &loc, &rot);
        if (!actor) return nullptr;
        actor->SetActorEnableCollision(false);
        UObject* root = ObjProp(actor, L"SkeletalMeshComponent");

        const float identity[12] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0};
        Call add(actor, L"AddComponentByClass");
        add.Arg<UObject*>(0, compClass);
        add.Arg<bool>(1, true);
        if (void* t = add.ArgPtr(2, 48)) std::memcpy(t, identity, sizeof(identity));
        add.Arg<bool>(3, true); // deferred: set the widget class before it registers
        if (!add.Invoke()) return actor;
        UObject* comp = add.Ret<UObject*>();
        if (!comp) return actor;
        if (auto** wc = Prop<UObject*>(comp, L"WidgetClass")) *wc = widgetClass;
        if (auto* space = Prop<uint8_t>(comp, L"Space")) *space = 1; // screen space
        Call finish(actor, L"FinishAddComponent");
        finish.Arg<UObject*>(0, comp);
        finish.Arg<bool>(1, true);
        if (void* t = finish.ArgPtr(2, 48)) std::memcpy(t, identity, sizeof(identity));
        finish.Invoke();
        DisableCollision(comp);
        if (root)
        {
            PartDesc place;
            AttachToLeader(comp, root, place);
        }
        Call desired(comp, L"SetDrawAtDesiredSize");
        desired.Arg<bool>(0, true);
        desired.Invoke();
        Call pivot(comp, L"SetPivot");
        const float p[2] = {0.5f, 0.5f}; // just the name, centred on the cursor point
        if (void* pp = pivot.ArgPtr(0, 8)) std::memcpy(pp, p, 8);
        pivot.Invoke();
        if (widgetCompOut) *widgetCompOut = comp;
        return actor;
    }

    bool SetCursorMarkerText(UObject* widgetComp, const std::wstring& name)
    {
        if (!UE::IsLive(widgetComp)) return false;
        Call get(widgetComp, L"GetUserWidgetObject");
        if (!get.Invoke()) return false;
        UObject* widget = get.Ret<UObject*>();
        if (!UE::IsLive(widget)) return false;
        bool ok = UE::SetTextBlock(ObjProp(widget, L"PlayerNameText"), name);
        for (const wchar_t* hide : {L"DistanceText", L"AdditionalInfoText", L"markerImage", L"PingTypeSwitcher"})
            if (UObject* w = ObjProp(widget, hide))
            {
                Call vis(w, L"SetVisibility");
                vis.Arg<uint8_t>(0, 1); // Collapsed
                vis.Invoke();
            }
        // Just the name: the marker's icon is a ping material the Blueprint may re-show,
        // so also make it fully transparent.
        if (UObject* image = ObjProp(widget, L"markerImage"))
        {
            Call fade(image, L"SetRenderOpacity");
            fade.Arg<float>(0, 0.0f);
            fade.Invoke();
        }
        return ok;
    }

    void MoveCursorMarker(UObject* actor, float x, float y, float z, bool visible)
    {
        auto* a = static_cast<AActor*>(actor);
        if (!UE::IsLive(actor)) return;
        a->SetActorHiddenInGame(!visible);
        if (!visible) return;
        Call move(actor, L"K2_SetActorLocation");
        move.Arg(0, Vec3f{x, y, z});
        move.Arg<bool>(1, false); // bSweep
        move.Arg<bool>(3, true);  // bTeleport
        move.Invoke();
    }

    // Which grid puzzle ("rune") the local player has open, set by hooks on the rune
    // Blueprint's own enter/exit events, so no polling is needed.
    static UE::Handle s_openRune;
    static uint32_t s_openRuneHash = 0;
    static bool s_runeHooksInstalled = false;

    bool InstallPuzzleHooks()
    {
        if (s_runeHooksInstalled) return true;
        const StringType enter = STR("/Game/ASophia/Puzzle/Rune/BP_Rune.BP_Rune_C:BP_OnEnterRune");
        const StringType exit = STR("/Game/ASophia/Puzzle/Rune/BP_Rune.BP_Rune_C:BP_OnExitRune");
        if (!UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, enter.c_str())
            || !UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, exit.c_str()))
            return false; // Blueprint not loaded yet; try again later

        UObjectGlobals::RegisterHook(enter, [](UnrealScriptFunctionCallableContext& ctx, void*) {}, [](UnrealScriptFunctionCallableContext& ctx, void*) {
            UObject* rune = ctx.Context;
            s_openRune.Set(rune);
            uint32_t h = 2166136261u;
            for (wchar_t c : rune->GetPathName()) { h ^= static_cast<uint32_t>(c); h *= 16777619u; }
            s_openRuneHash = h ? h : 1;
            Output::send<LogLevel::Default>(STR("[IslandsMP] Entered puzzle {}\n"), rune->GetPathName());
        }, nullptr);
        UObjectGlobals::RegisterHook(exit, [](UnrealScriptFunctionCallableContext& ctx, void*) {}, [](UnrealScriptFunctionCallableContext& ctx, void*) {
            if (s_openRune.Is(ctx.Context))
            {
                s_openRune.Reset();
                s_openRuneHash = 0;
                Output::send<LogLevel::Default>(STR("[IslandsMP] Left puzzle\n"));
            }
        }, nullptr);
        s_runeHooksInstalled = true;
        Output::send<LogLevel::Default>(STR("[IslandsMP] Puzzle enter/exit hooks installed\n"));
        return true;
    }

    bool IsLocalInPuzzle() { return s_openRuneHash != 0 && s_openRune.Get(); }

    struct Quat { float x, y, z, w; };

    // Opening a grid puzzle spawns a puzzle panel (APuzzlePanel) for the local player only,
    // floating in front of their camera and tilting with their mouse. Its GridSpace component
    // is the frame the grid is laid out in, so cursor points are exchanged in that frame.
    static UObject* LocalGridSpace()
    {
        static UE::Handle panelCache;
        static double nextFind = 0;
        if (!IsLocalInPuzzle()) { panelCache.Reset(); return nullptr; }
        UObject* rune = s_openRune.Get();
        UObject* panel = panelCache.Get();
        if (panel && ObjProp(panel, L"rune") != rune) panel = nullptr;
        if (!panel)
        {
            double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now < nextFind) return nullptr;
            nextFind = now + 0.5; // a full object scan; only while we're looking for it
            std::vector<UObject*> found;
            UObjectGlobals::FindAllOf(STR("PuzzlePanel"), found);
            UObject* any = nullptr;
            for (UObject* p : found)
            {
                if (!UE::IsLive(p) || p->GetName().rfind(STR("Default__"), 0) == 0) continue;
                if (ObjProp(p, L"rune") == rune) { panel = p; break; }
                if (!any) any = p;
            }
            if (!panel) panel = any;
            panelCache.Set(panel);
            if (panel)
                Output::send<LogLevel::Default>(STR("[IslandsMP] Puzzle panel {} ({} found)\n"), panel->GetFullName(), found.size());
        }
        return panel ? ObjProp(panel, L"GridSpace") : nullptr;
    }

    struct Xform { Quat q; float t[3]; float s[3]; };

    static bool WorldXform(UObject* comp, Xform& x)
    {
        Call c(comp, L"K2_GetComponentToWorld");
        if (!c.Invoke() || !c.RetPtr()) return false;
        const auto* f = static_cast<const float*>(c.RetPtr()); // FTransform: quat, translation, scale (16-byte aligned)
        x.q = {f[0], f[1], f[2], f[3]};
        std::copy(f + 4, f + 7, x.t);
        std::copy(f + 8, f + 11, x.s);
        for (float& s : x.s) if (std::fabs(s) < 1e-6f) s = 1.0f;
        return true;
    }

    static void Rotate(const Quat& q, const float v[3], float out[3])
    {
        // v + 2w(u x v) + 2u x (u x v), u = quaternion vector part
        float tx = 2 * (q.y * v[2] - q.z * v[1]), ty = 2 * (q.z * v[0] - q.x * v[2]), tz = 2 * (q.x * v[1] - q.y * v[0]);
        out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
        out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
        out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
    }

    bool PuzzleWorldToLocal(const float world[3], float local[3])
    {
        UObject* grid = LocalGridSpace();
        Xform x;
        if (!grid || !WorldXform(grid, x)) return false;
        float d[3] = {world[0] - x.t[0], world[1] - x.t[1], world[2] - x.t[2]};
        Quat inv{-x.q.x, -x.q.y, -x.q.z, x.q.w};
        Rotate(inv, d, local);
        for (int i = 0; i < 3; i++) local[i] /= x.s[i];
        return true;
    }

    bool PuzzleLocalToWorld(const float local[3], float world[3])
    {
        UObject* grid = LocalGridSpace();
        Xform x;
        if (!grid || !WorldXform(grid, x)) return false;
        float scaled[3] = {local[0] * x.s[0], local[1] * x.s[1], local[2] * x.s[2]};
        Rotate(x.q, scaled, world);
        for (int i = 0; i < 3; i++) world[i] += x.t[i];
        return true;
    }

    // Probe: the panel draws cells with instanced meshes; log their instance counts and the
    // first instance's position whenever they change, to see if they mirror the cell colours.
    bool ReadLocalPuzzleCursor(UObject* pawn, uint32_t& puzzleHash, float pos[3])
    {
        puzzleHash = 0;
        if (!IsLocalInPuzzle()) { s_openRuneHash = 0; return false; }
        puzzleHash = s_openRuneHash;

        // Where the mouse points on the puzzle: trace under the cursor now. (The character's
        // LatestHitResultUnderCursor doesn't update while a grid puzzle is open.)
        // Between tiles the trace can miss; then intersect the mouse ray with the plane of the
        // last hit, and failing that keep the last point, so the cursor never drops out.
        static uint32_t planeHash = 0;
        static bool havePlane = false;
        static float planeP[3], planeN[3];
        if (planeHash != s_openRuneHash) { planeHash = s_openRuneHash; havePlane = false; }

        UObject* pc = ObjProp(pawn, L"Controller");
        if (!pc) return havePlane;
        bool got = false;
        {
            Call trace(pc, L"GetHitResultUnderCursor");
            trace.Arg<uint8_t>(0, 3);   // ECC_Visibility
            trace.Arg<bool>(1, false);  // bTraceComplex
            FProperty* hitProp = trace.ArgProp(2);
            void* hit = trace.ArgPtr(2);
            if (trace.Invoke() && trace.Ret<bool>() && hit)
            {
                auto* impact = static_cast<float*>(UE::StructMember(hitProp, hit, L"ImpactPoint"));
                auto* normal = static_cast<float*>(UE::StructMember(hitProp, hit, L"ImpactNormal"));
                if (impact)
                {
                    std::copy(impact, impact + 3, pos);
                    std::copy(impact, impact + 3, planeP);
                    if (normal && (normal[0] != 0 || normal[1] != 0 || normal[2] != 0))
                    {
                        std::copy(normal, normal + 3, planeN);
                        havePlane = true;
                    }
                    got = true;
                }
            }
        }
        if (!got && havePlane)
        {
            Call ray(pc, L"DeprojectMousePositionToWorld");
            auto* o = static_cast<float*>(ray.ArgPtr(0, 12));
            auto* d = static_cast<float*>(ray.ArgPtr(1, 12));
            if (o && d && ray.Invoke() && ray.Ret<bool>())
            {
                float denom = d[0] * planeN[0] + d[1] * planeN[1] + d[2] * planeN[2];
                if (std::abs(denom) > 1e-4f)
                {
                    float t = ((planeP[0] - o[0]) * planeN[0] + (planeP[1] - o[1]) * planeN[1]
                               + (planeP[2] - o[2]) * planeN[2]) / denom;
                    if (t > 0)
                    {
                        for (int i = 0; i < 3; i++) pos[i] = o[i] + d[i] * t;
                        got = true;
                    }
                }
            }
        }
        static float lastPos[3];
        if (!got)
        {
            if (!havePlane) return false;
            std::copy(lastPos, lastPos + 3, pos);
        }
        else std::copy(pos, pos + 3, lastPos);

        // Send it relative to our puzzle panel: each player gets their own panel, placed in
        // front of their own camera, so a world point means nothing on the other side.
        float world[3] = {pos[0], pos[1], pos[2]};
        if (!PuzzleWorldToLocal(world, pos)) return false;
        static int logged = 0;
        if (logged++ < 5)
            Output::send<LogLevel::Default>(STR("[IslandsMP] Puzzle cursor at ({:.0f}, {:.0f}, {:.0f}) = panel ({:.1f}, {:.1f}, {:.1f})\n"),
                world[0], world[1], world[2], pos[0], pos[1], pos[2]);
        return true;
    }

    // Defaults put the card centred over the head; tune live from the UI.
    static float s_npPivot[2] = {0.57f, 1.00f};
    static float s_npHeight = 0.0f;

    void SetNameplateLayout(float pivotX, float pivotY, float height)
    {
        s_npPivot[0] = pivotX;
        s_npPivot[1] = pivotY;
        s_npHeight = height;
    }

    void ApplyNameplateLayout(UObject* nameplate)
    {
        if (!UE::IsLive(nameplate)) return;
        Call pivot(nameplate, L"SetPivot");
        if (void* p = pivot.ArgPtr(0, 8)) std::memcpy(p, s_npPivot, 8);
        pivot.Invoke();
        Call rel(nameplate, L"K2_SetRelativeLocationAndRotation");
        rel.Arg(0, Vec3f{0, 0, s_npHeight});
        rel.Arg(1, Vec3f{0, 0, 0});
        rel.Invoke();
    }

    bool SetNameplateText(UObject* nameplate, const std::wstring& name, const std::wstring& title, int32_t level)
    {
        if (!UE::IsLive(nameplate)) return false;
        Call get(nameplate, L"GetUserWidgetObject");
        if (!get.Invoke()) return false;
        UObject* widget = get.Ret<UObject*>();
        if (!UE::IsLive(widget)) return false;
        bool ok = UE::SetTextBlock(ObjProp(widget, L"PlayerNameText"), name);
        UE::SetTextBlock(ObjProp(widget, L"PlayerTitleText"), title);
        if (UObject* levelWidget = ObjProp(widget, L"Level"); UE::IsLive(levelWidget))
        {
            if (level > 0)
            {
                Call show(levelWidget, L"SetVisibility");
                show.Arg<uint8_t>(0, 4); // SelfHitTestInvisible (undo an earlier collapse)
                show.Invoke();
                Call set(levelWidget, L"SetData");
                set.Arg<int32_t>(0, level);
                set.Invoke();
                UE::SetTextBlock(ObjProp(levelWidget, L"LevelTxt"), std::to_wstring(level));
            }
            else
            {
                Call hide(levelWidget, L"SetVisibility");
                hide.Arg<uint8_t>(0, 1); // ESlateVisibility::Collapsed
                hide.Invoke();
            }
        }
        return ok;
    }

    // Player progression lives on the player state (ASophiaPlayerState::PlayerProgression).
    // FindFirstOf/FindAllOf walk every object in the game (hundreds of thousands), so the
    // results are cached and only searched for again when they go stale.
    static UObject* FindFirstCached(UE::Handle& cache, const wchar_t* className, double& nextTry)
    {
        if (UObject* o = cache.Get()) return o;
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now < nextTry) return nullptr;
        nextTry = now + 5.0;
        cache.Set(UObjectGlobals::FindFirstOf(className));
        return cache.Get();
    }

    static uint8_t* LocalProgression(FProperty** prop)
    {
        static UE::Handle cache;
        static double nextTry = 0;
        UObject* ps = FindFirstCached(cache, STR("SophiaPlayerState"), nextTry);
        if (!ps) return nullptr;
        *prop = ps->GetPropertyByNameInChain(STR("PlayerProgression"));
        return *prop ? Prop<uint8_t>(ps, L"PlayerProgression") : nullptr;
    }

    // HUD widget instances to read the local level/title from: skip class defaults and
    // templates (they hold design-time values like level 88 / "No Title Equipped!") and
    // prefer the copies that are actually visible.
    static std::vector<UObject*> FindLocalPlayerWidgets();

    static std::vector<UObject*> LocalPlayerWidgets()
    {
        static std::vector<UE::Handle> cache;
        static double nextScan = 0;
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        bool stale = cache.empty();
        for (auto& h : cache) if (!h.Get()) stale = true;
        if (stale && now >= nextScan)
        {
            nextScan = now + 5.0;
            cache.clear();
            for (UObject* w : FindLocalPlayerWidgets()) cache.emplace_back(w);
        }
        std::vector<UObject*> out, hidden;
        for (auto& h : cache)
            if (UObject* w = h.Get())
            {
                Call vis(w, L"IsVisible");
                (vis.Invoke() && vis.Ret<bool>() ? out : hidden).push_back(w);
            }
        out.insert(out.end(), hidden.begin(), hidden.end());
        return out;
    }

    static std::vector<UObject*> FindLocalPlayerWidgets()
    {
        std::vector<UObject*> visible, hidden;
        for (const wchar_t* cls : {STR("WBP_LocalPlayerWidget_C"), STR("WBP_LocalPlayerWidgetMap_C")})
        {
            std::vector<UObject*> widgets;
            UObjectGlobals::FindAllOf(cls, widgets);
            for (UObject* w : widgets)
            {
                if (!UE::IsLive(w)) continue;
                std::wstring name = w->GetName();
                if (name.rfind(L"Default__", 0) == 0) continue;
                if (w->GetFullName().find(L"WidgetTree") != std::wstring::npos
                    && w->GetFullName().find(L"Transient") == std::wstring::npos
                    && w->GetFullName().find(L"/Game/") != std::wstring::npos)
                    continue; // template inside a widget blueprint asset
                Call vis(w, L"IsVisible");
                (vis.Invoke() && vis.Ret<bool>() ? visible : hidden).push_back(w);
            }
        }
        visible.insert(visible.end(), hidden.begin(), hidden.end());
        return visible;
    }

    int32_t ReadLocalLevel()
    {
        // The level the HUD shows (WBP_LocalPlayerWidget[Map] -> WBP_LevelUp). PlayerProgression's
        // PlayerLevel is a different counter (e.g. 662 for a level 56 player). Several widget
        // instances can exist (HUD, map, menus); take the first that has a level.
        static int32_t lastGood = 0;
        // Best source: the progression subsystem's own getter. (Widgets can hold stale
        // design-time text, e.g. "88" until the menu is first opened.)
        static UE::Handle progCache;
        static double progNextTry = 0;
        if (UObject* prog = FindFirstCached(progCache, STR("GISProgression"), progNextTry))
        {
            Call get(prog, L"BPC_GetPlayerLevel");
            if (get.Invoke())
            {
                int32_t level = get.Ret<int32_t>();
                static int logged = 0;
                if (logged++ < 2)
                {
                    Call total(prog, L"BPC_GetTotalMasteryLevel");
                    Output::send<LogLevel::Default>(STR("[IslandsMP] Level source GISProgression: player={} totalMastery={}\n"),
                        level, total.Invoke() ? total.Ret<int32_t>() : -1);
                }
                if (level > 0) return lastGood = level;
            }
        }
        {
            for (UObject* w : LocalPlayerWidgets())
            {
                UObject* levelWidget = ObjProp(w, L"WBP_LevelUp");
                if (!levelWidget) levelWidget = ObjProp(w, L"Level");
                if (!UE::IsLive(levelWidget)) continue;
                // The badge's text is what the player sees; the int can lag behind (it read 88
                // while the badge showed 56), so prefer the text.
                std::wstring s;
                if (UObject* text = ObjProp(levelWidget, L"LevelTxt"))
                    if (auto* ftext = Prop<uint8_t>(text, L"Text")) s = UE::TextToString(ftext);
                auto* levelInt = Prop<int32_t>(levelWidget, L"Level");
                static int logged = 0;
                if (logged < 6)
                {
                    logged++;
                    Output::send<LogLevel::Default>(STR("[IslandsMP] Level source {}: text='{}' int={}\n"),
                        w->GetFullName(), s, levelInt ? *levelInt : -1);
                }
                if (!s.empty() && iswdigit(s[0]) && std::stoi(s) > 0) return lastGood = std::stoi(s);
                if (levelInt && *levelInt > 0) return lastGood = *levelInt;
            }
        }
        return lastGood;
    }

    std::wstring ReadLocalTitle()
    {
        // The HUD's local player widget shows the equipped mastery title.
        // The title text block is bound to Get_MasteryTitle_Text_0, so its Text property stays
        // empty; call the binding instead. Several widget copies exist; use the first non-empty.
        {
            for (UObject* w : LocalPlayerWidgets())
            {
                Call get(w, L"Get_MasteryTitle_Text_0");
                if (!get.Invoke() || !get.RetPtr()) continue;
                std::wstring s = UE::TextToString(get.RetPtr());
                if (s.find(L"No Title") != std::wstring::npos) return L""; // nothing equipped
                if (!s.empty()) return s;
                if (UObject* text = ObjProp(w, L"MasteryTitle"))
                    if (auto* ftext = Prop<uint8_t>(text, L"Text"))
                        if (s = UE::TextToString(ftext); !s.empty()) return s;
            }
        }
        return L"";
    }

    std::vector<std::wstring> ReadAllTitles()
    {
        std::vector<std::wstring> out;
        std::vector<UObject*> assets;
        UObjectGlobals::FindAllOf(STR("CharacterTitleDataAsset"), assets);
        for (UObject* a : assets)
        {
            if (!a || a->GetName().rfind(L"Default__", 0) == 0) continue;
            auto* arr = Prop<UE::RawArray>(a, L"PlayerTitles");
            for (int32_t i = 0; arr && arr->Data && i < arr->Num && i < 512; i++)
            {
                std::wstring s = UE::TextToString(arr->Data + static_cast<size_t>(i) * 0x18);
                if (!s.empty()) out.push_back(s);
            }
        }
        if (out.empty())
        {
            FProperty* prop = nullptr;
            uint8_t* data = LocalProgression(&prop);
            auto* titles = data ? static_cast<UE::RawArray*>(UE::StructMember(prop, data, L"UnlockedTitles")) : nullptr;
            for (int32_t i = 0; titles && titles->Data && i < titles->Num && i < 512; i++)
            {
                std::wstring s = UE::TextToString(titles->Data + static_cast<size_t>(i) * 0x18);
                if (!s.empty()) out.push_back(s);
            }
        }
        Output::send<LogLevel::Default>(STR("[IslandsMP] Found {} player titles\n"), out.size());
        return out;
    }


    // ---- Look direction -------------------------------------------------------------


    static Quat FromRotator(const float r[3]) // pitch, yaw, roll (degrees), UE convention
    {
        const float d = 3.14159265f / 360.0f;
        float sp = std::sin(r[0] * d), cp = std::cos(r[0] * d);
        float sy = std::sin(r[1] * d), cy = std::cos(r[1] * d);
        float sr = std::sin(r[2] * d), cr = std::cos(r[2] * d);
        return {cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy,
                cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
    }

    static void ToRotator(const Quat& q, float out[3])
    {
        const float rad = 180.0f / 3.14159265f;
        float test = q.z * q.x - q.w * q.y;
        float yawY = 2 * (q.w * q.z + q.x * q.y), yawX = 1 - 2 * (q.y * q.y + q.z * q.z);
        if (test < -0.4999995f)
        {
            out[0] = -90; out[1] = std::atan2(yawY, yawX) * rad;
            out[2] = std::remainder(-out[1] - 2 * std::atan2(q.x, q.w) * rad, 360.0f);
        }
        else if (test > 0.4999995f)
        {
            out[0] = 90; out[1] = std::atan2(yawY, yawX) * rad;
            out[2] = std::remainder(out[1] - 2 * std::atan2(q.x, q.w) * rad, 360.0f);
        }
        else
        {
            out[0] = std::asin(2 * test) * rad;
            out[1] = std::atan2(yawY, yawX) * rad;
            out[2] = std::atan2(-2 * (q.w * q.x + q.y * q.z), 1 - 2 * (q.x * q.x + q.y * q.y)) * rad;
        }
    }

    static Quat Mul(const Quat& a, const Quat& b)
    {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    static Quat AxisAngle(float x, float y, float z, float degrees)
    {
        float h = degrees * 3.14159265f / 360.0f, s = std::sin(h);
        return {x * s, y * s, z * s, std::cos(h)};
    }

    void ApplyLook(UObject* anim, UObject* visual, float yawDelta, float pitchDelta, const LookTuning& tuning)
    {
        if (!visual || visual == anim || !UE::IsLive(visual) || !UE::IsLive(anim)) return;
        // Off screen nobody sees the pose; refresh it only now and then (so its bounds stay
        // right for when it comes back into view).
        {
            Call seen(visual, L"WasRecentlyRendered");
            seen.Arg<float>(0, 0.25f);
            if (seen.Invoke() && !seen.Ret<bool>())
            {
                static uint32_t frame = 0;
                if (++frame % 8 != 0) return;
            }
        }
        Call copy(visual, L"CopyPoseFromSkeletalComponent");
        copy.Arg<UObject*>(0, anim);
        copy.Invoke();
        if (!tuning.enabled) return;

        // Spread the turn over the spine, neck and head, in component space (mesh faces +Y,
        // so yaw turns about +Z and pitch about the mesh's side axis X).
        struct Bone { FName name; float weight; };
        static const Bone bones[] = {{FName(L"spine_03", FNAME_Add), 0.3f}, {FName(L"neck_01", FNAME_Add), 0.3f},
                                     {FName(L"head", FNAME_Add), 0.4f}};
        for (const auto& bone : bones)
        {
            const FName& fname = bone.name;
            Call get(visual, L"GetBoneRotationByName");
            get.Arg(0, fname);
            get.Arg<uint8_t>(1, 1); // ComponentSpace
            if (!get.Invoke()) continue;
            float current[3];
            std::memcpy(current, get.RetPtr(), sizeof(current));

            Quat q = FromRotator(current);
            Quat yaw = AxisAngle(0, 0, 1, tuning.yawSign * yawDelta * bone.weight);
            Quat pitch = AxisAngle(1, 0, 0, tuning.pitchSign * pitchDelta * bone.weight);
            float result[3];
            ToRotator(Mul(yaw, Mul(pitch, q)), result);

            Call set(visual, L"SetBoneRotationByName");
            set.Arg(0, fname);
            if (void* p = set.ArgPtr(1, 12)) std::memcpy(p, result, 12);
            set.Arg<uint8_t>(2, 1); // ComponentSpace
            set.Invoke();
        }
    }

    void LogAppearance(const AppearanceDesc& d, const wchar_t* label)
    {
        Output::send<LogLevel::Default>(STR("[IslandsMP] Nameplate ({}): name='{}' title='{}' level={}\n"),
            label, UE::Widen(d.displayName), UE::Widen(d.title), d.level);
        Output::send<LogLevel::Default>(STR("[IslandsMP] Appearance ({}): {} parts, {} overrides, female={}, offset=({:.0f},{:.0f},{:.0f}) yaw={:.0f}\n"),
            label, d.parts.size(), d.overrides.size(), d.female, d.meshOffset[0], d.meshOffset[1], d.meshOffset[2], d.meshYaw);
        for (const auto& m : d.overrides)
            Output::send<LogLevel::Default>(STR("  override {} ({} scalars, {} vectors)\n"),
                UE::Widen(m.path), m.scalars.size(), m.vectors.size());
        for (const auto& p : d.parts)
        {
            Output::send<LogLevel::Default>(STR("  [{}] {} -> {} socket='{}' mats={}\n"),
                static_cast<int>(p.kind), UE::Widen(p.name), UE::Widen(p.mesh), UE::Widen(p.socket), p.materials.size());
            for (const auto& fx : p.fxParams)
            {
                float f = 0;
                if (fx.bytes.size() >= 4) std::memcpy(&f, fx.bytes.data(), 4);
                Output::send<LogLevel::Default>(STR("      fx {} ({} bytes) first={}\n"), UE::Widen(fx.name), fx.bytes.size(), f);
            }
            for (const auto& m : p.materials)
                Output::send<LogLevel::Default>(STR("      {}{} ({} scalars, {} vectors)\n"),
                    m.dynamic ? STR("dyn ") : STR(""), UE::Widen(m.path), m.scalars.size(), m.vectors.size());
        }
    }
}
