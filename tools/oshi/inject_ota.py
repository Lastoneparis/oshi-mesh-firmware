#!/usr/bin/env python3
"""Add the OTA loader to a locally staged build, as CI's "Update manifest with OTA file" step does.

    python3 tools/oshi/inject_ota.py [STAGE_DIR]     default: $OSHI_WORK/stage

A local `pio run` never produces the loader CI downloads for the second app partition, so without this
a locally assembled release omits it - silently, because the assembler simply never sees the file. For
each esp32/esp32s3/esp32c3/esp32c6 board this copies the loader in and appends it to that board's
.mt.json as the app1 partition, keyed by md5 and size exactly as the CI step does.

Loaders are cached in <stage>/../ota and fetched from the same "latest" releases CI uses, so the blob
tracks upstream rather than the firmware version - same caveat CI has.
"""
import glob, hashlib, json, os, re, subprocess, sys

PICK = {"esp32": "mt-esp32-ota.bin", "esp32s3": "mt-esp32s3-ota.bin",
        "esp32c3": "bleota-c3.bin", "esp32c6": "bleota-c3.bin"}
URL = {"mt-esp32-ota.bin": "https://github.com/meshtastic/esp32-unified-ota/releases/latest/download/mt-esp32-ota.bin",
       "mt-esp32s3-ota.bin": "https://github.com/meshtastic/esp32-unified-ota/releases/latest/download/mt-esp32s3-ota.bin",
       "bleota-c3.bin": "https://github.com/meshtastic/firmware-ota/releases/latest/download/firmware-c3.bin"}

stage = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.environ.get("OSHI_WORK", os.path.expanduser("~/.oshi-build")), "stage")
ota = os.path.join(os.path.dirname(os.path.abspath(stage)), "ota")
os.makedirs(ota, exist_ok=True)


def loader(name):
    path = os.path.join(ota, name)
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        subprocess.run(["curl", "-fsSL", "-o", path, URL[name]], check=True)
    return path


done = already = skipped = 0
for d in sorted(glob.glob(os.path.join(stage, "firmware-*"))):
    m = re.match(r"firmware-([a-z0-9]+)-(.+)-(\d+\.\d+\.\d+\.[0-9a-f]+)$", os.path.basename(d))
    if not m:
        continue
    name = PICK.get(m.group(1))
    if not name:
        skipped += 1
        continue
    src = loader(name)
    blob = open(src, "rb").read()
    with open(os.path.join(d, name), "wb") as fh:
        fh.write(blob)
    entry = {"name": name, "md5": hashlib.md5(blob).hexdigest(), "bytes": len(blob), "part_name": "app1"}
    for mtj in glob.glob(os.path.join(d, "*.mt.json")):
        mt = json.load(open(mtj))
        if any(f.get("name") == name for f in mt.get("files", [])):
            already += 1
            continue
        mt.setdefault("files", []).append(entry)
        json.dump(mt, open(mtj, "w"), indent=1)
        done += 1
print(f"ota injected into {done} board(s) (already had it: {already}, non-esp32 skipped: {skipped})")
