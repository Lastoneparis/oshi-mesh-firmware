#include "OshiMessage.h"
#include <algorithm>

namespace oshi
{

size_t buildFragment(const Message &msg, uint8_t idx, uint8_t flags, uint8_t *out, size_t cap)
{
    uint8_t count = fragmentCount(msg.body.size());
    if (count == 0 || idx >= count)
        return 0;
    DataFrame f;
    f.msgId = msg.msgId;
    f.origin = msg.origin;
    f.dest = msg.dest;
    f.idx = idx;
    f.count = count;
    f.flags = flags;
    size_t off = size_t(idx) * OMP_MAX_FRAG_DATA;
    f.len = std::min(OMP_MAX_FRAG_DATA, msg.body.size() - std::min(off, msg.body.size()));
    f.data = f.len ? msg.body.data() + off : nullptr;
    return encodeData(f, out, cap);
}

bool SeenSet::contains(uint32_t origin, uint32_t msgId, uint8_t *countOut) const
{
    for (const auto &it : items) {
        if (it.origin == origin && it.msgId == msgId) {
            if (countOut)
                *countOut = it.count;
            return true;
        }
    }
    return false;
}

void SeenSet::add(uint32_t origin, uint32_t msgId, uint8_t count)
{
    if (contains(origin, msgId))
        return;
    if (items.size() < cap) {
        items.push_back({origin, msgId, count});
        return;
    }
    items[next] = {origin, msgId, count};
    next = (next + 1) % cap;
}

Reassembler::Entry *Reassembler::find(uint32_t origin, uint32_t msgId)
{
    for (auto &e : entries)
        if (e.origin == origin && e.msgId == msgId)
            return &e;
    return nullptr;
}

const Reassembler::Entry *Reassembler::find(uint32_t origin, uint32_t msgId) const
{
    for (const auto &e : entries)
        if (e.origin == origin && e.msgId == msgId)
            return &e;
    return nullptr;
}

size_t Reassembler::bytesHeld() const
{
    size_t n = 0;
    for (const auto &e : entries)
        for (const auto &f : e.frags)
            n += f.size();
    return n;
}

void Reassembler::evictOldest()
{
    if (entries.empty())
        return;
    auto oldest = std::min_element(entries.begin(), entries.end(),
                                   [](const Entry &a, const Entry &b) { return int32_t(a.firstMs - b.firstMs) < 0; });
    entries.erase(oldest);
}

void Reassembler::expire(uint32_t nowMs)
{
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const Entry &e) { return nowMs - e.firstMs >= limits.ttlMs; }),
                  entries.end());
}

Reassembler::Result Reassembler::accept(const DataFrame &f, uint32_t sender, uint32_t nowMs)
{
    (void)sender;
    if (f.count == 0 || f.count > OMP_MAX_FRAGS || f.idx >= f.count || f.len > OMP_MAX_FRAG_DATA)
        return Result::REJECTED;
    // Every fragment but the last must be full, so the assembled length is unambiguous.
    if (f.idx + 1 < f.count && f.len != OMP_MAX_FRAG_DATA)
        return Result::REJECTED;

    expire(nowMs);
    Entry *e = find(f.origin, f.msgId);
    if (e && (e->count != f.count || e->dest != f.dest))
        return Result::REJECTED;
    if (!e) {
        while (entries.size() >= limits.maxEntries)
            evictOldest();
        entries.push_back(Entry{f.origin, f.msgId, f.dest, f.count, f.flags, 0, nowMs, {}});
        e = &entries.back();
        e->frags.resize(f.count);
    }

    uint64_t bit = 1ULL << f.idx;
    if (e->have & bit)
        return Result::DUPLICATE;

    while (bytesHeld() + f.len > limits.maxBytes && entries.size() > 1) {
        uint32_t o = e->origin, m = e->msgId;
        auto victim = std::min_element(entries.begin(), entries.end(), [&](const Entry &a, const Entry &b) {
            bool aSelf = a.origin == o && a.msgId == m, bSelf = b.origin == o && b.msgId == m;
            if (aSelf != bSelf)
                return bSelf;
            return int32_t(a.firstMs - b.firstMs) < 0;
        });
        if (victim->origin == o && victim->msgId == m)
            break;
        entries.erase(victim);
        e = find(o, m);
    }
    if (bytesHeld() + f.len > limits.maxBytes)
        return Result::REJECTED;

    e->frags[f.idx].assign(f.data, f.data + f.len);
    e->have |= bit;
    e->flags |= f.flags;
    return e->have == fullBitmap(e->count) ? Result::COMPLETE : Result::NEW_FRAGMENT;
}

bool Reassembler::bitmap(uint32_t origin, uint32_t msgId, uint8_t &count, uint64_t &have) const
{
    const Entry *e = find(origin, msgId);
    if (!e)
        return false;
    count = e->count;
    have = e->have;
    return true;
}

bool Reassembler::take(uint32_t origin, uint32_t msgId, Message &out)
{
    Entry *e = find(origin, msgId);
    if (!e || e->have != fullBitmap(e->count))
        return false;
    out.msgId = e->msgId;
    out.origin = e->origin;
    out.dest = e->dest;
    out.flags = e->flags;
    out.body.clear();
    for (const auto &f : e->frags)
        out.body.insert(out.body.end(), f.begin(), f.end());
    entries.erase(entries.begin() + (e - entries.data()));
    return true;
}

} // namespace oshi
