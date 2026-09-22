#pragma once

#include "OshiMessage.h"
#include <cstdint>
#include <vector>

namespace oshi
{

constexpr size_t CUSTODY_MAX_BYTES = 32 * 1024;

// Parked messages, kept across reboots. Serialization is separate from storage so it is testable
// without a filesystem.
class CustodySet
{
  public:
    explicit CustodySet(size_t maxBytes = CUSTODY_MAX_BYTES) : cap(maxBytes) {}

    bool add(const Message &m);
    bool remove(uint32_t origin, uint32_t msgId);
    const std::vector<Message> &all() const { return items; }
    size_t bytesUsed() const;
    size_t bytesFree() const { return cap > bytesUsed() ? cap - bytesUsed() : 0; }

    std::vector<uint8_t> serialize() const;
    // Replaces the contents. Rejects a bad magic, a CRC mismatch or a truncated record, leaving the set empty.
    bool deserialize(const uint8_t *buf, size_t len);

  private:
    size_t cap;
    std::vector<Message> items;
};

} // namespace oshi
