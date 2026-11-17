#!/bin/bash
set -e

# 1) app as a systemd service (auto start + restart on crash)
sudo -n tee /etc/systemd/system/tb4117.gui.service >/dev/null <<'UNIT'
[Unit]
Description=TB-4117 thermal camera UI (LVGL)
# Do NOT wait for multi-user.target: that pulls in networking.service, which can
# sit in a DHCP timeout for tens of seconds when the camera link is not up yet.
# The UI only needs local DRM + input, so start as early as possible.
After=network.target
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

# 2) free the panel: no X, boot to console
sudo -n systemctl disable lightdm 2>/dev/null || true
sudo -n systemctl stop lightdm 2>/dev/null || true
sudo -n systemctl set-default multi-user.target

# 3) enable & start
sudo -n systemctl daemon-reload
sudo -n pkill -x tb4117_verify 2>/dev/null || true
sleep 1
sudo -n systemctl enable --now tb4117.gui.service

sleep 6
echo "== service =="
systemctl is-enabled tb4117.gui.service
systemctl is-active tb4117.gui.service
echo "== process =="
pgrep -x tb4117_verify | head -1
echo "== default target =="
systemctl get-default
echo "== log =="
sudo -n journalctl -u tb4117.gui.service -n 5 --no-pager | tail -5
