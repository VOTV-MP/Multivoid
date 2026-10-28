// coop/save/join_window_baseline.cpp -- see coop/save/join_window_baseline.h.

#include "coop/save/join_window_baseline.h"

#include "coop/element/element.h"    // Element::LiveActor
#include "coop/element/registry.h"   // the host's eid-to-actor lookup
#include "coop/net/session.h"
#include "coop/props/prop_drive_host.h"       // ContextOf: the drive context a correction carries
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
#include <vector>

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
// host moved the pile in the join-load window. The capture writes this map and nothing after it does,
// so an eid sits here or in the clump map below, with the key and form the save holds.
std::unordered_map<coop::element::ElementId, ue_wrap::FVector>
    g_blobPileXforms[coop::net::kMaxPeers];

// The same for every live garbage clump at the capture instant: a clump at rest is in the save as
// a clump, so the joiner loads its own copy there. A map of its own: what reads the pile map (the
// position flush) means a PILE at that key.
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
// prop rides the connect snapshot at the host's current position, and the joiner's own loadObjects has
// returned before the snapshot lands; the diverged flush re-asserts the host's live position at
// quiescence, which heals a host move inside the join window. Unlike the pile map, it lives on through
// a settle phase (<= 30 s) after the window closes, and clears when the settle ends.
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
// The keyed scan reads TryGetActorLocation (a UFunction dispatch) for every keyed prop, about 2,000
// in a mature world, and at the pile cadence it hitched the host's game thread through the join
// tail. It runs on the first run (every already-moved prop) and then every Nth late-arm tick, a
// full scan each time, so a late move is still caught within a few seconds.
constexpr int kKeyedLateArmEvery = 5;   // 2 Hz / 5 = ~0.4 Hz keyed re-scan
int g_keyedLateArmTick[coop::net::kMaxPeers]{};
std::chrono::steady_clock::time_point g_flushArmUntil[coop::net::kMaxPeers]{};
std::chrono::steady_clock::time_point g_flushLastRun[coop::net::kMaxPeers]{};
constexpr auto kFlushLateWindow = std::chrono::seconds(25);       // cover a long load tail + late clusters
constexpr auto kFlushCadence    = std::chrono::milliseconds(500); // 2 Hz re-flush (cold; deduped to changes)
// The keyed props a hand held after their correction had gone out: the first correction was read before the grab,
// so each is sent a follow-up once it is out of the
// hand and at rest. After the late window only these stay watched, until the set empties or
// g_settleUntil passes.
std::unordered_set<coop::element::ElementId> g_handSinceSent[coop::net::kMaxPeers];
std::chrono::steady_clock::time_point g_settleUntil[coop::net::kMaxPeers]{};
constexpr auto kHandSettleWindow = std::chrono::seconds(30);
// A world-ready re-announce during a slot's settle is said once; cleared when the settle ends.
bool g_settleReannounceSaid[coop::net::kMaxPeers]{};

// The shared sender: a PropSnapPos at the actor's current transform and physics state, deduped per
// (slot, eid); true when a correction went out. Once one went out, a prop back at its save position
// is a move too: an extinguisher taken off its mount and put back. `kind` is a log tag, `phase` names
// the pass that sent it.
bool SendCorrection_(int peerSlot, const char* phase, coop::element::ElementId eid,
                     const ue_wrap::FVector& savePos, void* actor,
                     std::unordered_map<coop::element::ElementId, FlushedAt>& lastSent,
                     const char* kind, int capturedBits) {
    namespace pf = coop::net::propspawn_flags;
    constexpr float kDivergeCm2 = 4.0f * 4.0f;  // >4cm moved (above settle jitter) = a real in-window move
    constexpr float kResendCm2  = 4.0f * 4.0f;  // only re-send when the pos moved >4cm from what we last sent
    ue_wrap::FVector cur{};
    if (!ue_wrap::engine::TryGetActorLocation(actor, cur)) return false;   // no read, no correction
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
    ue_wrap::FRotator rot{};
    if (!ue_wrap::engine::TryGetActorRotation(actor, rot)) return false;   // no read, no correction
    coop::net::PropSnapPosPayload p{};
    p.eid = static_cast<uint32_t>(eid);
    p.locX = cur.X; p.locY = cur.Y; p.locZ = cur.Z;
    p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
    p.physFlags = flags;
    const coop::prop_drive_host::DriveContext ctx =
        coop::prop_drive_host::ContextOf(actor, static_cast<uint32_t>(eid));
    p.driveGen  = ctx.gen;
    p.driveOpen = ctx.open ? 1 : 0;
    g_session->SendReliableToSlot(peerSlot, coop::net::ReliableKind::PropSnapPos, &p, sizeof(p));
    lastSent[eid] = FlushedAt{cur, flags};
    UE_LOGI("[PILE-B3] HOST slot %d %s pos-correction eid=%u save=(%.1f,%.1f,%.1f) -> current=(%.1f,%.1f,%.1f) "
            "drift=%.1fcm flags=0x%02x gen=%u open=%u (%s in-window move -> deliver the authoritative position)",
            peerSlot, kind, static_cast<unsigned>(eid), savePos.X, savePos.Y, savePos.Z,
            cur.X, cur.Y, cur.Z, std::sqrt(dx * dx + dy * dy + dz * dz), static_cast<unsigned>(flags),
            static_cast<unsigned>(p.driveGen), static_cast<unsigned>(p.driveOpen), phase);
    return true;
}

