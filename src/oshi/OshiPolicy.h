#pragma once

#include <cstdint>

// Router policy decisions OSHI Mesh changes, as pure functions so they are testable without a Router.
namespace oshi
{

// Under the COMPATIBLE signature policy an unsigned broadcast from a node known to sign is accepted,
// because a 2.5-2.7 relay strips the signature. That excuse only exists if a legacy relay could have
// carried it: not when it was heard straight from its originator, and not when the relay is itself a signer.
inline bool unsignedExplainedByLegacyRelay(bool heardDirect, bool relayResolved, bool relayIsSigner)
{
    if (heardDirect)
        return false;
    return !(relayResolved && relayIsSigner);
}

inline bool heardDirect(uint8_t hopStart, uint8_t hopLimit)
{
    // hop_start == 0 means an old sender that does not report it: unknown, so never "direct".
    return hopStart != 0 && hopStart == hopLimit;
}

// Share of the regional duty cycle after which OSHI keeps the remaining airtime for ACKs and direct messages.
constexpr float DUTY_RESERVE_FRACTION = 0.8f;
constexpr int PRIORITY_BACKGROUND = 10;
constexpr int PRIORITY_DEFAULT = 64;
constexpr int PRIORITY_RELIABLE = 70;

// Stock firmware refuses every transmission once the hourly budget is spent, ACKs included. Shedding
// relayed broadcasts and our own background traffic first leaves room for the packets that matter.
inline bool shedUnderDutyCycle(float txPercent, float dutyCycle, bool fromUs, bool broadcast, int priority)
{
    if (dutyCycle >= 100 || txPercent > dutyCycle || txPercent < dutyCycle * DUTY_RESERVE_FRACTION)
        return false;
    int prio = priority == 0 ? PRIORITY_DEFAULT : priority;
    if (!fromUs)
        return broadcast && prio < PRIORITY_RELIABLE;
    return prio <= PRIORITY_BACKGROUND;
}

} // namespace oshi
