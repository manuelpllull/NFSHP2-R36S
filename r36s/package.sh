#!/bin/sh
# Makes the PortMaster package from the armhf build (r36s/build.sh first):
#
#   r36s/build/portmaster/nfshp2/     the port as PortMaster-New lays it
#                                     out (port.json, README.md,
#                                     screenshot.jpg, gameinfo.xml,
#                                     NFSHP2.sh, nfshp2/)
#   r36s/build/portmaster/nfshp2.zip  the testing zip: NFSHP2.sh and nfshp2/
#                                     (with the metadata inside, as installed)
#
# The package holds no game data: the player copies their own disc's
# files into nfshp2/gamedata (README.md). SDL2, GLES and libc are the
# system's, as PortMaster asks, so there is no libs.armhf.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(dirname "$here")
hereW=$( (cd "$here" && pwd -W) 2>/dev/null || echo "$here")
bin="$here/build/armhf/nfshp2.armhf"
out="$here/build/portmaster"
port="$out/nfshp2"
[ -f "$bin" ] || { echo "no $bin: run r36s/build.sh first" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$port/nfshp2/gamedata" "$port/nfshp2/licenses"

for f in NFSHP2.sh port.json README.md gameinfo.xml; do
    cp "$here/portmaster/$f" "$port/$f"
done
cp "$here/portmaster/screenshot.jpg" "$port/screenshot.jpg"
cp "$here/portmaster/cover.jpg" "$port/cover.jpg"
cp "$repo/LICENSE" "$port/nfshp2/licenses/LICENSE.nfshp2.txt"
cp "$here/portmaster/gamedata.txt" "$port/nfshp2/gamedata/README.txt"
# The settings file the launcher reads (ports/nfshp2/nfshp2.cfg on the card).
cp "$here/portmaster/nfshp2.cfg" "$port/nfshp2/nfshp2.cfg"

# The first run's install from a disc image (installer/): the patch script
# PortMaster's patcher runs, the installer, and the repository's own tools
# and name list it does the work with.
mkdir -p "$port/nfshp2/tools"
cp "$here/installer/patchscript" "$here/installer/nfshp2_install.py" "$port/nfshp2/tools/"
for f in zdir_extract.py bhash.py lzdecompress.py release.py; do
    cp "$repo/tools/$f" "$port/nfshp2/tools/$f"
done
cp "$repo/docs/zdir_names_retail.txt" "$port/nfshp2/tools/"
# The unpacker that writes the files (installer/unpack/build.sh).
unpack="$here/build/armhf/nfshp2-unpack"
[ -f "$unpack" ] || { echo "no $unpack: run r36s/installer/unpack/build.sh first" >&2; exit 1; }
cp "$unpack" "$port/nfshp2/tools/"
chmod 755 "$port/nfshp2/tools/nfshp2-unpack"

# The binary, stripped: crash reports' addresses still name lines through
# the unstripped build/armhf/nfshp2.armhf.
MSYS_NO_PATHCONV=1 docker run --rm -v "$hereW:/r36s" "${IMAGE:-nfshp2-r36s-build-armhf}" \
    arm-linux-gnueabihf-strip -o /r36s/build/portmaster/nfshp2/nfshp2/nfshp2.armhf \
    /r36s/build/armhf/nfshp2.armhf

# Line endings: PortMaster's scripts must be LF (CRLF gives a black screen).
for f in "$port/NFSHP2.sh" "$port/port.json" "$port/README.md" "$port/gameinfo.xml" \
         "$port/nfshp2/tools/patchscript" "$port/nfshp2/tools/"*.py; do
    if grep -q "$(printf '\r')" "$f"; then echo "CRLF in $f" >&2; exit 1; fi
done

# Windows' python3 may be the Store's stub: python first.
PY=python
command -v "$PY" >/dev/null 2>&1 || PY=python3
"$PY" "$here/scripts/make_zip.py" "$port" NFSHP2.sh nfshp2 "$out/nfshp2.zip" \
    port.json README.md gameinfo.xml screenshot.jpg cover.jpg
