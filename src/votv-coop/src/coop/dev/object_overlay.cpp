// coop/dev/object_overlay.cpp -- see coop/dev/object_overlay.h.

#include "coop/dev/object_overlay.h"

#include "coop/dev/dev_gate.h"
#include "coop/element/registry.h"
#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::dev::object_overlay {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace UP = ue_wrap::prop;

namespace {

// Controls. The F1 menu writes these from the render thread; Update() reads them
// on the game thread -- atomics, no lock. A layer/radius change sets g_dirty so
// the candidate cache rebuilds on the next Update instead of waiting out the
// refresh interval (the user sees the checkbox take effect immediately).
std::atomic<bool>  g_enabled{false};
std::atomic<bool>  g_layerNames{true};
std::atomic<bool>  g_layerNet{true};
std::atomic<bool>  g_layerPhys{false};
std::atomic<float> g_radiusM{25.f};
std::atomic<bool>  g_dirty{false};

// Candidate cache -- GAME THREAD ONLY (built by Refresh_, read by Project_).
// Larger than kMaxLabels: the player moves between refreshes, so the per-tick
// projection re-gates by CURRENT distance against a deeper distance-sorted pool.
constexpr int kMaxEntries = 96;
// Refresh cadence: the rebuild walks GUObjectArray + reads every candidate's
// location (the ue_wrap/prop.h FindNearest cost profile -- "one-shot fine, never
// per-frame"). Every ~2 s behind an explicit dev toggle keeps it cold; the
// per-tick projection keeps label POSITIONS live in between.
constexpr int kRefreshEveryNTicks = 120;

struct Entry {
    void*   actor = nullptr;
    int32_t idx   = -1;     // GUObjectArray InternalIndex -- IsLiveByIndex revalidation
                            // per tick (a GC purge frees actors without notice; never
                            // deref before validating, registry.h:160 contract)
    uint8_t kind  = 2;
    char    line1[56] = {};
    char    line2[56] = {};
    char    line3[40] = {};
};

std::vector<Entry> g_entries;
int  g_tick = 0;
int  g_sinceRefresh = kRefreshEveryNTicks;  // first enabled tick refreshes immediately
int  g_trackedInRange = 0;
int  g_untrackedInRange = 0;
// Registry totals for the status line, sampled at refresh cadence (audit WARN-4:
// reading HostCount/LocalCount in Project_ took the registry mutex 60x/s for a
// number that only needs to be ~2 s fresh).
size_t g_regHost = 0;
size_t g_regLocal = 0;
bool g_publishedAnything = false;  // game-thread-local: skip the publish-empty
                                   // mutex while disabled and already clear

// Published snapshot (the nameplate pattern: game thread writes, render thread
// copies). g_hasAny mirrors "enabled" for the lock-free HUD gate.
std::mutex g_mu;
Snapshot   g_snap;

void Publish_(const Snapshot& s) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_snap = s;
    }
    g_publishedAnything = (s.count != 0 || s.status[0] != '\0');
}

void PublishEmpty_(const char* status) {
    Snapshot s;
    if (status) std::snprintf(s.status, sizeof(s.status), "%s", status);
    Publish_(s);
}

