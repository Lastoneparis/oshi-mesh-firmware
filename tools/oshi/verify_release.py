#!/usr/bin/env python3
"""Check a published OSHI Mesh release against its own manifest, before anyone flashes from it.

    python3 tools/oshi/verify_release.py [BASE_URL]

For every file the manifest offers:
  - it is served, with the byte count the manifest states;
  - a .uf2 really is one (UF2 magic) for the family the manifest names;
  - a file from the published version is covered by SHA256SUMS.txt;
  - a file from an OLDER version is allowed only when its entry says so with carriedOverFrom,
    and then its own sha256 must match the bytes the server sends. An unlabelled path from
    another version is the failure this exists to catch - SHA256SUMS.txt does not cover it,
    so nothing else would notice.

Exits non-zero listing every offender. HEAD requests for the sizes, a 32-byte range for each
UF2 header, and a full download only for a carried-over file, so it is cheap enough to run
before every publish.
"""

import hashlib
import json
import re
import shutil
import subprocess
import sys

DEFAULT = "https://oshi-messenger.com/lora/firmware/oshi-mesh/latest"
BASE = (sys.argv[1] if len(sys.argv) > 1 else DEFAULT).rstrip("/")
UF2_MAGIC0 = 0x0A324655
UF2_FAMILY_ID = {"nRF52840": 0xADA52840, "RP2040": 0xE48BFF56, "RP2350": 0xE48BFF59}

curl = shutil.which("curl") or sys.exit("curl not found on PATH")


def get(url, rng=None):
    cmd = [curl, "-fsSL"] + (["-r", rng] if rng else []) + [url]
    r = subprocess.run(cmd, capture_output=True)
    return r.stdout if r.returncode == 0 else None


def head_size(url):
    r = subprocess.run([curl, "-fsSLI", url], capture_output=True, text=True)
    if r.returncode != 0:
        return None
    m = re.findall(r"(?im)^content-length:\s*(\d+)", r.stdout)
    return int(m[-1]) if m else -1


def main():
    raw = get(f"{BASE}/manifest.json") or sys.exit(f"cannot fetch {BASE}/manifest.json")
    m = json.loads(raw)
    version = m["version"]
    sums_raw = get(f"{BASE}/SHA256SUMS.txt") or sys.exit("cannot fetch SHA256SUMS.txt")
    sums = {
        rel.strip(): h.strip()
        for h, _, rel in (ln.partition("  ") for ln in sums_raw.decode().splitlines())
        if rel
    }
    host = BASE.split("/lora/")[0]

    entries = []
    for t in m["targets"] + m["uf2Targets"]:
        for f in [t] if "path" in t else t.get("files", []):
            entries.append(
                {
                    "id": t["id"],
                    "path": f["path"],
                    "bytes": f.get("bytes", t.get("bytes")),
                    "family": t.get("family"),
                    "sha256": f.get("sha256", t.get("sha256")),
                    "carried": t.get("carriedOverFrom"),
                }
            )
    print(
        f"{version}: {len(m['targets'])} ESP32 targets, {len(m['uf2Targets'])} UF2 targets, {len(entries)} files"
    )

    unlabelled, uncovered, carried = [], [], []
    for e in entries:
        own = f"/{version}/" in e["path"]
        if own:
            rel = e["path"].split(f"/{version}/", 1)[1]
            if rel not in sums:
                uncovered.append((e["id"], e["path"]))
        elif e["carried"] and f"/{e['carried']}/" in e["path"]:
            carried.append(e)
        else:
            unlabelled.append((e["id"], e["path"]))

    missing, wrong_size, bad_uf2, bad_hash = [], [], [], []
    for e in entries:
        size = head_size(host + e["path"])
        if size is None:
            missing.append((e["id"], e["path"]))
            continue
        if e["bytes"] is not None and size >= 0 and size != e["bytes"]:
            wrong_size.append((e["id"], f"{size} served, manifest says {e['bytes']}"))
        if e["path"].endswith(".uf2"):
            head = get(host + e["path"], rng="0-31") or b""
            if len(head) >= 32:
                magic0 = int.from_bytes(head[0:4], "little")
                famid = int.from_bytes(head[28:32], "little")
                want = UF2_FAMILY_ID.get(e["family"])
                if magic0 != UF2_MAGIC0 or (want is not None and famid != want):
                    bad_uf2.append(
                        (
                            e["id"],
                            f"magic=0x{magic0:08x} family=0x{famid:08x} (manifest: {e['family']})",
                        )
                    )
    for e in carried:
        blob = get(host + e["path"])
        if blob is None:
            continue
        got = hashlib.sha256(blob).hexdigest()
        if not e["sha256"]:
            bad_hash.append((e["id"], "carried over with no sha256 in the manifest"))
        elif got != e["sha256"]:
            bad_hash.append(
                (e["id"], f"sha256 {got[:16]}... != manifest {e['sha256'][:16]}...")
            )

    fatal = {
        "from another version and NOT labelled carriedOverFrom": unlabelled,
        "from this version with no SHA256SUMS line": uncovered,
        "not served": missing,
        "byte count differs from the manifest": wrong_size,
        "not a UF2 for the stated family": bad_uf2,
        "carried over but the bytes do not match its sha256": bad_hash,
    }
    for label, rows in fatal.items():
        print(f"  {label}: {len(rows)}")
        for a, b in rows:
            print(f"    {a} -> {b}")
    if carried:
        print(
            f"  carried over from an earlier build (allowed, sha256 verified): {len(carried)}"
        )
        for e in carried:
            print(f"    {e['id']} <- {e['carried']}")
    if any(fatal.values()):
        sys.exit(1)
    print(
        "  OK: every file is served at the stated size; carried-over files match their own sha256."
    )


if __name__ == "__main__":
    main()
