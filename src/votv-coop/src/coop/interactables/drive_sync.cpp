// coop/drive_sync.cpp -- see coop/interactables/drive_sync.h.
//
// Two lanes over this module (the rack lane lives in drive_rack_sync):
//   DriveSlotState -- idempotent any-peer slot FSM lines, host canonical.
//   DrivePayload   -- drive data_0 rows (signal_wire codec, blob chunks).
// Detection = 0x45 verb dirty-marks (vm_dispatch; capture-only, barrier
// emission) + 1 Hz diff-gated sweeps. Apply+prime is GT-atomic per lane.
// This module OWNS the verb registration for the whole drive chain
// (one-callback-per-verb-name; putDriveIn is shared slot/rack ctx) and
// forwards rack marks to drive_rack_sync::MarkDirtyFromVerb().

#include "coop/interactables/drive_sync.h"

#include "coop/element/registry.h"
#include "coop/interactables/desk_snd_fx.h"   // ScopedWireApply (the shared desk wire guard)
#include "coop/interactables/drive_rack_sync.h"  // MarkDirtyFromVerb + TryConsumeDenyReap (owner API)
#include "coop/interactables/signal_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/props/prop_lifecycle.h"        // DestroyLocalProp (deny-ghost teardown)

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/vm_dispatch.h"
#include "ue_wrap/desk/drive_chain.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <vector>

namespace coop::drive_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace DC = ue_wrap::drive_chain;
namespace SD = ue_wrap::signal_dynamic;
namespace vm = ue_wrap::vm_dispatch;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- vm_dispatch verb ids ----
constexpr int kVerbPutDriveIn = 1;   // driveSlot OR rack context (ctx discriminates)
constexpr int kVerbPulledOut  = 2;
constexpr int kVerbPayload    = 3;   // saveSignal / deleteSignal / comp_uploadData (mark-all)
constexpr int kVerbRackTake   = 4;   // getDrive

bool g_verbsRegistered = false;

// ---- dirty marks (set in the VM bracket -- relaxed atomics, drained at Tick) ----
std::atomic<bool> g_slotDirty[DC::kRoleCount] = {};
std::atomic<bool> g_payloadDirty{false};
// Eject capture: at drivePulledOut ENTRY slot.drive is STILL SET -- stash the
// occupant eid so the empty line can name it (the latch completion needs it).
// EidForActor takes the registry mutex (held microseconds, GT-safe) -- a
// deliberate, justified in-bracket read (design R2/R3: memory + own maps only).
std::atomic<uint32_t> g_lastEjectEid[DC::kRoleCount] = {};

// ---- baselines ----
struct SlotBase { bool known = false; bool occupied = false; uint32_t eid = 0; };
SlotBase g_slotBase[DC::kRoleCount];

std::map<uint32_t, uint64_t> g_driveBase;   // drive eid -> row blob hash

bool g_primed = false;
bool g_wasConnected = false;

// ---- wire plumbing ----
uint32_t g_nextSeq = 1;
coop::blob_chunks::Assembler g_payloadAsm;

// ---- pending applies (drive actor not resolvable yet -- spawn in flight) ----
struct Pending {
    int kind;  // 0=slot line, 1=payload blob
    coop::net::DriveSlotStatePayload slotLine{};
    SlotBase slotAtQueue{};  // the role's baseline when queued -- replay only
                             // if the slot has NOT moved on since
    std::vector<uint8_t> blob;
    uint8_t senderSlot = 0xFF;
    Clock::time_point until{};
};
std::vector<Pending> g_pending;
constexpr auto kPendingTtl = std::chrono::seconds(10);
constexpr size_t kPendingCap = 256;  // drop-oldest + WARN past this

// ---- locally-authored drive births (client): the payload broadcasts at
// adoption ONLY for these; every other first sight is prime-only (a joiner's
// save-loaded drives are NOT its births).
struct NotedBirth { void* actor = nullptr; Clock::time_point until{}; };
std::vector<NotedBirth> g_notedBirths;
constexpr auto kNotedBirthTtl = std::chrono::seconds(30);

// ---- cadence ----
Clock::time_point g_nextSweep{};
Clock::time_point g_nextStats{};

