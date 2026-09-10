// coop/save/save_indicator_suppress.cpp -- see the header. Detect-only, host-side.
#include "coop/save/save_indicator_suppress.h"

#include <chrono>

#include "ue_wrap/core/hot_path_guard.h"   // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/ufunction_hook.h"

namespace coop::save_indicator_suppress {

namespace {

namespace R = ue_wrap::reflection;

// Game thread only (the join-save capture is synchronous on the game thread, and the
// saveAnim/addHint Func dispatch is game-thread) -> plain values, no lock.
bool g_joinSaveActive = false;
bool g_installed      = false;
int  g_saveAnimHits   = 0;
int  g_addHintHits    = 0;
// The SAVED UMG anim may be DEFERRED (saveObjects sets state; the HUD plays it on a later
// tick OUTSIDE the synchronous capture). So we also catch fires for kPostWindowMs after
// the window closes, marked as deferred. 0 = not in a post-window catch.
long long g_postWindowUntilMs = 0;
constexpr long long kPostWindowMs = 3000;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Returns the window phase for a fire, or nullptr if outside any catch (ignore -- a
// normal-gameplay save/hint, don't spam).
const char* WindowPhase(long long& outDeferredMs) {
    if (g_joinSaveActive) { outDeferredMs = 0; return "IN-WINDOW"; }
    if (g_postWindowUntilMs != 0) {
        const long long now = NowMs();
        if (now < g_postWindowUntilMs) { outDeferredMs = now - (g_postWindowUntilMs - kPostWindowMs); return "DEFERRED-post-window"; }
    }
    return nullptr;
}

// Post-hooks fire AFTER the original saveAnim/addHint runs, and forward to it, so the
// indicator still shows -- this module only reports. They log a fire only IN the join-save
// window or within 3 s after it (the deferred catch); a normal-gameplay save or hint outside
// that is a bool check and a return.
void OnSaveAnim(void* /*context*/, void* /*src*/, void* /*res*/) {
    long long deferred = 0;
    const char* phase = WindowPhase(deferred);
    if (!phase) return;
    ++g_saveAnimHits;
    UE_LOGW("[SAVED-DETECT] mainGamemode.saveAnim FIRED [%s] (hit #%d) -- the UMG save-animation "
            "trigger, one of the two candidate painters of the SAVED indicator",
            phase, g_saveAnimHits);
    if (deferred) UE_LOGW("[SAVED-DETECT]   ^ saveAnim was DEFERRED ~%lldms after the capture window closed", deferred);
}
void OnAddHint(void* /*context*/, void* /*src*/, void* /*res*/) {
    long long deferred = 0;
    const char* phase = WindowPhase(deferred);
    if (!phase) return;
    ++g_addHintHits;
    UE_LOGW("[SAVED-DETECT] mainGamemode 'Add Hint from Gamemode' (addHint) FIRED [%s] (hit #%d) "
            "-- the corner-notification path%s", phase, g_addHintHits,
            deferred ? " (DEFERRED)" : "");
}

}  // namespace

void EnsureInstalled() {
    UE_ASSERT_GAME_THREAD("save_indicator_suppress::EnsureInstalled");
    if (g_installed) return;
    void* gmCls = R::FindClass(L"mainGamemode_C");
    if (!gmCls) return;  // gamemode class not loaded yet -> retry next Begin
    void* saveAnimFn = R::FindFunction(gmCls, L"saveAnim");
    void* addHintFn  = R::FindFunction(gmCls, L"Add Hint from Gamemode");
    bool any = false;
    if (saveAnimFn) { ue_wrap::ufunction_hook::InstallPostHook(saveAnimFn, &OnSaveAnim); any = true; }
    else UE_LOGW("[SAVED-DETECT] mainGamemode.saveAnim UFunction NOT resolved -- saveAnim detect hook skipped");
    if (addHintFn)  { ue_wrap::ufunction_hook::InstallPostHook(addHintFn, &OnAddHint);  any = true; }
    else UE_LOGW("[SAVED-DETECT] mainGamemode 'Add Hint from Gamemode' UFunction NOT resolved -- addHint detect hook skipped");
    g_installed = any;  // latch only if at least one resolved (else retry next Begin)
    if (any)
        UE_LOGI("[SAVED-DETECT] detect hooks installed (saveAnim=%p addHint=%p) -- read-only; "
                "both forward to the original, so the indicator still shows",
                saveAnimFn, addHintFn);
}

void Begin() {
    UE_ASSERT_GAME_THREAD("save_indicator_suppress::Begin");
    EnsureInstalled();
    g_joinSaveActive = true;
    g_saveAnimHits = 0;
    g_addHintHits  = 0;
    UE_LOGI("[SAVED-DETECT] join-save window OPEN (host-side) -- detecting the SAVED indicator");
}

void End() {
    UE_ASSERT_GAME_THREAD("save_indicator_suppress::End");
    g_joinSaveActive = false;
    g_postWindowUntilMs = NowMs() + kPostWindowMs;  // keep catching deferred fires ~3 s
    UE_LOGW("[SAVED-DETECT] join-save window CLOSED -- in-window saveAnim=%d addHint=%d; "
            "watching ~3s for a deferred fire. Both staying 0 while the indicator showed "
            "means a painter neither hook covers.",
            g_saveAnimHits, g_addHintHits);
}

}  // namespace coop::save_indicator_suppress