// Narrow a reflection wstring into a fixed ASCII buffer (label text; FName
// strings are ASCII in practice, anything else becomes '?').
void AsciiInto_(char* dst, size_t cap, const std::wstring& w) {
    size_t i = 0;
    for (; i + 1 < cap && i < w.size(); ++i) {
        const wchar_t c = w[i];
        dst[i] = (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    }
    dst[i] = '\0';
}

float DistCm_(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Identity text for one candidate, baked once per refresh (stable between
// refreshes; per-tick work is position + projection only).
void BuildLines_(Entry& e, void* obj, bool tracked, uint32_t eid, bool mirror,
                 bool wantNames, bool wantNet, bool wantPhys) {
    const bool isProp = UP::IsDescendantOfProp(obj);

    if (wantNames) {
        char cls[40] = {};
        AsciiInto_(cls, sizeof(cls), R::ClassNameOf(obj));
        char row[16] = {};
        if (isProp) AsciiInto_(row, sizeof(row), UP::GetPropNameString(obj));
        if (row[0]) std::snprintf(e.line1, sizeof(e.line1), "%s [%s]", cls, row);
        else        std::snprintf(e.line1, sizeof(e.line1), "%s", cls);
    }

    if (wantNet) {
        // Key suffix: the save key is a long GUID-ish string; the last 8 chars
        // are enough to match a log line. Untracked-but-KEYED is itself a
        // finding (a keyed actor the tracker never seeded), so show the key
        // for untracked entries too.
        const std::wstring key = UP::GetInteractableKeyString(obj);
        char keySuf[12] = {};
        if (!key.empty() && key != L"None") {
            const std::wstring tail =
                key.size() > 8 ? key.substr(key.size() - 8) : key;
            AsciiInto_(keySuf, sizeof(keySuf), tail);
        }
        if (tracked) {
            std::snprintf(e.line2, sizeof(e.line2), "eid 0x%X %s%s%s",
                          eid, mirror ? "MIRROR" : "LOCAL",
                          keySuf[0] ? " key .." : "", keySuf);
        } else {
            std::snprintf(e.line2, sizeof(e.line2), "UNTRACKED%s%s",
                          keySuf[0] ? " key .." : " (no eid)", keySuf);
        }
    }

    if (wantPhys && isProp) {
        // Live body state vs the SAVE flags -- `sleep` is what the save says,
        // IsActorRootBodyAtRest is what physics is DOING (the falling-walls
        // discriminator, see the v55 body-awake stamp).
        const bool sim = !E::IsActorRootBodyAtRest(obj);
        std::snprintf(e.line3, sizeof(e.line3), "%s%s%s%s",
                      sim ? "sim" : "rest",
                      UP::IsStatic(obj)   ? " static" : "",
                      UP::IsFrozen(obj)   ? " frozen" : "",
                      UP::IsSleeping(obj) ? " sleep"  : "");
    }
}

// Rebuild the candidate cache: tracked element-registry entries (Prop + Npc)
// plus a GUObjectArray walk for UNTRACKED prop-lineage / keyed-interactable /
// chipPile actors -- the local-only orphans are exactly the problem objects this
// overlay exists to expose. Game thread.
void Refresh_(const ue_wrap::FVector& eye) {
    const float radiusCm  = g_radiusM.load(std::memory_order_relaxed) * 100.f;
    const bool  wantNames = g_layerNames.load(std::memory_order_relaxed);
    const bool  wantNet   = g_layerNet.load(std::memory_order_relaxed);
    const bool  wantPhys  = g_layerPhys.load(std::memory_order_relaxed);

    struct Cand {
        void*    actor;
        int32_t  idx;
        float    dist;
        uint32_t eid;
        bool     mirror;
        bool     tracked;
    };
    std::vector<Cand> cands;
    cands.reserve(256);
    std::unordered_set<void*> seen;
    seen.reserve(512);

    // 1) Tracked elements. SnapshotActorsByType copies under the registry mutex;
    //    the actor pointers may already be GC-purged -> IsLiveByIndex BEFORE any
    //    deref (registry.h contract; the 2026-05-30 connect-edge AV lesson).
    auto& reg = element::Registry::Get();
    std::vector<element::Registry::ActorIdPair> pairs;
    for (const auto type : {element::ElementType::Prop, element::ElementType::Npc}) {
        reg.SnapshotActorsByType(type, pairs);
        for (const auto& p : pairs) {
            if (!p.actor || !R::IsLiveByIndex(p.actor, p.internalIdx)) continue;
            if (!seen.insert(p.actor).second) continue;
            const float d = DistCm_(E::GetActorLocation(p.actor), eye);
            if (d > radiusCm) continue;
            cands.push_back({p.actor, p.internalIdx, d, p.id, p.mirror, true});
        }
    }
    g_trackedInRange = static_cast<int>(cands.size());

    // 2) Untracked world objects of the prop family. Same walk idiom AND same
    //    universe predicate as prop_element_tracker::SeedWalk_ (CDO skip by
    //    name, IsLive gate): IsKeyedInteractable = Aprop + trashBitsPile +
    //    garbageClump + actorChipPile lineages. Deliberately NOT doors/
    //    appliances/etc -- those sync via interactable channels, not the
    //    element registry, so an "UNTRACKED" label on them would be a false
    //    alarm.
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        // Check order (cheap lineage pointer-compares first, IsLive last) is the
        // SeedWalk_ idiom, deliberately: IsLive's own first act is a deref of
        // obj's memory (reflection.h:58), so hoisting it gains no safety and
        // would run the SEH-guarded check on every one of ~250k slots instead
        // of the ~3k lineage matches. An ObjectAt pointer is mapped for the
        // duration of this game-thread task (GC purges run between tasks).
        if (!UP::IsKeyedInteractable(obj)) continue;
        if (seen.count(obj)) continue;
        if (R::ToString(R::NameOf(obj)).rfind(L"Default__", 0) == 0) continue;
        if (!R::IsLive(obj)) continue;
        const float d = DistCm_(E::GetActorLocation(obj), eye);
        if (d > radiusCm) continue;
        cands.push_back({obj, R::InternalIndexOf(obj), d, 0, false, false});
    }
    g_untrackedInRange = static_cast<int>(cands.size()) - g_trackedInRange;
    g_regHost  = reg.HostCount();
    g_regLocal = reg.LocalCount();

    // Nearest first; keep the closest kMaxEntries (the per-tick projection caps
    // again at kMaxLabels by CURRENT distance).
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    if (cands.size() > static_cast<size_t>(kMaxEntries))
        cands.resize(static_cast<size_t>(kMaxEntries));

    g_entries.clear();
    g_entries.reserve(cands.size());
    for (const auto& c : cands) {
        Entry e;
        e.actor = c.actor;
        e.idx   = c.idx;
        e.kind  = c.tracked ? (c.mirror ? 0 : 1) : 2;
        BuildLines_(e, c.actor, c.tracked, c.eid, c.mirror,
                    wantNames, wantNet, wantPhys);
        g_entries.push_back(e);
    }
}

// Re-project the cached candidates at their CURRENT location (a falling wall
// moves between refreshes -- the label must track it) and publish. Game thread.
void Project_(void* pc, const ue_wrap::FVector& eye) {
    const float radiusCm = g_radiusM.load(std::memory_order_relaxed) * 100.f;
    const float fadeFromCm = radiusCm * 0.85f;  // opaque inside, fading at the rim

    Snapshot snap;
    for (const Entry& e : g_entries) {
        if (snap.count >= kMaxLabels) break;
        if (!R::IsLiveByIndex(e.actor, e.idx)) continue;  // GC'd since refresh
        const ue_wrap::FVector loc = E::GetActorLocation(e.actor);
        const float d = DistCm_(loc, eye);
        if (d > radiusCm) continue;
        ue_wrap::FVector2D scr{};
        if (!E::ProjectWorldToScreen(pc, loc, scr, false)) continue;  // behind camera

        Label& L = snap.labels[snap.count];
        L.x = scr.X;
        L.y = scr.Y;
        L.dist = d;
        L.alpha = (d <= fadeFromCm)
                      ? 1.f
                      : std::max(0.f, 1.f - (d - fadeFromCm) / (radiusCm - fadeFromCm));
        L.kind = e.kind;
        std::memcpy(L.line1, e.line1, sizeof(L.line1));
        std::memcpy(L.line2, e.line2, sizeof(L.line2));
        std::memcpy(L.line3, e.line3, sizeof(L.line3));
        ++snap.count;
    }

    std::snprintf(snap.status, sizeof(snap.status),
                  "obj overlay: %d shown | range: %d trk %d untrk | reg %zu+%zu | r %.0fm",
                  snap.count, g_trackedInRange, g_untrackedInRange,
                  g_regHost, g_regLocal,
                  g_radiusM.load(std::memory_order_relaxed));
    Publish_(snap);
}

}  // namespace

void InitFromIni() {
    if (::coop::config::MasterEnabled() &&
        ::coop::config::IsIniKeyTrue("object_overlay")) {
        g_enabled.store(true, std::memory_order_release);
        UE_LOGI("object_overlay: force-enabled via ini (names=%d net=%d phys=%d r=%.0fm)",
                g_layerNames.load() ? 1 : 0, g_layerNet.load() ? 1 : 0,
                g_layerPhys.load() ? 1 : 0, g_radiusM.load());
    }
}

void Update() {
    // Dev info overlay (net identity / phys state labels = ESP on a joined
    // client). Same self-clear as toggle-off while the gate denies.
    if (!g_enabled.load(std::memory_order_acquire) || !coop::dev_gate::Allowed()) {
        // Self-clear once so a stale snapshot doesn't linger after toggle-off;
        // after that the disabled cost is this one atomic load per tick.
        if (g_publishedAnything) PublishEmpty_(nullptr);
        if (!g_entries.empty()) g_entries.clear();
        g_sinceRefresh = kRefreshEveryNTicks;
        return;
    }

    void* lp = coop::players::Registry::Get().Local();
    if (!lp) {
        PublishEmpty_("obj overlay: waiting for player/world");
        g_entries.clear();
        g_sinceRefresh = kRefreshEveryNTicks;
        return;
    }
    void* pc = E::GetController(lp);
    if (!pc) {
        PublishEmpty_("obj overlay: no controller");
        return;
    }

    // Viewer = camera eye (falls back to the actor when the camera manager isn't
    // resolvable -- e.g. the first frames of a level).
    ue_wrap::FVector eye = E::GetCameraLocation();
    if (eye.X == 0.f && eye.Y == 0.f && eye.Z == 0.f) eye = E::GetActorLocation(lp);

    ++g_tick;
    if (++g_sinceRefresh >= kRefreshEveryNTicks ||
        g_dirty.exchange(false, std::memory_order_acq_rel)) {
        g_sinceRefresh = 0;
        Refresh_(eye);
    }

    // Projection at ~30 Hz (every other 60 Hz pump tick): halves the per-tick
    // GetActorLocation + ProjectWorldToScreen UFunction load for up to
    // kMaxEntries candidates; visually indistinguishable for debug labels.
    if (g_tick & 1) return;
    Project_(pc, eye);
}

void GetSnapshot(Snapshot& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_snap;
}

bool IsEnabled() {
    // Role-aware: reports OFF while connected as a client (coop::dev_gate) so
    // every draw site + the menu checkbox die together; the latent flag
    // survives and the overlay returns on disconnect / when hosting.
    return g_enabled.load(std::memory_order_acquire) && coop::dev_gate::Allowed();
}

void SetEnabled(bool on) {
    g_enabled.store(on, std::memory_order_release);
    g_dirty.store(true, std::memory_order_release);
    UE_LOGI("object_overlay: %s", on ? "ON" : "OFF");
}

bool LayerNames() { return g_layerNames.load(std::memory_order_acquire); }
void SetLayerNames(bool on) {
    g_layerNames.store(on, std::memory_order_release);
    g_dirty.store(true, std::memory_order_release);
}

bool LayerNet() { return g_layerNet.load(std::memory_order_acquire); }
void SetLayerNet(bool on) {
    g_layerNet.store(on, std::memory_order_release);
    g_dirty.store(true, std::memory_order_release);
}

bool LayerPhys() { return g_layerPhys.load(std::memory_order_acquire); }
void SetLayerPhys(bool on) {
    g_layerPhys.store(on, std::memory_order_release);
    g_dirty.store(true, std::memory_order_release);
}

float RadiusM() { return g_radiusM.load(std::memory_order_acquire); }
void SetRadiusM(float meters) {
    g_radiusM.store(std::clamp(meters, 5.f, 100.f), std::memory_order_release);
    g_dirty.store(true, std::memory_order_release);
}

}  // namespace coop::dev::object_overlay
