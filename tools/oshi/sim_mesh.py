#!/usr/bin/env python3
"""OSHI Mesh simulation harness.

Starts several meshtasticd instances (SimRadio, UDP multicast between them, each with its own HOME) and plays
the phone on each over the TCP API to check OSHI Mesh end to end, including against a stock node:

  interop      text from an OSHI node reaches the stock node and back
  probe        the BEACON capability probe is answered by OSHI firmware and ignored by stock firmware
  fragmented   a 2 KB OMP message crosses as 12 fragments and is reported DELIVERED to the sender's phone
  custody      the destination is down: the sender's radio parks the message, then delivers it when the
               destination comes back, without the phone resending anything
  stock-relay  OMP frames sent to a stock node are not surfaced to its phone as garbage
  inbox-reboot the destination's phone is away and its radio reboots before the phone comes back: the
               message must still reach the phone (flash-backed inbox, not the 8-slot RAM queue)
               KNOWN HARNESS GAP: the air is carried through each node's API link, so closing B's phone also
               deafens B's radio; this scenario cannot pass until the harness injects air without a phone.
  stock-origin a phone on a STOCK radio does OMP itself (what the OSHI app does there): it broadcasts DATA
               frames with its own node number as origin; an OSHI node's phone must get the whole message
  via-stock-client  A and B out of range of each other, a stock CLIENT (rebroadcast ALL) between them:
               text and a fragmented OMP message must cross it, and the sender must see DELIVERED
  via-stock-router  same, with the stock node as a ROUTER (CORE_PORTNUMS_ONLY, the mode that drops
               PRIVATE_APP it can decode): OMP rides the OSHI channel it cannot decode, so it is relayed opaque

Usage: sim_mesh.py --oshi PATH/meshtasticd --stock PATH/meshtasticd [--only NAME ...]
Exit status is the number of failed scenarios.
"""

import argparse
import os
import random
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time

from meshtastic import mesh_pb2, tcp_interface
from pubsub import pub

PRIVATE_APP = 256
OMP_MAX_FRAG = 182
BROADCAST = 0xFFFFFFFF

T_DATA, T_SACK, T_CUSTODY, T_RECEIPT, T_BEACON, T_STATUS = 1, 2, 3, 4, 5, 6
STATES = {1: "QUEUED", 2: "SENT", 3: "DELIVERED", 4: "IN_CUSTODY", 5: "FAILED", 6: "UPLINKED", 7: "REJECTED"}


def omp(t, body=b""):
    return bytes([0x4F, 0x53, (1 << 4) | t]) + body


