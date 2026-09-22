#include "OshiModule.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "UptimeClock.h"
#include "airtime.h"
#include "configuration.h"
#include "main.h"
#include "modules/NodeInfoModule.h"
#include "mesh/Throttle.h"
#include "mesh/Channels.h"
#include "gps/RTC.h"
#include "mesh/CryptoEngine.h"
#include "oshi/OshiGatewayCodec.h"
#include "oshi/OshiGatewayEsp32.h"
#include "oshi/OshiGatewayLink.h"
#include <cstring>
#if ARCH_PORTDUINO
#include "platform/portduino/PortduinoGlue.h"
#endif

using namespace oshi;

OshiModule *oshiModule;
oshi::GatewayLink *oshiGateway;

namespace
{
constexpr char OSHI_CHANNEL_NAME[] = "OSHI";
// SHA-256("OSHI Mesh channel v1"). Public by design: it keeps OMP off the default channel so stock
// ROUTERs relay it opaquely instead of dropping PRIVATE_APP. Confidentiality comes from the OSHI envelope.
constexpr uint8_t OSHI_CHANNEL_PSK[32] = {0x29, 0x43, 0x6d, 0x19, 0xaf, 0x55, 0xbd, 0x4e, 0x20, 0x24, 0x73,
                                          0x23, 0xc8, 0xff, 0x45, 0x57, 0x86, 0xf0, 0x03, 0x11, 0x98, 0x17,
                                          0xe1, 0x6c, 0xf7, 0xdf, 0x30, 0x03, 0x68, 0x4c, 0xb9, 0x50};
constexpr char CUSTODY_DIR[] = "/oshi";
constexpr char CUSTODY_FILE[] = "/oshi/custody.bin";
constexpr char PULL_FILE[] = "/oshi/pull.bin";
constexpr char INBOX_FILE[] = "/oshi/inbox.bin";
constexpr uint32_t INBOX_SAVE_MIN_MS = 5000;
constexpr uint32_t PULL_INTERVAL_MS = 10 * 60 * 1000;
// After a downlink arrives there may be more waiting (the server returns at most 5 per pull).
constexpr uint32_t PULL_FOLLOWUP_MS = 30 * 1000;
constexpr uint32_t BEACON_INTERVAL_MS = 15 * 60 * 1000;
constexpr uint32_t FIRST_BEACON_MS = 45 * 1000;
constexpr uint16_t OMP_IMPL_VERSION = (OMP_VERSION << 8) | 0;
constexpr int32_t TICK_MS = 250;
constexpr size_t PHONE_PENDING_MAX = 24 * 1024;
// Slots left in the phone queue for ordinary traffic (text, routing, node info) while OMP drains.
constexpr int PHONE_QUEUE_RESERVE = 3;

uint8_t hopsAway(const meshtastic_MeshPacket &mp)
{
    return mp.hop_start >= mp.hop_limit ? mp.hop_start - mp.hop_limit : 0;
}
} // namespace

OshiModule::OshiModule() : MeshModule("oshi", meshtastic_PortNum_PRIVATE_APP), concurrency::OSThread("Oshi")
{
    isPromiscuous = true; // every packet tells us a node is alive, which is what releases parked messages
    encryptedOk = true;
    // Only nodes we can really address: a custody request or an uplink sent as a broadcast would be ignored.
    auto usable = [this](uint32_t n) { return canAddress(n); };
    outbox.setCustodianPicker(
        [this, usable](const Message &m) { return peers.pickCustodian(m.dest, m.body.size(), Time::getMillis(), usable); });
    outbox.setGatewayPicker([this, usable](const Message &) { return peers.pickGateway(Time::getMillis(), usable); });
}

void OshiModule::initialize()
{
    initialized = true;
    ensureChannel();
    outbox.setSelfNode(nodeDB->getNodeNum());
    loadCustody();
    loadInbox();
    loadPullCursor();
#if defined(ARCH_ESP32) && HAS_WIFI && !MESHTASTIC_EXCLUDE_WIFI
    // Any OSHI node on WiFi relays for the mesh; it only ever carries self-authenticating frames.
    if (config.network.wifi_enabled && !oshiGateway)
        oshiGateway = new oshi::Esp32Gateway(nodeDB->getNodeNum());
#endif
}

