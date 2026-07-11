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

#include <cstdint>
#include <string>

namespace coop::chat_feed {

// Max simultaneously-shown lines (oldest drops when a new line overflows).
inline constexpr int kMaxLines = 6;

// One feed line, ready to draw. text is UTF-8 (2026-07-04: the ASCII squash is
// gone -- ui::fonts loads a Cyrillic-capable font, so Russian passes end-to-end).
// alpha is the age-derived fade: a short fade-IN on arrival (the chat-imgui-samp
// feel), full while held, fading out over the TTL tail. nickLen > 0 marks the
// first nickLen BYTES as a player-nick prefix the HUD colors per slot; 0 = an
// event line (single color). Sized for the 203-byte wire text + a nick + ": ".
struct Line {
    char     text[256] = {};
    float    alpha = 1.f;
    uint64_t bornMs = 0;   // entry identity (the resurrection probe keys on it)
    uint8_t  nickLen = 0;  // byte length of the nick prefix inside text (0 = event line)
    uint8_t  slot = 0;     // sender peer slot (nick color); meaningful when nickLen > 0
    uint8_t  action = 0;   // 1 = peer-action line ("<nick> deleted an email: X") -- the
                           // HUD draws the predicate in the action color (yellow), so a
                           // world-state action reads apart from typed chat (user 2026-07-11)
};

struct Snapshot {
    int  count = 0;
    Line lines[kMaxLines];
};

// Append an event line. Auto-expires after the TTL (see Tick) like real chat -- a
// "X joined the game" line is interesting for a moment, then clutters forever.
// The wstring is UTF-8-encoded on the way in (Cyrillic nicks survive).
void Push(const std::wstring& line);

// Append a CHAT line: utf8Line starts with the sender's nick; nickByteLen is that
// prefix's byte length (the HUD colors it per `slot`). Game thread.
void PushChat(const std::string& utf8Line, uint8_t nickByteLen, uint8_t slot);

// Append a peer-ACTION line (same shape as PushChat, action flag set): the HUD
// draws the post-nick predicate in the action color instead of the chat body
// color. Game thread.
void PushAction(const std::string& utf8Line, uint8_t nickByteLen, uint8_t slot);

// Append an event line AFTER `delayMs` (promoted to the live feed by Tick once due). Used for the join
// announces: the client reports world-ready before its loading screen visually clears, so showing
// "X joined the game" immediately looks premature -- a short delay lets the join settle first
// (user 2026-06-21). Game thread (queued on the game-thread Tick).
void PushDelayed(const std::wstring& line, uint64_t delayMs);

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

// UTF-8-encode a wide string (UTF-16 surrogate pairs included; control chars
// stripped except TAB). The feeds carry UTF-8 (Cyrillic nicks render as-is).
// Shared here so peer_action_feed doesn't keep a copy (2026-07-10 dedupe).
std::string ToUtf8(const std::wstring& w);

}  // namespace coop::chat_feed
