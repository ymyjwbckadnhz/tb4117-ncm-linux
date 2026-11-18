#!/usr/bin/env python3
"""Summarise /var/log/tb4117/probe.log: one block per board boot.

Usage:  python3 tools/analyze_probe.py [probe.log]
"""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "/var/log/tb4117/probe.log"

line_re = re.compile(r"^(\S+ \S+) up=(\d+)s boot=(\S+) (.*)$")

boots = []          # list of dicts
current = None

with open(path, encoding="utf-8", errors="replace") as fh:
    for raw in fh:
        m = line_re.match(raw.rstrip("\n"))
        if not m:
            continue
        ts, up, boot, msg = m.group(1), int(m.group(2)), m.group(3), m.group(4)

        if current is None or current["boot"] != boot:
            current = {
                "boot": boot, "first": ts, "last": ts,
                "up_first": up, "up_last": up,
                "ready_at": None, "clock": [], "events": [],
            }
            boots.append(current)

        current["last"] = ts
        current["up_last"] = up
        if "MODULE READY" in msg:
            current["ready_at"] = up
        if msg.startswith("module clock:"):
            current["clock"].append(msg.split(":", 1)[1].strip())
        if any(k in msg for k in ("ping ", "http:80", "rtsp:554", "probe start", "probe:")):
            current["events"].append((up, msg))

print("%-10s %-19s %8s %8s  %s" % ("boot", "first line", "up_first", "up_last", "result"))
for b in boots:
    res = ("RTSP ready at up=%ds" % b["ready_at"]) if b["ready_at"] else "NEVER READY"
    print("%-10s %-19s %8d %8d  %s" % (b["boot"], b["first"], b["up_first"], b["up_last"], res))

print()
for b in boots:
    print("=== boot %s ===" % b["boot"])
    for up, msg in b["events"][:12]:
        print("  up=%-6d %s" % (up, msg))
    if b["clock"]:
        print("  module clock: %s ... %s" % (b["clock"][0], b["clock"][-1]))
    print()
