// coop/npc_adoption.cpp -- see coop/npc_adoption.h for the contract + RE ground truth.

#include "coop/creatures/npc_adoption.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_mirror.h"
#include "coop/creatures/npc_sync.h"
#include "coop/props/remote_prop_spawn.h"  // HasLoadTailQuiesced -- shared save-load-tail quiescence signal
#include "coop/dev/kerfur_census.h"  // DIAGNOSTIC: re-arm the kerfur census on world-ready / session-end
#include "ue_wrap/engine.h"
#include "ue_wrap/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD -- the no-mutex contract tripwire
#include "ue_wrap/kerfur.h"
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::npc_adoption {
namespace {

namespace R = ue_wrap::reflection;

// One pending save-persisted NPC awaiting its local twin. Game-thread-only state (no mutex).
struct Pending {
    uint32_t     eid;
    std::wstring classW;
    void*        actorClass;    // client-resolved UClass (stable -- classes are not GC'd)
    float        locX, locY, locZ;     // host pose at announce (multi-twin disambiguation)
    float        rotPitch, rotYaw, rotRoll;
    std::chrono::steady_clock::time_point armedAt;
};

std::vector<Pending> g_pending;
std::chrono::steady_clock::time_point g_lastScan{};
bool g_snapshotDelivered = false;
bool g_ghostSwept        = false;

constexpr int kPollIntervalMs = 200;    // 5 Hz scan WHILE pending; zero cost otherwise
// LAST-RESORT backstop only. The primary gate is HasLoadTailQuiesced() (the prop+NPC
// load-tail quiescence signal, itself deadline-capped at kSweepDeadlineMs in
// remote_prop_spawn) -- it fires first on every real join. The old 8 s here fired
// BEFORE the async 19 MB live-save load materialized the kerfur twins, so the adoption
// fresh-spawned a mirror that then duplicated the late-loading twin (the 2026-06-15
// kerfur-dupe). 60 s > the sweep deadline -> quiescence always wins; this only guards
// the pathological case where quiescence never signals at all.
constexpr int kAdoptTimeoutMs = 60000;

inline coop::element::MirrorManager<coop::element::Npc>& NpcMirrors() {
    return coop::element::MirrorManager<coop::element::Npc>::Instance();
}

inline long long MsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t).count();
}

// Bind an existing local actor as the host mirror for `eid` -- the camera-safe adoption: the actor
// is the client's own FULLY-INITIALIZED save-kerfur, so a later K2_DestroyActor cascades its cam
// child exactly as the host's does (the v74 floating-camera fix, now structural). Parks it
// host-driven. Returns true on success (false leaves the entry pending for the next scan).
bool BindAsMirror(uint32_t eid, void* obj, const std::wstring& classW) {
    auto mirror = std::make_unique<coop::element::Npc>();
    std::string typeName8;
    typeName8.reserve(classW.size());
    for (wchar_t c : classW) typeName8.push_back(static_cast<char>(c));
    mirror->SetTypeName(std::move(typeName8));
    mirror->SetActor(obj, R::InternalIndexOf(obj));
    if (!NpcMirrors().Install(static_cast<coop::element::ElementId>(eid), std::move(mirror))) {
        UE_LOGW("npc-adopt: MirrorManager::Install(eid=%u) failed for local actor %p -- retry/fallback",
                eid, obj);
        return false;
    }
    // THOROUGH PARK: stop actor + CMC ticks AND neutralize the kerfur's looping AI timers
    // (timer_face/timer_kerf/checkDoor survive a tick-disable). Now host-driven, zero local AI.
    ue_wrap::puppet::DisableCharacterTicks(obj);
    ue_wrap::kerfur::NeutralizeAiTimers(obj);
    return true;
}

// A candidate local actor found in the GUObjectArray scan (one walk feeds all pending entries).
struct Cand { void* obj; void* cls; float x, y, z; };

