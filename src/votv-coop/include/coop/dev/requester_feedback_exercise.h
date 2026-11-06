// coop/dev/requester_feedback_exercise.h -- [dev] the requester feedback's hint, exercised with no verb to run it.
//
// The host, once a client is world-ready and the feedback's seams stand, opens a requester-feedback scope for slot 1
// and raises one fixed hint (type 1) inside it, the call a verb run for that client would make. Routed, the host logs
// `[REQUESTER-FEEDBACK] routed hint type=1 to slot 1` and shows none, and the client logs `[REQUESTER-FEEDBACK] showed
// hint "Requester feedback exercise"`. The exerciser's own line says what the host's counter did:
//   [RF-EXERCISE] host raised its hint inside slot 1's scope: routed=<0|1>
// Once a session. Ini requester_feedback_exercise=1 or VOTVCOOP_REQUESTER_FEEDBACK_EXERCISE=1 on the host; with
// requester_feedback_off=1 (the red arm) nothing is routed and the host shows the hint. Off, a latched read. Game
// thread.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::requester_feedback_exercise {

// Each pump tick: the host's one raise once its conditions hold.
void Install(coop::net::Session* session);

// Session end: the once-latch clears.
void OnDisconnect();

}  // namespace coop::dev::requester_feedback_exercise