void OshiModule::ensureChannel()
{
    if (owner.is_licensed) {
        LOG_WARN("OSHI: licensed mode forbids encryption, OSHI channel disabled");
        oshiChannel = -1;
        return;
    }
    int8_t freeSlot = -1;
    for (ChannelIndex i = 0; i < channels.getNumChannels(); i++) {
        meshtastic_Channel &c = channels.getByIndex(i);
        if (c.role != meshtastic_Channel_Role_DISABLED && c.has_settings && strcmp(c.settings.name, OSHI_CHANNEL_NAME) == 0) {
            oshiChannel = i;
            return;
        }
        if (freeSlot < 0 && i > 0 && c.role == meshtastic_Channel_Role_DISABLED)
            freeSlot = i;
    }
    if (freeSlot < 0) {
        LOG_WARN("OSHI: no free channel slot; OMP falls back to the primary channel");
        oshiChannel = -1;
        return;
    }
    meshtastic_Channel c = meshtastic_Channel_init_zero;
    c.index = freeSlot;
    c.role = meshtastic_Channel_Role_SECONDARY;
    c.has_settings = true;
    strncpy(c.settings.name, OSHI_CHANNEL_NAME, sizeof(c.settings.name) - 1);
    c.settings.psk.size = sizeof(OSHI_CHANNEL_PSK);
    memcpy(c.settings.psk.bytes, OSHI_CHANNEL_PSK, sizeof(OSHI_CHANNEL_PSK));
    c.settings.uplink_enabled = false;
    c.settings.downlink_enabled = false;
    channels.setChannel(c);
    channels.onConfigChanged();
    nodeDB->saveToDisk(SEGMENT_CHANNELS);
    oshiChannel = freeSlot;
    LOG_INFO("OSHI: created channel '%s' at index %d", OSHI_CHANNEL_NAME, freeSlot);
}

bool OshiModule::isGatewayOnline() const
{
    return oshiGateway && oshiGateway->online();
}

BeaconFrame OshiModule::ourBeacon() const
{
    BeaconFrame b;
    size_t freeKb = custody.bytesFree() / 1024;
    b.caps = (freeKb >= 2 ? CAP_CUSTODIAN : 0) | (isGatewayOnline() ? CAP_GATEWAY_ONLINE : 0);
    b.version = OMP_IMPL_VERSION;
    b.custodyFreeKb = freeKb > 255 ? 255 : uint8_t(freeKb);
    return b;
}

bool OshiModule::swallowsForPhone(const meshtastic_MeshPacket &mp) const
{
    return mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
           mp.decoded.portnum == meshtastic_PortNum_PRIVATE_APP && isOmpFrame(mp.decoded.payload.bytes, mp.decoded.payload.size);
}

bool OshiModule::peerUsesPki(uint32_t node) const
{
#if ARCH_PORTDUINO
    if (portduino_config.force_simradio)
        return false;
#endif
    meshtastic_NodeInfoLite_public_key_t key = {0, {0}};
    return node != OMP_DEST_BROADCAST && !owner.is_licensed && config.security.private_key.size == 32 &&
           nodeDB->copyPublicKey(node, key) && key.size == 32;
}

bool OshiModule::canAddress(uint32_t node) const
{
    if (node == OMP_DEST_BROADCAST)
        return false;
#if ARCH_PORTDUINO
    if (portduino_config.force_simradio)
        return true; // the simulator carries channel-encrypted DMs
#endif
    return peerUsesPki(node);
}

bool OshiModule::canHold(RxMode mode, uint8_t count) const
{
    switch (mode) {
    case RxMode::CUSTODY:
        return custody.bytesFree() >= size_t(count) * OMP_MAX_FRAG_DATA + 16;
    case RxMode::UPLINK:
        return oshiGateway && oshiGateway->hasRoom();
    case RxMode::DELIVER:
        return true;
    }
    return false;
}

void OshiModule::askForKey(uint32_t node, uint32_t now)
{
    auto it = keyAskedMs.find(node);
    if (it != keyAskedMs.end() && now - it->second < 30UL * 60 * 1000)
        return;
    if (keyAskedMs.size() > 64)
        keyAskedMs.clear();
    keyAskedMs[node] = now;
    // Its NodeInfo carries its public key; without it every unicast to that node degrades to a broadcast.
    if (nodeInfoModule)
        nodeInfoModule->sendOurNodeInfo(node, true, 0);
}

