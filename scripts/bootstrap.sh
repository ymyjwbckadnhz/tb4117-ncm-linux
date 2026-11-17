#!/bin/bash
# Bootstrap the board from a PC: push the source tree and run setup_device.sh.
#
#   BOARD=linaro@192.168.1.50 bash scripts/bootstrap.sh
#   BOARD=linaro@10.0.0.47 KEY=~/.ssh/id_ed25519_board bash scripts/bootstrap.sh
set -e

BOARD=${BOARD:?用法: BOARD=linaro@<板子IP> bash scripts/bootstrap.sh}
KEY=${KEY:-}
REMOTE_DIR=${REMOTE_DIR:-linux-app}

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes"
[ -n "$KEY" ] && SSH_OPTS="-i $KEY $SSH_OPTS"

echo "== 检查连接 =="
if ! ssh $SSH_OPTS "$BOARD" "uname -m" 2>/dev/null; then
    echo "连不上 $BOARD" >&2
    echo "  1) 确认板子已联网、IP 正确" >&2
    echo "  2) 官方镜像首次开机 sshd 起不来，需先用串口执行：" >&2
    echo "     ssh-keygen -A && systemctl restart ssh" >&2
    exit 1
fi

echo "== 上传源码 =="
ssh $SSH_OPTS "$BOARD" "mkdir -p ~/$REMOTE_DIR/{src,scripts,tools,docs}"
scp $SSH_OPTS src/main.c src/lv_font_cn_22.c "$BOARD:~/$REMOTE_DIR/src/" >/dev/null
scp $SSH_OPTS CMakeLists.txt lv_conf.h LICENSE README.md "$BOARD:~/$REMOTE_DIR/" >/dev/null
scp $SSH_OPTS scripts/* "$BOARD:~/$REMOTE_DIR/scripts/" >/dev/null
scp $SSH_OPTS tools/* "$BOARD:~/$REMOTE_DIR/tools/" >/dev/null
scp $SSH_OPTS docs/*.md "$BOARD:~/$REMOTE_DIR/docs/" >/dev/null
echo "   完成"

if [ "$SKIP_SETUP" = "1" ]; then
    echo "源码已上传（跳过 setup）"
    exit 0
fi

echo "== 在板子上执行 setup_device.sh =="
ssh $SSH_OPTS -t "$BOARD" "cd ~/$REMOTE_DIR && bash scripts/setup_device.sh"

echo
echo "完成。屏幕应显示热像画面。"
