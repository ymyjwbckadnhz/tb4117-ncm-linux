#!/bin/bash
# One-shot setup: turn a stock LCKFB Debian image into this device.
#
#   git clone <repo> ~/linux-app && cd ~/linux-app
#   bash scripts/setup_device.sh
#
# The script is idempotent: re-running it just rebuilds and restarts.
set -e

cd "$(dirname "$0")/.."
ROOT=$(pwd)
BOARD_USER=$(id -un)

say() { printf '\n\033[1;36m== %s ==\033[0m\n' "$*"; }

# ---------------------------------------------------------------- sanity
say "检查环境"
ARCH=$(uname -m)
[ "$ARCH" = "aarch64" ] || { echo "需要在泰山派 aarch64 上运行（当前 $ARCH）"; exit 1; }
[ -f /etc/debian_version ] || echo "警告：不是 Debian，依赖名可能不同"
echo "架构 $ARCH  内核 $(uname -r)"
echo "用户 $BOARD_USER  目录 $ROOT"

# ---------------------------------------------------------------- deps
say "安装依赖（需要联网）"
sudo apt-get update -qq
PKGS="build-essential cmake pkg-config libfreetype-dev \
      libcurl4-openssl-dev libjpeg62-turbo-dev libdrm-dev \
      libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
      i2c-tools network-manager fonts-wqy-zenhei"

# the stock image ships a held libdrm2; retry allowing held packages to change
sudo apt-get install -y --no-install-recommends $PKGS \
    || sudo apt-get install -y --allow-change-held-packages --no-install-recommends $PKGS

# ---------------------------------------------------------------- LVGL
say "准备 LVGL"
[ -d third_party/lvgl/src ] || bash scripts/fetch_lvgl.sh

# ---------------------------------------------------------------- build
say "编译"
cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" --target tb4117_verify

# ---------------------------------------------------------------- camera link
say "配置机芯网卡（usb0，192.168.64.32/24）"
echo 'SUBSYSTEM=="net", ACTION=="add", DRIVERS=="cdc_ncm", NAME="usb0"' \
    | sudo tee /etc/udev/rules.d/70-4117-ncm.rules >/dev/null
sudo udevadm control --reload-rules

if ! nmcli -t -f NAME connection show | grep -qx 4117; then
    sudo nmcli con add type ethernet ifname usb0 con-name 4117 \
        ipv4.method manual ipv4.addresses 192.168.64.32/24 \
        ipv4.never-default yes ipv6.method disabled || true
fi

# ---------------------------------------------------------------- service
say "安装开机自启服务"
bash tools/install_service.sh

say "完成"
cat <<EOF

服务已启动，屏幕应显示热像画面。常用命令：

  sudo systemctl status tb4117.gui.service
  sudo journalctl -u tb4117.gui.service -f
  bash scripts/build.sh          # 改代码后重新编译 + 重启

网页：http://\$(hostname -I | awk '{print \$1}'):8080/
机芯配置页：http://\$(hostname -I | awk '{print \$1}'):8081/

如果屏幕黑屏：确认已关闭 lightdm（本脚本已处理），并且机芯已配成 NCM 模式。
EOF
