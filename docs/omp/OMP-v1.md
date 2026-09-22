# OSHI Mesh Protocol (OMP), version 1

Status: implemented in this repository (`src/oshi/`, `src/modules/OshiModule.cpp`). This document describes that
implementation; where the two disagree, the code is authoritative and the document is a bug.

OMP adds reliable, fragmented, custody-backed messaging to a Meshtastic mesh. It is carried inside ordinary Meshtastic
packets, so nodes that do not implement it relay it without interpreting it.

The key words MUST, SHOULD and MAY are used as in RFC 2119.

## Contents

1. [Conventions](#1-conventions)
2. [Transport](#2-transport)
3. [Frame prefix and types](#3-frame-prefix-and-types)
4. [Frames](#4-frames)
5. [Sending](#5-sending)
6. [Receiving](#6-receiving)
7. [Custody](#7-custody)
8. [Peers and beacons](#8-peers-and-beacons)
9. [Phone to radio protocol](#9-phone-to-radio-protocol)
10. [Gateway HTTP API](#10-gateway-http-api)
11. [Security considerations](#11-security-considerations)
12. [Constants](#12-constants)

## 1. Conventions

- All integers are unsigned and **little-endian**.
- *Node number*: the 32-bit Meshtastic node number (`NodeNum`).
- *Message*: an application payload (the *body*) of 0 to 11,648 bytes, identified by the pair **(origin, msgId)**.
  `origin` is the node number of the radio that first sent it; `msgId` is a 32-bit value chosen by the origin (the OSHI apps
  pick it at random). A sender MUST NOT reuse a `msgId` for a different body while the earlier message may still be in
  flight or in custody (up to 72 h).
- *Link recipient*: the node a frame is addressed to on this hop of OMP: the destination, a custodian or a gateway.
- *Frame*: one OMP unit, the complete payload of one Meshtastic packet.

## 2. Transport

| Item | Value |
| --- | --- |
| Meshtastic portnum | `PRIVATE_APP` = **256** |
| Channel | secondary channel named **`OSHI`**, PSK = SHA-256(`"OSHI Mesh channel v1"`) (32 bytes, AES-256), uplink and downlink to MQTT disabled |
| PSK (hex) | `29436d19af55bd4e20247323c8ff455786f003119817e16cf7df3003684cb950` |
| Maximum frame | **200 bytes** (`OMP_MAX_FRAME`) |
| Priority | `RELIABLE` (70) |

**Channel.** At first boot the firmware adds the `OSHI` channel in the first disabled slot with index 1 to 7 (the primary
channel, index 0, is never replaced). If a channel named `OSHI` already exists it is reused. If no slot is free, OMP is sent
on the primary channel instead. In licensed (ham) mode no channel is created, because encryption is not allowed.

The channel is not there for secrecy: its key is published above. It exists so that stock Meshtastic ROUTERs, whose
default `rebroadcast_mode` is `CORE_PORTNUMS_ONLY`, see OMP as an undecodable packet and relay it opaquely. On a channel they
can decrypt they would drop `PRIVATE_APP` (upstream `Router.cpp`, `shouldIgnoreNonstandardPorts`).

**Addressing at the Meshtastic layer.** For each frame the sender picks the packet's `to`:

| Condition | Meshtastic `to` | Encryption | `want_ack` |
| --- | --- | --- | --- |
| Link recipient is a node whose public key the sender holds (and PKI is available: 32-byte private key, not licensed) | the link recipient | Meshtastic PKI (X25519 + AES-CCM) | set on the last frame of a round for unicast DATA, on CUSTODY/RECEIPT notices, and on PULL |
| Otherwise, including broadcast | `0xFFFFFFFF` (broadcast) | `OSHI` channel PSK | not set |

When a frame is broadcast only because the key is unknown, the OMP header still carries the real destination, and the
receivers filter on it (section 6).

Frames that ask a specific node to act for the sender (a custody request, an internet uplink, a PULL) are acted on **only
when they arrive addressed to that node at the Meshtastic layer** (`to` = the receiver). In practice this means the sender
must hold the receiver's public key. End-to-end DATA to a destination works either way.

Meshtastic flooding or next-hop routing carries each frame to its link recipient. OMP itself adds no routing.

## 3. Frame prefix and types

Every OMP frame starts with a 3-byte prefix:

| Offset | Size | Field | Value |
| --- | --- | --- | --- |
| 0 | 1 | magic0 | `0x4F` (`'O'`) |
| 1 | 1 | magic1 | `0x53` (`'S'`) |
| 2 | 1 | version (high nibble), type (low nibble) | `(1 << 4) \| type` |

A receiver MUST ignore a payload whose magic is not `OS`, whose version nibble is not 1, or whose type is outside 1-7.
The magic keeps OMP apart from any other use of `PRIVATE_APP` (the older OSHI app framing uses `OM`).

| Type | Name | Size | Sent over the air | Purpose |
| --- | --- | --- | --- | --- |
| 1 | DATA | 18 + 0..182 | yes | one fragment of a message |
| 2 | SACK | 20 | yes | selective acknowledgement bitmap |
| 3 | CUSTODY | 15 | yes | a custodian tells the origin it now holds the message |
| 4 | RECEIPT | 15 | yes | a custodian tells the origin the message was delivered |
| 5 | BEACON | 7 | yes | capabilities; also the phone's capability probe |
| 6 | STATUS | 12 | **no**, radio to its own phone only | delivery state of a message |
| 7 | PULL | 79 | yes | signed request for a node's internet mail |

## 4. Frames

### 4.1 DATA (type 1)

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 3 | prefix | `4F 53 11` |
| 3 | 4 | msgId | |
| 7 | 4 | origin | node number of the original sender; 0 when sent by a phone to its radio (section 9) |
| 11 | 4 | dest | destination node number, or `0xFFFFFFFF` broadcast, or `0xFFFFFFF0` internet |
| 15 | 1 | idx | fragment index, 0-based, `idx < count` |
| 16 | 1 | count | number of fragments, 1-64 |
| 17 | 1 | flags | see below |
| 18 | 0-182 | data | fragment bytes |

Header: 18 bytes (`OMP_DATA_HEADER`). Fragment data: at most **182 bytes** (`OMP_MAX_FRAG_DATA` = 200 - 18).

Fragmentation: `count = max(1, ceil(len / 182))`; fragment `i` carries body bytes `[182*i, min(182*(i+1), len))`.
**Every fragment except the last MUST carry exactly 182 bytes**; receivers reject any other size, so the assembled length is
unambiguous. An empty body is one fragment with no data. The largest message is 64 x 182 = **11,648 bytes**
(`OMP_MAX_MESSAGE`); a larger body cannot be sent.

Special destinations:

| Value | Name | Meaning |
| --- | --- | --- |
| `0xFFFFFFFF` | `OMP_DEST_BROADCAST` | every OMP node; no acknowledgement, no custody |
| `0xFFFFFFF0` | `OMP_DEST_INTERNET` | the internet side; the body is a `U` frame (section 10.3), carried to a gateway node |

Flags:

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 (`0x01`) | `FLAG_CUSTODY_OK` | the origin allows the message to be handed to a custodian |
| 1 (`0x02`) | `FLAG_VIA_CUSTODY` | sent by a node on the origin's behalf (a custodian, or a gateway delivering internet mail) |
| 2 (`0x04`) | `FLAG_CUSTODY_REQ` | addressed to a custodian for holding, not for delivery |
| 3-7 | | reserved, send 0 |

A decoder MUST reject a DATA frame shorter than 18 bytes, longer than 200 bytes, with `count` 0 or above 64, or with
`idx >= count`.

### 4.2 SACK (type 2)

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 3 | prefix | `4F 53 12` |
| 3 | 4 | msgId | of the message being acknowledged |
| 7 | 4 | origin | of the message being acknowledged (not the sender of the SACK) |
| 11 | 1 | count | fragment count of the message, 1-64 |
| 12 | 8 | bitmap | bit `i` set = fragment `i` received; bits at and above `count` MUST be 0 on send and are masked off on receive |

Length: 20 bytes. A SACK with every bit `0..count-1` set means the message is complete at the SACK's sender.

### 4.3 CUSTODY (type 3) and RECEIPT (type 4)

Both share one layout:

| Offset | Size | Field | CUSTODY | RECEIPT |
| --- | --- | --- | --- | --- |
| 0 | 3 | prefix | `4F 53 13` | `4F 53 14` |
| 3 | 4 | msgId | message held | message delivered |
| 7 | 4 | origin | origin of the message (the receiver of this notice) | same |
| 11 | 4 | dest | the message's destination | the node that confirmed delivery (the destination) |

Length: 15 bytes. Both are sent by a custodian to the origin.

### 4.4 BEACON (type 5)

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 3 | prefix | `4F 53 15` |
| 3 | 1 | caps | bit 0 `CAP_CUSTODIAN`, bit 1 `CAP_GATEWAY_ONLINE`, others reserved |
| 4 | 2 | version | implementation version, `(OMP_VERSION << 8) \| minor`; this firmware sends `0x0100` |
| 6 | 1 | custodyFreeKb | free custody space in KiB, capped at 255 |

Length: 7 bytes. `CAP_CUSTODIAN` is set when at least 2 KiB of custody space is free; `CAP_GATEWAY_ONLINE` when the node's
own internet link is up (section 10).

### 4.5 STATUS (type 6), radio to phone only

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 3 | prefix | `4F 53 16` |
| 3 | 4 | msgId | a message the phone submitted |
| 7 | 1 | state | see table |
| 8 | 4 | node | node the state refers to, 0 when none |

Length: 12 bytes. Never transmitted; a radio ignores a STATUS received over the air. A decoder MUST reject a `state` outside
1-7.

| Value | State | Meaning | `node` |
| --- | --- | --- | --- |
| 1 | `QUEUED` | accepted by the radio's outbox | 0 |
| 2 | `SENT` | every fragment transmitted once (for a broadcast, the final state) | 0 |
| 3 | `DELIVERED` | the destination holds the complete message | destination |
| 4 | `IN_CUSTODY` | held by a custodian, or parked in this radio's flash | custodian, or the radio itself |
| 5 | `FAILED` | given up: no gateway, rounds exhausted with no custody possible, or 72 h in custody | 0 |
| 6 | `UPLINKED` | handed to a gateway for the internet (not yet confirmed by the server) | gateway node |
| 7 | `REJECTED` | refused: body too large, outbox full, or local gateway queue full | 0, or the radio itself |

A message may pass through `IN_CUSTODY` and later reach `DELIVERED` (through a RECEIPT, or directly when a parked message
is retried). `SENT` is reported again each time a parked message is retried.

### 4.6 PULL (type 7)

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 3 | prefix | `4F 53 17` |
| 3 | 4 | nodeNum | the node asking for its mail |
| 7 | 4 | afterSeq | return mail with relay sequence number greater than this |
| 11 | 4 | tsSec | Unix time, seconds |
| 15 | 64 | sig | XEdDSA signature, see below |

Length: exactly 79 bytes; a decoder MUST reject any other length.

The signature is XEdDSA (Signal's XEdDSA over Curve25519, as in Meshtastic's `CryptoEngine::xeddsa_sign`) made with the
node's Meshtastic private key, over this 20-byte message:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | nodeNum |
| 4 | 4 | tsSec |
| 8 | 4 | `OMP_PULL_SIGN_PORT` = `0x4F505531` (bytes `31 55 50 4F`), a domain separator |
| 12 | 4 | gateway: node number of the gateway the PULL is sent through |
| 16 | 4 | afterSeq |

This is the Meshtastic signing buffer `fromNode | packetId | portnum | payload` with `packetId = tsSec`,
`portnum = OMP_PULL_SIGN_PORT` and an 8-byte payload `gateway | afterSeq`. Binding the gateway into the signature stops a PULL
being replayed through a different gateway. The verification key is the Ed25519 point derived from the node's Curve25519
public key with sign bit 0.

## 5. Sending

### 5.1 Outbox

A sender keeps an outbox of at most **16** live messages. Submitting a message whose `(origin, msgId)` is already live is a
no-op. A message is refused (`REJECTED`) when its body exceeds 11,648 bytes or the outbox is full.

For `dest = OMP_DEST_INTERNET` the link recipient is the nearest known gateway (section 8). With none, the message fails
at once (`FAILED`). Otherwise the link recipient is `dest`.

### 5.2 Pacing

The sender transmits **at most one OMP frame every 2,500 ms** in total, across all messages, taking messages in round-robin
order and, within a message, the lowest-numbered fragment still pending. In regions with a duty cycle below 100%, OMP frames
are only sent while this node's hourly TX time is below half of the duty cycle.

### 5.3 Rounds (unicast)

```
            enqueue                          last fragment sent
  (QUEUED) ---------> SENDING -----------------------------------> AWAIT_ACK
                        ^                                            |   |
                        |  partial SACK, rounds left: resend missing |   | timeout, rounds left:
                        +--------------------------------------------+   | resend last fragment (poll)
                        +------------------------------------------------+
  AWAIT_ACK or SENDING --- full SACK ---> DONE (DELIVERED / IN_CUSTODY / UPLINKED)
  AWAIT_ACK --- 5th round ends without full SACK ---> rounds exhausted (5.4)
```

- **Round 0** sends every fragment. After its last fragment the sender reports `SENT` (except on a hand-off to a custodian)
  and waits for a SACK until
  `deadline = now + 25,000 ms + 3,000 ms x count`.
- A **SACK** is matched on msgId, origin and count, and only from the current link recipient. Its bitmap is OR-ed into the
  set of acknowledged fragments. Fragments acknowledged while a round is still sending are removed from that round.
- **Complete SACK**: the message is done: `DELIVERED` (node = SACK sender), or `IN_CUSTODY` when the link recipient is a
  custodian, or `UPLINKED` when the destination is the internet.
- **Partial SACK while waiting**: the next round starts and resends only the missing fragments.
- **Timeout**: the next round starts and resends **only the last fragment**. A receiver answers every last fragment with its
  bitmap (section 6.2), so this polls for the current state without resending the message.
- At most **5 rounds** (round 0 plus 4 repair or poll rounds). When the fifth ends without a complete SACK, the rounds are
  exhausted.

With `count = 1` the wait per round is 28 s, so a message to a silent destination is handed on after about 2.5 minutes.

### 5.4 When the rounds are exhausted

In order:

1. **Hand-off to a custodian** if the message is not already going to one, `FLAG_CUSTODY_OK` is set,
   `FLAG_VIA_CUSTODY` is not, and a custodian is known (section 8) that is neither the current link recipient nor the sender.
   The message restarts at round 0 with link recipient = the custodian and `FLAG_CUSTODY_REQ` set on the air.
2. Otherwise, **self-custody** if the destination is a node (not broadcast, not internet): the message is *parked* in the
   sender's flash and `IN_CUSTODY` is reported with node = the sender itself.
3. Otherwise **`FAILED`**.

`FLAG_CUSTODY_REQ` is never taken from the submitted flags: it is only ever set by the sender for a hand-off.

### 5.5 Parked messages

A parked message is retried, from round 0 straight to its destination, **whenever any packet from the destination is heard**
(OMP or not), at most once every **60 s**. If the retry's rounds are exhausted again, 5.4 applies again. A parked message
fails after **72 h**. Parked messages are stored in flash (`/oshi/custody.bin`) and restored at boot; the 72 h period
restarts at boot.

### 5.6 Broadcast

A message to `OMP_DEST_BROADCAST` is sent once, fragment by fragment, and reported `SENT`. Receivers do not acknowledge it
and it is never handed to custody.

## 6. Receiving

### 6.1 Acceptance

A node processes an OMP frame only if the Meshtastic packet is addressed to it or broadcast, and ignores frames from itself.
For DATA:

| Condition | Action |
| --- | --- |
| `origin` = this node | ignore |
| `dest` = this node, or `dest` = broadcast | receive for delivery to the phone |
| `dest` = internet, packet addressed to this node, own gateway online | receive for uplink |
| `FLAG_CUSTODY_REQ`, packet addressed to this node, custody space free | receive for custody |
| anything else | ignore |

### 6.2 Reassembly and SACK

- A receiver keeps the `(origin, msgId)` of the last **64** completed messages. A fragment of such a message is not
  delivered again; if the message is unicast (`dest` not broadcast) and the fragment is the last one, the receiver answers
  with a complete SACK.
- Reassembly limits: **6** messages in progress, **24 KiB** of fragment data in total, **5 minutes** per message from its
  first fragment. When full, the oldest message in progress is dropped.
- A fragment is rejected if it breaks 4.1, if a non-last fragment is not 182 bytes, or if its `count` or `dest` differs from
  earlier fragments of the same message. A repeated fragment is ignored.
- For a unicast message, the receiver sends a SACK with its current bitmap to the node the frame came from (the origin, a
  custodian or a gateway) when it receives the **last fragment** (`idx = count - 1`), and when the message becomes complete.
  It does not acknowledge other fragments.
- Broadcast messages are never acknowledged.
- The flags of all fragments are OR-ed together.

### 6.3 Control frames

| Frame | Accepted when | Effect |
| --- | --- | --- |
| SACK | trusted (below) | section 5.3 |
| CUSTODY | `origin` = this node, trusted, and the message is currently being handed to that sender as custodian | message done, `IN_CUSTODY` (node = custodian) |
| RECEIPT | `origin` = this node, trusted | `DELIVERED` to the phone, node = `dest` field |
| BEACON | always | peer table update (section 8) |
| PULL | addressed to this node, own gateway online, `nodeNum` = packet sender | forwarded to the server (section 10.4) |
| STATUS | never | ignored over the air |

*Trusted*: if this node holds the sender's public key, the frame MUST have arrived PKI-encrypted. Otherwise anyone holding
the public `OSHI` PSK could forge a SACK claiming to come from that node.

## 7. Custody

A **custodian** is an OMP node that holds a message for an origin and delivers it when the destination is reachable.

1. The origin, after its rounds are exhausted, sends the message to the custodian with `FLAG_CUSTODY_REQ` (section 5.4).
2. The custodian reassembles it like any unicast message and SACKs it. When complete it replaces `FLAG_CUSTODY_REQ` with
   `FLAG_VIA_CUSTODY`, stores it in flash (a **32 KiB** store, shared with the custodian's own parked messages), sends **CUSTODY** to the origin (`dest` = the message's
   destination) and queues it in its own outbox with the original `origin` and `msgId`.
3. The origin marks the message `IN_CUSTODY` on the complete SACK or on the CUSTODY notice, whichever comes first, and drops
   it from its outbox.
4. The custodian delivers it with the normal rounds. Because `FLAG_VIA_CUSTODY` is set, it never hands the message to a
   second custodian; if its rounds are exhausted it parks it (section 5.5).
5. When the destination's SACK is complete, the custodian deletes its copy and sends **RECEIPT** to the origin
   (`dest` field = the destination). The origin reports `DELIVERED` to its phone.

A custodian that finally gives up (72 h) deletes the message without telling the origin.

Custodian choice: among peers with `CAP_CUSTODIAN` whose beacon is fresh (heard in the last 60 minutes), whose advertised free
space is at least the body size, and which are not the destination: the fewest hops away, then the most free space.

## 8. Peers and beacons

- An OMP node broadcasts a BEACON 45 s after boot, then every **15 minutes**, and only while channel utilisation is below
  25%.
- Receivers record each beacon's sender, caps, free space and hop distance (`hop_start - hop_limit` of the Meshtastic packet)
  in a peer table of 32 entries (the least recently heard is replaced). An entry is fresh for **60 minutes**.
- Gateway choice: among fresh peers with `CAP_GATEWAY_ONLINE`, the fewest hops away.

## 9. Phone to radio protocol

A phone (or any API client: BLE, serial, TCP) talks OMP to its own radio through the normal Meshtastic API, with
`MeshPacket.decoded.portnum = 256` and the OMP frame as the payload. The radio consumes every OMP frame the phone sends; none
is transmitted as-is. Use the radio's own node number as `to`.

### 9.1 Capability probe

The phone sends a BEACON (`4F 53 15 00 00 00 00`; the content is ignored) to its radio. OMP firmware answers with its own
BEACON, `from` = the radio's node number. Stock firmware never echoes a phone packet back, so no answer within a timeout of
the phone's choosing means the radio does not speak OMP.

### 9.2 Sending a message

- The phone sends the message as DATA fragments (4.1) with `origin = 0`. The radio sets `origin` to its own node number.
- Only `FLAG_CUSTODY_OK` is honoured from the phone; other flags are cleared.
- The radio reassembles by `msgId` with the limits of 6.2 and queues the complete message (section 5). A message whose `dest`
  is the radio itself is dropped.
- For `dest = OMP_DEST_INTERNET` on a radio whose own gateway is online, the message goes straight to the gateway queue and
  the radio answers `UPLINKED` or `REJECTED` (node = the radio).
- The radio then reports progress with STATUS frames (4.5), `from` = the radio's node number. Frames for the phone use the
  `OSHI` channel index.

### 9.3 Receiving a message

- When a message for this node (or a broadcast) is complete, the radio stores it in a flash inbox (**16 KiB**,
  `/oshi/inbox.bin`, written at most every 5 s) and passes it to the phone as DATA fragments with the same layout as on air:
  `from` = the message's origin, `origin` field = the origin, flags as received.
- While no client is connected, frames for the phone wait in a RAM buffer (24 KiB, oldest dropped first) instead of the
  firmware's shared phone queue. They are passed on while that queue has more than 3 free slots, so ordinary traffic is not
  crowded out.
- A message is removed from the inbox when its last fragment enters the phone queue. Messages still in the inbox at boot are
  passed to the phone again, so **the phone MUST deduplicate on (origin, msgId)**.

### 9.4 Body encoding used by the OSHI apps

OMP carries opaque bytes. The OSHI apps put a one-byte tag at the start of the body:

| First byte | Body |
| --- | --- |
| `0x01` | raw: the rest is the app's JSON envelope as-is |
| `0x02` | the JSON envelope compressed with **raw DEFLATE** (RFC 1951, no zlib or gzip header) |
| `0x55` (`'U'`) | an internet uplink frame (10.3), used with `dest = OMP_DEST_INTERNET` |
| `0x44` (`'D'`) | an internet downlink frame (10.3), delivered by a gateway with `FLAG_VIA_CUSTODY` |

The apps send whichever of `0x01` or `0x02` is shorter. Decoders SHOULD cap the inflated size (the Android app stops at
8 x 11,648 bytes). The envelope's contents are end-to-end encrypted by the app and are outside this specification.
The firmware reads the body in one case only: a `D` frame, to update the downlink cursor (10.4).

## 10. Gateway HTTP API

A **gateway** is an OMP node with an internet link (today: an ESP32 with WiFi enabled). It is untrusted: it only carries
self-authenticating frames, so a hostile gateway can drop traffic but cannot forge a message or read anyone's mail.

The firmware's server base URL is `https://oshi-messenger.com/v2/mesh` (build flag `OSHI_GATEWAY_URL`). All requests are
`POST` with a JSON body of at most 64 KiB and `Content-Type: application/json`; binary fields are standard base64.

> Status: the server side is implemented but these routes are not yet deployed on the production server, and the gateway has
> not been tested against it.

### 10.1 Gateway behaviour

- The gateway's internet link is *online* while WiFi is connected and it is not backing off.
- Up to 8 jobs are queued. Each request has a 10 s timeout. A network error, HTTP 429 or 5xx keeps an uplink job for retry
  (a PULL job is dropped) and pauses the queue for 30 s. Any other non-200 answer drops the job.
- A gateway pulls on its own behalf too: it is simply a node whose PULL does not need the radio.

### 10.2 `POST /bind` and `DELETE /bind`

Links an OSHI account to a radio so that mail for the account can be pulled by that radio. Required for downlink only.
Authenticated with the OSHI v2 request signature: headers `x-oshi-signing-pubkey` (base64 Ed25519 key bound to the
account), `x-oshi-timestamp` (Unix ms, within 30 s) and `x-oshi-signature` (base64 Ed25519 signature over
`METHOD + "\n" + PATH + "\n" + hex(SHA-256(raw body)) + "\n" + TIMESTAMP`). Unsigned requests are always refused.

| Method | Body | 200 response |
| --- | --- | --- |
| `POST` | `{"userKey": "<account identity>", "nodeNum": <1..0xFFFFFFFE>, "nodePub": "<base64 32-byte Curve25519 node public key>"}` | `{"bound": true, "nodeNum": N}` |
| `DELETE` | `{"userKey": "<account identity>"}` | `{"unbound": 0 or 1}` |

A radio serves one account: a new signed claim on a node number replaces the previous one. Errors: 400 (`bad-json`,
`missing-userKey`, `bad-nodeNum`, `bad-nodePub`), 401 (`unauthorized`), 403 (signer is not `userKey`), 503.

### 10.3 `U` and `D` frames

Common envelope fields, in this order (`str8` = u8 length + UTF-8, `blob16` = u16 length + bytes):

| Field | Encoding |
| --- | --- |
| type | u8: 0 = one-to-one, 1 = group |
| msgId | str8 |
| groupId | str8 (empty for one-to-one) |
| header | blob16 |
| ciphertext | blob16 |
| x3dh | blob16: JSON, empty when absent |

`U` (uplink, built and signed by the sending app):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `0x55` |
| 1 | 1 | frame version = 1 |
| 2 | 1 | envelope version |
| 3 | 32 | from (sender identity key) |
| 35 | 32 | signingKey (sender's Ed25519 key, must be the one bound to `from`) |
| 67 | 32 | to (recipient identity key) |
| 99 | 8 | tsMs (Unix ms) |
| 107 | var | envelope fields |
| end - 64 | 64 | Ed25519 signature by `signingKey` over every byte before it |

`D` (downlink, built by the server):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `0x44` |
| 1 | 1 | frame version = 1 |
| 2 | 1 | envelope version |
| 3 | 4 | seq: relay sequence number |
| 7 | 32 | from |
| 39 | 32 | to |
| 71 | 8 | tsMs |
| 79 | var | envelope fields |

`D` carries no signature of its own: the envelope inside is end-to-end encrypted and authenticated by the apps.

### 10.4 `POST /uplink`

Request: `{"gateway": <gateway node number>, "frame": "<base64 U frame>"}`. The gateway sends the body of a message received
for `OMP_DEST_INTERNET`, unchanged.

The server checks the frame layout, the envelope, that `tsMs` is at most 10 minutes in the future and at most 7 days in the
past (a message can sit in custody), that `signingKey` is the key bound to `from`, and the signature. A replay is a no-op.

| Status | Body |
| --- | --- |
| 200 | `{"accepted": ["<msgId>"]}`, with `"duplicate": true` for a message already queued |
| 400 | `bad-json`, `bad-frame`, `stale`, or an envelope validation error |
| 401 | `unauthorized`, reason `unbound-identity`, `binding-mismatch` or `invalid-signature` |
| 409 | `message-id-conflict` (same id, different content) |
| 413 | body or ciphertext too large |
| 429 | `rate-limited` |
| 503 | `storage-unavailable` |

The origin sees `UPLINKED` when the gateway radio has acknowledged the message, before this request is made.

### 10.5 `POST /pull`

Request: `{"gateway": <gateway node number>, "pull": "<base64 76 bytes>"}`, where `pull` is the PULL frame without its
3-byte prefix (`nodeNum | afterSeq | tsSec | sig`).

The server rejects a `tsSec` more than 600 s from its clock, a node with no binding, a bad signature (using the bound
`nodePub` and the `gateway` field of the request), and a signature it has already seen. It returns queued envelopes for the
bound account with `seq > afterSeq`: at most 5 frames, and no more than 6 KiB in total unless the first frame alone is larger. Pulling is not destructive; the app still
acknowledges its mail through the normal relay route.

| Status | Body |
| --- | --- |
| 200 | `{"nodeNum": N, "frames": [{"seq": S, "frame": "<base64 D frame>"}, ...], "maxSeq": M, "more": true or false}` |
| 400 | `bad-json`, `bad-gateway`, `bad-pull`, `stale` |
| 401 | `unauthorized`, reason `invalid-signature` |
| 404 | `unbound-node` |
| 409 | `replayed` |

Node behaviour:

- A node sends a PULL every **10 minutes** through its own gateway if online, else through the nearest gateway peer, and only
  when it has a valid clock and channel utilisation is below 25%.
- The gateway forwards a PULL only for the node that sent it (`nodeNum` = packet sender). It delivers each returned `D` frame
  as an OMP message with `origin` = the gateway, `msgId` = `seq`, `dest` = the node and `FLAG_VIA_CUSTODY`.
- On receiving such a message, the node advances its cursor `afterSeq` to `seq` (persisted in `/oshi/pull.bin`) when the
  message has `FLAG_VIA_CUSTODY`, the body is a `D` frame, `seq` is greater than the cursor, and the origin is itself or a
  fresh OSHI peer. It then pulls again after 30 s, since more may be waiting.

## 11. Security considerations

**The `OSHI` channel key is public.** It gives no confidentiality or authenticity against anyone who has read this document.
For frames sent on it (broadcasts, and unicasts to a node whose key is unknown), anyone in range can read the OMP header
(msgId, origin, destination, fragment count, flags, sizes) and can inject frames. Message confidentiality and authenticity
MUST come from the body: the OSHI apps encrypt and authenticate their envelope end to end, and the radios never see it in
clear.

**PKI unicast.** When the sender holds the link recipient's public key, the frame is a Meshtastic PKI packet: encrypted and
authenticated between the two radios, and the OMP header is hidden from third parties (the Meshtastic outer header, with
`from` and `to`, is not).

**Control frames.** A SACK, CUSTODY or RECEIPT from a node whose key is known is accepted only if PKI-encrypted, which stops
forged delivery reports for those peers. From a node whose key is unknown, control frames are unauthenticated: a forged SACK
can end a transfer early or report a false `DELIVERED`, and a RECEIPT is not checked against the custodian the message was
handed to. Applications that need proof of delivery SHOULD use an end-to-end acknowledgement inside their encrypted
envelope.

**Beacons are unauthenticated.** A node can advertise itself as a custodian or gateway and attract messages. It can then
delay or drop them, but it cannot read or alter an end-to-end encrypted body. Custody is a matter of availability, not
confidentiality.

**PULL.** Signed by the node's own key, with the gateway, timestamp and cursor inside the signature, a 600 s freshness window
and a server-side replay cache. A gateway cannot fetch mail for a node that did not ask. The downlink cursor could be pushed
forward by a forged `D` message from a node posing as an OSHI peer; mail skipped that way stays in the relay queue, because
pulling is not destructive, and is collected by the app when it next reaches the internet.

**Uplink.** A `U` frame is signed by the sender's account key and checked against the account's binding, so a gateway or a
mesh node cannot forge or modify one. Replays are absorbed by the relay's receipts.

**Resource limits.** Outbox 16 messages, reassembly 6 messages / 24 KiB / 5 min, completed-message memory 64, custody 32
KiB, phone inbox 16 KiB, phone RAM buffer 24 KiB, gateway queue 8 jobs. All state is bounded by these limits; a flood of frames can push
out legitimate state within them.

**Replays over the air.** The completed-message memory is 64 entries, so an old DATA frame replayed later can be delivered
again. Applications MUST deduplicate on (origin, msgId).

**Known limitation.** A custodian acknowledges the last fragment before it tries to store the complete message. If its store
turns out to be too small for that message, the origin has already recorded `IN_CUSTODY` and the message is lost. The
custodian choice (free space at least the body size) makes this unlikely but does not prevent it. In the same way, a
gateway radio acknowledges an internet message before queueing it for upload; if its queue of 8 jobs is full the message is
dropped after the origin has recorded `UPLINKED`.

## 12. Constants

| Name | Value | Source |
| --- | --- | --- |
| `OMP_MAGIC0`, `OMP_MAGIC1` | `0x4F`, `0x53` | `OshiProtocol.h` |
| `OMP_VERSION` | 1 | `OshiProtocol.h` |
| `OMP_MAX_FRAME` | 200 bytes | `OshiProtocol.h` |
| `OMP_DATA_HEADER` | 18 bytes | `OshiProtocol.h` |
| `OMP_MAX_FRAG_DATA` | 182 bytes | `OshiProtocol.h` |
| `OMP_MAX_FRAGS` | 64 | `OshiProtocol.h` |
| `OMP_MAX_MESSAGE` | 11,648 bytes | `OshiProtocol.h` |
| `OMP_DEST_BROADCAST` | `0xFFFFFFFF` | `OshiProtocol.h` |
| `OMP_DEST_INTERNET` | `0xFFFFFFF0` | `OshiProtocol.h` |
| `OMP_PULL_SIGN_PORT` | `0x4F505531` | `OshiProtocol.h` |
| pace between frames | 2,500 ms | `OshiOutbox.h` `Config::paceMs` |
| SACK wait per round | 25,000 ms + 3,000 ms x count | `ackTimeoutBaseMs`, `ackTimeoutPerFragMs` |
| rounds | 5 | `maxRounds` |
| parked lifetime | 72 h | `parkedTtlMs` |
| parked retry spacing | 60 s | `parkedRetryMs` |
| outbox size | 16 | `maxEntries` |
| reassembly | 6 messages, 24 KiB, 5 min | `OshiMessage.h` `Reassembler::Limits` |
| completed-message memory | 64 | `SeenSet` |
| custody store | 32 KiB | `OshiCustody.h` `CUSTODY_MAX_BYTES` |
| phone inbox | 16 KiB | `OshiModule.h` |
| phone RAM buffer | 24 KiB, 3 queue slots reserved | `OshiModule.cpp` `PHONE_PENDING_MAX`, `PHONE_QUEUE_RESERVE` |
| first beacon, interval | 45 s, 15 min | `OshiModule.cpp` |
| peer freshness, table size | 60 min, 32 | `OshiPeers.h` |
| pull interval, follow-up | 10 min, 30 s | `OshiModule.cpp` |
| gateway queue, timeout, backoff | 8 jobs, 10 s, 30 s | `OshiGatewayEsp32.cpp` |
