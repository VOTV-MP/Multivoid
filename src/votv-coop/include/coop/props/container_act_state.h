// coop/props/container_act_state.h -- the holds of the host's container acts: a record taken out of a container and
// kept in escrow until its taker commits, and a reservation of room a put is promised. Pure state: no engine call,
// no session, no clock read (callers pass `nowMs`), and at most one hold per slot.
//
// A hold exists only on the host, and only a grant opens one. The ends that restore it (the commit's abort, the
// deadline, a departure, a disconnect) are container_act_host's; this module only answers who holds what.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace coop::props::container_act_state {

// A reply that has not come in this long closes the client's record: an honest wait ends in milliseconds, so the bound
// covers only a transport that refused the reply. The client waits it; the host slows a sender that held a reservation
// longer (a round trip is inside it).
inline constexpr uint64_t kReplyWaitMs = 10000;
// Above the client's reply wait plus a round trip.
inline constexpr uint64_t kHoldDeadlineMs = 30000;

enum class HoldKind : uint8_t { Escrow, Reservation };

struct Hold {
    HoldKind kind = HoldKind::Escrow;
    uint8_t slot = 0;
    uint16_t id = 0;
    uint32_t eid = 0;
    uint16_t index = 0;                       // Escrow: where the record was; Reservation: set from the commit's done
    ue_wrap::save_record::SaveRecord record;  // Escrow: the record taken, not neutered; Reservation: the record asked
    uint64_t grantedMs = 0;
};

// A reservation that ended by its deadline leaves a tombstone: the put it promised room for may still have been done on
// the client, and its late commit is inserted, not lost. It lives one more deadline. No precedent in MTA, whose server
// ignores a confirm that finds no reservation (CGame.cpp:3376); here the replay has already moved the item out of the
// client's inventory, so ignoring the late confirm would lose it.
inline constexpr uint64_t kTombstoneMs = kHoldDeadlineMs;
// The slowing of a sender that holds a container for long and walks away: its next put is answered Lapsed. MTA has no
// deadline and so no penalty; the shape of a per-client budget the sender cannot erase is Source's (player.cpp:3391-3392, sv_maxusrcmdprocessticks).
// A reservation held no longer than the client's own reply wait (10 s) is an honest round trip and is not slowed.
inline constexpr uint64_t kSlowedMs = kHoldDeadlineMs;
inline constexpr size_t kTombstonesPerSlot = 4;

struct Tombstone {
    uint8_t slot = 0;
    uint16_t id = 0;
    uint32_t eid = 0;
    std::wstring className;   // the asked record's class and name: the late commit is checked against them
    std::wstring name;
    size_t askedBytes = 0;    // the asked record's wire size: a late done may be no larger than this plus the slack
    uint64_t atMs = 0;
};

// The hold table. The host uses the one instance `Live()` returns, on the game thread; the selftest builds its own.
struct State {
    // False when h.slot already holds one.
    bool Open(Hold h);

    // A lookup that removes nothing: the slot's hold when its id is `id`, else null. Valid until the next change.
    const Hold* Match(uint8_t slot, uint16_t id) const;

    // Removes and returns the match.
    std::optional<Hold> Close(uint8_t slot, uint16_t id);

    bool SlotHolds(uint8_t slot) const;

    // Another slot has a reservation on this container.
    bool ReservedByOther(uint32_t eid, uint8_t slot) const;

    // Some slot has a record of this container in escrow.
    bool EscrowOn(uint32_t eid) const;

    // Some slot holds anything on this container.
    bool HoldOn(uint32_t eid) const;

    std::vector<uint32_t> HeldEids() const;

    // A reservation `h` ended by its deadline at `nowMs`: its tombstone is kept (at most kTombstonesPerSlot per slot,
    // the oldest of the slot evicted) and the slot is slowed until nowMs + kSlowedMs.
    void Lapse(const Hold& h, uint64_t nowMs);

    // The slot is slowed until nowMs + kSlowedMs: a reservation of its own ended without a timely done after holding
    // more than kReplyWaitMs. The mark ends by time or by the slot's departure, never by the slot's own commit.
    void Slow(uint8_t slot, uint64_t nowMs);

    // Removes the tombstone of (slot, id) on `eid`. None when there is no such tombstone, so a second late commit
    // finds nothing. It does not end the slowing.
    std::optional<Tombstone> ConsumeTombstone(uint8_t slot, uint16_t id, uint32_t eid);

    // The slot is slowed (see Slow).
    bool Slowed(uint8_t slot, uint64_t nowMs) const;

    // Drops what is past its span (a tombstone at atMs + kTombstoneMs, a slowing at its end). Cheap when none stand.
    void SweepLapsed(uint64_t nowMs);
    bool HasLapsed() const;
    size_t TombstoneCount() const;
    void DropLapsedOf(uint8_t slot);   // a departed sender's tombstones and slowing: the slot is reused
    void DropLapsed();

    // grantedMs + kHoldDeadlineMs <= nowMs.
    std::vector<Hold> TakeExpired(uint64_t nowMs);
    std::vector<Hold> TakeForSlot(uint8_t slot);
    std::vector<Hold> TakeForEid(uint32_t eid);
    std::vector<Hold> TakeAll();

    size_t Count() const;

    // Does an act on `eid` by `slot` wait on the holds? Yes when the slot holds one already, when another slot reserves
    // the container, and for a put (`isPut`) when a record of the container is in escrow. A take never waits on an
    // escrow.
    bool WaitsOnHolds(bool isPut, uint32_t eid, uint8_t slot) const;

private:
    std::vector<Hold> TakeIf(bool (*pred)(const Hold&, uint64_t), uint64_t arg);

    struct Slowing {
        uint8_t slot;
        uint64_t untilMs;
    };

    std::vector<Hold> holds_;   // at most one per slot, so a handful
    std::vector<Tombstone> tombstones_;
    std::vector<Slowing> slowed_;
};

State& Live();

}  // namespace coop::props::container_act_state
