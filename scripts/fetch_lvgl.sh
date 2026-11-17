#!/bin/bash
# Fetch LVGL into third_party/lvgl (that directory is not in the repo).
set -e
cd "$(dirname "$0")/.."

VER=9.2.2
if [ -d third_party/lvgl/src ]; then
    echo "third_party/lvgl already present"
    exit 0
fi

mkdir -p third_party
echo "downloading LVGL ${VER} ..."
curl -fL -o /tmp/lvgl.tar.gz \
    "https://github.com/lvgl/lvgl/archive/refs/tags/v${VER}.tar.gz"
tar -xzf /tmp/lvgl.tar.gz -C /tmp
rm -rf third_party/lvgl
mv "/tmp/lvgl-${VER}" third_party/lvgl
rm -f /tmp/lvgl.tar.gz
echo "LVGL ${VER} -> third_party/lvgl"
