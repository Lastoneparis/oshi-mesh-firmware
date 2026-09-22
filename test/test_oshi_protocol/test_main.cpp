// Tests for the OSHI Mesh Protocol pure layer: the frame codec (src/oshi/OshiProtocol.cpp), fragment
// reassembly and the replay set (src/oshi/OshiMessage.cpp), the outbound state machine
// (src/oshi/OshiOutbox.cpp), custody serialization (src/oshi/OshiCustody.cpp) and custodian/gateway
// selection (src/oshi/OshiPeers.cpp), and the gateway HTTPS codec (src/oshi/OshiGatewayCodec.cpp) whose
// output mesh_gateway.js on the server parses; plus the two Router policies OSHI changes (src/oshi/OshiPolicy.h):
// downgrade detection under COMPATIBLE signing, and duty-cycle shedding that keeps airtime for ACKs and DMs.
//
// Why these contracts matter: OMP frames ride PRIVATE_APP next to the legacy OSHI app framing ("OM"), are
// relayed by stock Meshtastic nodes that never parse them, and arrive lossy, duplicated and reordered. The
// codec must reject anything malformed before it reaches a buffer; reassembly must be bounded in memory on
// an ESP32 without PSRAM; the outbox must repair only missing fragments, never report DELIVERED without a
// complete bitmap from the node it sent to, and must park (not drop) a message whose destination is silent.
//
// Regressions guarded: accepting a SACK from a node other than the link peer (a forged "delivered"), an
// unbounded reassembly table, re-sending every fragment on each round instead of the missing ones, losing a
// message when the destination is offline, and custody records that survive a corrupted file.
//
// Deliberately does NOT include TestUtil.h: this suite is pure-function, like test_utf8.
#include "oshi/OshiCustody.h"
#include "oshi/OshiGatewayCodec.h"
#include "oshi/OshiMessage.h"
#include "oshi/OshiOutbox.h"
#include "oshi/OshiPeers.h"
#include "oshi/OshiPolicy.h"
#include "oshi/OshiProtocol.h"
#include <cstring>
#include <unity.h>

using namespace oshi;

void setUp(void) {}
void tearDown(void) {}

static Message makeMessage(uint32_t id, uint32_t origin, uint32_t dest, size_t len, uint8_t flags = 0)
{
    Message m;
    m.msgId = id;
    m.origin = origin;
    m.dest = dest;
    m.flags = flags;
    m.body.resize(len);
    for (size_t i = 0; i < len; i++)
        m.body[i] = uint8_t(i * 7 + id);
    return m;
}

// ---- codec ----

