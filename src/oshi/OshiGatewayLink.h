#pragma once

#include "OshiMessage.h"

namespace oshi
{

// The internet side of an OSHI gateway node. Implemented per platform (ESP32 WiFi); absent elsewhere.
class GatewayLink
{
  public:
    virtual ~GatewayLink() = default;
    virtual bool online() const = 0;
    // Queues a complete message for upload. False when the queue is full.
    virtual bool uplink(const Message &msg) = 0;
    // Whether uplink() would accept one more message; checked BEFORE acknowledging it to the sender.
    virtual bool hasRoom() const = 0;
    // Forwards a node's signed PULL (nodeNum|afterSeq|tsSec|sig, as the server expects it). False when busy.
    virtual bool forwardPull(uint32_t fromNode, const uint8_t *pull, size_t len) = 0;
    virtual void onDownlinkResult(uint32_t msgId, bool delivered) = 0;
    virtual void loop(uint32_t nowMs) = 0;
};

} // namespace oshi

extern oshi::GatewayLink *oshiGateway;
