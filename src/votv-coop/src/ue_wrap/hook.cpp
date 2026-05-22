#include "ue_wrap/hook.h"

#include "ue_wrap/log.h"

#include <MinHook.h>

namespace ue_wrap::hook {
namespace {

bool g_initialized = false;

const char* StatusName(MH_STATUS s) { return MH_StatusToString(s); }

}  // namespace

bool Init() {
    if (g_initialized) return true;
    const MH_STATUS s = MH_Initialize();
    if (s != MH_OK) {
        UE_LOGE("hook: MH_Initialize failed (%s)", StatusName(s));
        return false;
    }
    g_initialized = true;
    UE_LOGI("hook: MinHook initialized");
    return true;
}

bool Install(void* target, void* detour, void** original) {
    if (!g_initialized && !Init()) return false;
    if (!target || !detour || !original) {
        UE_LOGE("hook: Install called with null target/detour/original");
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) {
        UE_LOGE("hook: MH_CreateHook(%p) failed (%s)", target, StatusName(s));
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        UE_LOGE("hook: MH_EnableHook(%p) failed (%s)", target, StatusName(s));
        MH_RemoveHook(target);  // don't leave a created-but-disabled hook behind
        return false;
    }
    UE_LOGI("hook: installed on %p (trampoline %p)", target, *original);
    return true;
}

bool Uninstall(void* target) {
    if (!g_initialized || !target) return false;
    bool ok = true;
    MH_STATUS s = MH_DisableHook(target);
    if (s != MH_OK) {
        UE_LOGW("hook: MH_DisableHook(%p) (%s)", target, StatusName(s));
        ok = false;
    }
    s = MH_RemoveHook(target);
    if (s != MH_OK) {
        UE_LOGW("hook: MH_RemoveHook(%p) (%s)", target, StatusName(s));
        ok = false;
    }
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_initialized = false;
    UE_LOGI("hook: MinHook shut down");
}

}  // namespace ue_wrap::hook
