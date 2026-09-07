// coop/creatures/kerfur_prop_adoption.cpp -- see coop/creatures/kerfur_prop_adoption.h. The
// PROP-form analogue of npc_adoption, specialised for prop_kerfurOmega_C.

#include "coop/creatures/kerfur_prop_adoption.h"

#include "coop/element/element.h"      // ElementId + kInvalidId
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/prop.h"
#include "coop/creatures/kerfur_entity.h"        // IsKerfurPropClass + NotifyKerfurPropMirrorBound
#include "coop/net/session.h"
#include "coop/creatures/npc_sync.h"             // GetSession (the shared session accessor npc_adoption uses)
#include "coop/player/players_registry.h"     // Local()
#include "coop/props/remote_prop.h"          // RegisterPropMirror
#include "coop/props/remote_prop_spawn.h"    // OnSpawn -- the fresh-spawn fallback
#include "coop/props/join_membership_sweep.h"  // HasLoadTailQuiesced -- the claim and sweep half of the join window
#include "ue_wrap/engine/engine.h"            // GetActorLocation + SetActorSimulatePhysics
#include "ue_wrap/actors/prop.h"              // GetKeyString (anti-collision gate: candidate's own Aprop_Key)
#include "ue_wrap/core/hot_path_guard.h"    // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::kerfur_prop_adoption {
namespace {

namespace R = ue_wrap::reflection;

// One pending kerfur prop awaiting its local twin. Game-thread-only state (no mutex), like npc_adoption.
struct Pending {
    coop::net::PropSpawnPayload payload;  // full payload kept for the quiescence fresh-spawn fallback
    std::wstring classW;
    void*        actorClass;              // client-resolved UClass (stable; classes are not GC'd)
    float        x, y, z;                 // host pose at announce (multi-twin disambiguation)
    std::chrono::steady_clock::time_point armedAt;
    // How many scans this entry has survived. The bind and fresh-spawn logs carry it, so a live
    // log shows whether adoption bound on the first poll -- which is what the join barrier is
    // meant to guarantee, the twin being materialised before the PropSpawn arrives.
    int polls = 0;
};

std::vector<Pending> g_pending;
std::chrono::steady_clock::time_point g_lastScan{};

constexpr int kPollIntervalMs = 200;     // 5 Hz scan WHILE pending; zero cost otherwise
constexpr int kAdoptTimeoutMs = 60000;   // last-resort backstop (HasLoadTailQuiesced is the primary gate)
// A save-loaded kerfur prop is static -- its twin is sub-meter from the host pose. A nearest same-class
// candidate farther than this is almost certainly a DIFFERENT kerfur (or the twin is genuinely absent),
// so we keep waiting / fresh-spawn rather than bind the wrong one. Matches ClaimConversionGhosts' radius.
constexpr float kMaxBindDist2 = 500.f * 500.f;

using coop::element::PropMirrors;   // canonical accessor (coop/element/mirror_managers.h)
inline long long MsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t).count();
}
std::wstring KeyToW(const coop::net::WireKey& k) {
    std::wstring s;
    for (uint8_t i = 0; i < k.len && i < sizeof(k.data); ++i)
        s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(k.data[i])));
    return s;
}

// Bind an existing local kerfur prop actor as the host mirror for `eid` -- the class+pose adoption (no
// duplicate spawn). Registers the wire mirror, claims it (sweep-safe), freezes it (host-driven mirror),
// and records it for the K-5 held-pose eid stream. Returns true iff the mirror installed.
bool BindAsMirror(const Pending& e, void* obj) {
    const auto eid = static_cast<coop::element::ElementId>(e.payload.elementId);
    const std::wstring key = KeyToW(e.payload.key);
    coop::remote_prop::RegisterPropMirror(eid, obj, key, e.classW, /*senderSlot=*/0);
    auto* el = PropMirrors().Get(eid);
    if (!el || el->GetActor() != obj) return false;  // install failed / bound a different actor
    coop::join_membership_sweep::RecordClaimIfTracking(obj);  // host accounts for it -> survive the sweep
    ue_wrap::engine::SetActorSimulatePhysics(obj, false); // a host-driven mirror, not a local sim
    coop::kerfur_entity::NotifyKerfurPropMirrorBound(obj, eid);  // held-pose eid stream (K-5)
    return true;
}

// A candidate local kerfur prop actor found in the GUObjectArray scan (one walk feeds all pending).
// `key` = the actor's own Aprop_C.Key (cross-peer-stable for SAVE-loaded kerfurs; empty/None for a
// not-yet-keyed late-mint twin). Drives the anti-collision gate in the match loop.
struct Cand { void* obj; void* cls; float x, y, z; std::wstring key; };