bool OshiModule::controlFrameTrusted(const meshtastic_MeshPacket &mp) const
{
    // A peer we talk to over PKI must answer over PKI, which authenticates it; accepting a channel-encrypted
    // SACK "from" that node would let anyone holding the public OSHI PSK forge a delivery.
    return !peerUsesPki(mp.from) || mp.pki_encrypted;
}

ProcessMessage OshiModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    uint32_t self = nodeDB->getNodeNum();
    if (mp.from && mp.from != self)
        outbox.onNodeHeard(mp.from, Time::getMillis());
    if (mp.from != self && swallowsForPhone(mp))
        handleOmp(mp);
    return ProcessMessage::CONTINUE;
}

void OshiModule::handleOmp(const meshtastic_MeshPacket &mp)
{
    const uint8_t *b = mp.decoded.payload.bytes;
    size_t n = mp.decoded.payload.size;
    uint32_t self = nodeDB->getNodeNum();
    bool forUs = isBroadcast(mp.to) || mp.to == self;
    uint32_t now = Time::getMillis();
    FrameType t;
    if (!frameType(b, n, t) || !forUs)
        return;

    switch (t) {
    case FrameType::DATA: {
        DataFrame f;
        if (!decodeData(b, n, f) || f.origin == self)
            return;
        if (f.dest == self || f.dest == OMP_DEST_BROADCAST)
            receiveData(mp, f, RxMode::DELIVER);
        else if (mp.to == self && f.dest == OMP_DEST_INTERNET && isGatewayOnline())
            receiveData(mp, f, RxMode::UPLINK);
        else if (mp.to == self && (f.flags & FLAG_CUSTODY_REQ))
            receiveData(mp, f, RxMode::CUSTODY);
        break;
    }
    case FrameType::SACK: {
        SackFrame s;
        if (decodeSack(b, n, s) && controlFrameTrusted(mp))
            outbox.onSack(mp.from, s, now);
        break;
    }
    case FrameType::CUSTODY: {
        NoticeFrame nf;
        if (decodeNotice(b, n, FrameType::CUSTODY, nf) && nf.origin == self && controlFrameTrusted(mp))
            outbox.onCustody(mp.from, nf, now);
        break;
    }
    case FrameType::RECEIPT: {
        NoticeFrame nf;
        auto held = custodianOf.end();
        // Only the custodian we handed it to can say it arrived.
        if (decodeNotice(b, n, FrameType::RECEIPT, nf) && nf.origin == self && controlFrameTrusted(mp) &&
            (held = custodianOf.find(nf.msgId)) != custodianOf.end() && held->second == mp.from) {
            custodianOf.erase(held);
            StatusFrame s;
            s.msgId = nf.msgId;
            s.state = MsgState::DELIVERED;
            s.node = nf.dest;
            statusToPhone(s);
        }
        break;
    }
    case FrameType::BEACON: {
        BeaconFrame bf;
        if (decodeBeacon(b, n, bf)) {
            peers.onBeacon(mp.from, bf, hopsAway(mp), now);
            if (!canAddress(mp.from))
                askForKey(mp.from, now);
        }
        break;
    }
    case FrameType::PULL: {
        PullFrame pf;
        // A pull is only ever relayed for the node that signed it; the server re-checks the signature.
        if (mp.to == self && isGatewayOnline() && decodePull(b, n, pf) && pf.nodeNum == mp.from &&
            !oshiGateway->forwardPull(mp.from, b + 3, n - 3))
            LOG_WARN("OSHI: gateway busy, pull from 0x%08x dropped", mp.from);
        break;
    }
    case FrameType::STATUS:
        break;
    }
}

