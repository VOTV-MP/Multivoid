// coop/dev/spawn_match_probe.cpp -- see header.

#include "coop/dev/spawn_match_probe.h"

#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/props/remote_prop.h"   // ResolveMirrorEidByActor
#include "ue_wrap/core/log.h"

#include <chrono>
#include <unordered_map>

namespace coop::dev::spawn_match_probe {
namespace {

// All state client game-thread-only (the spawn and destroy receivers both are). No mutex.
constexpr size_t kWatchCap     = 512;   // backstop on the adopted-actor watch list
constexpr int    kMaxCandLines = 8;     // candidates printed per scan; the count carries the rest
constexpr auto   kFreshBind    = std::chrono::seconds(5);  // a destroy inside this is "behind the bind"
// The dedupe exists because some classes mint a fresh Key per process, so the same save-loaded
// prop lands at the same spot under two different keys. A rekey AT that spot is the mechanism
// working; a rekey metres away is a different prop being taken. Same threshold the exact-key
// path calls aligned.
constexpr float  kAlignedCm    = 2.0f;

struct Adoption {
    uint32_t     eid = 0;
    std::wstring key;
    std::chrono::steady_clock::time_point at{};
};
std::unordered_map<void*, Adoption> g_watch;

int g_scans          = 0;
int g_scansNoMatch   = 0;   // the scan accepted nothing
int g_scansMulti     = 0;   // more than one candidate was inside the radius
int g_scansExcluded  = 0;   // a scan where the radius held a hand-axis actor, refused by the caller's set
int g_notNearest     = 0;   // ... and the one taken was not the closest of them
int g_keyDivergedNear = 0;  // adopted over its own key AT the wire spot -- the dedupe working
int g_keyDivergedFar  = 0;  // ... metres away, which is a different prop being taken
int g_bound          = 0;
int g_destroyedFresh = 0;   // an adoption destroyed inside kFreshBind
int g_destroyedLater = 0;

// The rig kills its peers rather than disconnecting them, so a teardown-only summary prints
// nothing in the runs this exists for. Tick() prints on a period instead; the first print lands
// as soon as anything is recorded.
constexpr auto kVerdictPeriod = std::chrono::seconds(15);
std::chrono::steady_clock::time_point g_lastVerdict{};
bool g_dirty = false;  // something recorded since the last print

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::spawn_match_probe);
    return s;
}

void NoteFuzzyScan(uint32_t wireEid, const std::wstring& wireKey, const std::wstring& cls,
                   const std::wstring& propName, const ue_wrap::FVector& anchor,
                   const ue_wrap::prop::NearbyTrace& trace) {
    if (!IsEnabled()) return;
    ++g_scans;
    const size_t n = trace.candidates.size();
    if (n == 0) ++g_scansNoMatch;
    if (n > 1) ++g_scansMulti;

    if (trace.excludedRejects > 0) ++g_scansExcluded;
    UE_LOGW("spawn_match_probe: SCAN eid=%u key='%ls' cls='%ls' row='%ls' at=(%.1f,%.1f,%.1f) -- "
            "%zu candidate(s) in radius, %d class match(es) scanned, %d rejected on row name, "
            "%d refused as hand-axis, nearest outside=%.1fcm",
            wireEid, wireKey.c_str(), cls.c_str(), propName.c_str(), anchor.X, anchor.Y, anchor.Z,
            n, trace.classMatches, trace.rowNameRejects, trace.excludedRejects,
            trace.nearestOutsideCm);

    // The candidate table. The scan takes candidates[0]; anything nearer below it is a choice the
    // shipped log cannot show, and a local key differing from the wire key means the adoption is
    // rekeying a prop that already had a stable identity of its own.
    float bestDist  = -1.f;
    size_t bestAt   = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto& c = trace.candidates[i];
        if (bestDist < 0.f || c.distCm < bestDist) { bestDist = c.distCm; bestAt = i; }
        if (i >= static_cast<size_t>(kMaxCandLines)) continue;
        const std::wstring localKey = ue_wrap::prop::GetKeyString(c.actor);
        // Each resolve takes the mirror table's lock and builds a pointer vector over every row,
        // and this runs inside OnSpawn -- the very path whose ORDERING against an inbound destroy
        // is the thing being measured. So the wire-mirror
        // row, which is what the identity-steal gate itself reads, is resolved for every printed
        // candidate; the any-row column, which only separates a save-loaded twin from an untracked
        // actor, is resolved for the taken one alone.
        const auto mirrorEid = coop::remote_prop::ResolveMirrorEidByActor(c.actor, /*wireMirrorOnly=*/true);
        if (i == 0) {
            const auto anyEid = coop::remote_prop::ResolveMirrorEidByActor(c.actor, /*wireMirrorOnly=*/false);
            UE_LOGW("spawn_match_probe:   cand[0] <== TAKEN actor=%p slot=%d d=%.1fcm key='%ls' "
                    "key-matches-wire=%d wire-mirror-eid=%u any-eid=%u",
                    c.actor, c.objIndex, c.distCm, localKey.c_str(),
                    (localKey == wireKey) ? 1 : 0,
                    static_cast<unsigned>(mirrorEid), static_cast<unsigned>(anyEid));
            continue;
        }
        UE_LOGW("spawn_match_probe:   cand[%zu] actor=%p slot=%d d=%.1fcm key='%ls' key-matches-wire=%d "
                "wire-mirror-eid=%u",
                i, c.actor, c.objIndex, c.distCm, localKey.c_str(),
                (localKey == wireKey) ? 1 : 0, static_cast<unsigned>(mirrorEid));
    }
    if (n > static_cast<size_t>(kMaxCandLines)) {
        UE_LOGW("spawn_match_probe:   ... %zu further candidate(s) not printed", n - kMaxCandLines);
    }
    if (n > 1 && bestAt != 0) {
        ++g_notNearest;
        UE_LOGW("spawn_match_probe: NOT-NEAREST eid=%u -- took cand[0] at %.1fcm while cand[%zu] sat at "
                "%.1fcm; the scan takes object-array order, not distance",
                wireEid, trace.candidates[0].distCm, bestAt, bestDist);
    }
    g_dirty = true;
}

