// coop/interactables/drive_rack_sync.cpp -- see coop/interactables/drive_rack_sync.h.
//
// The rack lane, split out of drive_sync. Detection is the putDriveIn and getDrive marks that
// drive_sync's bracket forwards through MarkDirtyFromVerb, plus the 1 Hz diff-gated sweep.
// Apply and prime are atomic on the game thread.

#include "coop/interactables/drive_rack_sync.h"

#include "coop/element/registry.h"
#include "coop/interactables/desk_snd_fx.h"   // ScopedWireApply (the shared desk wire guard)
#include "coop/interactables/signal_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/props/prop_lifecycle.h"        // DestroyLocalProp (deny-ghost teardown)

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/engine/engine.h"            // SpawnActor + GetActorLocation (set-deny refund)

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <vector>

namespace coop::drive_rack_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace DC = ue_wrap::drive_chain;
namespace SD = ue_wrap::signal_dynamic;
using Clock = std::chrono::steady_clock;
using coop::element::LivePropActor;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- dirty mark (set via MarkDirtyFromVerb -- relaxed, drained at Tick) ----
std::atomic<bool> g_rackDirty{false};

// ---- baselines ----
std::map<uint32_t, uint64_t> g_rackBase;    // rack eid -> arrays blob hash
std::map<uint32_t, std::vector<uint8_t>> g_rackShadow;  // rack eid -> last canonical-form blob (client op derivation)

bool g_primed = false;
bool g_wasConnected = false;

// ---- wire plumbing ----
uint32_t g_nextSeq = 1;
coop::blob_chunks::Assembler g_rackAsm;

// ---- pending applies (rack actor not resolvable yet -- spawn in flight).
// Replay predicate = resolve-only (canonicals are absolute states; two queued
// blobs replay in insertion order, newest wins).
struct Pending {
    std::vector<uint8_t> blob;
    Clock::time_point until{};
};
std::vector<Pending> g_pending;
constexpr auto kPendingTtl = std::chrono::seconds(10);
constexpr size_t kPendingCap = 256;  // drop-oldest and WARN past this

// ---- rack take deny records, correlated by CONTENT ----
// Matching a deny on its slot alone can reap a legitimate unrelated birth from the same peer.
// Drives carry no byte discriminator, so the correlation key is the ROW CONTENT HASH: the
// loser's ghost holds exactly the row the WINNING take removed, both having raced the same
// pre-race state. The host records the removed row's hash at every successful take apply, a
// deny arms {senderSlot, rowHash}, and the reap fires when the denied ghost's payload arrives
// with a matching hash -- exact, never a false positive. The birth intent itself is authored
// normally.
struct DenyRec { uint8_t slot = 0xFF; uint64_t rowHash = 0; Clock::time_point until{}; };
DenyRec g_denies[8];
constexpr auto kDenyTtl = std::chrono::seconds(10);
// Last-removed row hash per (rackEid, idx) -- feeds the deny correlation.
struct TakenRow { uint32_t rackEid = 0; uint8_t idx = 0xFF; uint64_t rowHash = 0;
                  Clock::time_point when{}; };
TakenRow g_takenRing[8];
int g_takenNext = 0;

// ---- cadence ----
Clock::time_point g_nextSweep{};
Clock::time_point g_nextStats{};

// ---- counters (60 s line -- dead matchers visible) ----
std::atomic<uint64_t> g_cMarksRack{0};
uint64_t g_cRackOps = 0, g_cRackCanon = 0, g_cDenies = 0, g_cCanonApplied = 0;

// --------------------------------------------------------------------------
// helpers

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

void SnapshotRacks(std::vector<std::pair<uint32_t, void*>>& out) {
    out.clear();
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    for (const auto& p : pairs) {
        if (!p.actor || !R::IsLiveByIndex(p.actor, p.internalIdx)) continue;
        if (!DC::IsRackClass(R::ClassOf(p.actor))) continue;
        out.emplace_back(static_cast<uint32_t>(p.id), p.actor);
    }
}

