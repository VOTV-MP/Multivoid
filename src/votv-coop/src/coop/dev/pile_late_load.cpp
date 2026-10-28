// coop/dev/pile_late_load.cpp -- see coop/dev/pile_late_load.h.

#include "coop/dev/pile_late_load.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/pile_defer_report.h"
#include "coop/props/save_time_retire_util.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>

namespace coop::dev::pile_late_load {
namespace {

namespace Rep = coop::dev::pile_defer_report;

constexpr float kAsideCm = 200.f;   // along +X
constexpr float kKeyR2Cm = coop::save_time_retire_util::kExactMatchR2Cm;   // the bind's own 1 cm match

enum class Phase { None, Aside, Deferred, Done };

Phase g_phase = Phase::None;
ue_wrap::CachedObjRef g_clump;
ue_wrap::FVector g_saved{};
uint32_t g_eid = 0;
uint64_t g_tickNo = 0, g_deferredTick = 0;
// One clump per PROCESS: a rehost's second join loads a save whose clumps the first join already
// placed, and a seam acting there could choose one the host no longer sends.
bool g_usedInProcess = false;
bool g_choseThisSession = false;

}  // namespace

bool On() {
    static const bool on = coop::config::ResolveInt(::coop::config_registry::rows::pile_late_load) != 0 &&
                           Rep::ArmOf() != Rep::Arm::Off;
    return on;
}

void NoteLoadCaught(void* clump) {
    if (!On() || g_usedInProcess || g_phase != Phase::None || !clump) return;
    if (!ue_wrap::engine::TryGetActorLocation(clump, g_saved)) return;
    ue_wrap::FVector aside = g_saved;
    aside.X += kAsideCm;
    if (!ue_wrap::engine::SetActorLocation(clump, aside)) return;
    g_clump.Set(clump);
    g_phase = Phase::Aside;
    g_usedInProcess = true;
    g_choseThisSession = true;
    UE_LOGI("[PILE-LATE-LOAD] test seam: save-loaded clump kept aside at load, saved at (%.1f,%.1f,%.1f) -- an "
            "artificial late load, not a natural deferral", g_saved.X, g_saved.Y, g_saved.Z);
}

void NoteDeferred(uint32_t eid, const ue_wrap::FVector& key, bool wantClump) {
    if (!On() || g_phase != Phase::Aside || !wantClump) return;
    const float dx = key.X - g_saved.X, dy = key.Y - g_saved.Y, dz = key.Z - g_saved.Z;
    if (dx * dx + dy * dy + dz * dz > kKeyR2Cm) return;
    g_eid = eid;
    g_phase = Phase::Deferred;
    g_deferredTick = g_tickNo;
    UE_LOGI("[PILE-LATE-LOAD] chosen eid=%u deferred", eid);
}

uint32_t ChosenEid() { return g_eid; }
bool ChoseThisSession() { return g_choseThisSession; }

void Tick() {
    if (!On()) return;
    ++g_tickNo;
    if (g_phase != Phase::Deferred || g_tickNo <= g_deferredTick) return;
    g_phase = Phase::Done;
    void* clump = g_clump.Get();
    if (!clump) {
        UE_LOGW("[PILE-LATE-LOAD] eid=%u: the kept-aside clump is gone before the seam acted", g_eid);
        return;
    }
    if (Rep::ArmOf() == Rep::Arm::Giveup) {
        coop::save_time_retire_util::UnmarkAndDestroy(clump);
        UE_LOGI("[PILE-LATE-LOAD] native destroyed (never loaded)");
    } else {
        ue_wrap::engine::SetActorLocation(clump, g_saved);
        UE_LOGI("[PILE-LATE-LOAD] native put back at its saved pose");
    }
}

void OnDisconnect() {
    if (!On()) return;
    if (g_phase == Phase::Aside || g_phase == Phase::Deferred) {
        if (void* clump = g_clump.Get()) ue_wrap::engine::SetActorLocation(clump, g_saved);
    }
    g_clump.Set(nullptr);
    g_phase = Phase::None;
    g_eid = 0;
    g_deferredTick = 0;
    g_choseThisSession = false;
}

}  // namespace coop::dev::pile_late_load
