#pragma once

#include "OshiMessage.h"
#include <functional>
#include <vector>

namespace oshi
{

// Outbound OMP messages: paced fragment transmission, selective-ACK repair, then custody when the
// destination stays silent. Pure logic - the caller owns the radio and the clock.
class Outbox
{
  public:
    struct Config {
        uint32_t paceMs = 2500;
        uint32_t ackTimeoutBaseMs = 25000;
        uint32_t ackTimeoutPerFragMs = 3000;
        uint8_t maxRounds = 5;
        uint32_t parkedTtlMs = 72UL * 3600 * 1000;
        // A parked message is retried at most this often, even if its destination is heard constantly.
        uint32_t parkedRetryMs = 60 * 1000;
        size_t maxEntries = 16;
        bool selfCustody = true;
    };

    struct Frame {
        uint32_t linkTo = 0; // next recipient: the destination, a custodian, or broadcast
        bool wantAck = false;
        uint8_t len = 0;
        uint8_t bytes[OMP_MAX_FRAME];
    };

    // Returns the custodian to hand a message to, or 0 for none.
    using CustodianPicker = std::function<uint32_t(const Message &)>;

    Outbox() = default;
    explicit Outbox(const Config &c) : cfg(c) {}

    void setCustodianPicker(CustodianPicker p) { pickCustodian = std::move(p); }
    // Chooses the gateway node for an OMP_DEST_INTERNET message, or 0 when none is reachable.
    void setGatewayPicker(CustodianPicker p) { pickGateway = std::move(p); }
    void setSelfNode(uint32_t n) { selfNode = n; }

    // parked=true: hold until the destination is heard (a custodian's copy, or one restored from flash).
    bool enqueue(const Message &msg, uint32_t nowMs, bool parked = false);
    bool nextFrame(uint32_t nowMs, Frame &out);
    void onSack(uint32_t from, const SackFrame &s, uint32_t nowMs);
    void onCustody(uint32_t from, const NoticeFrame &n, uint32_t nowMs);
    void onNodeHeard(uint32_t node, uint32_t nowMs);
    void tick(uint32_t nowMs);

    std::vector<StatusFrame> drainStatus();
    // Messages that entered or left the parked state since the last call, for the persistent store.
    std::vector<Message> drainNewlyParked();
    std::vector<std::pair<uint32_t, uint32_t>> drainUnparked();

    size_t size() const { return entries.size(); }
    bool isParked(uint32_t origin, uint32_t msgId) const;

  private:
    enum class State { SENDING, AWAIT_ACK, PARKED, DONE };
    struct Entry {
        Message msg;
        uint32_t linkTo;
        uint8_t airFlags;
        uint8_t count;
        uint64_t acked = 0;
        uint64_t pending = 0; // fragments still to transmit in this round
        State state = State::SENDING;
        uint8_t round = 0;
        uint32_t deadline = 0;
        uint32_t parkedAt = 0;
        uint32_t lastTryMs = 0;
        bool toCustodian = false;
        bool persisted = false;
        bool sentReported = false;
    };

    void finish(Entry &e, MsgState st, uint32_t node);
    void startRound(Entry &e, uint32_t nowMs);
    void park(Entry &e, uint32_t nowMs);
    void onRoundsExhausted(Entry &e, uint32_t nowMs);
    void emit(const Entry &e, MsgState st, uint32_t node);
    bool isUnicast(const Entry &e) const { return e.linkTo != OMP_DEST_BROADCAST; }

    Config cfg;
    CustodianPicker pickCustodian;
    CustodianPicker pickGateway;
    uint32_t selfNode = 0;
    uint32_t lastTxMs = 0;
    bool anyTx = false;
    size_t rr = 0;
    std::vector<Entry> entries;
    std::vector<StatusFrame> status;
    std::vector<Message> newlyParked;
    std::vector<std::pair<uint32_t, uint32_t>> unparked;
};

} // namespace oshi
