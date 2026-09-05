// coop/player/roster_ledger.h -- who is in each peer slot, and everything true of that person
// while they are there: the one game-thread authority the roster snapshot, the nickname
// lookups, the join and leave narration and the moderation actions read from. Peer slots are
// recycled (the session hands out the lowest free slot), so a slot can go from one person to
// the next with no empty moment, and anything watching a per-slot boolean misses both the
// departure and the arrival while the old occupant's state (nickname, skin, colour, voice
// channel, inventory) silently attaches to the new one. So a slot carries an occupancy token,
// and a receiver never observes absence: it observes the current token and conforms to it,
// which heals a lost departure, a fast replacement and a missed edge alike. playerNo is the
// person's session-unique id (0 is empty), what travels on the wire and what the player list
// shows; bornGeneration is the net-layer generation the row was born from, host-side only,
// what a destructive action validates against so a stale capture fails closed. Teardown is a
// row transition: per-person state subscribes once, or declares itself as a PerSlotState,
// which registers its own clear; subscribers receive snapshots of the outgoing and incoming
// rows. Game thread only; the render thread reads the roster's POD snapshot instead.

#pragma once

#include "coop/net/link_kind.h"            // how this occupant's traffic reaches the session
#include "coop/player/players_registry.h"  // kMaxPeers

#include <array>
#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::roster_ledger {

inline constexpr int kMaxSlots = coop::players::kMaxPeers;

// The host's own player number is a role constant, not a draw from the counter: the counter
// starts above it and the host never draws, so re-seeding row 0 can never mint a second.
inline constexpr uint16_t kHostPlayerNo = 1;

// One slot's occupant. playerNo 0 is an empty slot, and every other field is then meaningless.
struct Row {
    uint16_t    playerNo = 0;
    uint32_t    bornGeneration = 0;  // host-side occupancy token; 0 on a client
    std::wstring nick;               // sanitized display name; empty = not yet known
    std::string guid;                // per-player inventory identity (host-side)
    std::string skin;                // body-skin name; empty = the native body
    bool        joinAnnounced = false;  // their "joined the game" line already fired

    // The two connection facts, host-measured and host-published (see RefreshLinkFacts). In the
    // Row rather than a PerSlotState because the Row is what the roster row serialises: a
    // message's fields split across two containers make the next field's home a coin flip.
    coop::net::LinkKind linkKind = coop::net::LinkKind::Unknown;
    int16_t     pingMs = -1;         // RTT to the SESSION in ms; -1 = not sampled

    bool occupied() const { return playerNo != 0; }
};

// Reads.

// The row for `slot`; an out-of-range or empty slot returns a shared empty row, so fields can
// be read unconditionally. Game thread.
const Row& Get(int slot);

struct LinkFacts {
    coop::net::LinkKind kind = coop::net::LinkKind::Unknown;
    int16_t             pingMs = -1;
};

// The connection facts to display for `slot` from this viewer's seat; every board goes through
// here rather than reading the row's fields, or row 0 renders as unknown on a client. Row 0 is
// special: RefreshLinkFacts is host-only and fills row 0 as local with no RTT, honestly, since
// the host's own traffic never crosses a socket. What a client wants on the host's plate is
// the host-to-client link, one link with one RTT the host has already measured and published
// on the viewer's own row, so a client reads its own row's facts for row 0: the same number
// from the same authority, nothing synthesised. On the host, row 0 stays local. Game thread.
LinkFacts DisplayLink(int slot);

// The name to display for `slot`, with the fallback applied: an empty nick yields the
// placeholder rather than a blank label. The one place the fallback lives. Game thread.
const std::wstring& DisplayName(int slot);

// True iff `slot` has an occupant. Game thread.
bool Occupied(int slot);

// The number of occupied slots. Game thread.
int OccupiedCount();

// Occupancy writes, the only entry points that change who is in a slot.

// Install `slot`'s occupant. If the slot held a different person, this is a replacement: the
// outgoing row is torn down and the incoming one installed, in that order, before any
// subscriber runs, so no subscriber observes both people at once. The same playerNo again is a
// no-op, since the repair pulse re-asserts rows constantly. bornGeneration is 0 on a client.
// Game thread.
void InstallRow(int slot, uint16_t playerNo, uint32_t bornGeneration);

// Empty `slot`, firing the transition if it was occupied. Game thread.
void ClearRow(int slot);

// Empty every slot, one transition per occupied row; called at session stop, not start, since
// the ledger must survive until the session that owns it ends, or the window before the next
// start fans out false departures into the menu. Game thread.
void ClearAll();

// An idempotent clear at session start, for a process that reuses the Session object. Game
// thread.
void Reset();

// Host: mint the next player number; session-monotonic, never reused within a session, never 0
// and never kHostPlayerNo. Game thread.
uint16_t MintPlayerNo();

