// coop/dev/lightswitch_probe.cpp -- see coop/dev/lightswitch_probe.h.

#include "coop/dev/lightswitch_probe.h"

#include "coop/config/config.h"
#include "harness/session_runtime.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/devices/lightswitch.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::lightswitch_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace LS = ue_wrap::lightswitch;

bool ProbeEnabled() {
    static const bool s_enabled = coop::config::ResolveFlag(::coop::config_registry::rows::lightswitch_probe);
    return s_enabled;
}

// Observer identification table: a FIRED log on a REAL flip = that edge is observable.
struct WatchedFn { void* fn = nullptr; const char* name = nullptr; };
std::array<WatchedFn, 8> g_watched{};
size_t g_watchedCount = 0;

void OnLightVerb(void* self, void* function, void* /*params*/) {
    if (!ProbeEnabled()) return;
    for (size_t i = 0; i < g_watchedCount; ++i) {
        if (g_watched[i].fn != function) continue;
        const std::wstring cls = (self && R::IsLive(self)) ? R::ClassNameOf(self) : std::wstring(L"?");
        UE_LOGI("[lightswitch_probe] VERB FIRED: %s (self=%p cls='%ls') -- ProcessEvent-OBSERVABLE",
                g_watched[i].name, self, cls.c_str());
        return;
    }
}

void RegisterOn(const wchar_t* className, const wchar_t* fnName, const char* label) {
    void* cls = R::FindClass(className);
    if (!cls) return;
    void* fn = R::FindFunction(cls, fnName);
    if (!fn) { UE_LOGW("[lightswitch_probe] '%ls::%ls' not found", className, fnName); return; }
    for (size_t i = 0; i < g_watchedCount; ++i) if (g_watched[i].fn == fn) return;
    if (!GT::RegisterPostObserver(fn, &OnLightVerb)) return;
    if (g_watchedCount < g_watched.size()) g_watched[g_watchedCount++] = WatchedFn{fn, label};
}

bool     g_installed   = false;
void*    g_switchCls   = nullptr;
int32_t  g_aOff        = -1;   // Alightswitch_C::A (the switch-flip bool)
int32_t  g_triggerOff  = -1;   // Alightswitch_C::Trigger (-> its lightRoot)
void*    g_useFn       = nullptr;
uint64_t g_tick        = 0;
bool     g_testDone    = false;

template <typename T> T ReadAt(void* obj, int32_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + off);
}

}  // namespace

void Install() {
    if (!ProbeEnabled() || g_installed) return;
    g_switchCls = R::FindClass(L"lightswitch_C");
    if (!g_switchCls) return;  // BP not loaded yet -- retry next tick
    LS::EnsureResolved();      // resolve the lightRoot class/offsets (best-effort)

    // Candidate SENDER edges (player-facing) + the current suspect-trap edge.
    RegisterOn(L"lightswitch_C",      L"player_use",        "lightswitch.player_use");
    RegisterOn(L"lightswitch_C",      L"use",               "lightswitch.use");
    RegisterOn(L"lightswitch_C",      L"actionOptionIndex", "lightswitch.actionOptionIndex");
    RegisterOn(L"trigger_lightRoot_C", L"SetActive",        "lightRoot.SetActive (current hook)");

    g_aOff       = R::FindPropertyOffset(g_switchCls, L"A");
    g_triggerOff = R::FindPropertyOffset(g_switchCls, L"Trigger");
    g_useFn      = R::FindFunction(g_switchCls, L"use");
    g_installed  = true;
    UE_LOGI("[lightswitch_probe] installed %zu observers; switch A@0x%X Trigger@0x%X use=%p. "
            "FLIP A REAL SWITCH -- a 'VERB FIRED' line names the observable sender edge.",
            g_watchedCount, g_aOff, g_triggerOff, g_useFn);
}

// Defined below Tick (it is the long one); declared here because Tick fires it.
void RunGroupApplySelftest();

