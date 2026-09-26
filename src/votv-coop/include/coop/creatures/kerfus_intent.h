// coop/creatures/kerfus_intent.h -- a client's own use of a Kerfus, as an intent the host performs.
//
// The Kerfus has three verbs, reached through its E-press (actionOptionIndex) or its named options
// (actionName) on the same branches (p_kerfus.cpp, bp_cfg 2026-09-23): on/off (action 8, "activate",
// needs energy), fix the servers (4, "use", needs it on) and pat (6, "pat", needs it on; more than 50
// quick pats blow it up). All three are refused while it is possessed. A client runs none of the
// Kerfus's brain (kerfus_brain), so a verb run on its copy would flip a field the host never hears of
// -- a Kerfus on here and off there. So a client's gate refuses the verb and sends KerfusIntent with the
// Kerfus's prop eid; the host checks that the Kerfus is one it holds and within the sender's reach,
// and runs the same verb on its own copy, which answers by KerfusState and by whatever the verb sets
// off (a fixed server through the server lane, an explosion through the explosion event). The
// colour variant's own option (14, its colour picker) is not a Kerfus verb and stays local. The
// precedent is the drone console's (coop/interactables/drone_call_intent). Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KerfusIntentPayload;
}  // namespace coop::net

namespace coop::kerfus_intent {

// Watch the two verbs -- a client refuses them; on a host an on/off press is credited to its presser
// (kerfus_follow) -- and cache the session. Idempotent; retried until the gate takes them. Game thread.
void Install(coop::net::Session* session);

// HOST per pump tick: run one queued intent per peer per token. Game thread.
void Tick(coop::net::Session& session);

// HOST: a client asks for a Kerfus verb. Queued here, run from Tick under the rate limit.
void OnIntent(coop::net::Session& session, const coop::net::KerfusIntentPayload& payload, uint8_t senderSlot);

// A peer left: its queue and its rate bucket go.
void OnPeerLeft(uint8_t slot);

// Session end: the queues, the buckets and the counters go.
void OnDisconnect();

// CLIENT: the intents this client has sent this session (the kerfus drill's proof). Game thread.
unsigned long long SentCount();

}  // namespace coop::kerfus_intent
