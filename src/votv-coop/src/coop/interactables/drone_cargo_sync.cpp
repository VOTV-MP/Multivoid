// coop/interactables/drone_cargo_sync.cpp -- see coop/interactables/drone_cargo_sync.h.

#include "coop/interactables/drone_cargo_sync.h"

#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/container_slice_wire.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace coop::drone_cargo_sync {
namespace {

namespace D  = ue_wrap::drone;
namespace sg = ue_wrap::script_gate;
namespace cw = coop::props::container_slice_wire;
using Items = std::vector<D::CargoItem>;

// The cargo has no element id; the slice grammar's eid field carries this in its place.
constexpr uint32_t kDroneEid = 0;

constexpr uint64_t kPollMs  = 500;   // the host reads its cargo this often; a change goes at the next read
constexpr uint64_t kSweepMs = 1000;  // a client drops a half-assembled blob this long after its last chunk
constexpr int      kTagPutSackOn = 0x44435053;  // 'DCPS'
constexpr int      kTagUsedOn    = 0x44435545;  // 'DCUE'

// The verbs that would write a client's copy of the drone's cargo, watched on the drone's own class.
UE_FACT_FN(kRowPutSackOn, "drone_cargo_sync", L"drone_C", L"putSackOn", Declared);
UE_FACT_FN(kRowUsedOn,    "drone_cargo_sync", L"drone_C", L"playerUsedOn", Declared);

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchesInstalled = false;
bool g_refusalSaid[2] = {};

// ---- host ------------------------------------------------------------------------------------
uint64_t g_nextPollMs = 0;
bool     g_haveSent = false;
uint64_t g_sentHash = 0;
uint32_t g_seq = 1;
bool     g_unreadSaid = false;
bool     g_tooBigSaid = false;
uint64_t g_truthMs[coop::net::kMaxPeers] = {};   // when the truth last went to each slot

// ---- client ----------------------------------------------------------------------------------
coop::blob_chunks::Assembler g_asm;
uint64_t g_nextSweepMs = 0;
bool     g_havePending = false;
Items    g_pending;
uint64_t g_pendingHash = 0;
bool     g_haveApplied = false;
uint64_t g_appliedHash = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool IsHost(const coop::net::Session* s) { return s && s->role() == coop::net::Role::Host; }

// The host's cargo as it reads now. False, said once, when it does not read.
bool ReadHostCargo(Items& out) {
    void* const drone = D::Find();
    if (!drone) return false;
    if (!D::ReadCargo(drone, out, cw::kMaxRecords)) {
        if (!g_unreadSaid) {
            g_unreadSaid = true;
            UE_LOGW("drone_cargo: the drone's cargo does not read (no inventory component, an unresolved entry "
                    "layout or more than %zu entries) -- it is not sent", cw::kMaxRecords);
        }
        return false;
    }
    return true;
}

// Ship the entries whole to every peer, or to one slot (slot >= 0). True when every chunk was accepted.
bool Send(coop::net::Session* s, const Items& items, int slot) {
    if (cw::SliceBytes(items) > coop::blob_chunks::MaxBlobBytes()) {
        if (!g_tooBigSaid) {
            g_tooBigSaid = true;
            UE_LOGW("drone_cargo: %zu entries are past the blob ceiling -- not sent", items.size());
        }
        return false;
    }
    const std::vector<uint8_t> blob = cw::Pack(kDroneEid, items);
    const uint32_t seq = g_seq++;
    return slot < 0 ? coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::DroneCargo, seq, blob)
                    : coop::blob_chunks::SendBlobToSlot(s, slot, coop::net::ReliableKind::DroneCargo, seq, blob);
}

void HostTick(coop::net::Session* s) {
    const uint64_t now = NowMs();
    if (now < g_nextPollMs) return;
    g_nextPollMs = now + kPollMs;
    // A broadcast reaches world-ready peers only; a joiner takes the cargo at its own world-ready edge.
    if (!s->AnyWorldReadyPeer()) return;
    Items items;
    if (!ReadHostCargo(items)) return;
    const uint64_t hash = cw::RecordsHash(kDroneEid, items);
    if (g_haveSent && hash == g_sentHash) return;
    if (!Send(s, items, -1)) return;   // not accepted: the next read sends it again
    g_haveSent = true;
    g_sentHash = hash;
    UE_LOGI("drone_cargo: sent %zu entries (hash %016llx)", items.size(), static_cast<unsigned long long>(hash));
}