// The rack canonical form: RackStateHead{op=3} + 16 x (u8 has + signal_wire row).
std::vector<uint8_t> RackCanonicalBlob(uint32_t rackEid, const DC::RackRow rows[DC::kRackSlots]) {
    coop::net::RackStateHead h{};
    h.rackEid = rackEid;
    h.op = 3;
    std::vector<uint8_t> out(sizeof(h));
    std::memcpy(out.data(), &h, sizeof(h));
    for (int i = 0; i < DC::kRackSlots; ++i) {
        out.push_back(rows[i].has ? 1 : 0);
        std::vector<uint8_t> rb = coop::signal_wire::Serialize(rows[i].row, false);
        const uint16_t len = static_cast<uint16_t>(rb.size());
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.insert(out.end(), rb.begin(), rb.end());
    }
    return out;
}

bool ParseRackCanonical(const std::vector<uint8_t>& blob, DC::RackRow out[DC::kRackSlots]) {
    size_t off = sizeof(coop::net::RackStateHead);
    for (int i = 0; i < DC::kRackSlots; ++i) {
        if (off + 3 > blob.size()) return false;
        out[i].has = blob[off] != 0;
        const uint16_t len = static_cast<uint16_t>(blob[off + 1] | (blob[off + 2] << 8));
        off += 3;
        if (off + len > blob.size()) return false;
        std::vector<uint8_t> rb(blob.begin() + off, blob.begin() + off + len);
        bool adopt = false;
        if (!coop::signal_wire::Deserialize(rb, out[i].row, adopt)) return false;
        off += len;
    }
    return true;
}

void RecordDeny(uint8_t slot, uint64_t rowHash) {
    if (!rowHash) return;  // no correlation possible -> never arm a blind reap
    const auto now = Clock::now();
    for (auto& d : g_denies) {
        if (d.slot == 0xFF || now >= d.until) { d = {slot, rowHash, now + kDenyTtl}; return; }
    }
    g_denies[0] = {slot, rowHash, now + kDenyTtl};
}

// --------------------------------------------------------------------------
// the rack lane

void HostBroadcastRackCanonical(uint32_t rackEid, void* rack, int toSlot /* -1 = all */) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    DC::RackRow rows[DC::kRackSlots];
    int n = 0;
    if (!DC::ReadRack(rack, rows, n)) return;
    std::vector<uint8_t> blob = RackCanonicalBlob(rackEid, rows);
    const uint32_t seq = g_nextSeq++;
    if (toSlot < 0)
        coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::RackState, seq, blob);
    else
        coop::blob_chunks::SendBlobToSlot(s, toSlot, coop::net::ReliableKind::RackState, seq, blob);
    g_rackBase[rackEid] = coop::blob_chunks::Fnv64(blob);
    g_rackShadow[rackEid] = std::move(blob);
    ++g_cRackCanon;
}

void ClientSendRackOp(uint32_t rackEid, uint8_t op, uint8_t idx, const SD::Row* row) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    coop::net::RackStateHead h{};
    h.rackEid = rackEid;
    h.op = op;
    h.idx = idx;
    std::vector<uint8_t> blob(sizeof(h));
    std::memcpy(blob.data(), &h, sizeof(h));
    if (row) {
        std::vector<uint8_t> rb = coop::signal_wire::Serialize(*row, false);
        blob.insert(blob.end(), rb.begin(), rb.end());
    }
    coop::blob_chunks::SendBlobToSlot(s, 0, coop::net::ReliableKind::RackState, g_nextSeq++, blob);
    ++g_cRackOps;
}