// ---- counters (60 s line -- dead matchers visible) ----
std::atomic<uint64_t> g_cMarksSlot{0}, g_cMarksPayload{0};
uint64_t g_cSlotSent = 0, g_cSlotApplied = 0, g_cPayloadSent = 0, g_cPayloadApplied = 0;
uint64_t g_cLatchCompleted = 0;

// --------------------------------------------------------------------------
// helpers

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// eid -> live actor: coop::element::LivePropActor (O(1) Registry::Get; the
// promoted canonical resolve idiom).
using coop::element::LivePropActor;

// All live drive-class props as (eid, actor).
void SnapshotDrives(std::vector<std::pair<uint32_t, void*>>& out) {
    out.clear();
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    for (const auto& p : pairs) {
        if (!p.actor || !R::IsLiveByIndex(p.actor, p.internalIdx)) continue;
        if (!DC::IsDriveClass(R::ClassOf(p.actor))) continue;
        out.emplace_back(static_cast<uint32_t>(p.id), p.actor);
    }
}

bool RowIsDefault(const SD::Row& r) {
    return r.size <= 0.f && r.name.empty() && r.id.empty();
}

std::vector<uint8_t> PayloadBlob(uint32_t eid, const SD::Row& row) {
    std::vector<uint8_t> b = coop::signal_wire::Serialize(row, /*adopt*/false);
    std::vector<uint8_t> out(4 + b.size());
    std::memcpy(out.data(), &eid, 4);
    std::memcpy(out.data() + 4, b.data(), b.size());
    return out;
}

// --------------------------------------------------------------------------
// the 0x45 verb bracket (capture-only: relaxed marks + one stashed eid read)

void OnVerbEntry(const vm::Bracket& b) {
    switch (b.verbId) {
        case kVerbPutDriveIn: {
            const int role = DC::RoleOfSlotActor(b.ctx);
            if (role >= 0) {
                g_slotDirty[role].store(true, std::memory_order_relaxed);
                g_cMarksSlot.fetch_add(1, std::memory_order_relaxed);
            } else if (DC::IsRackClass(R::ClassOf(b.ctx))) {
                coop::drive_rack_sync::MarkDirtyFromVerb();
                g_payloadDirty.store(true, std::memory_order_relaxed);  // harvest zeroes a drive
            }
            break;
        }
        case kVerbPulledOut: {
            const int role = DC::RoleOfSlotActor(b.ctx);
            if (role >= 0) {
                void* d = DC::SlotDrive(b.ctx);  // still set at ENTRY (measured)
                const uint32_t eid = d ? static_cast<uint32_t>(
                    coop::element::Registry::Get().EidForActor(d)) : 0;
                g_lastEjectEid[role].store(eid, std::memory_order_relaxed);
                g_slotDirty[role].store(true, std::memory_order_relaxed);
                g_cMarksSlot.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        case kVerbPayload:
            g_payloadDirty.store(true, std::memory_order_relaxed);
            g_cMarksPayload.fetch_add(1, std::memory_order_relaxed);
            break;
        case kVerbRackTake:
            if (DC::IsRackClass(R::ClassOf(b.ctx)))
                coop::drive_rack_sync::MarkDirtyFromVerb();
            break;
        default: break;
    }
}

// --------------------------------------------------------------------------
// slot lane

void AnnounceSlot(int role, bool occupied, uint32_t eid) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    coop::net::DriveSlotStatePayload p{};
    p.role = static_cast<uint8_t>(role);
    p.occupied = occupied ? 1 : 0;
    p.censusIdx = 0;
    p.driveEid = eid;
    s->SendReliable(coop::net::ReliableKind::DriveSlotState, &p, sizeof(p));
    ++g_cSlotSent;
}

// Read a slot's live state; diff vs baseline; announce the edge. `announce`
// false = prime-only (the connect seed).
void ProcessSlot(int role, bool announce) {
    void* slot = DC::SlotActor(role);
    if (!slot) return;
    void* drive = DC::SlotDrive(slot);
    const uint32_t eid = drive ? static_cast<uint32_t>(
        coop::element::Registry::Get().EidForActor(drive)) : 0;
    const bool occupied = drive != nullptr;
    SlotBase& b = g_slotBase[role];
    if (b.known && b.occupied == occupied && b.eid == eid) return;
    const uint32_t ejectEid = g_lastEjectEid[role].exchange(0, std::memory_order_relaxed);
    b = {true, occupied, eid};
    if (announce)
        AnnounceSlot(role, occupied, occupied ? eid : ejectEid);
}

void OnSlotLine(const coop::net::DriveSlotStatePayload& p, uint8_t senderSlot, bool fromPending);

void RetryPendingTick();

// --------------------------------------------------------------------------
// payload lane

void SendPayload(uint32_t eid, const SD::Row& row, int toSlot /* -1 = all */) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    std::vector<uint8_t> blob = PayloadBlob(eid, row);
    const uint32_t seq = g_nextSeq++;
    if (toSlot < 0)
        coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::DrivePayload, seq, blob);
    else
        coop::blob_chunks::SendBlobToSlot(s, toSlot, coop::net::ReliableKind::DrivePayload, seq, blob);
    ++g_cPayloadSent;
}

