#!/bin/bash
# Install the module probe (persistent log across reboots).
set -e

DIR=$(cd "$(dirname "$0")/.." && pwd)

sudo install -m 755 "$DIR/tools/tb4117-probe.sh" /usr/local/bin/tb4117-probe.sh
sudo mkdir -p /var/log/tb4117

sudo tee /etc/systemd/system/tb4117-probe.service >/dev/null <<'UNIT'
[Unit]
Description=TB-4117 module watchdog log
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/tb4117-probe.sh
KillSignal=SIGTERM
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable tb4117-probe.service
sudo systemctl restart tb4117-probe.service
sleep 3

echo "== 状态 =="
systemctl is-active tb4117-probe.service
echo "== 日志开头 =="
sudo tail -12 /var/log/tb4117/probe.log
