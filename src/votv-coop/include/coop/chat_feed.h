// coop/chat_feed.h -- the coop event/chat line store (gameplay layer, principle 7).
//
// Replaces the old ue_wrap::hud_feed (a game-UMG screen-text widget) with a plain
// thread-safe DATA store: the coop layer Push()es event lines (joins, disconnects,
// later real chat); the RENDER-THREAD half (ui::hud) draws the last N lines in our
// ImGui overlay. Same game-thread-snapshot / render-thread-draw split as
// coop::roster -> ui::scoreboard. No engine/UObject access here -- pure data.
//
// Push()/Tick()/Reset() run on the GAME THREAD (the callers -- event_feed,
// player_handshake, the harness tick -- are all game-thread). GetSnapshot()/
// HasAny() are safe from any thread (the render thread reads them).

#pragma once

#include <string>

namespace coop::chat_feed {

// Max simultaneously-shown lines (oldest drops when a new line overflows).
inline constexpr int kMaxLines = 6;

// One feed line, ready to draw. text is ASCII (peer nicks are sanitized upstream;
// the event phrasing is English). alpha is the age-derived fade (1 fresh -> 0 at TTL).
struct Line {
    char  text[100] = {};
    float alpha = 1.f;
};

struct Snapshot {
    int  count = 0;
    Line lines[kMaxLines];
};

// Append an event line. Auto-expires after the TTL (see Tick) like real chat -- a
// "X joined the game" line is interesting for a moment, then clutters forever.
void Push(const std::wstring& line);

// Drop expired lines + recompute the fade alphas by age, then republish the
// snapshot. Cheap no-op when the feed is empty. Call from a periodic game-thread
// tick (the harness tick, ~60 Hz).
void Tick();

// Copy the latest snapshot. Safe from ANY thread (the render thread reads it).
void GetSnapshot(Snapshot& out);

// True if there is at least one line to draw (lock-free). Any thread.
bool HasAny();

// Clear all lines (e.g. on a fresh session start so a prior session's lines don't
// linger). Game thread.
void Reset();

}  // namespace coop::chat_feed
