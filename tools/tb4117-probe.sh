#!/bin/bash
# Persistent watchdog for the TB-4117 module.
#
# Logs, across reboots and power cuts, when the module answers on
# ping / HTTP / RTSP, and reboots it through ISAPI when it is stuck in the
# "network alive, RTSP never starts" state that a too-short power cut leaves.
#
#   /var/log/tb4117/probe.log       current log (rotated to .1 at 1 MB)
#
# IMPORTANT: every interval below is measured with the board's monotonic
# uptime, never with the wall clock. The board has no RTC: it boots with a
# bogus date and NTP then jumps it forward by months. wall-clock arithmetic
# across that jump reports an interval of ~500 days and made the self-heal
# reboot perfectly healthy modules.

LOG_DIR=/var/log/tb4117
LOG=$LOG_DIR/probe.log
CONF=/home/linaro/linux-app/device.conf

mkdir -p "$LOG_DIR"

# ---------------------------------------------------------------- config
IP=192.168.64.64
AUTH=admin:admin
if [ -r "$CONF" ]; then
    v=$(sed -n 's/^camera=//p' "$CONF" | head -1); [ -n "$v" ] && IP=$v
    v=$(sed -n 's/^auth=//p'   "$CONF" | head -1); [ -n "$v" ] && AUTH=$v
fi

STUCK_SECONDS=${STUCK_SECONDS:-150}   # http up but no rtsp for this long -> reboot
REBOOT_COOLDOWN=${REBOOT_COOLDOWN:-600}

# ---------------------------------------------------------------- helpers
BOOT=$(tr -d '-' < /proc/sys/kernel/random/boot_id)
uptime_s() { cut -d. -f1 /proc/uptime; }

rotate() {
    if [ -f "$LOG" ] && [ "$(stat -c%s "$LOG" 2>/dev/null || echo 0)" -gt 1048576 ]; then
        mv -f "$LOG" "$LOG.1"
    fi
}

# stamp: wall clock for humans, up= for arithmetic. Flag an unsynced clock so
# a 2025/1970 timestamp in the log is not mistaken for a real event time.
stamp() {
    local d; d=$(date '+%F %T')
    case "$d" in
        19[0-9][0-9]-*|20[0-1][0-9]-*) d="$d(clock-unset)" ;;
    esac
    printf '%s' "$d"
}

say() {
    rotate
    printf '%s up=%ss boot=%s %s\n' \
        "$(stamp)" "$(uptime_s)" "${BOOT:0:8}" "$*" >> "$LOG"
}

probe_ping() { ping -c1 -W1 "$IP" >/dev/null 2>&1 && echo up || echo down; }
probe_tcp()  { timeout 2 bash -c "exec 3<>/dev/tcp/$IP/$1" 2>/dev/null && echo up || echo down; }

module_clock() {   # the module has no RTC: its Date header is effectively uptime
    timeout 3 curl -s -D - -o /dev/null "http://$IP/" 2>/dev/null \
        | tr -d '\r' | sed -n 's/^[Dd]ate: //p' | head -1
}

module_reboot() {
    local code
    code=$(timeout 12 curl -s -o /dev/null -w '%{http_code}' --digest -u "$AUTH" \
                -X PUT "http://$IP/ISAPI/System/reboot" 2>/dev/null)
    say "SELF-HEAL: PUT /ISAPI/System/reboot -> $code"
    return 0
}

# ---------------------------------------------------------------- start-up
say "=== probe start (pid $$) ==="
say "module ip: $IP  conf: $([ -r "$CONF" ] && echo yes || echo no)"
say "usb: $(dmesg 2>/dev/null | grep -c 'usb 1-1: new high-speed') enumerations since boot"
while read -r l; do say "  dmesg| $l"; done < <(dmesg 2>/dev/null | grep -E 'usb 1-1' | tail -6)

trap 'say "probe: stopping (board shutting down?)"; exit 0' TERM INT

# ---------------------------------------------------------------- loop
ping_st=init; http_st=init; rtsp_st=init
rtsp_down_at=0      # board uptime when 554 was first seen down (0 = not stuck)
last_reboot=0       # board uptime of the last self-heal (0 = never)
last_clock=""; last_beat=0; last_wall=0
healed=0

while :; do
    now_up=$(uptime_s)
    now_wall=$(date +%s)

    # NTP sync jumps the wall clock forward by months on this board (no RTC).
    # Record it so the timestamps in the log make sense; timers use uptime.
    if [ "$last_wall" -ne 0 ] && [ $((now_wall - last_wall)) -gt 3600 ]; then
        say "clock jumped +$((now_wall - last_wall))s (NTP sync); timers use uptime, unaffected"
        last_beat=0
    fi
    last_wall=$now_wall

    p=$(probe_ping)
    [ "$p" != "$ping_st" ] && { say "ping      $ping_st -> $p"; ping_st=$p; }

    h=$(probe_tcp 80)
    [ "$h" != "$http_st" ] && { say "http:80   $http_st -> $h"; http_st=$h; }

    r=$(probe_tcp 554)
    if [ "$r" != "$rtsp_st" ]; then
        say "rtsp:554  $rtsp_st -> $r"
        rtsp_st=$r
        if [ "$r" = "up" ]; then
            say "MODULE READY after ${now_up}s board uptime"
            rtsp_down_at=0
        fi
    fi

    # half-reset state: the module answers on HTTP but its RTSP server never
    # starts. A short power cut leaves it like this; an ISAPI reboot clears it.
    # Only act once it has been down STUCK_SECONDS — measured in board uptime.
    if [ "$ping_st" = up ] && [ "$http_st" = up ] && [ "$rtsp_st" = down ]; then
        [ "$rtsp_down_at" -eq 0 ] && rtsp_down_at=$now_up
        waited=$((now_up - rtsp_down_at))

        if [ "$last_reboot" -eq 0 ]; then
            cooldown_ok=1
        elif [ $((now_up - last_reboot)) -ge "$REBOOT_COOLDOWN" ]; then
            cooldown_ok=1
        else
            cooldown_ok=0
        fi

        if [ "$waited" -ge "$STUCK_SECONDS" ] && [ "$cooldown_ok" = 1 ]; then
            say "stuck: http up but rtsp down for ${waited}s -> rebooting module"
            module_reboot
            last_reboot=$now_up
            rtsp_down_at=0
            healed=$((healed + 1))
        fi
    else
        rtsp_down_at=0
    fi

    if [ $((now_wall - last_beat)) -ge 60 ] || [ "$last_beat" -eq 0 ]; then
        last_beat=$now_wall
        c=$(module_clock)
        if [ -n "$c" ] && [ "$c" != "$last_clock" ]; then
            say "module clock: $c"
            last_clock=$c
        fi
        say "heartbeat ping=$ping_st http=$http_st rtsp=$rtsp_st self-heal=$healed"
    fi

    sleep 2
done
