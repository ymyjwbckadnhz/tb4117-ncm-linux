#!/bin/bash
# Build a release tarball from an existing build (run on the board).
#
#   bash scripts/package.sh            -> dist/tb4117-ncm-linux-<ver>-aarch64.tar.gz
#
# The tarball contains the compiled binary plus a self-contained install.sh,
# so users on the official Debian image do not need a compiler at all.
set -e

cd "$(dirname "$0")/.."
ROOT=$(pwd)
VER=${VER:-$(git describe --tags --always 2>/dev/null || echo dev)}
NAME="tb4117-ncm-linux-${VER}-aarch64"
OUT="$ROOT/dist"
STAGE="$OUT/$NAME"

[ -x build/tb4117_verify ] || { echo "先编译：bash scripts/build.sh" >&2; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE"

echo "== 收集文件 =="
install -m 755 build/tb4117_verify "$STAGE/"
install -m 644 tools/../README.md "$STAGE/" 2>/dev/null || true
install -m 644 LICENSE "$STAGE/" 2>/dev/null || true

cat > "$STAGE/tb4117.gui.service" <<'UNIT'
[Unit]
Description=TB-4117 thermal camera UI (LVGL)
After=multi-user.target network.target
Wants=network.target

[Service]
Type=simple
ExecStart=/home/linaro/linux-app/build/tb4117_verify
Restart=always
RestartSec=2
User=root

[Install]
WantedBy=multi-user.target
UNIT

cat > "$STAGE/70-4117-ncm.rules" <<'RULE'
SUBSYSTEM=="net", ACTION=="add", DRIVERS=="cdc_ncm", NAME="usb0"
RULE

cat > "$STAGE/install.sh" <<'INSTALL'
#!/bin/bash
# Install the prebuilt TB-4117 handheld UI on a stock Debian 12 image.
set -e
[ "$(uname -m)" = "aarch64" ] || { echo "需要 aarch64（泰山派）"; exit 1; }

DEST=${DEST:-/home/linaro/linux-app}
HERE=$(cd "$(dirname "$0")" && pwd)

echo "== 安装运行库（不升级已装包） =="
sudo apt-get install -y --no-upgrade --no-install-recommends \
    libcurl4 libjpeg62-turbo libfreetype6 libdrm2 libdrm-tests \
    libavformat59 libavcodec59 libavutil57 libswscale6 \
    i2c-tools network-manager fonts-wqy-zenhei 2>/dev/null || \
sudo apt-get install -y --no-upgrade --no-install-recommends \
    libcurl4 libjpeg62-turbo libfreetype6 libdrm2 \
    libavformat59 libavcodec59 libavutil57 libswscale6 \
    i2c-tools network-manager fonts-wqy-zenhei

echo "== 安装程序 =="
install -d "$DEST/build"
install -m 755 "$HERE/tb4117_verify" "$DEST/build/tb4117_verify"

echo "== 机芯凭据 =="
CONF="$DEST/device.conf"
if [ ! -f "$CONF" ]; then
    if [ -t 0 ]; then
        read -r -p "  机芯 IP [192.168.64.64]: " CAM_IP; [ -z "$CAM_IP" ] && CAM_IP=192.168.64.64
        read -r -s -p "  机芯密码（admin 的密码）: " CAM_PW; echo
        printf 'camera=%s\nauth=admin:%s\n' "$CAM_IP" "$CAM_PW" > "$CONF"
    else
        printf 'camera=192.168.64.64\nauth=admin:CHANGE_ME\n' > "$CONF"
        echo "  已生成模板 $CONF，请改成真实密码"
    fi
fi
chmod 600 "$CONF"

echo "== 网络 / 服务 =="
sudo install -m 644 "$HERE/70-4117-ncm.rules" /etc/udev/rules.d/70-4117-ncm.rules
sudo udevadm control --reload-rules
if ! nmcli -t -f NAME connection show | grep -qx 4117; then
    sudo nmcli con add type ethernet ifname usb0 con-name 4117 \
        ipv4.method manual ipv4.addresses 192.168.64.32/24 \
        ipv4.never-default yes ipv6.method disabled || true
fi
sudo nmcli device set usb0 managed yes 2>/dev/null || true
sudo nmcli con mod 4117 connection.autoconnect yes 2>/dev/null || true
sudo nmcli con up 4117 2>/dev/null || true

sudo install -m 644 "$HERE/tb4117.gui.service" /etc/systemd/system/tb4117.gui.service
sudo systemctl disable lightdm 2>/dev/null || true
sudo systemctl set-default multi-user.target
sudo systemctl daemon-reload
sudo systemctl enable --now tb4117.gui.service

sleep 4
echo
echo "完成。systemctl status tb4117.gui.service"
echo "网页 http://<板子IP>:8080/  （板子 IP：$(hostname -I)）"
echo "机芯配置 http://<板子IP>:8081/"
INSTALL

chmod +x "$STAGE/install.sh"

cat > "$STAGE/README.txt" <<'TXT'
TB-4117 手持热像仪（泰山派 RK3566）

系统镜像：立创官方 tspi-1m-kernel6.1-debian-3.1inch-mipi-*.img
          https://wiki.lckfb.com/zh-hans/tspi-rk3566/download-center.html

安装（板子上执行）：

    tar xzf tb4117-ncm-linux-*-aarch64.tar.gz
    cd tb4117-ncm-linux-*-aarch64
    bash install.sh

没有键盘时：插 USB 键盘用 3.1 寸屏操作，或用串口（1500000 波特率），
或者从电脑 ssh 过去执行。

源码与说明：https://github.com/ymyjwbckadnhz/tb4117-ncm-linux
TXT

echo "== 打包 =="
cd "$OUT"
tar czf "$NAME.tar.gz" "$NAME"
sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256"
rm -rf "$STAGE"

ls -lh "$OUT"
echo
echo "产物：$OUT/$NAME.tar.gz"
