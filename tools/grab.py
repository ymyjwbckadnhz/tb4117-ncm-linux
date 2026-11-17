#!/usr/bin/env python3
"""Grab the panel framebuffer on the board, fetch it with scp, save as PNG."""
import os
import subprocess
import sys
import tempfile

KEY = os.path.expanduser(r'~/.ssh/id_ed25519_board')
HOST = r'linaro@10.0.0.47'
BMP = '/tmp/grab.bmp'
OPTS = ['-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=NUL', '-o', 'BatchMode=yes']


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'grab.png'
    r = subprocess.run(
        ['ssh', '-i', KEY] + OPTS + [HOST,
         'sudo -n /home/linaro/linux-app/tools/drm_grab /dev/dri/card0 %s && '
         'sudo -n chmod 644 %s' % (BMP, BMP)],
        capture_output=True, timeout=60)
    if r.returncode != 0:
        print('grab failed:', r.stderr.decode()[:300])
        return 1

    tmp = os.path.join(tempfile.gettempdir(), 'grab.bmp')
    r = subprocess.run(['scp', '-i', KEY] + OPTS + [HOST + ':' + BMP, tmp],
                       capture_output=True, timeout=60)
    if r.returncode != 0:
        print('scp failed:', r.stderr.decode()[:300])
        return 1

    from PIL import Image
    img = Image.open(tmp)
    img.save(out)
    print('saved', out, img.size)
    return 0


if __name__ == '__main__':
    sys.exit(main())
