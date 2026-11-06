// coop/interactables/cremator_occupancy.cpp -- see coop/interactables/cremator_occupancy.h.

#include "coop/interactables/cremator_occupancy.h"

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/cremator.h"
#include "ue_wrap/facts/function_row.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace coop::cremator_occupancy {
namespace {

namespace C  = ue_wrap::cremator;
namespace sg = ue_wrap::script_gate;

// The cremator's own begin-overlap of its `burning` component: the event that collects the sitter.
UE_FACT_FN(kRowBurningOverlap, "cremator_occupancy", L"cremator_C",
           L"BndEvt__cremator_burning_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
constexpr int kTagOverlap = 0x43444F43;  // 'CDOC'

constexpr int kMaxListeners = 4;
std::array<OverlapFn, kMaxListeners> g_listeners{};
int g_count = 0;

// Post, on the game thread: the body has run, so the cremator's own begin-overlap handling is done.
void OnOverlapPost(const sg::Call& c) {
    if (g_count == 0 || !c.object || !c.function || !c.locals) return;
    static void*   sFn = nullptr;
    static int32_t sOtherOff = -1;
    if (c.function != sFn) {
        sFn = c.function;
        sOtherOff = ue_wrap::reflection::FindParamOffset(c.function, L"OtherActor");
    }
    if (sOtherOff < 0) return;
    void* other = nullptr;
    std::memcpy(&other, c.locals + sOtherOff, sizeof(other));
    for (int i = 0; i < g_count; ++i) g_listeners[i](c.object, other);
}

}  // namespace

bool AddListener(OverlapFn fn) {
    if (!fn) return false;
    bool known = false;
    for (int i = 0; i < g_count; ++i) known = known || g_listeners[i] == fn;
    if (!known) {
        if (g_count == kMaxListeners) return false;
        g_listeners[g_count++] = fn;
    }
    static bool sWatching = false;
    if (!sWatching) sWatching = sg::WatchName(kRowBurningOverlap, kTagOverlap, nullptr, &OnOverlapPost);
    return sWatching;
}

bool WatchLive() { return sg::NameWatchLive(kRowBurningOverlap, kTagOverlap); }

bool IsOverlapping(void* cremator, void* actor) {
    return C::OverlapsBurnVolume(cremator, actor);
}

}  // namespace coop::cremator_occupancy
