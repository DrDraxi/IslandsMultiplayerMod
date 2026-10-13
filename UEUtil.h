#pragma once

// Small helpers for touching game objects safely from the mod.
// Everything here must only be called on the game thread (see GameThread.h).

#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/FProperty.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace IslandsMP::UE
{
    using RC::Unreal::UObject;
    using RC::Unreal::UFunction;
    using RC::Unreal::FProperty;

    // Weak reference to a UObject. Get() returns nullptr once the object is destroyed,
    // pending kill, unreachable, or its GUObjectArray slot now holds something else, so a
    // stale handle is never dereferenced. Validation goes through GUObjectArray (which is
    // always safe to read) before the object memory itself is touched.
    class Handle
    {
    public:
        Handle() = default;
        explicit Handle(UObject* obj) { Set(obj); }

        void Set(UObject* obj);
        void Reset() { m_raw = nullptr; m_index = -1; m_serial = 0; }
        UObject* Get() const;
        template<typename T> T* As() const { return static_cast<T*>(Get()); }
        bool IsSet() const { return m_raw != nullptr; }

        // Pointer comparison only; never dereferences.
        bool Is(const void* obj) const { return m_raw && m_raw == obj; }

    private:
        UObject* m_raw = nullptr;
        int32_t m_index = -1;
        int32_t m_serial = 0;
    };

    // Same check for a raw pointer we did not create a handle for.
    bool IsLive(UObject* obj);

    // Calls a reflected UFunction with a parameter buffer laid out from the function's own
    // reflection data, so missing/extra params or engine-version layout differences can't
    // corrupt the stack the way hand-written param structs did.
    class Call
    {
    public:
        Call(UObject* obj, const wchar_t* functionName);
        ~Call();
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;

        bool Valid() const { return m_obj && m_fn; }
        FProperty* ArgProp(int index) const
        {
            return index >= 0 && index < static_cast<int>(m_args.size()) ? m_args[index] : nullptr;
        }
        int NumArgs() const { return static_cast<int>(m_args.size()); }

        // Set argument by declaration index (return value excluded).
        template<typename T>
        Call& Arg(int index, const T& value)
        {
            void* p = ArgPtr(index, sizeof(T));
            if (p) std::memcpy(p, &value, sizeof(T));
            return *this;
        }

        // Raw pointer to an argument slot, or nullptr if out of range / too small.
        void* ArgPtr(int index, size_t minSize = 0);

        // FString argument (allocated with the engine allocator; freed with the call).
        bool ArgString(int index, const std::wstring& value);

        // Don't destroy this argument when the call ends: use after bit-copying a value we
        // don't own into it (e.g. another call's FText return value).
        void Forget(int index) { if (index >= 0 && index < static_cast<int>(m_forget.size())) m_forget[index] = true; }
        // Same for the return value: its contents were handed over to something else.
        void ForgetRet() { m_forgetRet = true; }

        // Raw pointer to the return value (valid until this Call is destroyed).
        void* RetPtr() { return m_ret ? m_buf.ptr + RetOffset() : nullptr; }

        bool Invoke();

        template<typename T>
        T Ret() const
        {
            T out{};
            if (m_ret && sizeof(T) <= RetSize()) std::memcpy(&out, m_buf.data() + RetOffset(), sizeof(T));
            return out;
        }

    private:
        size_t RetOffset() const;
        size_t RetSize() const;

        UObject* m_obj = nullptr;
        UFunction* m_fn = nullptr;
        std::vector<FProperty*> m_args;
        std::vector<bool> m_forget;
        bool m_forgetRet = false;
        FProperty* m_ret = nullptr;
        std::vector<uint8_t> m_storage;
        struct Span { uint8_t* ptr = nullptr; size_t size = 0; uint8_t* data() const { return ptr; } } m_buf;
    };

    // Reflection lookups by name, cached per class (a name lookup builds an FName and walks
    // the class chain, which adds up when done for every puppet every frame).
    FProperty* FindProp(UObject* obj, const wchar_t* name);
    // Same for a member of a struct type (including its base structs).
    FProperty* FindStructMember(RC::Unreal::UStruct* st, const wchar_t* name);
    // Drop cached lookups; call when a map unloads (classes can go with it).
    void ClearCaches();

    // Property access by name on a live object. Returns nullptr if missing.
    template<typename T>
    T* Prop(UObject* obj, const wchar_t* name)
    {
        FProperty* p = obj ? FindProp(obj, name) : nullptr;
        return p ? reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + p->GetOffset_Internal()) : nullptr;
    }

    inline UObject* ObjProp(UObject* obj, const wchar_t* name)
    {
        auto** p = Prop<UObject*>(obj, name);
        return p ? *p : nullptr;
    }

    // "/Game/Foo/Bar.Bar" for assets, or a full sub-object path.
    std::wstring PathOf(UObject* obj);
    std::wstring ClassNameOf(UObject* obj);

    // True for objects that can be looked up by path on another machine
    // (content assets, not transient/runtime-created objects).
    bool IsSharedAssetPath(const std::wstring& path);

    UObject* FindByPath(const std::wstring& path);
    UObject* FindClass(const wchar_t* path);

    std::string Narrow(const std::wstring& s);
    std::wstring Widen(const std::string& s);

    // Raw view of a TArray in memory.
    struct RawArray
    {
        uint8_t* Data;
        int32_t Num;
        int32_t Max;
    };
    static_assert(sizeof(RawArray) == 16);

    // Pointer to a named member inside a struct value (e.g. an FCustomizationSingle out param).
    void* StructMember(FProperty* structProp, void* structValue, const wchar_t* member);

    // Overwrite an FString in place (engine allocator).
    void SetFString(void* fstring, const std::wstring& value);

    // FText <-> string through UKismetTextLibrary.
    std::wstring TextToString(const void* ftext);
    // UTextBlock::SetText(FText) with a plain string.
    bool SetTextBlock(UObject* textBlock, const std::wstring& text);
    // Overwrite an FText property with a plain string.
    bool SetTextProperty(UObject* obj, const wchar_t* prop, const std::wstring& text);
}
