#!/usr/bin/env python3
"""Turn one GitHub Actions CI run (every board) into a web-flasher release for oshi-messenger.com/lora/flash/.

    python3 tools/oshi/make_flasher_release.py RUN_ID OUT_DIR [--repo Lastoneparis/oshi-mesh-firmware] [--only env,env]

For each board artifact it keeps only what a flasher writes (factory image, OTA loader, filesystem, app image, or the
.uf2), drops the debug .elf, and writes OUT_DIR/<version>/manifest.json + SHA256SUMS.txt. Offsets come from the
build's own .mt.json partition table, never from a guess. Board names and makers come from the
custom_meshtastic_* keys of variants/**/platformio.ini.
"""

import argparse
import configparser
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASE_URL = "/lora/firmware/oshi-mesh"
CHIP = {"esp32": "ESP32", "esp32s3": "ESP32-S3", "esp32c3": "ESP32-C3", "esp32c6": "ESP32-C6", "esp32s2": "ESP32-S2"}
UF2_FAMILY = {"nrf52840": "nRF52840", "rp2040": "RP2040", "rp2350": "RP2350", "nrf54l15": "nRF54L15"}
MAKERS = ["Heltec", "LILYGO", "RAK", "Seeed", "B&Q", "Elecrow", "M5Stack", "muzi", "Meshnology", "RadioMaster",
          "Waveshare", "EBYTE", "Nano", "CanaryOne", "WisMesh"]


def board_metadata():
    """env -> {name, maker, support} from every variant's platformio.ini."""
    meta = {}
    for ini in glob.glob(os.path.join(ROOT, "variants", "**", "platformio.ini"), recursive=True):
        cp = configparser.ConfigParser(interpolation=None, strict=False)
        try:
            cp.read(ini)
        except configparser.Error:
            continue
        for sec in cp.sections():
            if not sec.startswith("env:"):
                continue
            s = cp[sec]
            name = s.get("custom_meshtastic_display_name", "").strip()
            tags = s.get("custom_meshtastic_tags", "").replace("\n", " ").strip()
            first_tag = re.split(r"[,\s]+", tags)[0] if tags else ""
            maker = next((m for m in MAKERS if m.lower() in (tags + " " + name).lower()), first_tag)
            maker = {"RPi": "Raspberry Pi", "DIY": "DIY"}.get(maker, maker)
            meta[sec[4:]] = {"name": name, "maker": maker.replace("RAK", "RAKwireless") if maker == "RAK" else maker,
                             "support": s.get("custom_meshtastic_support_level", "").strip(),
                             "dfu": s.get("custom_meshtastic_requires_dfu", "").strip().lower() == "true"}
    return meta


