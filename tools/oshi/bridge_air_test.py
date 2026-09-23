#!/usr/bin/env python3
"""Meshtastic <-> MeshCore bridge test with two MeshCore radios on USB and a simulated Meshtastic side.

    Meshtastic side (simulated, real firmware):  OSHI Mesh node A  <-air->  stock node BR (the bridge's radio)
    Bridge:                                      oshi-meshcore-bridge's OshiBridgeCore, fed by BR's phone API
    MeshCore side (real radios, over the air):   radio R1 (the bridge's MeshCore radio)  <-air->  radio R2 (a far
                                                 OSHI user on MeshCore, played by this script)

  1. A's phone sends an OSHI message; it must reach R2 byte for byte (Meshtastic -> bridge -> MeshCore air).
  2. R2 sends an OSHI message; A's phone must receive it byte for byte (MeshCore air -> bridge -> Meshtastic).

    python3 tools/oshi/bridge_air_test.py --oshi PATH/meshtasticd --stock PATH/meshtasticd \
        --r1 /dev/cu.usbserial-X --r2 /dev/cu.usbserial-Y [--bridge ~/oshi-meshcore-bridge] [--protocol ~/oshi-mesh-protocol]
"""

import argparse
import os
import queue
import random
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--oshi", required=True)
    ap.add_argument("--stock", required=True)
    ap.add_argument("--r1", required=True)
    ap.add_argument("--r2", required=True)
    ap.add_argument("--bridge", default=os.path.expanduser("~/oshi-meshcore-bridge"))
    ap.add_argument("--protocol", default=os.path.expanduser("~/oshi-mesh-protocol"))
    ap.add_argument("--wait", type=float, default=180)
    a = ap.parse_args()

    sys.path[:0] = [HERE, a.bridge, os.path.join(a.protocol, "tools"), os.path.join(a.protocol, "python")]
    import sim_mesh as sm
    from pubsub import pub
    from oshi_bridge.core import OshiBridgeCore, BridgeSettings
    from oshi_omp import omp as romp, wire as rwire
    from meshcore_air_test import Companion

    # ---- MeshCore side: two real radios ----
    r1, r2 = Companion(a.r1, "R1(bridge)"), Companion(a.r2, "R2(far user)")
    r1.setup()
    r2.setup()
    list(r1.drain(2))
    list(r2.drain(2))

    # ---- Meshtastic side: simulated A (OSHI) and BR (stock) ----
    workdir = tempfile.mkdtemp(prefix="oshi-bridge-air-")
    pub.subscribe(sm.on_receive, "meshtastic.receive")
    sm.NODES["a"] = sm.Node("a", a.oshi, 1, workdir)
    sm.NODES["br"] = sm.Node("br", a.stock, 2, workdir)
    A, BR = sm.NODES["a"], sm.NODES["br"]
    for n in (A, BR):
        n.start()
    ch = sm.add_oshi_channel(BR)
    print(f"sim: A=!{A.num:08x} (OSHI Mesh)  BR=!{BR.num:08x} (stock, OSHI channel slot {ch})", flush=True)

    inbox = queue.Queue()  # BR's phone -> bridge core, handed over to the main thread

    def br_rx(packet, interface):
        if interface is not BR.iface:
            return
        d = packet.get("decoded", {})
        if d.get("portnum") not in ("PRIVATE_APP", 256):
            return
        payload = d.get("payload", b"")
        inbox.put((packet.get("from", 0), packet.get("to", 0xFFFFFFFF), bytes(payload),
                   bool(packet.get("pkiEncrypted"))))

    pub.subscribe(br_rx, "meshtastic.receive")

    def mesh_send(payload, to):
        BR.iface.sendData(payload, destinationId=to, portNum=256, wantAck=False, channelIndex=ch)

    core = OshiBridgeCore(BR.num, mesh_send, r1.send_datagram, BridgeSettings())
    time.sleep(15)  # NodeInfo exchange on the simulated side

    def step(r2_sink=None, dt=0.5):
        while not inbox.empty():
            frm, to, payload, pki = inbox.get()
            core.on_mesh_packet(frm, to, 256, payload, pki)
        core.pump()
        for dg in r1.drain(dt):
            core.on_mc_datagram(dg)
        if r2_sink is not None:
            for dg in r2.drain(dt):
                r2_sink(dg)

    results = []

    # 1. Meshtastic -> MeshCore. Three full fragments: stock 2.8 signs short packets, and a short signed last
    #    fragment overflows the simulator's loopback (real radios carry it).
    msg1 = random.randint(1, 0xFFFFFFFF)
    body1 = os.urandom(3 * sm.OMP_MAX_FRAG)
    for fr in sm.data_frames(msg1, sm.BROADCAST, body1):
        A.send_private(fr, to=A.num)
    joiner, got = rwire.Joiner(), {}

    def r2_sink(dg):
        part = rwire.parse(dg)
        if part is None or part.sender == r2.node_id:
            return
        joined = joiner.push(part)
        f = romp.decode(joined) if joined else None
        if isinstance(f, romp.DataFrame) and f.msg_id == msg1:
            if f.idx not in got:
                print(f"  R2 got fragment {f.idx + 1}/{f.count} (origin !{f.origin:08x})", flush=True)
            got[f.idx] = f.data

    t0 = time.time()
    while time.time() - t0 < a.wait and len(got) < 3:
        step(r2_sink)
    out1 = b"".join(got[i] for i in sorted(got)) if len(got) == 3 else None
    results.append(("Meshtastic -> MeshCore", out1 == body1, f"{len(got)}/3 fragments in {time.time() - t0:.0f} s"))
    print(("PASS" if out1 == body1 else "FAIL"), results[-1][0], results[-1][2], flush=True)

    # 2. MeshCore -> Meshtastic.
    msg2 = random.randint(1, 0xFFFFFFFF)
    body2 = os.urandom(3 * sm.OMP_MAX_FRAG)
    for seq, fr in enumerate(romp.fragment(msg2, r2.node_id, romp.DEST_BROADCAST, body2)):
        for dg in rwire.split(fr.encode(), r2.node_id, (200 + seq) & 0xFF):
            r2.send_datagram(dg)
            end = time.time() + 2.5
            while time.time() < end:
                step()
    t0 = time.time()
    got2 = None
    while time.time() - t0 < a.wait and got2 is None:
        step()
        got2 = sm.reassemble(A, msg2)
    results.append(("MeshCore -> Meshtastic", got2 == body2, f"{'whole message' if got2 else 'nothing'} at A's phone after {time.time() - t0:.0f} s"))
    print(("PASS" if got2 == body2 else "FAIL"), results[-1][0], results[-1][2], flush=True)

    print("bridge stats:", {k: v for k, v in core.stats.items() if v}, flush=True)
    for n in sm.NODES.values():
        n.stop()
    sys.exit(0 if all(ok for _, ok, _ in results) else 1)


if __name__ == "__main__":
    main()