// Host: seed row 0, ourselves, with the host's own local prefs. Idempotent and role-gated; a
// precondition of anything that needs the host to have an identity, rather than something the
// pump is trusted to have ticked first. The prefs are arguments so occupancy and prefs land
// together. The skin is written on every call, like the nick: this row is what the roster
// snapshot sends a joining client, and every other writer of the skin field is an inbound path
// (a Join, a SkinChange, a roster row), which a host never produces for itself; writing it
// every call is also what makes a mid-session re-skin reach a peer who joins afterwards. Game
// thread.
void EnsureRowZeroSeeded(const coop::net::Session& session, const std::wstring& localNick,
                         const std::string& localSkin);

// Field writes: no occupancy change, ignored for an empty slot.

void SetNick(int slot, std::wstring nick);
void SetGuid(int slot, std::string guid);
void SetSkin(int slot, std::string skin);
void SetJoinAnnounced(int slot, bool announced);
void SetLinkFacts(int slot, coop::net::LinkKind kind, int16_t pingMs);

// Host: conform every occupied row's connection facts to what the net layer measures. Row 0 is
// the host itself, local with no ping, since its traffic never crosses a socket; the other
// rows come from the session's link kind and RTT per slot. The cadence is the caller's: call
// it immediately before serialising rows (the repair pulse and the join broadcast), never on a
// free-running clock and never per tick. The link-kind read takes a GNS lock, which a per-tick
// fill would take hundreds of times a second on the game thread against the net poll, and the
// RTT itself updates about once a second, so a faster fill would re-copy an unchanged value;
// filling at send time also keeps the bytes exactly fresh. A row born this tick is filled
// before it is serialised this tick, so a joiner never ships a row with unknown facts. Game
// thread.
void RefreshLinkFacts(coop::net::Session& session);

// The transition fanout.

// A slot's occupant changed: an outgoing playerNo of 0 is a pure arrival, an incoming 0 a pure
// departure, and both non-zero a replacement the subscriber treats as death then birth. Both
// arguments are snapshots taken before the ledger mutated, so reading the departed person's
// nick is safe and ordered.
using SlotReplacedFn = void (*)(int slot, const Row& outgoing, const Row& incoming);

// Register a teardown subscriber, once, at install; subscribers fire in registration order.
// Game thread.
void SubscribeSlotReplaced(SlotReplacedFn fn);

// Per-slot state that cannot be forgotten at teardown: declaring state through this type
// registers its own clear in the constructor, so coverage is by construction. Which container:
// a Row holds a fact the wire row carries, that has a consumer only once an occupant is
// identified, and whose write is re-issued if dropped (nick, skin, the link facts); a
// PerSlotState holds a fact that exists before anyone is identified, or whose write is a
// one-shot latch nothing retries (the Join-sent latch: a client sends its Join to slot 0 before
// row 0 exists). The failure mode of a dropped write separates them: Row setters are
// occupancy-gated, so a write for a slot connected but not yet rowed is silently discarded,
// harmless for a value the next fill re-issues and fatal for a latch. T is reset to a
// value-initialised T when the slot's occupant changes.
template <typename T>
class PerSlotState {
public:
    PerSlotState() { RegisterPerSlotClear(&ClearThunk, this); }
    PerSlotState(const PerSlotState&) = delete;
    PerSlotState& operator=(const PerSlotState&) = delete;

    T& operator[](int slot) { return v_[Clamp(slot)]; }
    const T& operator[](int slot) const { return v_[Clamp(slot)]; }
    int size() const { return kMaxSlots; }

private:
    static int Clamp(int slot) { return (slot < 0 || slot >= kMaxSlots) ? 0 : slot; }
    static void ClearThunk(void* self, int slot) {
        static_cast<PerSlotState*>(self)->v_[Clamp(slot)] = T{};
    }
    std::array<T, kMaxSlots> v_{};
};

// The registration hook behind PerSlotState. The registry is a function-local static, so
// file-scope PerSlotState objects in any TU register before main with no static-init-order
// hazard.
using PerSlotClearFn = void (*)(void* self, int slot);
void RegisterPerSlotClear(PerSlotClearFn fn, void* self);

// The driver.

// Host, per tick: conform the ledger to the net layer's per-slot occupancy generations. A zero
// generation empties the row and a changed one performs the replacement, which covers a
// departure without successor, a fast replacement and a missed edge in one motion; nothing
// else watches for disconnects. Skips slot 0, which is seeded, not connection-derived. Not on
// a client: a client's slots have permanently zero generations, and a reconcile there would
// erase the rows the wire delivered; the client ledger is wire-driven. Game thread.
void ReconcileFromSession(coop::net::Session& session);

}  // namespace coop::roster_ledger