void OshiModule::receiveData(const meshtastic_MeshPacket &mp, const DataFrame &f, RxMode mode)
{
    uint32_t now = Time::getMillis();
    bool unicastIntent = f.dest != OMP_DEST_BROADCAST;
    uint8_t seenCount = 0;
    if (seen.contains(f.origin, f.msgId, &seenCount)) {
        if (unicastIntent && f.idx + 1 == f.count)
            sendSack(mp.from, f.origin, f.msgId, seenCount, fullBitmap(seenCount));
        return;
    }

    // Refuse up front rather than acknowledge a message we then cannot keep: the sender would show it as
    // held (or uplinked) while it is gone, and would stop looking for another custodian.
    if (!canHold(mode, f.count))
        return;
    Reassembler::Result r = rx.accept(f, mp.from, now);
    if (r == Reassembler::Result::REJECTED)
        return;
    uint8_t count = 0;
    uint64_t have = 0;
    if (unicastIntent && (f.idx + 1 == f.count || r == Reassembler::Result::COMPLETE) && rx.bitmap(f.origin, f.msgId, count, have))
        sendSack(mp.from, f.origin, f.msgId, count, have);
    if (r != Reassembler::Result::COMPLETE)
        return;

    Message msg;
    if (!rx.take(f.origin, f.msgId, msg))
        return;
    seen.add(msg.origin, msg.msgId, f.count);

    switch (mode) {
    case RxMode::DELIVER:
        noteDownlink(msg, now);
        deliverToPhone(msg);
        if (msg.flags & FLAG_VIA_CUSTODY)
            LOG_INFO("OSHI: 0x%08x/0x%08x delivered via custodian 0x%08x", msg.origin, msg.msgId, mp.from);
        break;
    case RxMode::UPLINK:
        if (!oshiGateway || !oshiGateway->uplink(msg))
            LOG_WARN("OSHI: gateway queue full, 0x%08x/0x%08x dropped", msg.origin, msg.msgId);
        break;
    case RxMode::CUSTODY: {
        msg.flags = (msg.flags & ~FLAG_CUSTODY_REQ) | FLAG_VIA_CUSTODY;
        if (!custody.add(msg)) {
            LOG_WARN("OSHI: custody full, refusing 0x%08x/0x%08x", msg.origin, msg.msgId);
            return;
        }
        custodyDirty = true;
        sendNotice(FrameType::CUSTODY, msg.origin, msg.origin, msg.msgId, msg.dest);
        outbox.enqueue(msg, now);
        break;
    }
    }
}

bool OshiModule::transmit(uint32_t linkTo, const uint8_t *bytes, size_t len, bool wantAck)
{
    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p)
        return false;
    p->decoded.portnum = meshtastic_PortNum_PRIVATE_APP;
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, bytes, len);
    p->channel = oshiChannel >= 0 ? oshiChannel : channels.getPrimaryIndex();

    bool direct = canAddress(linkTo);
    // Without the peer's key a DM would be refused, so the frame is broadcast and the OMP header carries the real target.
    p->to = direct ? linkTo : NODENUM_BROADCAST;
    p->want_ack = direct && wantAck;
    p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    return true;
}

void OshiModule::sendSack(uint32_t to, uint32_t origin, uint32_t msgId, uint8_t count, uint64_t have)
{
    SackFrame s;
    s.msgId = msgId;
    s.origin = origin;
    s.count = count;
    s.bitmap = have;
    uint8_t buf[OMP_MAX_FRAME];
    size_t n = encodeSack(s, buf, sizeof(buf));
    if (n)
        transmit(to, buf, n, false);
}

void OshiModule::sendNotice(FrameType type, uint32_t to, uint32_t origin, uint32_t msgId, uint32_t dest)
{
    NoticeFrame nf{msgId, origin, dest};
    uint8_t buf[OMP_MAX_FRAME];
    size_t n = encodeNotice(type, nf, buf, sizeof(buf));
    if (n)
        transmit(to, buf, n, true);
}

void OshiModule::sendBeacon()
{
    uint8_t buf[OMP_MAX_FRAME];
    size_t n = encodeBeacon(ourBeacon(), buf, sizeof(buf));
    if (n)
        transmit(OMP_DEST_BROADCAST, buf, n, false);
}

void OshiModule::toPhone(uint32_t from, const uint8_t *bytes, size_t len)
{
    while (!phonePending.empty() && phonePendingBytes + len > PHONE_PENDING_MAX) {
        phonePendingBytes -= phonePending.front().bytes.size();
        phonePending.pop_front();
    }
    PhoneFrame f;
    f.from = from;
    f.bytes.assign(bytes, bytes + len);
    phonePending.push_back(std::move(f));
    phonePendingBytes += len;
    pumpPhone();
}

