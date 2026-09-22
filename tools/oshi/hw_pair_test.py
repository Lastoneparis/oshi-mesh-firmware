#!/usr/bin/env python3
"""Two real OSHI Mesh radios on USB serial: send one OMP message A -> B over the air and check it arrives
intact and that A's phone is told DELIVERED. Also checks interop text on the primary channel.

Usage: hw_pair_test.py PORT_A PORT_B [--bytes N]
"""

import argparse
import os
import random
import struct
import sys
import threading
import time

from meshtastic import serial_interface
from pubsub import pub

PRIVATE_APP = 256
FRAG = 182
STATES = {1: "QUEUED", 2: "SENT", 3: "DELIVERED", 4: "IN_CUSTODY", 5: "FAILED", 6: "UPLINKED", 7: "REJECTED"}

rx = {}
lock = threading.Lock()


def on_receive(packet, interface):
    d = packet.get("decoded", {})
    if d.get("portnum") not in ("PRIVATE_APP", "TEXT_MESSAGE_APP"):
        return
    p = d.get("payload", b"")
    if isinstance(p, str):
        p = p.encode()
    with lock:
        rx.setdefault(id(interface), []).append(bytes(p))


def frames(msg_id, dest, body):
    n = max(1, (len(body) + FRAG - 1) // FRAG)
    for i in range(n):
        yield bytes([0x4F, 0x53, 0x11]) + struct.pack("<IIIBBB", msg_id, 0, dest, i, n, 1) + body[i * FRAG:(i + 1) * FRAG]


def got(iface):
    with lock:
        return list(rx.get(id(iface), []))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--bytes", type=int, default=600)
    args = ap.parse_args()
    pub.subscribe(on_receive, "meshtastic.receive")
    A = serial_interface.SerialInterface(args.a)
    B = serial_interface.SerialInterface(args.b)
    a_num, b_num = A.myInfo.my_node_num, B.myInfo.my_node_num
    print(f"A=!{a_num:08x} B=!{b_num:08x}", flush=True)

    ok = True
    tag = f"oshi-hw-{random.randint(0, 1 << 30)}"
    A.sendText(tag)
    t0 = time.time()
    while time.time() - t0 < 60 and not any(tag.encode() in p for p in got(B)):
        time.sleep(0.5)
    text_ok = any(tag.encode() in p for p in got(B))
    print(f"{'PASS' if text_ok else 'FAIL'} interop text A->B on the primary channel ({time.time() - t0:.0f}s)", flush=True)
    ok &= text_ok

    msg_id = random.randint(1, 1 << 31)
    body = os.urandom(args.bytes)
    for f in frames(msg_id, b_num, body):
        A.sendData(f, destinationId=a_num, portNum=PRIVATE_APP)
    t0 = time.time()
    parts, states = {}, []
    while time.time() - t0 < 240:
        for p in got(B):
            if p[:3] == b"OS\x11":
                mid, _, _, idx, cnt, _ = struct.unpack("<IIIBBB", p[3:18])
                if mid == msg_id:
                    parts[idx] = (cnt, p[18:])
        states = [STATES.get(p[7], p[7]) for p in got(A) if p[:3] == b"OS\x16" and struct.unpack("<I", p[3:7])[0] == msg_id]
        if parts and len(parts) == next(iter(parts.values()))[0] and "DELIVERED" in states:
            break
        time.sleep(1)
    whole = b"".join(parts[i][1] for i in sorted(parts)) if parts and len(parts) == next(iter(parts.values()))[0] else None
    omp_ok = whole == body and "DELIVERED" in states
    print(f"{'PASS' if omp_ok else 'FAIL'} OMP {args.bytes}B A->B over the air ({time.time() - t0:.0f}s) "
          f"bytes_ok={whole == body} fragments={len(parts)} statuses={states}", flush=True)
    ok &= omp_ok
    A.close()
    B.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
