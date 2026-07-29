// coop/chat_feed.h -- the coop event/chat line store (gameplay layer, principle 7).
//
// Replaces the old ue_wrap::hud_feed (a game-UMG screen-text widget) with a plain
// thread-safe DATA store: the coop layer Push()es event lines (joins, disconnects,
// chat); the RENDER-THREAD half (ui::chat_view) draws them in our ImGui overlay.
// Same game-thread-snapshot / render-thread-draw split as coop::roster ->
// ui::scoreboard. No engine/UObject access here -- pure data.
//
// TWO TIERS (2026-07-29, the chat-history feature). A line is born LIVE and fades
// on its TTL exactly as before. When it leaves the live set -- by expiry OR by
// overflow, the two exits are now ONE transition -- it either RETIRES into the
// retained tier (the chat HISTORY the T-reveal shows) or is destroyed, decided by
// the Keep class its pusher named. No predicate over the data can make that call:
// it was tested against the whole 15-site census and got 12 of 15 wrong, failing
// on exactly the lines that matter ("Connecting to <host>'s game..." names a peer
// while being purely this player's own status). So the class is a REQUIRED
// parameter with no default on the ambiguous entry points, and is fixed at the
// entry point for the unambiguous ones (PushChat / PushAction are always History).
//
// THE FEED IS NOT THE LOG. This store is one peer's VIEW: it mixes what was said
// in the lobby with this player's own UI notices ("Skin: X", "Nickname color:
// applied"). The lobby's chat RECORD is a separate, host-owned thing (see
// coop/comms/chat_log.h once the wire half lands) -- the retained tier here is
// what this peer SAW, seeded from that record on join.
//
// Push()/Tick()/Reset() run on the GAME THREAD (the callers -- event_feed,
// player_handshake, the harness tick -- are all game-thread). GetSnapshot()/
// HasAny()/RevealActive()/SetChatOpen()/SetRetentionFrozen() are safe from any
// thread (the render thread and the WndProc reach them).

#pragma once

#include <cstdint>
#include <string>