// Derive index ops from the shadow (client) or broadcast canonical (host).
void SweepRacks(bool announce) {
    std::vector<std::pair<uint32_t, void*>> racks;
    SnapshotRacks(racks);
    const bool host = IsHost();
    for (const auto& [eid, rack] : racks) {
        DC::RackRow rows[DC::kRackSlots];
        int n = 0;
        if (!DC::ReadRack(rack, rows, n)) continue;
        std::vector<uint8_t> blob = RackCanonicalBlob(eid, rows);
        const uint64_t h = coop::blob_chunks::Fnv64(blob);
        auto it = g_rackBase.find(eid);
        if (it != g_rackBase.end() && it->second == h) continue;
        if (!announce || it == g_rackBase.end()) {  // first sight: prime silently
            g_rackBase[eid] = h;
            g_rackShadow[eid] = std::move(blob);
            continue;
        }
        if (host) {
            // Record organically-removed rows into the taken ring too (the
            // host's own player winning a take race must arm the same
            // content correlation as a client-op take).
            DC::RackRow prev[DC::kRackSlots];
            auto sh = g_rackShadow.find(eid);
            if (sh != g_rackShadow.end() && ParseRackCanonical(sh->second, prev)) {
                for (int i = 0; i < DC::kRackSlots; ++i) {
                    if (prev[i].has && !rows[i].has) {
                        g_takenRing[g_takenNext] = {eid, static_cast<uint8_t>(i),
                            coop::blob_chunks::Fnv64(coop::signal_wire::Serialize(prev[i].row, false)),
                            Clock::now()};
                        g_takenNext = (g_takenNext + 1) % 8;
                    }
                }
            }
            g_rackBase[eid] = h;
            g_rackShadow[eid] = blob;
            HostBroadcastRackCanonical(eid, rack, -1);
            UE_LOGI("drive_rack: host rack eid=%u organic change -- canonical broadcast", eid);
        } else {
            // Client: derive ops vs the shadow, send to host, keep the shadow
            // at the SENT state (the canonical will confirm / correct).
            DC::RackRow prev[DC::kRackSlots];
            bool havePrev = false;
            auto sh = g_rackShadow.find(eid);
            if (sh != g_rackShadow.end()) havePrev = ParseRackCanonical(sh->second, prev);
            for (int i = 0; i < DC::kRackSlots; ++i) {
                const bool was = havePrev && prev[i].has;
                if (was && !rows[i].has) {
                    // Remember the removed row's hash (the deny content match).
                    g_takenRing[g_takenNext] = {eid, static_cast<uint8_t>(i),
                        coop::blob_chunks::Fnv64(coop::signal_wire::Serialize(prev[i].row, false)),
                        Clock::now()};
                    g_takenNext = (g_takenNext + 1) % 8;
                    ClientSendRackOp(eid, 1, static_cast<uint8_t>(i), nullptr);
                    UE_LOGI("drive_rack: rack eid=%u take idx=%d -> host", eid, i);
                } else if (rows[i].has && (!was ||
                           coop::blob_chunks::Fnv64(coop::signal_wire::Serialize(prev[i].row, false)) !=
                           coop::blob_chunks::Fnv64(coop::signal_wire::Serialize(rows[i].row, false)))) {
                    ClientSendRackOp(eid, 0, static_cast<uint8_t>(i), &rows[i].row);
                    UE_LOGI("drive_rack: rack eid=%u set idx=%d -> host", eid, i);
                }
            }
            g_rackBase[eid] = h;
            g_rackShadow[eid] = std::move(blob);
        }
    }
}

