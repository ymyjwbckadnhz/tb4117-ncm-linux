# Bootstrap the board from a PC: push the source tree and run setup_device.sh.
# Nothing has to be typed on the 3.1" screen.
#
#   .\scripts\bootstrap.ps1 -Board linaro@192.168.1.50
#   .\scripts\bootstrap.ps1 -Board linaro@10.0.0.47 -Key ~/.ssh/id_ed25519_board
param(
    [Parameter(Mandatory = $true)][string]$Board,
    [string]$Key = "",
    [string]$RemoteDir = "linux-app",
    [switch]$SkipSetup
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot

$opts = @("-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=NUL", "-o", "BatchMode=yes")
if ($Key -ne "") {
    $opts = @("-i", $Key) + $opts
}

Write-Host "== 检查连接 ==" -ForegroundColor Cyan
& ssh @opts $Board "uname -m; uname -r" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "连不上 $Board。请先确认：" -ForegroundColor Red
    Write-Host "  1) 板子已联网，IP 正确"
    Write-Host "  2) 官方镜像首次开机要先修 sshd： ssh-keygen -A && systemctl restart ssh"
    Write-Host "     （用串口 1500000 波特率登录执行）"
    exit 1
}

Write-Host "== 创建远端目录 ==" -ForegroundColor Cyan
& ssh @opts $Board "mkdir -p ~/$RemoteDir/{src,scripts,tools,docs}" 2>$null

Write-Host "== 上传源码 ==" -ForegroundColor Cyan
$files = @(
    @{ Local = "src\main.c";           Remote = "src/main.c" },
    @{ Local = "src\lv_font_cn_22.c";  Remote = "src/lv_font_cn_22.c" },
    @{ Local = "CMakeLists.txt";       Remote = "CMakeLists.txt" },
    @{ Local = "lv_conf.h";            Remote = "lv_conf.h" },
    @{ Local = "LICENSE";              Remote = "LICENSE" },
    @{ Local = "README.md";            Remote = "README.md" }
)
foreach ($f in $files) {
    $p = Join-Path $root $f.Local
    if (Test-Path $p) {
        & scp @opts $p "$($Board):~/$RemoteDir/$($f.Remote)" 2>$null
        Write-Host "   $($f.Remote)"
    }
}

foreach ($sub in @("scripts", "tools")) {
    Get-ChildItem (Join-Path $root $sub) -File | ForEach-Object {
        & scp @opts $_.FullName "$($Board):~/$RemoteDir/$sub/$($_.Name)" 2>$null
        Write-Host "   $sub/$($_.Name)"
    }
}
Get-ChildItem (Join-Path $root "docs") -File | ForEach-Object {
    & scp @opts $_.FullName "$($Board):~/$RemoteDir/docs/$($_.Name)" 2>$null
}

if ($SkipSetup) {
    Write-Host "源码已上传（跳过 setup）" -ForegroundColor Green
    exit 0
}

Write-Host "== 在板子上执行 setup_device.sh ==" -ForegroundColor Cyan
& ssh @opts $Board "cd ~/$RemoteDir && bash scripts/setup_device.sh" 2>$null

Write-Host "`n完成。屏幕应显示热像画面。" -ForegroundColor Green
& ssh @opts $Board "ip -4 -o addr show usb0 | awk '{print `"usb0: `"`$4}'; systemctl is-active tb4117.gui.service" 2>$null