// Diff-gated sweep over every live drive (1 Hz + on dirty-mark). Emission
// only on bytes-changed-vs-baseline (a spurious mark provably sends nothing).
void SweepPayloads(bool announce) {
    std::vector<std::pair<uint32_t, void*>> drives;
    SnapshotDrives(drives);
    for (const auto& [eid, actor] : drives) {
        SD::Row row;
        if (!DC::ReadDriveRow(actor, row)) continue;
        const uint64_t h = coop::blob_chunks::Fnv64(coop::signal_wire::Serialize(row, false));
        auto it = g_driveBase.find(eid);
        if (it == g_driveBase.end()) {
            g_driveBase[eid] = h;
            // Birth invariant: a NEW eid with a non-default payload broadcasts
            // ONLY when this peer authored the birth -- the HOST for its
            // organic world (delivery, host rack-take), a CLIENT only for a
            // drain-noted local birth (rack take / pocket re-place). A
            // joiner's save-loaded drives are first-sighted but NOT authored.
            bool authored = IsHost();
            if (!authored) {
                const auto now2 = Clock::now();
                for (auto& nb : g_notedBirths) {
                    if (nb.actor == actor && now2 < nb.until) {
                        authored = true;
                        nb = NotedBirth{};
                        break;
                    }
                }
            }
            if (announce && authored && !RowIsDefault(row)) SendPayload(eid, row, -1);
            continue;
        }
        if (it->second == h) continue;
        it->second = h;
        if (announce) SendPayload(eid, row, -1);
    }
    // Baseline GC: drop eids no longer live (bounded growth).
    if (g_driveBase.size() > drives.size() + 64) {
        std::map<uint32_t, uint64_t> keep;
        for (const auto& [eid, actor] : drives) {
            auto it = g_driveBase.find(eid);
            if (it != g_driveBase.end()) keep[eid] = it->second;
        }
        g_driveBase.swap(keep);
    }
}

void ApplyPayloadBlob(const std::vector<uint8_t>& blob, uint8_t senderSlot, bool fromPending) {
    if (blob.size() < 5) return;
    uint32_t eid = 0;
    std::memcpy(&eid, blob.data(), 4);
    void* actor = LivePropActor(eid);
    if (!actor || !DC::IsDriveClass(R::ClassOf(actor))) {
        if (!fromPending) {
            Pending pd;
            pd.kind = 1;
            pd.blob = blob;
            pd.senderSlot = senderSlot;
            pd.until = Clock::now() + kPendingTtl;
            if (g_pending.size() >= kPendingCap) {
                g_pending.erase(g_pending.begin());
                UE_LOGW("drive_sync: pending cap hit -- oldest dropped");
            }
            g_pending.push_back(std::move(pd));
        }
        return;
    }
    std::vector<uint8_t> rb(blob.begin() + 4, blob.end());
    SD::Row row;
    bool adopt = false;
    if (!coop::signal_wire::Deserialize(rb, row, adopt)) {
        UE_LOGW("drive_sync: payload blob for eid=%u malformed -- dropped", eid);
        return;
    }
    // The CONTENT-correlated deny reap. A denied rack take's ghost identifies itself here -- its
    // adoption payload hashes to exactly the row the winning take removed. The VERDICT
    // (ring+TTL+consume) is drive_rack_sync's, the take-race axis owner; the ACTION stays here.
    // The host destroys its own copy ECHO-SUPPRESSED, so no PropDestroy goes out and each peer
    // reaps its own ghost from the same deny op. Exact; a legitimate birth never matches.
    if (IsHost()) {
        const uint64_t rh = coop::blob_chunks::Fnv64(coop::signal_wire::Serialize(row, false));
        if (coop::drive_rack_sync::TryConsumeDenyReap(senderSlot, rh)) {
            coop::prop_lifecycle::DestroyLocalProp(actor, /*deferred*/true);
            UE_LOGW("drive_sync: reaped the denied rack-take ghost eid=%u from slot %u "
                    "(payload hash matched)", eid, senderSlot);
            return;
        }
    }
    {
        coop::desk_snd_fx::ScopedWireApply guard;
        if (!DC::WriteDriveRow(actor, row)) return;
        DC::CallDriveUpd(actor);
        SD::Row applied;
        if (DC::ReadDriveRow(actor, applied))
            g_driveBase[eid] = coop::blob_chunks::Fnv64(coop::signal_wire::Serialize(applied, false));
    }
    ++g_cPayloadApplied;
    UE_LOGI("drive_sync: payload applied eid=%u (name='%ls' size=%.0f) from slot %u",
            eid, row.name.c_str(), row.size, senderSlot);
}