// One throttled scan: collect untracked allowlisted NPC candidates, then bind each pending entry
// to its NEAREST same-class unclaimed candidate (multi-kerfur disambiguation), or fresh-spawn on
// timeout. Erases resolved entries from g_pending.
void ResolvePending() {
    // Tracked-actor set (already-bound mirrors -> never re-adopt).
    std::vector<coop::element::Npc*> mirrors;
    NpcMirrors().Snapshot(mirrors);
    std::unordered_set<void*> tracked;
    tracked.reserve(mirrors.size() * 2 + 1);
    for (coop::element::Npc* m : mirrors)
        // Liveness-guard (serial check): a mirror whose actor was freed (world-swap with no
        // EntityDestroy) holds a dangling pointer that could falsely match a recycled address.
        if (m && m->GetActor() && R::IsLiveByIndex(m->GetActor(), m->GetInternalIdx()))
            tracked.insert(m->GetActor());

    // ONE GUObjectArray walk -> candidates (the allowlist prefilter eliminates >99% before any
    // alloc; only a handful of real NPCs reach NameOf/GetActorLocation).
    std::vector<Cand> cands;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !coop::npc_sync::IsAllowlistedClass(cls)) continue;  // fast lineage prefilter
        if (tracked.count(obj) > 0) continue;                            // already a mirror
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;    // CDO (alloc-free prefix test)
        const auto loc = ue_wrap::engine::GetActorLocation(obj);
        cands.push_back(Cand{obj, cls, loc.X, loc.Y, loc.Z});
    }

    // Match each pending entry to its nearest unclaimed same-class candidate; bind or time out.
    std::unordered_set<void*> claimed;
    for (size_t p = 0; p < g_pending.size();) {
        Pending& e = g_pending[p];
        int   bestIdx = -1;
        float bestD2  = 0.0f;
        for (size_t c = 0; c < cands.size(); ++c) {
            if (cands[c].cls != e.actorClass) continue;             // exact skin class (same save)
            if (claimed.count(cands[c].obj) > 0) continue;          // taken by another entry
            const float dx = cands[c].x - e.locX;
            const float dy = cands[c].y - e.locY;
            const float dz = cands[c].z - e.locZ;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (bestIdx < 0 || d2 < bestD2) { bestIdx = static_cast<int>(c); bestD2 = d2; }
        }
        bool resolved = false;
        if (bestIdx >= 0) {
            void* obj = cands[static_cast<size_t>(bestIdx)].obj;
            if (BindAsMirror(e.eid, obj, e.classW)) {
                claimed.insert(obj);
                UE_LOGI("npc-adopt: bound LOCAL save NPC actor=%p class='%ls' as host mirror eid=%u "
                        "(class-match, NO duplicate spawn)", obj, e.classW.c_str(), e.eid);
                resolved = true;
            }
            // bind failed -> leave pending; retry next scan (do not consume the candidate).
        } else if (coop::remote_prop_spawn::HasLoadTailQuiesced() ||
                   MsSince(e.armedAt) >= kAdoptTimeoutMs) {
            // No local twin -- and the save load tail has DRAINED (the divergence sweep fired,
            // HasLoadTailQuiesced), or the last-resort timeout elapsed. CRITICAL: quiescence now
            // waits for the async ALLOWLISTED-NPC population to settle, not just the keyed props
            // (remote_prop_spawn::CountLoadTailUnsettled_). The kerfur twins respawn SECONDS after
            // the props' keys mint (2026-06-15 hands-on: prop-only quiescence fired while the kerfur
            // NPCs were still loading -> this branch fresh-spawned a mirror that then duplicated the
            // late-arriving twin). With the NPC-aware gate, every blob twin is present by quiescence
            // and is bound by the scan above (bestIdx>=0) -- so this branch only ever fires for a
            // twin that is GENUINELY absent (a host kerfur turned on AFTER the live-capture instant;
            // the blob has no record of it), where a fresh mirror is correct, not a duplicate.
            const bool quiesced = coop::remote_prop_spawn::HasLoadTailQuiesced();
            UE_LOGW("npc-adopt: eid=%u class='%ls' -- no local twin (%s); fresh-spawning a mirror",
                    e.eid, e.classW.c_str(),
                    quiesced ? "load tail quiesced (props+NPCs settled)" : "last-resort timeout");
            coop::npc_mirror::SpawnFreshNpcMirror(e.classW, e.actorClass, e.eid,
                                                  e.locX, e.locY, e.locZ,
                                                  e.rotPitch, e.rotYaw, e.rotRoll);
            resolved = true;
        }
        if (resolved) {
            g_pending[p] = std::move(g_pending.back());  // swap-pop (order irrelevant)
            g_pending.pop_back();
        } else {
            ++p;
        }
    }
}

}  // namespace

void ArmAdoption(uint32_t eid, const std::wstring& classW, void* actorClass,
                 float locX, float locY, float locZ,
                 float rotPitch, float rotYaw, float rotRoll) {
    UE_ASSERT_GAME_THREAD("npc_adoption::ArmAdoption");  // no-mutex: g_pending is GT-only
    // Already bound as a mirror (a re-announce after a successful adopt)? Nothing to do.
    if (NpcMirrors().Get(static_cast<coop::element::ElementId>(eid)) != nullptr) return;
    const auto now = std::chrono::steady_clock::now();
    // Idempotent on eid: a connect re-announce refreshes the pose + re-arms the timeout.
    for (Pending& e : g_pending) {
        if (e.eid == eid) {
            e.classW = classW; e.actorClass = actorClass;
            e.locX = locX; e.locY = locY; e.locZ = locZ;
            e.rotPitch = rotPitch; e.rotYaw = rotYaw; e.rotRoll = rotRoll;
            e.armedAt = now;
            return;
        }
    }
    g_pending.push_back(Pending{eid, classW, actorClass, locX, locY, locZ,
                                rotPitch, rotYaw, rotRoll, now});
    UE_LOGI("npc-adopt: armed deferred adoption eid=%u class='%ls' (awaiting local twin)",
            eid, classW.c_str());
}

