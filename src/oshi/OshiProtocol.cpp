#include "OshiProtocol.h"
#include <cstring>

namespace oshi
{

namespace
{

constexpr size_t PREFIX = 3;
constexpr size_t SACK_LEN = PREFIX + 4 + 4 + 1 + 8;
constexpr size_t NOTICE_LEN = PREFIX + 4 + 4 + 4;
constexpr size_t BEACON_LEN = PREFIX + 1 + 2 + 1;
constexpr size_t STATUS_LEN = PREFIX + 4 + 1 + 4;
constexpr size_t PULL_LEN = PREFIX + 4 + 4 + 4 + OMP_PULL_SIG_LEN;

void putU16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

void putU32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (v >> (8 * i)) & 0xFF;
}

void putU64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (v >> (8 * i)) & 0xFF;
}

uint16_t getU16(const uint8_t *p)
{
    return p[0] | (uint16_t(p[1]) << 8);
}

uint32_t getU32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= uint32_t(p[i]) << (8 * i);
    return v;
}

uint64_t getU64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= uint64_t(p[i]) << (8 * i);
    return v;
}

size_t putPrefix(FrameType t, uint8_t *out)
{
    out[0] = OMP_MAGIC0;
    out[1] = OMP_MAGIC1;
    out[2] = uint8_t(OMP_VERSION << 4) | (uint8_t(t) & 0x0F);
    return PREFIX;
}

bool checkPrefix(const uint8_t *buf, size_t len, FrameType expected, size_t minLen)
{
    FrameType t;
    return len >= minLen && frameType(buf, len, t) && t == expected;
}

} // namespace

bool isOmpFrame(const uint8_t *buf, size_t len)
{
    return buf && len >= PREFIX && buf[0] == OMP_MAGIC0 && buf[1] == OMP_MAGIC1 && (buf[2] >> 4) == OMP_VERSION;
}

bool frameType(const uint8_t *buf, size_t len, FrameType &out)
{
    if (!isOmpFrame(buf, len))
        return false;
    uint8_t t = buf[2] & 0x0F;
    if (t < uint8_t(FrameType::DATA) || t > uint8_t(FrameType::PULL))
        return false;
    out = FrameType(t);
    return true;
}

size_t encodeData(const DataFrame &f, uint8_t *out, size_t cap)
{
    if (f.count == 0 || f.count > OMP_MAX_FRAGS || f.idx >= f.count || f.len > OMP_MAX_FRAG_DATA ||
        cap < OMP_DATA_HEADER + f.len || (f.len && !f.data))
        return 0;
    size_t n = putPrefix(FrameType::DATA, out);
    putU32(out + n, f.msgId);
    putU32(out + n + 4, f.origin);
    putU32(out + n + 8, f.dest);
    out[n + 12] = f.idx;
    out[n + 13] = f.count;
    out[n + 14] = f.flags;
    if (f.len)
        memcpy(out + OMP_DATA_HEADER, f.data, f.len);
    return OMP_DATA_HEADER + f.len;
}

bool decodeData(const uint8_t *buf, size_t len, DataFrame &out)
{
    if (!checkPrefix(buf, len, FrameType::DATA, OMP_DATA_HEADER) || len > OMP_MAX_FRAME)
        return false;
    const uint8_t *p = buf + PREFIX;
    out.msgId = getU32(p);
    out.origin = getU32(p + 4);
    out.dest = getU32(p + 8);
    out.idx = p[12];
    out.count = p[13];
    out.flags = p[14];
    out.data = buf + OMP_DATA_HEADER;
    out.len = len - OMP_DATA_HEADER;
    return out.count > 0 && out.count <= OMP_MAX_FRAGS && out.idx < out.count;
}

size_t encodeSack(const SackFrame &f, uint8_t *out, size_t cap)
{
    if (cap < SACK_LEN || f.count == 0 || f.count > OMP_MAX_FRAGS)
        return 0;
    size_t n = putPrefix(FrameType::SACK, out);
    putU32(out + n, f.msgId);
    putU32(out + n + 4, f.origin);
    out[n + 8] = f.count;
    putU64(out + n + 9, f.bitmap & fullBitmap(f.count));
    return SACK_LEN;
}

