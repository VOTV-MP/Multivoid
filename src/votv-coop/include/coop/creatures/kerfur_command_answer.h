// coop/creatures/kerfur_command_answer.h -- the host's answer to a kerfur command it did not run at once.
//
// A client's radial command is cancelled where it is picked (kerfur_command_seat) and asked of the host, which runs
// the game's own verb for it or does not: a command the host judged and refused (the kerfur on a job, gone, in kill
// mode, out of reach, the sender's queue full) and a command that waits for the host's body of its sender are answered to the
// requester alone as KerfurCommandAnswer, naming the request by the id the client sent. The refusal crosses as an
// enum and the client words it through the l10n catalogue and shows it as the game's own refusal hint, since its own
// copy of the verb, which would have raised that hint, never ran. A command that never reached the host (its Omega
// untracked here, the send failed) is worded locally, with no wire. A malformed request is dropped and counted by
// kerfur_command; a flood is answered once a streak. The client keeps the ids it sent that have no answer yet, a few,
// and drops an answer that names none of them. A get_reports that carried the client's held disc and took it is answered
// the same way (consumed-held), on which the client destroys the disc it pledged (kerfur_disc_carry), and the pledge
// is kept with the id.
//
// Principle 7: gameplay/network module; engine access through ue_wrap/world only. Page: docs/npcs-and-kerfur.md.

#pragma once

#include "coop/creatures/kerfur_disc_carry.h"

#include <cstdint>
#include <optional>

namespace coop::net {
class Session;
struct KerfurCommandAnswerPayload;
}  // namespace coop::net

namespace coop::kerfur_command_answer {

// CLIENT: the number of the command about to be sent, from 1 and per session. A command that went is held with
// HoldSent until its answer or until newer ones push it out; one that did not go is never held. A command that carried
// the held disc holds its pledge with the id: the pledge ends with the id's refusal, its eviction and the session. Game
// thread.
uint32_t NextRequestId();
void HoldSent(uint32_t requestId, std::optional<coop::kerfur_disc_carry::Pledge> pledge = std::nullopt);

// CLIENT: the player is told a command this machine cancelled at the seat went nowhere (a KerfurRefusal value, as the
// host's answer would word it): its Omega is untracked here, or the send failed. No wire. Game thread.
void ShowLocal(uint8_t reason);

// HOST: tell `slot` its command `requestId` was refused for `reason` (a KerfurRefusal), or waits. A request with no id
// (0), the host's own slot, or an unknown reason is not answered. Game thread.
void SendRefused(coop::net::Session* session, uint8_t slot, uint32_t requestId, uint8_t reason);
void SendWaits(coop::net::Session* session, uint8_t slot, uint32_t requestId);
// HOST: tell `slot` its command `requestId` ran on the disc it carried and the Omega took it (consumed-held). A refused
// send is kept and sent again each tick (TickOwed) until accepted or the slot leaves (OnPeerGone, said): this answer is
// the only thing that makes the client let go of its disc once the Omega holds it. Game thread.
void SendConsumed(coop::net::Session* session, uint8_t slot, uint32_t requestId);
void TickOwed(coop::net::Session* session);
void OnPeerGone(uint8_t slot);

// CLIENT: the host's answer: shown as a hint in the player's language when it names a command sent here, else dropped
// and counted; a consumed-held ends the pledge of its command and destroys the pledged disc here (kerfur_disc_carry).
// Game thread.
void OnAnswer(const coop::net::KerfurCommandAnswerPayload& p);

// Instruments (the kerfur serve drill, leg D), this session: consumed-held answers that destroyed the pledged disc here,
// and those that did not (the hand held another thing, or the command pledged none), and the refusals shown as hints.
uint64_t ConsumedHere();
uint64_t ConsumeMisses();
uint64_t RefusalsShown();

// Session end: the counts are said and the held ids and their pledges forgotten.
void OnDisconnect();

}  // namespace coop::kerfur_command_answer
