// coop/player/roster_ledger.h -- WHO is in each peer slot, and everything that
// is true of that person while they are there.
//
// Gameplay/network layer (principle 7). Design of record:
// research/findings/join-identity/votv-nickname-arbitration-roster-id-DESIGN-2026-07-27.md
// (arc A). This is the single game-thread authority the roster snapshot, the
// nickname lookups, the join/leave narration and the moderation actions all read
// from; before it, each of those derived presence independently and they
// disagreed (a client's TAB listed only itself and the host).
//
// THE PROBLEM IT EXISTS FOR. Peer slots are RECYCLED --
// Session::FindFreePeerSlotForClient hands out the lowest free slot -- so slot 2
// can go from person X to person Y with no empty moment in between. Anything
// that watches a per-slot BOOLEAN ("is slot 2 connected?") cannot see that: the
// boolean reads true before and after, so the departure and the arrival both
// vanish, and X's leftover state (nickname, skin, colour, voice channel,
// inventory) silently becomes attributed to Y.
//
// THE ANSWER: a slot carries an OCCUPANCY TOKEN, and a receiver never observes
// absence -- it observes the CURRENT token and conforms to it. One mechanism
// therefore heals a lost departure, a fast replacement, and a missed edge.
//   - `playerNo` is the person's session-unique ID (0 = the slot is empty). It
//     is what travels on the wire and what TAB shows.
//   - `bornGeneration` is the net-layer generation the row was born from
//     (Session::peerGenerationForSlot). HOST-side only; it is what a destructive
//     action validates against so a stale capture fails CLOSED.
//
// TEARDOWN IS A ROW TRANSITION, not a disconnect callback. Anything that holds
// per-person state subscribes once (SubscribeSlotReplaced) or -- better --
// declares that state as a PerSlotState<T>, which registers ITSELF and is
// therefore covered by construction rather than by anyone remembering to add a
// line. Subscribers receive SNAPSHOTS of the outgoing and incoming rows, so they
// can read the departed person's fields without racing the clear.
//
// Game thread only. Every accessor asserts it: the rows hold std::wstring /
// std::string, and the render thread reads roster's POD snapshot instead.

#pragma once

#include "coop/net/link_kind.h"            // how this occupant's traffic reaches the session
#include "coop/player/players_registry.h"  // kMaxPeers

#include <array>
#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::roster_ledger {

inline constexpr int kMaxSlots = coop::players::kMaxPeers;

// The host's own player number is a ROLE CONSTANT, not a draw from the counter:
// the counter starts above it and the host never draws, so re-seeding row 0 can
// never mint a second #1.
inline constexpr uint16_t kHostPlayerNo = 1;

// One slot's occupant. A row with playerNo == 0 is an EMPTY slot; every other
// field is then meaningless and must not be displayed.
struct Row {
    uint16_t    playerNo = 0;
    uint32_t    bornGeneration = 0;  // host-side occupancy token; 0 on a client
    std::wstring nick;               // sanitized display name; empty = not yet known
    std::string guid;                // v73 per-player inventory identity (host-side)
    std::string skin;                // v93 body-skin name; empty = native kel
    bool        joinAnnounced = false;  // their "joined the game" line already fired

    // v131 -- the two CONNECTION facts, host-measured and host-published (see
    // RefreshLinkFacts). They live in the Row, not in a PerSlotState, because
    // the Row is what BuildRowForSlot SERIALIZES: splitting a message's fields
    // across two containers means serialization reads two places and the next
    // field's home becomes a coin flip. (The person-vs-transport wording that
    // used to sit on PerSlotState below was already false against the shipped
    // `joinSent`; the working rule is stated there.)
    coop::net::LinkKind linkKind = coop::net::LinkKind::Unknown;
    int16_t     pingMs = -1;         // RTT to the SESSION in ms; -1 = not sampled

    bool occupied() const { return playerNo != 0; }
};

// --- reads -------------------------------------------------------------------

// The row for `slot`. Out-of-range or empty slots return a shared empty row, so
// callers may read fields unconditionally. Game thread.
const Row& Get(int slot);

// The name to DISPLAY for `slot`, fallback already applied -- an empty nick
// yields the placeholder rather than a blank label. This is the one place the
// fallback lives; callers must not re-invent it (six sites used to, and they
// disagreed). Game thread.
const std::wstring& DisplayName(int slot);

// True iff `slot` currently has an occupant. Game thread.
bool Occupied(int slot);

// The number of occupied slots. Game thread.
int OccupiedCount();

// --- occupancy writes (the ONLY entry points that change who is in a slot) ---

// Install `slot`'s occupant. If the slot already held a DIFFERENT person
// (playerNo differs), this is a REPLACEMENT: the outgoing row is torn down and
// the incoming one installed atomically, in that order, before any subscriber
// runs -- so no subscriber can observe both people at once. Installing the SAME
// playerNo again is a no-op (the repair pulse re-asserts rows constantly).
// `bornGeneration` is 0 on a client. Game thread.
void InstallRow(int slot, uint16_t playerNo, uint32_t bornGeneration);

// Empty `slot`, firing the transition if it was occupied. Game thread.
void ClearRow(int slot);

// Empty every slot, one transition per occupied row. Called at session STOP (not
// at start): the ledger must survive until the session that owns it ends,
// otherwise the window between stop and the next start fans out false
// departures into the menu. Game thread.
void ClearAll();

