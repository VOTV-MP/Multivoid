// coop/interactables/verb_lanes.cpp -- see coop/interactables/verb_lanes.h.

#include "coop/interactables/verb_lanes.h"

#include "coop/interactables/door_state_verbs.h"
#include "coop/interactables/door_verb_intent.h"
#include "coop/interactables/drone_call_intent.h"
#include "coop/interactables/keypad_verbs.h"
#include "coop/interactables/lightgroup_verbs.h"
#include "coop/interactables/toggle_verbs.h"
#include "coop/net/session.h"

#include "ue_wrap/core/walk_timer.h"  // per-lane [WALK-TIME] attribution (diagnostic)

namespace coop::verb_lanes {

void Install(coop::net::Session& session) {
    coop::drone_call_intent::Install(&session);  // a client's press of the drone console is run by the host
    coop::door_verb_intent::Install(&session);   // a client's press, hit or pry of a base door is run by the host
    coop::door_state_verbs::Install(&session);   // a door's open state moves at doorOpen/doorClose: the host sends, a client refuses its own
    coop::lightgroup_verbs::Install(&session);   // a light group's state moves at its runTrigger: the host sends, a client refuses its own
    coop::toggle_verbs::Install(&session);       // a symmetric device's state goes at the verb that writes it: each peer sends its own
    coop::keypad_verbs::Install(&session);       // a keypad's verbs: the host sends each, a client's own entries run on the host
}

void Tick(coop::net::Session& session) {
    { ue_wrap::ScopedWalkTimer _w{"sync:drone_call"}; coop::drone_call_intent::Tick(session); }  // HOST: run one queued drone-console press a tick a client
    { ue_wrap::ScopedWalkTimer _w{"sync:door_verb"}; coop::door_verb_intent::Tick(session); }    // HOST: run one queued door verb a tick a client
    coop::door_state_verbs::Tick();     // settle the door state watches
    coop::lightgroup_verbs::Tick();     // settle the light group watch
    coop::toggle_verbs::Tick();         // settle the toggle verb watches
    coop::keypad_verbs::Tick(session);  // settle the keypad watches; HOST: run queued keypad intents
}

void OnPeerLeft(uint8_t slot) {
    coop::drone_call_intent::OnPeerLeft(slot);  // its console presses
    coop::door_verb_intent::OnPeerLeft(slot);   // its door verbs
    coop::keypad_verbs::OnPeerLeft(slot);       // its keypad entries
}

void OnDisconnect() {
    coop::drone_call_intent::OnDisconnect();  // its own console button stands
    coop::door_verb_intent::OnDisconnect();   // a door's own verbs run where they are used
    coop::door_state_verbs::OnDisconnect();
    coop::lightgroup_verbs::OnDisconnect();
    coop::toggle_verbs::OnDisconnect();
    coop::keypad_verbs::OnDisconnect();
}

}  // namespace coop::verb_lanes