def mui_envs():
    """Envs whose resolved config pulls in meshtastic/device-ui (the LVGL colour UI). It carries Meshtastic's own logo and
    name, so those builds are not published as OSHI Mesh until the UI itself is rebranded."""
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    for f in [os.path.join(ROOT, "platformio.ini")] + glob.glob(os.path.join(ROOT, "variants", "**", "*.ini"), recursive=True):
        try:
            cp.read(f)
        except configparser.Error:
            pass

    def resolved(sec, key, seen=()):
        if sec not in cp or sec in seen:
            return ""
        v = cp[sec].get(key, "")
        for parent in [x.strip() for x in cp[sec].get("extends", "").split(",") if x.strip()]:
            v += "\n" + resolved(parent, key, seen + (sec,))
        return v

    out = set()
    for sec in cp.sections():
        if sec.startswith("env:"):
            txt = resolved(sec, "lib_deps") + resolved(sec, "build_flags")
            if "device-ui" in txt or "HAS_TFT=1" in txt:
                out.add(sec[4:])
    return out


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_id")
    ap.add_argument("out")
    ap.add_argument("--repo", default="Lastoneparis/oshi-mesh-firmware")
    ap.add_argument("--only", default="")
    ap.add_argument("--jobs", type=int, default=6, help="artifacts downloaded in parallel")
    ap.add_argument("--include-mui", action="store_true", help="also publish builds with the Meshtastic-branded colour UI")
    a = ap.parse_args()
    only = set(filter(None, a.only.split(",")))

    names = subprocess.run(["gh", "api", f"repos/{a.repo}/actions/runs/{a.run_id}/artifacts", "--paginate", "-q",
                            ".artifacts[].name"], capture_output=True, text=True, check=True).stdout.split()
    firmware = sorted(n for n in names if n.startswith("firmware-"))
    meta = board_metadata()
    targets, uf2, sums, version, skipped = [], [], [], None, []

    excluded = set() if a.include_mui else mui_envs()
    wanted = []
    for art in firmware:
        m = re.match(r"firmware-([a-z0-9]+)-(.+)-(\d+\.\d+\.\d+\.[0-9a-f]+)$", art)
        if m and (not only or m.group(2) in only):
            if m.group(2) in excluded:
                skipped.append(f"{m.group(2)} (Meshtastic colour UI)")
                continue
            wanted.append(art)
    cache = tempfile.mkdtemp(prefix="oshi-artifacts-")

    def fetch(art):
        d = os.path.join(cache, art)
        for attempt in range(3):  # the artifact API returns the odd 502
            shutil.rmtree(d, ignore_errors=True)
            r = subprocess.run(["gh", "run", "download", a.run_id, "-R", a.repo, "-n", art, "-D", d],
                               capture_output=True, text=True)
            if r.returncode == 0:
                break
            time.sleep(10 * (attempt + 1))
        else:
            raise RuntimeError(f"{art}: download failed 3 times: {r.stderr.strip()[:200]}")
        for elf in glob.glob(os.path.join(d, "*.elf")):  # debug symbols: 20-50 MB each, never flashed
            os.remove(elf)
        return art

    try:
        with ThreadPoolExecutor(max_workers=a.jobs) as pool:
            for done in pool.map(fetch, wanted):
                print(f"fetched {done}", flush=True)
    except BaseException:
        shutil.rmtree(cache, ignore_errors=True)  # a few GB of artifacts must not outlive a failed run
        raise

    for art in wanted:
        m = re.match(r"firmware-([a-z0-9]+)-(.+)-(\d+\.\d+\.\d+\.[0-9a-f]+)$", art)
        if not m:
            continue
        arch, env, ver = m.groups()
        if only and env not in only:
            continue
        version = version or ver
        if ver != version:
            sys.exit(f"mixed versions in one run: {ver} vs {version}")
        tmp = os.path.join(cache, art)
        if True:
            dest = os.path.join(a.out, version, env)
            info = meta.get(env, {})
            name = info.get("name") or env
            maker = info.get("maker") or "Other"
            # Only the firmware image: nRF52 artifacts also carry bootloader updates and factory-erase .uf2 files,
            # and a flasher offering one of those in place of the firmware would overwrite the wrong thing.
            uf2s = glob.glob(os.path.join(tmp, f"firmware-{env}-{ver}.uf2"))
            mtj = glob.glob(os.path.join(tmp, "*.mt.json"))
            if arch in UF2_FAMILY and uf2s:
                os.makedirs(dest, exist_ok=True)
                f = uf2s[0]
                shutil.copy2(f, dest)
                rel = f"{env}/{os.path.basename(f)}"
                sums.append((sha(f), rel))
                uf2.append({"id": env, "boardName": name, "vendor": maker, "family": UF2_FAMILY[arch],
                            "path": f"{BASE_URL}/{version}/{rel}", "bytes": os.path.getsize(f), "sha256": sha(f)})
                continue
            if arch not in CHIP or not mtj:
                skipped.append(f"{env} ({arch})")
                continue
            mt = json.load(open(mtj[0]))
            # Offsets by partition TYPE: some tables name them app0/app1, others app/flashApp (T-Beam).
            sub = {p.get("subtype"): int(p["offset"], 16) for p in mt.get("part", [])}
            parts = {"app0": sub.get("ota_0"), "app1": sub.get("ota_1"), "spiffs": sub.get("spiffs")}
            by_part = {f.get("part_name"): f["name"] for f in mt["files"] if f.get("part_name")}
            factory = next((f["name"] for f in mt["files"] if f["name"].endswith(".factory.bin")), None)
            app = by_part.get("app0")
            if not factory or not app:
                skipped.append(f"{env} (no factory/app image)")
                continue
            os.makedirs(dest, exist_ok=True)
            plan = [(factory, 0)]
            ota = by_part.get("app1")
            if ota and parts["app1"] is not None:
                plan.append((ota, parts["app1"]))
            lfs = by_part.get("spiffs")
            if lfs and parts["spiffs"] is not None:
                plan.append((lfs, parts["spiffs"]))
            files = []
            for fname, off in plan:
                src = os.path.join(tmp, fname)
                shutil.copy2(src, dest)
                rel = f"{env}/{fname}"
                sums.append((sha(src), rel))
                files.append({"path": f"{BASE_URL}/{version}/{rel}", "offset": hex(off), "bytes": os.path.getsize(src),
                              "sha256": sha(src)})
            src = os.path.join(tmp, app)
            shutil.copy2(src, dest)
            rel = f"{env}/{app}"
            sums.append((sha(src), rel))
            end = max(int(p["offset"], 16) + int(p["size"], 16) for p in mt["part"])
            targets.append({"id": env, "boardName": name, "vendor": maker, "chipMatch": CHIP[arch],
                            "minimumFlashMB": (end + (1 << 20) - 1) >> 20, "files": files,
                            "update": {"path": f"{BASE_URL}/{version}/{rel}", "offset": hex(parts["app0"] if parts["app0"] is not None else 0x10000),
                                       "bytes": os.path.getsize(src), "sha256": sha(src)}})
        print(f"ok {env}", flush=True)

    shutil.rmtree(cache, ignore_errors=True)
    if not version:
        sys.exit("no firmware artifacts")
    commit = version.rsplit(".", 1)[1]
    manifest = {
        "family": "OSHI Mesh", "version": version,
        "offsetsFrom": "each board's own partition table (.mt.json)",
        "source": f"https://github.com/{a.repo}/tree/{commit}",
        "provenance": f"{BASE_URL}/{version}/PROVENANCE.md",
        "built": __import__("datetime").date.today().isoformat(),
        "targets": sorted(targets, key=lambda t: (t["vendor"].lower(), t["boardName"].lower())),
        "uf2Targets": sorted(uf2, key=lambda t: (t["vendor"].lower(), t["boardName"].lower())),
    }
    out = os.path.join(a.out, version)
    json.dump(manifest, open(os.path.join(out, "manifest.json"), "w"), indent=1)
    with open(os.path.join(out, "SHA256SUMS.txt"), "w") as f:
        for h, r in sorted(sums, key=lambda x: x[1]):
            f.write(f"{h}  {r}\n")
    run_url = f"https://github.com/{a.repo}/actions/runs/{a.run_id}"
    with open(os.path.join(out, "PROVENANCE.md"), "w") as f:
        f.write(f"""# OSHI Mesh {version} - provenance

- Source: https://github.com/{a.repo} at commit `{commit}` (the suffix of the version string).
- Built by GitHub Actions, not on a developer's machine: {run_url}
  Every file here is an artifact of that public run; the run lists the exact toolchain and inputs.
- License: GPL-3.0. Compatible with Meshtastic and MeshCore. The source of this exact version, with its license and
  credits, is at the link above; the OSHI Mesh code lives in `src/oshi/` and `src/modules/OshiModule.*`.
- Boards: {len(targets)} ESP32-family boards (web flasher) and {len(uf2)} boards installed by .uf2 file.
- No region is set: a freshly flashed node transmits nothing until its region is chosen.

What the web flasher writes on an ESP32 board, with offsets from that build's own partition table (`.mt.json`):
full install = erase, then the factory image at 0x0, the OTA loader at the second app partition and the filesystem at the
spiffs partition; "keep my settings" = the app image alone at the first app partition, no erase.

Check any file: `shasum -a 256 -c SHA256SUMS.txt`.
""")
    print(f"version {version}: {len(targets)} ESP32 boards, {len(uf2)} UF2 boards, skipped {len(skipped)}: {', '.join(skipped)}")


if __name__ == "__main__":
    main()