void ClientTick() {
    const uint64_t now = NowMs();
    if (now >= g_nextSweepMs) {
        g_nextSweepMs = now + kSweepMs;
        if (!g_asm.Idle()) g_asm.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));
    }
    if (!g_havePending) return;
    if (g_haveApplied && g_pendingHash == g_appliedHash) {   // the copy already holds this
        g_havePending = false;
        g_pending.clear();
        return;
    }
    void* const drone = D::Find();
    if (!drone) return;   // the drone resolves later; the blob waits
    switch (D::WriteCargo(drone, g_pending)) {
        case D::CargoWrite::Written:
            g_haveApplied = true;
            g_appliedHash = g_pendingHash;
            UE_LOGI("drone_cargo: applied %zu entries (hash %016llx)", g_pending.size(),
                    static_cast<unsigned long long>(g_pendingHash));
            break;
        case D::CargoWrite::NotReady:
            return;   // the entry layout or the component is not up yet; try again next tick
        case D::CargoWrite::Refused:
            UE_LOGW("drone_cargo: the host's %zu entries were refused by the drone's inventory", g_pending.size());
            break;
    }
    g_havePending = false;
    g_pending.clear();
}

// ---- the client's refusals -------------------------------------------------------------------
sg::Verdict Refuse(int which, const char* verb, const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client || !call.object) return sg::Verdict::Run;
    if (!g_refusalSaid[which]) {
        g_refusalSaid[which] = true;
        UE_LOGI("drone_cargo: this client's drone.%s is refused -- the cargo is the host's", verb);
    }
    return sg::Verdict::Cancel;
}

sg::Verdict OnPutSackOnPre(const sg::Call& call) { return Refuse(0, "putSackOn", call); }
sg::Verdict OnUsedOnPre(const sg::Call& call) { return Refuse(1, "playerUsedOn", call); }

void EnsureWatches() {
    sg::ResolvePendingNames();
    if (g_watchesInstalled) return;
    const bool a = sg::WatchName(kRowPutSackOn, kTagPutSackOn, &OnPutSackOnPre, nullptr);
    const bool b = sg::WatchName(kRowUsedOn, kTagUsedOn, &OnUsedOnPre, nullptr);
    g_watchesInstalled = a && b;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    EnsureWatches();
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    EnsureWatches();
    if (!s || !s->connected()) return;
    if (IsHost(s)) HostTick(s); else ClientTick();
}

void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || IsHost(s)) return;   // a client's copy is the host's to refuse at its receive edge
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(chunk, senderSlot, blob)) return;
    size_t o = 0;
    uint32_t eid = 0;
    uint16_t count = 0;
    Items items;
    const char* why = nullptr;
    if (!cw::ParseHeader(blob, o, eid, count) || eid != kDroneEid ||
        !cw::ParseRecords(blob, o, count, items, &why)) {
        UE_LOGW("drone_cargo: a malformed cargo blob was dropped (%s)", why ? why : "header");
        return;
    }
    g_pendingHash = cw::RecordsHash(kDroneEid, items);
    g_pending = std::move(items);
    g_havePending = true;   // the newest word replaces an older one still waiting
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || peerSlot < 1 || peerSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    Items items;
    if (!ReadHostCargo(items)) return;
    if (!Send(s, items, peerSlot))
        UE_LOGW("drone_cargo: the cargo was not accepted for slot %d at its connect", peerSlot);
}

void MarkHostWrite() { g_nextPollMs = 0; }

void ForgetApplied() { g_haveApplied = false; }

bool SendTruthToSlot(int peerSlot, bool bounded) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || peerSlot < 1 || peerSlot >= static_cast<int>(coop::net::kMaxPeers)) return false;
    const uint64_t now = NowMs();
    if (bounded && g_truthMs[peerSlot] != 0 && now < g_truthMs[peerSlot] + kTruthEveryMs) return false;
    Items items;
    if (!ReadHostCargo(items) || !Send(s, items, peerSlot)) return false;
    g_truthMs[peerSlot] = now;
    return true;
}

void OnDisconnect() {
    g_asm.Clear();
    g_havePending = false;
    g_pending.clear();
    g_pendingHash = 0;
    g_haveApplied = false;
    g_appliedHash = 0;
    g_haveSent = false;
    g_sentHash = 0;
    g_nextPollMs = 0;
    g_nextSweepMs = 0;
    g_refusalSaid[0] = g_refusalSaid[1] = false;
    for (uint64_t& ms : g_truthMs) ms = 0;
}

}  // namespace coop::drone_cargo_sync
