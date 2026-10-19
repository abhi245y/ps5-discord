#!/usr/bin/env python3
# ps5-discord - Drives the running app from a PC over FTP (development only).
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/remote.py shot <name> [out.png]   live frame, saved next to out (needs Pillow for .png)
#   tools/remote.py press down,down,cross   keys, one every 0.35 s
#   tools/remote.py quit
# The app reads /app0/dev/request.txt (the install folder) and writes its
# pictures to its own storage, which a PC can read while the title runs.
import io, json, os, sys, time
from ftplib import FTP
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOST = os.environ.get("PS5_HOST", "192.168.1.2")
PORT = int(os.environ.get("FTP_PORT", "2121"))
TITLE = json.loads((ROOT / "sce_sys/param.json").read_text())["titleId"]


def ftp():
    f = FTP()
    f.connect(HOST, PORT, timeout=10)
    f.login()
    return f


def request(verb, argument):
    f = ftp()
    try:
        f.mkd(f"/data/homebrew/{TITLE}/dev")
    except Exception:
        pass
    line = f"{verb} {argument} {time.time_ns()}\n".encode()
    f.storbinary(f"STOR /data/homebrew/{TITLE}/dev/request.txt", io.BytesIO(line))
    f.close()


def fetch(name, out):
    remote = f"/mnt/sandbox/{TITLE}_000/download0/hui/dev/{name}.bmp"
    for _ in range(20):
        time.sleep(0.5)
        data = io.BytesIO()
        f = ftp()
        try:
            f.retrbinary(f"RETR {remote}", data.write)
        except Exception:
            continue
        finally:
            f.close()
        if data.tell() > 54:
            break
    else:
        sys.exit(f"no picture at {remote}")
    if out.suffix == ".png":
        from PIL import Image
        Image.open(io.BytesIO(data.getvalue())).save(out)
    else:
        out.write_bytes(data.getvalue())
    print(out)


verb = sys.argv[1] if len(sys.argv) > 1 else ""
if verb == "shot" and len(sys.argv) >= 3:
    name = sys.argv[2]
    f = ftp()  # drop an old picture so a stale one is not taken for the new one
    try:
        f.delete(f"/mnt/sandbox/{TITLE}_000/download0/hui/dev/{name}.bmp")
    except Exception:
        pass
    f.close()
    request("shot", name)
    fetch(name, Path(sys.argv[3] if len(sys.argv) > 3 else f"{name}.png"))
elif verb == "press" and len(sys.argv) == 3:
    request("press", sys.argv[2])
elif verb == "quit":
    request("quit", "-")
else:
    sys.exit(__doc__ or "usage: remote.py shot <name> [out] | press <keys> | quit")
