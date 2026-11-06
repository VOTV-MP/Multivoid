// coop/moderation/cremate.h -- the /cremate verb: the host sends a player to the crematorium, and the game's own burn
// does the rest.
//
// The host opens the nearest cremator's door and orders the player to its firebox with the TeleportClient order. The
// cremator's burn volume taking the player's puppet in is the landing; the host then shuts the door and, when the
// machine is idle, pulls its lever. The machine kills the player on the player's own screen, the host reads that death
// off the pose stream and ends the player's session with the Cremated end reason. What happens is told to the caller
// and to the other admins as plain lines. This file reaches the commands layer only through the reply function it is
// handed. Game thread throughout.

#pragma once

#include "coop/moderation/moderation.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace coop::moderation::cremate {

// Where a reply line goes: a seat and the occupancy generation it was addressed at. Slot 0 or below is the host's own
// feed. The transport drops a line to a seat whose occupant changed.
struct ReplySeat {
    int      slot = -1;
    uint32_t generation = 0;
};

// One reply line to a seat, now or later.
using ReplyFn = void (*)(const ReplySeat& to, std::string_view line);

// A /cremate, as the command layer hands it over.
struct Order {
    PlayerToken            target;
    std::string            targetNick;  // how the lines name the target, after it left too
    ReplySeat              caller;      // told every outcome
    std::vector<ReplySeat> told;        // the other admins, told the same outcome lines
};

// Started: the trip is under way. The rest refuse and do nothing. NoSession: no running hosted session. Gone: the
// token's seat changed hands. NotAlive: no puppet, or its health reads zero. Busy: the target is already on a trip.
// NoCremator: none with a firebox is loaded. DoorStuck: its door's open did not run. NotSent: the puppet's position,
// the cremator's rotation or the order itself was unavailable (the log says which).
enum class Result : uint8_t { Started, NoSession, Gone, NotAlive, Busy, NoCremator, DoorStuck, NotSent };

// Sends `order.target` to the nearest cremator. `reply` carries every later line.
Result Start(const Order& order, ReplyFn reply);

// Takes the session the order is sent through and registers the listeners (the pose receive's life edge and fresh pose,
// the burn volume's begin-overlap, the host's lever run), once per session start. Game thread.
void Install(coop::net::Session* session);

// Retries the begin-overlap watch until it is registered, then drains what the listeners recorded and moves each trip.
// Game thread, once per pump tick.
void Tick();

// The slot's occupant left: its trip ends ("left before the end"). A caller's departure changes nothing here: the trip
// goes on and the transport drops the lines to its old seat.
void OnDisconnectSlot(int slot);

// The session ended: every trip is dropped without a line.
void OnDisconnect();

}  // namespace coop::moderation::cremate
