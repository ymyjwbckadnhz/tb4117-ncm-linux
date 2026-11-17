#!/bin/bash
sudo -n systemctl stop lightdm 2>/dev/null
sleep 2
sudo -n pkill -x tb4117_verify 2>/dev/null
sleep 1
sudo -n bash -c 'cd /home/linaro/linux-app && setsid ./build/tb4117_verify > /tmp/verify.log 2>&1 < /dev/null &'
sleep 10
echo "pid: $(pgrep -x tb4117_verify | head -1)"
echo "--- log ---"
sudo -n tail -3 /tmp/verify.log
echo "--- dump ---"
sudo -n /home/linaro/linux-app/tools/dump_fb /dev/dri/card0 /tmp/fb.bmp
sudo -n ls -l /tmp/fb.bmp 2>/dev/null
