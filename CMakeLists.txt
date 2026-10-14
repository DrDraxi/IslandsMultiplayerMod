project(IslandsMultiplayerMod)

set(TARGET IslandsMultiplayerMod)

# Steamworks SDK (headers + steam_api64.lib). Defaults to a folder next to this one.
set(STEAMWORKS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../steamworks_sdk" CACHE PATH "Path to the Steamworks SDK root")

set(${TARGET}_Sources
    "${CMAKE_CURRENT_SOURCE_DIR}/dllmain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/GameDiscovery.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SteamIntegration.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/PlayerSync.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Appearance.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/UEUtil.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/MainMenu.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Chat.cpp"
)

add_library(${TARGET} SHARED ${${TARGET}_Sources})
target_include_directories(${TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${STEAMWORKS_SDK_DIR}/public")
target_link_libraries(${TARGET} PRIVATE ImGui)
target_link_libraries(${TARGET} PUBLIC UE4SS)

# Steamworks SDK - delay-loaded so the mod DLL doesn't require steam_api64.dll at load time
target_link_directories(${TARGET} PRIVATE "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64")
target_link_libraries(${TARGET} PRIVATE steam_api64 delayimp)
target_link_options(${TARGET} PRIVATE "/DELAYLOAD:steam_api64.dll")
