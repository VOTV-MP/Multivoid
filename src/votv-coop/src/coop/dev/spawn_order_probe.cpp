// coop/dev/spawn_order_probe.cpp -- see header.

#include "coop/dev/spawn_order_probe.h"

#include "coop/config/config.h"
#include "coop/creatures/kerfur_entity.h"   // IsKerfurPropClass (off-prop kerfur family)
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"         // IsChipPile
#include "ue_wrap/core/reflection.h"

#include <unordered_map>
#include <unordered_set>

namespace coop::dev::spawn_order_probe {
namespace {

namespace R = ue_wrap::reflection;

// All state game-thread-only (the thunk + the sweep tick + BeginClaimTracking are GT). No mutex.
//
// Recording is CONTINUOUS while enabled (NOT gated on a join-arm window): the keyless natives can spawn
// BEFORE the snapshot bracket opens (the client's own-save boot load precedes connect; the host-blob load
// can precede SnapshotBegin), so gating recording to [ArmForJoin..quiescence] would MISS them and read a
// false fired=0. The first probe run (2026-06-25 16:15) hit exactly this -- the boot 871 piles loaded at
// 16:14:44, the probe armed at 16:15:04, so fired=0 told us nothing. Recording from the first thunk fire
// answers the real question (does the thunk EVER fire for a keyless spawn?) regardless of bracket timing.
bool g_verdictPending = false;  // a join armed -> EmitVerdictAtQuiescence should report (set by ArmForJoin)
// Recorded keyless spawns this session, by actor ptr -> family. A map so the quiescence walk can confirm the
// family. Stale entries (a deduped spawn whose address was recycled) are harmless: the survivor lookup only
// reads LIVE survivors, and a recycled address a survivor now occupies WAS fired at that address anyway.
std::unordered_map<void*, Family> g_fired;
int g_fireChip = 0;
int g_fireKerfur = 0;
constexpr size_t kFiredCap = 8192;  // backstop against unbounded growth over a long session (a probe)

const char* FamilyName(Family f) { return f == Family::ChipPile ? "chipPile" : "kerfurOff"; }

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::IsIniKeyTrue("spawn_order_probe");
    return s;
}

void ArmForJoin() {
    if (!IsEnabled()) return;
    // Do NOT clear the recorded set here: keyless natives loaded BEFORE this bracket (boot own-save load /
    // a host-blob load that preceded SnapshotBegin) must stay recorded so the verdict can see them. The set
    // is cleared after the verdict instead. ArmForJoin only marks that a verdict is now expected.
    g_verdictPending = true;
    UE_LOGW("spawn_order_probe: join bracket open, verdict pending -- recording (continuous) every "
            "keyless-family BeginDeferred spawn (chipPile + off-kerfur) so far this session: chipPile=%d "
            "kerfurOff=%d (read-only; build plan 1A)", g_fireChip, g_fireKerfur);
}

void NoteKeylessSpawn(void* newActor, Family family) {
    if (!newActor) return;  // recording is continuous (the thunk gates on IsEnabled+Client before calling)
    if (g_fired.size() >= kFiredCap) return;  // probe backstop -- stop inserting (the verdict still reports)
    // insert-or-refresh; a re-fire on the same ptr is counted once (map key).
    auto [it, inserted] = g_fired.try_emplace(newActor, family);
    if (!inserted) {
        it->second = family;
        return;  // already recorded this ptr (dedup re-spawn at a recycled address) -> do not double-count
    }
    if (family == Family::ChipPile) ++g_fireChip; else ++g_fireKerfur;
}

void EmitVerdictAtQuiescence() {
    if (!g_verdictPending) return;
    g_verdictPending = false;  // one verdict per join

    // Walk the GUObjectArray for the SURVIVING keyless natives (the set the bind would have to cover) and
    // check each was recorded by the thunk. Same cheap class filters the seed-walk uses (pointer-class
    // compares; ClassNameOf only never needed -- IsChipPile / IsKerfurPropClass are lineage tests).
    const int32_t n = R::NumObjects();
    int survChip = 0, survKerfur = 0;     // live keyless natives at quiescence
    int caughtChip = 0, caughtKerfur = 0; // ... of which the thunk recorded
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO
        void* cls = R::ClassOf(obj);
        Family fam;
        if (ue_wrap::prop::IsChipPile(obj)) {
            fam = Family::ChipPile;
        } else if (cls && coop::kerfur_entity::IsKerfurPropClass(cls)) {
            fam = Family::KerfurOff;
        } else {
            continue;  // not a keyless family the map covers
        }
        const bool caught = (g_fired.find(obj) != g_fired.end());
        if (fam == Family::ChipPile) { ++survChip; if (caught) ++caughtChip; }
        else                         { ++survKerfur; if (caught) ++caughtKerfur; }
    }

    const int missChip = survChip - caughtChip;
    const int missKerfur = survKerfur - caughtKerfur;
    auto verdict = [](int miss, int surv) {
        if (surv == 0) return "N/A (no survivors of this family)";
        return miss == 0 ? "CAUGHT-ALL -> spawn-order is the deterministic PRIMARY (S4.1)"
                         : "MISSES -> fall to the exact-transform bijection (S4.2/S3.3)";
    };
    UE_LOGW("spawn_order_probe: VERDICT chipPile -- fired=%d survivors=%d caught=%d missed=%d :: %s",
            g_fireChip, survChip, caughtChip, missChip, verdict(missChip, survChip));
    UE_LOGW("spawn_order_probe: VERDICT kerfurOff -- fired=%d survivors=%d caught=%d missed=%d :: %s",
            g_fireKerfur, survKerfur, caughtKerfur, missKerfur, verdict(missKerfur, survKerfur));
    UE_LOGW("spawn_order_probe: (fired = keyless BeginDeferred spawns the thunk saw this join; survivors = "
            "live keyless natives at quiescence; caught = survivors the thunk recorded. missed>0 means the "
            "native thunk does NOT see every keyless load-spawn -> spawn-order cannot be the primary.)");
    g_fired.clear();  // release the recorded set; the verdict is emitted
}

}  // namespace coop::dev::spawn_order_probe
