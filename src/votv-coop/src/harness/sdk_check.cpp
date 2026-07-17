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

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <string>

namespace harness::sdk_check {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// ---- compat_report.txt accumulator -------------------------------------
// Each check function appends its findings here in addition to the live
// log stream. After all checks run, the buffer is written to a single
// shareable file beside the mod DLL. Users can paste it verbatim into a
// GitHub issue when sdk-check reports DEGRADED post-VOTV-update.
std::ostringstream g_report;

void ReportHeader() {
    g_report.str({});
    g_report.clear();

    // Local time stamp.
    char ts[32] = {};
    {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    }
    // Game exe identification: walk the running process module list to
    // pick out VotV-Win64-Shipping.exe (or whatever the exe is named).
    // The HMODULE for the main exe is GetModuleHandle(nullptr).
    char exePath[MAX_PATH] = {};
    ::GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    uint64_t exeSize = 0;
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (::GetFileAttributesExA(exePath, GetFileExInfoStandard, &fad)) {
            exeSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        }
    }

    g_report << "=== VOTV_MP coop mod -- SDK compatibility report ===\n";
    g_report << "Generated:        " << ts << "\n";
    g_report << "Mod target VOTV:  " << P::kTargetGameVersion << "\n";
    g_report << "Mod target UE:    " << P::kTargetEngineVersion << "\n";
    g_report << "Running game exe: " << exePath << "\n";
    g_report << "  size:           " << exeSize << " bytes\n";
    g_report << "  expected size:  " << P::kExpectedExeSize << " bytes\n";
    if (P::kExpectedExeSize != 0 && exeSize != P::kExpectedExeSize) {
        g_report << "  *** SIZE MISMATCH: game build differs from the version this mod was authored for.\n";
    }
    g_report << "\n";
}

void ReportLine(const char* line) {
    g_report << line << "\n";
}

void ReportFooter(int ok, int total, int fail, int failPriority, int skipped) {
    g_report << "\n=== SUMMARY ===\n";
    g_report << "  resolved:           " << ok << " / " << total << "\n";
    g_report << "  total failures:     " << fail << "\n";
    g_report << "  CRITICAL+IMPORTANT: " << failPriority << "\n";
    if (skipped > 0) {
        g_report << "  skipped (dead cls): " << skipped
                 << "  (UFunction checks whose owning UClass already failed above)\n";
    }
    if (failPriority == 0 && fail == 0) {
        g_report << "Status: HEALTHY (every checked name resolved against the live build)\n";
    } else if (failPriority == 0) {
        g_report << "Status: HEALTHY (only cosmetic failures; gameplay subsystems intact)\n";
    } else {
        g_report << "Status: DEGRADED -- VOTV likely diverged from the target build.\n\n";
        g_report << "Share this report when filing an issue at the mod repository -- it identifies\n";
        g_report << "the divergence completely. The maintainer will produce an updated SDK profile\n";
        g_report << "and a new build targeted at the current VOTV release.\n";
        g_report << "Baseline this mod targets: " << P::kTargetGameVersion << "\n";
    }
}

// Find the directory containing the mod DLL so the report lands next to it
// (same convention as votv-coop.log, votv-coop-loaded.txt, scenario.txt).
std::wstring ModuleDir() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? L"." : p.substr(0, sep);
}

