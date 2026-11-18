#!/bin/bash
# Reboot the module through ISAPI.
#
# Use this when the module is in the half-reset state: it answers on ping and
# HTTP but its RTSP server never starts (typically after a power cut that was
# too short for the module to fully discharge).
#
#   bash tools/module_reboot.sh
set -e

CONF=${CONF:-/home/linaro/linux-app/device.conf}
IP=192.168.64.64
AUTH=admin:admin
if [ -r "$CONF" ]; then
    v=$(sed -n 's/^camera=//p' "$CONF" | head -1); [ -n "$v" ] && IP=$v
    v=$(sed -n 's/^auth=//p'   "$CONF" | head -1); [ -n "$v" ] && AUTH=$v
fi

echo "机芯 $IP"
echo -n "  554 端口: "
timeout 3 bash -c "exec 3<>/dev/tcp/$IP/554" 2>/dev/null && echo open || echo "refused（半复位状态）"

code=$(curl -s -m 12 -o /dev/null -w '%{http_code}' --digest -u "$AUTH" \
            -X PUT "http://$IP/ISAPI/System/reboot" 2>/dev/null || echo 000)
echo "  reboot -> HTTP $code"

for i in $(seq 1 18); do
    sleep 10
    if timeout 3 bash -c "exec 3<>/dev/tcp/$IP/554" 2>/dev/null; then
        echo "  第 $((i*10))s：RTSP 已就绪"
        exit 0
    fi
done
echo "  3 分钟内 RTSP 仍未就绪——建议断电 15 秒以上再上电" >&2
exit 1
