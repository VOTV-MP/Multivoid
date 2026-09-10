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

using field_io::FStringView;
using field_io::TArrayView;
using field_io::ReadFStringAt;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

int32_t g_offName       = -1;
int32_t g_offType       = -1;
int32_t g_offReadWrites = -1;
int32_t g_offData       = -1;
int32_t g_offObjectData = -1;
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

    struct Row { const wchar_t* name; int32_t* slot; };
    const Row rows[] = {
        { L"name",             &g_offName },
        { L"floppyType",       &g_offType },
        { L"floppyReadwrites", &g_offReadWrites },
        { L"floppyData",       &g_offData },
        { L"floppyObjectData", &g_offObjectData },
    };
    for (const Row& r : rows) *r.slot = R::FindPropertyOffset(cls, r.name);
    g_fnProcess = R::FindFunction(cls, L"pocessFloppy");
    g_fnEject   = R::FindFunction(cls, L"ejectFloppy");

    // No offset fallbacks: an unresolved member here means the class is not the one this wrapper
    // was written against, and a guessed offset would write into whatever now lives there.
    if (g_offName < 0 || g_offType < 0 || g_offReadWrites < 0 || g_offData < 0 ||
        g_offObjectData < 0 || !g_fnProcess || !g_fnEject) {
        UE_LOGW("serverbox: slot resolution incomplete (name=%d type=%d rw=%d data=%d json=%d "
                "pocessFloppy=%p ejectFloppy=%p) -- the slot half stays off",
                g_offName, g_offType, g_offReadWrites, g_offData, g_offObjectData, g_fnProcess,
                g_fnEject);
        return false;
    }
    g_resolved = true;
    UE_LOGI("serverbox: slot resolved (floppyType=0x%X objectData=0x%X insert=%p eject=%p)",
            g_offType, g_offObjectData, g_fnProcess, g_fnEject);
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

bool ReadSlot(void* box, SlotState& out) {
    if (!box || !g_resolved) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(box);
    out.floppyType = *reinterpret_cast<const int32_t*>(p + g_offType);
    out.readWrites = *reinterpret_cast<const int32_t*>(p + g_offReadWrites);
    const auto* data = reinterpret_cast<const TArrayView*>(p + g_offData);
    out.dataNum = data->data ? data->num : 0;
    const auto* json = reinterpret_cast<const FStringView*>(p + g_offObjectData);
    // An FString's count includes its terminator; an empty slot's string carries neither.
    out.objectDataLen = (json->data && json->num > 1) ? json->num - 1 : 0;
    return true;
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
