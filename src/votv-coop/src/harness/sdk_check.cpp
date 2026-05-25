// harness/sdk_check.cpp -- SDK self-check on boot.
//
// See harness/sdk_check.h for purpose.
//
// Adding a new check: append a row to one of the kClasses[] / kFunctions[]
// / kAssets[] tables below. Severity choices:
//   - CRITICAL: failure means the mod can't function (engine substrate)
//   - IMPORTANT: failure silently disables a major feature (e.g. one NPC
//     class falls through suppression and spawns on both peers)
//   - COSMETIC: visual polish only (text outline / nameplate font)
//
// The single Run() entry point is intentionally one big function with
// linear tables -- the goal is to be transparently "is this name in the
// list?" auditable. Splitting into per-subsystem mini-checks would mean
// modules go out of sync with the actual lookups in their .cpp files.

#include "harness/sdk_check.h"

#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cstdint>

namespace harness::sdk_check {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

enum class Severity { Critical, Important, Cosmetic };

const char* SevTag(Severity s) {
    switch (s) {
        case Severity::Critical:  return "CRITICAL";
        case Severity::Important: return "IMPORTANT";
        case Severity::Cosmetic:  return "COSMETIC";
    }
    return "?";
}

struct ClassCheck {
    const wchar_t* name;
    Severity sev;
    const char* purpose;  // brief tag for log -- "what breaks if this is missing"
};

struct FunctionCheck {
    const wchar_t* className;
    const wchar_t* fnName;
    Severity sev;
    const char* purpose;
};

struct AssetCheck {
    const wchar_t* objectName;
    const wchar_t* className;
    Severity sev;
    const char* purpose;
};

// ---- Class catalog ------------------------------------------------------
// Every CLASS name we look up via R::FindClass at runtime. Failure = the
// subsystem that owns this class is dead (a UFunction lookup on a null
// UClass returns nullptr).
const ClassCheck kClasses[] = {
    // --- Critical engine substrate (mod can't function without these) ---
    {P::name::WorldClass,                    Severity::Critical,  "world context resolution"},
    {P::name::ActorClass,                    Severity::Critical,  "actor spawn + transform UFunctions"},
    {P::name::PawnClassName,                 Severity::Critical,  "controller + pawn dispatch"},
    {P::name::ActorComponentClass,           Severity::Critical,  "component destroy + tick toggle"},
    {P::name::SceneComponentClass,           Severity::Critical,  "component location + visibility"},
    {P::name::KismetSystemLibraryClass,      Severity::Critical,  "ExecuteConsoleCommand (level open)"},
    {P::name::GameplayStaticsClass,          Severity::Critical,  "deferred spawn + LoadGameFromSlot + NPC interceptor"},
    {P::name::GameInstanceClass,             Severity::Critical,  "persistent world context + save registration"},
    {P::name::MainPlayerClass,               Severity::Critical,  "puppet hijack + observer install + autotest"},
    // --- Important: gameplay subsystems (degrade silently if missing) ---
    {P::name::PropClass,                     Severity::Important, "prop_C base -- ALL prop wire sync"},
    {P::name::PropInventoryClass,            Severity::Important, "storage container extract sync"},
    {P::name::PhysicsHandleComponentClass,   Severity::Important, "light-grab observers"},
    {P::name::PhysicsConstraintComponentClass, Severity::Important, "heavy-grab observers"},
    {P::name::PrimitiveComponentClass,       Severity::Important, "throw impulse + velocity capture"},
    {P::name::ControllerClassName,           Severity::Important, "control rotation (camera + head yaw)"},
    {P::name::PlayerControllerClassName,     Severity::Important, "SetViewTargetWithBlend (freecam)"},
    {P::name::PlayerCameraManagerClass,      Severity::Important, "camera location/rotation for nameplate"},
    {P::name::SkinnedMeshComponentClass,     Severity::Important, "puppet mesh + bone queries"},
    {P::name::SkeletalMeshComponentClass,    Severity::Important, "puppet AnimClass"},
    {P::name::WidgetComponentClass,          Severity::Important, "world-space nameplate"},
    {P::name::UserWidgetClass,               Severity::Important, "screen + 3D widgets"},
    {P::name::WidgetTreeClass,               Severity::Important, "UMG widget tree construction"},
    {P::name::TextBlockClass,                Severity::Important, "nameplate + HUD feed text"},
    {P::name::WidgetClass,                   Severity::Important, "UWidget::SetVisibility (input-transparent overlay)"},
    {P::name::KismetTextLibraryClass,        Severity::Important, "Conv_StringToText for widget text"},
    {P::name::KismetStringLibraryClass,      Severity::Important, "Conv_StringToName for wire-Key spawning"},
    {P::name::TimelineComponentClass,        Severity::Important, "autotest Timeline force-play"},
    // --- Cosmetic (visual only) ---
    {P::name::CameraActorClass,              Severity::Cosmetic,  "dev freecam"},
    {P::name::PostProcessComponentClass,     Severity::Cosmetic,  "remote pawn screen-gamma strip"},
};

// ---- Function catalog ---------------------------------------------------
// (Class, Function) pairs we look up via R::FindFunction. We test ONLY the
// resolvers used by shipping code paths -- autotest-only functions aren't
// included (the autotest itself logs its own resolve failures).
const FunctionCheck kFunctions[] = {
    // --- Critical engine UFunctions ---
    {P::name::KismetSystemLibraryClass, P::name::ExecuteConsoleCommandFn,    Severity::Critical, "level open + autotest harness"},
    {P::name::GameplayStaticsClass,     P::name::BeginDeferredSpawnFn,       Severity::Critical, "actor spawn + NPC interceptor"},
    {P::name::GameplayStaticsClass,     P::name::FinishSpawningActorFn,      Severity::Critical, "actor spawn second step"},
    {P::name::GameplayStaticsClass,     P::name::LoadGameFromSlotFn,         Severity::Critical, "STORY save load"},
    {P::name::GameplayStaticsClass,     P::name::SpawnObjectFn,              Severity::Critical, "NewObject (widget tree construction)"},
    {P::name::GameInstanceClass,        P::name::SetSaveSlotObjectFn,        Severity::Critical, "save apply on BeginPlay"},
    {P::name::ActorClass,               P::name::GetActorLocationFn,         Severity::Critical, "pose snapshot read"},
    {P::name::ActorClass,               P::name::SetActorLocationFn,         Severity::Critical, "pose apply (puppet drive)"},
    {P::name::ActorClass,               P::name::GetActorRotationFn,         Severity::Critical, "pose yaw read"},
    {P::name::ActorClass,               P::name::SetActorRotationFn,         Severity::Critical, "pose yaw apply"},
    {P::name::ActorClass,               P::name::GetActorVelocityFn,         Severity::Critical, "AnimBP locomotion speed"},
    {P::name::ActorClass,               P::name::DestroyActorFn,             Severity::Critical, "cross-peer destroy + cleanup"},
    {P::name::PawnClassName,            P::name::GetControllerFn,            Severity::Critical, "controller resolution"},
    // --- Important: gameplay UFunctions ---
    {P::name::ActorClass,               P::name::GetActorForwardVectorFn,    Severity::Important, "autotest hand placement"},
    {P::name::ActorClass,               P::name::SetActorTickEnabledFn,      Severity::Important, "puppet tick gating"},
    {P::name::ActorClass,               P::name::TeleportToFn,               Severity::Important, "client KPP teleport + autotest"},
    {P::name::ActorClass,               P::name::GetActorBoundsFn,           Severity::Important, "puppet visible-feet derivation"},
    {P::name::ActorClass,               P::name::AddComponentByClassFn,      Severity::Important, "world-space nameplate construction"},
    {P::name::ActorClass,               P::name::FinishAddComponentFn,       Severity::Important, "nameplate registration"},
    {P::name::PawnClassName,            P::name::DetachFromControllerFn,     Severity::Important, "puppet controller cleanup"},
    {P::name::PawnClassName,            P::name::SpawnDefaultControllerFn,   Severity::Important, "satellite AI controller spawn"},
    {P::name::SceneComponentClass,      P::name::SetVisibilityFn,            Severity::Important, "component visibility (puppet body shown)"},
    {P::name::SceneComponentClass,      P::name::SetHiddenInGameFn,          Severity::Important, "component hide (editor arrow strip)"},
    {P::name::SceneComponentClass,      P::name::GetComponentLocationFn,     Severity::Important, "puppet head-bone world Z"},
    {P::name::SceneComponentClass,      P::name::GetComponentForwardFn,      Severity::Important, "camera-forward target"},
    {P::name::SceneComponentClass,      P::name::GetSocketLocationFn,        Severity::Important, "bone-anchored nameplate"},
    {P::name::ActorComponentClass,      P::name::DestroyComponentFn,         Severity::Important, "remote pawn local-only system strip"},
    {P::name::ActorComponentClass,      P::name::SetComponentTickEnabledFn,  Severity::Important, "satellite CMC park + widget tick"},
    {P::name::SkinnedMeshComponentClass, P::name::SetSkeletalMeshFn,         Severity::Important, "puppet skin"},
    {P::name::SkinnedMeshComponentClass, P::name::GetNumBonesFn,             Severity::Important, "bone enumeration (head/foot find)"},
    {P::name::SkinnedMeshComponentClass, P::name::GetBoneNameFn,             Severity::Important, "bone enumeration"},
    {P::name::SkeletalMeshComponentClass, P::name::SetAnimClassFn,           Severity::Important, "puppet AnimBP"},
    {P::name::ControllerClassName,      P::name::GetControlRotationFn,       Severity::Important, "head pitch + camera-yaw lead"},
    {P::name::PlayerControllerClassName, P::name::SetViewTargetWithBlendFn,  Severity::Important, "dev freecam"},
    {P::name::PlayerCameraManagerClass, P::name::GetCameraLocationFn,        Severity::Important, "autotest hand placement"},
    {P::name::PlayerCameraManagerClass, P::name::GetCameraRotationFn,        Severity::Important, "autotest camera tilt"},
    {P::name::PropClass,                P::name::PropInitFn,                 Severity::Important, "Aprop_C::Init POST observer (spawn detector)"},
    {P::name::PropInventoryClass,       P::name::PropInventoryTakeObjFn,     Severity::Important, "storage extract observer"},
    {P::name::PhysicsHandleComponentClass, P::name::GrabComponentAtLocationFn,    Severity::Important, "grab observer (light)"},
    {P::name::PhysicsHandleComponentClass, P::name::SetTargetLocationFn,          Severity::Important, "per-tick grab driver observer"},
    {P::name::PhysicsHandleComponentClass, P::name::ReleaseComponentFn,           Severity::Important, "grab release observer + before-destroy cleanup"},
    {P::name::PhysicsConstraintComponentClass, P::name::SetConstrainedComponentsFn, Severity::Important, "heavy grab observer"},
    {P::name::PhysicsConstraintComponentClass, P::name::BreakConstraintFn,        Severity::Important, "heavy grab release observer"},
    {P::name::PrimitiveComponentClass,  P::name::AddImpulseFn,               Severity::Important, "throw impulse observer"},
    {P::name::PrimitiveComponentClass,  P::name::SetSimulatePhysicsFn,       Severity::Important, "receiver prop kinematic toggle"},
    {P::name::PrimitiveComponentClass,  P::name::GetPhysicsLinearVelocityFn, Severity::Important, "throw velocity capture"},
    {P::name::PrimitiveComponentClass,  P::name::SetCollisionEnabledFn,      Severity::Important, "mushroom collision restore (wire-converged props)"},
    {P::name::TextBlockClass,           P::name::NameplateSetTextFn,         Severity::Important, "nameplate + HUD text"},
    {P::name::KismetTextLibraryClass,   P::name::ConvStringToTextFn,         Severity::Important, "FString -> FText for widget text"},
    {P::name::KismetStringLibraryClass, P::name::ConvStringToNameFn,         Severity::Important, "wire-Key -> live FName for prop spawn"},
    {P::name::UserWidgetClass,          P::name::AddToViewportFn,            Severity::Important, "HUD feed + dev HUD attach"},
    {P::name::UserWidgetClass,          P::name::RemoveFromViewportFn,       Severity::Important, "HUD feed detach"},
    {P::name::WidgetClass,              P::name::WidgetSetVisibilityFn,      Severity::Important, "HUD overlay input-transparent"},
    {P::name::WidgetComponentClass,     P::name::SetWidgetFn,                Severity::Important, "nameplate widget attach"},
    // --- BP-Timeline observers on mainPlayer_C (K2Node ordinal embedded!) ---
    {P::name::MainPlayerClass,          P::name::MainPlayerUseInputEventFn,  Severity::Important, "E-press observer (K2Node ordinal embedded -- BP recook breaks)"},
    {P::name::MainPlayerClass,          P::name::MainPlayerGrabUpdateFn,     Severity::Important, "grab Timeline tick observer"},
    {P::name::MainPlayerClass,          P::name::MainPlayerGrabFinishedFn,   Severity::Important, "grab Timeline end observer"},
};

// ---- Phase 5N1 NPC class allowlist --------------------------------------
// Checked separately because the allowlist is a single array; missing entries
// = those NPC types silently spawn on both peers (suppression no-op).

// ---- Asset catalog ------------------------------------------------------
const AssetCheck kAssets[] = {
    {P::name::Widget3DTranslucentMatName,         P::name::MaterialInstanceConstantClass, Severity::Important, "nameplate transparent material (2-sided)"},
    {P::name::Widget3DTranslucentOneSidedMatName, P::name::MaterialInstanceConstantClass, Severity::Important, "nameplate transparent material (1-sided)"},
    {P::name::FontName,                           P::name::FontClassName,                 Severity::Cosmetic,  "Roboto font for widgets (UMG falls back if missing)"},
};

void RunClassChecks(int& ok, int& fail, int& failPriority) {
    UE_LOGI("sdk-check: --- CLASS resolution ---");
    for (const auto& c : kClasses) {
        void* cls = R::FindClass(c.name);
        if (cls) {
            ++ok;
        } else {
            ++fail;
            if (c.sev != Severity::Cosmetic) ++failPriority;
            UE_LOGW("sdk-check[%s]: CLASS '%ls' NOT FOUND -- breaks %s",
                    SevTag(c.sev), c.name, c.purpose);
        }
    }
}

void RunFunctionChecks(int& ok, int& fail, int& failPriority) {
    UE_LOGI("sdk-check: --- UFUNCTION resolution ---");
    for (const auto& f : kFunctions) {
        void* cls = R::FindClass(f.className);
        if (!cls) {
            // Already counted as a class failure above; skip the function
            // (we'd get a noisy second log line for every fn on the dead class).
            continue;
        }
        void* fn = R::FindFunction(cls, f.fnName);
        if (fn) {
            ++ok;
        } else {
            ++fail;
            if (f.sev != Severity::Cosmetic) ++failPriority;
            UE_LOGW("sdk-check[%s]: UFUNCTION '%ls.%ls' NOT FOUND -- breaks %s",
                    SevTag(f.sev), f.className, f.fnName, f.purpose);
        }
    }
}

void RunNpcAllowlistCheck(int& ok, int& fail, int& failPriority) {
    UE_LOGI("sdk-check: --- Phase 5N1 NPC ALLOWLIST (12 classes) ---");
    int npcOk = 0;
    for (size_t i = 0; i < P::name::kNpcAllowlistSize; ++i) {
        if (R::FindClass(P::name::kNpcAllowlist[i])) {
            ++ok; ++npcOk;
        } else {
            ++fail; ++failPriority;
            UE_LOGW("sdk-check[IMPORTANT]: NPC CLASS '%ls' NOT FOUND -- this NPC will spawn on both peers (suppression no-op)",
                    P::name::kNpcAllowlist[i]);
        }
    }
    UE_LOGI("sdk-check: NPC allowlist: %d/%zu resolved", npcOk, P::name::kNpcAllowlistSize);
}

void RunAssetChecks(int& ok, int& fail, int& failPriority) {
    UE_LOGI("sdk-check: --- ASSET resolution ---");
    for (const auto& a : kAssets) {
        void* obj = R::FindObject(a.objectName, a.className);
        if (obj) {
            ++ok;
        } else {
            ++fail;
            if (a.sev != Severity::Cosmetic) ++failPriority;
            UE_LOGW("sdk-check[%s]: ASSET '%ls' (class '%ls') NOT FOUND -- breaks %s",
                    SevTag(a.sev), a.objectName, a.className, a.purpose);
        }
    }
}

}  // namespace

