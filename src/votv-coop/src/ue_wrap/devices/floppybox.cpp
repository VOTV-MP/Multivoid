// ue_wrap/devices/floppybox.cpp -- see ue_wrap/devices/floppybox.h.

#include "ue_wrap/devices/floppybox.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <unordered_map>

namespace ue_wrap::floppybox {
namespace {

namespace R = reflection;
namespace F = field_io;

// ClassOf verdict cache (the portable_pc shape): NameOf once per distinct
// UClass; no FindClass polling (the box spawns from save/shop).
std::unordered_map<void*, bool> g_verdict;
void* g_cls = nullptr;
int32_t g_offTypes = -1;
int32_t g_offData = -1;
void* g_fnGen = nullptr;

void ResolveMembers(void* cls) {
    g_cls = cls;
    g_offTypes = R::FindPropertyOffset(cls, L"floppyTypes");
    g_offData  = R::FindPropertyOffset(cls, L"floppyData");
    if (g_offTypes < 0) { UE_LOGW("floppybox: floppyTypes offset -- fallback 0x3A8"); g_offTypes = 0x3A8; }
    if (g_offData < 0)  { UE_LOGW("floppybox: floppyData offset -- fallback 0x3B8"); g_offData = 0x3B8; }
    g_fnGen = R::FindFunction(cls, L"gen");
    if (!g_fnGen)
        UE_LOGW("floppybox: gen() not found -- visual rebuild disabled");
    UE_LOGI("floppybox: resolved (types=0x%X data=0x%X gen=%p)", g_offTypes, g_offData, g_fnGen);
}

}  // namespace

bool IsFloppyBoxClass(void* cls) {
    if (!cls) return false;
    if (cls == g_cls) return true;
    auto it = g_verdict.find(cls);
    if (it != g_verdict.end()) return it->second;
    const bool is = R::NameEquals(R::NameOf(cls), L"prop_floppyBox_C");
    g_verdict.emplace(cls, is);
    if (is && !g_cls) ResolveMembers(cls);
    return is;
}

bool ReadArrays(void* actor, BoxArrays& out) {
    if (!actor || g_offTypes < 0 || g_offData < 0) return false;
    out.types = F::ReadInt32ArrayField(actor, g_offTypes);
    out.data  = F::ReadFStringArrayField(actor, g_offData);
    return true;
}

bool ReadDigest(void* actor, uint64_t& outDigest) {
    if (!actor || g_offTypes < 0 || g_offData < 0) return false;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(actor);
    uint64_t h = 1469598103934665603ull;
    auto mixByte = [&h](uint8_t b) { h ^= b; h *= 1099511628211ull; };
    const auto* tv = reinterpret_cast<const F::TArrayView*>(base + g_offTypes);
    if (tv->data && tv->num > 0 && tv->num <= 4096) {
        const uint8_t* p = tv->data;
        for (int32_t i = 0; i < tv->num * 4; ++i) mixByte(p[i]);
    }
    const auto* dv = reinterpret_cast<const F::TArrayView*>(base + g_offData);
    if (dv->data && dv->num > 0 && dv->num <= 4096) {
        for (int32_t i = 0; i < dv->num; ++i) {
            const auto* sv = reinterpret_cast<const F::FStringView*>(dv->data + i * 16);
            mixByte(0x1F);
            if (sv->data && sv->num > 1) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(sv->data);
                for (int32_t b = 0; b < (sv->num - 1) * static_cast<int32_t>(sizeof(wchar_t)); ++b)
                    mixByte(p[b]);
            }
        }
    }
    outDigest = h;
    return true;
}

bool WriteArraysAndGen(void* actor, const BoxArrays& in) {
    if (!actor || g_offTypes < 0 || g_offData < 0) return false;
    if (!F::WriteInt32ArrayField(actor, g_offTypes, in.types)) return false;
    if (!F::WriteFStringArrayField(actor, g_offData, in.data)) return false;
    if (g_fnGen) {
        ParamFrame f(g_fnGen);
        if (f.valid()) Call(actor, f);
    }
    return true;
}

void ResetCache() {
    g_verdict.clear();
    g_cls = nullptr;
    g_offTypes = -1;
    g_offData = -1;
    g_fnGen = nullptr;
}

}  // namespace ue_wrap::floppybox
