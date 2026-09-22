# Implementing OMP

This guide is for anyone adding the OSHI Mesh Protocol to an app, a firmware or a tool. The normative description is
[OMP-v1.md](OMP-v1.md); this page says what each kind of implementation needs and gives test vectors.

Every vector below was produced by the encoders in this repository (`src/oshi/OshiProtocol.cpp`, `OshiMessage.cpp`) or,
for the gateway formats, by the server's reference codec. All values are little-endian.

## 1. What to implement

### An app talking to an OSHI radio

This is the smallest useful implementation. The radio does fragmentation, repair, custody and receipts; the app only needs:

1. The Meshtastic client API you already use (BLE, serial or TCP), sending and receiving `MeshPacket`s with
   `decoded.portnum = 256` (`PRIVATE_APP`).
2. **Probe** (OMP-v1 9.1): send `4F 53 15 00 00 00 00` to your radio's own node number. An answer on port 256 whose payload
   starts `4F 53 15` means OMP is available. No answer means stock firmware: fall back to plain Meshtastic.
3. **Send** (9.2): split the body into DATA fragments with `origin = 0` (vector V2), one `MeshPacket` each, to your radio's
   node number. Pick a random 32-bit `msgId`. Set `FLAG_CUSTODY_OK` if the message may be held for an offline recipient.
4. **Track** (4.5): parse STATUS frames (`4F 53 16`) and match them to your `msgId`.
5. **Receive** (9.3): reassemble DATA frames by `(origin, msgId)`, and deduplicate on that pair, since the radio may pass
   a message again after a reboot.
6. **Body**: OMP carries opaque bytes. To interoperate with the OSHI apps, use their body tags (9.4); otherwise pick your
   own format and make sure your bodies cannot be mistaken for theirs (for example your own first-byte tag).

Nothing else is required: the radio creates the `OSHI` channel itself.

### A firmware or a node implementation

Implement all of OMP-v1 sections 2 to 8: the `OSHI` channel with the published PSK, the frame codecs, the sender rounds and
pacing, receiver SACK rules, the completed-message memory, beacons, and custody if you want to hold messages for others.
The gateway (section 10) is optional. Keep the resource limits; they are what makes the protocol safe on small devices.

To be relayed by stock Meshtastic ROUTERs your frames MUST go on the `OSHI` channel, not on a channel those ROUTERs can
decrypt.

### A gateway server

Implement `POST /v2/mesh/bind`, `/uplink` and `/pull` (section 10), with the `U` and `D` codecs and XEdDSA verification
(vector V12).

## 2. Constants

| Item | Value |
| --- | --- |
| Portnum | 256 |
| Channel name | `OSHI` |
| Channel PSK, SHA-256(`OSHI Mesh channel v1`) | `29436d19af55bd4e20247323c8ff455786f003119817e16cf7df3003684cb950` |
| Magic | `4F 53` |
| Third byte | `0x10 \| type`: DATA `11`, SACK `12`, CUSTODY `13`, RECEIPT `14`, BEACON `15`, STATUS `16`, PULL `17` |
| Max frame / DATA header / fragment data | 200 / 18 / 182 bytes |
| Max fragments / max message | 64 / 11,648 bytes |
| Broadcast / internet destination | `0xFFFFFFFF` / `0xFFFFFFF0` |
| PULL signing domain | `0x4F505531` |

Check the PSK yourself:

```sh
printf 'OSHI Mesh channel v1' | shasum -a 256
# 29436d19af55bd4e20247323c8ff455786f003119817e16cf7df3003684cb950  -
```

## 3. Test vectors

Values used throughout: `msgId = 0xDEADBEEF`, `origin = 0x12345678`, `dest = 0x9ABCDEF0`, gateway `0x0A0B0C0D`.

### V1: DATA, single fragment, `FLAG_CUSTODY_OK`, body `01 "hello"`

```
4f 53 11 ef be ad de 78 56 34 12 f0 de bc 9a 00
01 01 01 68 65 6c 6c 6f                              (24 bytes)
```

