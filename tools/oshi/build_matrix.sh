#!/bin/bash
# Build the boards in tools/oshi/boards.tsv, one architecture group at a time, each group
# against a freshly installed framework (see fresh_framework.sh).
#
#     tools/oshi/build_matrix.sh              every group, largest first
#     tools/oshi/build_matrix.sh esp32 rp2040 only these; also how a stopped run resumes
#
# Artifacts, logs and build_results.txt land in $OSHI_WORK (default ~/.oshi-build), staged
# for `make_flasher_release.py --local`.
#
# Serial inside a group, and it has to be: PlatformIO re-runs a tool-scons install/remove
# dance on every build and both cores share one package directory, so a parallel build
# reading it mid-dance dies with "No module named 'SCons.Tool.FortranCommon'" (3 workers
# gave 10 failures, 0 successes). Never SIGKILL a run either - pio killed mid-install
# leaves the package it was writing empty.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
WORK="${OSHI_WORK:-$HOME/.oshi-build}"
BOARDS="${OSHI_BOARDS:-$HERE/boards.tsv}"
RES="$WORK/build_results.txt"
mkdir -p "$WORK/stage" "$WORK/buildlogs"

boards_of() { awk -v a="$1" -F'\t' '!/^#/ && NF>=2 && $2==a {print $1}' "$BOARDS"; }

if [[ $# -gt 0 ]]; then
	groups="$*"
else
	groups=$(awk -F'\t' '!/^#/ && NF>=2 {print $2}' "$BOARDS" | sort | uniq -c | sort -rn | awk '{print $2}')
fi

total=0
for g in $groups; do total=$((total + $(boards_of "$g" | grep -c .))); done
[[ "$total" -gt 0 ]] || {
	echo "no boards for: $groups (in $BOARDS)"
	exit 1
}
echo "### MATRIX $total board(s), $(wc -w <<<"$groups" | tr -d ' ') group(s) $(date '+%F %H:%M:%S')" >>"$RES"

i=0
for g in $groups; do
	echo "### GROUP $g ($(boards_of "$g" | grep -c .) boards) $(date '+%H:%M:%S')" >>"$RES"

	out=$("$HERE/fresh_framework.sh" "$g" 2>&1)
	rc=$?
	printf '%s\n' "$out" >>"$RES"
	[[ $rc -eq 0 ]] || {
		echo "### ABORT at group $g: framework not pristine" >>"$RES"
		exit 1
	}

	for env in $(boards_of "$g"); do
		i=$((i + 1))
		r=$("$HERE/build_board.sh" "$env" "$g")
		echo "[$i/$total] $r" >>"$RES"
		case "$r" in ABORT_DISK*)
			echo "### stopped: disk" >>"$RES"
			exit 8
			;;
		esac
	done

	# Evidence that the restore is doing something, and the first number to look at if a
	# board that used to build starts failing.
	"$HERE/fresh_framework.sh" --drift "$g" >>"$RES" 2>&1
done

# Without this a locally assembled release omits the OTA loader CI writes to the second app
# partition, and nothing says so. Same "latest" releases CI pulls from, so the loader tracks
# upstream rather than the firmware version - the same caveat CI has.
mkdir -p "$WORK/ota"
fetch_ota() { # <file> <url>
	[[ -s "$WORK/ota/$1" ]] && return 0
	curl -fsSL -o "$WORK/ota/$1" "$2" || {
		echo "### WARNING: could not fetch $1" >>"$RES"
		return 1
	}
}
fetch_ota mt-esp32-ota.bin https://github.com/meshtastic/esp32-unified-ota/releases/latest/download/mt-esp32-ota.bin
fetch_ota mt-esp32s3-ota.bin https://github.com/meshtastic/esp32-unified-ota/releases/latest/download/mt-esp32s3-ota.bin
fetch_ota bleota-c3.bin https://github.com/meshtastic/firmware-ota/releases/latest/download/firmware-c3.bin
python3 "$HERE/inject_ota.py" "$WORK/stage" >>"$RES" 2>&1 || echo "### WARNING: inject_ota.py failed" >>"$RES"

echo "### DONE staged=$(find "$WORK/stage" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ') ok=$(grep -c ' OK ' "$RES") skip=$(grep -c ' SKIP ' "$RES") fail=$(grep -c ' FAIL ' "$RES")" >>"$RES"
df -g /System/Volumes/Data | tail -1 | awk '{print "### free GB:", $4}' >>"$RES"
tail -5 "$RES"