// Resting as prop_drive_host reads it (its kRestVelCmS): an unreadable body counts as rest, since
// there is nothing to wait on.
bool AtRest_(void* actor) {
    constexpr float kRestVelCmS = 1.0f;
    const ue_wrap::prop::VelocityState v = ue_wrap::prop::GetPhysicsVelocity(actor, /*linearOnly=*/true);
    if (!v.ok) return true;
    const float lin2 = v.linearCmS.X * v.linearCmS.X + v.linearCmS.Y * v.linearCmS.Y +
                       v.linearCmS.Z * v.linearCmS.Z;
    return lin2 < kRestVelCmS * kRestVelCmS;
}

// One keyed prop of the diverged flush, for the keyed scan and for the settle phase alike; true when
// a correction went out. A prop in a hand is the hand's: its stream is continuous and its release
// carries the pose, so no correction is read for it now. When a correction for it had already gone
// out, the prop is noted in g_handSinceSent: that correction was read at the pose the hand then
// took it from, so the client's last word for it is stale once the hand has moved it. The prop stays
// noted until it is out of the hand and at rest, and every pass sends it a follow-up at its current
// pose (deduped to a move past 4 cm of what was last delivered), so the last correction the client
// holds is the resting one. A driven prop is sent as any other,
// stamped with its drive context, and the client orders it.
// Divergence from MTA, which stamps a detach with a fresh element context
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:1689-1700, client
// reference/mtasa-blue/Client/mods/deathmatch/logic/rpc/CElementRPCs.cpp:389) because its server
// authors the detach: a keyed prop's release here is authored by its holder, which holds no host-side
// per-eid context to stamp, so the order comes from the Default lane (the follow-up lands behind any
// stale sample).
bool FlushKeyedProp_(int peerSlot, const char* phase, coop::element::ElementId eid,
                     const ue_wrap::FVector& savePos) {
    coop::element::Element* el = coop::element::Registry::Get().Get(eid);
    void* actor = el ? el->LiveActor() : nullptr;  // slot-validated
    auto& handSince = g_handSinceSent[peerSlot];
    // Dead (e.g. a hold-R pickup destroyed it): nothing left to correct, and no follow-up owed.
    if (!actor) { handSince.erase(eid); return false; }
    // The keyed path arms only the generic correction (the receiver's pile matcher matches
    // nothing for a keyed eid, so no twin and no dupe risk). A keyed eid resolving to a pile
    // is the pile map's row.
    if (ue_wrap::prop::IsChipPile(actor)) { handSince.erase(eid); return false; }
    auto& lastSent = g_lastFlushedKeyedPos[peerSlot];
    if (coop::prop_drive_host::InAHand(actor)) {
        if (lastSent.count(eid)) handSince.insert(eid);
        return false;
    }
    const auto st = g_blobKeyedState[peerSlot].find(eid);
    const int captured = (st == g_blobKeyedState[peerSlot].end()) ? -1 : st->second;
    const bool sent = SendCorrection_(peerSlot, phase, eid, savePos, actor, lastSent, "keyed", captured);
    // Unsent means it sits within 4 cm of what was last delivered; either way a prop at rest has had
    // its last word.
    if (handSince.count(eid) && AtRest_(actor)) handSince.erase(eid);
    return sent;
}