void OshiModule::pumpPhone()
{
    // With no phone attached the frames would only sit in the 8-slot queue and crowd out everything else;
    // they wait here instead, and the inbox keeps a flash copy of whole messages.
    if (service->api_state == MeshService::STATE_DISCONNECTED)
        return;
    while (!phonePending.empty() && service->toPhoneQueueFree() > PHONE_QUEUE_RESERVE) {
        meshtastic_MeshPacket *p = router->allocForSending();
        if (!p)
            return;
        const PhoneFrame &f = phonePending.front();
        p->from = f.from;
        p->to = nodeDB->getNodeNum();
        p->channel = oshiChannel >= 0 ? oshiChannel : 0;
        p->decoded.portnum = meshtastic_PortNum_PRIVATE_APP;
        p->decoded.payload.size = f.bytes.size();
        memcpy(p->decoded.payload.bytes, f.bytes.data(), f.bytes.size());
        phonePendingBytes -= f.bytes.size();
        if (f.lastOfMessage)
            inboxDirty |= inbox.remove(f.origin, f.msgId);
        phonePending.pop_front();
        service->sendToPhone(p);
    }
}

void OshiModule::deliverToPhone(const Message &msg, bool persist)
{
    if (persist && inbox.add(msg))
        inboxDirty = true;
    uint8_t count = fragmentCount(msg.body.size());
    uint8_t buf[OMP_MAX_FRAME];
    for (uint8_t i = 0; i < count; i++) {
        size_t n = buildFragment(msg, i, msg.flags, buf, sizeof(buf));
        if (!n)
            continue;
        toPhone(msg.origin, buf, n);
        if (i + 1 == count && !phonePending.empty()) {
            PhoneFrame &last = phonePending.back();
            last.lastOfMessage = true;
            last.origin = msg.origin;
            last.msgId = msg.msgId;
        }
    }
}

void OshiModule::statusToPhone(const StatusFrame &s)
{
    uint8_t buf[OMP_MAX_FRAME];
    size_t n = encodeStatus(s, buf, sizeof(buf));
    if (n)
        toPhone(nodeDB->getNodeNum(), buf, n);
}

bool OshiModule::handleFromPhone(const meshtastic_MeshPacket &p)
{
    if (!swallowsForPhone(p))
        return false;
    const uint8_t *b = p.decoded.payload.bytes;
    size_t n = p.decoded.payload.size;
    FrameType t;
    if (!frameType(b, n, t))
        return true;
    uint32_t now = Time::getMillis();
    uint32_t self = nodeDB->getNodeNum();

    if (t == FrameType::BEACON) {
        // The app's capability probe; stock firmware never echoes a phone packet back, so silence means stock.
        uint8_t buf[OMP_MAX_FRAME];
        size_t len = encodeBeacon(ourBeacon(), buf, sizeof(buf));
        if (len)
            toPhone(self, buf, len);
        return true;
    }
    if (t != FrameType::DATA)
        return true;

    DataFrame f;
    if (!decodeData(b, n, f))
        return true;
    f.origin = 0;
    Reassembler::Result r = fromPhone.accept(f, 0, now);
    if (r != Reassembler::Result::COMPLETE)
        return true;
    Message msg;
    if (!fromPhone.take(0, f.msgId, msg))
        return true;
    msg.origin = self;
    msg.flags &= FLAG_CUSTODY_OK;

    if (msg.dest == OMP_DEST_INTERNET && isGatewayOnline()) {
        StatusFrame s;
        s.msgId = msg.msgId;
        s.state = oshiGateway->uplink(msg) ? MsgState::UPLINKED : MsgState::REJECTED;
        s.node = self;
        statusToPhone(s);
        return true;
    }
    if (msg.dest == self)
        return true;
    outbox.enqueue(msg, now);
    return true;
}

bool OshiModule::submitDownlink(const Message &msg)
{
    Message m = msg;
    m.origin = nodeDB->getNodeNum();
    m.flags = FLAG_VIA_CUSTODY;
    if (m.dest == m.origin) {
        deliverToPhone(m);
        if (oshiGateway)
            oshiGateway->onDownlinkResult(m.msgId, true);
        return true;
    }
    return outbox.enqueue(m, Time::getMillis());
}