void WriteReportFile() {
    const std::wstring path = ModuleDir() + L"\\votv-coop-compat-report.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) {
        UE_LOGW("sdk-check: failed to open compat report '%ls' for writing", path.c_str());
        return;
    }
    const std::string content = g_report.str();
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    UE_LOGI("sdk-check: wrote compat report '%ls' (%zu bytes)", path.c_str(), content.size());
}

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
    {P::name::GamemodeClass,                 Severity::Important, "puppet spawn world-context lookup (puppet.cpp)"},
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
    {P::name::PropClass,                P::name::PropSetKeyFn,               Severity::Critical,  "wire-Key apply before Init -- prop identity (without this, every receiver-spawned prop has wrong key + tracking lost)"},
    {P::name::PropClass,                P::name::PropThrownFn,               Severity::Important, "prop throw sound + particle effects on receiver"},
    {P::name::PropInventoryClass,       P::name::PropInventoryTakeObjFn,     Severity::Important, "storage extract observer"},
    {P::name::PhysicsHandleComponentClass, P::name::GrabComponentAtLocationFn,    Severity::Important, "grab observer (light)"},
    {P::name::PhysicsHandleComponentClass, P::name::SetTargetLocationFn,          Severity::Important, "per-tick grab driver observer"},
    {P::name::PhysicsHandleComponentClass, P::name::ReleaseComponentFn,           Severity::Important, "grab release observer + before-destroy cleanup"},
    {P::name::PhysicsConstraintComponentClass, P::name::SetConstrainedComponentsFn, Severity::Important, "heavy grab observer"},
    {P::name::PhysicsConstraintComponentClass, P::name::BreakConstraintFn,        Severity::Important, "heavy grab release observer"},
    {P::name::PrimitiveComponentClass,  P::name::AddImpulseFn,               Severity::Important, "throw impulse observer"},
    {P::name::PrimitiveComponentClass,  P::name::SetSimulatePhysicsFn,       Severity::Important, "receiver prop kinematic toggle"},
    {P::name::PrimitiveComponentClass,  P::name::GetPhysicsLinearVelocityFn, Severity::Important, "throw velocity capture (host-side release)"},
    {P::name::PrimitiveComponentClass,  P::name::SetPhysicsLinearVelocityFn, Severity::Important, "receiver prop re-entry linear velocity (throw sync)"},
    {P::name::PrimitiveComponentClass,  P::name::SetPhysicsAngularVelocityInDegreesFn, Severity::Important, "receiver prop re-entry angular velocity (spin sync)"},
    {P::name::PrimitiveComponentClass,  P::name::SetCollisionEnabledFn,      Severity::Important, "mushroom collision restore (wire-converged props)"},
    {P::name::TextBlockClass,           P::name::NameplateSetTextFn,         Severity::Important, "nameplate + HUD text"},
    {P::name::KismetTextLibraryClass,   P::name::ConvStringToTextFn,         Severity::Important, "FString -> FText for widget text"},
    {P::name::KismetStringLibraryClass, P::name::ConvStringToNameFn,         Severity::Important, "wire-Key -> live FName for prop spawn"},
    {P::name::UserWidgetClass,          P::name::AddToViewportFn,            Severity::Important, "HUD feed + dev HUD attach"},
    {P::name::UserWidgetClass,          P::name::RemoveFromViewportFn,       Severity::Important, "HUD feed detach"},
    {P::name::WidgetClass,              P::name::WidgetSetVisibilityFn,      Severity::Important, "HUD overlay input-transparent"},
    {P::name::WidgetComponentClass,     P::name::SetWidgetFn,                Severity::Important, "nameplate widget attach"},
    {P::name::WidgetComponentClass,     P::name::SetTintColorAndOpacityFn,   Severity::Important, "nameplate tint (full opacity)"},
    {P::name::WidgetComponentClass,     P::name::RequestRedrawFn,            Severity::Important, "nameplate per-tick redraw"},
    {P::name::WidgetComponentClass,     P::name::RequestRenderUpdateFn,      Severity::Important, "nameplate render-target refresh"},
    {P::name::UserWidgetClass,          P::name::SetPositionInViewportFn,    Severity::Important, "HUD feed + dev HUD positioning"},
    {P::name::UserWidgetClass,          P::name::SetAlignmentInViewportFn,   Severity::Important, "HUD feed + dev HUD alignment"},
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

// Helper: convert a UTF-16 string to a flat narrow string for the report
// (we only have ASCII names so this lossless-narrows). Newlines stripped.
std::string Narrow(const wchar_t* w) {
    std::string out;
    if (!w) return out;
    for (; *w; ++w) out.push_back(static_cast<char>(*w < 128 ? *w : '?'));
    return out;
}

void RunClassChecks(int& ok, int& fail, int& failPriority) {
    UE_LOGI("sdk-check: --- CLASS resolution ---");
    g_report << "--- CLASS resolution ---\n";
    for (const auto& c : kClasses) {
        void* cls = R::FindClass(c.name);
        if (cls) {
            ++ok;
        } else {
            ++fail;
            if (c.sev != Severity::Cosmetic) ++failPriority;
            UE_LOGW("sdk-check[%s]: CLASS '%ls' NOT FOUND -- breaks %s",
                    SevTag(c.sev), c.name, c.purpose);
            char buf[512];
            std::snprintf(buf, sizeof(buf), "  [%s] CLASS '%s' NOT FOUND -- breaks %s",
                          SevTag(c.sev), Narrow(c.name).c_str(), c.purpose);
            ReportLine(buf);
        }
    }
}

