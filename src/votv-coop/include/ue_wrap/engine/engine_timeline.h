// ue_wrap/engine/engine_timeline.h -- a UTimelineComponent as a device's lever keeps it: whether it runs, which
// way, and where, read in place, and the two verbs that start it. Engine-wrapper layer (principle 7): no
// gameplay, network or coop state. Game thread.
// Implementation: src/ue_wrap/engine/engine_timeline.cpp.

#pragma once

namespace ue_wrap::engine_timeline {

// UTimelineComponent::TheTimeline's run, direction and place, as the component itself keeps them: Play and
// Reverse write them at once, where a device's own copy of the direction waits for the timeline's next tick.
struct State {
    bool  playing = false;
    bool  reverse = false;
    float position = 0.0f;
};

// Read the timeline in place: one struct read and no dispatch. False when `timeline` is null or dead, or the
// timeline's layout did not resolve (said once, and the reader stays off, since a missing member will not appear
// later).
bool Read(void* timeline, State& out);

// UTimelineComponent::PlayFromStart and ReverseFromEnd, as a device's graph calls them.
bool PlayFromStart(void* timeline);
bool ReverseFromEnd(void* timeline);

}  // namespace ue_wrap::engine_timeline
