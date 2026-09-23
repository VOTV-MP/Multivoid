// coop/save/join_window_baseline.cpp -- see coop/save/join_window_baseline.h.

#include "coop/save/join_window_baseline.h"

#include "coop/element/element.h"    // Element::LiveActor
#include "coop/element/registry.h"   // the host's eid-to-actor lookup
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"  // the capture-instant collectors
#include "coop/props/prop_wire_parity.h"      // PhysFlagsOf, the state a correction carries
#include "ue_wrap/actors/prop.h"     // IsChipPile: a grabbed clump belongs to the convert stream
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"   // the host's current prop position

#include <chrono>
#include <cmath>     // the drift log
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace coop::join_window_baseline {
namespace {

coop::net::Session* g_session = nullptr;  // set once at Install (boot), read thereafter

// The keyed-prop keys the host's world held at the capture instant, which is what this joiner's
// blob contains. SendDivergenceDeletes diffs it against the then-live set at the connect edge and
// sends an explicit PropDestroy per key the host has since removed (MTA's Packet_EntityRemove),
// instead of the divergence sweep inferring the delete.
std::unordered_set<std::wstring> g_blobKeys[coop::net::kMaxPeers];

// The save-time position of every live keyless chipPile at the capture instant, by host eid: the
// positions the joiner loads its natives at. The connect replay stamps each pile's snapshot with
// it, so the client's twin destroy matches the save-loaded native at the old spot even when the
// host moved the pile in the join-load window.
std::unordered_map<coop::element::ElementId, ue_wrap::FVector>
    g_blobPileXforms[coop::net::kMaxPeers];

// The same for every live garbage clump at the capture instant: a clump at rest is in the save as
// a clump, so the joiner loads its own copy there. A map of its own: what reads the pile map (the
// position flush, a land convert's key) means a PILE at that key.
std::unordered_map<coop::element::ElementId, ue_wrap::FVector>
    g_blobClumpXforms[coop::net::kMaxPeers];

// The save-time position of every live off-form kerfur at the capture instant, by host eid; the
// host stamps it onto a KerfurConvert at a window turn-on so the client retires its stale local
// off-prop at the exact key. It outlives the snapshot (window turn-ons fire during the client's
// load tail) and clears at ClearForSlot and the late-flush expiry, the join window's true close; a
// later turn-on resolves by eid against an already-bound prop.
std::unordered_map<coop::element::ElementId, ue_wrap::FVector>
    g_blobKerfurXforms[coop::net::kMaxPeers];

// The save-time position of every live keyed prop at the capture instant, by host eid. A keyed
// prop rides the connect snapshot at the host's current position, but the joiner's own loadObjects
// re-creates it at the save position afterwards and clobbers that; the diverged flush re-asserts
// the host's live position at quiescence, past the clobber. Same lifetime and clears as the pile
// map.
std::unordered_map<coop::element::ElementId, ue_wrap::FVector>
    g_blobKeyedXforms[coop::net::kMaxPeers];
// Each of those keyed props' frozen and sleep bits at the same instant: the state the joiner's blob
// holds, so a flag change the host makes in the window without moving the prop is still a change.
std::unordered_map<coop::element::ElementId, uint8_t> g_blobKeyedState[coop::net::kMaxPeers];

// The late-armed flush: as a one-shot at the connect replay, a pile the host moved after that
// instant (a cluster cleared late in the joiner's long load tail) got no correction, its frozen
// save-position identity went stale and the position re-bind resurrected the old copy. The
// joiner's authoritative positions keep flushing for a window past world-ready, so every in-window
// move is delivered; a per-(slot, eid) last-sent position dedupes the wire to actual changes. The
// one-shot opens the window; TickLateArm re-runs on the cadence until it expires.
struct FlushedAt {
    ue_wrap::FVector pos;
    uint8_t          flags = 0;  // the propspawn_flags the correction carried
};
std::unordered_map<coop::element::ElementId, FlushedAt> g_lastFlushedPilePos[coop::net::kMaxPeers];
// The keyed half's own last-sent dedupe, sharing the arm window.
std::unordered_map<coop::element::ElementId, FlushedAt> g_lastFlushedKeyedPos[coop::net::kMaxPeers];
// The keyed scan reads GetActorLocation (a UFunction dispatch) for every keyed prop, about 2,000
// in a mature world, and at the pile cadence it hitched the host's game thread through the join
// tail. It runs on the first run (every already-moved prop) and then every Nth late-arm tick, a
// full scan each time, so a late move is still caught within a few seconds.
constexpr int kKeyedLateArmEvery = 5;   // 2 Hz / 5 = ~0.4 Hz keyed re-scan
int g_keyedLateArmTick[coop::net::kMaxPeers]{};
std::chrono::steady_clock::time_point g_flushArmUntil[coop::net::kMaxPeers]{};
std::chrono::steady_clock::time_point g_flushLastRun[coop::net::kMaxPeers]{};
constexpr auto kFlushLateWindow = std::chrono::seconds(25);       // cover a long load tail + late clusters
constexpr auto kFlushCadence    = std::chrono::milliseconds(500); // 2 Hz re-flush (cold; deduped to changes)

// The diverged-position flush, one reconcile for every save-authoritative entity: a pile has no
// wire-position channel (both peers load it from the identical save), and a keyed prop rides the
// snapshot but the joiner's loadObjects re-creates it at the save position afterwards; either
// way a host move in the join window goes stale on the client. Per the joiner's save-time maps,
// the host's current position is compared and, where it diverged, a PropSnapPos is sent for the
// client to apply at quiescence, after its loadObjects. A pure position compare, independent of
// any convert's timing. The first run arms the late window and resets the dedupe; the cadence
// runs deliver only new moves.
void FlushDivergedPositions_(int peerSlot, bool firstRun) {
    if (!g_session || peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    const auto& pileM  = g_blobPileXforms[peerSlot];
    const auto& keyedM = g_blobKeyedXforms[peerSlot];
    if (pileM.empty() && keyedM.empty()) return;  // stale-fallback join (no save-time maps captured)
    if (firstRun) {
        g_lastFlushedPilePos[peerSlot].clear();
        g_lastFlushedKeyedPos[peerSlot].clear();
        g_keyedLateArmTick[peerSlot] = 0;
        g_flushArmUntil[peerSlot] = std::chrono::steady_clock::now() + kFlushLateWindow;
    }
    constexpr float kDivergeCm2 = 4.0f * 4.0f;  // >4cm moved (above settle jitter) = a real in-window move
    constexpr float kResendCm2  = 4.0f * 4.0f;  // only re-send when the pos moved >4cm from what we last sent

    // The shared sender: a PropSnapPos at the actor's current transform and physics state, deduped
    // per (slot, eid); true when a correction went out. Once one went out, a prop back at its save
    // position is a move too: an extinguisher taken off its mount and put back. `kind` is a log tag.
    auto sendCorrection = [&](coop::element::ElementId eid, const ue_wrap::FVector& savePos, void* actor,
                              std::unordered_map<coop::element::ElementId, FlushedAt>& lastSent,
                              const char* kind, int capturedBits) -> bool {
        namespace pf = coop::net::propspawn_flags;
        const ue_wrap::FVector cur = ue_wrap::engine::GetActorLocation(actor);
        const float dx = cur.X - savePos.X, dy = cur.Y - savePos.Y, dz = cur.Z - savePos.Z;
        // The full state is read only for a prop that moved, changed its frozen or sleep bits (two raw
        // reads, `capturedBits` -1 for a pile) or was corrected already, never across the whole keyed
        // scan: PhysFlagsOf costs two engine calls. 0 for a pile.
        uint8_t flags = 0;
        const auto it = lastSent.find(eid);
        if (it == lastSent.end()) {
            const bool moved = dx * dx + dy * dy + dz * dz > kDivergeCm2;
            const bool restated = capturedBits >= 0 &&
                                  coop::prop_wire_parity::FrozenSleepBitsOf(actor) != static_cast<uint8_t>(capturedBits);
            if (!moved && !restated) return false;  // unmoved and unchanged (save IS current)
            flags = coop::prop_wire_parity::PhysFlagsOf(actor);
        } else {
            flags = coop::prop_wire_parity::PhysFlagsOf(actor);
            const float sx = cur.X - it->second.pos.X, sy = cur.Y - it->second.pos.Y, sz = cur.Z - it->second.pos.Z;
            const bool sameState = ((flags ^ it->second.flags) & (pf::kFrozen | pf::kSleep)) == 0;
            if (sx * sx + sy * sy + sz * sz <= kResendCm2 && sameState)
                return false;  // already delivered at this position and state
        }
        const ue_wrap::FRotator rot = ue_wrap::engine::GetActorRotation(actor);
        coop::net::PropSnapPosPayload p{};
        p.eid = static_cast<uint32_t>(eid);
        p.locX = cur.X; p.locY = cur.Y; p.locZ = cur.Z;
        p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
        p.physFlags = flags;
        g_session->SendReliableToSlot(peerSlot, coop::net::ReliableKind::PropSnapPos, &p, sizeof(p));
        lastSent[eid] = FlushedAt{cur, flags};
        UE_LOGI("[PILE-B3] HOST slot %d %s pos-correction eid=%u save=(%.1f,%.1f,%.1f) -> current=(%.1f,%.1f,%.1f) "
                "drift=%.1fcm flags=0x%02x (%s in-window move -> deliver the authoritative position)",
                peerSlot, kind, static_cast<unsigned>(eid), savePos.X, savePos.Y, savePos.Z,
                cur.X, cur.Y, cur.Z, std::sqrt(dx * dx + dy * dy + dz * dz), static_cast<unsigned>(flags),
                firstRun ? "one-shot" : "late-arm");
        return true;
    };

    int sentPile = 0, checkedPile = 0;
    for (const auto& [eid, savePos] : pileM) {
        ++checkedPile;
        coop::element::Element* el = coop::element::Registry::Get().Get(eid);
        void* actor = el ? el->LiveActor() : nullptr;  // slot-validated
        if (!actor) continue;  // dead -> skip
        // Only a resting chipPile native gets a correction: a pile the host is grabbing or throwing
        // is a clump on an arc, which the convert stream owns, and chasing its waypoints armed
        // twins at airborne spots and doubled the pile.
        if (!ue_wrap::prop::IsChipPile(actor)) continue;  // grabbed clump / proxy -> the convert stream owns it
        if (sendCorrection(eid, savePos, actor, g_lastFlushedPilePos[peerSlot], "pile", -1)) ++sentPile;
    }

    // The keyed scan runs on the first run, then every Nth late-arm tick.
    bool doKeyed = firstRun;
    if (!firstRun && !keyedM.empty() && ++g_keyedLateArmTick[peerSlot] >= kKeyedLateArmEvery) {
        g_keyedLateArmTick[peerSlot] = 0;
        doKeyed = true;
    }
    int sentKeyed = 0, checkedKeyed = 0;
    for (auto kit = keyedM.begin(); doKeyed && kit != keyedM.end(); ++kit) {
        const auto& eid = kit->first;
        const auto& savePos = kit->second;
        ++checkedKeyed;
        coop::element::Element* el = coop::element::Registry::Get().Get(eid);
        void* actor = el ? el->LiveActor() : nullptr;  // slot-validated
        if (!actor) continue;  // dead (e.g. hold-R pickup destroyed it) -> skip
        // The keyed path arms only the generic correction (the receiver's pile matcher matches
        // nothing for a keyed eid, so no twin and no dupe risk), and a host carrying the prop
        // mid-join sends transient positions that the late arm corrects, so no resting gate. A
        // keyed eid resolving to a pile is the pile map's row.
        if (ue_wrap::prop::IsChipPile(actor)) continue;
        const auto st = g_blobKeyedState[peerSlot].find(eid);
        const int captured = (st == g_blobKeyedState[peerSlot].end()) ? -1 : st->second;
        if (sendCorrection(eid, savePos, actor, g_lastFlushedKeyedPos[peerSlot], "keyed", captured)) ++sentKeyed;
    }

    if (sentPile > 0 || sentKeyed > 0 || firstRun)
        UE_LOGI("[PILE-B3] HOST slot %d diverged save-pos flush (%s) -- pile %d/%d + keyed %d/%d "
                "correction(s)/checked (connect-snapshot save-authoritative hole closed for piles AND keyed "
                "props; late-armed through the join tail)",
                peerSlot, firstRun ? "one-shot arm" : "late-arm tick",
                sentPile, checkedPile, sentKeyed, checkedKeyed);
}

}  // namespace

void Install(coop::net::Session* session) { g_session = session; }

void CaptureForSlot(int peerSlot) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    // The keyed-prop set this blob contains (the host's live keyed props this instant), diffed at
    // the connect edge.
    g_blobKeys[peerSlot].clear();
    coop::prop_element_tracker::CollectTrackedKeyedPropKeys(g_blobKeys[peerSlot]);
    // Every live keyless chipPile's save-time position at this same instant, for the connect
    // replay's match key.
    g_blobPileXforms[peerSlot].clear();
    coop::prop_element_tracker::CollectTrackedPileTransforms(g_blobPileXforms[peerSlot]);
    g_blobClumpXforms[peerSlot].clear();
    coop::prop_element_tracker::CollectTrackedClumpTransforms(g_blobClumpXforms[peerSlot]);
    // Every live off-form kerfur's save-time position at this same instant, for a window turn-on's
    // KerfurConvert.
    g_blobKerfurXforms[peerSlot].clear();
    coop::prop_element_tracker::CollectTrackedKerfurTransforms(g_blobKerfurXforms[peerSlot]);
    // Every live keyed prop's save-time position at this same instant, for the diverged flush past
    // the joiner's loadObjects clobber.
    g_blobKeyedXforms[peerSlot].clear();
    coop::prop_element_tracker::CollectTrackedKeyedPropTransforms(g_blobKeyedXforms[peerSlot]);
    g_blobKeyedState[peerSlot].clear();
    for (const auto& [eid, pos] : g_blobKeyedXforms[peerSlot]) {
        coop::element::Element* el = coop::element::Registry::Get().Get(eid);
        if (void* actor = el ? el->LiveActor() : nullptr)
            g_blobKeyedState[peerSlot][eid] = coop::prop_wire_parity::FrozenSleepBitsOf(actor);
    }
    UE_LOGI("join_window_baseline: slot %d -- captured %zu keyed-prop keys + %zu pile + %zu clump + "
            "%zu kerfur + %zu keyed save-time xforms at the blob instant",
            peerSlot, g_blobKeys[peerSlot].size(), g_blobPileXforms[peerSlot].size(),
            g_blobClumpXforms[peerSlot].size(), g_blobKerfurXforms[peerSlot].size(),
            g_blobKeyedXforms[peerSlot].size());
}

void SendDivergenceDeletes(int peerSlot) {
    if (!g_session || peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    auto& blobKeys = g_blobKeys[peerSlot];
    if (blobKeys.empty()) return;  // no live-capture baseline (stale-fallback join) -- sweep owns it
    // The host's live keyed-prop set.
    std::unordered_set<std::wstring> liveKeys;
    coop::prop_element_tracker::CollectTrackedKeyedPropKeys(liveKeys);
    int sent = 0;
    for (const std::wstring& k : blobKeys) {
        if (liveKeys.count(k)) continue;  // still live -- the snapshot (re)asserts it
        // The blob had this prop and the host no longer does: named explicitly, so the joiner drops
        // exactly it.
        coop::net::PropDestroyPayload dp{};
        dp.key.len = 0;
        for (size_t i = 0; i < k.size() && i < 31; ++i)
            dp.key.data[dp.key.len++] = static_cast<char>(k[i]);
        dp.elementId = 0;  // resolve by key: the eid is host-range-unstable across a transfer
        // Per slot: the divergence is this joiner's blob's. On the bulk lane ahead of the snapshot
        // bracket, so the removes land before the adds.
        g_session->SendReliableToSlot(peerSlot, coop::net::ReliableKind::PropDestroy,
                                      &dp, sizeof(dp));
        ++sent;
    }
    UE_LOGI("join_window_baseline: slot %d -- blob-vs-live diff sent %d explicit PropDestroy "
            "(blob had %zu keyed props, host live has %zu) [R2 MTA Packet_EntityRemove]",
            peerSlot, sent, blobKeys.size(), liveKeys.size());
    blobKeys.clear();
}

void FlushDivergedPositions(int peerSlot) {
    FlushDivergedPositions_(peerSlot, /*firstRun=*/true);
}

// The host cadence: each armed joiner's authoritative positions keep flushing for the late window
// past its one-shot, so a move after the one-shot still arrives.
void TickLateArm() {
    if (!g_session) return;
    const auto now = std::chrono::steady_clock::now();
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (g_flushArmUntil[slot].time_since_epoch().count() == 0) continue;  // never armed
        if (now >= g_flushArmUntil[slot]) {                                    // window expired -> disarm + free dedup
            g_flushArmUntil[slot] = {};
            g_lastFlushedPilePos[slot].clear();
            g_lastFlushedKeyedPos[slot].clear();
            // The join window closes here for the slot. The save-time maps are join-window
            // structures, and with "active join" defined as a non-empty map that nothing emptied,
            // every steady-state host pile grab kept stamping its pre-grab key and every landing
            // carried one; the client then armed a hopeless pending twin per drop (its native was
            // already retired at the hand-off), and each pinned the quiescence drain for many
            // full-array sweeps, a hitch storm during pile play. The late flush is the maps' last
            // consumer: at its expiry the joiner has quiesced and reconciled, and a late kerfur
            // turn-on resolves by eid.
            if (!g_blobPileXforms[slot].empty() || !g_blobClumpXforms[slot].empty() ||
                !g_blobKerfurXforms[slot].empty() || !g_blobKeyedXforms[slot].empty()) {
                UE_LOGI("[PILE-09] slot %d join window CLOSED (b3 late-flush expiry) -- retiring "
                        "save-time maps (%zu pile + %zu clump + %zu kerfur + %zu keyed xform(s)); steady-state "
                        "grabs no longer stamp save-time keys for this joiner",
                        slot, g_blobPileXforms[slot].size(), g_blobClumpXforms[slot].size(),
                        g_blobKerfurXforms[slot].size(), g_blobKeyedXforms[slot].size());
                g_blobPileXforms[slot].clear();
                g_blobClumpXforms[slot].clear();
                g_blobKerfurXforms[slot].clear();
                g_blobKeyedXforms[slot].clear();
                g_blobKeyedState[slot].clear();
            }
            continue;
        }
        if (now - g_flushLastRun[slot] < kFlushCadence) continue;              // debounce to the cadence
        g_flushLastRun[slot] = now;
        FlushDivergedPositions_(slot, /*firstRun=*/false);
    }
}

bool TryGetPileXform(int peerSlot, coop::element::ElementId eid, ue_wrap::FVector& out) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return false;
    const auto& m = g_blobPileXforms[peerSlot];
    auto it = m.find(eid);
    if (it == m.end()) return false;
    out = it->second;
    return true;
}

