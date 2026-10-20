#!/bin/bash
# Build one env and stage what a release needs, in the layout make_flasher_release.py
# --local expects, then free the build directory.
#
#     tools/oshi/build_board.sh <env> <arch>
#
# A board already staged is skipped so a stopped matrix can be relaunched; OSHI_REBUILD=1
# forces one.
set -u
env="${1:?usage: build_board.sh <env> <arch>}"; arch="${2:?}"
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
WORK="${OSHI_WORK:-$HOME/.oshi-build}"
PIO="${PIO:-pio}"
MIN_FREE_GB="${OSHI_MIN_FREE_GB:-4}"
mkdir -p "$WORK/stage" "$WORK/buildlogs"
cd "$REPO" || exit 9

# The trailing [0-9] matters: firmware-*-"$env"-* also matches LONGER env names, so
# "tbeam" matched "tbeam-s3-core" and three boards were silently never built.
if [ "${OSHI_REBUILD:-0}" != 1 ] && ls "$WORK"/stage/firmware-*-"$env"-[0-9]* >/dev/null 2>&1; then
  echo "SKIP $env (already staged)"; exit 0
fi
[ "${OSHI_REBUILD:-0}" = 1 ] && rm -rf "$WORK"/stage/firmware-*-"$env"-[0-9]*

free=$(df -g / | tail -1 | awk '{print $4}')
if [ "${free:-0}" -lt "$MIN_FREE_GB" ]; then echo "ABORT_DISK $env (${free}GB free)"; exit 8; fi

if ! "$PIO" run -e "$env" > "$WORK/buildlogs/$env.log" 2>&1; then
  echo "FAIL $env"; rm -rf ".pio/build/$env"; exit 1
fi

bd=".pio/build/$env"
ver=$(ls "$bd" 2>/dev/null | grep -oE "$env-[0-9]+\.[0-9]+\.[0-9]+\.[0-9a-f]+" | head -1 | sed "s/^$env-//")
if [ -z "$ver" ]; then echo "NOVER $env"; rm -rf "$bd"; exit 2; fi

dest="$WORK/stage/firmware-$arch-$env-$ver"
mkdir -p "$dest"
# Everything a flasher writes, plus the partition table. Never the .elf: 20-50 MB of debug
# symbols per board.
for f in "$bd"/firmware-$env-$ver.bin "$bd"/firmware-$env-$ver.factory.bin "$bd"/firmware-$env-$ver.uf2 \
         "$bd"/firmware-$env-$ver.hex "$bd"/littlefs-$env-$ver.bin "$bd"/*.mt.json; do
  [ -f "$f" ] && cp -p "$f" "$dest/"
done
rm -rf "$bd"
# libdeps is per-env and ~150 MB a board, so a 150-board run quietly ate 11 GB while only
# build/ was being cleaned.
rm -rf ".pio/libdeps/$env"
echo "OK $env $ver $(du -sk "$dest" | cut -f1)KB"