void NoteFuzzyBound(void* actor, uint32_t wireEid, const std::wstring& wireKey, float takenDistCm) {
    if (!IsEnabled() || !actor) return;
    ++g_bound;
    const std::wstring localKey = ue_wrap::prop::GetKeyString(actor);
    if (!localKey.empty() && localKey != L"None" && localKey != wireKey) {
        if (takenDistCm >= 0.f && takenDistCm <= kAlignedCm) {
            ++g_keyDivergedNear;   // the same prop under two per-process keys; no line, it floods a join
        } else {
            ++g_keyDivergedFar;
            UE_LOGW("spawn_match_probe: BOUND-OVER-KEY-AT-RANGE eid=%u actor=%p d=%.1fcm carried its own key "
                    "'%ls' and is being rekeyed to '%ls' -- too far to be the same prop under two keys",
                    wireEid, actor, takenDistCm, localKey.c_str(), wireKey.c_str());
        }
    }
    if (g_watch.size() < kWatchCap)
        g_watch[actor] = Adoption{wireEid, wireKey, std::chrono::steady_clock::now()};
    g_dirty = true;
}

void NoteDestroy(void* actor, uint32_t destroyEid) {
    if (!IsEnabled() || !actor) return;
    auto it = g_watch.find(actor);
    if (it == g_watch.end()) return;
    const auto age = std::chrono::steady_clock::now() - it->second.at;
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(age).count();
    if (age <= kFreshBind) {
        ++g_destroyedFresh;
        UE_LOGW("spawn_match_probe: DESTROY-BEHIND-BIND actor=%p adopted for eid=%u key='%ls' %lld ms ago, "
                "destroyed now (element id at the seam=%u) -- the adoption is spent and this eid's "
                "pose stream has no actor",
                actor, it->second.eid, it->second.key.c_str(), static_cast<long long>(ms), destroyEid);
    } else {
        ++g_destroyedLater;
    }
    g_watch.erase(it);
    g_dirty = true;
}

void Tick() {
    if (!IsEnabled() || !g_dirty) return;
    const auto now = std::chrono::steady_clock::now();
    if (g_lastVerdict.time_since_epoch().count() != 0 && now - g_lastVerdict < kVerdictPeriod) return;
    EmitVerdict();
}

void EmitVerdict() {
    if (!IsEnabled() || g_scans == 0) return;
    g_lastVerdict = std::chrono::steady_clock::now();
    g_dirty = false;
    UE_LOGW("spawn_match_probe: VERDICT scans=%d no-match=%d multi-candidate=%d not-nearest=%d "
            "hand-axis-refused=%d bound=%d rekeyed-in-place=%d rekeyed-at-range=%d "
            "destroyed-behind-bind=%d destroyed-later=%d",
            g_scans, g_scansNoMatch, g_scansMulti, g_notNearest, g_scansExcluded, g_bound,
            g_keyDivergedNear, g_keyDivergedFar, g_destroyedFresh, g_destroyedLater);
    const char* reading =
        (g_destroyedFresh > 0 && g_scansExcluded > 0)
                                 ? "ADOPTIONS STILL DIE BEHIND THE BIND WITH THE HAND AXIS ALREADY REFUSED -- a second producer of spent adoptions, not the hand mirror"
        : (g_bound == 0)         ? "NO ADOPTION -- the fuzzy path never took a candidate this run; nothing here bears on the ordering defect"
        : (g_destroyedFresh > 0) ? "ADOPTIONS DIE BEHIND THE BIND -- a destroy lands on the adopted actor within seconds, which is the pose-stream orphan"
        : (g_keyDivergedFar > 0) ? "ADOPTIONS TAKE A PROP AT RANGE -- a rekey metres from the wire spot is a different prop, not the same one under two keys"
        : (g_notNearest > 0)     ? "ORDER BEATS DISTANCE -- a nearer candidate was passed over for an earlier array slot"
                                 : "ADOPTIONS LOOK CLEAN -- taken in place, no early destroy; rekeyed-in-place is the per-process-key dedupe working";
    UE_LOGW("spawn_match_probe: reading :: %s", reading);
}

}  // namespace coop::dev::spawn_match_probe
