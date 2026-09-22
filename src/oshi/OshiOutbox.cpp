#include "OshiOutbox.h"
#include <algorithm>

namespace oshi
{

namespace
{
uint8_t lowestBit(uint64_t v)
{
    uint8_t i = 0;
    while (!(v & 1)) {
        v >>= 1;
        i++;
    }
    return i;
}
} // namespace

void Outbox::emit(const Entry &e, MsgState st, uint32_t node)
{
    StatusFrame s;
    s.msgId = e.msg.msgId;
    s.state = st;
    s.node = node;
    s.origin = e.msg.origin;
    status.push_back(s);
}

bool Outbox::isParked(uint32_t origin, uint32_t msgId) const
{
    for (const auto &e : entries)
        if (e.msg.origin == origin && e.msg.msgId == msgId)
            return e.state == State::PARKED;
    return false;
}

bool Outbox::enqueue(const Message &msg, uint32_t nowMs, bool parked)
{
    for (const auto &e : entries)
        if (e.msg.origin == msg.origin && e.msg.msgId == msg.msgId && e.state != State::DONE)
            return true;

    uint8_t count = fragmentCount(msg.body.size());
    size_t live = std::count_if(entries.begin(), entries.end(), [](const Entry &e) { return e.state != State::DONE; });
    if (count == 0 || live >= cfg.maxEntries) {
        StatusFrame s;
        s.msgId = msg.msgId;
        s.state = MsgState::REJECTED;
        s.origin = msg.origin;
        status.push_back(s);
        return false;
    }

    Entry e;
    e.msg = msg;
    e.linkTo = msg.dest;
    e.airFlags = msg.flags & ~FLAG_CUSTODY_REQ;
    e.count = count;
    if (msg.dest == OMP_DEST_INTERNET) {
        e.linkTo = pickGateway ? pickGateway(msg) : 0;
        if (!e.linkTo) {
            StatusFrame s;
            s.msgId = msg.msgId;
            s.state = MsgState::FAILED;
            s.origin = msg.origin;
            status.push_back(s);
            return false;
        }
    }
    if (parked) {
        e.state = State::PARKED;
        e.parkedAt = nowMs;
        e.persisted = true;
        entries.push_back(std::move(e));
        return true;
    }
    entries.push_back(std::move(e));
    Entry &added = entries.back();
    emit(added, MsgState::QUEUED, 0);
    startRound(added, nowMs);
    return true;
}

void Outbox::startRound(Entry &e, uint32_t nowMs)
{
    (void)nowMs;
    e.pending = fullBitmap(e.count) & ~e.acked;
    e.state = e.pending ? State::SENDING : State::AWAIT_ACK;
}

bool Outbox::nextFrame(uint32_t nowMs, Frame &out)
{
    if (anyTx && nowMs - lastTxMs < cfg.paceMs)
        return false;
    size_t n = entries.size();
    for (size_t k = 0; k < n; k++) {
        size_t i = (rr + k) % n;
        Entry &e = entries[i];
        if (e.state != State::SENDING || !e.pending)
            continue;
        uint8_t idx = lowestBit(e.pending);
        size_t len = buildFragment(e.msg, idx, e.airFlags, out.bytes, sizeof(out.bytes));
        e.pending &= ~(1ULL << idx);
        if (!len)
            continue;
        out.len = uint8_t(len);
        out.linkTo = e.linkTo;
        out.wantAck = isUnicast(e) && !e.pending;
        lastTxMs = nowMs;
        anyTx = true;
        rr = (i + 1) % n;
        if (!e.pending) {
            if (!isUnicast(e)) {
                finish(e, MsgState::SENT, 0);
            } else {
                if (!e.sentReported && !e.toCustodian) {
                    e.sentReported = true;
                    emit(e, MsgState::SENT, 0);
                }
                e.state = State::AWAIT_ACK;
                e.deadline = nowMs + cfg.ackTimeoutBaseMs + cfg.ackTimeoutPerFragMs * e.count;
            }
        }
        return true;
    }
    return false;
}

void Outbox::finish(Entry &e, MsgState st, uint32_t node)
{
    e.state = State::DONE;
    emit(e, st, node);
    if (e.persisted) {
        unparked.push_back({e.msg.origin, e.msg.msgId});
        e.persisted = false;
    }
}

void Outbox::park(Entry &e, uint32_t nowMs)
{
    e.state = State::PARKED;
    e.parkedAt = e.persisted ? e.parkedAt : nowMs;
    e.linkTo = e.msg.dest;
    e.toCustodian = false;
    e.airFlags &= ~FLAG_CUSTODY_REQ;
    e.acked = 0;
    if (!e.persisted) {
        e.persisted = true;
        newlyParked.push_back(e.msg);
        emit(e, MsgState::IN_CUSTODY, selfNode);
    }
}

void Outbox::onRoundsExhausted(Entry &e, uint32_t nowMs)
{
    if (!e.toCustodian && (e.msg.flags & FLAG_CUSTODY_OK) && !(e.msg.flags & FLAG_VIA_CUSTODY) && pickCustodian) {
        uint32_t c = pickCustodian(e.msg);
        if (c && c != e.linkTo && c != selfNode) {
            e.linkTo = c;
            e.toCustodian = true;
            e.airFlags |= FLAG_CUSTODY_REQ;
            e.acked = 0;
            e.round = 0;
            startRound(e, nowMs);
            return;
        }
    }
    if (cfg.selfCustody && e.msg.dest != OMP_DEST_BROADCAST && e.msg.dest != OMP_DEST_INTERNET) {
        e.round = 0;
        park(e, nowMs);
        return;
    }
    finish(e, MsgState::FAILED, 0);
}

void Outbox::onSack(uint32_t from, const SackFrame &s, uint32_t nowMs)
{
    for (auto &e : entries) {
        if (e.msg.msgId != s.msgId || e.msg.origin != s.origin || e.linkTo != from || s.count != e.count)
            continue;
        if (e.state != State::SENDING && e.state != State::AWAIT_ACK)
            return;
        e.acked |= s.bitmap;
        if (e.acked == fullBitmap(e.count)) {
            if (e.toCustodian)
                finish(e, MsgState::IN_CUSTODY, from);
            else if (e.msg.dest == OMP_DEST_INTERNET)
                finish(e, MsgState::UPLINKED, from);
            else
                finish(e, MsgState::DELIVERED, from);
            return;
        }
        if (e.state == State::AWAIT_ACK) {
            if (++e.round >= cfg.maxRounds) {
                onRoundsExhausted(e, nowMs);
                return;
            }
            startRound(e, nowMs);
        } else {
            e.pending &= ~e.acked;
        }
        return;
    }
}

void Outbox::onCustody(uint32_t from, const NoticeFrame &n, uint32_t nowMs)
{
    (void)nowMs;
    for (auto &e : entries) {
        if (e.msg.msgId == n.msgId && e.msg.origin == n.origin && e.toCustodian && e.linkTo == from &&
            e.state != State::DONE && e.state != State::PARKED) {
            finish(e, MsgState::IN_CUSTODY, from);
            return;
        }
    }
}

void Outbox::onNodeHeard(uint32_t node, uint32_t nowMs)
{
    for (auto &e : entries) {
        if (e.state != State::PARKED || e.msg.dest != node)
            continue;
        if (e.lastTryMs && nowMs - e.lastTryMs < cfg.parkedRetryMs)
            continue;
        e.lastTryMs = nowMs;
        e.round = 0;
        e.acked = 0;
        e.linkTo = e.msg.dest;
        startRound(e, nowMs);
    }
}

void Outbox::tick(uint32_t nowMs)
{
    for (auto &e : entries) {
        if (e.state == State::AWAIT_ACK && int32_t(nowMs - e.deadline) >= 0) {
            if (++e.round >= cfg.maxRounds) {
                onRoundsExhausted(e, nowMs);
                continue;
            }
            // Poll with the last fragment: the receiver answers any last fragment with its bitmap.
            e.pending = (1ULL << (e.count - 1));
            e.state = State::SENDING;
        } else if (e.state == State::PARKED && nowMs - e.parkedAt >= cfg.parkedTtlMs) {
            finish(e, MsgState::FAILED, 0);
        }
    }
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry &e) { return e.state == State::DONE; }),
                  entries.end());
    if (rr >= entries.size())
        rr = 0;
}

std::vector<StatusFrame> Outbox::drainStatus()
{
    std::vector<StatusFrame> out;
    out.swap(status);
    return out;
}

std::vector<Message> Outbox::drainNewlyParked()
{
    std::vector<Message> out;
    out.swap(newlyParked);
    return out;
}

std::vector<std::pair<uint32_t, uint32_t>> Outbox::drainUnparked()
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    out.swap(unparked);
    return out;
}

} // namespace oshi