namespace coop::chat_feed {

// Max simultaneously-shown LIVE lines (oldest retires when a new line overflows).
inline constexpr int kMaxLines = 6;

// Max RETAINED (history) lines. Chosen from the user's own reference ("close like
// minecraft") and bounded by measurement: 100 * sizeof(Line) is ~28 KB of store,
// and the join seed it implies is ~21 KB against the 8192-message reliable inbox
// cap -- 1.2 %. Chosen by product reference, bounded by measurement; not derived.
inline constexpr int kMaxRetained = 100;

inline constexpr int kMaxSnapshotLines = kMaxLines + kMaxRetained;

// How long the reveal takes to ramp in/out. Shared by the store (which publishes
// the retained tier for exactly this long after a close) and ui::chat_view (which
// runs the alpha ramp). One constant so the two cannot disagree about when the
// history stops existing -- a store that dropped the rows first would make the
// fade-out draw zero frames.
inline constexpr uint64_t kRevealMs = 220;

// Does this line belong to the chat HISTORY, or is it this peer's own passing
// notice? See the header comment: no data predicate decides this, so the pusher
// says. Transient lines live their TTL and are then gone; History lines retire
// into the tier the T-reveal shows.
enum class Keep : uint8_t {
    Transient,  // this peer's own UI notice / debug line -- never enters history
    History,    // what happened in this lobby: chat, peer actions, join/leave
};

// One feed line, ready to draw. text is UTF-8 (2026-07-04: the ASCII squash is
// gone -- ui::fonts loads a Cyrillic-capable font, so Russian passes end-to-end).
//
// `alpha` is the STORE alpha and ONLY the store alpha: the age-derived TTL curve
// (a short arrival ramp, full while held, fading over the tail), and a constant 0
// for a retained row. It is NOT what gets drawn -- the render half composes it
// with the reveal ramp. Keeping the two apart is a CONSTRAINT, not a detail: the
// resurrection probe below compares consecutive published alphas and treats a rise
// in a line's fade-out tail as impossible, so folding the reveal into the
// published value would make the probe's "can't happen" condition happen routinely
// and destroy the evidence hud.cpp:313-317 rests on.
//
// `key` is the entry's identity AND the total order (the probe keys on it; the
// scroll anchor keys on it). High 32 bits = the host's wire line number once the
// wire half lands, low 32 = a local tiebreak, so a locally-authored line always
// sorts immediately after the newest wire line it could have followed. bornMs is
// NOT identity -- Tick() hoists `now` outside its promotion loop, so two lines
// promoted in one tick share it exactly.
//
// `text` holds up to 255 bytes. A composed chat line can be LONGER than that -- an
// 80-byte nick plus ": " plus a 203-byte message is 285 -- so it is cut at birth, on a
// CHARACTER boundary (coop::text::CapUtf8Bytes). The header used to claim the buffer
// was sized to fit; it never was, and the byte-wise cut that resulted could put a
// split multi-byte sequence on screen.
//
// `nickArgb` is the nick prefix's colour, FROZEN at birth (user 2026-07-29: "old
// chat history is essentially a frozen history"). Resolved once, by the receiver,
// at the moment the line is composed -- see coop/comms/chat_nick_color.h.
// nickLen > 0 marks the first nickLen BYTES of text as that prefix; 0 = an event
// line drawn in one colour.
struct Line {
    char     text[256] = {};
    float    alpha = 1.f;     // STORE alpha (TTL curve); 0 for a retained row
    uint64_t key = 0;         // total order + entry identity (see above)
    uint32_t nickArgb = 0;    // frozen nick colour, 0xAARRGGBB; 0 when nickLen == 0
    uint8_t  nickLen = 0;     // byte length of the nick prefix inside text
    uint8_t  action = 0;      // 1 = peer-action line ("<nick> deleted an email: X") -- the
                              // HUD draws the predicate in the action color (yellow), so a
                              // world-state action reads apart from typed chat (user 2026-07-11)
};

// The published view. `lines[0 .. count)` is ascending by `key`: the retained
// (history) rows first, then the live ones -- retirement is FIFO, so every
// retained key is older than every live key. The retained region is present ONLY
// while RevealActive(); with chat closed a snapshot is the same <= 6 rows it has
// always been.
struct Snapshot {
    int      count = 0;      // total published rows
    int      liveCount = 0;  // trailing rows that are LIVE; the rest are history
    uint32_t gen = 0;        // bumps on every republish (render copies only on change)
    Line     lines[kMaxSnapshotLines];
};

// Append an event line. Auto-expires after the TTL (see Tick) like real chat -- a
// "X joined the game" line is interesting for a moment, then clutters forever.
// The wstring is UTF-8-encoded on the way in (Cyrillic nicks survive).
void Push(const std::wstring& line, Keep keep);

// Append a WIRE-authored chat line -- the host committed it at `lineSeq`, which is
// the total order every peer sorts by. utf8Line starts with the speaker's nick;
// nickByteLen is that prefix's byte length and nickArgb the colour it is drawn in,
// resolved by the RECEIVER at apply time (coop::chat_nick_color::ForSlot).
//
// `seeded` rows are a joiner's history: they land RETAINED, never live, so arriving
// in a lobby does not replay a conversation you were not in across somebody's screen.
// Applying a row also advances the local sort base, so a locally-authored line pushed
// afterwards sorts after it rather than in front of the whole history.
// Always History. Game thread.
void PushWireChat(const std::string& utf8Line, uint8_t nickByteLen, uint32_t nickArgb,
                  uint32_t lineSeq, bool seeded);

// Append a peer-ACTION line (same shape as PushChat, action flag set): the HUD
// draws the post-nick predicate in the action color instead of the chat body
// color. Always History. Game thread.
void PushAction(const std::string& utf8Line, uint8_t nickByteLen, uint32_t nickArgb);

// Append an event line AFTER `delayMs` (promoted to the live feed by Tick once due). Used for the join
// announces: the client reports world-ready before its loading screen visually clears, so showing
// "X joined the game" immediately looks premature -- a short delay lets the join settle first
// (user 2026-06-21). Game thread (queued on the game-thread Tick). The delay is WALL clock: it is
// about when the line should APPEAR, not about how long it then lives.
void PushDelayed(const std::wstring& line, uint64_t delayMs, Keep keep);

// Drop expired lines + recompute the fade alphas by age, then republish the
// snapshot. Cheap no-op when the feed is empty. Call from a periodic game-thread
// tick (the harness tick, ~60 Hz). Also advances the SUSPENSION accumulator -- see
// SetChatOpen.
void Tick();

// Copy the latest snapshot into `out` IF it changed since the caller's `gen` (which
// is updated in place), else leave `out` alone and return false. Safe from ANY
// thread. There is no unconditional variant: with the history in the snapshot a
// per-frame copy would be ~28 KB of pointless memcpy, and every caller already holds
// its last copy.
bool GetSnapshotIfNewer(Snapshot& out, uint32_t& gen);

// True if there is at least one LIVE line to draw (lock-free). Any thread.
// Deliberately NOT "any published row": a retained history line is not a reason to
// keep the passive HUD -- and therefore the whole overlay frame -- alive.
bool HasAny();

// The chat surface's open/close EDGE, pushed in by ui::chat_input from whichever
// thread closed it (the WndProc ESC path, the render-thread submit, the SEH
// unlatch). Writes ATOMICS only -- never the line store, which is game-thread-only.
void SetChatOpen(bool open);

// True while the history is on screen: chat is open, OR it closed less than
// kRevealMs ago and the fade-out is still drawing. Any thread. Two consumers: the
// snapshot (whether to publish the retained tier) and the TTL suspension below --
// plus ui::hud::IsActive(), which keeps the overlay frame alive through the ramp.
bool RevealActive();

// While the reveal is up, the TTL clock DOES NOT ADVANCE: a player reading history
// must not have the messages expire out from under them, and one who scrolls back
// for a minute must not return to an empty feed. Implemented as a suspended-time
// accumulator with a per-entry birth snapshot, so a line born mid-reveal is aged
// against the suspension that has accrued SINCE IT WAS BORN -- without that
// snapshot the subtraction underflows and every new message pops one tick after it
// arrives, which is the one thing the user said must never happen.

// Freeze retained-tier eviction while the reader is paged back through history
// (ui::chat_view PINNED). Any thread. A hard ceiling at 2x kMaxRetained still
// wins -- an unbounded store is not a scroll feature.
void SetRetentionFrozen(bool frozen);

// Clear all lines -- live, retained and pending (e.g. on a fresh session start so a
// prior session's lines don't linger, and on the leave funnel so lobby A's
// conversation cannot surface in lobby B). Game thread.
void Reset();

// UTF-8-encode a wide string (UTF-16 surrogate pairs included; control chars
// stripped except TAB). The feeds carry UTF-8 (Cyrillic nicks render as-is).
// Shared here so peer_action_feed doesn't keep a copy (2026-07-10 dedupe).
std::string ToUtf8(const std::wstring& w);

}  // namespace coop::chat_feed
