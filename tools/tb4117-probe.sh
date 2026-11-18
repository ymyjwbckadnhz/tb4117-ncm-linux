#!/bin/bash
# Persistent watchdog log for the TB-4117 module.
#
# Records, across reboots and power cuts, when the module answers on
# ping / HTTP / RTSP. Installed as tb4117-probe.service so it starts at boot.
#
#   /var/log/tb4117/probe.log       current log (rotated to .1 at 1 MB)
#
# Nothing here talks to the module faster than once every 2 s, and the
# authenticated sample (its uptime) is taken once a minute.

LOG_DIR=/var/log/tb4117
LOG=$LOG_DIR/probe.log
CONF=/home/linaro/linux-app/device.conf

mkdir -p "$LOG_DIR"

# ---------------------------------------------------------------- config
IP=192.168.64.64
if [ -r "$CONF" ]; then
    v=$(sed -n 's/^camera=//p' "$CONF" | head -1)
    [ -n "$v" ] && IP=$v
fi

# ---------------------------------------------------------------- helpers
boot_id() { tr -d '-' < /proc/sys/kernel/random/boot_id; }
BOOT=$(boot_id)
uptime_s() { cut -d. -f1 /proc/uptime; }

rotate() {
    if [ -f "$LOG" ] && [ "$(stat -c%s "$LOG" 2>/dev/null || echo 0)" -gt 1048576 ]; then
        mv -f "$LOG" "$LOG.1"
    fi
}

say() {
    rotate
    printf '%s up=%ss boot=%s %s\n' \
        "$(date '+%F %T')" "$(uptime_s)" "${BOOT:0:8}" "$*" >> "$LOG"
}

probe_ping() { ping -c1 -W1 "$IP" >/dev/null 2>&1 && echo up || echo down; }
probe_tcp()  { timeout 2 bash -c "exec 3<>/dev/tcp/$IP/$1" 2>/dev/null && echo up || echo down; }

module_clock() {   # the module has no RTC: this is effectively its uptime
    timeout 3 curl -s -D - -o /dev/null "http://$IP/" 2>/dev/null \
        | tr -d '\r' | sed -n 's/^[Dd]ate: //p' | head -1
}

# ---------------------------------------------------------------- start-up
say "=== probe start (pid $$) ==="
say "board: $(uname -r) $(. /etc/os-release 2>/dev/null; echo "$PRETTY_NAME")"
say "module ip: $IP  conf: $([ -r "$CONF" ] && echo yes || echo no)"
say "usb: $(dmesg 2>/dev/null | grep -c 'usb 1-1: new high-speed') enumerations since boot"
while read -r l; do say "  dmesg| $l"; done < <(dmesg 2>/dev/null | grep -E 'usb 1-1' | tail -6)

# log a clean shutdown marker (Ctrl-C / systemctl stop / reboot)
trap 'say "probe: stopping (board shutting down?)"; exit 0' TERM INT

# ---------------------------------------------------------------- loop
ping_st=init; http_st=init; rtsp_st=init
last_clock=""; last_beat=0
fail_since=0

while :; do
    p=$(probe_ping)
    [ "$p" != "$ping_st" ] && { say "ping      $ping_st -> $p"; ping_st=$p; }

    h=$(probe_tcp 80)
    [ "$h" != "$http_st" ] && { say "http:80   $http_st -> $h"; http_st=$h; }

    r=$(probe_tcp 554)
    if [ "$r" != "$rtsp_st" ]; then
        say "rtsp:554  $rtsp_st -> $r"
        rtsp_st=$r
        if [ "$r" = "up" ]; then
            say "MODULE READY (rtsp listening) after $(uptime_s)s board uptime"
            fail_since=0
        else
            fail_since=$(uptime_s)
        fi
    fi

    now=$(date +%s)
    if [ $((now - last_beat)) -ge 60 ]; then
        last_beat=$now
        c=$(module_clock)
        if [ -n "$c" ] && [ "$c" != "$last_clock" ]; then
            say "module clock: $c"
            last_clock=$c
        fi
        say "heartbeat ping=$ping_st http=$http_st rtsp=$rtsp_st$([ -n "$fail_since" ] && [ "$rtsp_st" != up ] && echo " (rtsp down since up=${fail_since}s)")"
    fi

    sleep 2
done
