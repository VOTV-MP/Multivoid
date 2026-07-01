#include "ue_wrap/asset_load.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <string>

namespace ue_wrap::asset_load {

namespace {
namespace R = ue_wrap::reflection;
}  // namespace

void* LoadObjectByPath(const wchar_t* fullObjectPath) {
    if (!fullObjectPath || !*fullObjectPath) return nullptr;

    // URyRuntimeObjectHelpers::LoadObject(FString fullObjectPath) -> UObject*.
    // A static BlueprintFunctionLibrary UFunction: resolve the class's CDO (a
    // valid object of the class to ProcessEvent on), find the fn on that class,
    // then dispatch a {FString in, UObject* ReturnValue} frame. Pattern from the
    // pak-mount feasibility POC (research/findings/votv-mp-pak-mount-feasibility).
    void* cdo = R::FindClassDefaultObject(L"RyRuntimeObjectHelpers");
    if (!cdo) {
        UE_LOGW("asset_load: RyRuntimeObjectHelpers CDO not found -- RyRuntime plugin absent; "
                "cannot LoadObject('%ls')", fullObjectPath);
        return nullptr;
    }
    void* fn = R::FindFunction(R::ClassOf(cdo), L"LoadObject");
    if (!fn) {
        UE_LOGW("asset_load: RyRuntimeObjectHelpers::LoadObject UFunction not found");
        return nullptr;
    }

    // The param-frame FString aliases OUR buffer for the call's duration.
    // LoadObject only READS it (path -> object resolve) and never retains it, so a
    // local buffer is safe -- the same convention engine::ExecuteConsoleCommand
    // uses for its command FString. FString::Num counts the trailing NUL, which
    // std::wstring::data() guarantees is present.
    std::wstring path(fullObjectPath);
    R::FString fs{ path.data(),
                   static_cast<int32_t>(path.size()) + 1,
                   static_cast<int32_t>(path.size()) + 1 };

    ParamFrame f(fn);
    if (!f.Set<R::FString>(L"fullObjectPath", fs)) return nullptr;
    if (!Call(cdo, f)) {
        UE_LOGW("asset_load: LoadObject('%ls') ProcessEvent dispatch failed", fullObjectPath);
        return nullptr;
    }

    void* obj = f.Get<void*>(L"ReturnValue");
    if (obj) {
        // Log the loaded object's identity + package chain so a NAME COLLISION is
        // visible: our pak export is named `kerfurOmega_KelSkin` (same as the game's
        // own mesh). If the outer/package is `/Game/Mods/VOTVCoop/scientist` we got
        // OURS; if it's the game's kerfurAnthro package, StaticLoadObject resolved to
        // the resident game object instead (docs/COOP_CLIENT_MODEL.md 6a/8 rename).
        void* outer  = R::OuterOf(obj);
        void* outer2 = outer ? R::OuterOf(outer) : nullptr;
        UE_LOGI("asset_load: LoadObject('%ls') -> %p [obj='%ls' class='%ls' outer='%ls' pkg='%ls']",
                fullObjectPath, obj,
                R::ToString(R::NameOf(obj)).c_str(),
                R::ClassNameOf(obj).c_str(),
                outer  ? R::ToString(R::NameOf(outer)).c_str()  : L"?",
                outer2 ? R::ToString(R::NameOf(outer2)).c_str() : L"?");
    } else {
        UE_LOGW("asset_load: LoadObject('%ls') returned null -- pak not mounted or path wrong",
                fullObjectPath);
    }
    return obj;
}

}  // namespace ue_wrap::asset_load
