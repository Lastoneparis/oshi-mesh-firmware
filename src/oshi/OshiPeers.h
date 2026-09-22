#pragma once

#include "OshiProtocol.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace oshi
{

// OSHI nodes learned from their beacons, and the choice of custodian/gateway among them.
class PeerTable
{
  public:
    struct Peer {
        uint32_t node;
        uint8_t caps;
        uint8_t freeKb;
        uint8_t hops;
        uint32_t lastHeardMs;
    };

    explicit PeerTable(size_t capacity = 32, uint32_t freshMs = 60 * 60 * 1000) : cap(capacity), fresh(freshMs) {}

    void onBeacon(uint32_t node, const BeaconFrame &b, uint8_t hops, uint32_t nowMs);
    // Nearest fresh custodian with room for bodyLen, excluding the destination and self. 0 if none.
    uint32_t pickCustodian(uint32_t exclude, size_t bodyLen, uint32_t nowMs) const;
    // Nearest fresh node whose beacon says its gateway is online. 0 if none.
    uint32_t pickGateway(uint32_t nowMs) const;
    bool isOshiNode(uint32_t node, uint32_t nowMs) const;
    const std::vector<Peer> &all() const { return peers; }

  private:
    bool isFresh(const Peer &p, uint32_t nowMs) const { return nowMs - p.lastHeardMs < fresh; }
    size_t cap;
    uint32_t fresh;
    std::vector<Peer> peers;
};

} // namespace oshi
