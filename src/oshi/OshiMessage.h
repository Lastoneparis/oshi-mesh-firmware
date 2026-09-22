#pragma once

#include "OshiProtocol.h"
#include <cstdint>
#include <vector>

namespace oshi
{

struct Message {
    uint32_t msgId = 0;
    uint32_t origin = 0;
    uint32_t dest = 0;
    uint8_t flags = 0;
    std::vector<uint8_t> body;
};

inline uint8_t fragmentCount(size_t bodyLen)
{
    if (bodyLen == 0)
        return 1;
    size_t n = (bodyLen + OMP_MAX_FRAG_DATA - 1) / OMP_MAX_FRAG_DATA;
    return n > OMP_MAX_FRAGS ? 0 : uint8_t(n);
}

// Encodes fragment idx of msg with the given on-air flags. Returns the frame length, 0 if idx is out of range.
size_t buildFragment(const Message &msg, uint8_t idx, uint8_t flags, uint8_t *out, size_t cap);

// Completed (origin, msgId) pairs, so a retransmitted fragment is re-acknowledged instead of re-delivered.
class SeenSet
{
  public:
    explicit SeenSet(size_t capacity = 64) : cap(capacity) {}
    bool contains(uint32_t origin, uint32_t msgId, uint8_t *countOut = nullptr) const;
    void add(uint32_t origin, uint32_t msgId, uint8_t count);

  private:
    struct Item {
        uint32_t origin;
        uint32_t msgId;
        uint8_t count;
    };
    size_t cap;
    size_t next = 0;
    std::vector<Item> items;
};

class Reassembler
{
  public:
    enum class Result { NEW_FRAGMENT, DUPLICATE, COMPLETE, REJECTED };

    struct Limits {
        size_t maxEntries = 6;
        size_t maxBytes = 24 * 1024;
        uint32_t ttlMs = 5 * 60 * 1000;
    };

    Reassembler() = default;
    explicit Reassembler(const Limits &l) : limits(l) {}

    // sender is the node the frame arrived from (the origin, or a custodian); it is where the SACK goes.
    Result accept(const DataFrame &f, uint32_t sender, uint32_t nowMs);
    bool bitmap(uint32_t origin, uint32_t msgId, uint8_t &count, uint64_t &have) const;
    // Removes a complete entry and returns it assembled.
    bool take(uint32_t origin, uint32_t msgId, Message &out);
    void expire(uint32_t nowMs);
    size_t entryCount() const { return entries.size(); }
    size_t bytesHeld() const;

  private:
    struct Entry {
        uint32_t origin;
        uint32_t msgId;
        uint32_t dest;
        uint8_t count;
        uint8_t flags;
        uint64_t have;
        uint32_t firstMs;
        std::vector<std::vector<uint8_t>> frags;
    };
    Entry *find(uint32_t origin, uint32_t msgId);
    const Entry *find(uint32_t origin, uint32_t msgId) const;
    void evictOldest();

    Limits limits;
    std::vector<Entry> entries;
};

} // namespace oshi