// --------------------------------------------------------------------------
// slot line apply

void OnSlotLine(const coop::net::DriveSlotStatePayload& p, uint8_t senderSlot, bool fromPending) {
    if (p.role >= DC::kRoleCount) return;
    void* slot = DC::SlotActor(p.role);
    if (!slot) return;
    void* cur = DC::SlotDrive(slot);
    const uint32_t curEid = cur ? static_cast<uint32_t>(
        coop::element::Registry::Get().EidForActor(cur)) : 0;

    if (p.occupied) {
        if (cur && curEid == p.driveEid) {  // already true: prime-only no-op
            g_slotBase[p.role] = {true, true, curEid};
            return;
        }
        void* drive = LivePropActor(p.driveEid);
        if (!drive || !DC::IsDriveClass(R::ClassOf(drive))) {
            if (!fromPending) {
                // At most ONE pending line per role (a newer
                // line supersedes the older), and the baseline at queue time
                // is stashed so the replay DROPS if the slot moved on --
                // never resurrect a stale occupant.
                for (auto it2 = g_pending.begin(); it2 != g_pending.end();) {
                    if (it2->kind == 0 && it2->slotLine.role == p.role)
                        it2 = g_pending.erase(it2);
                    else ++it2;
                }
                Pending pd;
                pd.kind = 0;
                pd.slotLine = p;
                pd.slotAtQueue = g_slotBase[p.role];
                pd.senderSlot = senderSlot;
                pd.until = Clock::now() + kPendingTtl;
                if (g_pending.size() >= kPendingCap) {
                g_pending.erase(g_pending.begin());
                UE_LOGW("drive_sync: pending cap hit -- oldest dropped");
            }
            g_pending.push_back(pd);
            }
            return;
        }
        if (cur && curEid != p.driveEid) {
            // Conflict: locally captured a DIFFERENT drive. The HOST is
            // canonical: host re-announces its state; a client converges to
            // the incoming line (eject ours, insert theirs).
            if (IsHost()) {
                AnnounceSlot(p.role, true, curEid);
                return;
            }
            coop::desk_snd_fx::ScopedWireApply guard;
            DC::CallDrivePulledOut(slot);
            DC::CompleteEjectLatch(slot, cur);
            ++g_cLatchCompleted;
        }
        {
            coop::desk_snd_fx::ScopedWireApply guard;
            DC::CallPutDriveIn(slot, drive);
            g_slotBase[p.role] = {true, true, p.driveEid};
        }
        ++g_cSlotApplied;
        UE_LOGI("drive_sync: slot role=%u INSERT eid=%u applied (from slot %u)",
                p.role, p.driveEid, senderSlot);
    } else {
        if (!cur) {  // already empty: prime + belt latch completion
            g_slotBase[p.role] = {true, false, 0};
            DC::CompleteEjectLatch(slot, LivePropActor(p.driveEid));
            return;
        }
        {
            coop::desk_snd_fx::ScopedWireApply guard;
            DC::CallDrivePulledOut(slot);
            DC::CompleteEjectLatch(slot, cur);
            g_slotBase[p.role] = {true, false, 0};
        }
        ++g_cSlotApplied;
        ++g_cLatchCompleted;
        UE_LOGI("drive_sync: slot role=%u EJECT applied (was eid=%u, from slot %u)",
                p.role, curEid, senderSlot);
    }
}