void HostApplyRackOp(const coop::net::RackStateHead& h, const std::vector<uint8_t>& blob,
                     uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    void* rack = LivePropActor(h.rackEid);
    if (!rack || !DC::IsRackClass(R::ClassOf(rack))) return;
    DC::RackRow rows[DC::kRackSlots];
    int n = 0;
    if (!DC::ReadRack(rack, rows, n) || h.idx >= DC::kRackSlots) return;

    auto deny = [&](uint8_t origOp) {
        coop::net::RackStateHead d{};
        d.rackEid = h.rackEid;
        d.op = 2;
        d.idx = h.idx;
        d._pad0 = origOp;
        std::vector<uint8_t> db(sizeof(d));
        std::memcpy(db.data(), &d, sizeof(d));
        if (senderSlot != 0 && senderSlot < coop::net::kMaxPeers)
            coop::blob_chunks::SendBlobToSlot(s, senderSlot,
                coop::net::ReliableKind::RackState, g_nextSeq++, db);
        ++g_cDenies;
    };

    if (h.op == 0) {  // set{idx, row}
        SD::Row row;
        bool adopt = false;
        std::vector<uint8_t> rb(blob.begin() + sizeof(h), blob.end());
        if (!coop::signal_wire::Deserialize(rb, row, adopt)) return;
        if (rows[h.idx].has) {
            // Raced: the slot filled first. Deny + REFUND: the presser's drive
            // actor is already destroyed on its side; the host rematerializes
            // it at the rack (host-authored spawn -> the watcher fans it; the
            // payload rides drive_sync's birth-invariant broadcast next sweep).
            deny(0);
            const auto loc = ue_wrap::engine::GetActorLocation(rack);
            void* refunded = ue_wrap::engine::SpawnActor(
                DC::DriveClass(), {loc.X, loc.Y, loc.Z + 120.f});
            if (refunded) {
                coop::desk_snd_fx::ScopedWireApply guard;
                DC::WriteDriveRow(refunded, row);
                DC::CallDriveUpd(refunded);
            }
            UE_LOGW("drive_rack: rack eid=%u set idx=%u from slot %u RACED -- denied + refund %s",
                    h.rackEid, h.idx, senderSlot, refunded ? "spawned" : "SPAWN FAILED");
            return;
        }
        DC::RackRow in;
        in.has = true;
        in.row = row;
        {
            coop::desk_snd_fx::ScopedWireApply guard;
            if (!DC::WriteRackRow(rack, h.idx, in)) return;
            DC::CallRackGen(rack);
        }
        HostBroadcastRackCanonical(h.rackEid, rack, -1);
        UE_LOGI("drive_rack: host applied rack set eid=%u idx=%u from slot %u",
                h.rackEid, h.idx, senderSlot);
    } else if (h.op == 1) {  // take{idx}
        if (!rows[h.idx].has) {
            deny(1);
            // Arm the CONTENT-correlated reap: the loser's ghost carries the
            // row the winning take removed at this (rackEid, idx).
            uint64_t rowHash = 0;
            const auto now = Clock::now();
            for (const auto& t : g_takenRing) {
                if (t.rackEid == h.rackEid && t.idx == h.idx &&
                    now - t.when < kDenyTtl) { rowHash = t.rowHash; break; }
            }
            RecordDeny(senderSlot, rowHash);
            UE_LOGW("drive_rack: rack eid=%u take idx=%u from slot %u RACED -- denied%s",
                    h.rackEid, h.idx, senderSlot,
                    rowHash ? " + content-reap armed" : " (no row hash -- no reap)");
            return;
        }
        // Record the removed row's hash BEFORE clearing (the deny correlation).
        g_takenRing[g_takenNext] = {h.rackEid, h.idx,
            coop::blob_chunks::Fnv64(coop::signal_wire::Serialize(rows[h.idx].row, false)),
            Clock::now()};
        g_takenNext = (g_takenNext + 1) % 8;
        DC::RackRow in;  // has=false, empty row
        {
            coop::desk_snd_fx::ScopedWireApply guard;
            if (!DC::WriteRackRow(rack, h.idx, in)) return;
            DC::CallRackGen(rack);
        }
        HostBroadcastRackCanonical(h.rackEid, rack, -1);
        UE_LOGI("drive_rack: host applied rack take eid=%u idx=%u from slot %u",
                h.rackEid, h.idx, senderSlot);
    }
}

