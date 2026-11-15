// coop/world/obelisk_copy.cpp -- see coop/world/obelisk_copy.h.

#include "coop/world/obelisk_copy.h"

#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>

namespace coop::obelisk_copy {
namespace {

namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;

UE_FACT_FN(kRowBeginDeferred, "obelisk_copy", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);

void*    g_copy = nullptr;      // the obelisk copy whose mints are refused
int32_t  g_copyIdx = -1;
void*    g_hookFn = nullptr;
bool     g_hooked = false;
bool     g_armed = false;
int32_t  g_fallenOff = -1;      // obelisk_C::fallen
constexpr uint32_t kMints = 71; // the obelisk prop and the seventy rocks
uint32_t g_refused = 0;

SG::Verdict OnBeginDeferredPre(void* /*context*/, void* source, const uint8_t* parms);

void Disarm(const char* why) {
    if (g_armed && UH::SetArmed(g_hookFn, &OnBeginDeferredPre, false)) {
        g_armed = false;
        UE_LOGI("obelisk_copy: %s -- %u mint(s) of the copy refused, the spawn hook is disarmed", why, g_refused);
    }
    g_copy = nullptr;
    g_copyIdx = -1;
    g_refused = 0;
}

// Before any deferred spawn, while armed: one whose source is the copy is refused. The graph mints the
// prop_obelisk_C at the landing and the seventy rocks a tick or two later, so the count ends the watch; a copy that
// has died or has set `fallen` (ten seconds after the landing) ends it too.
SG::Verdict OnBeginDeferredPre(void* /*context*/, void* source, const uint8_t* /*parms*/) {
    if (!g_copy) return SG::Verdict::Run;
    if (!R::IsLiveByIndex(g_copy, g_copyIdx) ||
        (g_fallenOff >= 0 && *(static_cast<const uint8_t*>(g_copy) + g_fallenOff) != 0)) {
        Disarm("the copy has fallen or ended");
        return SG::Verdict::Run;
    }
    if (source != g_copy || !coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    if (++g_refused >= kMints) Disarm("the copy made its last mint");
    return SG::Verdict::Cancel;
}

}  // namespace

void OnCopy(void* actor) {
    if (!actor || !R::IsLive(actor)) return;
    g_fallenOff = R::FindPropertyOffset(R::ClassOf(actor), L"fallen");
    if (g_fallenOff >= 0 && *(static_cast<const uint8_t*>(actor) + g_fallenOff) != 0) {
        UE_LOGI("obelisk_copy: the copy %p has already fallen -- nothing to refuse", actor);
        return;
    }
    if (!g_hooked) {
        const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowBeginDeferred);
        if (!fn || !UH::InstallPreHook(fn, &OnBeginDeferredPre, /*armed=*/false)) {
            UE_LOGE("obelisk_copy: the deferred spawn's pre hook did not install -- the copy %p mints its own rocks "
                    "and obelisk prop beside the host's", actor);
            return;
        }
        g_hooked = true;
        g_hookFn = fn.Fn();
    }
    g_copy = actor;
    g_copyIdx = R::InternalIndexOf(actor);
    g_refused = 0;
    g_armed = UH::SetArmed(g_hookFn, &OnBeginDeferredPre, true);
    UE_LOGI("obelisk_copy: the copy %p falls here; its own mints are refused (hook armed: %d)", actor, g_armed ? 1 : 0);
}

void OnCopyGone(void* actor) {
    if (actor && actor == g_copy) Disarm("the copy ended");
}

void OnDisconnect() {
    Disarm("the session ended");
}

}  // namespace coop::obelisk_copy
