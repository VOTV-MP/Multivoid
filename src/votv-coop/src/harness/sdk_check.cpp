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

#include "harness/compat_report.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/paths.h"
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
// Each check function appends its findings here as well as to the live log stream. After every
// check has run the buffer is written to a single shareable file beside the mod DLL, so a player
// can paste it whole into a GitHub issue when sdk-check reports DEGRADED after a game update.
std::ostringstream g_report;

using Severity = harness::compat_report::Severity;
int g_failures[3] = {};  // by Severity, for the compat report's verdict

void Failed(Severity s) { ++g_failures[static_cast<int>(s)]; }

void ReportHeader() {
    g_report.str({});
    g_report.clear();
    g_failures[0] = g_failures[1] = g_failures[2] = 0;

    // Local time stamp.
    char ts[32] = {};
    {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    }
    // The running exe's path; which build it is, the report's game-build section says.
    char exePath[MAX_PATH] = {};
    ::GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    g_report << "=== VOTV_MP coop mod -- SDK compatibility report ===\n";
    g_report << "Generated:        " << ts << "\n";
    g_report << "Mod target VOTV:  " << P::kTargetBuild << "\n";
    g_report << "Mod target UE:    " << P::kTargetEngineVersion << "\n";
    g_report << "Running game exe: " << exePath << "\n";
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
}

// The section is this check's, and so are its failures; the file beside the exe and its one verdict are
// harness/compat_report's, which the fingerprint census writes its own section into.
void WriteReportFile() {
    namespace CR = harness::compat_report;
    CR::Begin(kReportSection);
    std::istringstream in(g_report.str());
    for (std::string line; std::getline(in, line);) CR::Line("sdk-check", line);
    for (int sev = 0; sev < 3; ++sev) CR::Failure(kReportSection, static_cast<Severity>(sev), g_failures[sev]);
    CR::Write();
}

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
// The functions the mod CALLS by name. A function it watches or hooks is a facts row instead, which the
// fingerprint census resolves and fingerprints (harness/fingerprint_census): one function, one checker.
const FunctionCheck kFunctions[] = {
    // --- Critical engine UFunctions ---
    {P::name::KismetSystemLibraryClass, P::name::ExecuteConsoleCommandFn,    Severity::Critical, "level open + autotest harness"},
    {P::name::GameplayStaticsClass,     P::name::LoadGameFromSlotFn,         Severity::Critical, "STORY save load"},
    {P::name::GameplayStaticsClass,     P::name::SpawnObjectFn,              Severity::Critical, "NewObject (widget tree construction)"},
    {P::name::GameInstanceClass,        P::name::SetSaveSlotObjectFn,        Severity::Critical, "save apply on BeginPlay"},
    {P::name::ActorClass,               P::name::GetActorLocationFn,         Severity::Critical, "pose snapshot read"},
    {P::name::ActorClass,               P::name::SetActorLocationFn,         Severity::Critical, "pose apply (puppet drive)"},
    {P::name::ActorClass,               P::name::GetActorRotationFn,         Severity::Critical, "pose yaw read"},
    {P::name::ActorClass,               P::name::SetActorRotationFn,         Severity::Critical, "pose yaw apply"},
    {P::name::PawnClassName,            P::name::GetControllerFn,            Severity::Critical, "controller resolution"},
    // --- Important: gameplay UFunctions ---
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
    {P::name::PropClass,                P::name::PropInitFn,                 Severity::Important, "Aprop_C::Init, the body the Init watch expresses a birth after"},
    {P::name::PropClass,                P::name::PropSetKeyFn,               Severity::Critical,  "wire-Key apply before Init -- prop identity (without this, every receiver-spawned prop has wrong key + tracking lost)"},
    {P::name::PropClass,                P::name::PropThrownFn,               Severity::Important, "prop throw sound + particle effects on receiver"},
    {P::name::PrimitiveComponentClass,  P::name::SetSimulatePhysicsFn,       Severity::Important, "receiver prop kinematic toggle"},
    {P::name::PrimitiveComponentClass,  P::name::GetPhysicsLinearVelocityFn, Severity::Important, "throw velocity capture (host-side release)"},
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
};

// ---- Asset catalog ------------------------------------------------------
const AssetCheck kAssets[] = {
    {P::name::Widget3DTranslucentMatName,         P::name::MaterialInstanceConstantClass, Severity::Important, "nameplate transparent material (2-sided)"},
    {P::name::Widget3DTranslucentOneSidedMatName, P::name::MaterialInstanceConstantClass, Severity::Important, "nameplate transparent material (1-sided)"},
    {P::name::FontName,                           P::name::FontClassName,                 Severity::Cosmetic,  "Roboto font for widgets (UMG falls back if missing)"},
};

// The checks run on a player's launch, in the world's first frames, so each lookup is the object index's: a
// class in one lookup, an asset among its own class's instances. The finders' first lookup of a class, and
// every asset lookup, walked the whole object array rendering a name per object -- 93 ms in one frame, measured.
void* FindLoadedClass(const wchar_t* name) { return ue_wrap::object_index::ClassByName(name); }

struct AssetQuery {
    const wchar_t* name;
    void*          found;
};

void* FindAsset(const wchar_t* objectName, const wchar_t* className) {
    void* cls = FindLoadedClass(className);
    if (!cls) return nullptr;
    AssetQuery q{objectName, nullptr};
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        auto* query = static_cast<AssetQuery*>(ctx);
        if (!query->found && R::NameEquals(R::NameOf(obj), query->name)) query->found = obj;
    }, &q);
    return q.found;
}

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
        void* cls = FindLoadedClass(c.name);
        if (cls) {
            ++ok;
        } else {
            ++fail;
            Failed(c.sev);
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
        void* cls = FindLoadedClass(f.className);
        if (!cls) {
            // The class was already counted as a CLASS failure (RunClassChecks). To avoid a noisy
            // double-count the function attempt is SKIPPED here, but it is tracked in the `skipped`
            // counter so the SUMMARY reflects the true total: without it, skipped entries vanish
            // from `total = ok + fail` and a line like "resolved: 45/47" reads as healthy while
            // twenty-odd entries were never attempted.
            ++skipped;
            continue;
        }
        void* fn = R::FindFunction(cls, f.fnName);
        if (fn) {
            ++ok;
        } else {
            ++fail;
            Failed(f.sev);
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
    UE_LOGI("sdk-check: --- NPC ALLOWLIST (%zu classes) ---", P::name::kNpcAllowlistSize);
    g_report << "--- NPC ALLOWLIST (" << P::name::kNpcAllowlistSize << " classes) ---\n";
    constexpr Severity kNpcSev = Severity::Important;
    int npcOk = 0;
    for (size_t i = 0; i < P::name::kNpcAllowlistSize; ++i) {
        if (FindLoadedClass(P::name::kNpcAllowlist[i])) {
            ++ok; ++npcOk;
        } else {
            ++fail; ++failPriority;
            Failed(kNpcSev);
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
        void* obj = FindAsset(a.objectName, a.className);
        if (obj) {
            ++ok;
        } else {
            ++fail;
            Failed(a.sev);
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
            P::kTargetBuild, P::kTargetEngineVersion);
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
                P::kTargetBuild);
        UE_LOGE("sdk-check: see multivoid-compat-report.txt next to the mod DLL for the shareable diff.");
    }
    ReportFooter(ok, total, fail, failPriority, skipped);
    WriteReportFile();
    return failPriority;
}

}  // namespace harness::sdk_check