| Bytes | Field |
| --- | --- |
| `4f 53 11` | magic, version 1, type DATA |
| `ef be ad de` | msgId `0xDEADBEEF` |
| `78 56 34 12` | origin `0x12345678` |
| `f0 de bc 9a` | dest `0x9ABCDEF0` |
| `00` `01` | idx 0, count 1 |
| `01` | flags `FLAG_CUSTODY_OK` |
| `01 68 65 6c 6c 6f` | body: tag `0x01` (raw) + `hello` |

### V2: the same message as a phone sends it to its radio (`origin = 0`)

```
4f 53 11 ef be ad de 00 00 00 00 f0 de bc 9a 00
01 01 01 68 65 6c 6c 6f
```

### V3: broadcast, no flags

```
4f 53 11 ef be ad de 78 56 34 12 ff ff ff ff 00
01 00 01 68 65 6c 6c 6f
```

Header of a message for the internet (`dest = 0xFFFFFFF0`, no flags):

```
4f 53 11 ef be ad de 78 56 34 12 f0 ff ff ff 00 01 00
```

### V4: fragmentation of a 400-byte body

`count = ceil(400 / 182) = 3`. Frames of 200, 200 and 54 bytes (18-byte header + 182, 182, 36 bytes of body). Headers:

```
frag 0: 4f 53 11 ef be ad de 78 56 34 12 f0 de bc 9a 00 03 00   + body[0..182)
frag 1: 4f 53 11 ef be ad de 78 56 34 12 f0 de bc 9a 01 03 00   + body[182..364)
frag 2: 4f 53 11 ef be ad de 78 56 34 12 f0 de bc 9a 02 03 00   + body[364..400)
```

Fragment counts at the edges: 0 bytes -> 1, 182 -> 1, 183 -> 2, 11,648 -> 64, 11,649 -> not sendable.

### V5: SACK, 12 fragments, fragments 2 and 9 missing (bitmap `0x0DFB`)

```
4f 53 12 ef be ad de 78 56 34 12 0c fb 0d 00 00 00 00 00 00      (20 bytes)
```

Bitmap `0x0DFB` = binary `1101 1111 1011`: fragments 0, 1, 3-8, 10, 11 received; 2 and 9 missing. The complete SACK for
12 fragments has bitmap `0x0FFF`:

```
4f 53 12 ef be ad de 78 56 34 12 0c ff 0f 00 00 00 00 00 00
```

### V6: CUSTODY and RECEIPT

```
CUSTODY: 4f 53 13 ef be ad de 78 56 34 12 f0 de bc 9a        (15 bytes)
RECEIPT: 4f 53 14 ef be ad de 78 56 34 12 f0 de bc 9a
```

### V7: BEACON

Custodian and gateway online, version `0x0100`, 32 KiB free:

```
4f 53 15 03 00 01 20                                         (7 bytes)
```

The phone's probe (content ignored):

```
4f 53 15 00 00 00 00
```

### V8: STATUS, `DELIVERED` by `0x9ABCDEF0`

```
4f 53 16 ef be ad de 03 f0 de bc 9a                          (12 bytes)
```

### V9: PULL layout

`nodeNum = 0x12345678`, `afterSeq = 41`, `tsSec = 1790000000` (`0x6AB13B80`), signature shown as `aa` x 64:

```
4f 53 17 78 56 34 12 29 00 00 00 80 3b b1 6a aa
aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa
aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa
aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa
aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa           (79 bytes)
```

The 20 bytes signed, for gateway `0x0A0B0C0D`:

```
78 56 34 12  80 3b b1 6a  31 55 50 4f  0d 0c 0b 0a  29 00 00 00
nodeNum      tsSec        0x4F505531   gateway      afterSeq
```

### V10: body encoding

| Body | Bytes |
| --- | --- |
| `0x01` + `{"t":"hi"}` | `01 7b 22 74 22 3a 22 68 69 22 7d` |
| `0x02` + raw DEFLATE of `{"t":"hello hello hello hello"}` (31 bytes) | `02 ab 56 2a 51 b2 52 ca 48 cd c9 c9 57 c0 20 95 6a 01` |

DEFLATE output depends on the compressor and its level; this one is zlib level 9 with `wbits = -15`. A decoder must accept
any valid raw DEFLATE stream (no zlib header, no checksum).

