#!/bin/bash
echo "== running display services =="
systemctl list-units --type=service --state=running 2>/dev/null | grep -i -e lightdm -e gdm -e sddm -e x11 -e nodm -e xinit
echo "== xorg cmdline =="
tr '\0' ' ' < /proc/1812/cmdline 2>/dev/null; echo
echo "== default display manager =="
cat /etc/X11/default-display-manager 2>/dev/null
echo "== logged in =="
who 2>/dev/null
echo "== xorg conf =="
ls /etc/X11/xorg.conf.d/ 2>/dev/null