int Run() {
    UE_LOGI("sdk-check: === self-check START (target VOTV %s, engine %s) ===",
            P::kTargetGameVersion, P::kTargetEngineVersion);
    int ok = 0, fail = 0, failPriority = 0;
    RunClassChecks(ok, fail, failPriority);
    RunFunctionChecks(ok, fail, failPriority);
    RunNpcAllowlistCheck(ok, fail, failPriority);
    RunAssetChecks(ok, fail, failPriority);
    const int total = ok + fail;
    if (failPriority == 0 && fail == 0) {
        UE_LOGI("sdk-check: === HEALTHY %d/%d resolved (no failures) ===", ok, total);
    } else if (failPriority == 0) {
        UE_LOGI("sdk-check: === HEALTHY %d/%d resolved (%d cosmetic failure(s) only) ===",
                ok, total, fail);
    } else {
        UE_LOGE("sdk-check: === DEGRADED %d/%d resolved (%d total failures, %d CRITICAL+IMPORTANT) -- see WARN lines above ===",
                ok, total, fail, failPriority);
        UE_LOGE("sdk-check: VOTV build likely diverged from target (%s). Re-run UE4SS SDK dump + diff sdk_profile.h.",
                P::kTargetGameVersion);
    }
    return failPriority;
}

}  // namespace harness::sdk_check
