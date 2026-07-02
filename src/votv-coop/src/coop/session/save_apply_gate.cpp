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
// True once ANY pump tick ran without the gameplay GM class loaded (menu / pre-world).
// Discriminates the two "pawn already live at install time" cases: a menu-join client's
// install can only land on the FIRST POST-TRAVEL tick (`open untitled_1` BLOCKS the game
// thread, so gm class + the parked possessed pawn appear to us in the same tick) -- there
// the world came up DURING this session and its loadObjects is still coming (the ubergraph
// stages it across rendered frames), so the pre-session stamp must NOT fire. A first
// session started from INSIDE a world (browser-flow host / in-world joiner) never sees a
// pre-world tick -> its world predates the hook -> stamp. Later sessions in the same
// process need no install-time stamp at all: the process-lifetime POST hook already
// stamped every world load since.
bool    g_sawPreWorldTick = false;
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
    // Throttle the WHOLE unlatched path to ~1 Hz (perf audit 2026-07-02): FindClass is a
    // full GUObjectArray walk (there is no name->class cache), and this runs per pump tick
    // through the entire menu-join download window -- unthrottled it is 60 walks/s on the
    // same thread draining the transfer (the firefly_sync %-throttle precedent). The first
    // call still attempts immediately; a ~1 s install granularity is safe -- the gm class
    // loads at map open and loadObjects fires at the load tail, >=4 s later. Also bounds
    // the two permanent-failure branches below (fn-null / hook-refused) to 1 walk/s
    // instead of per-tick, without failing the gate closed forever on a transient refuse.
    static uint32_t s_throttle = 0;
    if ((s_throttle++ % 64) != 0) return;  // call #1 attempts immediately (0 % 64 == 0)
    void* gmCls = R::FindClass(L"mainGamemode_C");
    if (!gmCls) {
        g_sawPreWorldTick = true;  // ticking before any gameplay world this session
        return;                    // retry (throttled) -- the class loads with the map
    }
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
    // Pre-session world: a pawn already live at install time AND no pre-world tick seen
    // this session means the world predates the hook entirely (first session started from
    // INSIDE a world: browser-flow host / in-world joiner) -- its load is definitionally
    // done, stamp now with the pawn itself as the liveness anchor. With a pre-world tick
    // on record the pawn is the PARKED mid-load pawn of a world that came up during this
    // session (menu-join: the blocking travel makes gm class + possessed pawn appear to
    // us in the same tick) -- do NOT stamp; this world's loadObjects POST decides.
    void* pawn = coop::players::Registry::Get().Local();
    if (pawn && !g_sawPreWorldTick) {
        g_appliedAnchor = pawn;
        g_anchorIdx     = R::InternalIndexOf(pawn);
        g_appliedPawn   = pawn;
        UE_LOGI("save_apply_gate: loadObjects POST latch installed (fn=%p); pre-session world "
                "(no pre-world tick this session) -> local pawn %p stamped as placed", fn, pawn);
    } else {
        UE_LOGI("save_apply_gate: loadObjects POST latch installed (fn=%p); awaiting this "
                "world's loadObjects for the placement stamp (pawn=%p preWorldTick=%d)",
                fn, pawn, g_sawPreWorldTick ? 1 : 0);
    }
}

bool IsSaveAppliedFor(void* localPawn) {
    return localPawn && localPawn == g_appliedPawn &&
           g_appliedAnchor && R::IsLiveByIndex(g_appliedAnchor, g_anchorIdx);
}

}  // namespace coop::save_apply_gate
