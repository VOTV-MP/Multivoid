// coop/dev/lightswitch_probe.cpp -- see coop/dev/lightswitch_probe.h.

#include "coop/dev/lightswitch_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/devices/lightswitch.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <array>
#include <cstdint>
#include <string>

namespace coop::dev::lightswitch_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace LS = ue_wrap::lightswitch;

bool ProbeEnabled() {
    static const bool s_enabled = coop::config::IsIniKeyTrue("lightswitch_probe");
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

void Tick() {
    if (!ProbeEnabled() || g_testDone || !g_installed) return;
    // Let the world (lightswitches) load before the one-shot synthetic flip.
    if (g_tick++ < 300) return;
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
}

}  // namespace coop::dev::lightswitch_probe