void RetryPendingTick() {
    if (g_pending.empty()) return;
    const auto now = Clock::now();
    std::vector<Pending> keep;
    for (auto& pd : g_pending) {
        if (now >= pd.until) {
            UE_LOGW("drive_sync: pending apply kind=%d expired (actor never resolved)", pd.kind);
            continue;
        }
        bool done = false;
        if (pd.kind == 0) {
            // If the slot's state moved since the line was
            // queued, the world passed it by -- DROP, never replay stale.
            const SlotBase& nowB = g_slotBase[pd.slotLine.role];
            if (nowB.known != pd.slotAtQueue.known ||
                nowB.occupied != pd.slotAtQueue.occupied ||
                nowB.eid != pd.slotAtQueue.eid) {
                UE_LOGW("drive_sync: pending slot line role=%u dropped (slot moved on)",
                        pd.slotLine.role);
                continue;
            }
            void* drive = LivePropActor(pd.slotLine.driveEid);
            if (drive || !pd.slotLine.occupied) {
                OnSlotLine(pd.slotLine, pd.senderSlot, /*fromPending*/true);
                done = true;
            }
        } else if (pd.kind == 1) {
            uint32_t eid = 0;
            if (pd.blob.size() >= 4) std::memcpy(&eid, pd.blob.data(), 4);
            if (LivePropActor(eid)) {
                ApplyPayloadBlob(pd.blob, pd.senderSlot, /*fromPending*/true);
                done = true;
            }
        }
        if (!done) keep.push_back(std::move(pd));
    }
    g_pending.swap(keep);
}

void PrimeAll() {
    for (int r = 0; r < DC::kRoleCount; ++r) {
        g_slotBase[r] = {};
        g_lastEjectEid[r].store(0, std::memory_order_relaxed);
        ProcessSlot(r, /*announce*/false);
    }
    g_driveBase.clear();
    SweepPayloads(/*announce*/false);
}

}  // namespace

// --------------------------------------------------------------------------

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_verbsRegistered) return;
    if (!DC::EnsureResolved()) return;
    // One cb serves all six verb names; verbIds discriminate. putDriveIn
    // covers BOTH the slot FSM and the rack (ctx class discriminates).
    const bool ok =
        vm::RegisterVirtualVerb(L"putDriveIn",      kVerbPutDriveIn, &OnVerbEntry) &&
        vm::RegisterVirtualVerb(L"drivePulledOut",  kVerbPulledOut,  &OnVerbEntry) &&
        vm::RegisterVirtualVerb(L"getDrive",        kVerbRackTake,   &OnVerbEntry) &&
        vm::RegisterVirtualVerb(L"saveSignal",      kVerbPayload,    &OnVerbEntry) &&
        vm::RegisterVirtualVerb(L"deleteSignal",    kVerbPayload,    &OnVerbEntry) &&
        vm::RegisterVirtualVerb(L"comp_uploadData", kVerbPayload,    &OnVerbEntry);
    if (ok) {
        g_verbsRegistered = true;
        vm::SetEnabled(true);
        UE_LOGI("drive_sync: 6 verb matchers registered (0x45 dirty-marks armed)");
    }
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (!DC::EnsureResolved()) return;
    if (!g_verbsRegistered) Install(s);
    vm::TickResolvePending();

    // Connect seed: (re-)prime silently at the connected rising edge so the
    // state AT connect is the ground truth (no pre-connect edge storms).
    const bool conn = s->connected();
    if (!g_primed || (conn && !g_wasConnected)) {
        PrimeAll();
        g_primed = true;
        UE_LOGI("drive_sync: baselines primed (%s)", conn ? "connect seed" : "boot");
    }
    g_wasConnected = conn;
    if (!conn) return;

    // Barrier drain: verb dirty-marks -> immediate diff-gated processing.
    for (int r = 0; r < DC::kRoleCount; ++r)
        if (g_slotDirty[r].exchange(false, std::memory_order_relaxed))
            ProcessSlot(r, /*announce*/true);
    if (g_payloadDirty.exchange(false, std::memory_order_relaxed))
        SweepPayloads(/*announce*/true);

    const auto now = Clock::now();
    if (now >= g_nextSweep) {  // 1 Hz safety sweeps (matcher gaps: eraser wipe etc.)
        g_nextSweep = now + std::chrono::seconds(1);
        for (int r = 0; r < DC::kRoleCount; ++r) ProcessSlot(r, /*announce*/true);
        SweepPayloads(/*announce*/true);
        g_payloadAsm.Sweep(now, std::chrono::seconds(20));
        RetryPendingTick();
    }

    if (now >= g_nextStats) {
        g_nextStats = now + std::chrono::seconds(60);
        UE_LOGI("drive_sync: 60s marks slot=%llu payload=%llu | sent slot=%llu "
                "payload=%llu | applied slot=%llu payload=%llu | latchFix=%llu pending=%zu",
                (unsigned long long)g_cMarksSlot.load(std::memory_order_relaxed),
                (unsigned long long)g_cMarksPayload.load(std::memory_order_relaxed),
                (unsigned long long)g_cSlotSent, (unsigned long long)g_cPayloadSent,
                (unsigned long long)g_cSlotApplied, (unsigned long long)g_cPayloadApplied,
                (unsigned long long)g_cLatchCompleted, g_pending.size());
    }
}

