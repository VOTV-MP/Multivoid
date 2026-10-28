// ue_wrap/engine/engine_timeline.cpp -- see ue_wrap/engine/engine_timeline.h.

#include "ue_wrap/engine/engine_timeline.h"

#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>

namespace ue_wrap::engine_timeline {
namespace {

namespace R = reflection;

struct Bool { int32_t off = -1; uint8_t mask = 0; };

Bool    g_reverse, g_playing;
int32_t g_position = -1;
bool    g_resolved = false;
bool    g_latchedOff = false;

const uint8_t* Base(const void* obj) { return reinterpret_cast<const uint8_t*>(obj); }

bool Bit(const void* obj, const Bool& b) { return (Base(obj)[b.off] & b.mask) != 0; }

// Resolve once, from the engine's class: the layout is the engine's, the same for every timeline.
bool EnsureResolved() {
    if (g_resolved) return true;
    if (g_latchedOff) return false;
    void* cls = object_index::ClassByName(L"TimelineComponent");
    if (!cls) return false;  // not loaded yet: asked again, one hash lookup and no walk of the object array
    const int32_t tl = R::FindPropertyOffset(cls, L"TheTimeline");
    void* st = tl >= 0 ? R::PropertyInnerStruct(cls, L"TheTimeline") : nullptr;
    Bool rev, play;
    const int32_t pos = st ? R::FindPropertyOffset(st, L"Position") : -1;
    if (st && R::FindBoolProperty(st, L"bReversePlayback", rev.off, rev.mask) &&
        R::FindBoolProperty(st, L"bPlaying", play.off, play.mask) && rev.mask && play.mask && pos >= 0) {
        g_reverse = {tl + rev.off, rev.mask};
        g_playing = {tl + play.off, play.mask};
        g_position = tl + pos;
        g_resolved = true;
        return true;
    }
    g_latchedOff = true;
    UE_LOGW("engine_timeline: TimelineComponent.TheTimeline did not resolve (bPlaying, bReversePlayback or Position) "
            "-- timeline reads stay off; game version mismatch?");
    return false;
}

}  // namespace

bool Read(void* timeline, State& out) {
    if (!timeline || !R::IsLive(timeline) || !EnsureResolved()) return false;
    out.playing = Bit(timeline, g_playing);
    out.reverse = Bit(timeline, g_reverse);
    out.position = *reinterpret_cast<const float*>(Base(timeline) + g_position);
    return true;
}

bool PlayFromStart(void* timeline) {
    return timeline && R::IsLive(timeline) && component_calls::CallParamlessNamed(timeline, L"PlayFromStart");
}

bool ReverseFromEnd(void* timeline) {
    return timeline && R::IsLive(timeline) && component_calls::CallParamlessNamed(timeline, L"ReverseFromEnd");
}

}  // namespace ue_wrap::engine_timeline
