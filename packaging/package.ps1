<#
Builds a player zip: UE4SS + the mod, ready to drop into IslandsofInsight\Binaries\Win64.

Release packages (default) have the UE4SS console and GUI turned off. -Dev keeps them on,
for the Islands MP tab and the local test tools.

  .\package.ps1 -Ue4ssDir <folder with UE4SS.dll and dwmapi.dll> [-ModDll <path>] [-Dev] [-OutDir <folder>]

-Ue4ssDir     A UE4SS install to take UE4SS.dll and dwmapi.dll from (e.g. the game's Win64
              folder, or an RE-UE4SS build output).
-SteamApiDll  steam_api64.dll to ship (defaults to the Steamworks SDK next to this repo).
-ModDll       The built mod (defaults to the RE-UE4SS build output next to this repo).
#>
param(
    [Parameter(Mandatory = $true)][string]$Ue4ssDir,
    [string]$ModDll = "$PSScriptRoot\..\..\RE-UE4SS\build\Game__Shipping__Win64\bin\IslandsMultiplayerMod.dll",
    [string]$SteamApiDll = "$PSScriptRoot\..\..\steamworks_sdk\redistributable_bin\win64\steam_api64.dll",
    [string]$OutDir = "$PSScriptRoot\..\dist",
    [switch]$Dev
)
$ErrorActionPreference = 'Stop'

$src = Get-Content "$PSScriptRoot\..\SteamIntegration.cpp" -Raw
if ($src -notmatch 'MOD_VERSION = "([^"]+)"') { throw "MOD_VERSION not found in SteamIntegration.cpp" }
$version = $Matches[1]
$name = if ($Dev) { "IslandsMP-$version-dev" } else { "IslandsMP-$version" }

$stage = Join-Path $OutDir $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
# Same layout as the game folder, so the zip is extracted straight into the game's root
# folder (the one with IslandsofInsight.exe) and everything lands in the right place.
$null = New-Item -ItemType Directory -Force (Join-Path $stage "IslandsofInsight\Binaries\Win64\Mods\IslandsMultiplayerMod\dlls")
$win64 = Join-Path $stage "IslandsofInsight\Binaries\Win64"

foreach ($f in 'UE4SS.dll', 'dwmapi.dll') { Copy-Item (Join-Path $Ue4ssDir $f) $win64 }
Copy-Item $SteamApiDll $win64
Copy-Item $ModDll (Join-Path $win64 "Mods\IslandsMultiplayerMod\dlls\main.dll")
Set-Content (Join-Path $win64 "Mods\mods.txt") "IslandsMultiplayerMod : 1`r`n" -NoNewline

$ini = Get-Content "$PSScriptRoot\UE4SS-settings.ini" -Raw
if ($Dev)
{
    $ini = $ini -replace '(?m)^ConsoleEnabled = 0', 'ConsoleEnabled = 1' `
                -replace '(?m)^GuiConsoleEnabled = 0', 'GuiConsoleEnabled = 1' `
                -replace '(?m)^GuiConsoleVisible = 0', 'GuiConsoleVisible = 1'
}
Set-Content (Join-Path $win64 "UE4SS-settings.ini") $ini -NoNewline
(Get-Content "$PSScriptRoot\README.txt" -Raw).Replace('{VERSION}', $version) | Set-Content (Join-Path $stage "README.txt") -NoNewline

$zip = Join-Path $OutDir "$name.zip"
Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
Write-Host "Packaged $zip"