// Belt-and-braces idempotent clear at session START, for a process that reuses
// the Session object. Game thread.
void Reset();

// HOST-side: mint the next player number. Session-monotonic, never reused within
// a session, never 0, and never kHostPlayerNo. Game thread.
uint16_t MintPlayerNo();

// HOST-side: seed row 0 (ourselves) with `localNick`. Idempotent and role-gated;
// a PRECONDITION of anything that needs the host to have an identity, rather
// than something the pump is trusted to have ticked first. The nick is an
// argument so occupancy and name land together and the pairing cannot be
// forgotten at a call site. Game thread.
void EnsureRowZeroSeeded(const coop::net::Session& session, const std::wstring& localNick);

// --- field writes (no occupancy change; ignored for an empty slot) -----------

void SetNick(int slot, std::wstring nick);
void SetGuid(int slot, std::string guid);
void SetSkin(int slot, std::string skin);
void SetJoinAnnounced(int slot, bool announced);
void SetLinkFacts(int slot, coop::net::LinkKind kind, int16_t pingMs);

// HOST-side: conform every occupied row's connection facts to what the net layer
// currently measures. Row 0 is the host itself -- LinkKind::Local, no ping,
// because their traffic never crosses a socket and there is no RTT to report;
// rows 1..3 come from Session::LinkKindForSlot + rttMsForSlot.
//
// CADENCE IS THE CALLER'S: call this IMMEDIATELY BEFORE serializing rows (the
// repair pulse, and the join broadcast), never on a free-running clock and never
// per tick. Two reasons, both measured: LinkKindForSlot takes a GNS lock via
// GetConnectionInfo, and a per-tick fill would take it ~375x/s on the GAME
// thread against a 200 Hz net Poll(); and rttMsBySlot_ itself only updates ~1 Hz
// (session.cpp, nextRttSample), so a faster fill would re-copy an unchanged
// value. Filling at send time also means the bytes are exactly fresh and there
// is no second clock to drift against the pulse.
//
// A row born this tick is therefore filled before it is serialized this tick,
// which is why a joiner never ships a row with Unknown/-1 in it. Game thread.
void RefreshLinkFacts(coop::net::Session& session);

// --- the transition fanout ---------------------------------------------------

// A slot's occupant changed. `outgoing.playerNo == 0` means the slot was empty
// (a pure arrival); `incoming.playerNo == 0` means it is now empty (a pure
// departure); both non-zero is a REPLACEMENT and the subscriber must treat it as
// death-then-birth. Both arguments are SNAPSHOTS taken before the ledger
// mutated, so reading the departed person's nick here is safe and ordered.
using SlotReplacedFn = void (*)(int slot, const Row& outgoing, const Row& incoming);

// Register a teardown subscriber. Call once, at install/boot time. Subscribers
// fire in registration order. Game thread.
void SubscribeSlotReplaced(SlotReplacedFn fn);

// Per-slot state that CANNOT be forgotten at teardown: declaring state through
// this type registers its own clear in the constructor, so coverage is by
// construction rather than by anyone remembering to add a line to a checklist.
//
//   coop::roster_ledger::PerSlotState<MyState> g_bySlot;   // that is the whole wiring
//
// WHICH CONTAINER? This said "use it for what describes a PERSON rather than the
// transport" until 2026-07-28, and that was already false against the shipped
// `joinSent` -- a pure LINK fact that lives here because a client sends its Join
// to slot 0 BEFORE row 0 exists. Category is the wrong test. The working one:
//
//   Row            -- a fact the wire row CARRIES, that only has a consumer once
//                     an occupant is identified, and whose write is RE-ISSUED if
//                     it is ever dropped (nick, skin, linkKind, pingMs).
//   PerSlotState   -- a fact that exists BEFORE anyone is identified, OR whose
//                     write is a one-shot LATCH that nothing retries (joinSent).
//
// The failure mode of a DROPPED write is what separates them. Row setters are
// occupancy-gated, so a write for a slot that is connected but not yet rowed is
// silently discarded: harmless for a value the next fill re-issues, fatal for a
// latch -- `joinSent` in a Row meant the client re-sent its Join every tick for
// the whole session, in silence.
//
// T is reset to a value-initialized T when the slot's occupant changes.
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

// Registration hook behind PerSlotState (not for direct use). The registry is a
// function-local static so file-scope PerSlotState objects in any TU can
// register before main without a static-init-order hazard.
using PerSlotClearFn = void (*)(void* self, int slot);
void RegisterPerSlotClear(PerSlotClearFn fn, void* self);

// --- the driver --------------------------------------------------------------

// HOST-side per-tick reconcile: conform the ledger to the net layer's per-slot
// occupancy generations. A zero generation empties the row; a changed one
// performs the replacement. This covers departure-without-successor, fast
// replacement and a missed edge in one motion -- which is why nothing else needs
// to watch for disconnects.
//
// Skips slot 0 (host-self is seeded, not connection-derived). Does NOT run on a
// client: a client's slots 1..3 have permanently zero generations, so a
// reconcile there would erase exactly the rows the wire just delivered -- the
// client ledger is entirely wire-driven. Game thread, once per tick.
void ReconcileFromSession(coop::net::Session& session);

}  // namespace coop::roster_ledger