bool TryGetClumpXform(int peerSlot, coop::element::ElementId eid, ue_wrap::FVector& out) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return false;
    const auto& m = g_blobClumpXforms[peerSlot];
    auto it = m.find(eid);
    if (it == m.end()) return false;
    out = it->second;
    return true;
}

bool TryGetPileXformAnySlot(coop::element::ElementId eid, ue_wrap::FVector& out) {
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        const auto& m = g_blobPileXforms[slot];
        auto it = m.find(eid);
        if (it != m.end()) { out = it->second; return true; }
    }
    return false;
}

bool TryGetKerfurXformAnySlot(coop::element::ElementId eid, ue_wrap::FVector& out) {
    // A KerfurConvert is one fan-out with no slot, and a kerfur's host eid is unique, so all active
    // slots are searched.
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        const auto& m = g_blobKerfurXforms[slot];
        auto it = m.find(eid);
        if (it != m.end()) { out = it->second; return true; }
    }
    return false;
}

void RecordGrabTimePileXform(coop::element::ElementId eid, const ue_wrap::FVector& preGrabLoc) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    int slots = 0;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (g_blobPileXforms[slot].empty()) continue;   // no active join captured piles here
        g_blobPileXforms[slot][eid] = preGrabLoc;
        ++slots;
    }
    if (slots > 0)
        UE_LOGI("[PILE-09] HOST pre-grab pos recorded eid=%u at (%.1f,%.1f,%.1f) -> %d active join slot(s) "
                "(the kToPile convert will carry it as the save-time key)",
                static_cast<unsigned>(eid), preGrabLoc.X, preGrabLoc.Y, preGrabLoc.Z, slots);
}

void ClearForSlot(int peerSlot) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    g_blobKeys[peerSlot].clear();  // the unconsumed blob baseline
    g_blobPileXforms[peerSlot].clear();  // and the save-time maps
    g_blobClumpXforms[peerSlot].clear();
    g_blobKerfurXforms[peerSlot].clear();
    g_blobKeyedXforms[peerSlot].clear();
    g_blobKeyedState[peerSlot].clear();
    g_flushArmUntil[peerSlot] = {};        // disarm the late flush and drop its dedupe baselines
    g_lastFlushedPilePos[peerSlot].clear();
    g_lastFlushedKeyedPos[peerSlot].clear();
}

bool HasCapture(int peerSlot) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return false;
    return !g_blobKeyedXforms[peerSlot].empty();  // filled at the capture, retired when the window closes
}

bool IsLateWindowOpen(int peerSlot) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return false;
    return g_flushArmUntil[peerSlot].time_since_epoch().count() != 0;  // disarmed at expiry and at ClearForSlot
}

}  // namespace coop::join_window_baseline
