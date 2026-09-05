// coop/comms/chat_feed.h -- the coop event and chat line store: a thread-safe data store the
// coop layer pushes lines into (joins, disconnects, chat, notices) and the render-thread chat
// view draws from the overlay, the same game-thread-snapshot, render-thread-draw split as the
// roster and the scoreboard. No engine access. Two tiers: a line is born live and fades on its
// TTL; when it leaves the live set, by expiry or overflow, it either retires into the retained
// tier (the chat history the reveal shows) or is destroyed, decided by the Keep class its
// pusher named, since no predicate over the data can tell a lobby event from this player's own
// status notice. The class is a required parameter on the ambiguous entry points and fixed on
// the unambiguous ones. The feed is not the log: this store is one peer's view, mixing lobby
// chat with the player's own notices, while the lobby's chat record is host-owned (see
// coop/comms/chat_log.h), and the retained tier is what this peer saw, seeded from that record
// on join. Push, Tick and Reset run on the game thread; the snapshot, the emptiness check, the
// reveal state and the two setters are safe from any thread.

#pragma once

#include <cstdint>
#include <string>

namespace coop::chat_feed {

// The most live lines shown at once; the oldest retires when a new line overflows.
inline constexpr int kMaxLines = 6;

// The most retained history lines, chosen by product reference and bounded by measurement: the
// store is a few tens of KB, and the join seed it implies is a small fraction of the reliable
// inbox cap.
inline constexpr int kMaxRetained = 100;

// Paging back through history freezes eviction, or the rows being read vanish as new ones
// arrive, so the held tier may legitimately exceed the retained cap while pinned. This is the
// one name for that allowance, and every ceiling below is derived from it, so the four cannot
// drift apart: the publish walk once stopped at the retained cap while the store held twice
// that, and with a reader paged back the newest rows were the ones outside the window.
inline constexpr int kRetentionFreezeFactor = 2;

// The most rows the retained tier can hold at once, pinned.
inline constexpr int kMaxHeldLines = kMaxRetained * kRetentionFreezeFactor;

// The publish array must physically hold everything the store can contain, or a row that
// exists is a row nobody can see.
inline constexpr int kMaxSnapshotLines = kMaxLines + kMaxHeldLines;

static_assert(kMaxSnapshotLines >= kMaxLines + kMaxHeldLines,
              "the snapshot must hold every live row plus the whole held tier");
static_assert(kRetentionFreezeFactor >= 1,
              "a freeze factor below 1 would evict the rows the reader is paged back over");

// How long the reveal ramps in and out, shared by the store (which publishes the retained tier
// for exactly this long after a close) and the chat view (which runs the alpha ramp), so the
// two cannot disagree about when the history stops existing.
inline constexpr uint64_t kRevealMs = 220;

// Does this line belong to the chat history, or is it this peer's own passing notice? No data
// predicate decides it, so the pusher says. Transient lines live their TTL and are gone;
// History lines retire into the tier the reveal shows.
enum class Keep : uint8_t {
    Transient,  // this peer's own UI notice / debug line -- never enters history
    History,    // what happened in this lobby: chat, peer actions, join/leave
};

// One feed line, ready to draw; the text is UTF-8. `alpha` is the store alpha only: the
// age-derived TTL curve (a short arrival ramp, full while held, fading over the tail), and 0
// for a retained row. It is not what is drawn: the render half composes it with the reveal
// ramp, and keeping the two apart is a constraint, since the resurrection probe compares
// consecutive published alphas and treats a rise in a line's fade-out as impossible. `key` is
// the entry's identity and the total order: the high 32 bits are the host's wire line number,
// the low 32 a local tiebreak, so a locally authored line sorts right after the newest wire
// line it could have followed; the birth time is not identity, since lines promoted in one
// tick share it. `text` holds up to 255 bytes, and a composed chat line can be longer, so it
// is cut at birth on a character boundary. `nickArgb` is the nick prefix's colour, frozen at
// birth and resolved once by the receiver when the line is composed (see
// coop/comms/chat_nick_color.h); nickLen marks the prefix's bytes, and 0 is an event line in
// one colour.
struct Line {
    char     text[256] = {};
    float    alpha = 1.f;     // STORE alpha (TTL curve); 0 for a retained row
    uint64_t key = 0;         // total order + entry identity (see above)
    uint32_t nickArgb = 0;    // frozen nick colour, 0xAARRGGBB; 0 when nickLen == 0
    uint8_t  nickLen = 0;     // byte length of the nick prefix inside text
    uint8_t  action = 0;      // 1 = a peer-action line, whose predicate the HUD draws in the action colour
};

// The published view: the lines are ascending by key, the retained rows first and then the
// live ones, since retirement is FIFO and every retained key is older than every live key.
// The retained region is present only while the reveal is active; with chat closed a snapshot
// is the same handful of live rows.
struct Snapshot {
    int      count = 0;      // total published rows
    int      liveCount = 0;  // trailing rows that are LIVE; the rest are history
    uint32_t gen = 0;        // bumps on every republish (render copies only on change)
    Line     lines[kMaxSnapshotLines];
};

// Append an event line, which expires after the TTL like chat: a joined-the-game line is
// interesting for a moment, then clutter. The wide string is UTF-8-encoded on the way in.
void Push(const std::wstring& line, Keep keep);

// Append a wire-authored chat line the host committed at `lineSeq`, the total order every peer
// sorts by. The line starts with the speaker's nick; the byte length and the colour are
// resolved by the receiver at apply time. Seeded rows are a joiner's history: they land
// retained, never live, so arriving in a lobby does not replay a conversation across the
// screen. Applying a row also advances the local sort base, so a locally authored line pushed
// afterwards sorts after it. Always History. Game thread.
void PushWireChat(const std::string& utf8Line, uint8_t nickByteLen, uint32_t nickArgb,
                  uint32_t lineSeq, bool seeded);

// Append a peer-action line, the chat shape with the action flag set, so the HUD draws the
// predicate in the action colour. Always History. Game thread.
void PushAction(const std::string& utf8Line, uint8_t nickByteLen, uint32_t nickArgb);

// Append an event line after `delayMs`, promoted to the live feed by Tick once due; for the
// join announces, since a client reports world-ready before its loading screen clears and an
// immediate line looks premature. The delay is wall clock: when the line appears, not how
// long it lives. Game thread.
void PushDelayed(const std::wstring& line, uint64_t delayMs, Keep keep);

// Drop expired lines, recompute the fade alphas by age, then republish the snapshot; a cheap
// no-op when empty. From the periodic game-thread tick. Also advances the suspension
// accumulator (see SetChatOpen).
void Tick();

// Copy the latest snapshot into `out` if it changed since the caller's generation, updated in
// place; else leave it and return false. Any thread. No unconditional variant: with the history
// in the snapshot a per-frame copy would be tens of KB of pointless copying, and every caller
// holds its last copy.
bool GetSnapshotIfNewer(Snapshot& out, uint32_t& gen);

// True if there is at least one live line to draw; lock-free, any thread. Deliberately not any
// published row: a retained history line is no reason to keep the passive HUD, and so the
// whole overlay frame, alive.
bool HasAny();

// The chat surface's open and close edge, pushed in by the chat input from whichever thread
// closed it (the window-procedure Escape path, the render-thread submit, the fault unlatch).
// Writes atomics only, never the line store.
void SetChatOpen(bool open);

// True while the history is on screen: chat open, or closed less than the reveal time ago with
// the fade-out still drawing. Any thread. Consumed by the snapshot (whether to publish the
// retained tier), the TTL suspension below, and the HUD's active check, which keeps the
// overlay frame alive through the ramp.
bool RevealActive();

// While the reveal is up the TTL clock does not advance: a player reading history must not
// have messages expire under them. A suspended-time accumulator with a per-entry birth
// snapshot, so a line born mid-reveal is aged against the suspension accrued since its birth;
// without the snapshot the subtraction underflows and every new message pops one tick after
// it arrives.

// Freeze retained-tier eviction while the reader is paged back through history. Any thread.
// The hard ceiling at the freeze factor still wins: an unbounded store is not a scroll
// feature.
void SetRetentionFrozen(bool frozen);

// Clear all lines, live, retained and pending: on a fresh session start, and on the leave
// funnel, so one lobby's conversation cannot surface in another. Game thread.
void Reset();

// UTF-8-encode a wide string, surrogate pairs included, control characters stripped except
// tab. Shared with the peer-action feed.
std::string ToUtf8(const std::wstring& w);

}  // namespace coop::chat_feed