bool decodeSack(const uint8_t *buf, size_t len, SackFrame &out)
{
    if (!checkPrefix(buf, len, FrameType::SACK, SACK_LEN))
        return false;
    const uint8_t *p = buf + PREFIX;
    out.msgId = getU32(p);
    out.origin = getU32(p + 4);
    out.count = p[8];
    if (out.count == 0 || out.count > OMP_MAX_FRAGS)
        return false;
    out.bitmap = getU64(p + 9) & fullBitmap(out.count);
    return true;
}

size_t encodeNotice(FrameType type, const NoticeFrame &f, uint8_t *out, size_t cap)
{
    if (cap < NOTICE_LEN || (type != FrameType::CUSTODY && type != FrameType::RECEIPT))
        return 0;
    size_t n = putPrefix(type, out);
    putU32(out + n, f.msgId);
    putU32(out + n + 4, f.origin);
    putU32(out + n + 8, f.dest);
    return NOTICE_LEN;
}

bool decodeNotice(const uint8_t *buf, size_t len, FrameType expected, NoticeFrame &out)
{
    if ((expected != FrameType::CUSTODY && expected != FrameType::RECEIPT) || !checkPrefix(buf, len, expected, NOTICE_LEN))
        return false;
    const uint8_t *p = buf + PREFIX;
    out.msgId = getU32(p);
    out.origin = getU32(p + 4);
    out.dest = getU32(p + 8);
    return true;
}

size_t encodeBeacon(const BeaconFrame &f, uint8_t *out, size_t cap)
{
    if (cap < BEACON_LEN)
        return 0;
    size_t n = putPrefix(FrameType::BEACON, out);
    out[n] = f.caps;
    putU16(out + n + 1, f.version);
    out[n + 3] = f.custodyFreeKb;
    return BEACON_LEN;
}

bool decodeBeacon(const uint8_t *buf, size_t len, BeaconFrame &out)
{
    if (!checkPrefix(buf, len, FrameType::BEACON, BEACON_LEN))
        return false;
    const uint8_t *p = buf + PREFIX;
    out.caps = p[0];
    out.version = getU16(p + 1);
    out.custodyFreeKb = p[3];
    return true;
}

size_t encodeStatus(const StatusFrame &f, uint8_t *out, size_t cap)
{
    if (cap < STATUS_LEN)
        return 0;
    size_t n = putPrefix(FrameType::STATUS, out);
    putU32(out + n, f.msgId);
    out[n + 4] = uint8_t(f.state);
    putU32(out + n + 5, f.node);
    return STATUS_LEN;
}

bool decodeStatus(const uint8_t *buf, size_t len, StatusFrame &out)
{
    if (!checkPrefix(buf, len, FrameType::STATUS, STATUS_LEN))
        return false;
    const uint8_t *p = buf + PREFIX;
    out.msgId = getU32(p);
    uint8_t s = p[4];
    if (s < uint8_t(MsgState::QUEUED) || s > uint8_t(MsgState::REJECTED))
        return false;
    out.state = MsgState(s);
    out.node = getU32(p + 5);
    return true;
}

size_t encodePull(const PullFrame &f, uint8_t *out, size_t cap)
{
    if (cap < PULL_LEN)
        return 0;
    size_t n = putPrefix(FrameType::PULL, out);
    putU32(out + n, f.nodeNum);
    putU32(out + n + 4, f.afterSeq);
    putU32(out + n + 8, f.tsSec);
    memcpy(out + n + 12, f.sig, OMP_PULL_SIG_LEN);
    return PULL_LEN;
}

bool decodePull(const uint8_t *buf, size_t len, PullFrame &out)
{
    if (!checkPrefix(buf, len, FrameType::PULL, PULL_LEN) || len != PULL_LEN)
        return false;
    const uint8_t *p = buf + PREFIX;
    out.nodeNum = getU32(p);
    out.afterSeq = getU32(p + 4);
    out.tsSec = getU32(p + 8);
    memcpy(out.sig, p + 12, OMP_PULL_SIG_LEN);
    return true;
}

void pullSigningPayload(uint32_t gateway, uint32_t afterSeq, uint8_t out[8])
{
    putU32(out, gateway);
    putU32(out + 4, afterSeq);
}

} // namespace oshi
