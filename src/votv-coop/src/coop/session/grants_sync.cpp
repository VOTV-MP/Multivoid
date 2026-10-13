// coop/session/grants_sync.cpp -- see coop/session/grants_sync.h.

#include "coop/session/grants_sync.h"

#include "coop/net/peer_identity.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/session_serial.h"
#include "coop/permissions/grants_core.h"
#include "coop/permissions/permission_host.h"
#include "coop/player/roster_ledger.h"
#include "coop/session/local_grants.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace coop::session::grants_sync {
namespace {

namespace G = coop::permissions::grants;
namespace PH = coop::permissions::host;
using coop::net::PermissionGrantsPayload;
using coop::net::ReliableKind;
using coop::net::Role;
using coop::net::Session;

// What the host knew at its last compute (game thread).
G::DueState g_last;

// The bits last SENT to each slot's current occupant. Cleared by the ledger when the occupant
// changes, at a session's end and at its bring-up: the next tick sends again.
coop::roster_ledger::PerSlotState<std::optional<uint32_t>> g_sent;

// The serial a refusal was last logged for: one warning per session.
uint32_t g_refusedSerial = 0;

// The bits `id` holds, one per projected node in table order; `*earliest` takes the earliest expiry
// of its answers merged in. `owner` is true for the host's own id alone.
uint32_t ComputeBits(const std::string& id, bool owner, int64_t* earliest) {
    uint32_t bits = 0;
    for (size_t i = 0; i < static_cast<size_t>(G::Projected::Count); ++i) {
        const G::Entry& entry = G::kProjected[i];
        if (PH::Allows(id, entry.node, entry.defaultGranted, owner)) bits |= 1u << i;
    }
    *earliest = G::MergeEarliest(*earliest, PH::NextExpiry(id));
    return bits;
}

void Refuse(const char* why, const Session::ReliableMessage& msg, unsigned count) {
    const uint32_t serial = coop::net::session_serial::Current();
    if (g_refusedSerial == serial) return;
    g_refusedSerial = serial;
    UE_LOGW("grants: REFUSED %s (len=%u slot=%d count=%u)", why, static_cast<unsigned>(msg.payloadLen),
            msg.senderPeerSlot, count);
}

}  // namespace

void HostTick(Session& session) {
    if (!session.running() || session.role() != Role::Host) return;
    const uint64_t revision = PH::Revision();
    const int64_t now = PH::NowSeconds();
    const uint32_t serial = coop::net::session_serial::Current();
    const bool due = G::Due(g_last, revision, serial, now);
    if (due) {
        // An empty id (no identity loaded) computes as an unknown user, and the owner passes.
        int64_t earliest = 0;
        const uint32_t bits = ComputeBits(coop::net::peer_identity::LocalGuid(), true, &earliest);
        coop::session::local_grants::SetBits(serial, bits);
        g_last = G::DueState{revision, serial, earliest};
    }
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (!session.IsSlotReady(slot)) continue;
        std::optional<uint32_t>& sent = g_sent[slot];
        if (!due && sent) continue;
        // The id is published after the slot turns ready: until then nothing is sent and the
        // memory stays empty, so the slot is visited again next tick.
        const std::string id = session.ProvedGuidForSlotWithToken(
            slot, coop::roster_ledger::Get(slot).bornGeneration);
        if (id.empty()) continue;
        const uint32_t bits = ComputeBits(id, false, &g_last.earliest);
        if (sent && *sent == bits) continue;
        PermissionGrantsPayload p{};
        p.count = static_cast<uint8_t>(G::Projected::Count);
        p.bits = bits;
        if (session.SendReliableToSlot(slot, ReliableKind::PermissionGrants, &p, sizeof(p))) {
            sent = bits;
            UE_LOGI("grants: sent bits=0x%08x to slot %d", bits, slot);
        }
    }
}

void HandleGrants(Session& session, const Session::ReliableMessage& msg) {
    UE_ASSERT_GAME_THREAD("grants_sync::HandleGrants");
    PermissionGrantsPayload p{};
    const unsigned count = msg.payloadLen >= 1 ? msg.payload[0] : 0u;
    if (msg.payloadLen != sizeof(PermissionGrantsPayload)) return Refuse("length", msg, count);
    if (!session.running()) return Refuse("session", msg, count);
    std::memcpy(&p, msg.payload, sizeof(p));
    if (p.count != static_cast<uint8_t>(G::Projected::Count)) return Refuse("count", msg, count);
    coop::session::local_grants::SetBits(coop::net::session_serial::Current(), p.bits);
    UE_LOGI("grants: applied bits=0x%08x (from the host)", p.bits);
}

}  // namespace coop::session::grants_sync