void OshiModule::processOutboxEvents()
{
    uint32_t self = nodeDB->getNodeNum();
    for (const auto &s : outbox.drainStatus()) {
        if (s.state == MsgState::DELIVERED || s.state == MsgState::FAILED || s.state == MsgState::UPLINKED)
            custodyDirty |= custody.remove(s.origin, s.msgId);
        if (s.origin == self) {
            if (s.state == MsgState::IN_CUSTODY && s.node != self) {
                if (custodianOf.size() > 64)
                    custodianOf.erase(custodianOf.begin());
                custodianOf[s.msgId] = s.node;
            }
            statusToPhone(s);
            if (oshiGateway && (s.state == MsgState::DELIVERED || s.state == MsgState::FAILED))
                oshiGateway->onDownlinkResult(s.msgId, s.state == MsgState::DELIVERED);
        } else if (s.state == MsgState::DELIVERED) {
            // We were the custodian: tell the origin its message arrived.
            sendNotice(FrameType::RECEIPT, s.origin, s.origin, s.msgId, s.node);
        }
    }
    for (const auto &m : outbox.drainNewlyParked())
        custodyDirty |= custody.add(m);
    for (const auto &k : outbox.drainUnparked())
        custodyDirty |= custody.remove(k.first, k.second);
    if (custodyDirty)
        saveCustody();
}

void OshiModule::loadCustody()
{
#ifdef FSCom
    std::vector<uint8_t> buf;
    {
        concurrency::LockGuard g(spiLock);
        auto f = FSCom.open(CUSTODY_FILE, FILE_O_READ);
        if (!f)
            return;
        size_t sz = f.size();
        if (sz > CUSTODY_MAX_BYTES + 4096) {
            f.close();
            return;
        }
        buf.resize(sz);
        size_t got = f.read(buf.data(), sz);
        f.close();
        buf.resize(got);
    }
    if (!custody.deserialize(buf.data(), buf.size())) {
        LOG_WARN("OSHI: custody file unreadable, discarded");
        return;
    }
    uint32_t now = Time::getMillis();
    for (const auto &m : custody.all())
        outbox.enqueue(m, now, true);
    LOG_INFO("OSHI: restored %u parked messages", (unsigned)custody.all().size());
#endif
}

void OshiModule::saveCustody()
{
#ifdef FSCom
    std::vector<uint8_t> bytes = custody.serialize();
    {
        concurrency::LockGuard g(spiLock);
        FSCom.mkdir(CUSTODY_DIR);
    }
    auto f = SafeFile(CUSTODY_FILE, false);
    {
        concurrency::LockGuard g(spiLock);
        f.write(bytes.data(), bytes.size());
    }
    if (!f.close())
        LOG_ERROR("OSHI: can't write %s", CUSTODY_FILE);
#endif
    custodyDirty = false;
}

int32_t OshiModule::runOnce()
{
    if (!initialized)
        initialize();
    uint32_t now = Time::getMillis();
    outbox.setSelfNode(nodeDB->getNodeNum());
    outbox.tick(now);
    rx.expire(now);
    fromPhone.expire(now);

    // Stay under half the regional duty cycle: OMP must never be what trips the hard limit on ACKs and DMs.
    if (airTime->isTxAllowedAirUtil()) {
        Outbox::Frame f;
        if (outbox.nextFrame(now, f))
            transmit(f.linkTo, f.bytes, f.len, f.wantAck);
    }
    processOutboxEvents();
    pumpPhone();
    if (inboxDirty && Throttle::hasElapsed(lastInboxSaveMs, INBOX_SAVE_MIN_MS)) {
        saveInbox();
        lastInboxSaveMs = now;
    }
    maybePull(now);

    uint32_t beaconDue = beaconSent ? BEACON_INTERVAL_MS : FIRST_BEACON_MS;
    if (Throttle::hasElapsed(lastBeaconMs, beaconDue) && airTime->isTxAllowedChannelUtil(true)) {
        sendBeacon();
        lastBeaconMs = now;
        beaconSent = true;
    }
    if (oshiGateway)
        oshiGateway->loop(now);
    return TICK_MS;
}

