// coop/interactables/verb_lanes.cpp -- see coop/interactables/verb_lanes.h.

#include "coop/interactables/verb_lanes.h"

#include "coop/interactables/camera_toggle_intent.h"
#include "coop/interactables/coffee_intent.h"
#include "coop/interactables/coffee_state.h"
#include "coop/interactables/cord_sync.h"
#include "coop/interactables/desk_verb_intent.h"
#include "coop/interactables/door_state_verbs.h"
#include "coop/interactables/door_verb_intent.h"
#include "coop/interactables/drone_call_intent.h"
#include "coop/interactables/eraser_press_intent.h"
#include "coop/interactables/keypad_verbs.h"
#include "coop/interactables/lightgroup_verbs.h"
#include "coop/interactables/toggle_verbs.h"
#include "coop/items/point_sack_intent.h"
#include "coop/net/session.h"

#include "ue_wrap/core/walk_timer.h"  // per-lane [WALK-TIME] attribution (diagnostic)

namespace coop::verb_lanes {

void Install(coop::net::Session& session) {
    coop::drone_call_intent::Install(&session);  // a client's press of the drone console is run by the host
    coop::eraser_press_intent::Install(&session);  // a client's press of the drive eraser is run by the host
    coop::door_verb_intent::Install(&session);   // a client's press, hit or pry of a base door is run by the host
    coop::camera_toggle_intent::Install(&session);  // a client's use of a security camera or its laptop light button is run by the host
    coop::door_state_verbs::Install(&session);   // a door's open state moves at doorOpen/doorClose: the host sends, a client refuses its own
    coop::lightgroup_verbs::Install(&session);   // a light group's state moves at its runTrigger: the host sends, a client refuses its own
    coop::toggle_verbs::Install(&session);       // a symmetric device's state goes at the verb that writes it: each peer sends its own
    coop::keypad_verbs::Install(&session);       // a keypad's verbs: the host sends each, a client's own entries run on the host
    coop::desk_verb_intent::Install(&session);   // a client's press on the desk's save family is run by the host
    coop::point_sack_intent::Install(&session);  // a client's opened point sack is paid by the host
    coop::cord_sync::Install(&session);          // a cord's link is the host's: a client claims its own player's plug, the host answers
    coop::coffee_intent::Install(&session);      // a client's press of the coffee vending machine or the coffee maker is run by the host
    coop::coffee_state::Install(&session);       // the coffee maker's state is the host's: it publishes the maker's record, a client's fill is cancelled
}

void Tick(coop::net::Session& session) {
    { ue_wrap::ScopedWalkTimer _w{"sync:drone_call"}; coop::drone_call_intent::Tick(session); }  // HOST: run one queued drone-console press a tick a client
    coop::eraser_press_intent::Tick(session);  // HOST: run one queued eraser press a tick a client, once its drive is seated
    { ue_wrap::ScopedWalkTimer _w{"sync:door_verb"}; coop::door_verb_intent::Tick(session); }    // HOST: run one queued door verb a tick a client
    { ue_wrap::ScopedWalkTimer _w{"sync:camera_toggle"}; coop::camera_toggle_intent::Tick(session); }  // settle its gates; HOST: run one queued camera request a tick a client
    coop::door_state_verbs::Tick();     // settle the door state watches
    coop::lightgroup_verbs::Tick();     // settle the light group watch
    coop::toggle_verbs::Tick();         // settle the toggle verb watches
    coop::point_sack_intent::Tick();    // settle the point sack watch
    coop::keypad_verbs::Tick(session);  // settle the keypad watches; HOST: run queued keypad intents
    { ue_wrap::ScopedWalkTimer _w{"sync:desk_verb"}; coop::desk_verb_intent::Tick(session); }  // settle its seams; HOST: run one queued desk press a tick a client
    { ue_wrap::ScopedWalkTimer _w{"sync:cord"}; coop::cord_sync::Tick(); }  // settle its watches; HOST: the seed a joiner is owed; CLIENT: retry the parked links
    { ue_wrap::ScopedWalkTimer _w{"sync:coffee"}; coop::coffee_intent::Tick(session); }  // settle its gates; HOST: run one queued coffee press a tick a client
    coop::coffee_state::Tick();         // settle the maker's brew and fill gates
}

void ConnectReplayForSlot(int slot) {
    coop::cord_sync::QueueConnectBroadcastForSlot(slot);  // HOST: every corded prop's link to the joiner, then the seed's end
}

void OnDisconnect() {
    coop::drone_call_intent::OnDisconnect();  // its own console button stands
    coop::eraser_press_intent::OnDisconnect();  // its own eraser button stands
    coop::door_verb_intent::OnDisconnect();   // a door's own verbs run where they are used
    coop::camera_toggle_intent::OnDisconnect();  // a camera's own press runs where it is made
    coop::door_state_verbs::OnDisconnect();
    coop::lightgroup_verbs::OnDisconnect();
    coop::toggle_verbs::OnDisconnect();
    coop::keypad_verbs::OnDisconnect();
    coop::desk_verb_intent::OnDisconnect();
    coop::point_sack_intent::OnDisconnect();
    coop::cord_sync::OnDisconnect();
    coop::coffee_intent::OnDisconnect();   // a coffee device's own press runs where it is made
    coop::coffee_state::OnDisconnect();    // the maker's own fill runs where it is made
}

}  // namespace coop::verb_lanes
