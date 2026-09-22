#pragma once

#include "MeshModule.h"
#include "concurrency/OSThread.h"
#include "oshi/OshiCustody.h"
#include "oshi/OshiMessage.h"
#include "oshi/OshiOutbox.h"
#include "oshi/OshiPeers.h"
#include <deque>

// OSHI Mesh: reliable, fragmented, custody-backed messaging between OSHI nodes, carried as opaque
// PRIVATE_APP frames on a private secondary channel so stock Meshtastic nodes relay it untouched.
class OshiModule : public MeshModule, private concurrency::OSThread
{
  public:
    OshiModule();

    // Called by MeshService for every packet the phone sends. Returns true if it was an OMP frame and was consumed.
    bool handleFromPhone(const meshtastic_MeshPacket &p);
    // Raw OMP frames are handled here and never forwarded to the phone as-is.
    bool swallowsForPhone(const meshtastic_MeshPacket &mp) const;

    int8_t channelIndex() const { return oshiChannel; }
    bool isGatewayOnline() const;

    // Used by the gateway: a message that arrived from the internet for a mesh node.
    bool submitDownlink(const oshi::Message &msg);

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override { return true; }
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

  private:
    enum class RxMode { DELIVER, CUSTODY, UPLINK };

    // MeshModule::setup() is never called by the framework, so this runs from the first runOnce().
    void initialize();
    void ensureChannel();
    void handleOmp(const meshtastic_MeshPacket &mp);
    void receiveData(const meshtastic_MeshPacket &mp, const oshi::DataFrame &f, RxMode mode);
    bool controlFrameTrusted(const meshtastic_MeshPacket &mp) const;

    bool transmit(uint32_t linkTo, const uint8_t *bytes, size_t len, bool wantAck);
    void sendSack(uint32_t to, uint32_t origin, uint32_t msgId, uint8_t count, uint64_t have);
    void sendNotice(oshi::FrameType type, uint32_t to, uint32_t origin, uint32_t msgId, uint32_t dest);
    void sendBeacon();
    void toPhone(uint32_t from, const uint8_t *bytes, size_t len);
    void pumpPhone();
    bool peerUsesPki(uint32_t node) const;
    void deliverToPhone(const oshi::Message &msg);
    void statusToPhone(const oshi::StatusFrame &s);
    oshi::BeaconFrame ourBeacon() const;

    void loadCustody();
    void saveCustody();
    void processOutboxEvents();
    void maybePull(uint32_t now);
    void noteDownlink(const oshi::Message &msg, uint32_t now);
    void loadPullCursor();
    void savePullCursor();

    bool initialized = false;
    int8_t oshiChannel = -1;
    uint32_t lastBeaconMs = 0;
    bool beaconSent = false;
    bool custodyDirty = false;
    uint32_t lastPullMs = 0;
    bool pulledOnce = false;
    uint32_t pullAfterSeq = 0;

    oshi::Outbox outbox;
    oshi::Reassembler rx;
    oshi::Reassembler fromPhone;
    oshi::SeenSet seen;
    oshi::PeerTable peers;
    oshi::CustodySet custody;

    struct PhoneFrame {
        uint32_t from;
        std::vector<uint8_t> bytes;
    };
    std::deque<PhoneFrame> phonePending;
    size_t phonePendingBytes = 0;
};

extern OshiModule *oshiModule;