// The diverged-position flush, one reconcile for every save-authoritative entity: a pile has no
// wire-position channel (both peers load it from the identical save), and a keyed prop rides the
// snapshot, which lands after the joiner's loadObjects; either way a host move in the join window
// goes stale on the client. Per the joiner's save-time maps, the host's current position is
// compared and, where it diverged, a PropSnapPos is sent for the client to apply at quiescence,
// after its loadObjects. A pure position compare, independent of
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
        g_handSinceSent[peerSlot].clear();
        g_keyedLateArmTick[peerSlot] = 0;
        g_flushArmUntil[peerSlot] = std::chrono::steady_clock::now() + kFlushLateWindow;
    }
    const char* phase = firstRun ? "one-shot" : "late-arm";

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
        if (SendCorrection_(peerSlot, phase, eid, savePos, actor, g_lastFlushedPilePos[peerSlot], "pile", -1))
            ++sentPile;
    }

    // The keyed scan runs on the first run, then every Nth late-arm tick.
    bool doKeyed = firstRun;
    if (!firstRun && !keyedM.empty() && ++g_keyedLateArmTick[peerSlot] >= kKeyedLateArmEvery) {
        g_keyedLateArmTick[peerSlot] = 0;
        doKeyed = true;
    }
    int sentKeyed = 0, checkedKeyed = 0;
    for (auto kit = keyedM.begin(); doKeyed && kit != keyedM.end(); ++kit) {
        ++checkedKeyed;
        if (FlushKeyedProp_(peerSlot, phase, kit->first, kit->second)) ++sentKeyed;
    }

    if (sentPile > 0 || sentKeyed > 0 || firstRun)
        UE_LOGI("[PILE-B3] HOST slot %d diverged save-pos flush (%s) -- pile %d/%d + keyed %d/%d "
                "correction(s)/checked (connect-snapshot save-authoritative hole closed for piles AND keyed "
                "props; late-armed through the join tail)",
                peerSlot, firstRun ? "one-shot arm" : "late-arm tick",
                sentPile, checkedPile, sentKeyed, checkedKeyed);
}

// After the late window: only the props a hand held once their correction had gone out are still
// watched, each sent its follow-up and dropped from the set at rest.
void SettleHandHeld_(int peerSlot) {
    const auto& keyedM = g_blobKeyedXforms[peerSlot];
    auto& handSince = g_handSinceSent[peerSlot];
    // The set shrinks inside FlushKeyedProp_: walk a copy. Cold: a few props, 2 Hz, 30 s at most.
    const std::vector<coop::element::ElementId> watched(handSince.begin(), handSince.end());
    for (const coop::element::ElementId eid : watched) {
        const auto kit = keyedM.find(eid);
        if (kit != keyedM.end()) FlushKeyedProp_(peerSlot, "settle", eid, kit->second);
    }
}

// One cadence tick of a slot's settle phase; ends it when the set is empty or its window is over,
// retiring the keyed half of the join window's structures.
void SettleTick_(int peerSlot, std::chrono::steady_clock::time_point now) {
    auto& handSince = g_handSinceSent[peerSlot];
    const bool inWindow = now < g_settleUntil[peerSlot];
    if (inWindow && now - g_flushLastRun[peerSlot] >= kFlushCadence) {
        g_flushLastRun[peerSlot] = now;
        SettleHandHeld_(peerSlot);
    }
    if (inWindow && !handSince.empty()) return;
    const size_t unsettled = handSince.size();
    handSince.clear();
    g_settleUntil[peerSlot] = {};
    g_settleReannounceSaid[peerSlot] = false;
    g_flushArmUntil[peerSlot] = {};
    g_lastFlushedKeyedPos[peerSlot].clear();
    g_blobKeyedXforms[peerSlot].clear();
    g_blobKeyedState[peerSlot].clear();
    if (unsettled == 0)
        UE_LOGI("[PILE-09] slot %d hand-held settle ended -- the watched set emptied (each followed up at "
                "rest, dead, a pile, or unmoved); keyed save-time map retired", peerSlot);
    else
        UE_LOGW("[PILE-09] slot %d hand-held settle ended with %zu prop(s) unsettled (still in a hand or "
                "moving when its window ended); keyed save-time map retired", peerSlot, unsettled);
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
    // Every live keyed prop's save-time position at this same instant, for the diverged flush that
    // heals a host move in the join window.
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
        // Per slot: the divergence is this joiner's blob's. Inside the snapshot scope, ahead of the snapshot
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
    // A settling slot's join window is over and its keyed map is the first world's save-time one:
    // a re-announce now must not re-open a full flush over it.
    if (peerSlot >= 1 && peerSlot < coop::net::kMaxPeers &&
        g_settleUntil[peerSlot].time_since_epoch().count() != 0) {
        if (!g_settleReannounceSaid[peerSlot]) {
            g_settleReannounceSaid[peerSlot] = true;
            UE_LOGI("[PILE-B3] HOST slot %d world-ready re-announced during the hand settle -- the join "
                    "window is over; not re-opened", peerSlot);
        }
        return;
    }
    FlushDivergedPositions_(peerSlot, /*firstRun=*/true);
}

