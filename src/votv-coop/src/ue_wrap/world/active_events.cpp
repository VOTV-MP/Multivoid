// ue_wrap/world/active_events.cpp -- see ue_wrap/world/active_events.h.

#include "ue_wrap/world/active_events.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

namespace ue_wrap::active_events {
namespace {

namespace R = ue_wrap::reflection;

int32_t g_offActiveEvents = -1;  // mainGamemode_C::activeEvents (int refcount)
int32_t g_offSenders = -1;       // mainGamemode_C::activeEvents_senders (TArray<UObject*>)
bool g_resolveFailed = false;

// UE4 TArray<UObject*> header.
struct RawPtrArray {
    void**  Data;
    int32_t Num;
    int32_t Max;
};

bool Resolved() { return g_offActiveEvents >= 0 && g_offSenders >= 0; }

}  // namespace

bool EnsureResolved() {
    if (Resolved()) return true;
    if (g_resolveFailed) return false;
    void* cls = object_index::ClassByName(L"mainGamemode_C");
    if (!cls) return false;  // the world has not loaded it yet
    g_offActiveEvents = R::FindPropertyOffset(cls, L"activeEvents");
    g_offSenders = R::FindPropertyOffset(cls, L"activeEvents_senders");
    if (Resolved()) {
        UE_LOGI("event_active: resolved (activeEvents=0x%X activeEvents_senders=0x%X)", g_offActiveEvents,
                g_offSenders);
        return true;
    }
    g_resolveFailed = true;  // a member missing from a loaded class does not appear later
    UE_LOGW("event_active: resolution INCOMPLETE on a loaded mainGamemode_C (activeEvents=0x%X "
            "activeEvents_senders=0x%X) -- latched OFF; game version mismatch?", g_offActiveEvents, g_offSenders);
    return false;
}

bool ReadCount(int32_t& out) {
    if (!Resolved()) return false;
    void* gm = world_singleton::Gamemode();
    if (!gm) return false;
    out = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(gm) + g_offActiveEvents);
    return true;
}

bool ReadSenders(Senders& out) {
    if (!Resolved()) return false;
    void* gm = world_singleton::Gamemode();
    if (!gm) return false;
    const auto* arr = reinterpret_cast<const RawPtrArray*>(reinterpret_cast<const uint8_t*>(gm) + g_offSenders);
    out.data = arr->Data;
    out.num = arr->Num;
    return true;
}

}  // namespace ue_wrap::active_events
