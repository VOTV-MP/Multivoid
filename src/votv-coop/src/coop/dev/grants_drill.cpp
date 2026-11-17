// coop/dev/grants_drill.cpp -- see coop/dev/grants_drill.h.

#include "coop/dev/grants_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/net/session_serial.h"
#include "coop/permissions/grants_core.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/local_grants.h"

#include "ue_wrap/core/log.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::grants_drill {
namespace {

namespace G = coop::permissions::grants;
namespace LG = coop::session::local_grants;

enum class Arm : uint8_t { Off, Grant, None, HostDeny };

Arm ParseArm(const std::string& v) {
    return v == "grant" ? Arm::Grant : v == "none" ? Arm::None : v == "hostdeny" ? Arm::HostDeny : Arm::Off;
}

Arm ArmNow() {
    static const Arm arm = ParseArm(coop::config::ResolveEnum(::coop::config_registry::rows::grants_drill));
    return arm;
}

// The arm the verdict is judged against: the drill's own, or another's (the red).
Arm ExpectNow() {
    static const Arm expect = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::grants_drill_expect);
        return v == "same" ? ArmNow() : ParseArm(v);
    }();
    return expect;
}

// Whether `arm` expects this peer to hold the projected node `n`.
bool Expects(Arm arm, bool host, G::Projected n) {
    if (n == G::Projected::Cheats) return true;   // granted by default; no arm withholds it
    if (host) return !(arm == Arm::HostDeny && n == G::Projected::Hud);
    return arm == Arm::Grant && n == G::Projected::Hud;
}

// The serial last judged (0 = none), and whether the host saw a client on its last tick.
uint32_t g_judgedSerial = 0;
bool g_wasPaired = false;

void Judge(bool host, uint8_t peerId) {
    const Arm expect = ExpectNow();
    uint32_t bits = 0;
    int mismatch = -1;
    for (size_t i = 0; i < static_cast<size_t>(G::Projected::Count); ++i) {
        const auto n = static_cast<G::Projected>(i);
        const bool has = LG::Has(n);
        if (has) bits |= 1u << i;
        if (mismatch < 0 && has != Expects(expect, host, n)) mismatch = static_cast<int>(i);
    }
    char role[16];
    if (host) std::snprintf(role, sizeof(role), "host");
    else std::snprintf(role, sizeof(role), "c%u", static_cast<unsigned>(peerId));
    if (mismatch < 0) {
        UE_LOGI("[GRANTS-DRILL] %s PASS (bits=0x%x)", role, bits);
        return;
    }
    UE_LOGE("[GRANTS-DRILL] %s FAIL: %s is %d", role, G::kProjected[mismatch].node,
            (bits >> mismatch) & 1u ? 1 : 0);
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (ArmNow() == Arm::Off) return;
    if (!s || !s->running()) {
        // A session that ended re-arms the host's pairing edge for the next one.
        g_wasPaired = false;
        return;
    }
    const uint32_t serial = coop::net::session_serial::Current();
    if (serial == 0) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (host) {
        // The host judges once a client is in, and again each time one joins anew: a joiner the rig
        // relaunched after a boot flake re-marks the host's log, and the verdict must come after the
        // mark (bug_report_drill's rule). The edge is read before the judged-serial latch.
        coop::roster::Snapshot roster;
        coop::roster::GetSnapshot(roster);
        const bool paired = roster.count >= 2;
        if (paired && !g_wasPaired) g_judgedSerial = 0;
        g_wasPaired = paired;
        if (!paired) return;
    }
    if (g_judgedSerial == serial) return;
    // This session's bits are in: judged by the serial, never by a count of messages.
    if (LG::AppliedSerial() != serial) return;
    const uint8_t peerId = coop::players::Registry::Get().LocalPeerId();
    if (peerId >= coop::players::kMaxPeers) return;  // the slot is not assigned yet
    g_judgedSerial = serial;
    Judge(host, peerId);
}

}  // namespace coop::dev::grants_drill