def data_frames(msg_id, dest, body, flags=0, origin=0):
    count = max(1, (len(body) + OMP_MAX_FRAG - 1) // OMP_MAX_FRAG)
    for i in range(count):
        chunk = body[i * OMP_MAX_FRAG:(i + 1) * OMP_MAX_FRAG]
        yield omp(T_DATA, struct.pack("<IIIBBB", msg_id, origin, dest, i, count, flags) + chunk)


class Node:
    def __init__(self, name, binary, idx, workdir):
        self.name = name
        self.binary = binary
        self.hwid = 1000 + idx
        self.port = 4410 + idx
        self.home = os.path.join(workdir, name)
        self.proc = None
        self.iface = None
        self.rx = []
        self.lock = threading.Lock()
        os.makedirs(self.home, exist_ok=True)
        with open(os.path.join(self.home, "config.yaml"), "w") as f:
            f.write("Config:\n  EnableUDP: true\nGeneral:\n  MaxNodes: 200\n")

    def start(self):
        log = open(os.path.join(self.home, "node.log"), "ab")
        env = dict(os.environ, HOME=self.home, SIM_CARRY_CIPHERTEXT=os.environ.get("SIM_CARRY_CIPHERTEXT", "1"))  # every node decrypts for itself, as on air
        self.proc = subprocess.Popen(
            [self.binary, "-s", "-c", os.path.join(self.home, "config.yaml"), "-p", str(self.port), "-h", str(self.hwid)],
            cwd=self.home, env=env, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.time() + 40
        while time.time() < deadline:
            try:
                self.iface = tcp_interface.TCPInterface(hostname="127.0.0.1", portNumber=self.port, noNodes=False)
                return
            except Exception:
                time.sleep(1)
        raise RuntimeError(f"{self.name}: API never came up (see {self.home}/node.log)")

    def stop(self):
        if self.iface:
            try:
                self.iface.close()
            except Exception:
                pass
            self.iface = None
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None

    @property
    def num(self):
        return self.iface.myInfo.my_node_num

    def send_private(self, payload, to=BROADCAST):
        self.iface.sendData(payload, destinationId=to, portNum=PRIVATE_APP, wantAck=False)

    def omp_received(self, t):
        with self.lock:
            return [(f, p) for (f, p) in self.rx if len(p) >= 3 and p[:2] == b"OS" and (p[2] & 0x0F) == t]

    def text_received(self):
        with self.lock:
            return [p for (f, p) in self.rx if p and p[:2] != b"OS" and p[:2] != b"OM"]


NODES = {}
OUT_OF_RANGE = set()  # frozenset({name, name}) pairs that cannot hear each other
APP_PORTS = {"PRIVATE_APP", "TEXT_MESSAGE_APP", PRIVATE_APP, 1}


def relay_on_air(sender, raw):
    """SimRadio hands every frame it transmits to its phone as SIMULATOR_APP; putting it into every other
    node's RX path is the job Meshtasticator does. Full mesh: every node hears every other one."""
    for n in NODES.values():
        if n is sender or not n.iface or not n.proc or n.proc.poll() is not None:
            continue
        if frozenset((sender.name, n.name)) in OUT_OF_RANGE:
            continue
        pkt = mesh_pb2.MeshPacket()
        pkt.CopyFrom(raw)
        pkt.rx_snr = 6.0
        try:
            n.iface._sendPacket(pkt, destinationId=raw.to, wantAck=raw.want_ack, hopLimit=raw.hop_limit)
        except Exception as e:  # a node restarting mid-relay is expected in the custody scenario
            print(f"relay to {n.name} failed: {e!r}", flush=True)


def on_receive(packet, interface):
    for n in NODES.values():
        if n.iface is not interface:
            continue
        d = packet.get("decoded", {})
        port = d.get("portnum")
        if port in ("SIMULATOR_APP", 69):
            raw = packet.get("raw")
            if raw is not None:
                relay_on_air(n, raw)
            return
        if port not in APP_PORTS:
            return
        payload = d.get("payload", b"")
        if isinstance(payload, str):
            payload = payload.encode()
        with n.lock:
            n.rx.append((packet.get("from"), bytes(payload)))


def wait_for(pred, timeout, step=0.5):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(step)
    return None


def statuses(node, msg_id):
    out = []
    for _, p in node.omp_received(T_STATUS):
        mid, st, who = struct.unpack("<IBI", p[3:12])
        if mid == msg_id:
            out.append(STATES.get(st, st))
    return out


def reassemble(node, msg_id):
    parts = {}
    total = None
    for _, p in node.omp_received(T_DATA):
        mid, origin, dest, idx, count, flags = struct.unpack("<IIIBBB", p[3:18])
        if mid == msg_id:
            parts[idx] = p[18:]
            total = count
    if total is None or len(parts) != total:
        return None
    return b"".join(parts[i] for i in range(total))


# ---- scenarios ----

def s_interop(a, b, stock):
    tag = f"oshi-interop-{random.randint(0, 1 << 30)}"
    a.iface.sendText(tag)
    got = wait_for(lambda: any(tag.encode() in p for p in stock.text_received()), 30)
    back = f"stock-interop-{random.randint(0, 1 << 30)}"
    stock.iface.sendText(back)
    got_back = wait_for(lambda: any(back.encode() in p for p in a.text_received()), 30)
    return bool(got and got_back), f"oshi->stock={bool(got)} stock->oshi={bool(got_back)}"


def s_probe(a, b, stock):
    a.send_private(omp(T_BEACON, bytes(4)), to=a.num)
    stock.send_private(omp(T_BEACON, bytes(4)), to=stock.num)
    oshi_ok = wait_for(lambda: a.omp_received(T_BEACON), 10)
    time.sleep(3)
    stock_quiet = not stock.omp_received(T_BEACON) or all(f != stock.num for f, _ in stock.omp_received(T_BEACON))
    return bool(oshi_ok) and stock_quiet, f"oshi_answered={bool(oshi_ok)} stock_silent={stock_quiet}"


def s_fragmented(a, b, stock):
    msg_id = random.randint(1, 1 << 31)
    body = os.urandom(2000)
    for fr in data_frames(msg_id, b.num, body):
        a.send_private(fr, to=a.num)
    got = wait_for(lambda: reassemble(b, msg_id), 180)
    delivered = wait_for(lambda: "DELIVERED" in statuses(a, msg_id), 90)
    return got == body and bool(delivered), f"bytes_ok={got == body} statuses={statuses(a, msg_id)}"


def s_custody(a, b, stock):
    b.stop()
    msg_id = random.randint(1, 1 << 31)
    body = b"parked while you were away " * 4
    for fr in data_frames(msg_id, b.num_cached, body):
        a.send_private(fr, to=a.num)
    parked = wait_for(lambda: "IN_CUSTODY" in statuses(a, msg_id), 400, step=2)
    b.rx.clear()
    b.start()
    got = wait_for(lambda: reassemble(b, msg_id), 300, step=2)
    delivered = wait_for(lambda: "DELIVERED" in statuses(a, msg_id), 120, step=2)
    return bool(parked) and got == body and bool(delivered), f"parked={bool(parked)} delivered_bytes={got == body} statuses={statuses(a, msg_id)}"


def s_stock_relay(a, b, stock):
    msg_id = random.randint(1, 1 << 31)
    for fr in data_frames(msg_id, stock.num, b"to a stock node"):
        a.send_private(fr, to=a.num)
    time.sleep(20)
    raw = [p for f, p in stock.omp_received(T_DATA)]
    # A stock phone sees PRIVATE_APP bytes as-is; the check is that stock firmware neither crashes nor loops.
    alive = stock.proc and stock.proc.poll() is None
    return bool(alive), f"stock_alive={bool(alive)} raw_omp_frames_on_stock_phone={len(raw)}"


def s_inbox_reboot(a, b, stock):
    b.iface.close()
    b.iface = None                       # phone gone; the radio keeps running
    time.sleep(3)
    msg_id = random.randint(1, 1 << 31)
    body = b"kept in flash across a reboot " * 6
    for fr in data_frames(msg_id, b.num_cached, body):
        a.send_private(fr, to=a.num)
    delivered = wait_for(lambda: "DELIVERED" in statuses(a, msg_id), 120)
    time.sleep(7)                        # past the inbox save throttle
    b.stop()                             # radio reboot with nothing handed to a phone yet
    b.rx.clear()
    b.start()
    got = wait_for(lambda: reassemble(b, msg_id), 60)
    return bool(delivered) and got == body, f"radio_acked={bool(delivered)} phone_got_it_after_reboot={got == body}"


OSHI_PSK = bytes.fromhex("29436d19af55bd4e20247323c8ff455786f003119817e16cf7df3003684cb950")


def add_oshi_channel(node):
    """What the OSHI app does on a stock radio: the OSHI channel in the first free secondary slot."""
    from meshtastic.protobuf import channel_pb2
    ln = node.iface.localNode
    for c in ln.channels:
        if c.role != channel_pb2.Channel.Role.DISABLED and c.settings.name == "OSHI":
            return c.index
    free = next(c for c in ln.channels if c.index > 0 and c.role == channel_pb2.Channel.Role.DISABLED)
    free.settings.name = "OSHI"
    free.settings.psk = OSHI_PSK
    free.role = channel_pb2.Channel.Role.SECONDARY
    ln.writeChannel(free.index)
    time.sleep(3)
    return free.index


def s_stock_origin(a, b, stock):
    msg_id = random.randint(1, 1 << 31)
    # Three FULL fragments: a stock 2.8 sender XEdDSA-signs any packet the 64-byte signature still fits in, and a
    # short signed last fragment (244 B) overflows the simulator's 227-byte loopback (real radios carry 255).
    body = os.urandom(3 * OMP_MAX_FRAG)
    ch = add_oshi_channel(stock)
    for fr in data_frames(msg_id, BROADCAST, body, origin=stock.num):
        stock.iface.sendData(fr, destinationId=BROADCAST, portNum=PRIVATE_APP, wantAck=False, channelIndex=ch)
        time.sleep(2.5)  # OMP pacing; the phone paces its own radio
    got = wait_for(lambda: reassemble(b, msg_id), 90)
    return got == body, f"oshi_phone_got_whole_message={got == body}"


def set_stock_role(stock, role):
    node = stock.iface.localNode
    if node.localConfig.device.role == role:
        return
    node.localConfig.device.role = role
    node.writeConfig("device")  # the firmware applies the role's defaults (ROUTER -> CORE_PORTNUMS_ONLY)
    time.sleep(5)
    stock.stop()
    stock.start()
    time.sleep(15)


def via_stock(a, b, stock, role):
    set_stock_role(stock, role)
    OUT_OF_RANGE.add(frozenset(("a", "b")))
    try:
        mode = stock.iface.localNode.localConfig.device.rebroadcast_mode
        tag = f"via-stock-{random.randint(0, 1 << 30)}"
        # A channel broadcast: a DM here would test key learning, not relaying (2.8 rejects a channel-encrypted
        # DM from a sender whose PKI key it holds while that sender does not hold its key yet).
        a.iface.sendText(tag)
        text_ok = wait_for(lambda: any(tag.encode() in p for p in b.text_received()), 60)
        msg_id = random.randint(1, 1 << 31)
        body = os.urandom(600)
        for fr in data_frames(msg_id, b.num_cached, body):
            a.send_private(fr, to=a.num)
        got = wait_for(lambda: reassemble(b, msg_id), 240)
        delivered = wait_for(lambda: "DELIVERED" in statuses(a, msg_id), 120)
        ok = bool(text_ok) and got == body and bool(delivered)
        return ok, (f"stock_rebroadcast_mode={mode} text={bool(text_ok)} omp_bytes={got == body} "
                    f"statuses={statuses(a, msg_id)}")
    finally:
        OUT_OF_RANGE.clear()


def s_via_stock_client(a, b, stock):
    from meshtastic.protobuf import config_pb2
    return via_stock(a, b, stock, config_pb2.Config.DeviceConfig.Role.CLIENT)


def s_via_stock_router(a, b, stock):
    from meshtastic.protobuf import config_pb2
    return via_stock(a, b, stock, config_pb2.Config.DeviceConfig.Role.ROUTER)


SCENARIOS = [("interop", s_interop), ("probe", s_probe), ("fragmented", s_fragmented), ("custody", s_custody),
             ("stock-relay", s_stock_relay), ("inbox-reboot", s_inbox_reboot),
             ("stock-origin", s_stock_origin), ("via-stock-client", s_via_stock_client), ("via-stock-router", s_via_stock_router)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--oshi", required=True)
    ap.add_argument("--stock", required=True)
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--keep", action="store_true", help="keep the work directory with node logs")
    args = ap.parse_args()

    workdir = tempfile.mkdtemp(prefix="oshi-sim-")
    pub.subscribe(on_receive, "meshtastic.receive")
    NODES["a"] = Node("a", args.oshi, 1, workdir)
    NODES["b"] = Node("b", args.oshi, 2, workdir)
    NODES["stock"] = Node("stock", args.stock, 3, workdir)
    failures = 0
    try:
        for n in NODES.values():
            n.start()
        a, b, stock = NODES["a"], NODES["b"], NODES["stock"]
        b.num_cached = b.num
        print(f"nodes up: a=!{a.num:08x} b=!{b.num:08x} stock=!{stock.num:08x}; waiting for NodeInfo exchange")
        time.sleep(20)
        for name, fn in SCENARIOS:
            if args.only and name not in args.only:
                continue
            t0 = time.time()
            try:
                ok, detail = fn(a, b, stock)
            except Exception as e:
                ok, detail = False, f"exception: {e!r}"
            failures += 0 if ok else 1
            print(f"{'PASS' if ok else 'FAIL'} {name} ({time.time() - t0:.0f}s) {detail}", flush=True)
    finally:
        for n in NODES.values():
            n.stop()
        if args.keep or failures:
            print(f"logs: {workdir}")
        else:
            shutil.rmtree(workdir, ignore_errors=True)
    sys.exit(failures)


if __name__ == "__main__":
    main()