void test_data_roundtrip()
{
    uint8_t payload[OMP_MAX_FRAG_DATA];
    memset(payload, 0xAB, sizeof(payload));
    DataFrame f;
    f.msgId = 0xDEADBEEF;
    f.origin = 0x11223344;
    f.dest = 0x55667788;
    f.idx = 3;
    f.count = 9;
    f.flags = FLAG_CUSTODY_OK;
    f.data = payload;
    f.len = sizeof(payload);
    uint8_t buf[OMP_MAX_FRAME];
    size_t n = encodeData(f, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(OMP_MAX_FRAME, n);
    DataFrame d;
    TEST_ASSERT_TRUE(decodeData(buf, n, d));
    TEST_ASSERT_EQUAL_HEX32(f.msgId, d.msgId);
    TEST_ASSERT_EQUAL_HEX32(f.origin, d.origin);
    TEST_ASSERT_EQUAL_HEX32(f.dest, d.dest);
    TEST_ASSERT_EQUAL(3, d.idx);
    TEST_ASSERT_EQUAL(9, d.count);
    TEST_ASSERT_EQUAL(FLAG_CUSTODY_OK, d.flags);
    TEST_ASSERT_EQUAL(sizeof(payload), d.len);
    TEST_ASSERT_EQUAL_MEMORY(payload, d.data, d.len);
}

void test_legacy_om_frame_is_not_omp()
{
    const uint8_t legacy[] = {0x4F, 0x4D, 1, 2, 3, 4, 0, 1, 'x'};
    FrameType t;
    TEST_ASSERT_FALSE(isOmpFrame(legacy, sizeof(legacy)));
    TEST_ASSERT_FALSE(frameType(legacy, sizeof(legacy), t));
}

void test_data_rejects_idx_out_of_range_and_truncation()
{
    uint8_t buf[OMP_MAX_FRAME];
    DataFrame f;
    f.count = 2;
    f.idx = 1;
    size_t n = encodeData(f, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(OMP_DATA_HEADER, n);
    DataFrame d;
    TEST_ASSERT_FALSE(decodeData(buf, n - 1, d));
    buf[3 + 12] = 2; // idx == count
    TEST_ASSERT_FALSE(decodeData(buf, n, d));
    buf[3 + 12] = 0;
    buf[3 + 13] = OMP_MAX_FRAGS + 1;
    TEST_ASSERT_FALSE(decodeData(buf, n, d));
    f.idx = 5;
    TEST_ASSERT_EQUAL(0, encodeData(f, buf, sizeof(buf)));
}

void test_wrong_version_rejected()
{
    uint8_t buf[32];
    SackFrame s;
    s.msgId = 1;
    s.origin = 2;
    s.count = 4;
    s.bitmap = 0xF;
    size_t n = encodeSack(s, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    buf[2] = (2 << 4) | uint8_t(FrameType::SACK);
    SackFrame d;
    TEST_ASSERT_FALSE(decodeSack(buf, n, d));
}

void test_sack_bitmap_masked_to_count()
{
    uint8_t buf[32];
    SackFrame s;
    s.msgId = 7;
    s.origin = 8;
    s.count = 3;
    s.bitmap = ~0ULL;
    size_t n = encodeSack(s, buf, sizeof(buf));
    SackFrame d;
    TEST_ASSERT_TRUE(decodeSack(buf, n, d));
    TEST_ASSERT_EQUAL_HEX64(0x7, d.bitmap);
}

void test_notice_beacon_status_roundtrip()
{
    uint8_t buf[32];
    NoticeFrame n{1, 2, 3};
    size_t len = encodeNotice(FrameType::RECEIPT, n, buf, sizeof(buf));
    NoticeFrame nd;
    TEST_ASSERT_TRUE(decodeNotice(buf, len, FrameType::RECEIPT, nd));
    TEST_ASSERT_FALSE(decodeNotice(buf, len, FrameType::CUSTODY, nd));
    TEST_ASSERT_EQUAL_HEX32(3, nd.dest);

    BeaconFrame b;
    b.caps = CAP_CUSTODIAN | CAP_GATEWAY_ONLINE;
    b.version = 0x0102;
    b.custodyFreeKb = 31;
    len = encodeBeacon(b, buf, sizeof(buf));
    BeaconFrame bd;
    TEST_ASSERT_TRUE(decodeBeacon(buf, len, bd));
    TEST_ASSERT_EQUAL(b.caps, bd.caps);
    TEST_ASSERT_EQUAL_HEX16(0x0102, bd.version);

    StatusFrame s;
    s.msgId = 9;
    s.state = MsgState::IN_CUSTODY;
    s.node = 0xABCDEF01;
    len = encodeStatus(s, buf, sizeof(buf));
    StatusFrame sd;
    TEST_ASSERT_TRUE(decodeStatus(buf, len, sd));
    TEST_ASSERT_EQUAL(uint8_t(MsgState::IN_CUSTODY), uint8_t(sd.state));
    buf[3 + 4] = 99;
    TEST_ASSERT_FALSE(decodeStatus(buf, len, sd));
}

void test_pull_roundtrip_and_signing_payload()
{
    PullFrame p;
    p.nodeNum = 0x0B0B;
    p.afterSeq = 41;
    p.tsSec = 1790000000;
    for (int i = 0; i < 64; i++)
        p.sig[i] = uint8_t(i);
    uint8_t buf[OMP_MAX_FRAME];
    size_t n = encodePull(p, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(3 + 12 + 64, n);
    PullFrame d;
    TEST_ASSERT_TRUE(decodePull(buf, n, d));
    TEST_ASSERT_EQUAL_HEX32(0x0B0B, d.nodeNum);
    TEST_ASSERT_EQUAL(41, d.afterSeq);
    TEST_ASSERT_EQUAL_MEMORY(p.sig, d.sig, 64);
    TEST_ASSERT_FALSE(decodePull(buf, n - 1, d));
    // The server (mesh_gateway.js) rebuilds exactly these bytes after nodeNum|tsSec|port.
    uint8_t sp[8];
    pullSigningPayload(0x6A7E, 41, sp);
    const uint8_t expect[8] = {0x7E, 0x6A, 0, 0, 41, 0, 0, 0};
    TEST_ASSERT_EQUAL_MEMORY(expect, sp, 8);
}

// ---- fragmentation / reassembly ----

void test_fragment_count_boundaries()
{
    TEST_ASSERT_EQUAL(1, fragmentCount(0));
    TEST_ASSERT_EQUAL(1, fragmentCount(OMP_MAX_FRAG_DATA));
    TEST_ASSERT_EQUAL(2, fragmentCount(OMP_MAX_FRAG_DATA + 1));
    TEST_ASSERT_EQUAL(OMP_MAX_FRAGS, fragmentCount(OMP_MAX_MESSAGE));
    TEST_ASSERT_EQUAL(0, fragmentCount(OMP_MAX_MESSAGE + 1));
}

static Reassembler::Result feed(Reassembler &r, const Message &m, uint8_t idx, uint32_t now)
{
    uint8_t buf[OMP_MAX_FRAME];
    size_t n = buildFragment(m, idx, m.flags, buf, sizeof(buf));
    DataFrame f;
    if (!decodeData(buf, n, f))
        return Reassembler::Result::REJECTED;
    return r.accept(f, m.origin, now);
}

void test_reassembly_out_of_order_with_duplicates()
{
    Message m = makeMessage(42, 0x1, 0x2, OMP_MAX_FRAG_DATA * 2 + 17);
    Reassembler r;
    TEST_ASSERT_EQUAL(int(Reassembler::Result::NEW_FRAGMENT), int(feed(r, m, 2, 0)));
    TEST_ASSERT_EQUAL(int(Reassembler::Result::DUPLICATE), int(feed(r, m, 2, 1)));
    TEST_ASSERT_EQUAL(int(Reassembler::Result::NEW_FRAGMENT), int(feed(r, m, 0, 2)));
    uint8_t count;
    uint64_t have;
    TEST_ASSERT_TRUE(r.bitmap(m.origin, m.msgId, count, have));
    TEST_ASSERT_EQUAL_HEX64(0x5, have);
    TEST_ASSERT_EQUAL(int(Reassembler::Result::COMPLETE), int(feed(r, m, 1, 3)));
    Message out;
    TEST_ASSERT_TRUE(r.take(m.origin, m.msgId, out));
    TEST_ASSERT_EQUAL(m.body.size(), out.body.size());
    TEST_ASSERT_EQUAL_MEMORY(m.body.data(), out.body.data(), m.body.size());
    TEST_ASSERT_EQUAL(0, r.entryCount());
}

void test_reassembly_rejects_short_middle_fragment()
{
    uint8_t data[10] = {0};
    DataFrame f;
    f.msgId = 1;
    f.origin = 1;
    f.dest = 2;
    f.idx = 0;
    f.count = 2;
    f.data = data;
    f.len = sizeof(data);
    Reassembler r;
    TEST_ASSERT_EQUAL(int(Reassembler::Result::REJECTED), int(r.accept(f, 1, 0)));
}

void test_reassembly_is_bounded_in_entries_and_bytes()
{
    Reassembler::Limits lim;
    lim.maxEntries = 3;
    lim.maxBytes = OMP_MAX_FRAG_DATA * 4;
    Reassembler r(lim);
    for (uint32_t id = 1; id <= 10; id++) {
        Message m = makeMessage(id, 0x9, 0x2, OMP_MAX_FRAG_DATA * 8);
        feed(r, m, 0, id);
        feed(r, m, 1, id);
        TEST_ASSERT_TRUE(r.entryCount() <= lim.maxEntries);
        TEST_ASSERT_TRUE(r.bytesHeld() <= lim.maxBytes);
    }
}

void test_reassembly_expires_partial_entries()
{
    Reassembler::Limits lim;
    lim.ttlMs = 1000;
    Reassembler r(lim);
    Message m = makeMessage(5, 1, 2, OMP_MAX_FRAG_DATA + 1);
    feed(r, m, 0, 0);
    r.expire(1500);
    TEST_ASSERT_EQUAL(0, r.entryCount());
}

void test_seen_set_ring()
{
    SeenSet s(2);
    s.add(1, 1, 3);
    s.add(1, 2, 3);
    uint8_t c = 0;
    TEST_ASSERT_TRUE(s.contains(1, 1, &c));
    TEST_ASSERT_EQUAL(3, c);
    s.add(1, 3, 1);
    TEST_ASSERT_FALSE(s.contains(1, 1));
    TEST_ASSERT_TRUE(s.contains(1, 3));
}

// ---- outbox ----

static Outbox::Config fastConfig()
{
    Outbox::Config c;
    c.paceMs = 10;
    c.ackTimeoutBaseMs = 100;
    c.ackTimeoutPerFragMs = 0;
    c.maxRounds = 3;
    c.parkedRetryMs = 50;
    c.parkedTtlMs = 10000;
    return c;
}

// Drains every frame the outbox will send starting at *now, advancing the clock by the pace.
static std::vector<Outbox::Frame> drainFrames(Outbox &o, uint32_t &now)
{
    std::vector<Outbox::Frame> out;
    Outbox::Frame f;
    for (int guard = 0; guard < 200; guard++) {
        if (o.nextFrame(now, f))
            out.push_back(f);
        now += 10;
        if (!o.nextFrame(now, f))
            break;
        out.push_back(f);
        now += 10;
    }
    return out;
}

static bool hasStatus(const std::vector<StatusFrame> &v, MsgState st, uint32_t node = 0xFFFFFFFF)
{
    for (const auto &s : v)
        if (s.state == st && (node == 0xFFFFFFFF || s.node == node))
            return true;
    return false;
}

void test_outbox_paces_frames()
{
    Outbox o(fastConfig());
    o.enqueue(makeMessage(1, 0xA, 0xB, OMP_MAX_FRAG_DATA * 3), 0);
    Outbox::Frame f;
    TEST_ASSERT_TRUE(o.nextFrame(0, f));
    TEST_ASSERT_FALSE(o.nextFrame(5, f));
    TEST_ASSERT_TRUE(o.nextFrame(10, f));
}

void test_outbox_delivered_only_on_full_sack_from_link_peer()
{
    Outbox o(fastConfig());
    Message m = makeMessage(1, 0xA, 0xB, OMP_MAX_FRAG_DATA * 3);
    o.enqueue(m, 0);
    uint32_t now = 0;
    auto frames = drainFrames(o, now);
    TEST_ASSERT_EQUAL(3, frames.size());
    TEST_ASSERT_FALSE(frames[0].wantAck);
    TEST_ASSERT_TRUE(frames[2].wantAck);
    TEST_ASSERT_EQUAL_HEX32(0xB, frames[2].linkTo);

    SackFrame s{1, 0xA, 3, 0x7};
    o.onSack(0xC, s, now); // wrong peer: must be ignored
    auto st = o.drainStatus();
    TEST_ASSERT_FALSE(hasStatus(st, MsgState::DELIVERED));
    o.onSack(0xB, s, now);
    st = o.drainStatus();
    TEST_ASSERT_TRUE(hasStatus(st, MsgState::DELIVERED, 0xB));
    o.tick(now);
    TEST_ASSERT_EQUAL(0, o.size());
}

void test_outbox_repairs_only_missing_fragments()
{
    Outbox o(fastConfig());
    o.enqueue(makeMessage(2, 0xA, 0xB, OMP_MAX_FRAG_DATA * 5), 0);
    uint32_t now = 0;
    drainFrames(o, now);
    SackFrame s{2, 0xA, 5, 0x1B}; // missing idx 2
    o.onSack(0xB, s, now);
    auto frames = drainFrames(o, now);
    TEST_ASSERT_EQUAL(1, frames.size());
    DataFrame d;
    TEST_ASSERT_TRUE(decodeData(frames[0].bytes, frames[0].len, d));
    TEST_ASSERT_EQUAL(2, d.idx);
    TEST_ASSERT_TRUE(frames[0].wantAck);
}

void test_outbox_polls_with_last_fragment_on_timeout()
{
    Outbox o(fastConfig());
    o.enqueue(makeMessage(3, 0xA, 0xB, OMP_MAX_FRAG_DATA * 4), 0);
    uint32_t now = 0;
    drainFrames(o, now);
    now += 200;
    o.tick(now);
    auto frames = drainFrames(o, now);
    TEST_ASSERT_EQUAL(1, frames.size());
    DataFrame d;
    TEST_ASSERT_TRUE(decodeData(frames[0].bytes, frames[0].len, d));
    TEST_ASSERT_EQUAL(3, d.idx);
}

void test_outbox_hands_to_custodian_then_parks()
{
    Outbox o(fastConfig());
    o.setSelfNode(0xA);
    o.setCustodianPicker([](const Message &) { return 0xC0u; });
    o.enqueue(makeMessage(4, 0xA, 0xB, 10, FLAG_CUSTODY_OK), 0);
    uint32_t now = 0;
    for (int round = 0; round < 3; round++) {
        drainFrames(o, now);
        now += 200;
        o.tick(now);
    }
    auto frames = drainFrames(o, now);
    TEST_ASSERT_TRUE(frames.size() >= 1);
    TEST_ASSERT_EQUAL_HEX32(0xC0, frames[0].linkTo);
    DataFrame d;
    TEST_ASSERT_TRUE(decodeData(frames[0].bytes, frames[0].len, d));
    TEST_ASSERT_TRUE(d.flags & FLAG_CUSTODY_REQ);
    TEST_ASSERT_EQUAL_HEX32(0xB, d.dest);

    NoticeFrame n{4, 0xA, 0xB};
    o.onCustody(0xC0, n, now);
    auto st = o.drainStatus();
    TEST_ASSERT_TRUE(hasStatus(st, MsgState::IN_CUSTODY, 0xC0));
}

void test_outbox_self_custody_and_retry_when_dest_heard()
{
    Outbox o(fastConfig());
    o.setSelfNode(0xA);
    o.enqueue(makeMessage(5, 0xA, 0xB, 10), 0);
    uint32_t now = 0;
    for (int round = 0; round < 3; round++) {
        drainFrames(o, now);
        now += 200;
        o.tick(now);
    }
    TEST_ASSERT_TRUE(o.isParked(0xA, 5));
    auto parked = o.drainNewlyParked();
    TEST_ASSERT_EQUAL(1, parked.size());
    TEST_ASSERT_TRUE(hasStatus(o.drainStatus(), MsgState::IN_CUSTODY, 0xA));

    Outbox::Frame f;
    TEST_ASSERT_FALSE(o.nextFrame(now + 1000, f));
    o.onNodeHeard(0xB, now);
    auto frames = drainFrames(o, now);
    TEST_ASSERT_EQUAL(1, frames.size());
    SackFrame s{5, 0xA, 1, 0x1};
    o.onSack(0xB, s, now);
    TEST_ASSERT_TRUE(hasStatus(o.drainStatus(), MsgState::DELIVERED, 0xB));
    auto unparked = o.drainUnparked();
    TEST_ASSERT_EQUAL(1, unparked.size());
}

void test_outbox_parked_expires()
{
    Outbox o(fastConfig());
    o.enqueue(makeMessage(6, 0xA, 0xB, 10), 0, true);
    o.tick(20000);
    TEST_ASSERT_TRUE(hasStatus(o.drainStatus(), MsgState::FAILED));
    TEST_ASSERT_EQUAL(1, o.drainUnparked().size());
}

void test_outbox_broadcast_sent_once_no_ack()
{
    Outbox o(fastConfig());
    o.enqueue(makeMessage(7, 0xA, OMP_DEST_BROADCAST, OMP_MAX_FRAG_DATA * 2), 0);
    uint32_t now = 0;
    auto frames = drainFrames(o, now);
    TEST_ASSERT_EQUAL(2, frames.size());
    TEST_ASSERT_FALSE(frames[1].wantAck);
    TEST_ASSERT_TRUE(hasStatus(o.drainStatus(), MsgState::SENT));
    o.tick(now);
    TEST_ASSERT_EQUAL(0, o.size());
}

void test_outbox_rejects_oversize_and_full()
{
    Outbox::Config c = fastConfig();
    c.maxEntries = 1;
    Outbox o(c);
    TEST_ASSERT_FALSE(o.enqueue(makeMessage(8, 0xA, 0xB, OMP_MAX_MESSAGE + 1), 0));
    TEST_ASSERT_TRUE(o.enqueue(makeMessage(9, 0xA, 0xB, 10), 0));
    TEST_ASSERT_FALSE(o.enqueue(makeMessage(10, 0xA, 0xB, 10), 0));
    TEST_ASSERT_TRUE(hasStatus(o.drainStatus(), MsgState::REJECTED));
}

// ---- custody ----

void test_custody_roundtrip_and_corruption()
{
    CustodySet s;
    TEST_ASSERT_TRUE(s.add(makeMessage(1, 0xA, 0xB, 300)));
    TEST_ASSERT_TRUE(s.add(makeMessage(2, 0xA, 0xC, 0)));
    auto bytes = s.serialize();
    CustodySet t;
    TEST_ASSERT_TRUE(t.deserialize(bytes.data(), bytes.size()));
    TEST_ASSERT_EQUAL(2, t.all().size());
    TEST_ASSERT_EQUAL(300, t.all()[0].body.size());
    TEST_ASSERT_EQUAL_HEX32(0xC, t.all()[1].dest);

    bytes[bytes.size() - 1] ^= 0xFF;
    TEST_ASSERT_FALSE(t.deserialize(bytes.data(), bytes.size()));
    TEST_ASSERT_EQUAL(0, t.all().size());
    TEST_ASSERT_FALSE(t.deserialize(bytes.data(), 5));
}

void test_custody_capacity()
{
    CustodySet s(400);
    TEST_ASSERT_TRUE(s.add(makeMessage(1, 0xA, 0xB, 300)));
    TEST_ASSERT_FALSE(s.add(makeMessage(2, 0xA, 0xB, 300)));
    TEST_ASSERT_TRUE(s.remove(0xA, 1));
    TEST_ASSERT_TRUE(s.add(makeMessage(2, 0xA, 0xB, 300)));
}

// ---- gateway routing ----

void test_outbox_internet_without_gateway_fails()
{
    Outbox o(fastConfig());
    TEST_ASSERT_FALSE(o.enqueue(makeMessage(20, 0xA, OMP_DEST_INTERNET, 10), 0));
    TEST_ASSERT_TRUE(hasStatus(o.drainStatus(), MsgState::FAILED));
}

void test_outbox_internet_goes_to_gateway_and_reports_uplinked()
{
    Outbox o(fastConfig());
    o.setGatewayPicker([](const Message &) { return 0x6A7Eu; });
    TEST_ASSERT_TRUE(o.enqueue(makeMessage(21, 0xA, OMP_DEST_INTERNET, 10), 0));
    uint32_t now = 0;
    auto frames = drainFrames(o, now);
    TEST_ASSERT_EQUAL(1, frames.size());
    TEST_ASSERT_EQUAL_HEX32(0x6A7E, frames[0].linkTo);
    SackFrame s{21, 0xA, 1, 0x1};
    o.onSack(0x6A7E, s, now);
    auto st = o.drainStatus();
    TEST_ASSERT_TRUE(hasStatus(st, MsgState::UPLINKED, 0x6A7E));
    TEST_ASSERT_FALSE(hasStatus(st, MsgState::DELIVERED));
}

// ---- peers ----

void test_peers_pick_nearest_custodian_with_room()
{
    PeerTable t;
    BeaconFrame far{CAP_CUSTODIAN, 0x100, 30};
    BeaconFrame near{CAP_CUSTODIAN, 0x100, 30};
    BeaconFrame full{CAP_CUSTODIAN, 0x100, 0};
    t.onBeacon(1, far, 3, 0);
    t.onBeacon(2, near, 1, 0);
    t.onBeacon(3, full, 0, 0);
    TEST_ASSERT_EQUAL_HEX32(2, t.pickCustodian(0xB, 1000, 10));
    TEST_ASSERT_EQUAL_HEX32(1, t.pickCustodian(2, 1000, 10)); // never the destination itself
    TEST_ASSERT_EQUAL_HEX32(0, t.pickCustodian(0xB, 40 * 1024, 10));
}

void test_peers_stale_entries_ignored()
{
    PeerTable t(8, 1000);
    BeaconFrame gw{CAP_GATEWAY_ONLINE, 0x100, 0};
    t.onBeacon(7, gw, 1, 0);
    TEST_ASSERT_EQUAL_HEX32(7, t.pickGateway(500));
    TEST_ASSERT_EQUAL_HEX32(0, t.pickGateway(1500));
    TEST_ASSERT_FALSE(t.isOshiNode(7, 1500));
}

void test_peers_table_bounded()
{
    PeerTable t(4);
    BeaconFrame b{CAP_CUSTODIAN, 0x100, 10};
    for (uint32_t n = 1; n <= 10; n++)
        t.onBeacon(n, b, 1, n);
    TEST_ASSERT_EQUAL(4, t.all().size());
    TEST_ASSERT_TRUE(t.isOshiNode(10, 11));
    TEST_ASSERT_FALSE(t.isOshiNode(1, 11));
}

// ---- gateway codec ----

void test_base64_roundtrip_all_lengths()
{
    uint8_t data[40];
    for (int i = 0; i < 40; i++)
        data[i] = uint8_t(i * 37 + 1);
    for (size_t n = 0; n <= sizeof(data); n++) {
        std::string e = base64Encode(data, n);
        TEST_ASSERT_EQUAL(((n + 2) / 3) * 4, e.size());
        std::vector<uint8_t> d;
        TEST_ASSERT_TRUE(base64Decode(e, d));
        TEST_ASSERT_EQUAL(n, d.size());
        if (n)
            TEST_ASSERT_EQUAL_MEMORY(data, d.data(), n);
    }
    std::vector<uint8_t> d;
    TEST_ASSERT_TRUE(base64Decode("-_8=", d)); // url-safe spelling decodes too
    TEST_ASSERT_EQUAL(2, d.size());
    TEST_ASSERT_FALSE(base64Decode("ab$c", d));
}

void test_pull_response_parse()
{
    // Shape produced by mesh_gateway.js handlePull (JSON.stringify key order).
    const std::string json = "{\"nodeNum\":2827,\"frames\":[{\"seq\":17,\"frame\":\"RAEEEQAAAA==\"},"
                             "{\"seq\":18,\"frame\":\"RAEEEgAAAA==\"}],\"maxSeq\":18,\"more\":false}";
    uint32_t node = 0;
    std::vector<DownlinkFrame> frames;
    TEST_ASSERT_TRUE(parsePullResponse(json, node, frames));
    TEST_ASSERT_EQUAL(2827, node);
    TEST_ASSERT_EQUAL(2, frames.size());
    TEST_ASSERT_EQUAL(17, frames[0].seq);
    uint32_t seq = 0;
    TEST_ASSERT_TRUE(downlinkSeq(frames[1].bytes.data(), frames[1].bytes.size(), seq));
    TEST_ASSERT_EQUAL(18, seq);
    TEST_ASSERT_FALSE(parsePullResponse("{\"error\":\"unbound-node\"}", node, frames));
}

void test_request_bodies()
{
    const uint8_t f[3] = {1, 2, 3};
    TEST_ASSERT_EQUAL_STRING("{\"gateway\":27262,\"frame\":\"AQID\"}", uplinkBody(0x6A7E, f, 3).c_str());
    TEST_ASSERT_EQUAL_STRING("{\"gateway\":1,\"pull\":\"AQID\"}", pullBody(1, f, 3).c_str());
}

// ---- router policies ----

void test_unsigned_from_signer_only_excused_by_a_legacy_relay()
{
    TEST_ASSERT_FALSE(unsignedExplainedByLegacyRelay(true, false, false));  // heard from the originator itself
    TEST_ASSERT_FALSE(unsignedExplainedByLegacyRelay(false, true, true));   // relayed by a node that keeps signatures
    TEST_ASSERT_TRUE(unsignedExplainedByLegacyRelay(false, true, false));   // relayed by a legacy node
    TEST_ASSERT_TRUE(unsignedExplainedByLegacyRelay(false, false, false));  // relay unknown: benefit of the doubt
    TEST_ASSERT_TRUE(heardDirect(3, 3));
    TEST_ASSERT_FALSE(heardDirect(3, 2));
    TEST_ASSERT_FALSE(heardDirect(0, 0)); // pre-hop_start sender
}

void test_duty_cycle_shedding_keeps_acks_and_dms()
{
    const float dc = 10.0f;
    TEST_ASSERT_FALSE(shedUnderDutyCycle(5.0f, dc, false, true, PRIORITY_DEFAULT));   // plenty of budget
    TEST_ASSERT_TRUE(shedUnderDutyCycle(9.0f, dc, false, true, PRIORITY_DEFAULT));    // relayed broadcast shed
    TEST_ASSERT_TRUE(shedUnderDutyCycle(9.0f, dc, false, true, 0));                   // unset priority = default
    TEST_ASSERT_FALSE(shedUnderDutyCycle(9.0f, dc, false, false, PRIORITY_DEFAULT));  // relayed DM kept
    TEST_ASSERT_FALSE(shedUnderDutyCycle(9.0f, dc, false, true, 120));                // relayed ACK kept
    TEST_ASSERT_TRUE(shedUnderDutyCycle(9.0f, dc, true, true, PRIORITY_BACKGROUND));  // our telemetry shed
    TEST_ASSERT_FALSE(shedUnderDutyCycle(9.0f, dc, true, false, PRIORITY_RELIABLE));  // our DM kept
    TEST_ASSERT_FALSE(shedUnderDutyCycle(11.0f, dc, false, true, PRIORITY_DEFAULT));  // over: hard gate's job
    TEST_ASSERT_FALSE(shedUnderDutyCycle(99.0f, 100.0f, false, true, PRIORITY_DEFAULT)); // no duty cycle region
}

void test_outbox_sent_reported_once_across_parked_retries()
{
    Outbox o(fastConfig());
    o.setSelfNode(0xA);
    o.enqueue(makeMessage(30, 0xA, 0xB, 10), 0);
    uint32_t now = 0;
    for (int round = 0; round < 3; round++) {
        drainFrames(o, now);
        now += 200;
        o.tick(now);
    }
    o.onNodeHeard(0xB, now);
    drainFrames(o, now);
    int sent = 0;
    for (const auto &s : o.drainStatus())
        sent += s.state == MsgState::SENT;
    TEST_ASSERT_EQUAL(1, sent);
}

void test_peers_skip_unaddressable()
{
    PeerTable t;
    BeaconFrame c{CAP_CUSTODIAN | CAP_GATEWAY_ONLINE, 0x100, 30};
    t.onBeacon(1, c, 1, 0);
    t.onBeacon(2, c, 3, 0);
    auto onlyTwo = [](uint32_t n) { return n == 2; };
    TEST_ASSERT_EQUAL_HEX32(1, t.pickCustodian(0xB, 100, 10));
    TEST_ASSERT_EQUAL_HEX32(2, t.pickCustodian(0xB, 100, 10, onlyTwo));
    TEST_ASSERT_EQUAL_HEX32(2, t.pickGateway(10, onlyTwo));
    TEST_ASSERT_EQUAL_HEX32(0, t.pickGateway(10, [](uint32_t) { return false; }));
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_data_roundtrip);
    RUN_TEST(test_legacy_om_frame_is_not_omp);
    RUN_TEST(test_data_rejects_idx_out_of_range_and_truncation);
    RUN_TEST(test_wrong_version_rejected);
    RUN_TEST(test_sack_bitmap_masked_to_count);
    RUN_TEST(test_notice_beacon_status_roundtrip);
    RUN_TEST(test_pull_roundtrip_and_signing_payload);
    RUN_TEST(test_fragment_count_boundaries);
    RUN_TEST(test_reassembly_out_of_order_with_duplicates);
    RUN_TEST(test_reassembly_rejects_short_middle_fragment);
    RUN_TEST(test_reassembly_is_bounded_in_entries_and_bytes);
    RUN_TEST(test_reassembly_expires_partial_entries);
    RUN_TEST(test_seen_set_ring);
    RUN_TEST(test_outbox_paces_frames);
    RUN_TEST(test_outbox_delivered_only_on_full_sack_from_link_peer);
    RUN_TEST(test_outbox_repairs_only_missing_fragments);
    RUN_TEST(test_outbox_polls_with_last_fragment_on_timeout);
    RUN_TEST(test_outbox_hands_to_custodian_then_parks);
    RUN_TEST(test_outbox_self_custody_and_retry_when_dest_heard);
    RUN_TEST(test_outbox_parked_expires);
    RUN_TEST(test_outbox_broadcast_sent_once_no_ack);
    RUN_TEST(test_outbox_rejects_oversize_and_full);
    RUN_TEST(test_custody_roundtrip_and_corruption);
    RUN_TEST(test_custody_capacity);
    RUN_TEST(test_outbox_internet_without_gateway_fails);
    RUN_TEST(test_outbox_internet_goes_to_gateway_and_reports_uplinked);
    RUN_TEST(test_peers_pick_nearest_custodian_with_room);
    RUN_TEST(test_peers_stale_entries_ignored);
    RUN_TEST(test_peers_table_bounded);
    RUN_TEST(test_base64_roundtrip_all_lengths);
    RUN_TEST(test_pull_response_parse);
    RUN_TEST(test_request_bodies);
    RUN_TEST(test_unsigned_from_signer_only_excused_by_a_legacy_relay);
    RUN_TEST(test_duty_cycle_shedding_keeps_acks_and_dms);
    RUN_TEST(test_outbox_sent_reported_once_across_parked_retries);
    RUN_TEST(test_peers_skip_unaddressable);
    exit(UNITY_END());
}

void loop() {}
