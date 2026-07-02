// coop/session/save_apply_gate.cpp -- see header.

#include "coop/session/save_apply_gate.h"

#include "coop/player/players_registry.h"  // Registry::Get().Local() -- the pawn the stamp anchors
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/ufunction_hook.h"

#include <cstdint>

namespace coop::save_apply_gate {
namespace {

namespace R = ue_wrap::reflection;

// Game-thread only (pump tick + BP Func dispatch) -> plain statics, no atomics.
bool    g_installed     = false;
void*   g_appliedAnchor = nullptr;  // gm instance (POST path) or the pawn itself (pre-session stamp)
int32_t g_anchorIdx     = -1;       // its GUObjectArray slot -- IsLiveByIndex revalidation
void*   g_appliedPawn   = nullptr;  // the local pawn the placement belongs to

void OnLoadObjectsPost(void* src, void* /*res*/) {
    // gm::loadObjects RETURNED: the save is applied -- objects spawned AND the player
    // teleported to the save's playerTransform (or legitimately left at the map spawn
    // for a zero transform). Stamp the placement for the CURRENT local pawn.
    void* pawn = coop::players::Registry::Get().Local();
    g_appliedAnchor = src;
    g_anchorIdx     = src ? R::InternalIndexOf(src) : -1;
    g_appliedPawn   = pawn;
    if (pawn)
        UE_LOGI("save_apply_gate: gm loadObjects DONE (gm=%p) -- save applied; local pawn %p "
                "is PLACED, pose stream unlocked for this world", src, pawn);
    else
        // A loadObjects with no possessed local pawn would keep the stream gated. Not an
        // expected flow (possession precedes the load tail on every observed path) -- loud
        // so a real occurrence is diagnosable from one log line.
        UE_LOGW("save_apply_gate: gm loadObjects DONE (gm=%p) but NO local pawn resolved -- "
                "pose stream stays gated until the pawn's world re-runs loadObjects", src);
}

}  // namespace

void EnsureInstalled() {
    if (g_installed) return;
    void* gmCls = R::FindClass(L"mainGamemode_C");
    if (!gmCls) return;  // gameplay GM class not loaded yet (menu window) -- retry next tick
    void* fn = R::FindFunction(gmCls, L"loadObjects");
    if (!fn) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGW("save_apply_gate: mainGamemode_C::loadObjects UFunction NOT resolved -- "
                    "pawn-placement latch unavailable (pose stream will stay gated!)");
        }
        return;
    }
    if (!ue_wrap::ufunction_hook::InstallPostHook(fn, &OnLoadObjectsPost)) {
        static bool s_failed = false;
        if (!s_failed) {
            s_failed = true;
            UE_LOGW("save_apply_gate: InstallPostHook(loadObjects) REFUSED (Func table full / "
                    "bad slot) -- pawn-placement latch unavailable");
        }
        return;
    }
    g_installed = true;
    // Pre-session world: a local pawn already live at hook-install time means this world's
    // loadObjects pre-dates the hook (browser-flow host clicking Host mid-game, or an
    // in-world joiner before its save-transfer reload). Its load is definitionally done --
    // stamp now, with the pawn itself as the liveness anchor. A fresh boot (env host /
    // menu-join client) has NO pawn yet at this point, so the POST latch alone decides.
    void* pawn = coop::players::Registry::Get().Local();
    if (pawn) {
        g_appliedAnchor = pawn;
        g_anchorIdx     = R::InternalIndexOf(pawn);
        g_appliedPawn   = pawn;
        UE_LOGI("save_apply_gate: loadObjects POST latch installed (fn=%p); local pawn %p was "
                "already live -> pre-session world stamped as placed", fn, pawn);
    } else {
        UE_LOGI("save_apply_gate: loadObjects POST latch installed (fn=%p); awaiting this "
                "world's loadObjects for the placement stamp", fn);
    }
}

bool IsSaveAppliedFor(void* localPawn) {
    return localPawn && localPawn == g_appliedPawn &&
           g_appliedAnchor && R::IsLiveByIndex(g_appliedAnchor, g_anchorIdx);
}

}  // namespace coop::save_apply_gate
