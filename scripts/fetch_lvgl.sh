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

# GitHub first, Gitee mirror as a fallback for networks that cannot reach GitHub
URLS="
https://github.com/lvgl/lvgl/archive/refs/tags/v${VER}.tar.gz
https://gitee.com/mirrors/lvgl/repository/archive/v${VER}.tar.gz
"

ok=0
for u in $URLS; do
    echo "trying $u"
    if curl -fL --connect-timeout 15 -o /tmp/lvgl.tar.gz "$u"; then
        ok=1
        break
    fi
done

if [ "$ok" != 1 ]; then
    echo "download failed; put the LVGL ${VER} source tree at third_party/lvgl manually" >&2
    exit 1
fi

rm -rf /tmp/lvgl-src third_party/lvgl
mkdir -p /tmp/lvgl-src
tar -xzf /tmp/lvgl.tar.gz -C /tmp/lvgl-src
# the archive contains exactly one top-level directory
mv /tmp/lvgl-src/* third_party/lvgl
rm -rf /tmp/lvgl-src /tmp/lvgl.tar.gz

echo "LVGL ${VER} -> third_party/lvgl"
