#include "MainMenu.h"
#include "PlayerSync.h"
#include "UEUtil.h"
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <cstring>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace IslandsMP
{
    using UE::Call;
    using UE::ObjProp;
    using UE::Prop;

    // Title screen layout:
    //   centre (the game's big Play button) -> "Play online"
    //   left column, above Options/Exit     -> "Play with friends", "Play offline"
    // Every way of starting goes through the menu's own Play handler, which we hook to pick
    // the session for that start.

    static constexpr const wchar_t* kMenuClass = L"WBP_ChooseGameType_C";
    static constexpr const wchar_t* kButtonClass = L"/Game/ASophia/UI/Menus/LoginScreenAssets/WBP_PlayGameBar.WBP_PlayGameBar_C";
    static constexpr const wchar_t* kButtonClicked =
        L"/Game/ASophia/UI/Menus/LoginScreenAssets/WBP_PlayGameBar.WBP_PlayGameBar_C:"
        L"BndEvt__WBP_PlayGameBar_ListenButton_K2Node_ComponentBoundEvent_3_OnButtonClickedEvent__DelegateSignature";
    static constexpr const wchar_t* kMenuPlayName =
        L"BndEvt__WBP_ChooseGameType_WBP_PlayGameBarPrimary_Short_1_K2Node_ComponentBoundEvent_9_OnClicked__DelegateSignature";
    static constexpr const wchar_t* kMenuPlayPath =
        L"/Game/ASophia/UI/Menus/WBP_ChooseGameType.WBP_ChooseGameType_C:"
        L"BndEvt__WBP_ChooseGameType_WBP_PlayGameBarPrimary_Short_1_K2Node_ComponentBoundEvent_9_OnClicked__DelegateSignature";
    static constexpr const wchar_t* kPlayOnline = L"PLAY ONLINE";

    static UE::Handle s_menu;
    static UE::Handle s_friendsBtn, s_offlineBtn;
    static float s_scanTimer = 0.0f, s_relabelTimer = 0.0f;
    static bool s_hooked = false;
    static int s_clicked = 0;                         // set by the click hook: 1 friends, 2 offline
    static bool s_havePending = false;                // a left-column button is starting the game
    static SessionMode s_pending = SessionMode::Solo;

    static std::wstring ClassOf(UObject* o) { return o ? UE::ClassNameOf(o) : L"null"; }

    // The left column grows downward when we add buttons, which pushes Options/Exit off the
    // bottom of the screen. Once the new size is laid out, move the column up by the growth.
    static UE::Handle s_column;
    static float s_columnHeightBefore = 0.0f;
    static float s_columnFixTimer = -1.0f; // counts down to the fix; <0 = nothing pending

    static float DesiredHeight(UObject* widget)
    {
        Call c(widget, L"GetDesiredSize");
        if (!c.Invoke() || !c.RetPtr()) return 0.0f;
        return static_cast<const float*>(c.RetPtr())[1];
    }

    static void FixColumnPosition()
    {
        UObject* column = s_column.Get();
        if (!column) return;
        float after = DesiredHeight(column);
        float grow = after - s_columnHeightBefore;
        UObject* slot = ObjProp(column, L"Slot");
        Output::send<LogLevel::Default>(STR("[IslandsMP] Left column height {:.0f} -> {:.0f}, slot {}\n"),
            s_columnHeightBefore, after, ClassOf(slot));
        if (grow <= 1.0f || s_columnHeightBefore <= 0.0f || ClassOf(slot) != L"CanvasPanelSlot") return;
        auto* layout = Prop<float>(slot, L"LayoutData"); // Offsets L,T,R,B; Anchors min x,y max x,y; Alignment x,y
        if (!layout) return;
        float data[10];
        std::memcpy(data, layout, sizeof(data));
        bool stretched = data[5] != data[7];
        auto* autoSize = Prop<bool>(slot, L"bAutoSize");
        // Keep the bottom edge where it was: the top moves up by the growth.
        data[1] -= stretched ? grow : grow * (1.0f - data[9]); // alignment y sets how far the top moves
        if (!stretched && !(autoSize && *autoSize)) data[3] += grow; // fixed height: make room
        Call set(slot, L"SetLayout");
        if (void* p = set.ArgPtr(0, sizeof(data))) std::memcpy(p, data, sizeof(data));
        set.Invoke();
    }

    static UObject* ParentOf(UObject* widget)
    {
        Call get(widget, L"GetParent");
        return get.Invoke() ? get.Ret<UObject*>() : nullptr;
    }

    static UObject* FindMenu()
    {
        std::vector<UObject*> found;
        UObjectGlobals::FindAllOf(kMenuClass, found);
        for (UObject* m : found)
            if (UE::IsLive(m) && m->GetName().rfind(STR("Default__"), 0) != 0) return m;
        return nullptr;
    }

    static void InstallHooks()
    {
        if (s_hooked) return;
        if (!UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kButtonClicked)
            || !UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kMenuPlayPath))
            return;
        UObjectGlobals::RegisterHook(StringType(kButtonClicked),
            [](UnrealScriptFunctionCallableContext&, void*) {},
            [](UnrealScriptFunctionCallableContext& ctx, void*) {
                if (s_friendsBtn.Is(ctx.Context)) s_clicked = 1;
                else if (s_offlineBtn.Is(ctx.Context)) s_clicked = 2;
            }, nullptr);
        // Runs before the menu starts the game: the big button means online, ours say otherwise.
        UObjectGlobals::RegisterHook(StringType(kMenuPlayPath),
            [](UnrealScriptFunctionCallableContext&, void*) {
                ChooseSessionFromMenu(s_havePending ? s_pending : SessionMode::Public);
                s_havePending = false;
            },
            [](UnrealScriptFunctionCallableContext&, void*) {}, nullptr);
        s_hooked = true;
    }

    static UObject* CreateButton(UObject* menu, const wchar_t* label)
    {
        UObject* lib = UE::FindByPath(L"/Script/UMG.Default__WidgetBlueprintLibrary");
        UObject* cls = UE::FindClass(kButtonClass);
        if (!lib || !cls) return nullptr;
        UObject* owner = nullptr;
        {
            Call get(menu, L"GetOwningPlayer");
            if (get.Invoke()) owner = get.Ret<UObject*>();
        }
        Call create(lib, L"Create");
        create.Arg<UObject*>(0, menu);
        create.Arg<UObject*>(1, cls);
        create.Arg<UObject*>(2, owner);
        if (!create.Invoke()) return nullptr;
        UObject* btn = create.Ret<UObject*>();
        if (!UE::IsLive(btn)) return nullptr;
        UE::SetTextProperty(btn, L"In Text", label);
        UE::SetTextBlock(ObjProp(btn, L"TextBlock_0"), label);
        return btn;
    }

    // Layout of a vertical box slot, so re-added widgets keep their look.
    struct BoxSlot
    {
        bool ok = false;
        uint8_t padding[16]{};
        uint8_t size[8]{};
        uint8_t hAlign = 0, vAlign = 0;
    };

    static BoxSlot ReadSlot(UObject* widget)
    {
        BoxSlot s;
        UObject* slot = ObjProp(widget, L"Slot");
        if (!slot) return s;
        auto* pad = Prop<uint8_t>(slot, L"Padding");
        auto* size = Prop<uint8_t>(slot, L"Size");
        auto* h = Prop<uint8_t>(slot, L"HorizontalAlignment");
        auto* v = Prop<uint8_t>(slot, L"VerticalAlignment");
        if (!pad || !size || !h || !v) return s;
        std::memcpy(s.padding, pad, 16);
        std::memcpy(s.size, size, 8);
        s.hAlign = *h;
        s.vAlign = *v;
        s.ok = true;
        return s;
    }

    static void ApplySlot(UObject* slot, const BoxSlot& s)
    {
        if (!slot || !s.ok) return;
        { Call c(slot, L"SetPadding"); if (void* p = c.ArgPtr(0, 16)) std::memcpy(p, s.padding, 16); c.Invoke(); }
        { Call c(slot, L"SetSize"); if (void* p = c.ArgPtr(0, 8)) std::memcpy(p, s.size, 8); c.Invoke(); }
        { Call c(slot, L"SetHorizontalAlignment"); c.Arg<uint8_t>(0, s.hAlign); c.Invoke(); }
        { Call c(slot, L"SetVerticalAlignment"); c.Arg<uint8_t>(0, s.vAlign); c.Invoke(); }
    }

    static UObject* AddTo(UObject* panel, UObject* widget, const BoxSlot& look)
    {
        Call add(panel, L"AddChild");
        add.Arg<UObject*>(0, widget);
        UObject* slot = add.Invoke() ? add.Ret<UObject*>() : nullptr;
        ApplySlot(slot, look);
        return slot;
    }

    // Put our buttons at the top of the column that holds Options/Exit. Panels have no
    // "insert at", so the existing children are taken out and added back after ours.
    static void AddToLeftColumn(UObject* column, const std::vector<UObject*>& ours)
    {
        std::vector<UObject*> old;
        std::vector<BoxSlot> looks;
        {
            Call count(column, L"GetChildrenCount");
            int n = count.Invoke() ? count.Ret<int32_t>() : 0;
            for (int i = 0; i < n; i++)
            {
                Call at(column, L"GetChildAt");
                at.Arg<int32_t>(0, i);
                UObject* w = at.Invoke() ? at.Ret<UObject*>() : nullptr;
                if (!w) continue;
                old.push_back(w);
                looks.push_back(ReadSlot(w));
            }
        }
        BoxSlot buttonLook;
        for (size_t i = 0; i < old.size(); i++)
            if (looks[i].ok && ClassOf(old[i]) == L"WBP_PlayGameBar_C") { buttonLook = looks[i]; break; }

        s_column.Set(column);
        s_columnHeightBefore = DesiredHeight(column);
        s_columnFixTimer = 0.3f;

        bool box = ClassOf(column).find(L"Box") != std::wstring::npos;
        if (box && !old.empty())
        {
            Call clear(column, L"ClearChildren");
            clear.Invoke();
        }
        for (UObject* w : ours) AddTo(column, w, buttonLook);
        if (box)
            for (size_t i = 0; i < old.size(); i++) AddTo(column, old[i], looks[i]);
        Output::send<LogLevel::Default>(STR("[IslandsMP] Left column {} ({}): {} buttons kept, {} added\n"),
            column->GetName(), ClassOf(column), old.size(), ours.size());
    }

    static void RelabelPlay(UObject* menu)
    {
        UObject* play = ObjProp(menu, L"WBP_PlayGameBarPrimary_Short_1");
        if (!UE::IsLive(play)) return;
        auto* current = Prop<uint8_t>(play, L"In Text");
        if (current && UE::TextToString(current) == kPlayOnline) return;
        UE::SetTextProperty(play, L"In Text", kPlayOnline);
        UE::SetTextBlock(ObjProp(play, L"TextBlock_0"), kPlayOnline);
    }

    static std::wstring WidgetText(UObject* w)
    {
        std::wstring cls = ClassOf(w);
        if (cls != L"TextBlock" && cls != L"RichTextBlock") return {};
        Call get(w, L"GetText");
        return get.Invoke() && get.RetPtr() ? UE::TextToString(get.RetPtr()) : std::wstring{};
    }

    // Hide the "Solo play without connecting to any servers..." line under the Play button;
    // it no longer describes what that button does.
    static void HideSoloBlurb(UObject* panel, int depth = 0)
    {
        if (!panel || depth > 4) return;
        Call count(panel, L"GetChildrenCount");
        if (!count.Valid() || !count.Invoke()) return;
        int n = count.Ret<int32_t>();
        for (int i = 0; i < n; i++)
        {
            Call at(panel, L"GetChildAt");
            at.Arg<int32_t>(0, i);
            UObject* w = at.Invoke() ? at.Ret<UObject*>() : nullptr;
            if (!w) continue;
            std::wstring text = WidgetText(w);
            if (text.find(L"Solo play") != std::wstring::npos || text.find(L"stored locally") != std::wstring::npos)
            {
                Call vis(w, L"SetVisibility");
                vis.Arg<uint8_t>(0, 1); // Collapsed
                vis.Invoke();
                Output::send<LogLevel::Default>(STR("[IslandsMP] Hid title text {}\n"), w->GetName());
                continue;
            }
            if (ClassOf(w).find(L"Box") != std::wstring::npos || ClassOf(w).find(L"Canvas") != std::wstring::npos
                || ClassOf(w).find(L"Overlay") != std::wstring::npos)
                HideSoloBlurb(w, depth + 1);
        }
    }

    // Reset Progress moves to the bottom-right corner of the screen.
    static void MoveResetButton(UObject* menu)
    {
        UObject* reset = ObjProp(menu, L"WBP_ResetProgressBtn");
        UObject* canvas = ObjProp(menu, L"BaseCanvas");
        if (!UE::IsLive(reset) || !UE::IsLive(canvas)) return;
        { Call rm(reset, L"RemoveFromParent"); rm.Invoke(); }
        Call add(canvas, L"AddChild");
        add.Arg<UObject*>(0, reset);
        UObject* slot = add.Invoke() ? add.Ret<UObject*>() : nullptr;
        if (!slot) return;
        // Offsets L,T,R,B; anchors min x,y / max x,y; alignment x,y: pinned to the corner.
        const float layout[10] = {-48.0f, -40.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        { Call c(slot, L"SetLayout"); if (void* p = c.ArgPtr(0, sizeof(layout))) std::memcpy(p, layout, sizeof(layout)); c.Invoke(); }
        { Call c(slot, L"SetAutoSize"); c.Arg<bool>(0, true); c.Invoke(); }
        { Call c(slot, L"SetZOrder"); c.Arg<int32_t>(0, 50); c.Invoke(); }
        Output::send<LogLevel::Default>(STR("[IslandsMP] Moved Reset Progress to the corner ({})\n"), ClassOf(slot));
    }

    static void Inject(UObject* menu)
    {
        UObject* play = ObjProp(menu, L"WBP_PlayGameBarPrimary_Short_1");
        UObject* options = ObjProp(menu, L"OptionsButton");
        UObject* column = UE::IsLive(options) ? ParentOf(options) : nullptr;
        Output::send<LogLevel::Default>(STR("[IslandsMP] Title screen found: Play in {}, Options in {} ({})\n"),
            ClassOf(play ? ParentOf(play) : nullptr), column ? column->GetName() : StringType(STR("?")), ClassOf(column));

        RelabelPlay(menu);
        if (UObject* box = play ? ParentOf(play) : nullptr)
        {
            UObject* outer = ParentOf(box);
            HideSoloBlurb(outer ? outer : box);
        }
        MoveResetButton(menu);
        UObject* friends = CreateButton(menu, L"PLAY WITH FRIENDS");
        UObject* offline = CreateButton(menu, L"PLAY OFFLINE");
        s_friendsBtn.Set(friends);
        s_offlineBtn.Set(offline);
        std::vector<UObject*> ours;
        for (UObject* b : {friends, offline}) if (b) ours.push_back(b);
        if (column && !ours.empty()) AddToLeftColumn(column, ours);
        InstallHooks();
        Output::send<LogLevel::Default>(STR("[IslandsMP] Title screen buttons added (friends {}, offline {}, hooks {})\n"),
            friends != nullptr, offline != nullptr, s_hooked);
    }

    void TickMainMenu(float dt)
    {
        if (IsInWorld()) { s_menu.Reset(); return; }

        UObject* menu = s_menu.Get();
        if (!menu)
        {
            s_friendsBtn.Reset();
            s_offlineBtn.Reset();
            // A full object scan, so only now and then, and never while playing.
            if ((s_scanTimer += dt) < 1.0f) return;
            s_scanTimer = 0;
            menu = FindMenu();
            if (!menu) return;
            s_menu.Set(menu);
            Inject(menu);
        }

        if (s_columnFixTimer >= 0.0f && (s_columnFixTimer -= dt) < 0.0f) FixColumnPosition();

        // The menu may set its own label again (e.g. Continue / New Game) after we did.
        if ((s_relabelTimer += dt) >= 1.0f)
        {
            s_relabelTimer = 0;
            RelabelPlay(menu);
        }

        if (s_clicked)
        {
            s_pending = s_clicked == 1 ? SessionMode::Friends : SessionMode::Solo;
            s_havePending = true;
            s_clicked = 0;
            Call play(menu, kMenuPlayName); // same as pressing Play; the hook reads s_pending
            play.Invoke();
        }
    }
}
