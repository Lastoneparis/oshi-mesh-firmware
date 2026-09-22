#include "OshiCustody.h"
#include <algorithm>

namespace oshi
{

namespace
{
constexpr uint32_t CUSTODY_MAGIC = 0x3143534F; // "OSC1"
constexpr size_t RECORD_HEADER = 4 + 4 + 4 + 1 + 2;

void put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 0; i < 4; i++)
        v.push_back((x >> (8 * i)) & 0xFF);
}

uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320 & (0u - (c & 1)));
    }
    return ~c;
}

size_t recordSize(const Message &m)
{
    return RECORD_HEADER + m.body.size();
}
} // namespace

size_t CustodySet::bytesUsed() const
{
    size_t n = 0;
    for (const auto &m : items)
        n += recordSize(m);
    return n;
}

bool CustodySet::add(const Message &m)
{
    if (m.body.size() > OMP_MAX_MESSAGE)
        return false;
    for (const auto &it : items)
        if (it.origin == m.origin && it.msgId == m.msgId)
            return true;
    if (bytesUsed() + recordSize(m) > cap)
        return false;
    items.push_back(m);
    return true;
}

bool CustodySet::remove(uint32_t origin, uint32_t msgId)
{
    auto it = std::find_if(items.begin(), items.end(),
                           [&](const Message &m) { return m.origin == origin && m.msgId == msgId; });
    if (it == items.end())
        return false;
    items.erase(it);
    return true;
}

std::vector<uint8_t> CustodySet::serialize() const
{
    std::vector<uint8_t> body;
    for (const auto &m : items) {
        put32(body, m.msgId);
        put32(body, m.origin);
        put32(body, m.dest);
        body.push_back(m.flags);
        body.push_back(m.body.size() & 0xFF);
        body.push_back(m.body.size() >> 8);
        body.insert(body.end(), m.body.begin(), m.body.end());
    }
    std::vector<uint8_t> out;
    put32(out, CUSTODY_MAGIC);
    put32(out, uint32_t(items.size()));
    put32(out, crc32(body.data(), body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool CustodySet::deserialize(const uint8_t *buf, size_t len)
{
    items.clear();
    if (!buf || len < 12 || get32(buf) != CUSTODY_MAGIC)
        return false;
    uint32_t count = get32(buf + 4);
    if (crc32(buf + 12, len - 12) != get32(buf + 8))
        return false;
    const uint8_t *p = buf + 12, *end = buf + len;
    std::vector<Message> parsed;
    for (uint32_t i = 0; i < count; i++) {
        if (size_t(end - p) < RECORD_HEADER)
            return false;
        Message m;
        m.msgId = get32(p);
        m.origin = get32(p + 4);
        m.dest = get32(p + 8);
        m.flags = p[12];
        size_t n = p[13] | (size_t(p[14]) << 8);
        p += RECORD_HEADER;
        if (size_t(end - p) < n || n > OMP_MAX_MESSAGE)
            return false;
        m.body.assign(p, p + n);
        p += n;
        parsed.push_back(std::move(m));
    }
    if (p != end)
        return false;
    items = std::move(parsed);
    return true;
}

} // namespace oshi