// One throttled scan: collect untracked (non-mirror) live kerfur PROP candidates, then bind each
// pending entry to its NEAREST same-class unclaimed candidate, or fresh-spawn on quiescence/timeout.
void ResolvePending() {
    // Already-mirrored kerfur props -> never re-adopt.
    std::vector<coop::element::Prop*> mirrors;
    PropMirrors().Snapshot(mirrors);
    std::unordered_set<void*> tracked;
    tracked.reserve(mirrors.size() * 2 + 1);
    for (coop::element::Prop* m : mirrors)
        if (m && m->IsMirror() && m->GetActor() &&
            R::IsLiveByIndex(m->GetActor(), m->GetInternalIdx()))
            tracked.insert(m->GetActor());

    // ONE GUObjectArray walk -> untracked kerfur PROP candidates (IsKerfurPropClass is a bounded
    // SuperStruct pointer walk vs the cached prop base; >99% of objects fail it before any alloc).
    std::vector<Cand> cands;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !coop::kerfur_entity::IsKerfurPropClass(cls)) continue;  // prop-form kerfurs only
        if (tracked.count(obj) > 0) continue;                                // already a mirror
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;       // CDO
        const auto loc = ue_wrap::engine::GetActorLocation(obj);
        cands.push_back(Cand{obj, cls, loc.X, loc.Y, loc.Z, ue_wrap::prop::GetKeyString(obj)});
    }

    void* localPlayer = coop::players::Registry::Get().Local();
    std::unordered_set<void*> claimed;
    for (size_t p = 0; p < g_pending.size();) {
        Pending& e = g_pending[p];
        ++e.polls;
        // Already bound (a racing OnSpawn / a prior scan adopted it)? Resolve.
        if (PropMirrors().Get(static_cast<coop::element::ElementId>(e.payload.elementId)) != nullptr) {
            g_pending[p] = std::move(g_pending.back());
            g_pending.pop_back();
            continue;
        }
        // The anti-collision gate in front of the fuzzy match below. The prop form is an ordinary
        // keyed prop, so a save-loaded one restores the SAME key on both peers and binds by exact
        // key; only a keyless or freshly-minted one reaches this class-and-pose fallback. A
        // candidate that carries a real key of its own therefore already belongs to the host kerfur
        // with that key, and a pending kerfur whose broadcast key differs must not take it: without
        // this gate the nearer pending steals the neighbour's actor before its exact bind claims
        // it, and two host eids end up on one actor. So skip any candidate whose key is real
        // (non-empty, not None) and differs from this pending kerfur's. The test is the candidate's
        // identity key, never its current bind state, so it does not depend on whether the exact
        // bind has run yet. A legitimate twin still passes: the same save key, or a key not yet
        // minted. A pending kerfur left with no candidate falls through to the fresh-spawn or the
        // defer below.
        const std::wstring pendKey = KeyToW(e.payload.key);
        int   bestIdx = -1;
        float bestD2  = kMaxBindDist2;
        for (size_t c = 0; c < cands.size(); ++c) {
            if (cands[c].cls != e.actorClass) continue;       // exact skin class (same save)
            if (claimed.count(cands[c].obj) > 0) continue;    // taken by another pending this scan
            if (!cands[c].key.empty() && cands[c].key != L"None" &&
                cands[c].key != pendKey) continue;            // exact-belongs to a DIFFERENT kerfur -> never steal
            const float dx = cands[c].x - e.x, dy = cands[c].y - e.y, dz = cands[c].z - e.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bestD2) { bestIdx = static_cast<int>(c); bestD2 = d2; }
        }
        bool resolved = false;
        if (bestIdx >= 0) {
            void* obj = cands[static_cast<size_t>(bestIdx)].obj;
            if (BindAsMirror(e, obj)) {
                claimed.insert(obj);
                UE_LOGI("kerfur-prop-adopt: bound LOCAL save kerfur prop actor=%p class='%ls' as host "
                        "mirror eid=%u (class+pose match, NO duplicate spawn; poll #%d, %lld ms after arm"
                        " -- barrier expectation: poll #1)",
                        obj, e.classW.c_str(), e.payload.elementId, e.polls, MsSince(e.armedAt));
                resolved = true;
            }
            // bind failed -> leave pending; retry next scan.
        } else if (coop::join_membership_sweep::HasLoadTailQuiesced() ||
                   MsSince(e.armedAt) >= kAdoptTimeoutMs) {
            // No local twin AND the save load tail has drained (or the last-resort timeout) -> the
            // kerfur is genuinely absent on this peer (an asymmetric form, or a host kerfur created
            // after the client's world snapshot). Fresh-spawn a mirror via the normal receiver, with
            // deferKerfur=false so it does NOT re-arm here (one-shot fresh spawn, no defer loop).
            const bool quiesced = coop::join_membership_sweep::HasLoadTailQuiesced();
            if (!quiesced) {
                // Tripwire, never a trim: under the join barrier every join-path PropSpawn arrives
                // after the settle and the sweep latch fires seconds later, so reaching the
                // last-resort timeout BEFORE quiescence means the probe-and-sweep chain wedged.
                // Report it.
                UE_LOGW("kerfur-prop-adopt: TRIPWIRE -- last-resort timeout fired BEFORE load-tail "
                        "quiescence (eid=%u, %d polls) -- structurally unexpected under the join "
                        "barrier; report this log",
                        e.payload.elementId, e.polls);
            }
            UE_LOGW("kerfur-prop-adopt: eid=%u class='%ls' -- no local twin (%s; poll #%d, %lld ms after "
                    "arm); fresh-spawning a mirror",
                    e.payload.elementId, e.classW.c_str(),
                    quiesced ? "load tail quiesced" : "last-resort timeout",
                    e.polls, MsSince(e.armedAt));
            // Both trailing flags are passed explicitly. OnSpawn's fourth parameter is fromConvert
            // and the fifth is deferKerfur, which DEFAULTS to true: a single positional false binds
            // fromConvert and leaves the defer on, so the fresh spawn would re-enter this module,
            // refresh the already- pending eid and spawn nothing, and the entry below would then be
            // popped -- dropping the kerfur for good, since the host announces each prop once.
            // deferKerfur=false makes this the one-shot fallback it has to be.
            coop::remote_prop_spawn::OnSpawn(e.payload, /*senderSlot=*/0, localPlayer,
                                             /*fromConvert=*/false, /*deferKerfur=*/false);
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

void Arm(const coop::net::PropSpawnPayload& payload) {
    UE_ASSERT_GAME_THREAD("kerfur_prop_adoption::Arm");  // no-mutex: g_pending is GT-only
    const auto eid = static_cast<coop::element::ElementId>(payload.elementId);
    if (eid == 0 || eid == coop::element::kInvalidId) return;
    // Already bound as a mirror (a re-announce after a successful adopt)? Nothing to do.
    if (PropMirrors().Get(eid) != nullptr) return;
    // Build the class name from the payload (ASCII wire class name).
    std::wstring cls;
    for (uint8_t i = 0; i < payload.className.len && i < sizeof(payload.className.data); ++i)
        cls.push_back(static_cast<wchar_t>(static_cast<unsigned char>(payload.className.data[i])));
    void* actorClass = R::FindClass(cls.c_str());  // one resolve at arm time; cached in the entry
    const auto now = std::chrono::steady_clock::now();
    // Idempotent on eid: a re-announce refreshes the pose + re-arms the timeout.
    for (Pending& e : g_pending) {
        if (e.payload.elementId == payload.elementId) {
            e.payload = payload; e.classW = cls; e.actorClass = actorClass;
            e.x = payload.locX; e.y = payload.locY; e.z = payload.locZ;
            e.armedAt = now;
            return;
        }
    }
    g_pending.push_back(Pending{payload, cls, actorClass,
                                payload.locX, payload.locY, payload.locZ, now});
    UE_LOGI("kerfur-prop-adopt: armed deferred adoption eid=%u class='%ls' (awaiting local twin)",
            payload.elementId, cls.c_str());
}

void Tick() {
    UE_ASSERT_GAME_THREAD("kerfur_prop_adoption::Tick");  // no-mutex: g_pending is GT-only
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only
    if (!g_pending.empty() && MsSince(g_lastScan) >= kPollIntervalMs) {
        g_lastScan = std::chrono::steady_clock::now();
        ResolvePending();
    }
}

void OnSnapshotComplete() {
    UE_ASSERT_GAME_THREAD("kerfur_prop_adoption::OnSnapshotComplete");
    // Reserved for parity with npc_adoption (no separate ghost sweep here -- the divergence sweep +
    // the per-mirror claim already cover the prop universe). Informational.
}

void OnClientWorldReady() {
    UE_ASSERT_GAME_THREAD("kerfur_prop_adoption::OnClientWorldReady");
    g_pending.clear();  // a fresh connect replay re-arms from the new world's PropSpawns
}

void OnSessionEnd() {
    UE_ASSERT_GAME_THREAD("kerfur_prop_adoption::OnSessionEnd");
    g_pending.clear();
}

}  // namespace coop::kerfur_prop_adoption
