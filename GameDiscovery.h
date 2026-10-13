#pragma once

#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>

namespace IslandsMP
{
    void DiscoverGameClasses();
    RC::Unreal::UClass* FindGameClass(const wchar_t* className);
    void FindObjectsContaining(const wchar_t* substring);
    void DumpClassDetails(RC::Unreal::UClass* cls);

    // Dump ALL UObjects to a file for offline analysis
    // label is used in the filename: IslandsMP_Dump_<label>.txt
    void DumpAllObjectsToFile(const char* label = "default");
}
