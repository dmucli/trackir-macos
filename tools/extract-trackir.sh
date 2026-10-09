#!/bin/sh
# Unpacks NaturalPoint's Windows installer (TrackIR_5.5.3.exe) on a Mac and copies out what trackir-mac needs:
# TrackIR5.exe (FPGA images, camera keys), the stock Profiles and NPClient*.dll (for `make verify`).
# Nothing is executed; the installer is only read.
#
#   tools/extract-trackir.sh ~/Downloads/TrackIR_5.5.3.exe [output-dir]
#
# Needs: brew install sevenzip msitools
set -eu

if [ $# -lt 1 ]; then
    echo "usage: $0 <TrackIR_5.5.3.exe> [output-dir]" >&2
    exit 2
fi
INSTALLER=$1
OUT=${2:-TrackIR5}
for tool in 7zz msiextract python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing $tool: brew install sevenzip msitools" >&2; exit 1; }
done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 1. The WiX Burn bootstrapper's first cabinet holds its manifest (ux/0).
7zz x -y -o"$TMP/ux" "$INSTALLER" >/dev/null
[ -f "$TMP/ux/0" ] || { echo "not a WiX Burn installer (no manifest)" >&2; exit 1; }

# 2. The attached container is a second cabinet inside the .exe whose size the manifest gives. Carve it out and
#    work out which of its entries (a0, a1, ...) is the TrackIR MSI.
MSI_NAME=$(python3 -I - "$INSTALLER" "$TMP/ux/0" "$TMP/container.cab" <<'EOF'
import re, struct, sys
exe = open(sys.argv[1], "rb").read()
manifest = open(sys.argv[2], "rb").read().decode("utf-8", "replace")
size = int(re.search(r'<Container [^>]*FileSize="(\d+)"', manifest).group(1))
at = exe.find(b"MSCF", 1)
while at != -1:
    if struct.unpack_from("<I", exe, at + 8)[0] == size:  # CFHEADER.cbCabinet
        break
    at = exe.find(b"MSCF", at + 1)
if at == -1:
    sys.exit("attached container not found")
open(sys.argv[3], "wb").write(exe[at:at + size])
for attrs in re.findall(r"<Payload ([^>]*)>", manifest):
    path = re.search(r'FilePath="([^"]*)"', attrs)
    source = re.search(r'SourcePath="([^"]*)"', attrs)
    if path and source and path.group(1).lower().endswith(".msi") and "trackir" in path.group(1).lower():
        print(source.group(1))
        break
else:
    sys.exit("no TrackIR .msi in the manifest")
EOF
)
7zz x -y -o"$TMP/payloads" "$TMP/container.cab" >/dev/null
[ -f "$TMP/payloads/$MSI_NAME" ] || { echo "payload $MSI_NAME missing from the container" >&2; exit 1; }

# 3. Unpack the MSI and keep the TrackIR5 folder.
mkdir -p "$TMP/msi"
(cd "$TMP/msi" && msiextract "$TMP/payloads/$MSI_NAME" >/dev/null)
SRC=$(find "$TMP/msi" -type f -name TrackIR5.exe -print | head -n 1)
[ -n "$SRC" ] || { echo "TrackIR5.exe not found in the MSI" >&2; exit 1; }
mkdir -p "$OUT"
cp "$SRC" "$OUT/"
for dll in NPClient.dll NPClient64.dll; do  # only used by `make verify`
    [ -f "$(dirname "$SRC")/$dll" ] && cp "$(dirname "$SRC")/$dll" "$OUT/"
done
[ -d "$(dirname "$SRC")/Profiles" ] && cp -R "$(dirname "$SRC")/Profiles" "$OUT/"

echo "extracted $OUT/TrackIR5.exe and $OUT/Profiles"
echo "next: build/trackir-mac extract-fpga \"$OUT/TrackIR5.exe\""
