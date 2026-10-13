#include "UEUtil.h"
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Core/HAL/UnrealMemory.hpp>
#include <unordered_map>

using namespace RC;
using namespace RC::Unreal;

namespace IslandsMP::UE
{
    // ---- Handle ---------------------------------------------------------------

    static FUObjectItem* ItemFor(UObject* raw, int32_t index)
    {
        if (!raw || index < 0 || index >= FUObjectArray::GetNumElements()) return nullptr;
        FUObjectItem* item = FUObjectArray::IndexToObject(index);
        if (!item) return nullptr;
        if (static_cast<void*>(item->GetUObject()) != static_cast<void*>(raw)) return nullptr;
        return item;
    }

    void Handle::Set(UObject* obj)
    {
        Reset();
        if (!obj) return;
        m_raw = obj;
        m_index = obj->GetInternalIndex();
        if (FUObjectItem* item = ItemFor(obj, m_index))
            m_serial = item->GetSerialNumber();
    }

    UObject* Handle::Get() const
    {
        FUObjectItem* item = ItemFor(m_raw, m_index);
        if (!item) return nullptr;
        // Serial numbers are only assigned once something takes a weak reference. If one
        // existed when we captured the handle it must still match; if one appeared later
        // the slot was not recycled (recycling resets it to 0 first), so that's fine too.
        if (m_serial != 0 && item->GetSerialNumber() != m_serial) return nullptr;
        if (!item->IsValid(false)) return nullptr; // pending kill / unreachable
        return m_raw;
    }

    bool IsLive(UObject* obj)
    {
        if (!obj) return false;
        Handle h(obj);
        return h.Get() != nullptr;
    }

    // ---- Lookup caches -----------------------------------------------------------

    struct CacheKey
    {
        const void* owner;
        std::wstring name;
        bool operator==(const CacheKey& o) const { return owner == o.owner && name == o.name; }
    };
    struct CacheKeyHash
    {
        size_t operator()(const CacheKey& k) const
        {
            return std::hash<const void*>()(k.owner) ^ (std::hash<std::wstring>()(k.name) * 31);
        }
    };

    // The owning class is kept as a Handle so an entry for a class that was unloaded (and
    // whose memory was reused) is never trusted.
    struct PropEntry { Handle owner; FProperty* prop = nullptr; };
    struct FuncEntry
    {
        Handle owner;
        Handle fn;
        std::vector<FProperty*> args;
        FProperty* ret = nullptr;
    };
    static std::unordered_map<CacheKey, PropEntry, CacheKeyHash> s_props;
    static std::unordered_map<CacheKey, FProperty*, CacheKeyHash> s_members;
    static std::unordered_map<CacheKey, FuncEntry, CacheKeyHash> s_funcs;

    void ClearCaches()
    {
        s_props.clear();
        s_members.clear();
        s_funcs.clear();
    }

    FProperty* FindProp(UObject* obj, const wchar_t* name)
    {
        UClass* cls = obj ? obj->GetClassPrivate() : nullptr;
        if (!cls) return nullptr;
        CacheKey key{cls, name};
        auto it = s_props.find(key);
        if (it != s_props.end() && it->second.owner.Get() == cls) return it->second.prop;
        PropEntry e;
        e.owner.Set(cls);
        e.prop = obj->GetPropertyByNameInChain(name);
        s_props[std::move(key)] = e;
        return e.prop;
    }

    FProperty* FindStructMember(UStruct* st, const wchar_t* name)
    {
        if (!st) return nullptr;
        CacheKey key{st, name};
        auto it = s_members.find(key);
        if (it != s_members.end()) return it->second;
        FProperty* found = nullptr;
        for (FProperty* p : TFieldRange<FProperty>(st, EFieldIterationFlags::IncludeSuper))
            if (p->GetName() == name) { found = p; break; }
        s_members[std::move(key)] = found;
        return found;
    }

