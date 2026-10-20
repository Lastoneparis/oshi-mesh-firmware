#!/usr/bin/env python3
"""Emit "<env>\t<mcu>" for every env in platformio.ini: the architecture groups the matrix uses.

Reads PlatformIO's OWN resolved config (`pio project config --json-output`) rather than
parsing platformio.ini, which only pulls the envs in through extra_configs. board -> mcu
comes from the installed platforms' board JSON, then the repo's boards/ directory.
"""
import glob, json, os, sys

cfg_json, repo = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else ".")
board_mcu = {}
for pat in (os.path.expanduser("~/.platformio/platforms/*/boards/*.json"),
            os.path.join(repo, "boards", "*.json"),
            os.path.join(repo, "variants", "*", "boards", "*.json")):
    for f in glob.glob(pat):
        try:
            board_mcu[os.path.basename(f)[:-5]] = json.load(open(f))["build"]["mcu"]
        except Exception:
            pass

cfg = json.load(open(cfg_json))
sections = cfg.items() if isinstance(cfg, dict) else [(s, dict(o)) for s, o in cfg]
rows, unresolved = [], []
for name, opts in sections:
    if not name.startswith("env:"):
        continue
    opts = dict(opts) if not isinstance(opts, dict) else opts
    board = opts.get("board")
    if isinstance(board, list):
        board = board[0] if board else None
    if not board:
        continue
    mcu = board_mcu.get(board)
    (rows if mcu else unresolved).append((name[4:], mcu or board))
for env, mcu in sorted(rows):
    print(f"{env}\t{mcu}")
if unresolved:
    print("# unresolved board->mcu: " + ", ".join(f"{e}({b})" for e, b in sorted(unresolved)), file=sys.stderr)
