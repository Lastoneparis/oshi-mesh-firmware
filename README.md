# OSHI Mesh Firmware

**Meshtastic-compatible LoRa mesh firmware with reliable delivery, store-and-forward custody and an internet gateway.**

[![Latest release](https://img.shields.io/github/v/release/Lastoneparis/oshi-mesh-firmware?label=release)](https://github.com/Lastoneparis/oshi-mesh-firmware/releases/latest)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)
[![Flash from your browser](https://img.shields.io/badge/flash-from%20your%20browser-7B3FF2)](https://oshi-messenger.com/lora/flash/)
[![Protocol: OMP v1](https://img.shields.io/badge/protocol-OMP%20v1-informational)](docs/omp/OMP-v1.md)

**Install in one minute:** open <https://oshi-messenger.com/lora/flash/> in Chrome or Edge, plug your board in over USB,
press *Connect*. Heltec V3/V4, LILYGO T3-S3/T-Beam/T-Deck, Station G2 and more; RAK4631, T-Echo and T1000-E as `.uf2`.
Binaries and checksums are also on the [releases page](https://github.com/Lastoneparis/oshi-mesh-firmware/releases/latest).

OSHI Mesh is a fork of the [Meshtastic firmware](https://github.com/meshtastic/firmware) (`develop`). An OSHI node is still a
normal Meshtastic node: the same radio settings, the same packet format, the same channels and keys, and the official
Meshtastic apps keep working. On top of that it adds the **OSHI Mesh Protocol (OMP)**, a small open protocol carried inside
ordinary Meshtastic packets, which gives OSHI nodes things the stock firmware does not have: messages up to 11.6 KB,
end-to-end delivery receipts, repair of lost fragments, and messages that wait in flash for an offline recipient instead of
being dropped.

OMP is documented so that other apps and firmwares can implement it:

- [docs/omp/OMP-v1.md](docs/omp/OMP-v1.md): the protocol specification (frames, state machines, phone protocol, gateway API)
- [docs/omp/IMPLEMENTING.md](docs/omp/IMPLEMENTING.md): how to implement it, with byte-exact test vectors

> OSHI Mesh is an independent project. It is **not affiliated with or endorsed by Meshtastic LLC**. Meshtastic® is a
> registered trademark of Meshtastic LLC. Almost all of the code in this repository is the work of the Meshtastic
> contributors; see [Credits](#credits-and-license).

## Contents

- [Why a fork](#why-a-fork)
- [Compatibility](#compatibility)
- [Feature comparison](#feature-comparison)
- [Status and what has been tested](#status-and-what-has-been-tested)
- [Supported boards](#supported-boards)
- [Install](#install)
- [Build from source](#build-from-source)
- [Apps](#apps)
- [Repository layout](#repository-layout)
- [Credits and license](#credits-and-license)

## Why a fork

Each item below is a limitation of stock Meshtastic that OSHI Mesh addresses. Line numbers refer to the upstream commit this
branch is based on, [`08cd97e`](https://github.com/meshtastic/firmware/tree/08cd97ea2) on `meshtastic/firmware` `develop`.

| Stock limitation | Where upstream | What OSHI Mesh does |
| --- | --- | --- |
| **ROUTERs drop app traffic.** The ROUTER role defaults to `rebroadcast_mode = CORE_PORTNUMS_ONLY`, and a node in that mode drops every packet it can decode whose portnum is not on a fixed list. `PRIVATE_APP` (256) is not on it, so a third-party app's packets die at the first ROUTER that shares their channel. Packets it *cannot* decode are relayed opaquely. | `src/mesh/NodeDB.cpp:1674` (default), `src/mesh/Router.cpp:1608-1621` (drop), `src/mesh/NextHopRouter.cpp:29-40` (opaque relay allowed) | OMP travels on a dedicated secondary channel, `OSHI`, that stock nodes do not hold. To a stock ROUTER an OMP frame is an opaque packet, which it relays. |
| **No fragmentation.** A packet carries at most 233 bytes of application data (`DATA_PAYLOAD_LEN`); anything longer has to be split and reassembled by each app, with no shared repair logic. | `src/mesh/generated/meshtastic/mesh.pb.h:366`, `src/mesh/RadioInterface.h:20` | The firmware fragments a message into up to 64 frames of 182 bytes (11,648 bytes), reassembles it, and repairs only the fragments that were lost (selective ACK bitmap). |
| **DMs to an offline node are lost.** A reliable unicast is attempted `NUM_RELIABLE_UNICAST_ATTEMPTS` = 5 times, then the sender gets a `MAX_RETRANSMIT` NAK and the packet is gone. Nothing is persisted. | `src/mesh/NextHopRouter.h:115`, `src/mesh/NextHopRouter.cpp:473-477` | After 5 unanswered rounds the message is handed to a custodian node, or parked in the sender's own flash, and re-sent automatically when the destination is heard again (up to 72 h, survives reboots). |
| **Store & Forward is narrow.** The S&F server only runs on boards with PSRAM (at least 1 MB free), only stores `TEXT_MESSAGE_APP`, and keeps its records in RAM, so a reboot loses them. | `src/modules/StoreForwardModule.cpp:609-640` (PSRAM), `:413` (text only) | Custody works on any board with a filesystem, stores any OMP message (opaque bytes, including end-to-end encrypted payloads), and keeps it in flash (`/oshi/custody.bin`, 32 KB). |
| **The `COMPATIBLE` signature policy accepts unsigned broadcasts from nodes known to sign.** This exists because 2.5-2.7 relays strip signatures, but it also accepts an unsigned broadcast heard directly from its originator, where no relay could have stripped anything. | `src/mesh/Router.cpp:735`, `:788-789` | An unsigned broadcast from a known signer is dropped when it was heard direct, or when its relay is itself a known signer, and the signed form would have fit in a LoRa packet (`src/oshi/OshiPolicy.h`, hook in `Router.cpp`). |
| **One duty-cycle gate for everything.** Once the hourly TX budget is spent, `Router::send` refuses every transmission, including ACKs and DMs, while relayed broadcasts were free to consume the budget beforehand. | `src/mesh/Router.cpp:484-487` | Between 80% and 100% of the regional duty cycle, relayed broadcasts below `RELIABLE` priority and our own `BACKGROUND` traffic are shed first, keeping the last 20% for ACKs and direct messages (only where the region has a duty cycle below 100%). OMP itself only transmits while TX air use is under half the duty cycle. |
| **Small RAM queue to the phone.** Packets waiting for the phone sit in a RAM queue of `MAX_RX_TOPHONE` slots (8 on classic ESP32, 16 on nRF52840); when the phone is away, newer packets push older ones out, and a reboot empties it. | `src/mesh/mesh-pb-constants.h:28-41` | Complete OMP messages for the phone are kept in a flash-backed inbox (`/oshi/inbox.bin`, 16 KB) until the phone has collected them, and OMP frames wait outside the shared queue while no client is connected, so they do not crowd out ordinary traffic. |

## Compatibility

### What stays identical on air

- LoRa modem settings, regions, presets and frequency slots.
- The Meshtastic packet header, channel encryption (AES-CTR with the channel PSK), PKI direct messages, XEdDSA packet
  signatures, flooding and next-hop routing, hop limits, ACK/NAK.
- Every standard portnum (text, position, node info, telemetry, traceroute, admin, ...). An OSHI node sends, relays and
  answers them exactly as the upstream code does.
- The phone API (BLE, serial, TCP/WiFi). The official Meshtastic apps, CLI and web client work with an OSHI node.

A stock Meshtastic node and an OSHI node on the same mesh exchange text messages normally (tested in simulation and over the air, see below).

### What is OSHI-to-OSHI only

- OMP frames: `PRIVATE_APP` (256) payloads starting with the magic `OS`, on the secondary channel `OSHI`. Stock nodes
  relay them (they cannot decrypt them) but do not interpret them.
- Which stock nodes relay them, by rebroadcast mode (read in the 2.7 and 2.8 sources, and tested in simulation for 2.8):
  `ALL` (the default for most roles) and `CORE_PORTNUMS_ONLY` (the ROUTER default) relay the `OSHI` channel opaque.
  `LOCAL_ONLY` and `KNOWN_ONLY`, which an owner sets on purpose to carry only their own channels, do not relay it
  (a PKI direct message to or from a node they know still passes). `NONE` and `CLIENT_MUTE` relay nothing at all.
- Fragmentation, selective repair, delivery receipts, custody and the internet gateway all require OSHI firmware on the
  endpoints that take part. Stock relays in between are fine.

### Behaviour differences a stock user may notice

- The firmware adds a secondary channel named `OSHI` in the first free slot (index 1-7). If every slot is taken, OMP falls
  back to the primary channel, where `CORE_PORTNUMS_ONLY` ROUTERs that decode it will drop it. In licensed (ham) mode the
  channel is not created, because encryption is not allowed.
- OSHI nodes broadcast a 7-byte OMP `BEACON` on the `OSHI` channel 45 s after boot and every 15 minutes after that, only
  while channel utilisation is under 25%.
- The duty-cycle shedding and the stricter `COMPATIBLE` signature rule above also apply to stock traffic relayed through an
  OSHI node.
- Default names are `OSHI xxxx` / `OSHI_xxxx` instead of `Meshtastic xxxx`, and the firmware reports the `DIY_EDITION`
  edition.

## Feature comparison

| | Stock Meshtastic | OSHI Mesh |
| --- | --- | --- |
| Interoperates with stock nodes and apps | yes | yes |
| Max payload per message | 233 B | 11,648 B (64 x 182 B fragments) |
| Firmware fragmentation and reassembly | no | yes |
| Selective repair of lost fragments | no | yes (64-bit SACK bitmap) |
| End-to-end delivery receipt to the sender's phone | ACK for a single packet | `DELIVERED` for the whole message, also through a custodian (`RECEIPT`) |
| DM to a node that is offline | lost after 5 attempts | parked in flash, re-sent when the node is heard, 72 h |
| Store & forward | text only, PSRAM boards, RAM | any OMP message, any board with a filesystem, flash |
| Message status to the phone | ACK/NAK routing packets | `QUEUED`, `SENT`, `DELIVERED`, `IN_CUSTODY`, `FAILED`, `UPLINKED`, `REJECTED` |
| Messages kept for an absent phone | RAM queue, 8-16 packets | flash inbox, 16 KB of whole messages |
| Relayed by stock `CORE_PORTNUMS_ONLY` ROUTERs | `PRIVATE_APP` dropped | yes (opaque on the `OSHI` channel) |
| Internet gateway | MQTT (plaintext or channel-encrypted) | HTTPS to `/v2/mesh`, carries only self-authenticating frames |
| Duty cycle near the limit | everything blocked at 100% | broadcasts shed from 80%, ACKs and DMs keep the rest |

## Status and what has been tested

This is early software. What has been verified, and how:

| Area | Result |
| --- | --- |
| Unit tests (`test/test_oshi_protocol`) | 34/34 pass: codecs, fragmentation, reassembly limits, outbox rounds, SACK repair, custody hand-off, persistence, gateway codec, router policies |
| Simulation (`tools/oshi/sim_mesh.py`, several `meshtasticd` instances including an unmodified stock one, every packet carried as ciphertext) | 7 scenarios pass: interop with a stock node; capability probe (answered by OSHI, ignored by stock); 2 KB message delivered as 12 fragments with `DELIVERED` receipt; custody (destination offline, message parked, delivered when it returns); OMP frames through a stock node are not surfaced to its phone; **two OSHI nodes out of range of each other with a stock node between them, as CLIENT (`ALL`) and as ROUTER (`CORE_PORTNUMS_ONLY`)**: text and a 600-byte OMP message cross, the stock node relays every OSHI frame without decoding it, and the sender gets `DELIVERED`. The `inbox-reboot` scenario cannot pass yet: the harness carries the air through each node's API link, so closing the phone also deafens the radio. |
| Over the air, two Heltec V3 | interop text with a stock node, and a 600-byte OMP message delivered with its receipt |
| iPhone over BLE | the capability probe detects OSHI firmware, and iOS relaunches the app in the background to collect LoRa messages |

Not verified yet:

- The internet gateway against the production server (the `/v2/mesh` routes are not deployed yet).
- T-Beam on real hardware (it builds; it has not been flashed and tested).
- Large multi-hop networks. Tests so far are two radios over the air and small simulated meshes.
- Relaying through stock **2.5-2.7** nodes in simulation (the stock node tested is 2.8; 2.7 was checked by reading its
  source only).
- Bridging to MeshCore: implemented and unit-tested, not yet tested on air (see [MeshCore](#meshcore)).

## MeshCore

A LoRa radio listens to one network at a time: MeshCore uses its own radio settings and packet format, so no Meshtastic or
OSHI node can hear it directly. OSHI messages reach MeshCore through
[oshi-meshcore-bridge](https://github.com/Lastoneparis/oshi-meshcore-bridge) (GPL-3.0, a fork of the Akita
Meshtastic-MeshCore bridge): one computer (Raspberry Pi, PC, Mac) with two radios, one running OSHI Mesh or Meshtastic,
one running MeshCore companion firmware. It carries OMP frames across as MeshCore group-channel datagrams on a dedicated
channel, re-fragmented to fit MeshCore's 184-byte packets, and announces itself with a `CAP_BRIDGE` beacon so the sender
accepts its delivery receipts (firmware `b960d30` or later). Its own README lists what has and has not been verified.

## Supported boards

OSHI Mesh changes no board support code: every target the upstream firmware builds for should build here. The internet
gateway needs WiFi and is ESP32-only; everything else is platform-independent C++.

| Board | PlatformIO env | Status |
| --- | --- | --- |
| Heltec WiFi LoRa 32 V3 | `heltec-v3` | released, tested over the air |
| Heltec WiFi LoRa 32 V4, Heltec Wireless Tracker | `heltec-v4`, `heltec-wireless-tracker` | released, not tested on hardware |
| LILYGO T3-S3, T-Beam, T-Beam Supreme, T-Deck | `tlora-t3s3-v1`, `tbeam`, `tbeam-s3-core`, `t-deck` | released, not tested on hardware |
| B&Q Station G2 | `station-g2` | released, not tested on hardware |
| RAK WisBlock 4631, LILYGO T-Echo, Seeed T1000-E (nRF52, `.uf2`) | `rak4631`, `t-echo`, `tracker-t1000-e` | released, not tested on hardware |
| Any other upstream target | see `variants/` | expected to build, not released |

## Install

**Web flasher:** <https://oshi-messenger.com/lora>. Flash from the browser over USB, no toolchain needed.

A build can also be flashed with the standard Meshtastic tools (`esptool`, the device-install scripts in `bin/`, or UF2 on
nRF52), the same way as upstream firmware. The `OSHI` channel is added on first boot.

## Build from source

```sh
git clone https://github.com/Lastoneparis/oshi-mesh-firmware.git
cd oshi-mesh-firmware
pio run -e heltec-v3            # or any env from variants/
pio run -e heltec-v3 -t upload  # flash over USB
```

The toolchain is the upstream one (PlatformIO). See the Meshtastic
[build guide](https://meshtastic.org/docs/development/firmware/build/) for setup details.

Build flags:

- `-DMESHTASTIC_EXCLUDE_OSHI=1` disables the OSHI hooks in the router and phone service and does not start the module or the gateway, leaving upstream behaviour.
- `-DOSHI_GATEWAY_URL=\"https://example.org/v2/mesh\"` points the gateway at another server implementing the
  [gateway API](docs/omp/OMP-v1.md#10-gateway-http-api).

Tests and simulation:

```sh
pio test -e coverage -f test_oshi_protocol      # native (portduino) build: Linux, WSL or bin/test-native-docker.sh
python3 tools/oshi/sim_mesh.py --oshi PATH/meshtasticd --stock PATH/meshtasticd
```

The harness sets `SIM_CARRY_CIPHERTEXT=1`, which makes an OSHI build's `SimRadio` hand every packet over as ciphertext
so each node decrypts for itself, as on air. Without it the simulator passes a sender's channel packets as plaintext, and
a stock relay that lacks the `OSHI` channel fails to re-encrypt them, which no real radio does. A stock build already
carries what it cannot decrypt as ciphertext, which is the part that matters for relaying.

## Use it in your own device, firmware or app

Everything here is GPL-3.0 and meant to be reused:

- **Another board:** OSHI Mesh touches no board code, so any `variants/` target builds with `pio run -e <env>`. The OSHI
  code is platform-independent C++ under `src/oshi/` plus `src/modules/OshiModule.*`; only the internet gateway needs
  an ESP32 with WiFi.
- **Another firmware (MeshCore, Reticulum, your own):** implement OMP from [OMP-v1.md](docs/omp/OMP-v1.md) and check your
  encoder against the byte-exact vectors in [IMPLEMENTING.md](docs/omp/IMPLEMENTING.md). The codec in
  `src/oshi/OshiProtocol.{h,cpp}` has no Meshtastic dependency and can be copied as is.
- **An app:** OMP travels as ordinary `PRIVATE_APP` packets through the standard Meshtastic phone API (BLE, serial,
  TCP), so any Meshtastic client library (Python, JS, Swift, Kotlin) can send and receive it. `tools/oshi/sim_mesh.py`
  is a working Python example.
- **Your own server:** build with `-DOSHI_GATEWAY_URL=...` and implement the [gateway API](docs/omp/OMP-v1.md#10-gateway-http-api).
- **Upstream only:** `-DMESHTASTIC_EXCLUDE_OSHI=1` gives back stock behaviour.

## Apps

- **OSHI messenger** (iOS, Android, desktop) detects OSHI firmware with the capability probe and uses OMP for long messages,
  receipts and custody. On stock firmware it falls back to plain Meshtastic packets.
- **Official Meshtastic apps** (Android, iOS, web, CLI) work with an OSHI node for everything Meshtastic does. They do not
  speak OMP: raw OMP frames from the mesh are not passed to the phone, and OMP messages the radio delivers to its client
  arrive as `PRIVATE_APP` packets, which these apps do not display.
- Anyone can add OMP to an app: see [docs/omp/IMPLEMENTING.md](docs/omp/IMPLEMENTING.md).

## Repository layout

| Path | What |
| --- | --- |
| `src/oshi/OshiProtocol.*` | OMP frame encoding and decoding |
| `src/oshi/OshiMessage.*` | fragmentation, reassembly, duplicate suppression |
| `src/oshi/OshiOutbox.*` | paced sending, selective-ACK repair, custody hand-off |
| `src/oshi/OshiCustody.*` | flash store for parked messages |
| `src/oshi/OshiPeers.*` | OSHI peers learned from beacons; custodian and gateway choice |
| `src/oshi/OshiGateway*` | HTTPS gateway (ESP32) and its request codec |
| `src/oshi/OshiPolicy.h` | router policy changes (signature rule, duty-cycle shedding) |
| `src/modules/OshiModule.*` | glue to the Meshtastic firmware: channel, radio, phone, persistence |
| `src/mesh/{MeshService,Router}.cpp`, `src/modules/Modules.cpp` | the only upstream files with OSHI hooks, all behind `MESHTASTIC_EXCLUDE_OSHI` |
| `test/test_oshi_protocol/` | unit tests |
| `tools/oshi/` | multi-node simulator and hardware pair test |
| `docs/omp/` | protocol specification |

Everything else is upstream Meshtastic. How the fork tracks upstream is described in [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits and license

OSHI Mesh is built on the Meshtastic firmware, written by the
[Meshtastic contributors](https://github.com/meshtastic/firmware/graphs/contributors) and maintained by the Meshtastic
project. Nearly all of this repository is their work; OSHI Mesh adds the OMP module and a handful of hooks. Please support
the upstream project: <https://meshtastic.org>.

Licensed under the [GNU General Public License v3.0](LICENSE), like the upstream firmware. The OMP specification in
`docs/omp/` may be implemented by anyone, in any software, under any license.

OSHI Mesh is not affiliated with or endorsed by Meshtastic LLC. Meshtastic® is a registered trademark of Meshtastic LLC,
used here only to describe compatibility.

Security issues: see [SECURITY.md](SECURITY.md).
