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

// Per-slot person-state that CANNOT be forgotten at teardown: declaring state
// through this type registers its own clear in the constructor, so coverage is
// by construction rather than by anyone remembering to add a line to a
// checklist. Use it for anything keyed by peer slot that describes a PERSON
// (their preferences, their cached state) rather than the transport.
//
//   coop::roster_ledger::PerSlotState<MyState> g_bySlot;   // that is the whole wiring
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
