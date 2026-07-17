// coop/props/prop_wire_parity.cpp -- see coop/props/prop_wire_parity.h.
//
// Bodies moved VERBATIM from remote_prop_spawn.cpp's anon namespace 2026-07-12 (extraction); the
// log prefixes keep the original "remote_prop::OnSpawn" wording so existing log greps stay valid.

#include "coop/props/prop_wire_parity.h"

#include "coop/net/protocol.h"
#include "coop/props/remote_prop.h"  // DriveSimulate
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

namespace coop::prop_wire_parity {

namespace R = ue_wrap::reflection;

// ==== RULE 1 RETIREMENT PLAN (audit fix 2026-05-25, issue #4) ====
// The collision restore is an INTERIM fix at the SYMPTOM (collision write at OnSpawn-time). The
// architecturally correct ROOT-CAUSE fix is to suppress AmushroomSpawner_C::Spawn on the client
// entirely, so the local actor never goes through spawnedNaturally() in the first place (Option B
// from the RE doc). Gating criteria for retirement:
//   (1) Phase 5N1 Stream B-Spawners ships -- single BeginDeferredActorSpawnFromClass observer with
//       a class allowlist that includes AmushroomSpawner_C (covers all natural spawners at once).
//   (2) Hands-on test confirms mushrooms sync correctly with this restore call commented out.
//   (3) Then remove IsCollisionRestoreClass + RestoreCollisionIfNeeded + the
//       PropFoodMushroomClass constant. Per RULE 2 no parallel paths.
// Tracked in [[project-coop-mushroom-state-re]] under "Stream B-Spawners" follow-up scope.
bool IsCollisionRestoreClass(const std::wstring& cls) {
    return cls == ue_wrap::profile::name::PropFoodMushroomClass;
}

void RestoreCollisionIfNeeded(const wchar_t* pathLabel,
                              const std::wstring& classW,
                              void* actor) {
    if (!IsCollisionRestoreClass(classW)) return;
    const bool ok = ue_wrap::prop::ForceRestoreDefaultCollision(actor);
    if (ok) {
        UE_LOGI("remote_prop::OnSpawn[%ls]: collision restored (QueryAndPhysics) on '%ls' actor=%p",
                pathLabel, classW.c_str(), actor);
    } else {
        UE_LOGW("remote_prop::OnSpawn[%ls]: collision restore FAILED on '%ls' actor=%p -- prop may fall through ground",
                pathLabel, classW.c_str(), actor);
    }
}

void ReconcileToHostPhysics(void* actor, uint8_t physFlags) {
    const bool hostSimulating =
        (physFlags & coop::net::propspawn_flags::kSimulatePhysics) != 0;
    if (hostSimulating) return;
    if (!actor || !R::IsLive(actor)) return;
    void* mesh = ue_wrap::prop::GetStaticMesh(actor);  // Aprop_C only; null for non-Aprop_C
    if (!mesh) return;
    coop::remote_prop::DriveSimulate(mesh, /*simulate=*/false);
}

bool SpParitySimulate(uint8_t physFlags) {
    namespace pf = coop::net::propspawn_flags;
    return (physFlags & (pf::kStatic | pf::kFrozen | pf::kSleep)) == 0;
}

void RestoreSpParityPhysicsAfterConverge(void* actor, uint8_t physFlags) {
    if (!actor || !R::IsLive(actor)) return;
    void* mesh = ue_wrap::prop::GetStaticMesh(actor);
    if (!mesh) return;
    coop::remote_prop::DriveSimulate(mesh, SpParitySimulate(physFlags));
}

}  // namespace coop::prop_wire_parity