### V11: `D` downlink frame

`seq = 42`, envelope version 4, `from` = 32 x `0x11`, `to` = 32 x `0x22`, `tsMs = 1790000000000`, one-to-one, `msgId = "m1"`,
no group, `header = "hd"`, `ciphertext = "ct"`, no x3dh (94 bytes):

```
44 01 04                    'D', frame version 1, envelope version 4
2a 00 00 00                 seq 42
11 x 32                     from
22 x 32                     to
00 6c 50 c4 a0 01 00 00     tsMs 1790000000000
00                          type 0 (one-to-one)
02 6d 31                    msgId "m1"
00                          groupId ""
02 00 68 64                 header "hd"
02 00 63 74                 ciphertext "ct"
00 00                       x3dh (empty)
```

A node reads `seq` at offset 3 to move its pull cursor.

### V12: PULL signature verification

A node's Meshtastic key pair is Curve25519. XEdDSA verification uses the Ed25519 point derived from the Curve25519 public key
`u` as `y = (u - 1) / (u + 1) mod 2^255 - 19`, sign bit 0.

| Item | Hex |
| --- | --- |
| Node public key (Curve25519, what `/bind` receives as `nodePub`) | `869d2d16f9b5b4b13ebf934cd4e3dc9f482259d1172c48308f62c37980cfd24b` |
| Derived Ed25519 verification key | `6b726e36cd2ca77845b8ff32f1a6b2f3923b04c43821d3072e476b5a67977f28` |
| Signed message (V9 layout: node `0x12345678`, ts `1790000000`, gateway `0x0A0B0C0D`, afterSeq 41) | `78563412803bb16a3155504f0d0c0b0a29000000` |
| Signature | `60126d34f37e8a335464823295c272e88aef86a735e2dd826ec95c2504079886ebc448df88f7feb9434d265be9394b41fa7e544c4f62776d416491c7b8d1620c` |

This signature verifies under the derived key. It is a verification vector only: signatures made by the firmware are hedged
(randomised), so two signatures of the same message differ and cannot be compared byte for byte.

The matching `/pull` request body:

```json
{"gateway":168496141,"pull":"eFY0EikAAACAO7FqYBJtNPN+ijNUZIIylcJy6Irvhqc14t2CbslcJQQHmIbrxEjfiPf+uUNNJlvpOUtB+n5UTE9id21BZJHHuNFiDA=="}
```

(`168496141` = `0x0A0B0C0D`; `pull` is the 76 bytes `nodeNum | afterSeq | tsSec | sig`.)

## 4. Checklist

Decoders:

- [ ] Ignore anything on port 256 that does not start `4F 53` with version nibble 1 and type 1-7.
- [ ] Reject DATA shorter than 18 or longer than 200 bytes, `count` 0 or above 64, `idx >= count`.
- [ ] Reject a non-last fragment that is not exactly 182 bytes, and fragments whose `count` or `dest` disagree with earlier
      ones of the same message.
- [ ] Mask SACK bitmaps to `count` bits.
- [ ] Deduplicate complete messages on `(origin, msgId)`.
- [ ] Bound reassembly state (the firmware uses 6 messages, 24 KiB, 5 minutes).

Senders:

- [ ] Every fragment but the last is exactly 182 bytes.
- [ ] At most one frame every 2.5 s; on a shared channel, fewer is better.
- [ ] Unicast: wait `25 s + 3 s x count` after a round, repair from the SACK bitmap, poll with the last fragment, stop after 5
      rounds.
- [ ] Use PKI for unicast when you hold the recipient's key; accept SACK/CUSTODY/RECEIPT from such a node only over PKI.
- [ ] Never send STATUS over the air.

Test against this implementation:

- Unit tests: `test/test_oshi_protocol/test_main.cpp` (run with `pio test -e coverage -f test_oshi_protocol` on Linux).
- `tools/oshi/sim_mesh.py` runs several `meshtasticd` instances (OSHI and stock) and plays the phone over TCP; its helpers
  `omp()` and `data_frames()` are a minimal Python client.

Questions and interoperability reports are welcome as issues or discussions on this repository.
