# Build the chuhAIster native binaries with the Zig toolchain (D:\Tools\zig).
# Usage: .\build.ps1 [tray|core|all]     (default: all; Windows x64, static)
param([string]$Target = "all")
$ErrorActionPreference = "Stop"
$zig = "D:\Tools\zig\zig.exe"

# Icon + version block, linked into both exes (regenerate cswap.ico with
# make_icon.py only when the artwork changes).
$res = Join-Path $PSScriptRoot "chuhaister.res"
Push-Location $PSScriptRoot
& $zig rc /fo $res (Join-Path $PSScriptRoot "chuhaister.rc")
Pop-Location
if ($LASTEXITCODE -ne 0) { exit 1 }

if ($Target -in @("tray", "all")) {
    & $zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode `
        (Join-Path $PSScriptRoot "chuhaister_tray.cpp") -o (Join-Path $PSScriptRoot "chuhaister-tray.exe") `
        $res -lgdi32 -lshell32 -ladvapi32 -lgdiplus -lole32 -lwinhttp -lcrypt32 -lbcrypt -lws2_32 `
        -static "-Wl,--subsystem,windows"
    if ($LASTEXITCODE -ne 0) { exit 1 }
}
if ($Target -in @("core", "all")) {
    & $zig c++ -target x86_64-windows-gnu -std=c++17 -O2 `
        (Join-Path $PSScriptRoot "chuhaister_core.cpp") -o (Join-Path $PSScriptRoot "chuhaister-core.exe") `
        $res -lwinhttp -lcrypt32 -lbcrypt -lws2_32 -static
    if ($LASTEXITCODE -ne 0) { exit 1 }
}
if ($Target -eq "installer") {
    $iscc = "D:\Tools\InnoSetup\ISCC.exe"
    if (-not (Test-Path $iscc)) { Write-Error "Inno Setup not found at $iscc"; exit 1 }
    & $iscc (Join-Path $PSScriptRoot "installer.iss")
    if ($LASTEXITCODE -ne 0) { exit 1 }
    Get-ChildItem (Join-Path $PSScriptRoot "dist") -Filter "*.exe" |
        Select-Object Name, @{n="SizeMB";e={[math]::Round($_.Length/1MB,2)}}, LastWriteTime
    exit 0
}

Get-ChildItem $PSScriptRoot -Filter "chuhaister-*.exe" | Select-Object Name, @{n="SizeMB";e={[math]::Round($_.Length/1MB,2)}}, LastWriteTime
