#!/usr/bin/env python3
"""Collect every CJK character used in main.c and regenerate the LVGL font."""
import io
import re
import subprocess
import sys

SRC = r'X:\DEV\Linux\4117\linux-app\src\main.c'
OUT = r'X:\DEV\Linux\4117\linux-app\src\lv_font_cn_22.c'
FONT = r'C:\Windows\Fonts\simhei.ttf'

text = io.open(SRC, encoding='utf-8').read()

# every non-ASCII char that appears inside string literals / anywhere in the source
chars = sorted({c for c in text if ord(c) > 0x2000})
cjk = ''.join(chars)
print('CJK chars found (%d): %s' % (len(chars), cjk))

symbols = cjk + ' 0123456789.:%-/'

cmd = [
    'npx', '-y', 'lv_font_conv',
    '--font', FONT,
    '--size', '22',
    '--bpp', '4',
    '--format', 'lvgl',
    '--no-compress',
    '--symbols', symbols,
    '-o', OUT,
    '--lv-include', 'lvgl.h',
]

print('running lv_font_conv...')
r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
if r.returncode != 0:
    print(r.stdout[-2000:])
    print(r.stderr[-2000:])
    sys.exit(1)
print('font regenerated with %d symbols' % len(symbols))