void ClientApplyRackBlob(const coop::net::RackStateHead& h, const std::vector<uint8_t>& blob,
                         bool fromPending) {
    if (h.op == 2) {  // deny: destroy OUR ghost from that exact take (content-matched)
        // Never sweep all untracked drives -- a legitimate fresh birth would die too. The denied
        // take's ghost carries the row that sat at (rackEid, idx) in OUR shadow, so match by
        // content hash.
        uint64_t wantHash = 0;
        {
            const auto now = Clock::now();
            for (const auto& t : g_takenRing) {
                if (t.rackEid == h.rackEid && t.idx == h.idx &&
                    now - t.when < kDenyTtl) { wantHash = t.rowHash; break; }
            }
        }
        int destroyed = 0;
        if (wantHash) {
            for (void* obj : R::FindObjectsByClass(L"prop_drive_C")) {
                if (!obj || !R::IsLive(obj)) continue;
                if (coop::element::Registry::Get().EidForActor(obj) !=
                    coop::element::kInvalidId) continue;
                SD::Row grow;
                if (!DC::ReadDriveRow(obj, grow)) continue;
                if (coop::blob_chunks::Fnv64(coop::signal_wire::Serialize(grow, false)) != wantHash)
                    continue;
                coop::prop_lifecycle::DestroyLocalProp(obj, /*deferred*/true);
                ++destroyed;
                break;
            }
        }
        UE_LOGW("drive_rack: rack op=%u idx=%u DENIED by host -- %d content-matched ghost(s) "
                "destroyed%s", h._pad0, h.idx, destroyed,
                wantHash ? "" : " (no shadow row -- nothing destroyed; host reaps by payload)");
        return;
    }
    if (h.op != 3) return;  // clients only ever apply canonical (+deny)
    void* rack = LivePropActor(h.rackEid);
    if (!rack || !DC::IsRackClass(R::ClassOf(rack))) {
        if (!fromPending) {
            Pending pd;
            pd.blob = blob;
            pd.until = Clock::now() + kPendingTtl;
            if (g_pending.size() >= kPendingCap) {
                g_pending.erase(g_pending.begin());
                UE_LOGW("drive_rack: pending cap hit -- oldest dropped");
            }
            g_pending.push_back(std::move(pd));
        }
        return;
    }
    // Drain before adopt: push any local pending diff as ops FIRST.
    SweepRacks(/*announce*/true);
    DC::RackRow rows[DC::kRackSlots];
    if (!ParseRackCanonical(blob, rows)) {
        UE_LOGW("drive_rack: rack canonical eid=%u malformed -- dropped", h.rackEid);
        return;
    }
    {
        coop::desk_snd_fx::ScopedWireApply guard;
        for (int i = 0; i < DC::kRackSlots; ++i) DC::WriteRackRow(rack, i, rows[i]);
        DC::CallRackGen(rack);
    }
    DC::RackRow now[DC::kRackSlots];
    int n = 0;
    if (DC::ReadRack(rack, now, n)) {
        std::vector<uint8_t> nb = RackCanonicalBlob(h.rackEid, now);
        g_rackBase[h.rackEid] = coop::blob_chunks::Fnv64(nb);
        g_rackShadow[h.rackEid] = std::move(nb);
    }
    ++g_cCanonApplied;
    UE_LOGI("drive_rack: rack canonical adopted eid=%u", h.rackEid);
}

void RetryPendingTick() {
    if (g_pending.empty()) return;
    const auto now = Clock::now();
    std::vector<Pending> keep;
    for (auto& pd : g_pending) {
        if (now >= pd.until) {
            UE_LOGW("drive_rack: pending canonical expired (rack never resolved)");
            continue;
        }
        coop::net::RackStateHead h{};
        if (pd.blob.size() >= sizeof(h)) std::memcpy(&h, pd.blob.data(), sizeof(h));
        if (LivePropActor(h.rackEid)) {
            ClientApplyRackBlob(h, pd.blob, /*fromPending*/true);
            continue;
        }
        keep.push_back(std::move(pd));
    }
    g_pending.swap(keep);
}

void PrimeRacks() {
    g_rackBase.clear();
    g_rackShadow.clear();
    SweepRacks(/*announce*/false);
}

}  // namespace

