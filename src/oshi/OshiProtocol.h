#pragma once

// OSHI Mesh Protocol (OMP) v1. Every OMP frame is the payload of a PRIVATE_APP (256) packet and starts
// with the magic "OS", so it never collides with the legacy OSHI app framing ("OM") on the same portnum.

#include <cstddef>
#include <cstdint>

namespace oshi
{

constexpr uint8_t OMP_MAGIC0 = 0x4F; // 'O'
constexpr uint8_t OMP_MAGIC1 = 0x53; // 'S'
constexpr uint8_t OMP_VERSION = 1;

// Sized so a frame fits a PKI DM (255 - 16 header - 12 PKC overhead - Data framing) with margin.
constexpr size_t OMP_MAX_FRAME = 200;
constexpr size_t OMP_DATA_HEADER = 18;
constexpr size_t OMP_MAX_FRAG_DATA = OMP_MAX_FRAME - OMP_DATA_HEADER;
constexpr uint8_t OMP_MAX_FRAGS = 64;
constexpr size_t OMP_MAX_MESSAGE = OMP_MAX_FRAG_DATA * OMP_MAX_FRAGS;

constexpr uint32_t OMP_DEST_BROADCAST = 0xFFFFFFFF;
// A message for the internet side, carried to a gateway node that uplinks it.
constexpr uint32_t OMP_DEST_INTERNET = 0xFFFFFFF0;

enum class FrameType : uint8_t {
    DATA = 1,
    SACK = 2,
    CUSTODY = 3, // a custodian tells the origin it now holds the message
    RECEIPT = 4, // a custodian or gateway tells the origin the message reached its destination
    BEACON = 5,
    STATUS = 6, // radio -> own phone only, never transmitted
    PULL = 7,   // a node asks a gateway for its internet mail, signed with its node key
};

// XEdDSA domain separator for PULL signatures, passed where CryptoEngine::xeddsa_sign expects a portnum.
constexpr uint32_t OMP_PULL_SIGN_PORT = 0x4F505531;
constexpr size_t OMP_PULL_SIG_LEN = 64;

enum DataFlags : uint8_t {
    FLAG_CUSTODY_OK = 1 << 0,  // origin allows the message to be parked with a custodian
    FLAG_VIA_CUSTODY = 1 << 1, // sent by a custodian on behalf of origin
    FLAG_CUSTODY_REQ = 1 << 2, // addressed to a custodian for holding, not for delivery
};

enum BeaconCaps : uint8_t {
    CAP_CUSTODIAN = 1 << 0,
    CAP_GATEWAY_ONLINE = 1 << 1,
};

enum class MsgState : uint8_t {
    QUEUED = 1,
    SENT = 2,
    DELIVERED = 3,
    IN_CUSTODY = 4,
    FAILED = 5,
    UPLINKED = 6,
    REJECTED = 7,
};

struct DataFrame {
    uint32_t msgId = 0;
    uint32_t origin = 0;
    uint32_t dest = 0;
    uint8_t idx = 0;
    uint8_t count = 0;
    uint8_t flags = 0;
    const uint8_t *data = nullptr;
    size_t len = 0;
};

struct SackFrame {
    uint32_t msgId = 0;
    uint32_t origin = 0;
    uint8_t count = 0;
    uint64_t bitmap = 0;
};

// CUSTODY and RECEIPT share this layout.
struct NoticeFrame {
    uint32_t msgId = 0;
    uint32_t origin = 0;
    uint32_t dest = 0;
};

struct BeaconFrame {
    uint8_t caps = 0;
    uint16_t version = 0;
    uint8_t custodyFreeKb = 0;
};

struct PullFrame {
    uint32_t nodeNum = 0;
    uint32_t afterSeq = 0;
    uint32_t tsSec = 0;
    uint8_t sig[OMP_PULL_SIG_LEN] = {0};
};

struct StatusFrame {
    uint32_t msgId = 0;
    MsgState state = MsgState::QUEUED;
    uint32_t node = 0;
    uint32_t origin = 0; // local bookkeeping only, not on the wire
};

inline uint64_t fullBitmap(uint8_t count)
{
    return count >= 64 ? ~0ULL : ((1ULL << count) - 1);
}

bool isOmpFrame(const uint8_t *buf, size_t len);
bool frameType(const uint8_t *buf, size_t len, FrameType &out);

size_t encodeData(const DataFrame &f, uint8_t *out, size_t cap);
size_t encodeSack(const SackFrame &f, uint8_t *out, size_t cap);
size_t encodeNotice(FrameType type, const NoticeFrame &f, uint8_t *out, size_t cap);
size_t encodeBeacon(const BeaconFrame &f, uint8_t *out, size_t cap);
size_t encodeStatus(const StatusFrame &f, uint8_t *out, size_t cap);
size_t encodePull(const PullFrame &f, uint8_t *out, size_t cap);
// The 20 bytes a PULL signature covers: nodeNum | tsSec | OMP_PULL_SIGN_PORT | gateway | afterSeq.
void pullSigningPayload(uint32_t gateway, uint32_t afterSeq, uint8_t out[8]);

// Decoders reject anything malformed: wrong magic/version/type, truncated, idx >= count, count > OMP_MAX_FRAGS.
bool decodeData(const uint8_t *buf, size_t len, DataFrame &out);
bool decodeSack(const uint8_t *buf, size_t len, SackFrame &out);
bool decodeNotice(const uint8_t *buf, size_t len, FrameType expected, NoticeFrame &out);
bool decodeBeacon(const uint8_t *buf, size_t len, BeaconFrame &out);
bool decodeStatus(const uint8_t *buf, size_t len, StatusFrame &out);
bool decodePull(const uint8_t *buf, size_t len, PullFrame &out);

} // namespace oshi