void OshiModule::maybePull(uint32_t now)
{
#if !(MESHTASTIC_EXCLUDE_PKI) && !(MESHTASTIC_EXCLUDE_XEDDSA)
    uint32_t self = nodeDB->getNodeNum();
    uint32_t gw = isGatewayOnline() ? self : peers.pickGateway(now);
    if (!gw || (pulledOnce && !Throttle::hasElapsed(lastPullMs, PULL_INTERVAL_MS)))
        return;
    // The server accepts a pull within 10 minutes of its clock; without a time source we cannot sign one.
    uint32_t ts = getValidTime(RTCQualityDevice);
    if (!ts || !airTime->isTxAllowedChannelUtil(true))
        return;
    PullFrame pf;
    pf.nodeNum = self;
    pf.afterSeq = pullAfterSeq;
    pf.tsSec = ts;
    uint8_t payload[8];
    pullSigningPayload(gw, pullAfterSeq, payload);
    if (!crypto->xeddsa_sign(self, ts, OMP_PULL_SIGN_PORT, payload, sizeof(payload), pf.sig))
        return;
    uint8_t buf[OMP_MAX_FRAME];
    size_t n = encodePull(pf, buf, sizeof(buf));
    if (!n)
        return;
    if (gw == self)
        oshiGateway->forwardPull(self, buf + 3, n - 3);
    else
        transmit(gw, buf, n, true);
    lastPullMs = now;
    pulledOnce = true;
#else
    (void)now;
#endif
}

void OshiModule::noteDownlink(const Message &msg, uint32_t now)
{
    uint32_t seq = 0;
    if (!(msg.flags & FLAG_VIA_CUSTODY) || !downlinkSeq(msg.body.data(), msg.body.size(), seq))
        return;
    // Only a gateway we know may move the cursor, or a forged 'D' could make us skip our own mail.
    bool fromGateway = msg.origin == nodeDB->getNodeNum() || peers.isOshiNode(msg.origin, now);
    if (!fromGateway || seq <= pullAfterSeq)
        return;
    pullAfterSeq = seq;
    savePullCursor();
    lastPullMs = now - PULL_INTERVAL_MS + PULL_FOLLOWUP_MS;
}

void OshiModule::loadPullCursor()
{
#ifdef FSCom
    concurrency::LockGuard g(spiLock);
    auto f = FSCom.open(PULL_FILE, FILE_O_READ);
    if (!f)
        return;
    uint8_t b[4] = {0};
    if (f.read(b, 4) == 4)
        pullAfterSeq = b[0] | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    f.close();
#endif
}

void OshiModule::savePullCursor()
{
#ifdef FSCom
    uint8_t b[4] = {uint8_t(pullAfterSeq), uint8_t(pullAfterSeq >> 8), uint8_t(pullAfterSeq >> 16), uint8_t(pullAfterSeq >> 24)};
    {
        concurrency::LockGuard g(spiLock);
        FSCom.mkdir(CUSTODY_DIR);
    }
    auto f = SafeFile(PULL_FILE, false);
    {
        concurrency::LockGuard g(spiLock);
        f.write(b, sizeof(b));
    }
    if (!f.close())
        LOG_ERROR("OSHI: can't write %s", PULL_FILE);
#endif
}

void OshiModule::loadInbox()
{
#ifdef FSCom
    std::vector<uint8_t> buf;
    {
        concurrency::LockGuard g(spiLock);
        auto f = FSCom.open(INBOX_FILE, FILE_O_READ);
        if (!f)
            return;
        buf.resize(f.size());
        buf.resize(f.read(buf.data(), buf.size()));
        f.close();
    }
    if (!inbox.deserialize(buf.data(), buf.size())) {
        LOG_WARN("OSHI: inbox file unreadable, discarded");
        return;
    }
    for (const auto &m : inbox.all())
        deliverToPhone(m, false);
    if (!inbox.all().empty())
        LOG_INFO("OSHI: %u messages waiting for the phone", (unsigned)inbox.all().size());
#endif
}

void OshiModule::saveInbox()
{
#ifdef FSCom
    std::vector<uint8_t> bytes = inbox.serialize();
    {
        concurrency::LockGuard g(spiLock);
        FSCom.mkdir(CUSTODY_DIR);
    }
    auto f = SafeFile(INBOX_FILE, false);
    {
        concurrency::LockGuard g(spiLock);
        f.write(bytes.data(), bytes.size());
    }
    if (!f.close())
        LOG_ERROR("OSHI: can't write %s", INBOX_FILE);
#endif
    inboxDirty = false;
}
