#!/usr/bin/env bash
# Signs a release for the in-app updater: checks the app zip, writes update.txt (the
# manifest the core checks), signs it with a FIDO key (touch it when it blinks) and
# writes update.sig in the core's format, plus SHA256SUMS. Publishes nothing: it prints
# the gh line to run.
#
# usage: tools/sign-release.sh VERSION [ZIP] [MIN_FROM]
#   VERSION   e.g. 0.5.0-alpha.1 (the tag is vVERSION; must equal the zip's version.txt)
#   ZIP       default app/dist/PPSA99072.zip
#   MIN_FROM  oldest installed version that may update to it (default 0.0.0)
# KEY=~/.ssh/ps5-discord-release-tpm to sign with the spare (default: the ZUKEY).
set -euo pipefail

version=${1:?usage: tools/sign-release.sh VERSION [ZIP] [MIN_FROM]}
zip=${2:-app/dist/PPSA99072.zip}
min_from=${3:-0.0.0}
key=${KEY:-$HOME/.ssh/ps5-discord-release-zukey}
out=dist/release-v$version
asset=ps5-discord-app-v$version.zip

if git rev-parse -q --verify "refs/tags/v$version" >/dev/null ||
    git ls-remote --exit-code --tags origin "v$version" >/dev/null 2>&1; then
    echo "sign-release: tag v$version exists already: a new zip needs a new version" >&2
    exit 1
fi
mkdir -p "$out"
cp "$zip" "$out/$asset"

# The zip: stored entries only, all 0777, one PPSA99072/ folder with the app's own files.
python3 -I - "$out/$asset" "$version" <<'EOF'
import sys, zipfile
path, version = sys.argv[1:]
with zipfile.ZipFile(path) as z:
    names = set()
    for i in z.infolist():
        n = i.filename
        part = n.split("/")
        if not n.startswith("PPSA99072/") or ".." in part or n in names or "\\" in n:
            sys.exit(f"sign-release: bad entry {n}")
        if part[1] in ("control.key", "previous", ".PPSA99072.update"):
            sys.exit(f"sign-release: {n} must not be in a release")
        if i.compress_type != zipfile.ZIP_STORED or (i.external_attr >> 16) & 0o777 != 0o777:
            sys.exit(f"sign-release: {n} is not stored 0777 (tools/zip-open-modes.py)")
        names.add(n)
    for need in ("eboot.bin", "sce_sys/param.json", "assets/ps5-discord.elf", "version.txt"):
        if "PPSA99072/" + need not in names:
            sys.exit(f"sign-release: no {need}")
    inside = z.read("PPSA99072/version.txt").decode().strip()
    if inside != version:
        sys.exit(f"sign-release: the zip is {inside}, not {version}")
EOF

sha=$(sha256sum "$out/$asset" | cut -d' ' -f1)
size=$(stat -c %s "$out/$asset")
printf 'ps5-discord-update 1\nversion %s\nasset %s\nsize %s\nsha256 %s\nmin_from %s\n' \
    "$version" "$asset" "$size" "$sha" "$min_from" > "$out/update.txt"

rm -f "$out/update.txt.sig"
echo "sign-release: touch the key to sign"
ssh-keygen -Y sign -n ps5-discord-release -f "$key" "$out/update.txt"
ssh-keygen -Y check-novalidate -n ps5-discord-release -f "$out/update.txt.sig" < "$out/update.txt"

# update.sig: flags (1), counter (4), r (32), s (32), from the SSHSIG's signature.
python3 -I - "$out/update.txt.sig" "$out/update.sig" <<'EOF'
import base64, struct, sys
armored = open(sys.argv[1]).read().split("\n")
blob = base64.b64decode("".join(l for l in armored if l and not l.startswith("-----")))
def take(b, at):
    (n,) = struct.unpack(">I", b[at:at + 4])
    return b[at + 4:at + 4 + n], at + 4 + n
assert blob[:6] == b"SSHSIG"
at = 10  # magic and version
_, at = take(blob, at)  # public key
_, at = take(blob, at)  # namespace
_, at = take(blob, at)  # reserved
_, at = take(blob, at)  # hash algorithm
sig, _ = take(blob, at)
kind, at = take(sig, 0)
assert kind == b"sk-ecdsa-sha2-nistp256@openssh.com", kind
ecdsa, at = take(sig, at)
flags, counter = sig[at], struct.unpack(">I", sig[at + 1:at + 5])[0]
r, rat = take(ecdsa, 0)
s, _ = take(ecdsa, rat)
r, s = int.from_bytes(r, "big"), int.from_bytes(s, "big")
with open(sys.argv[2], "wb") as f:
    f.write(bytes([flags]) + struct.pack(">I", counter) + r.to_bytes(32, "big") + s.to_bytes(32, "big"))
EOF
rm "$out/update.txt.sig"
(cd "$out" && sha256sum "$asset" update.txt update.sig > SHA256SUMS)

echo "sign-release: $out ready. Publish with:"
echo "  gh release create v$version --target <commit> --prerelease --title v$version --notes-file <notes> \\"
echo "    $out/$asset $out/update.txt $out/update.sig $out/SHA256SUMS"
