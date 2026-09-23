// ue_wrap/world/active_events.cpp -- see ue_wrap/world/active_events.h.

#include "ue_wrap/world/active_events.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>

namespace ue_wrap::active_events {
namespace {

namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;

void*   g_gmCls = nullptr;
int32_t g_offActiveEvents = -1;  // mainGamemode_C::activeEvents (int refcount)
int32_t g_offSenders = -1;       // mainGamemode_C::activeEvents_senders (TArray<UObject*>)
Clock::time_point g_nextResolve{};
int  g_postClassAttempts = 0;
bool g_resolveLatched = false;
constexpr int kMaxPostClassAttempts = 5;

void*   g_gm = nullptr;
int32_t g_gmIdx = -1;
Clock::time_point g_nextGmScan{};

// UE4 TArray<UObject*> header.
struct RawPtrArray {
    void**  Data;
    int32_t Num;
    int32_t Max;
};

bool Resolved() { return g_offActiveEvents >= 0 && g_offSenders >= 0; }

}  // namespace

bool EnsureResolved() {
    if (g_resolveLatched) return Resolved();
    const auto now = Clock::now();
    if (now < g_nextResolve) return false;
    g_nextResolve = now + std::chrono::seconds(2);
    if (!g_gmCls) g_gmCls = R::FindClass(L"mainGamemode_C");
    if (!g_gmCls) return false;  // world not loaded yet -- keep trying
    if (g_offActiveEvents < 0) g_offActiveEvents = R::FindPropertyOffset(g_gmCls, L"activeEvents");
    if (g_offSenders < 0) g_offSenders = R::FindPropertyOffset(g_gmCls, L"activeEvents_senders");
    if (Resolved()) {
        g_resolveLatched = true;
        UE_LOGI("event_active: resolved (activeEvents=0x%X activeEvents_senders=0x%X)", g_offActiveEvents,
                g_offSenders);
        return true;
    }
    if (++g_postClassAttempts >= kMaxPostClassAttempts) {
        g_resolveLatched = true;
        UE_LOGW("event_active: resolution INCOMPLETE after %d passes on a loaded mainGamemode_C "
                "(activeEvents=0x%X activeEvents_senders=0x%X) -- latched OFF; game version mismatch?",
                g_postClassAttempts, g_offActiveEvents, g_offSenders);
    }
    return false;
}

void* Gamemode(int32_t* indexOut) {
    if (!g_gm || !R::IsLiveByIndex(g_gm, g_gmIdx)) {
        g_gm = nullptr;
        g_gmIdx = -1;
        // The miss walks the object array, so a lost cache is looked for at most once a second.
        const auto now = Clock::now();
        if (g_gmCls && now >= g_nextGmScan) {
            g_nextGmScan = now + std::chrono::seconds(1);
            for (void* obj : R::FindObjectsByClass(L"mainGamemode_C")) {
                if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) {
                    g_gm = obj;
                    g_gmIdx = R::InternalIndexOf(obj);
                    break;
                }
            }
        }
    }
    if (indexOut) *indexOut = g_gmIdx;
    return g_gm;
}

bool ReadCount(int32_t& out) {
    if (!Resolved()) return false;
    void* gm = Gamemode();
    if (!gm) return false;
    out = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(gm) + g_offActiveEvents);
    return true;
}

bool ReadSenders(Senders& out) {
    if (!Resolved()) return false;
    void* gm = Gamemode();
    if (!gm) return false;
    const auto* arr = reinterpret_cast<const RawPtrArray*>(reinterpret_cast<const uint8_t*>(gm) + g_offSenders);
    out.data = arr->Data;
    out.num = arr->Num;
    return true;
}

}  // namespace ue_wrap::active_events
