#include "OshiPeers.h"
#include <algorithm>

namespace oshi
{

void PeerTable::onBeacon(uint32_t node, const BeaconFrame &b, uint8_t hops, uint32_t nowMs)
{
    for (auto &p : peers) {
        if (p.node == node) {
            p.caps = b.caps;
            p.freeKb = b.custodyFreeKb;
            p.hops = hops;
            p.lastHeardMs = nowMs;
            return;
        }
    }
    if (peers.size() >= cap) {
        auto oldest = std::min_element(peers.begin(), peers.end(), [&](const Peer &a, const Peer &c) {
            return nowMs - a.lastHeardMs > nowMs - c.lastHeardMs;
        });
        peers.erase(oldest);
    }
    peers.push_back({node, b.caps, b.custodyFreeKb, hops, nowMs});
}

uint32_t PeerTable::pickCustodian(uint32_t exclude, size_t bodyLen, uint32_t nowMs, const Usable &usable) const
{
    const Peer *best = nullptr;
    for (const auto &p : peers) {
        if (p.node == exclude || !(p.caps & CAP_CUSTODIAN) || !isFresh(p, nowMs) || size_t(p.freeKb) * 1024 < bodyLen ||
            (usable && !usable(p.node)))
            continue;
        if (!best || p.hops < best->hops || (p.hops == best->hops && p.freeKb > best->freeKb))
            best = &p;
    }
    return best ? best->node : 0;
}

uint32_t PeerTable::pickGateway(uint32_t nowMs, const Usable &usable) const
{
    const Peer *best = nullptr;
    for (const auto &p : peers) {
        if (!(p.caps & CAP_GATEWAY_ONLINE) || !isFresh(p, nowMs) || (usable && !usable(p.node)))
            continue;
        if (!best || p.hops < best->hops)
            best = &p;
    }
    return best ? best->node : 0;
}

bool PeerTable::hasCap(uint32_t node, uint8_t cap, uint32_t nowMs) const
{
    for (const auto &p : peers)
        if (p.node == node)
            return isFresh(p, nowMs) && (p.caps & cap);
    return false;
}

bool PeerTable::isOshiNode(uint32_t node, uint32_t nowMs) const
{
    for (const auto &p : peers)
        if (p.node == node)
            return isFresh(p, nowMs);
    return false;
}

} // namespace oshi
