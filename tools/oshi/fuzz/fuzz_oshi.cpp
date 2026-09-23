// libFuzzer harness for everything in src/oshi that reads bytes it did not write: OMP frames heard on air, fragment
// streams fed to the Reassembler, custody records read back from flash, and gateway responses from the internet.
// Besides "no crash, no sanitizer report" it asserts the invariants the firmware relies on.
//
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -I src/oshi \
//     tools/oshi/fuzz/fuzz_oshi.cpp src/oshi/OshiProtocol.cpp src/oshi/OshiMessage.cpp src/oshi/OshiCustody.cpp \
//     src/oshi/OshiGatewayCodec.cpp -o fuzz_oshi && ./fuzz_oshi -max_total_time=300 tools/oshi/fuzz/corpus

#include "OshiCustody.h"
#include "OshiGatewayCodec.h"
#include "OshiMessage.h"
#include "OshiProtocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace oshi;

#define REQUIRE(c)                                                                                                     \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            std::fprintf(stderr, "invariant failed: %s (%s:%d)\n", #c, __FILE__, __LINE__);                             \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

// Every decoder, on arbitrary bytes: a frame that decodes must re-encode to exactly the bytes it came from.
static void frames(const uint8_t *d, size_t n)
{
    uint8_t out[OMP_MAX_FRAME + 64];
    FrameType t;
    bool typed = frameType(d, n, t);
    REQUIRE(!typed || isOmpFrame(d, n)); // an unknown type nibble is still OMP (forward compatible), just untyped

    DataFrame df;
    if (decodeData(d, n, df)) {
        REQUIRE(df.count > 0 && df.count <= OMP_MAX_FRAGS && df.idx < df.count);
        REQUIRE(df.len <= OMP_MAX_FRAG_DATA && df.len + OMP_DATA_HEADER == n);
        size_t m = encodeData(df, out, sizeof(out));
        REQUIRE(m == n && std::memcmp(out, d, n) == 0);
    }
    SackFrame sf;
    if (decodeSack(d, n, sf)) {
        REQUIRE(sf.count > 0 && sf.count <= OMP_MAX_FRAGS);
        REQUIRE((sf.bitmap & ~fullBitmap(sf.count)) == 0);
    }
    NoticeFrame nf;
    if (decodeNotice(d, n, FrameType::CUSTODY, nf) || decodeNotice(d, n, FrameType::RECEIPT, nf)) {
        size_t m = encodeNotice(t, nf, out, sizeof(out));
        REQUIRE(m > 0 && m <= n && std::memcmp(out, d, m) == 0);
    }
    BeaconFrame bf;
    if (decodeBeacon(d, n, bf)) {
        size_t m = encodeBeacon(bf, out, sizeof(out));
        REQUIRE(m > 0 && m <= n && std::memcmp(out, d, m) == 0);
    }
    StatusFrame st;
    if (decodeStatus(d, n, st)) {
        size_t m = encodeStatus(st, out, sizeof(out));
        REQUIRE(m > 0 && m <= n && std::memcmp(out, d, m) == 0);
    }
    PullFrame pf;
    if (decodePull(d, n, pf)) {
        size_t m = encodePull(pf, out, sizeof(out));
        REQUIRE(m > 0 && m <= n && std::memcmp(out, d, m) == 0);
    }
}

// The input as a stream of on-air fragments: [len u8][sender-byte u8][dt u8][frame bytes...] repeated. Whatever
// arrives, the Reassembler stays inside its limits and only ever completes a message it can account for.
static void reassembly(const uint8_t *d, size_t n)
{
    Reassembler::Limits lim;
    lim.maxEntries = 4;
    lim.maxBytes = 4096;
    lim.ttlMs = 60000;
    Reassembler rx(lim);
    uint32_t now = 1;
    size_t i = 0;
    while (i + 3 <= n) {
        size_t len = d[i];
        uint32_t sender = d[i + 1];
        now += uint32_t(d[i + 2]) * 1000;
        i += 3;
        if (len > n - i)
            len = n - i;
        DataFrame f;
        if (decodeData(d + i, len, f)) {
            Reassembler::Result r = rx.accept(f, sender, now);
            REQUIRE(rx.entryCount() <= lim.maxEntries);
            REQUIRE(rx.bytesHeld() <= lim.maxBytes + OMP_MAX_FRAG_DATA * OMP_MAX_FRAGS);
            uint8_t count = 0;
            uint64_t have = 0;
            if (rx.bitmap(f.origin, f.msgId, count, have))
                REQUIRE(count > 0 && count <= OMP_MAX_FRAGS && (have & ~fullBitmap(count)) == 0);
            if (r == Reassembler::Result::COMPLETE) {
                Message msg;
                REQUIRE(rx.take(f.origin, f.msgId, msg));
                REQUIRE(msg.body.size() <= OMP_MAX_MESSAGE);
                REQUIRE(msg.origin == f.origin && msg.msgId == f.msgId);
            }
        }
        rx.expire(now);
        i += len;
    }
}

// Custody records come back from flash, which may be truncated or corrupted: accepted means it survives a round trip.
static void custody(const uint8_t *d, size_t n)
{
    CustodySet c(8 * 1024);
    if (c.deserialize(d, n)) {
        REQUIRE(c.bytesUsed() <= 8 * 1024 + OMP_MAX_MESSAGE);
        std::vector<uint8_t> again = c.serialize();
        CustodySet c2(8 * 1024);
        REQUIRE(c2.deserialize(again.data(), again.size()));
        REQUIRE(c2.all().size() == c.all().size());
        REQUIRE(c2.serialize() == again);
    } else {
        REQUIRE(c.all().empty());
    }
}

// Responses from the internet gateway server.
static void gateway(const uint8_t *d, size_t n)
{
    std::string s(reinterpret_cast<const char *>(d), n);
    std::vector<uint8_t> raw;
    if (base64Decode(s, raw)) {
        std::string back = base64Encode(raw.data(), raw.size());
        std::vector<uint8_t> raw2;
        REQUIRE(base64Decode(back, raw2) && raw2 == raw);
    }
    uint32_t node = 0;
    std::vector<DownlinkFrame> fr;
    parsePullResponse(s, node, fr);
    uint32_t seq = 0;
    downlinkSeq(d, n, seq);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0)
        return 0;
    // The first byte picks the target, so one corpus exercises all four.
    const uint8_t *d = data + 1;
    size_t n = size - 1;
    switch (data[0] & 3) {
    case 0:
        frames(d, n);
        break;
    case 1:
        reassembly(d, n);
        break;
    case 2:
        custody(d, n);
        break;
    default:
        gateway(d, n);
        break;
    }
    return 0;
}