void OnDriveSlotState(const coop::net::DriveSlotStatePayload& p, uint8_t senderSlot) {
    if (!DC::EnsureResolved()) return;
    OnSlotLine(p, senderSlot, /*fromPending*/false);
}

void OnDrivePayloadChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    std::vector<uint8_t> blob;
    if (!g_payloadAsm.OnChunk(p, senderSlot, blob)) return;
    if (!DC::EnsureResolved()) return;
    ApplyPayloadBlob(blob, senderSlot, /*fromPending*/false);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!DC::EnsureResolved()) return;
    // Slot lines.
    for (int r = 0; r < DC::kRoleCount; ++r) {
        void* slot = DC::SlotActor(r);
        if (!slot) continue;
        void* drive = DC::SlotDrive(slot);
        coop::net::DriveSlotStatePayload p{};
        p.role = static_cast<uint8_t>(r);
        p.occupied = drive ? 1 : 0;
        p.driveEid = drive ? static_cast<uint32_t>(
            coop::element::Registry::Get().EidForActor(drive)) : 0;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::DriveSlotState, &p, sizeof(p));
    }
    // Drive payloads (non-default only -- default rows are the CDO the joiner
    // already has via the save copy / mirror construction).
    std::vector<std::pair<uint32_t, void*>> drives;
    SnapshotDrives(drives);
    int sent = 0;
    for (const auto& [eid, actor] : drives) {
        SD::Row row;
        if (!DC::ReadDriveRow(actor, row) || RowIsDefault(row)) continue;
        SendPayload(eid, row, peerSlot);
        ++sent;
    }
    // (Rack canonicals ride drive_rack_sync's seed, called right after this
    // one in subsystems -- the shipped slot-lines -> payloads -> racks order.)
    UE_LOGI("drive_sync: connect seed -> joiner slot %d (3 slot lines, %d payloads)",
            peerSlot, sent);
}

void NoteLocalDriveBirth(void* actor) {
    if (!actor) return;
    const auto now = Clock::now();
    for (auto& nb : g_notedBirths) {
        if (!nb.actor || now >= nb.until) { nb = {actor, now + kNotedBirthTtl}; return; }
    }
    g_notedBirths.push_back({actor, now + kNotedBirthTtl});
}

void OnDisconnect() {
    for (int r = 0; r < DC::kRoleCount; ++r) {
        g_slotDirty[r].store(false, std::memory_order_relaxed);
        g_lastEjectEid[r].store(0, std::memory_order_relaxed);
        g_slotBase[r] = {};
    }
    g_payloadDirty.store(false, std::memory_order_relaxed);
    g_driveBase.clear();
    g_pending.clear();
    g_payloadAsm.Clear();
    g_notedBirths.clear();
    g_primed = false;
    g_wasConnected = false;
    g_session.store(nullptr, std::memory_order_release);
    UE_LOGI("drive_sync: teardown (slot/payload baselines + pending + noted births cleared)");
}

}  // namespace coop::drive_sync
