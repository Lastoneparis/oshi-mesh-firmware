#!/bin/bash
# Restore one architecture's PlatformIO framework packages to a just-installed state.
#
#     tools/oshi/fresh_framework.sh <arch>            restore (snapshots one if absent)
#     tools/oshi/fresh_framework.sh --snapshot <arch> snapshot what is installed now
#     tools/oshi/fresh_framework.sh --drift <arch>    report movement from pristine
#
# A build mutates the framework package in place and the package is shared by every env of
# that architecture, so the Nth board compiles against what the previous N-1 left behind.
# One tbeam build strips 1.13 MB out of esp32/lib/libbt.a, cuts 19 KB from
# dio_qspi/include/sdkconfig.h, rewrites ld/sections.ld and deletes 427 component headers.
# Restoring is ~18 s for 2.1 GB, so build_matrix.sh does it before every group.
#
# --snapshot is only valid straight after `pio pkg install`, before any `pio run`.
set -u

REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
PRISTINE="${OSHI_PRISTINE_DIR:-$HOME/.platformio-pristine}"
WORK="${OSHI_WORK:-$HOME/.oshi-build}"
BOARDS="${OSHI_BOARDS:-$REPO/tools/oshi/boards.tsv}"
PIO="${PIO:-pio}"
PKG="$HOME/.platformio/packages"

mode=restore
case "${1:-}" in
--snapshot)
	mode=snapshot
	shift
	;;
--drift)
	mode=drift
	shift
	;;
esac
arch="${1:?usage: fresh_framework.sh [--snapshot|--drift] <arch>}"

# Only the esp32 family is proven to mutate; the others are refreshed anyway, because a
# cheap restore beats trusting a framework nobody has audited.
case "$arch" in
esp32 | esp32s2 | esp32s3 | esp32c2 | esp32c3 | esp32c6 | esp32h2 | esp32p4)
	pkgs="framework-arduinoespressif32 framework-arduinoespressif32-libs"
	;;
nrf52 | nrf52832 | nrf52840) pkgs="framework-arduinoadafruitnrf52" ;;
rp2040 | rp2350) pkgs="framework-arduinopico" ;;
*)
	echo "FRESH-SKIP $arch (no framework mapping)"
	exit 0
	;;
esac

# Sizes catch both rewritten text files and .a archives that lost members, without reading
# 2 GB of static libraries.
manifest() { (cd "$PKG" && find "$1" -type f -print0 | sort -z | xargs -0 stat -f '%z %N'); }
hashof() { shasum -a 256 "$1" | cut -c1-16; }

mkdir -p "$PRISTINE" "$WORK/buildlogs"

if [[ "$mode" = snapshot ]]; then
	for p in $pkgs; do
		[[ -d "$PKG/$p" ]] || {
			echo "FRESH-FAIL $arch ($p is not installed)"
			exit 1
		}
		manifest "$p" >"$PRISTINE/$p.manifest"
		(cd "$PKG" && tar --use-compress-program="zstd -3 -T0" -cf "$PRISTINE/$p.tar.zst.part" "$p") ||
			{
				echo "FRESH-FAIL $arch (could not snapshot $p)"
				exit 1
			}
		mv -f "$PRISTINE/$p.tar.zst.part" "$PRISTINE/$p.tar.zst"
		echo "FRESH-SNAPSHOT $p $(hashof "$PRISTINE/$p.manifest") files=$(wc -l <"$PRISTINE/$p.manifest" | tr -d ' ') snap=$(du -h "$PRISTINE/$p.tar.zst" | cut -f1)"
	done
	exit 0
fi

if [[ "$mode" = drift ]]; then
	rc=0
	for p in $pkgs; do
		[[ -f "$PRISTINE/$p.manifest" ]] || {
			echo "FRESH-DRIFT $p unknown (no snapshot)"
			continue
		}
		[[ -d "$PKG/$p" ]] || {
			echo "FRESH-DRIFT $p absent"
			rc=1
			continue
		}
		d=$(diff <(manifest "$p") "$PRISTINE/$p.manifest" | grep -c '^[<>]' || true)
		echo "FRESH-DRIFT $p $((d / 2)) file(s) differ from pristine"
	done
	exit $rc
fi

for p in $pkgs; do
	if [[ ! -f "$PRISTINE/$p.tar.zst" ]] || [[ ! -f "$PRISTINE/$p.manifest" ]]; then
		# No snapshot yet: wipe, let PlatformIO install, snapshot before any build touches it.
		env0=$(awk -v a="$arch" -F'\t' '!/^#/ && NF>=2 && $2==a {print $1; exit}' "$BOARDS")
		[[ -n "$env0" ]] || {
			echo "FRESH-FAIL $arch (no env of that arch in $BOARDS)"
			exit 1
		}
		echo "FRESH-BOOTSTRAP $arch ($p absent from $PRISTINE) via env $env0"
		rm -rf "${PKG:?}/${p:?}"
		(cd "$REPO" && "$PIO" pkg install -e "$env0") >"$WORK/buildlogs/_fresh-$arch.log" 2>&1 ||
			{
				echo "FRESH-FAIL $arch (pio pkg install -e $env0; see $WORK/buildlogs/_fresh-$arch.log)"
				exit 1
			}
		"$0" --snapshot "$arch" || exit 1
		continue
	fi
	want=$(hashof "$PRISTINE/$p.manifest")
	have=""
	[[ -d "$PKG/$p" ]] && have=$(manifest "$p" | shasum -a 256 | cut -c1-16)
	if [[ "$have" = "$want" ]]; then
		echo "FRESH-OK $p already pristine ($want)"
		continue
	fi
	echo "FRESH-RESTORE $p (installed=${have:-absent} pristine=$want)"
	# Delete before extracting rather than extract beside and swap: the libs package is
	# 2.1 GB. A failed extract leaves it absent, which fails loudly here and which a later
	# `pio` reinstalls from the network - the safe direction to fail in.
	rm -rf "${PKG:?}/${p:?}"
	(cd "$PKG" && tar --use-compress-program="zstd -d" -xf "$PRISTINE/$p.tar.zst") ||
		{
			echo "FRESH-FAIL $arch (could not extract $p; package is now ABSENT)"
			exit 1
		}
	now=$(manifest "$p" | shasum -a 256 | cut -c1-16)
	[[ "$now" = "$want" ]] || {
		echo "FRESH-FAIL $arch ($p restored to $now, expected $want)"
		exit 1
	}
	echo "FRESH-VERIFIED $p $now"
done
