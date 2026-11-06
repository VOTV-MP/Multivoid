// coop/creatures/kerfur_command_answer.h -- the host's answer to a kerfur command it did not run at once.
//
// A client's radial command is cancelled where it is picked (kerfur_command_seat) and asked of the host, which runs
// the game's own verb for it or does not: a command the host judged and refused (the kerfur on a job, gone, in kill
// mode, the sender's queue full) and a command that waits for the host's body of its sender are answered to the
// requester alone as KerfurCommandAnswer, naming the request by the id the client sent. The refusal crosses as an
// enum and the client words it through the l10n catalogue and shows it as the game's own refusal hint, since its own
// copy of the verb, which would have raised that hint, never ran. A malformed or flooding request is dropped and
// counted by kerfur_command and never answered. The client keeps the ids it sent that have no answer yet, a few, and
// drops an answer that names none of them.
//
// Principle 7: gameplay/network module; engine access through ue_wrap/world only. Page: docs/npcs-and-kerfur.md.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KerfurCommandAnswerPayload;
}  // namespace coop::net

namespace coop::kerfur_command_answer {

// CLIENT: the number of the command about to be sent, from 1 and per session, held as sent until its answer or until
// newer ones push it out. Game thread.
uint32_t NextRequestId();

// HOST: tell `slot` its command `requestId` was refused for `reason` (a KerfurRefusal), or waits. A request with no id
// (0), the host's own slot, or an unknown reason is not answered. Game thread.
void SendRefused(coop::net::Session* session, uint8_t slot, uint32_t requestId, uint8_t reason);
void SendWaits(coop::net::Session* session, uint8_t slot, uint32_t requestId);

// CLIENT: the host's answer: shown as a hint in the player's language when it names a command sent here, else dropped
// and counted. Game thread.
void OnAnswer(const coop::net::KerfurCommandAnswerPayload& p);

// Session end: the counts are said and the held ids forgotten.
void OnDisconnect();

}  // namespace coop::kerfur_command_answer