// The host cadence: each armed joiner's authoritative positions keep flushing for the late window
// past its one-shot, so a move after the one-shot still arrives. A prop a hand held after its
// correction went out keeps a slot watched for a follow-up past the window (SettleTick_).
void TickLateArm() {
    if (!g_session) return;
    const auto now = std::chrono::steady_clock::now();
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        // Settling: only the props a hand held after their correction went out.
        if (g_settleUntil[slot].time_since_epoch().count() != 0) {
            SettleTick_(slot, now);
            continue;
        }
        if (g_flushArmUntil[slot].time_since_epoch().count() == 0) continue;  // never armed
        if (now >= g_flushArmUntil[slot]) {                                    // window expired -> free dedup
            g_lastFlushedPilePos[slot].clear();
            // A prop a hand held after its correction went out may still owe a follow-up: the slot
            // then stays armed through a settle phase, with its keyed map, state and dedupe, until
            // SettleTick_ retires them. Otherwise the keyed half retires here with the rest.
            // A last word for every prop corrected this join, read now: one moved since its last
            // delivery (a grab and release inside the last scan interval among them) is sent its
            // follow-up on the same lane, behind anything still queued; one in a hand now is
            // watched. Cold: the dedupe holds only the props corrected this join.
            std::vector<coop::element::ElementId> corrected;
            corrected.reserve(g_lastFlushedKeyedPos[slot].size());
            for (const auto& sent : g_lastFlushedKeyedPos[slot]) corrected.push_back(sent.first);
            for (const coop::element::ElementId eid : corrected) {
                const auto kit = g_blobKeyedXforms[slot].find(eid);
                if (kit != g_blobKeyedXforms[slot].end()) FlushKeyedProp_(slot, "expiry", eid, kit->second);
            }
            const size_t held = g_handSinceSent[slot].size();
            if (held == 0) {
                g_flushArmUntil[slot] = {};
                g_lastFlushedKeyedPos[slot].clear();
            }
            // The join window closes here for the slot. The save-time maps are join-window
            // structures, written by the capture alone. The late flush is their last consumer: at
            // its expiry the joiner has quiesced and reconciled, and a late kerfur turn-on resolves
            // by eid.
            if (held > 0) {
                g_settleUntil[slot] = now + kHandSettleWindow;
                UE_LOGI("[PILE-09] slot %d join window CLOSED -- %zu prop(s) a hand held after their "
                        "correction went out stay watched for a follow-up (<= 30 s)", slot, held);
                g_blobPileXforms[slot].clear();
                g_blobClumpXforms[slot].clear();
                g_blobKerfurXforms[slot].clear();
            } else if (!g_blobPileXforms[slot].empty() || !g_blobClumpXforms[slot].empty() ||
                       !g_blobKerfurXforms[slot].empty() || !g_blobKeyedXforms[slot].empty()) {
                UE_LOGI("[PILE-09] slot %d join window CLOSED (b3 late-flush expiry) -- retiring "
                        "save-time maps (%zu pile + %zu clump + %zu kerfur + %zu keyed xform(s)); the capture's "
                        "keys are spent for this joiner",
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
    g_handSinceSent[peerSlot].clear();     // and the settle phase it kept the late flush watching
    g_settleUntil[peerSlot] = {};
    g_settleReannounceSaid[peerSlot] = false;
}

bool HasCapture(int peerSlot) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return false;
    // Filled at the capture; retired at the window's close or, after a settle phase, at its end.
    return !g_blobKeyedXforms[peerSlot].empty();
}

bool IsLateWindowOpen(int peerSlot) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return false;
    // Disarmed at expiry and at ClearForSlot. A settle phase is past the window: only the props a hand
    // held are still watched, and a host move of any other prop no longer reaches the joiner this way.
    return g_flushArmUntil[peerSlot].time_since_epoch().count() != 0 &&
           g_settleUntil[peerSlot].time_since_epoch().count() == 0;
}

}  // namespace coop::join_window_baseline