void RunFunctionChecks(int& ok, int& fail, int& failPriority, int& skipped) {
    UE_LOGI("sdk-check: --- UFUNCTION resolution ---");
    g_report << "--- UFUNCTION resolution ---\n";
    for (const auto& f : kFunctions) {
        void* cls = R::FindClass(f.className);
        if (!cls) {
            // Class already counted as a CLASS failure (RunClassChecks); to
            // avoid noisy double-counts the function attempt is SKIPPED here,
            // but tracked in the `skipped` counter so the SUMMARY reflects the
            // true total. Audit fix 2026-05-25: previously skipped entries
            // silently vanished from `total = ok + fail`, making "resolved: 45/47"
            // misleading when 23 function entries were skipped on a dead class.
            ++skipped;
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
            char buf[512];
            std::snprintf(buf, sizeof(buf), "  [%s] UFUNCTION '%s.%s' NOT FOUND -- breaks %s",
                          SevTag(f.sev), Narrow(f.className).c_str(), Narrow(f.fnName).c_str(), f.purpose);
            ReportLine(buf);
        }
    }
}

void RunNpcAllowlistCheck(int& ok, int& fail, int& failPriority) {
    UE_LOGI("sdk-check: --- Phase 5N1 NPC ALLOWLIST (12 classes) ---");
    g_report << "--- Phase 5N1 NPC ALLOWLIST (12 classes) ---\n";
    constexpr Severity kNpcSev = Severity::Important;
    int npcOk = 0;
    for (size_t i = 0; i < P::name::kNpcAllowlistSize; ++i) {
        if (R::FindClass(P::name::kNpcAllowlist[i])) {
            ++ok; ++npcOk;
        } else {
            ++fail; ++failPriority;
            UE_LOGW("sdk-check[%s]: NPC CLASS '%ls' NOT FOUND -- this NPC will spawn on both peers (suppression no-op)",
                    SevTag(kNpcSev), P::name::kNpcAllowlist[i]);
            char buf[512];
            std::snprintf(buf, sizeof(buf), "  [%s] NPC CLASS '%s' NOT FOUND -- this NPC will spawn on both peers (suppression no-op)",
                          SevTag(kNpcSev), Narrow(P::name::kNpcAllowlist[i]).c_str());
            ReportLine(buf);
        }
    }
    UE_LOGI("sdk-check: NPC allowlist: %d/%zu resolved", npcOk, P::name::kNpcAllowlistSize);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "  NPC allowlist: %d / %zu resolved", npcOk, P::name::kNpcAllowlistSize);
    ReportLine(buf);
}

void RunAssetChecks(int& ok, int& fail, int& failPriority) {
    UE_LOGI("sdk-check: --- ASSET resolution ---");
    g_report << "--- ASSET resolution ---\n";
    for (const auto& a : kAssets) {
        void* obj = R::FindObject(a.objectName, a.className);
        if (obj) {
            ++ok;
        } else {
            ++fail;
            if (a.sev != Severity::Cosmetic) ++failPriority;
            UE_LOGW("sdk-check[%s]: ASSET '%ls' (class '%ls') NOT FOUND -- breaks %s",
                    SevTag(a.sev), a.objectName, a.className, a.purpose);
            char buf[512];
            std::snprintf(buf, sizeof(buf), "  [%s] ASSET '%s' (class '%s') NOT FOUND -- breaks %s",
                          SevTag(a.sev), Narrow(a.objectName).c_str(), Narrow(a.className).c_str(), a.purpose);
            ReportLine(buf);
        }
    }
}

}  // namespace

int Run() {
    UE_LOGI("sdk-check: === self-check START (target VOTV %s, engine %s) ===",
            P::kTargetGameVersion, P::kTargetEngineVersion);
    ReportHeader();
    int ok = 0, fail = 0, failPriority = 0, skipped = 0;
    RunClassChecks(ok, fail, failPriority);
    RunFunctionChecks(ok, fail, failPriority, skipped);
    RunNpcAllowlistCheck(ok, fail, failPriority);
    RunAssetChecks(ok, fail, failPriority);
    const int total = ok + fail;
    if (failPriority == 0 && fail == 0) {
        UE_LOGI("sdk-check: === HEALTHY %d/%d resolved (no failures) ===", ok, total);
    } else if (failPriority == 0) {
        UE_LOGI("sdk-check: === HEALTHY %d/%d resolved (%d cosmetic failure(s) only) ===",
                ok, total, fail);
    } else {
        UE_LOGE("sdk-check: === DEGRADED %d/%d resolved (%d total failures, %d CRITICAL+IMPORTANT%s) -- see WARN lines above ===",
                ok, total, fail, failPriority,
                skipped > 0 ? "; some UFunction checks skipped due to dead owning UClass" : "");
        UE_LOGE("sdk-check: VOTV build likely diverged from target (%s). Re-run UE4SS SDK dump + diff sdk_profile.h.",
                P::kTargetGameVersion);
        UE_LOGE("sdk-check: see votv-coop-compat-report.txt next to the mod DLL for the shareable diff.");
    }
    ReportFooter(ok, total, fail, failPriority, skipped);
    WriteReportFile();
    return failPriority;
}

}  // namespace harness::sdk_check
