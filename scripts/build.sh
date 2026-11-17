#!/bin/bash
# Build on the board and restart the UI service.
# Usage: bash ~/linux-app/build/build.sh [--no-restart]
set -e
cd "$(dirname "$0")/.."

if [ ! -d build ]; then
    echo "== configure =="
    cmake -B build >/dev/null
fi

echo "== build =="
S=$(date +%s)
if ! cmake --build build -j4 --target tb4117_verify > /tmp/build.log 2>&1; then
    echo "BUILD FAILED:"
    grep -E "error|错误" /tmp/build.log | head -15
    exit 1
fi
grep -E "warning|Linking" /tmp/build.log | head -5 || true
E=$(date +%s)
echo "build: $((E-S)) s"

if [ "$1" != "--no-restart" ]; then
    echo "== restart =="
    sudo -n systemctl restart tb4117.gui.service
    sleep 3
    sudo -n journalctl -u tb4117.gui.service -n 6 --no-pager | tail -6
fi
