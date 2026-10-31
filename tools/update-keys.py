#!/usr/bin/env python3
"""Print src/update_keys.h from sk-ecdsa-sha2-nistp256 public keys (ssh-keygen -t ecdsa-sk).

usage: update-keys.py KEY.pub [KEY.pub]   (the main key first, then the spare)
"""
import base64
import struct
import sys

APP = b"ssh:ps5-discord-release"
print("// The keys that sign releases (tools/sign-release.sh), written by tools/update-keys.py.")
print("// Raw P-256 points of FIDO keys made with ssh-keygen -t ecdsa-sk, application " + APP.decode() + ".")
print("#pragma once\n")
print("static const unsigned char update_keys[][65] = {")
for path in sys.argv[1:]:
    fields = open(path).read().split()
    blob = base64.b64decode(fields[1])
    parts, at = [], 0
    while at < len(blob):
        (n,) = struct.unpack(">I", blob[at:at + 4])
        parts.append(blob[at + 4:at + 4 + n])
        at += 4 + n
    kind, curve, point, app = parts
    if kind != b"sk-ecdsa-sha2-nistp256@openssh.com" or curve != b"nistp256" or len(point) != 65 or app != APP:
        sys.exit(f"{path}: not a P-256 FIDO key for {APP.decode()}")
    print(f"    // {path.rsplit('/', 1)[-1]}")
    hexes = [f"0x{b:02x}" for b in point]
    for i in range(0, 65, 13):
        print("    " + ("{" if i == 0 else " ") + ", ".join(hexes[i:i + 13]) + ("}," if i + 13 >= 65 else ","))
print("};")
