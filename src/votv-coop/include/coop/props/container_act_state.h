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
#include <vector>

namespace coop::props::container_act_state {

// Above the client's 10 s reply wait plus a round trip.
inline constexpr uint64_t kHoldDeadlineMs = 30000;

enum class HoldKind : uint8_t { Escrow, Reservation };

struct Hold {
    HoldKind kind = HoldKind::Escrow;
    uint8_t slot = 0;
    uint16_t id = 0;
    uint32_t eid = 0;
    uint16_t index = 0;                       // Escrow: where the record was; Reservation: unused
    ue_wrap::save_record::SaveRecord record;  // Escrow: the record taken, not neutered; Reservation: the record asked
    uint64_t grantedMs = 0;
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

    std::vector<uint32_t> HeldEids() const;

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

    std::vector<Hold> holds_;   // at most one per slot, so a handful
};

State& Live();

}  // namespace coop::props::container_act_state