void Tick() {
    UE_ASSERT_GAME_THREAD("npc_adoption::Tick");  // no-mutex: g_pending + latches are GT-only
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only

    // (1) Resolve pending adoptions (throttled; only while entries exist).
    if (!g_pending.empty() && MsSince(g_lastScan) >= kPollIntervalMs) {
        g_lastScan = std::chrono::steady_clock::now();
        ResolvePending();
    }

    // (2) ONE-SHOT ghost sweep, only after the connect snapshot is fully delivered AND every armed
    // adoption has converged AND the save load tail has QUIESCED -- so a local twin is NEVER swept
    // before its adoption could bind, AND a LATE-spawning untracked twin is present before the sweep
    // (which latches off forever) runs.
    // WARNING: this sweep MUST stay here in Tick, NEVER inline in OnSnapshotComplete. At the instant
    // OnSnapshotComplete sets g_snapshotDelivered, g_pending is EMPTY (the EntitySpawn->ArmAdoption
    // tasks are game_thread::Post'd and still queued). FIFO game-thread ordering guarantees those
    // ArmAdoption tasks drain before THIS (a later) Tick observes the flag, so g_pending is filled
    // (then drained by adoption) before the sweep fires. Sweeping inline at SnapshotComplete would
    // run with g_pending empty and K2-destroy the client's own not-yet-adopted local save-kerfur --
    // re-introducing the exact double/missing-NPC bug v75 fixes.
    //
    // HasLoadTailQuiesced GATE (2026-06-24, reverse-kerfur follow-ghost RCA): g_pending.empty() alone
    // is NOT enough. The async live-save load spawns a DUPLICATE active kerfur (the real save-kerfur is
    // adopted; a second untracked kerfurOmega_C twin materializes SECONDS later in the load tail). If
    // the one-shot sweep fires the instant g_pending drains -- which can be well BEFORE the tail
    // settles (hands-on 12:30:17 sweep "0 orphans", load tail not quiesced until 12:30:23) -- it finds
    // 0, latches g_ghostSwept, and the late twin survives as a follow-ghost active kerfur the host's
    // turn_off later strands. Gate on the SAME load-tail quiescence the ResolvePending fresh-spawn
    // (above) + the prop divergence sweep already use, so the sweep waits until every late twin is
    // present (deadline-capped in remote_prop_spawn, so it can't hang). Re-armed per OnClientWorldReady.
    if (g_snapshotDelivered && g_pending.empty() && !g_ghostSwept &&
        coop::npc_sync::IsInstalled() &&
        coop::remote_prop_spawn::HasLoadTailQuiesced()) {
        g_ghostSwept = true;
        const int swept = coop::npc_mirror::DestroyUntrackedClientNpcs();
        UE_LOGI("npc-adopt: post-snapshot ghost sweep complete (%d untracked orphan(s) destroyed; "
                "adoption converged + load tail quiesced)", swept);
    }
}

void OnSnapshotComplete() {
    UE_ASSERT_GAME_THREAD("npc_adoption::OnSnapshotComplete");
    // Marks the connect snapshot fully delivered. The one-shot ghost sweep (Tick) then
    // fires only after BOTH this flag and g_pending being empty -- i.e. every armed
    // adoption converged. Runs for every join incl. live-capture: ResolvePending's
    // fresh-spawn is gated on HasLoadTailQuiesced (which now waits for the async NPC
    // load too), so a still-loading local twin is adopted, never duplicated, and the
    // ghost sweep only sees genuine orphans the host's world does not contain.
    g_snapshotDelivered = true;
}

void OnClientWorldReady() {
    UE_ASSERT_GAME_THREAD("npc_adoption::OnClientWorldReady");
    // A fresh connect replay is about to deliver this world's EntitySpawns. First DROP stale
    // dead-actor mirrors from the prior world (a save-transfer join does TWO level loads; a mirror
    // bound in world-1 survives the swap as a dangling Element and would block world-2 from re-
    // mirroring/re-adopting the SAME host eid -- see npc_mirror::PruneDeadClientMirrors). Then drop
    // any stale pending from a prior world and re-arm the latches so the new world re-adopts +
    // re-sweeps.
    coop::npc_mirror::PruneDeadClientMirrors();
    g_pending.clear();
    g_snapshotDelivered = false;
    g_ghostSwept = false;
    coop::kerfur_census::Reset();  // DIAGNOSTIC: re-census the new world
}

void OnSessionEnd() {
    UE_ASSERT_GAME_THREAD("npc_adoption::OnSessionEnd");
    g_pending.clear();
    g_snapshotDelivered = false;
    g_ghostSwept = false;
    coop::kerfur_census::Reset();  // DIAGNOSTIC
}

}  // namespace coop::npc_adoption
