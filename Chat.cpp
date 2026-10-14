#include "Chat.h"
#include "PlayerSync.h"
#include "UEUtil.h"
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <chrono>
#include <cmath>
#include <deque>

extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int vKey);

using namespace RC;
using namespace RC::Unreal;

namespace IslandsMP
{
    using UE::Call;
    using UE::ObjProp;
    using UE::Prop;

    static constexpr const wchar_t* kChatClass = L"/Game/ASophia/UI/HUD/WBP_ChatBox.WBP_ChatBox_C";
    static constexpr const wchar_t* kCommitted =
        L"/Game/ASophia/UI/HUD/WBP_ChatBox.WBP_ChatBox_C:"
        L"BndEvt__WBP_ChatBox_MessageInput_K2Node_ComponentBoundEvent_0_OnEditableTextCommittedEvent__DelegateSignature";
    static constexpr double kFadeAfter = 6.0;      // an incoming message shows the box this long
    static constexpr size_t kMaxChatChars = 200;

    enum ESlateVisibility : uint8_t { Visible = 0, Collapsed = 1, Hidden = 2, HitTestInvisible = 3, SelfHitTestInvisible = 4 };

    static UE::Handle s_chat;
    static bool s_hooked = false;
    static bool s_typing = false;
    static bool s_shown = false;
    static double s_lastActivity = 0;
    static double s_closedAt = 0;
    static double s_typingSince = 0;
    static bool s_commitPending = false;   // set by the commit hook
    static std::wstring s_commitText;
    static uint8_t s_commitMethod = 0;
    static std::deque<std::pair<std::wstring, std::wstring>> s_queue; // shown once the widget exists

    static double Now()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // ---- UTF-8 <-> UTF-16 (chat can contain any language) ----------------------------

    static std::string ToUtf8(const std::wstring& w)
    {
        std::string out;
        for (size_t i = 0; i < w.size(); i++)
        {
            uint32_t c = w[i];
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size())
            {
                uint32_t lo = w[i + 1];
                if (lo >= 0xDC00 && lo <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; }
            }
            if (c < 0x80) out += static_cast<char>(c);
            else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
            else { out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        }
        return out;
    }

