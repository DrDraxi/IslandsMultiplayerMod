# Islands of Insight Multiplayer

A UE4SS C++ mod that brings back multiplayer presence to [Islands of Insight](https://store.steampowered.com/app/2071500/) after its official servers shut down. Players see each other walking, gliding and solving, connected peer-to-peer through Steam.

## Features

- **Other players in your world** with their own customization: body, face, hair, outfit, colours, markings, wings (with the glide fade) and the equipped foot trail at its unlocked length.
- **Animation and head look:** idle, walk, run, jump, fall, glide, crouch; the head turns to where the player is looking.
- **Name cards** with name, mastery title and level. They shrink and fade with distance and hide while you're in a puzzle.
- **Map markers** with each player's name.
- **Shared puzzle cursor:** in the same grid puzzle you see the other player's name where their mouse is, placed relative to the puzzle panel (each player's panel faces their own camera).
- **Title screen buttons:**
  - PLAY ONLINE joins a public session with room (up to 10 players), or starts one.
  - PLAY WITH FRIENDS starts a friends-only session; friends join from the Steam friends list or an invite.
  - PLAY OFFLINE plays without a session; Steam invites still work.
- If Steam is offline (outage, Tuesday maintenance), the session waits and starts once Steam reconnects.

## Requirements

- Islands of Insight on Steam (Windows).
- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) in `IslandsofInsight\Binaries\Win64`. The release zip includes it, with its console and GUI turned off.
- Every player on the same mod version; lobbies are filtered by version.

## Install (players)

1. In Steam, right-click Islands of Insight, then Manage > Browse local files.
2. Open `IslandsofInsight\Binaries\Win64`.
3. Copy the contents of the release zip's `Win64` folder there (`dwmapi.dll`, `UE4SS.dll`, `UE4SS-settings.ini`, `steam_api64.dll`, `Mods\`).
4. Start the game from Steam and pick a mode on the title screen.

### Tip: stop SteamVR from starting

The game ships Unreal's OpenXR plugin, which wakes SteamVR at launch if you have it installed. This isn't caused by the mod. To turn it off, add this to the game's Steam launch options (Properties > General):

```
-nohmd
```

## Build (developers)

The mod builds as a UE4SS C++ mod inside an RE-UE4SS checkout.

1. Clone [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) and set it up per its docs.
2. Put this repo and the Steamworks SDK next to it:
   ```
   UEMods/
     RE-UE4SS/
     IslandsMultiplayerMod/   <- this repo
     steamworks_sdk/          <- Steamworks SDK (public/, redistributable_bin/)
   ```
   Or pass `-DSTEAMWORKS_SDK_DIR=<path>` to CMake.
3. Add this line to `RE-UE4SS/cppmods/CMakeLists.txt`:
   ```cmake
   add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../IslandsMultiplayerMod" "IslandsMultiplayerMod")
   ```
4. From a VS 2022 x64 developer prompt in `RE-UE4SS`:
   ```
   cmake --build build --target IslandsMultiplayerMod
   ```
5. Copy `build\Game__Shipping__Win64\bin\IslandsMultiplayerMod.dll` to `Mods\IslandsMultiplayerMod\dlls\main.dll` in the game, and list `IslandsMultiplayerMod : 1` in `Mods\mods.txt`.

### Packaging

`packaging/package.ps1` builds the player zip (UE4SS, `steam_api64.dll`, the mod and a README) into `dist/`:

```powershell
# Release: UE4SS console and GUI turned off (what players get)
.\packaging\package.ps1 -Ue4ssDir "<game>\IslandsofInsight\Binaries\Win64"

# Dev: console and GUI on, with the Islands MP tab and the local test tools
.\packaging\package.ps1 -Ue4ssDir "<game>\IslandsofInsight\Binaries\Win64" -Dev
```

The version comes from `MOD_VERSION` in `SteamIntegration.cpp`. The release UE4SS settings are in `packaging/UE4SS-settings.ini`.

## How it works

- **Threading:** UE4SS runs `on_update` and ImGui off the game thread, so all UObject work happens in an engine-tick post-hook. The ImGui tab only queues commands and reads a snapshot.
- **Networking:** a Steam lobby with `ISteamNetworkingMessages`. The lobby owner relays packets; every packet carries its origin SteamID.
  - Player state goes out at 10 Hz (unreliable).
  - Appearance goes out on change and every 5 s (reliable).
  - The puzzle cursor goes out at 15 Hz.
- **Puppets:** a hidden animated skeletal mesh drives a poseable mesh (for head look). Body, face, outfit and hair follow it through master pose, and materials are rebuilt from the sender's captured dynamic material parameters.
- **Code layout:**
  - `PlayerSync.cpp`: session and puppet lifecycle.
  - `Appearance.cpp`: capture and apply looks, name cards, markers, puzzle cursor.
  - `SteamIntegration.cpp`: lobbies and transport.
  - `MainMenu.cpp`: title screen buttons.
  - `UEUtil.cpp`: safe handles, cached reflection and calls.

See [TESTING.md](TESTING.md) for the single-PC test tools (loopback clone, stress test).
