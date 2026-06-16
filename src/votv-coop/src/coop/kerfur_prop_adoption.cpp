// coop/kerfur_prop_adoption.cpp -- see coop/kerfur_prop_adoption.h. The PROP-form analogue of
// coop/npc_adoption.cpp (deferred class+pose adoption), specialized for prop_kerfurOmega_C.

#include "coop/kerfur_prop_adoption.h"

#include "coop/element/element.h"      // ElementId + kInvalidId
#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/kerfur_entity.h"        // IsKerfurPropClass + NotifyKerfurPropMirrorBound
#include "coop/net/session.h"
#include "coop/npc_sync.h"             // GetSession (the shared session accessor npc_adoption uses)
#include "coop/players_registry.h"     // Local()
#include "coop/remote_prop.h"          // RegisterPropMirror
#include "coop/remote_prop_spawn.h"    // OnSpawn (fresh-spawn fallback) + RecordClaimIfTracking + HasLoadTailQuiesced
#include "ue_wrap/engine.h"            // GetActorLocation + SetActorSimulatePhysics
#include "ue_wrap/hot_path_guard.h"    // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

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
};

std::vector<Pending> g_pending;
std::chrono::steady_clock::time_point g_lastScan{};

constexpr int kPollIntervalMs = 200;     // 5 Hz scan WHILE pending; zero cost otherwise
constexpr int kAdoptTimeoutMs = 60000;   // last-resort backstop (HasLoadTailQuiesced is the primary gate)
// A save-loaded kerfur prop is static -- its twin is sub-meter from the host pose. A nearest same-class
// candidate farther than this is almost certainly a DIFFERENT kerfur (or the twin is genuinely absent),
// so we keep waiting / fresh-spawn rather than bind the wrong one. Matches ClaimConversionGhosts' radius.
constexpr float kMaxBindDist2 = 500.f * 500.f;

inline coop::element::MirrorManager<coop::element::Prop>& PropMirrors() {
    return coop::element::MirrorManager<coop::element::Prop>::Instance();
}
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
    coop::remote_prop_spawn::RecordClaimIfTracking(obj);  // host accounts for it -> survive the sweep
    ue_wrap::engine::SetActorSimulatePhysics(obj, false); // a host-driven mirror, not a local sim
    coop::kerfur_entity::NotifyKerfurPropMirrorBound(obj, eid);  // held-pose eid stream (K-5)
    return true;
}

// A candidate local kerfur prop actor found in the GUObjectArray scan (one walk feeds all pending).
struct Cand { void* obj; void* cls; float x, y, z; };

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
        cands.push_back(Cand{obj, cls, loc.X, loc.Y, loc.Z});
    }

    void* localPlayer = coop::players::Registry::Get().Local();
    std::unordered_set<void*> claimed;
    for (size_t p = 0; p < g_pending.size();) {
        Pending& e = g_pending[p];
        // Already bound (a racing OnSpawn / a prior scan adopted it)? Resolve.
        if (PropMirrors().Get(static_cast<coop::element::ElementId>(e.payload.elementId)) != nullptr) {
            g_pending[p] = std::move(g_pending.back());
            g_pending.pop_back();
            continue;
        }
        int   bestIdx = -1;
        float bestD2  = kMaxBindDist2;
        for (size_t c = 0; c < cands.size(); ++c) {
            if (cands[c].cls != e.actorClass) continue;       // exact skin class (same save)
            if (claimed.count(cands[c].obj) > 0) continue;    // taken by another pending this scan
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
                        "mirror eid=%u (class+pose match, NO duplicate spawn)",
                        obj, e.classW.c_str(), e.payload.elementId);
                resolved = true;
            }
            // bind failed -> leave pending; retry next scan.
        } else if (coop::remote_prop_spawn::HasLoadTailQuiesced() ||
                   MsSince(e.armedAt) >= kAdoptTimeoutMs) {
            // No local twin AND the save load tail has drained (or the last-resort timeout) -> the
            // kerfur is genuinely absent on this peer (an asymmetric form, or a host kerfur created
            // after the client's world snapshot). Fresh-spawn a mirror via the normal receiver, with
            // deferKerfur=false so it does NOT re-arm here (one-shot fresh spawn, no defer loop).
            const bool quiesced = coop::remote_prop_spawn::HasLoadTailQuiesced();
            UE_LOGW("kerfur-prop-adopt: eid=%u class='%ls' -- no local twin (%s); fresh-spawning a mirror",
                    e.payload.elementId, e.classW.c_str(),
                    quiesced ? "load tail quiesced" : "last-resort timeout");
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
