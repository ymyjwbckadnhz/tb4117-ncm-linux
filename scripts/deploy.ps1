# Push only changed sources to the board, build there, restart the UI service.
#   .\build\deploy.ps1
#   .\build\deploy.ps1 -NoRestart
param(
    [string]$Board = "linaro@10.0.0.47",
    [string]$Key = "$env:USERPROFILE\.ssh\id_ed25519_board",
    [switch]$NoRestart
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$opts = @("-i", $Key, "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=NUL", "-o", "BatchMode=yes")

# remote path -> local path
$files = [ordered]@{
    "/home/linaro/linux-app/src/main.c"          = "$root\src\main.c"
    "/home/linaro/linux-app/src/lv_font_cn_22.c" = "$root\src\lv_font_cn_22.c"
    "/home/linaro/linux-app/CMakeLists.txt"      = "$root\CMakeLists.txt"
    "/home/linaro/linux-app/lv_conf.h"           = "$root\lv_conf.h"
    "/home/linaro/linux-app/scripts/build.sh"      = "$root\scripts\build.sh"
}

$remoteSums = @{}
$paths = ($files.Keys | ForEach-Object { "`"$_`"" }) -join " "
$out = & ssh @opts $Board "md5sum $paths 2>/dev/null" 2>$null
foreach ($line in $out) {
    if ($line -match '^([0-9a-f]{32})\s+(.+)$') {
        $remoteSums[$matches[2].Trim()] = $matches[1]
    }
}

Write-Host "== prepare remote dirs =="
& ssh @opts $Board "mkdir -p ~/linux-app/src ~/linux-app/scripts" 2>$null

Write-Host "== upload (only changed) =="
$uploaded = 0
foreach ($remote in $files.Keys) {
    $local = $files[$remote]
    if (-not (Test-Path $local)) { continue }
    $localSum = (Get-FileHash -Algorithm MD5 $local).Hash.ToLower()
    if ($remoteSums[$remote] -eq $localSum) {
        Write-Host "   same: $(Split-Path -Leaf $remote)"
        continue
    }
    Write-Host "   push: $(Split-Path -Leaf $remote)"
    & scp @opts $local "$($Board):$remote" 2>$null
    $uploaded++
}

if ($uploaded -eq 0) { Write-Host "   (nothing changed)" }

$arg = if ($NoRestart) { "--no-restart" } else { "" }
Write-Host "== build on board =="
& ssh @opts $Board "bash ~/linux-app/scripts/build.sh $arg" 2>$null