void Tick() {
    if (!ProbeEnabled() || g_testDone || !g_installed) return;
    // Let the world (lightswitches) load before the one-shot synthetic flip.
    if (g_tick++ < 300) return;

    // SOLO ONLY. This probe MUTATES -- a synthetic use() plus the group selftest's four
    // writes -- and its window lands squarely in a joining client's connect-snapshot. Armed
    // on a host during `mp.py smoke` it cost the client its join: PASS with the flag off,
    // "expected 2 peers, got 1" with it on, twice, everything else identical. It is a solo
    // measurement instrument and this makes that its contract rather than its documentation.
    // Deliberately NOT latched: a solo run that later hosts still gets its measurement, and a
    // session that ends leaves the probe free to fire.
    // running(), not connected(): a HOST alone is not "connected" yet, and firing in that
    // window is precisely what broke the join -- the mutation landed just before the client
    // arrived. running() covers hosting-with-nobody-yet as well as an established link.
    const auto& sess = harness::session_runtime::Session();
    if (sess.running() || sess.connected()) {
        static bool s_said = false;
        if (!s_said) { s_said = true;
            UE_LOGW("[lightswitch_probe] a session is CONNECTED -- holding the mutating one-shot. "
                    "This probe writes world state; run it solo. (The read-only per-key census is "
                    "`lightgroup_census=1` and is safe on both peers.)"); }
        return;
    }
    g_testDone = true;

    void* sw = R::FindObjectByClass(L"lightswitch_C");
    if (!sw || !R::IsLive(sw)) {
        UE_LOGW("[lightswitch_probe] no live lightswitch_C found -- synthetic flip test skipped");
        return;
    }
    const int aBefore = (g_aOff >= 0) ? (int)ReadAt<uint8_t>(sw, g_aOff) : -1;
    void* root = (g_triggerOff >= 0) ? ReadAt<void*>(sw, g_triggerOff) : nullptr;
    bool actBefore = false;
    const bool haveRoot = root && R::IsLive(root) && LS::IsLightRoot(root) && LS::TryReadActive(root, actBefore);
    const std::wstring rootKey = haveRoot ? LS::GetKeyString(root) : std::wstring(L"-");
    UE_LOGI("[lightswitch_probe] TEST: switch=%p A=%d Trigger=%p (isLightRoot=%d key='%ls') IsActive=%d -- calling use()...",
            sw, aBefore, root, haveRoot ? 1 : 0, rootKey.c_str(), haveRoot ? (int)actBefore : -1);

    if (!g_useFn) { UE_LOGW("[lightswitch_probe] use() not resolved -- abort"); return; }
    ue_wrap::ParamFrame f(g_useFn);
    ue_wrap::Call(sw, f);

    const int aAfter = (g_aOff >= 0) ? (int)ReadAt<uint8_t>(sw, g_aOff) : -1;
    bool actAfter = false;
    const bool haveAfter = haveRoot && LS::TryReadActive(root, actAfter);
    UE_LOGI("[lightswitch_probe] TEST RESULT: switch A %d->%d (%s) ; lightRoot IsActive %d->%d (%s). "
            "If both changed but 'lightRoot.SetActive' did NOT fire above -> SetActive is BP-INTERNAL (the trap) "
            "AND use() does the FULL flip (switch visual + lights) -> SENDER hooks a player edge, RECEIVER replays use().",
            aBefore, aAfter, (aBefore != aAfter) ? "FLIPPED" : "unchanged",
            haveRoot ? (int)actBefore : -1, haveAfter ? (int)actAfter : -1,
            (haveRoot && haveAfter && actBefore != actAfter) ? "TOGGLED" : "unchanged");

    RunGroupApplySelftest();
}

// ---- GROUP-APPLY SELFTEST (2026-09-01) -----------------------------------------------
// The group lane's whole premise is that runTrigger index 1/2 moves a light group even where
// index 0 -- the verb a switch press uses -- refuses because the group's gate is shut. That is
// bytecode-derived; this executes it. It matters because at idle two peers AGREE, so an
// integration run can pass with the apply path never having run once: a lane can look healthy
// and be dead. The assertion order is deliberate -- the DEFECT is shown first (index 0 does
// nothing with the gate shut), so a green result cannot come from the group having been movable
// all along.
//
// Mutating, and it restores everything it touched: gate, then state, in that order.
void RunGroupApplySelftest() {
    void* root = nullptr;
    for (void* r : R::FindObjectsByClass(L"trigger_lightRoot_C")) {
        if (!r || !R::IsLive(r)) continue;
        const std::wstring k = LS::GetKeyString(r);
        if (k.empty() || k == L"None") continue;
        root = r; break;
    }
    if (!root) { UE_LOGW("[lightswitch_probe] GROUP SELFTEST: no keyed lightRoot -- SKIPPED"); return; }

    const std::wstring key = LS::GetKeyString(root);
    bool before = false;
    if (!LS::TryReadActive(root, before)) { UE_LOGW("[lightswitch_probe] GROUP SELFTEST: isActive unreadable -- SKIPPED"); return; }
    const bool gatePrior = LS::GetGroupGate(root);

    int pass = 0, fail = 0;
    auto check = [&](const char* what, bool ok) {
        if (ok) { ++pass; UE_LOGI("[lightswitch_probe] GROUP SELFTEST  ok   %s", what); }
        else    { ++fail; UE_LOGE("[lightswitch_probe] GROUP SELFTEST  FAIL %s", what); }
    };

    // 1. THE DEFECT: with the gate shut, the switch's own verb cannot move the group.
    // RAII for the same reason the production path uses it -- three ProcessEvent calls happen
    // inside this scope and a fault in any of them would otherwise leave the gate shut forever,
    // in a field that is save-persistent.
    {  // the hold's scope: everything needing the gate shut happens inside it
    LS::ScopedGroupGateShut hold(root);
    check("gate reads back shut after the scoped shut", hold.shut() && LS::GetGroupGate(root) == false);
    LS::CallRunTrigger(root, 0);
    bool afterGated = before;
    LS::TryReadActive(root, afterGated);
    check("index 0 (the switch's verb) is REFUSED while the gate is shut", afterGated == before);

    // 2. THE FIX: the absolute setters ignore the gate, which is what lets a receiver land the
    //    authority's state on a peer whose own breaker is off.
    LS::ApplyGroupState(root, !before);
    bool afterAbs = before;
    LS::TryReadActive(root, afterAbs);
    check("ApplyGroupState moved isActive DESPITE the shut gate", afterAbs == !before);

    LS::ApplyGroupState(root, before);
    bool restored = !before;
    LS::TryReadActive(root, restored);
    check("ApplyGroupState restored the original isActive", restored == before);

    }  // <-- the hold releases HERE

    // 3. Assert the restore ACTUALLY happened rather than trusting that the destructor ran:
    //    "it leaves nothing behind" is the claim that matters, so it gets its own assertion.
    check("gate restored to its prior value at scope exit", LS::GetGroupGate(root) == gatePrior);

    UE_LOGI("[lightswitch_probe] GROUP SELFTEST: %s (%d passed, %d failed) key='%ls' gateWas=%d isActiveWas=%d",
            fail == 0 ? "ALL PASS" : "FAILED", pass, fail, key.c_str(), gatePrior ? 1 : 0, before ? 1 : 0);
}

}  // namespace coop::dev::lightswitch_probe