    static const FuncEntry* FindFunc(UObject* obj, const wchar_t* name)
    {
        UClass* cls = obj->GetClassPrivate();
        if (!cls) return nullptr;
        CacheKey key{cls, name};
        auto it = s_funcs.find(key);
        if (it != s_funcs.end() && it->second.owner.Get() == cls)
            return &it->second;
        FuncEntry e;
        e.owner.Set(cls);
        if (UFunction* fn = obj->GetFunctionByNameInChain(name))
        {
            e.fn.Set(fn);
            for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::None))
            {
                if (!(p->GetPropertyFlags() & CPF_Parm)) continue;
                if (p->GetPropertyFlags() & CPF_ReturnParm) e.ret = p;
                else e.args.push_back(p);
            }
        }
        else
        {
            Output::send<LogLevel::Warning>(STR("[IslandsMP] Function not found: {}::{}\n"), ClassNameOf(obj), name);
        }
        auto& slot = s_funcs[std::move(key)];
        slot = std::move(e);
        return &slot;
    }

    // ---- Call -----------------------------------------------------------------

    Call::Call(UObject* obj, const wchar_t* functionName)
    {
        if (!obj) return;
        const FuncEntry* f = FindFunc(obj, functionName);
        UObject* fn = f ? f->fn.Get() : nullptr;
        if (!fn) return; // not found (warned once when first looked up)
        m_obj = obj;
        m_fn = static_cast<UFunction*>(fn);
        m_args = f->args;
        m_ret = f->ret;

        size_t size = static_cast<size_t>(m_fn->GetParmsSize());
        m_storage.assign(size + 16, 0);
        uintptr_t base = reinterpret_cast<uintptr_t>(m_storage.data());
        m_buf.ptr = reinterpret_cast<uint8_t*>((base + 15) & ~uintptr_t(15)); // FTransform needs 16
        m_buf.size = size;

        // Construct every param properly (FString/FText/TArray outputs must be valid
        // objects, not zeroed memory, or Blueprint code assigning into them can crash).
        for (FProperty* p : m_args) p->InitializeValue_InContainer(m_buf.ptr);
        if (m_ret) m_ret->InitializeValue_InContainer(m_buf.ptr);
        m_forget.assign(m_args.size(), false);
    }

    Call::~Call()
    {
        if (!Valid()) return;
        for (size_t i = 0; i < m_args.size(); i++)
            if (!m_forget[i]) m_args[i]->DestroyValue_InContainer(m_buf.ptr);
        if (m_ret && !m_forgetRet) m_ret->DestroyValue_InContainer(m_buf.ptr);
    }

    void* Call::ArgPtr(int index, size_t minSize)
    {
        if (!Valid() || index < 0 || index >= static_cast<int>(m_args.size())) return nullptr;
        FProperty* p = m_args[index];
        if (minSize && static_cast<size_t>(p->GetSize()) < minSize)
        {
            Output::send<LogLevel::Warning>(STR("[IslandsMP] Arg {} of {} is {} bytes, wanted {}\n"),
                index, m_fn->GetName(), p->GetSize(), minSize);
            return nullptr;
        }
        return m_buf.ptr + p->GetOffset_Internal();
    }

    bool Call::ArgString(int index, const std::wstring& value)
    {
        struct RawString { wchar_t* data; int32_t num; int32_t max; };
        auto* s = static_cast<RawString*>(ArgPtr(index, sizeof(RawString)));
        if (!s) return false;
        if (s->data) FMemory::Free(s->data);
        int32_t n = static_cast<int32_t>(value.size()) + 1;
        s->data = static_cast<wchar_t*>(FMemory::Malloc(sizeof(wchar_t) * n));
        std::memcpy(s->data, value.c_str(), sizeof(wchar_t) * n);
        s->num = s->max = n;
        return true;
    }

    bool Call::Invoke()
    {
        if (!Valid()) return false;
        m_obj->ProcessEvent(m_fn, m_buf.ptr);
        return true;
    }

    size_t Call::RetOffset() const { return m_ret ? static_cast<size_t>(m_ret->GetOffset_Internal()) : 0; }
    size_t Call::RetSize() const { return m_ret ? static_cast<size_t>(m_ret->GetSize()) : 0; }

    // ---- Names / lookup -----------------------------------------------------

    std::wstring PathOf(UObject* obj)
    {
        if (!obj) return {};
        return obj->GetPathName();
    }

    std::wstring ClassNameOf(UObject* obj)
    {
        if (!obj || !obj->GetClassPrivate()) return L"?";
        return obj->GetClassPrivate()->GetName();
    }

    bool IsSharedAssetPath(const std::wstring& path)
    {
        if (path.empty()) return false;
        if (path.rfind(L"/Game/", 0) != 0 && path.rfind(L"/Engine/", 0) != 0) return false;
        if (path.find(L':') != std::wstring::npos) return false;          // sub-object of a level/actor
        if (path.find(L"Transient") != std::wstring::npos) return false;
        return true;
    }

    UObject* FindByPath(const std::wstring& path)
    {
        if (path.empty()) return nullptr;
        return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path.c_str());
    }

    UObject* FindClass(const wchar_t* path)
    {
        return UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path);
    }

    std::string Narrow(const std::wstring& s)
    {
        std::string out;
        out.reserve(s.size());
        for (wchar_t c : s) out.push_back(c < 128 ? static_cast<char>(c) : '?');
        return out;
    }

    std::wstring Widen(const std::string& s)
    {
        return std::wstring(s.begin(), s.end());
    }

    void SetFString(void* fstring, const std::wstring& value)
    {
        struct RawString { wchar_t* data; int32_t num; int32_t max; };
        auto* s = static_cast<RawString*>(fstring);
        if (!s) return;
        if (s->data) FMemory::Free(s->data);
        int32_t n = static_cast<int32_t>(value.size()) + 1;
        s->data = static_cast<wchar_t*>(FMemory::Malloc(sizeof(wchar_t) * n));
        std::memcpy(s->data, value.c_str(), sizeof(wchar_t) * n);
        s->num = s->max = n;
    }

    static UObject* TextLibrary()
    {
        static Handle h;
        if (!h.Get())
            h.Set(UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary")));
        return h.Get();
    }

    std::wstring TextToString(const void* ftext)
    {
        UObject* lib = TextLibrary();
        if (!lib || !ftext) return {};
        Call c(lib, L"Conv_TextToString");
        void* arg = c.ArgPtr(0, 0x18);
        if (!arg) return {};
        std::memcpy(arg, ftext, 0x18);
        c.Forget(0); // borrowed FText, not ours to release
        if (!c.Invoke()) return {};
        struct RawString { wchar_t* data; int32_t num; int32_t max; };
        auto* s = static_cast<RawString*>(c.RetPtr());
        return s && s->data && s->num > 1 ? std::wstring(s->data, s->num - 1) : L"";
    }

    bool SetTextBlock(UObject* textBlock, const std::wstring& text)
    {
        UObject* lib = TextLibrary();
        if (!lib || !textBlock) return false;
        Call conv(lib, L"Conv_StringToText");
        if (!conv.ArgString(0, text) || !conv.Invoke() || !conv.RetPtr()) return false;
        Call set(textBlock, L"SetText");
        void* arg = set.ArgPtr(0, 0x18);
        if (!arg) return false;
        std::memcpy(arg, conv.RetPtr(), 0x18);
        set.Forget(0); // conv owns this FText; SetText copies it
        return set.Invoke();
    }

    bool SetTextProperty(UObject* obj, const wchar_t* prop, const std::wstring& text)
    {
        auto* dst = Prop<uint8_t>(obj, prop);
        UObject* lib = TextLibrary();
        if (!dst || !lib) return false;
        Call conv(lib, L"Conv_StringToText");
        if (!conv.ArgString(0, text) || !conv.Invoke() || !conv.RetPtr()) return false;
        // Move the new FText in. The old value's reference is dropped without a release, which
        // leaks one small text object; harmless for the few labels we set.
        std::memcpy(dst, conv.RetPtr(), 0x18);
        conv.ForgetRet();
        return true;
    }

    void* StructMember(FProperty* structProp, void* structValue, const wchar_t* member)
    {
        auto* sp = CastField<FStructProperty>(structProp);
        if (!sp || !structValue) return nullptr;
        FProperty* m = FindStructMember(sp->GetStruct(), member);
        return m ? static_cast<uint8_t*>(structValue) + m->GetOffset_Internal() : nullptr;
    }
}
