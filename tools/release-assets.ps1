# Release files: the self-contained launcher exe and the package zip, built from a working package
# folder (engine installed, game files not included).
#   tools\release-assets.ps1 -Version 0.9.13 -Engine bbhost-0.4.0-fps23 -Package D:\BloodbornePC -Dist D:\dist
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Engine,
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Dist,
    [string]$Project = (Join-Path $PSScriptRoot '..\launcher\windows\BloodborneLauncher\BloodborneLauncher.csproj'))
$ErrorActionPreference = 'Stop'
$dist = $Dist
$pkg = $Package
$csproj = $Project

# 1. The launcher on its own, self-contained (players need no .NET); the updater downloads this name.
$exeDir = "$dist\launcher"
if (Test-Path $exeDir) { Remove-Item -LiteralPath $exeDir -Recurse -Force -Confirm:$false }
dotnet publish $csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true `
    -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true -p:DebugType=none -p:Version=$Version `
    -o $exeDir --nologo -v q | Out-Null
$exe = "$exeDir\Bloodborne Launcher.exe"
"launcher: {0} ({1:N1} MB)" -f (Get-Item $exe).VersionInfo.FileVersion, ((Get-Item $exe).Length / 1MB)

# 2. The whole package, without the developer's files.
$stage = "$dist\stage-$Version\BloodbornePC"
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -Confirm:$false }
robocopy $pkg $stage /E /XF 'Build Launcher.exe' 'Build Launcher.exe.old' 'Force Rebuild Launcher.cmd' build-stamp.json bbport.ini.bak `
    /XD user logs mods network engine-assets __pycache__ /NFL /NDL /NJH /NJS /NP | Out-Null
# out\ is also where every game start writes files generated from the player's dump
# (boot*.bin, eboot.elf, *.json): only the engine's programs and DLLs ship.
Get-ChildItem "$stage\out" -File | Where-Object { $_.Extension -notin '.exe', '.dll', '.ico' } | Remove-Item -Force -Confirm:$false
Get-ChildItem "$stage\out" -Directory | Remove-Item -Recurse -Force -Confirm:$false
Get-ChildItem $stage -Recurse -File -Include *.orig, *.bak, *.old | Remove-Item -Force -Confirm:$false
Copy-Item $exe "$stage\Bloodborne Launcher.exe" -Force
# RecompLauncher reads this to set the game up and start it (the same file as in the Linux package).
Copy-Item (Join-Path $PSScriptRoot '..\recomplauncher.json') "$stage\recomplauncher.json" -Force
# Settings as shipped: the tested defaults, without this PC's menu position.
(Get-Content "$stage\bbport.ini") | Where-Object { $_ -notmatch '^menu_pos=' } | Set-Content -Encoding ascii "$stage\bbport.ini"
# New downloads start with the EldenRingStyle layout (the launcher's built-in profile).
$elden = [ordered]@{
    move_forward = 'W'; move_back = 'S'; move_left = 'A'; move_right = 'D'; walk = 'Left Alt'; roll = 'Space'; l3 = 'Left Ctrl'
    attack = 'Mouse1'; strong_attack = 'Mouse4'; strong_modifier = 'Left Shift'; transform = 'C, Mouse5'; firearm = 'X, Mouse3'
    lock_on = 'Q, Mouse2'; blood_vial = 'R'; use_item = 'F'; interact = 'E'; switch_item = 'Down'; switch_right = 'Right'
    switch_left = 'Left'; dpad_up = 'Up'; gestures = 'G'; personal_effects = 'T'; menu = 'Escape'; confirm = 'Return'
    back = 'Backspace'; look_up = 'I'; look_down = 'K'; look_left = 'J'; look_right = 'L'
}
$lines = @(Get-Content "$stage\bbport.ini" | Where-Object { $_ -notmatch '^bind\.' })
$lines += $elden.Keys | ForEach-Object { "bind.$_=$($elden[$_])" }
$lines | Set-Content -Encoding ascii "$stage\bbport.ini"

# Which engine the package carries: the launcher's updater compares it to decide between
# replacing only itself or the whole package.
$package = "{ `"version`": `"$Version`", `"engine`": `"$Engine`" }"
Set-Content -Encoding ascii "$stage\package.json" $package
Set-Content -Encoding ascii "$dist\package.json" $package

$zip = "$dist\BloodbornePC-v$Version-win64.zip"
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force -Confirm:$false }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
$check = [IO.Compression.ZipFile]::OpenRead($zip)
$bad = @($check.Entries | Where-Object { $_.FullName -match '[\\/]out[\\/].+\.(bin|elf|json|txt)$|eboot|[\\/]user[\\/]|\.(orig|bak|old)$|build-stamp|Build Launcher' })
$check.Dispose()
if ($bad.Count) { Remove-Item -LiteralPath $zip -Force -Confirm:$false; throw "Refusing to ship: $($bad.Count) game-derived or dev files, e.g. $($bad[0].FullName)" }
"package: {0} ({1:N0} MB)" -f (Split-Path $zip -Leaf), ((Get-Item $zip).Length / 1MB)
