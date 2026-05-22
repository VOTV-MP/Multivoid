#include "ue_wrap/engine.h"

#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cstring>
#include <string>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

// UKismetSystemLibrary::ExecuteConsoleCommand(WorldContextObject, Command,
// SpecificPlayer) parameter frame (UE4.27 x64 ABI). Layout is fixed by the
// function's UProperties: object ptr, then FString (16B), then object ptr.
#pragma pack(push, 1)
struct ExecuteConsoleCommandParams {
    void* WorldContextObject;   // 0x00
    R::FString Command;         // 0x08  (Data ptr / Num / Max)
    void* SpecificPlayer;       // 0x18  (nullptr = the first local player)
};                              // size 0x20
#pragma pack(pop)
static_assert(sizeof(ExecuteConsoleCommandParams) == 0x20, "param frame layout");

// Resolved once (the CDO + UFunction never move; the GameInstance persists for
// the process lifetime, so caching its pointer is safe across level loads).
void* g_kslCdo = nullptr;
void* g_execFn = nullptr;
void* g_worldContext = nullptr;

void* ResolveWorldContext() {
    // The GameInstance persists across level loads and is a valid world context.
    if (void* gi = R::FindObjectByClass(P::name::GameInstanceClass)) return gi;
    // Fall back to any live World (e.g. before the GameInstance is up).
    return R::FindObjectByClass(P::name::WorldClass);
}

bool Resolve() {
    if (!g_kslCdo) g_kslCdo = R::FindClassDefaultObject(P::name::KismetSystemLibraryClass);
    if (g_kslCdo && !g_execFn) {
        if (void* cls = R::ClassOf(g_kslCdo)) {
            g_execFn = R::FindFunction(cls, P::name::ExecuteConsoleCommandFn);
        }
    }
    // World context can become available later than the CDO; re-resolve until found.
    if (!g_worldContext) g_worldContext = ResolveWorldContext();
    return g_kslCdo && g_execFn && g_worldContext;
}

}  // namespace

bool ExecuteConsoleCommand(const wchar_t* command) {
    if (!command) return false;
    if (!Resolve()) {
        UE_LOGE("engine: ExecuteConsoleCommand unresolved (cdo=%p fn=%p world=%p)",
                g_kslCdo, g_execFn, g_worldContext);
        return false;
    }

    // The command FString. ExecuteConsoleCommand takes a const FString& and only
    // reads it (it forwards to GEngine->Exec); it does not take ownership, so a
    // local buffer is correct -- nothing frees it. UE's FString::Num counts the
    // null terminator.
    std::wstring buf(command);
    R::FString cmd{};
    cmd.Data = buf.data();
    cmd.Num = static_cast<int32_t>(buf.size()) + 1;
    cmd.Max = cmd.Num;

    ExecuteConsoleCommandParams params{};
    params.WorldContextObject = g_worldContext;
    params.Command = cmd;
    params.SpecificPlayer = nullptr;

    const bool ok = R::CallFunction(g_kslCdo, g_execFn, &params);
    if (ok) {
        UE_LOGI("engine: console command issued: %ls", command);
    } else {
        UE_LOGE("engine: CallFunction failed for console command: %ls", command);
    }
    return ok;
}

}  // namespace ue_wrap::engine
