#!/usr/bin/env python3
"""Drive the native simulator over the phone API, without the Meshtastic PyPI client.

    python3 tools/oshi/sim_client.py [--host 127.0.0.1] [--port 4403] [--timeout 30]

Replaces `python3 -c 'from meshtastic.test import testSimulator; testSimulator()'` in CI. That call
pulled in meshtastic==2.7.11, whose Requires-Python <3.15 cap decides which Python our whole CI may
run; owning the handshake means a competitor's release schedule no longer gates our build.

Protobufs come from bin/_generated/meshtastic_v25 (bin/regen-py-protos.sh, generated from the
in-tree protobufs/ submodule), so the wire format tracked is this tree's, not a published package's.

What it asserts, all observable on the wire and nothing guessed about radio behaviour:
  1. the stream API frames correctly and want_config_id is answered by a matching config_complete_id
  2. my_info arrives and carries a node number
  3. a text message to broadcast is accepted (frames out without the link dropping)
  4. AdminMessage.exit_simulator makes the process exit - which is what the CI step then verifies
"""

import argparse
import os
import secrets
import socket
import sys
import time

_REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(_REPO, "bin", "_generated"))
try:
    from meshtastic_v25 import admin_pb2, mesh_pb2, portnums_pb2
except ImportError:  # pragma: no cover - a clear message beats an opaque traceback
    sys.exit(
        "ERROR: in-tree protobuf bindings missing. Run bin/regen-py-protos.sh first."
    )

START1, START2 = 0x94, 0xC3
MAX_FRAME = 512
BROADCAST = 0xFFFFFFFF


class Stream:
    """The Meshtastic stream framing: 0x94 0xc3 then a big-endian uint16 length, then a protobuf.

    The radio also writes plain debug text to the same socket, so the reader resynchronises on the
    magic rather than trusting the next byte to start a frame.
    """

    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = bytearray()

    def send(self, to_radio):
        body = to_radio.SerializeToString()
        if len(body) > MAX_FRAME:
            raise ValueError(
                f"frame of {len(body)}B exceeds the {MAX_FRAME}B stream limit"
            )
        self.sock.sendall(
            bytes([START1, START2, len(body) >> 8, len(body) & 0xFF]) + body
        )

    def _fill(self):
        chunk = self.sock.recv(4096)
        if not chunk:
            raise ConnectionError("radio closed the stream")
        self.buf += chunk

    def recv(self, deadline):
        """Next FromRadio, or None once the deadline passes. Skips any non-frame bytes."""
        while True:
            i = self.buf.find(bytes([START1, START2]))
            if i < 0:
                if len(self.buf) > 1:
                    del self.buf[:-1]  # keep a possible split magic
            else:
                if i:
                    del self.buf[:i]
                if len(self.buf) >= 4:
                    n = (self.buf[2] << 8) | self.buf[3]
                    if n <= MAX_FRAME and len(self.buf) >= 4 + n:
                        body = bytes(self.buf[4 : 4 + n])
                        del self.buf[: 4 + n]
                        fr = mesh_pb2.FromRadio()
                        fr.ParseFromString(body)
                        return fr
                    if n > MAX_FRAME:
                        del self.buf[:2]  # not a real header; resync past it
                        continue
            if time.time() > deadline:
                return None
            try:
                self._fill()
            except socket.timeout:
                return None

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def data_packet(portnum, payload, to, want_ack=False):
    pkt = mesh_pb2.MeshPacket(to=to, id=secrets.randbits(32), want_ack=want_ack)
    pkt.decoded.portnum = portnum
    pkt.decoded.payload = payload
    return mesh_pb2.ToRadio(packet=pkt)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4403)
    ap.add_argument("--timeout", type=float, default=30.0)
    a = ap.parse_args()

    s = Stream(a.host, a.port, a.timeout)
    want = secrets.randbits(31) or 1
    s.send(mesh_pb2.ToRadio(want_config_id=want))

    my_num, seen, completed = None, [], False
    deadline = time.time() + a.timeout
    while time.time() < deadline:
        fr = s.recv(deadline)
        if fr is None:
            break
        which = fr.WhichOneof("payload_variant")
        seen.append(which)
        if which == "my_info":
            my_num = fr.my_info.my_node_num
        elif which == "config_complete_id":
            if fr.config_complete_id != want:
                sys.exit(
                    f"FAIL config_complete_id {fr.config_complete_id} != want_config_id {want}"
                )
            completed = True
            break

    if not completed:
        s.close()
        sys.exit(
            f"FAIL no config_complete_id within {a.timeout}s (saw: {', '.join(seen) or 'nothing'})"
        )
    if not my_num:
        s.close()
        sys.exit(
            f"FAIL config completed without a node number (saw: {', '.join(seen)})"
        )
    print(
        f"ok handshake: node !{my_num:08x}, {len(seen)} frames ({len(set(seen))} kinds)"
    )

    s.send(
        data_packet(
            portnums_pb2.PortNum.TEXT_MESSAGE_APP, b"OSHI sim_client", BROADCAST
        )
    )
    print("ok text message accepted")

    admin = admin_pb2.AdminMessage(exit_simulator=True)
    s.send(
        data_packet(portnums_pb2.PortNum.ADMIN_APP, admin.SerializeToString(), my_num)
    )
    print("ok exit_simulator sent")
    s.close()


if __name__ == "__main__":
    main()