// --------------------------------------------------------------------------

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Verb matchers live in drive_sync (one-callback-per-verb-name; putDriveIn
    // is shared slot/rack ctx) -- it forwards rack marks via MarkDirtyFromVerb.
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (!DC::EnsureResolved()) return;

    // Connect seed: (re-)prime silently at the connected rising edge so the
    // state AT connect is the ground truth (no pre-connect edge storms).
    const bool conn = s->connected();
    if (!g_primed || (conn && !g_wasConnected)) {
        PrimeRacks();
        g_primed = true;
        UE_LOGI("drive_rack: baselines primed (%s)", conn ? "connect seed" : "boot");
    }
    g_wasConnected = conn;
    if (!conn) return;

    // Barrier drain: verb dirty-mark -> immediate diff-gated processing.
    if (g_rackDirty.exchange(false, std::memory_order_relaxed))
        SweepRacks(/*announce*/true);

    const auto now = Clock::now();
    if (now >= g_nextSweep) {  // 1 Hz safety sweep (matcher gaps)
        g_nextSweep = now + std::chrono::seconds(1);
        SweepRacks(/*announce*/true);
        g_rackAsm.Sweep(now, std::chrono::seconds(20));
        RetryPendingTick();
    }

    if (now >= g_nextStats) {
        g_nextStats = now + std::chrono::seconds(60);
        UE_LOGI("drive_rack: 60s marks=%llu | ops=%llu canon=%llu applied=%llu | denies=%llu "
                "pending=%zu",
                (unsigned long long)g_cMarksRack.load(std::memory_order_relaxed),
                (unsigned long long)g_cRackOps, (unsigned long long)g_cRackCanon,
                (unsigned long long)g_cCanonApplied, (unsigned long long)g_cDenies,
                g_pending.size());
    }
}

void OnRackStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    std::vector<uint8_t> blob;
    if (!g_rackAsm.OnChunk(p, senderSlot, blob)) return;
    if (!DC::EnsureResolved()) return;
    coop::net::RackStateHead h{};
    if (blob.size() < sizeof(h)) return;
    std::memcpy(&h, blob.data(), sizeof(h));
    if (IsHost()) {
        if (h.op == 0 || h.op == 1) HostApplyRackOp(h, blob, senderSlot);
        // ops 2/3 never legitimately arrive at the host
    } else {
        ClientApplyRackBlob(h, blob, /*fromPending*/false);
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!DC::EnsureResolved()) return;
    std::vector<std::pair<uint32_t, void*>> racks;
    SnapshotRacks(racks);
    for (const auto& [eid, rack] : racks)
        HostBroadcastRackCanonical(eid, rack, peerSlot);
    UE_LOGI("drive_rack: connect seed -> joiner slot %d (%zu racks)", peerSlot, racks.size());
}

void MarkDirtyFromVerb() {
    g_rackDirty.store(true, std::memory_order_relaxed);
    g_cMarksRack.fetch_add(1, std::memory_order_relaxed);
}

bool TryConsumeDenyReap(uint8_t senderSlot, uint64_t rowHash) {
    const auto now = Clock::now();
    for (auto& d : g_denies) {
        if (d.slot == senderSlot && d.rowHash == rowHash && now < d.until) {
            d = DenyRec{};
            return true;
        }
    }
    return false;
}

void OnDisconnect() {
    g_rackDirty.store(false, std::memory_order_relaxed);
    g_rackBase.clear();
    g_rackShadow.clear();
    g_pending.clear();
    g_rackAsm.Clear();
    for (auto& d : g_denies) d = DenyRec{};
    for (auto& t : g_takenRing) t = TakenRow{};
    g_takenNext = 0;
    g_primed = false;
    g_wasConnected = false;
    g_session.store(nullptr, std::memory_order_release);
    UE_LOGI("drive_rack: teardown (baselines/shadow/pending/deny+taken rings cleared)");
}

}  // namespace coop::drive_rack_sync
