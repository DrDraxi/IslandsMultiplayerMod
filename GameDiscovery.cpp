#include "GameDiscovery.h"
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Constructs/Loop.hpp>
#include <string>
#include <algorithm>
#include <fstream>

using namespace RC;
using namespace RC::Unreal;

namespace IslandsMP
{
    UClass* FindGameClass(const wchar_t* className)
    {
        UClass* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, className);
        if (!cls)
        {
            std::wstring withSuffix = std::wstring(className) + STR("_C");
            cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, withSuffix.c_str());
        }
        if (cls)
            Output::send<LogLevel::Verbose>(STR("[IslandsMP Discovery] Found class: {}\n"), cls->GetFullName());
        else
            Output::send<LogLevel::Warning>(STR("[IslandsMP Discovery] Class NOT found: {}\n"), className);
        return cls;
    }

    void DumpClassDetails(UClass* cls)
    {
        if (!cls) return;

        Output::send<LogLevel::Default>(STR("[IslandsMP Discovery] === {} ===\n"), cls->GetFullName());

        Output::send<LogLevel::Default>(STR("[IslandsMP Discovery] Properties:\n"));
        for (FProperty* prop : cls->ForEachProperty())
        {
            Output::send<LogLevel::Default>(STR("  - {} ({})\n"),
                prop->GetName(), prop->GetClass().GetName());
        }

        Output::send<LogLevel::Default>(STR("[IslandsMP Discovery] Functions:\n"));
        for (UFunction* func : cls->ForEachFunction())
        {
            Output::send<LogLevel::Default>(STR("  - {}\n"), func->GetName());
        }
    }

    void FindObjectsContaining(const wchar_t* substring)
    {
        std::wstring sub(substring);
        std::wstring subLower = sub;
        std::transform(subLower.begin(), subLower.end(), subLower.begin(), ::towlower);
        int count = 0;

        UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t /*index*/, int32_t /*chunk*/) -> LoopAction {
            std::wstring name = obj->GetFullName();
            std::wstring nameLower = name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);

            if (nameLower.find(subLower) != std::wstring::npos)
            {
                Output::send<LogLevel::Default>(STR("[IslandsMP Discovery] {}\n"), name);
                count++;
            }
            return LoopAction::Continue;
        });

        Output::send<LogLevel::Default>(
            STR("[IslandsMP Discovery] Found {} objects containing '{}'\n"),
            std::to_wstring(count), sub);
    }

    void DiscoverGameClasses()
    {
        Output::send<LogLevel::Default>(
            STR("\n========================================\n"
                "[IslandsMP] Phase 0: Game Discovery\n"
                "========================================\n\n"));

        Output::send<LogLevel::Default>(STR("--- Player Character Classes ---\n"));
        FindObjectsContaining(STR("Character"));
        FindObjectsContaining(STR("Seeker"));
        FindObjectsContaining(STR("Player"));

        Output::send<LogLevel::Default>(STR("\n--- Movement Components ---\n"));
        FindObjectsContaining(STR("MovementComponent"));
        FindObjectsContaining(STR("CharacterMovement"));

        Output::send<LogLevel::Default>(STR("\n--- Multiplayer / Online ---\n"));
        FindObjectsContaining(STR("Kraken"));
        FindObjectsContaining(STR("OnlineSubsystem"));
        FindObjectsContaining(STR("GameSession"));
        FindObjectsContaining(STR("GameMode"));
        FindObjectsContaining(STR("GameState"));
        FindObjectsContaining(STR("PlayerState"));

        Output::send<LogLevel::Default>(STR("\n--- Session / Lobby ---\n"));
        FindObjectsContaining(STR("Session"));
        FindObjectsContaining(STR("Lobby"));
        FindObjectsContaining(STR("Matchmak"));

        Output::send<LogLevel::Default>(STR("\n--- Social / Party ---\n"));
        FindObjectsContaining(STR("Social"));
        FindObjectsContaining(STR("Party"));
        FindObjectsContaining(STR("Invite"));

        Output::send<LogLevel::Default>(STR("\n--- Animation ---\n"));
        FindObjectsContaining(STR("AnimInstance"));
        FindObjectsContaining(STR("AnimBlueprint"));
        FindObjectsContaining(STR("Glid"));

        Output::send<LogLevel::Default>(
            STR("\n========================================\n"
                "[IslandsMP] Discovery complete. Review output above.\n"
                "========================================\n\n"));
    }

    void DumpAllObjectsToFile(const char* label)
    {
        std::string filename = std::string("IslandsMP_Dump_") + label + ".txt";
        std::wstring wFilename(filename.begin(), filename.end());

        std::wofstream file(wFilename);
        if (!file.is_open())
        {
            Output::send<LogLevel::Warning>(STR("[IslandsMP] Failed to open dump file\n"));
            return;
        }

        Output::send<LogLevel::Default>(STR("[IslandsMP] Dumping all UObjects to {}...\n"), wFilename);

        int count = 0;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t /*index*/, int32_t /*chunk*/) -> LoopAction {
            std::wstring fullName = obj->GetFullName();
            file << fullName << L"\n";
            count++;
            return LoopAction::Continue;
        });

        file.close();
        Output::send<LogLevel::Default>(STR("[IslandsMP] Dumped {} objects to {}\n"),
            count, wFilename);
    }
}
