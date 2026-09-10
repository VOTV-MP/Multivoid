// ue_wrap/devices/serverbox.cpp -- see ue_wrap/devices/serverbox.h.

#include "ue_wrap/devices/serverbox.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <chrono>

namespace ue_wrap::serverbox {
namespace {

namespace R = reflection;
namespace P = profile;

using field_io::TArrayView;
using field_io::ReadFStringAt;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

int32_t g_offName   = -1;
void*   g_fnProcess = nullptr;  // pocessFloppy
void*   g_fnEject   = nullptr;  // ejectFloppy
bool     g_resolved = false;
uint64_t g_nextTryMs = 0;

// The box list resolves on its own: the break-and-fix lane wants the boxes and nothing else, so a
// slot field this wrapper cannot find must not cost it the list.
int32_t  g_offServers = -1;  // mainGamemode_C.servers
uint64_t g_nextServersTryMs = 0;

bool EnsureServersResolved() {
    if (g_offServers >= 0) return true;
    const uint64_t now = NowMs();
    if (now < g_nextServersTryMs) return false;
    g_nextServersTryMs = now + 1000;
    void* gmCls = R::FindClass(P::name::GamemodeClass);
    if (!gmCls) return false;
    g_offServers = R::FindPropertyOffset(gmCls, L"servers");
    if (g_offServers < 0) return false;
    UE_LOGI("serverbox: server list resolved (servers=0x%X)", g_offServers);
    return true;
}

// The live gamemode, revalidated by internal index (recycled-slot-safe).
void*   g_gm    = nullptr;
int32_t g_gmIdx = -1;

void* Gamemode() {
    if (!g_gm || !R::IsLiveByIndex(g_gm, g_gmIdx)) {
        g_gm = R::FindObjectByClass(P::name::GamemodeClass);
        g_gmIdx = g_gm ? R::InternalIndexOf(g_gm) : -1;
    }
    return g_gm;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved) return true;
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return false;
    g_nextTryMs = now + 1000;

    void* cls = R::FindClass(L"serverBox_C");
    if (!cls) return false;  // world not loaded yet

    g_offName   = R::FindPropertyOffset(cls, L"name");
    g_fnProcess = R::FindFunction(cls, L"pocessFloppy");
    g_fnEject   = R::FindFunction(cls, L"ejectFloppy");

    // No offset fallbacks: an unresolved member here means the class is not the one this wrapper
    // was written against, and a guessed offset would write into whatever now lives there.
    if (g_offName < 0 || !g_fnProcess || !g_fnEject) {
        UE_LOGW("serverbox: resolution incomplete (name=%d pocessFloppy=%p ejectFloppy=%p) -- "
                "the box's verbs stay off", g_offName, g_fnProcess, g_fnEject);
        return false;
    }
    g_resolved = true;
    UE_LOGI("serverbox: resolved (name=0x%X insert=%p eject=%p)", g_offName, g_fnProcess,
            g_fnEject);
    return true;
}

size_t ReadServers(std::vector<void*>& out) {
    if (!EnsureServersResolved()) return 0;
    void* gm = Gamemode();
    if (!gm) return 0;
    const auto* arr = reinterpret_cast<const TArrayView*>(
        reinterpret_cast<const uint8_t*>(gm) + g_offServers);
    if (!arr->data || arr->num <= 0) return 0;
    void* const* elems = reinterpret_cast<void* const*>(arr->data);
    const size_t before = out.size();
    for (int32_t i = 0; i < arr->num; ++i) out.push_back(elems[i]);
    return out.size() - before;
}

std::wstring ReadName(void* box) {
    if (!box || !g_resolved) return std::wstring();
    return ReadFStringAt(box, g_offName);
}

bool CallProcessFloppy(void* box, void* discActor) {
    if (!box || !discActor || !g_resolved) return false;
    ParamFrame f(g_fnProcess);
    if (!f.valid()) return false;
    if (!f.Set(L"Object", discActor)) return false;
    return Call(box, f);
}

bool CallEjectFloppy(void* box) {
    if (!box || !g_resolved) return false;
    ParamFrame f(g_fnEject);
    if (!f.valid()) return false;
    return Call(box, f);
}

}  // namespace ue_wrap::serverbox