    static std::wstring FromUtf8(const std::string& s)
    {
        std::wstring out;
        for (size_t i = 0; i < s.size();)
        {
            uint8_t b = static_cast<uint8_t>(s[i]);
            uint32_t c = 0;
            int extra = 0;
            if (b < 0x80) { c = b; }
            else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; extra = 1; }
            else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; extra = 2; }
            else if ((b & 0xF8) == 0xF0) { c = b & 0x07; extra = 3; }
            else { i++; continue; }
            if (i + extra >= s.size() && extra) break; // truncated sequence
            for (int k = 1; k <= extra; k++) c = (c << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
            i += 1 + extra;
            if (c >= 0x10000) { c -= 0x10000; out += static_cast<wchar_t>(0xD800 + (c >> 10)); out += static_cast<wchar_t>(0xDC00 + (c & 0x3FF)); }
            else out += static_cast<wchar_t>(c);
        }
        return out;
    }

    // ---- Widget ------------------------------------------------------------------------

    static double s_enterAt = 0;        // last Enter key (UE4SS key event) while typing
    static std::wstring s_lastTyped;     // input text seen last frame while typing

    // The chat box sends through the old party code (Server_SendChatMessage on the character
    // or the party component). Catch the text there, whichever one it uses.
    static void HookNativeSend()
    {
        static bool done = false;
        if (done) return;
        done = true;
        for (const wchar_t* fn : {STR("/Script/IslandsofInsight.SophiaCharacter:Server_SendChatMessage"),
                                  STR("/Script/IslandsofInsight.PartySystemComponent:Server_SendChatMessage")})
        {
            if (!UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, fn)) continue;
            UObjectGlobals::RegisterHook(StringType(fn),
                [](UnrealScriptFunctionCallableContext& ctx, void*) {
                    struct Params { UObject* sender; uint8_t text[0x18]; };
                    s_commitText = UE::TextToString(ctx.GetParams<Params>().text);
                    s_commitMethod = 1; // OnEnter
                    s_commitPending = true;
                },
                [](UnrealScriptFunctionCallableContext&, void*) {}, nullptr);
        }
        Output::send<LogLevel::Default>(STR("[IslandsMP] Chat send hooks installed\n"));
    }

    static void InstallHook()
    {
        HookNativeSend();
        if (s_hooked) return;
        if (!UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kCommitted)) return;
        UObjectGlobals::RegisterHook(StringType(kCommitted),
            [](UnrealScriptFunctionCallableContext& ctx, void*) {
                // (const FText& Text, ETextCommit CommitMethod): read before the Blueprint runs.
                if (!s_chat.Is(ctx.Context)) return;
                struct Params { uint8_t text[0x18]; uint8_t method; };
                auto& p = ctx.GetParams<Params>();
                // Only a real Enter commit with text counts. The box also "commits" empty text
                // when it clears itself (OnCleared) and when focus moves; those must not close
                // the chat. An Enter the box swallows itself is caught by the fallback in TickChat.
                constexpr uint8_t OnEnter = 1;
                std::wstring text = UE::TextToString(p.text);
                if (p.method != OnEnter || text.empty()) return;
                s_commitText = text;
                s_commitMethod = p.method;
                s_commitPending = true;
            },
            [](UnrealScriptFunctionCallableContext&, void*) {}, nullptr);
        s_hooked = true;
    }

    static UObject* LocalPC() { return LocalPlayerController(); }

    static void SetVisibility(UObject* widget, uint8_t v)
    {
        if (!widget) return;
        Call c(widget, L"SetVisibility");
        c.Arg<uint8_t>(0, v);
        c.Invoke();
    }

    static UObject* Chat(bool create)
    {
        UObject* chat = s_chat.Get();
        if (chat || !create || !IsInWorld()) return chat;
        // Our own instance, not the HUD's: the HUD keeps its copy hidden while there's no party,
        // which made the box flicker or only show up in the pause menu.
        UObject* cls = UE::FindClass(kChatClass);
        UObject* lib = UE::FindByPath(L"/Script/UMG.Default__WidgetBlueprintLibrary");
        UObject* pc = LocalPC();
        if (!cls || !lib || !pc) return nullptr;
        Call make(lib, L"Create");
        make.Arg<UObject*>(0, pc);
        make.Arg<UObject*>(1, cls);
        make.Arg<UObject*>(2, pc);
        chat = make.Invoke() ? make.Ret<UObject*>() : nullptr;
        if (!UE::IsLive(chat)) return nullptr;
        Call add(chat, L"AddToViewport");
        add.Arg<int32_t>(0, 20);
        add.Invoke();
        s_chat.Set(chat);
        InstallHook();
        SetVisibility(chat, Collapsed);
        s_shown = false;
        Output::send<LogLevel::Default>(STR("[IslandsMP] Chat box created\n"));
        return chat;
    }

    // The box says "Party Chat"; there are no parties any more, it's the session's chat.
    static void Retitle(UObject* panel, int depth = 0)
    {
        if (!panel || depth > 6) return;
        Call count(panel, L"GetChildrenCount");
        if (!count.Valid() || !count.Invoke()) return;
        int n = count.Ret<int32_t>();
        for (int i = 0; i < n; i++)
        {
            Call at(panel, L"GetChildAt");
            at.Arg<int32_t>(0, i);
            UObject* w = at.Invoke() ? at.Ret<UObject*>() : nullptr;
            if (!w) continue;
            std::wstring cls = UE::ClassNameOf(w);
            if (cls == L"TextBlock")
            {
                Call g(w, L"GetText");
                std::wstring s = g.Invoke() && g.RetPtr() ? UE::TextToString(g.RetPtr()) : L"";
                std::wstring lower = s;
                for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
                if (lower.find(L"party") != std::wstring::npos)
                    UE::SetTextBlock(w, iswupper(s.empty() ? L'a' : s[0]) && iswupper(s.size() > 1 ? s[1] : L'a') ? L"CHAT" : L"Chat");
            }
            else if (cls.find(L"Panel") != std::wstring::npos || cls.find(L"Box") != std::wstring::npos
                     || cls.find(L"Overlay") != std::wstring::npos || cls.find(L"Border") != std::wstring::npos)
                Retitle(w, depth + 1);
        }
    }

    static void Show(UObject* chat, bool input)
    {
        if (!chat) return;
        Retitle(ObjProp(chat, L"WidgetTree") ? ObjProp(ObjProp(chat, L"WidgetTree"), L"RootWidget") : nullptr);
        // The Blueprint may take itself off the screen after a message; put it back.
        Call inView(chat, L"IsInViewport");
        if (inView.Invoke() && !inView.Ret<bool>())
        {
            Call add(chat, L"AddToViewport");
            add.Arg<int32_t>(0, 20);
            add.Invoke();
        }
        SetVisibility(chat, input ? Visible : SelfHitTestInvisible);
        SetVisibility(ObjProp(chat, L"RootPanel"), SelfHitTestInvisible);
        SetVisibility(ObjProp(chat, L"MessageFeed"), SelfHitTestInvisible);
        SetVisibility(ObjProp(chat, L"MessageInput"), input ? Visible : Hidden);
        for (UObject* w : {chat, ObjProp(chat, L"RootPanel")})
            if (w) { Call op(w, L"SetRenderOpacity"); op.Arg<float>(0, 1.0f); op.Invoke(); }
        s_shown = true;
        s_lastActivity = Now();

        static int logged = 0;
        if (logged++ < 3)
        {
            auto vis = [](UObject* w) -> int {
                if (!w) return -1;
                Call v(w, L"GetVisibility");
                return v.Invoke() ? v.Ret<uint8_t>() : -2;
            };
            float size[2] = {0, 0};
            { Call ds(chat, L"GetDesiredSize"); if (ds.Invoke() && ds.RetPtr()) std::memcpy(size, ds.RetPtr(), 8); }
            Output::send<LogLevel::Default>(STR("[IslandsMP] Chat shown: box {} root {} feed {} input {}, size {:.0f}x{:.0f}\n"),
                vis(chat), vis(ObjProp(chat, L"RootPanel")), vis(ObjProp(chat, L"MessageFeed")),
                vis(ObjProp(chat, L"MessageInput")), size[0], size[1]);
        }
    }

    static void AddLine(UObject* chat, const std::wstring& name, const std::wstring& text)
    {
        UObject* textLib = UE::FindByPath(L"/Script/Engine.Default__KismetTextLibrary");
        if (!chat || !textLib) return;
        Call conv(textLib, L"Conv_StringToText");
        if (!conv.ArgString(0, text) || !conv.Invoke() || !conv.RetPtr()) return;
        Call add(chat, L"AddMessage");
        if (!add.ArgString(0, name)) return;
        void* msg = add.ArgPtr(1, 0x18);
        if (!msg) return;
        std::memcpy(msg, conv.RetPtr(), 0x18);
        add.Forget(1); // conv owns the FText; AddMessage copies it
        add.Invoke();
        // Keep the newest line in view.
        if (UObject* feed = ObjProp(chat, L"MessageFeed")) { Call end(feed, L"ScrollToEnd"); end.Invoke(); }
    }

    // ---- Floating messages ---------------------------------------------------------------
    // Every line (yours too) also pops up as one of the game's party chat notifications on the
    // HUD, then fades out. The chat box itself only shows while you type, with the history.

    static constexpr double kToastHold = 6.0, kToastFade = 2.0;
    static constexpr size_t kMaxToasts = 6;
    struct Toast { UE::Handle widget; double born; };
    static UE::Handle s_toastHost;
    static std::deque<Toast> s_toasts;

    // A second chat box with only its message list left visible: the floating lines sit
    // where the chat is, and come from the same AddMessage the history uses.
    static void HideChrome(UObject* panel, UObject* keep, int depth = 0)
    {
        if (!panel || depth > 8) return;
        Call count(panel, L"GetChildrenCount");
        if (!count.Valid() || !count.Invoke()) return;
        int n = count.Ret<int32_t>();
        for (int i = 0; i < n; i++)
        {
            Call at(panel, L"GetChildAt");
            at.Arg<int32_t>(0, i);
            UObject* w = at.Invoke() ? at.Ret<UObject*>() : nullptr;
            if (!w || w == keep) continue;
            std::wstring cls = UE::ClassNameOf(w);
            bool container = cls.find(L"Panel") != std::wstring::npos || cls.find(L"Box") != std::wstring::npos
                          || cls.find(L"Overlay") != std::wstring::npos || cls.find(L"Border") != std::wstring::npos
                          || cls.find(L"SizeBox") != std::wstring::npos || cls.find(L"ScaleBox") != std::wstring::npos;
            if (container) HideChrome(w, keep, depth + 1);
            else SetVisibility(w, Collapsed); // images, titles, the text input
        }
    }

    static UObject* ToastHost()
    {
        if (UObject* host = s_toastHost.Get()) return host;
        UObject* cls = UE::FindClass(kChatClass);
        UObject* lib = UE::FindByPath(L"/Script/UMG.Default__WidgetBlueprintLibrary");
        UObject* pc = LocalPC();
        if (!cls || !lib || !pc) return nullptr;
        Call make(lib, L"Create");
        make.Arg<UObject*>(0, pc);
        make.Arg<UObject*>(1, cls);
        make.Arg<UObject*>(2, pc);
        UObject* host = make.Invoke() ? make.Ret<UObject*>() : nullptr;
        if (!UE::IsLive(host)) return nullptr;
        Call add(host, L"AddToViewport");
        add.Arg<int32_t>(0, 15);
        add.Invoke();
        UObject* tree = ObjProp(host, L"WidgetTree");
        HideChrome(tree ? ObjProp(tree, L"RootWidget") : nullptr, ObjProp(host, L"MessageFeed"));
        s_toastHost.Set(host);
        Output::send<LogLevel::Default>(STR("[IslandsMP] Chat pop-up area created\n"));
        return host;
    }

    static void ShowToast(const std::wstring& name, const std::wstring& text)
    {
        UObject* host = ToastHost();
        UObject* feed = host ? ObjProp(host, L"MessageFeed") : nullptr;
        if (!feed) return;
        AddLine(host, name, text);
        // The line AddMessage just added is the feed's last child; fade that one out later.
        Call count(feed, L"GetChildrenCount");
        int n = count.Invoke() ? count.Ret<int32_t>() : 0;
        if (n <= 0) return;
        Call at(feed, L"GetChildAt");
        at.Arg<int32_t>(0, n - 1);
        UObject* line = at.Invoke() ? at.Ret<UObject*>() : nullptr;
        if (!UE::IsLive(line)) return;
        SetVisibility(line, HitTestInvisible);
        s_toasts.push_back({UE::Handle(line), Now()});
        while (s_toasts.size() > kMaxToasts)
        {
            if (UObject* old = s_toasts.front().widget.Get()) { Call rm(old, L"RemoveFromParent"); rm.Invoke(); }
            s_toasts.pop_front();
        }
    }

    static void TickToasts()
    {
        double now = Now();
        for (auto it = s_toasts.begin(); it != s_toasts.end();)
        {
            UObject* w = it->widget.Get();
            double age = now - it->born;
            if (!w || age > kToastHold + kToastFade)
            {
                if (w) { Call rm(w, L"RemoveFromParent"); rm.Invoke(); }
                it = s_toasts.erase(it);
                continue;
            }
            if (age > kToastHold)
            {
                Call op(w, L"SetRenderOpacity");
                op.Arg<float>(0, static_cast<float>(1.0 - (age - kToastHold) / kToastFade));
                op.Invoke();
            }
            ++it;
        }
    }

    static uint8_t VisibilityOf(UObject* w)
    {
        Call v(w, L"GetVisibility");
        return v.Invoke() ? v.Ret<uint8_t>() : 255;
    }

    static void Want(UObject* w, uint8_t vis)
    {
        if (w && VisibilityOf(w) != vis) SetVisibility(w, vis);
    }

    static void KeepInViewport(UObject* w, int32_t z)
    {
        if (!w) return;
        Call inView(w, L"IsInViewport");
        if (inView.Invoke() && !inView.Ret<bool>()) { Call add(w, L"AddToViewport"); add.Arg<int32_t>(0, z); add.Invoke(); }
    }

    // Chat box: on screen only while typing (with the history). Pop-up copy: while it has
    // fading lines and you're not typing.
    static void EnforceState()
    {
        if (UObject* chat = s_chat.Get())
        {
            if (s_typing)
            {
                KeepInViewport(chat, 20);
                Want(chat, Visible);
                Want(ObjProp(chat, L"RootPanel"), SelfHitTestInvisible);
                Want(ObjProp(chat, L"MessageFeed"), SelfHitTestInvisible);
                Want(ObjProp(chat, L"MessageInput"), Visible);
            }
            else Want(chat, Collapsed);
        }
        if (UObject* host = s_toastHost.Get())
        {
            if (!s_toasts.empty() && !s_typing)
            {
                KeepInViewport(host, 15);
                Want(host, SelfHitTestInvisible);
                Want(ObjProp(host, L"RootPanel"), SelfHitTestInvisible);
                Want(ObjProp(host, L"MessageFeed"), SelfHitTestInvisible);
            }
            else Want(host, Collapsed);
        }
    }

    static void Display(const std::wstring& name, const std::wstring& text)
    {
        UObject* chat = Chat(true);
        if (!chat) { s_queue.emplace_back(name, text); return; }
        AddLine(chat, name, text); // history, seen when you open the chat
        ShowToast(name, text);
    }

    static void CloseInput()
    {
        s_typing = false;
        s_closedAt = Now();
        UObject* chat = s_chat.Get();
        if (UObject* input = chat ? ObjProp(chat, L"MessageInput") : nullptr)
        {
            UE::SetTextBlock(input, L""); // EditableText::SetText(FText)
            SetVisibility(input, Hidden);
        }
        if (chat) SetVisibility(chat, Collapsed); // closed means gone; new messages show it briefly
        s_shown = false;
        UObject* lib = UE::FindByPath(L"/Script/UMG.Default__WidgetBlueprintLibrary");
        if (UObject* pc = lib ? LocalPC() : nullptr)
        {
            Call mode(lib, L"SetInputMode_GameOnly");
            mode.Arg<UObject*>(0, pc);
            mode.Invoke();
        }
    }

    // ---- API ---------------------------------------------------------------------------

    void ChatOpenRequested()
    {
        if (s_typing) { s_enterAt = Now(); return; }
        if (!IsInWorld() || Now() - s_closedAt < 0.4) return; // the Enter that sent a message
        UObject* chat = Chat(true);
        if (!chat) return;
        s_typing = true;
        s_typingSince = Now();
        Show(chat, true);
        UObject* input = ObjProp(chat, L"MessageInput");
        UObject* lib = UE::FindByPath(L"/Script/UMG.Default__WidgetBlueprintLibrary");
        UObject* pc = LocalPC();
        if (lib && pc && input)
        {
            // Keys go to the text box, not the character.
            Call mode(lib, L"SetInputMode_UIOnlyEx");
            mode.Arg<UObject*>(0, pc);
            mode.Arg<UObject*>(1, input);
            mode.Invoke();
            Call focus(input, L"SetKeyboardFocus");
            focus.Invoke();
        }
    }

    void ChatShowMessage(const std::string& name, const std::string& text)
    {
        Display(FromUtf8(name), FromUtf8(text));
    }

    void ChatSystemLine(const std::string& text)
    {
        Display(L"", FromUtf8(text));
    }

    void ChatCancel()
    {
        if (s_typing) CloseInput();
    }

    void ChatReset()
    {
        s_chat.Reset();
        s_toastHost.Reset();
        s_toasts.clear();
        s_typing = false;
        s_shown = false;
    }

    void TickChat(float)
    {
        if (!IsInWorld()) return;
        if (!s_queue.empty())
            if (UObject* chat = Chat(true))
            {
                while (!s_queue.empty())
                {
                    AddLine(chat, s_queue.front().first, s_queue.front().second);
                    ShowToast(s_queue.front().first, s_queue.front().second);
                    s_queue.pop_front();
                }
            }

        // Fallback: the box swallows Enter and clears itself without reporting a send. If the
        // text vanished within half a second of an Enter (either order: the key event arrives
        // from another thread), that was a send.
        static std::wstring clearedText;
        static double clearedAt = 0;
        if (s_typing && !s_commitPending)
        {
            UObject* chat = s_chat.Get();
            UObject* input = chat ? ObjProp(chat, L"MessageInput") : nullptr;
            std::wstring now;
            if (input) { Call g(input, L"GetText"); if (g.Invoke() && g.RetPtr()) now = UE::TextToString(g.RetPtr()); }
            if (now.empty() && !s_lastTyped.empty()) { clearedText = s_lastTyped; clearedAt = Now(); }
            if (!clearedText.empty() && std::fabs(clearedAt - s_enterAt) < 0.5)
            {
                s_commitText = clearedText;
                s_commitMethod = 1;
                s_commitPending = true;
                clearedText.clear();
            }
            else if (!clearedText.empty() && Now() - clearedAt > 0.6)
                clearedText.clear(); // emptied without Enter (backspace)
            s_lastTyped = now;
        }
        if (!s_typing) { s_lastTyped.clear(); clearedText.clear(); }

        if (s_commitPending)
        {
            s_commitPending = false;
            Output::send<LogLevel::Default>(STR("[IslandsMP] Chat send: '{}' (method {})\n"), s_commitText, static_cast<int>(s_commitMethod));
            constexpr uint8_t OnEnter = 1;
            std::wstring text = s_commitText;
            while (!text.empty() && iswspace(text.back())) text.pop_back();
            while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
            if (text.size() > kMaxChatChars) text.resize(kMaxChatChars);
            bool send = s_commitMethod == OnEnter && !text.empty();
            CloseInput();
            if (send)
            {
                std::string utf8 = ToUtf8(text);
                // Our own line goes into the feed without popping the closed box back up.
                if (SendChat(utf8)) Display(FromUtf8(LocalDisplayName()), text);
                else ChatSystemLine("Not in a session. Pick PLAY ONLINE or PLAY WITH FRIENDS, or use INVITE.");
            }
        }

        // Typing but the text box doesn't have the keyboard (it never appeared, or something
        // took focus): give the controls back rather than leave the player stuck.
        TickToasts();
        EnforceState();

        if (s_typing)
        {
            constexpr int VK_ESC = 0x1B;
            if (GetAsyncKeyState(VK_ESC) & 0x8000)
            {
                Output::send<LogLevel::Default>(STR("[IslandsMP] Chat cancelled (Esc)\n"));
                CloseInput();
            }
            else
            {
                // Clicking elsewhere can take the keyboard away; give it back to the text box.
                static double refocusAt = 0;
                if (Now() >= refocusAt)
                {
                    refocusAt = Now() + 0.25;
                    if (UObject* input = s_chat.Get() ? ObjProp(s_chat.Get(), L"MessageInput") : nullptr)
                    {
                        Call focus(input, L"SetKeyboardFocus");
                        focus.Invoke();
                    }
                }
            }
        }


    }
}
