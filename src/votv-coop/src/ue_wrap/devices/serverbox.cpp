// ue_wrap/devices/serverbox.cpp -- see ue_wrap/devices/serverbox.h.

#include "ue_wrap/devices/serverbox.h"

#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <chrono>
#include <cstdint>

namespace ue_wrap::serverbox {
namespace {

namespace R = reflection;
namespace P = profile;

using field_io::TArrayView;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

int32_t  g_offServers = -1;  // mainGamemode_C.servers
uint64_t g_nextServersTryMs = 0;

bool EnsureServersResolved() {
    if (g_offServers >= 0) return true;
    const uint64_t now = NowMs();
    if (now < g_nextServersTryMs) return false;
    g_nextServersTryMs = now + 1000;
    void* gmCls = R::FindClass(P::name::GamemodeClass);
    if (!gmCls) return false;  // world not loaded yet
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

}  // namespace ue_wrap::serverbox
