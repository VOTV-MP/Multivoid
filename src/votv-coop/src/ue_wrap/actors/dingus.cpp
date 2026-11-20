// ue_wrap/actors/dingus.cpp -- see ue_wrap/actors/dingus.h.

#include "ue_wrap/actors/dingus.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::dingus {
namespace {

namespace R  = reflection;
namespace OI = object_index;

constexpr int kClassCount = 4;
constexpr const wchar_t* kClassNames[kClassCount] = {L"prop_dingus_C", L"prop_dingus2_C", L"prop_dingus4ugly_C",
                                                     L"prop_dingus5erie_C"};
std::atomic<void*> g_classes[kClassCount];

// What each class's reads and writes need, resolved when its class pointer is: `isUsed`'s byte and bit and `ws1`'s offset. Game
// thread only.
struct Layout {
    int32_t usedOff = -1;
    uint8_t usedMask = 0;
    int32_t ws1Off = -1;
};
Layout g_layout[kClassCount];

// What `loadData` gives the music of a found dingus: its volume multiplier, and the attenuation asset its graph imports.
constexpr float kFoundVolume = 0.05f;
constexpr const wchar_t* kAttenuationPath = L"/Game/audio/misc/att_small.att_small";

// The attenuation asset, loaded by RefreshClasses once the four classes are known and held by slot and serial.
void* g_attenuation = nullptr;
int32_t g_attenuationIndex = -1;
uint64_t g_attenuationTriedAt = ~0ull;

// The index of `actor`'s class among the four, or -1.
int ClassIndexOf(void* actor) {
    void* const cls = actor ? R::ClassOf(actor) : nullptr;
    if (!cls) return -1;
    for (int i = 0; i < kClassCount; ++i)
        if (g_classes[i].load(std::memory_order_acquire) == cls) return i;
    return -1;
}

// `ws1`, the music component of a live dingus of class `ci`; null when it does not read or is not live.
void* Music(void* actor, int ci) {
    const int32_t off = g_layout[ci].ws1Off;
    void* const ws1 = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(actor) + off) : nullptr;
    return ws1 && R::IsLive(ws1) ? ws1 : nullptr;
}

bool ReadBool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return false;
    return (static_cast<const uint8_t*>(obj)[off] & mask) != 0;
}

bool HasBool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return R::FindBoolProperty(R::ClassOf(obj), name, off, mask);
}

// The attenuation asset, loaded when it is not held live and the class set has moved since the last try, so a load that failed is
// not repeated until a class loads. Called from RefreshClasses only, never from a walk or a mark.
void ResolveAttenuation(uint64_t classSet) {
    if (g_attenuation) {
        if (R::IsLiveByIndex(g_attenuation, g_attenuationIndex)) return;
        g_attenuation = nullptr;
        g_attenuationTriedAt = ~0ull;
    }
    if (g_attenuationTriedAt == classSet) return;
    g_attenuationTriedAt = classSet;
    g_attenuation = asset_load::LoadObjectByPath(kAttenuationPath);
    g_attenuationIndex = g_attenuation ? R::InternalIndexOf(g_attenuation) : -1;
}

// `ws1`'s attenuation settings property set to the held asset, as the graph's own assignment does. False when no live asset is held.
bool WriteAttenuation(void* ws1) {
    static R::InstanceOffset sAtt{L"AttenuationSettings"};
    const int32_t off = sAtt.Of(ws1);
    if (!g_attenuation || !R::IsLiveByIndex(g_attenuation, g_attenuationIndex) || off < 0) return false;
    *reinterpret_cast<void**>(static_cast<char*>(ws1) + off) = g_attenuation;
    return true;
}

struct Walk {
    DingusFn fn;
    void* ctx;
    size_t n;
};

}  // namespace

bool RefreshClasses() {
    static uint64_t sSet = ~0ull, sUnlist = ~0ull;
    static bool sComplete = false;
    const uint64_t set = OI::ClassSetVersion(), unlist = OI::ClassUnlistVersion();
    if (!sComplete || set != sSet || unlist != sUnlist) {
        sSet = set;
        sUnlist = unlist;
        for (int i = 0; i < kClassCount; ++i) {
            void* const cls = OI::ClassByName(kClassNames[i]);
            if (cls != g_classes[i].load(std::memory_order_relaxed)) {
                g_layout[i] = Layout{};
                if (cls) {
                    R::FindBoolProperty(cls, L"isUsed", g_layout[i].usedOff, g_layout[i].usedMask);
                    g_layout[i].ws1Off = R::FindPropertyOffset(cls, L"ws1");
                }
            }
            g_classes[i].store(cls, std::memory_order_release);
        }
    }
    bool complete = true;
    for (const std::atomic<void*>& c : g_classes) complete = complete && c.load(std::memory_order_acquire) != nullptr;
    const bool filled = complete && !sComplete;
    sComplete = complete;
    if (complete) ResolveAttenuation(set);
    return filled;
}

bool IsDingusClass(void* cls) {
    if (!cls) return false;
    for (const std::atomic<void*>& c : g_classes)
        if (c.load(std::memory_order_acquire) == cls) return true;
    return false;
}

bool IsDingus(void* actor) { return actor && IsDingusClass(R::ClassOf(actor)); }

int ReadFound(void* actor) {
    const int ci = ClassIndexOf(actor);
    if (ci < 0 || g_layout[ci].usedOff < 0) return -1;
    return (static_cast<const uint8_t*>(actor)[g_layout[ci].usedOff] & g_layout[ci].usedMask) != 0 ? 1 : 0;
}

int ReadMusicOn(void* actor) {
    const int ci = ClassIndexOf(actor);
    if (ci < 0) return -1;
    void* const ws1 = Music(actor, ci);
    if (!ws1 || !HasBool(ws1, L"bIsActive")) return -1;
    return ReadBool(ws1, L"bIsActive") ? 1 : 0;
}

bool MarkFound(void* actor) {
    const int ci = ClassIndexOf(actor);
    if (ci < 0 || g_layout[ci].usedOff < 0) return false;
    static_cast<uint8_t*>(actor)[g_layout[ci].usedOff] |= g_layout[ci].usedMask;
    void* const ws1 = Music(actor, ci);
    if (!ws1) return false;
    return component_calls::SetVolumeMultiplier(ws1, kFoundVolume) && WriteAttenuation(ws1) &&
           component_calls::CallParamlessNamed(ws1, L"Deactivate");
}

size_t ForEachDingus(DingusFn fn, void* ctx) {
    if (!fn) return 0;
    Walk w{fn, ctx, 0};
    for (const std::atomic<void*>& c : g_classes) {
        void* const cls = c.load(std::memory_order_acquire);
        if (!cls) continue;
        OI::ForEachInstance(cls, [](void* p, void* obj, int32_t) {
            Walk* const walk = static_cast<Walk*>(p);
            if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            walk->fn(walk->ctx, obj);
            ++walk->n;
        }, &w);
    }
    return w.n;
}

}  // namespace ue_wrap::dingus
