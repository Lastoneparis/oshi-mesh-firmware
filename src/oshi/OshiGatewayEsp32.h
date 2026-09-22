#pragma once

#include "configuration.h"

#if defined(ARCH_ESP32) && HAS_WIFI && !MESHTASTIC_EXCLUDE_WIFI && !MESHTASTIC_EXCLUDE_OSHI

#include "OshiGatewayLink.h"
#include "concurrency/Lock.h"
#include <atomic>
#include <deque>
#include <vector>

namespace oshi
{

// HTTPS link to /v2/mesh on its own FreeRTOS task: a TLS handshake blocks for seconds, which would stall
// the cooperative OSThread scheduler and with it the radio.
class Esp32Gateway : public GatewayLink
{
  public:
    explicit Esp32Gateway(uint32_t selfNode);
    bool online() const override;
    bool uplink(const Message &msg) override;
    bool forwardPull(uint32_t fromNode, const uint8_t *pull, size_t len) override;
    void onDownlinkResult(uint32_t msgId, bool delivered) override {}
    void loop(uint32_t nowMs) override;

  private:
    struct Job {
        enum Kind { UPLINK, PULL } kind = UPLINK;
        uint32_t node = 0;
        std::vector<uint8_t> bytes;
    };
    struct Result {
        uint32_t node;
        uint32_t seq;
        std::vector<uint8_t> bytes;
    };

    static void taskEntry(void *arg);
    void taskLoop();
    bool push(Job &&j);
    int post(const char *route, const std::string &body, std::string &response);

    uint32_t self;
    std::atomic<bool> backingOff{false};
    concurrency::Lock lock;
    std::deque<Job> jobs;
    std::vector<Result> results;
};

} // namespace oshi

#endif
