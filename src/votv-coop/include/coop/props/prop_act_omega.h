// coop/props/prop_act_omega.h -- an Omega's put-on and take-off of an outfit item: two entries of prop_act's family registry
// (coop/props/prop_act_families).
//
// A client's kerfurOmega_C.playerUsedOn (the put-on of the held item) and .upgradeTake (the take-off of a worn one) are
// cancelled at their PRE and asked of the host as the families OmegaPutOn and OmegaTakeOff. The host judges them as prop_act
// judges its own verbs, then by the game's equipItem rules for the row and, for a put-on, the hand lane's record of the hold.
//   PUT-ON (apply at the commit): the grant makes the client replay its own playerUsedOn under prop_act's latch; it commits 1
//   only when its copy wears the row and its hand lost the hold. The host then spends the hold the judge recorded and writes
//   the row with updateDrip, whose POST sends the outfit to every client. A commit of 0, or a row the host cannot write,
//   puts the asker's copy back. MTA's vehicle entry has this shape (CGame.cpp:3079-3196, 3373).
//   TAKE-OFF (apply at the grant): no replay, no commit: the host runs unequipItem on its copy before it answers the grant,
//   and one it cannot write is answered a refusal, as MTA's vehicle exit changes the state first and answers the requester
//   a failure (CGame.cpp:3475-3508). It has no animation to wait for and drops an item that must exist once.
// The entries register themselves; nothing names them but the wire's family bytes. Game thread.

#pragma once

#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "coop/props/prop_act_host.h"

#include <cstdint>
#include <string>

namespace coop::prop_act_omega {

// The host's evidence of a put-on is the hold the client announces, which it can forge: the host has no record of a client's
// inventory (MTA's server owns a player's weapon table, CPlayerPuresyncPacket.cpp:215-231; the same limit as hand_spend.h).
// A forged hold buys a put-on per prop_act token, cosmetic and free; the one mint is the take-off that then drops the row's
// item on the host. Its bound: {burst 2, 1 per 30 s} per slot, 120 an hour. The slot's state ends with the peer, so a rejoin
// starts its budget anew (a handshake and a world-ready each time), and N seated clients multiply the bound. A take-off past
// it is answered QueueFull's line ("too many actions at once, try again"), not the wrong-item line.
inline constexpr coop::net::IntentBudget kDropBudget{2.0f, 1.0f / 30.0f};

namespace internal {   // pure: prop_act_selftest drives these
// The game's equipItem (kerfurOmega_C): a row of list_kerfurDrip, of type 0 (an accessory on a bone: the bone must be free)
// or 1 (a mesh), not already worn, and not kerfoacc_pant, whose put-on quits the game.
enum class Refusal : uint8_t { None, NotARow, Pant, Type, Worn, Bone };
Refusal PutOnRefusal(bool rowFound, uint8_t type, bool isPant, bool worn, bool boneTaken);
const char* RefusalName(Refusal r);
coop::prop_act_host::Tally TermOf(Refusal r);   // the tally term of a refusal other than None
// A wire row: 1..27 printable ASCII characters, a NUL, and zeros to the end. False for anything else.
bool RowText(const char (&row)[coop::net::kPropActVerbBytes], std::wstring& out);
}  // namespace internal

}  // namespace coop::prop_act_omega
