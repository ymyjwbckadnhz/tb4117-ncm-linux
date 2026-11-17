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

# ---------------------------------------------------------------- ssh
if [ -x /usr/sbin/sshd ] && [ ! -f /etc/ssh/ssh_host_ed25519_key ]; then
    say "生成 SSH host key（官方镜像首次启动缺这个，sshd 会起不来）"
    sudo ssh-keygen -A
    sudo systemctl restart ssh || true
fi

# ---------------------------------------------------------------- network
say "检查网络"
online=0
for u in https://mirrors.aliyun.com https://deb.debian.org https://mirrors.tuna.tsinghua.edu.cn; do
    if curl -fsS -m 8 -o /dev/null "$u" 2>/dev/null; then online=1; break; fi
done

if [ "$online" != 1 ]; then
    cat <<'EOF'

未检测到外网，apt 装不了依赖。先在板子上连 WiFi：

  nmcli device wifi list
  nmcli device wifi connect "<SSID>" password "<密码>"
  ip -4 addr show wlan1          # 确认拿到 IP

没有键盘时怎么办：
  · 插一个 USB 键盘到板子上，用 3.1 寸屏的桌面打开终端（字很小，但能敲）
  · 或者用串口（1500000 波特率），从电脑连上去执行上面的命令
  · 或者在电脑上先 ssh，再跑 scripts/bootstrap.ps1

连上网后重新执行本脚本。
EOF
    exit 1
fi
echo "网络正常"

# ---------------------------------------------------------------- deps
say "安装依赖"
sudo apt-get update -qq
PKGS="build-essential cmake pkg-config libfreetype-dev \
      libcurl4-openssl-dev libjpeg62-turbo-dev libdrm-dev \
      libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
      i2c-tools network-manager fonts-wqy-zenhei"

# --no-upgrade: never touch packages that are already installed. The stock image
# pins libdrm and ships a patched network-manager; upgrading either can drop the
# WiFi link mid-deploy.
install_pkgs() {
    sudo apt-get install -y --no-upgrade --no-install-recommends $PKGS && return 0
    echo "常规安装失败，解除 libdrm 的 hold 再试 ..."
    sudo apt-mark unhold libdrm2 libdrm-dev 2>/dev/null || true
    sudo apt-get install -y --no-upgrade --no-install-recommends $PKGS && return 0
    echo "仍失败，允许改动 held 包 ..."
    sudo apt-get install -y --no-upgrade --allow-change-held-packages --no-install-recommends $PKGS
}

if ! install_pkgs; then
    echo "依赖安装失败：请手动执行 sudo apt-get install $PKGS" >&2
    exit 1
fi

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

# the stock image leaves usb0 unmanaged/unavailable until it is told otherwise
sudo nmcli device set usb0 managed yes 2>/dev/null || true
sudo nmcli con mod 4117 connection.autoconnect yes 2>/dev/null || true
sudo nmcli con up 4117 2>/dev/null || true

sleep 2
if ip -4 addr show usb0 2>/dev/null | grep -q 192.168.64.32; then
    echo "机芯链路就绪：$(ip -4 -o addr show usb0 | awk '{print $4}')"
else
    echo "警告：usb0 没有拿到 192.168.64.32，请确认机芯已切到 NCM 并上电"
fi

# ---------------------------------------------------------------- boot time
# The stock image ships /etc/network/interfaces.d/network-priority with
#   auto eth0 / auto wlan0 / auto ec20
# None of those exist on this board (the camera link is usb0, managed by
# NetworkManager) so every boot runs ifupdown's DHCP and waits for timeouts,
# which is a big part of the slow cold boot. Keep a copy OUTSIDE interfaces.d -
# a ".orig" file in that directory is still sourced by "source !/etc/network/interfaces.d/*"!
say "关掉 ifupdown 里不存在接口的自动启动"
VENDOR=/etc/network/interfaces.d/network-priority
if [ -f "$VENDOR" ] && grep -q '^auto ' "$VENDOR"; then
    sudo cp "$VENDOR" /root/network-priority.orig
    sudo sed -i 's/^auto eth0/#auto eth0/; s/^auto wlan0/#auto wlan0/; s/^auto ec20/#auto ec20/' "$VENDOR"
    echo "已停用 eth0/wlan0/ec20 的 auto（备份 /root/network-priority.orig）"
else
    echo "无需修改"
fi
# ---------------------------------------------------------------- credentials
say "机芯凭据"
CONF="$ROOT/device.conf"
if [ -f "$CONF" ]; then
    chmod 600 "$CONF"
    echo "已存在 $CONF（保持不动）"
elif [ -t 0 ]; then
    echo "请输入机芯的登录信息（写入 $CONF，权限 600，不会进仓库）"
    read -r -p "  机芯 IP [192.168.64.64]: " CAM_IP
    [ -z "$CAM_IP" ] && CAM_IP=192.168.64.64
    read -r -s -p "  机芯密码（admin 的密码）: " CAM_PW
    echo
    printf 'camera=%s\nauth=admin:%s\n' "$CAM_IP" "$CAM_PW" > "$CONF"
    chmod 600 "$CONF"
    echo "已写入 $CONF"
else
    printf 'camera=192.168.64.64\nauth=admin:CHANGE_ME\n' > "$CONF"
    chmod 600 "$CONF"
    echo "已生成模板 $CONF —— 请把 auth 改成 admin:<机芯密码>"
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
