// coop/interactables/camera_toggle_intent.h -- a client's own use of a security camera, or press of the laptop's
// camera-light button, runs on the host.
//
// A camera's two bits are the host's (CameraActiveState, CameraLightState: coop/interactables/interactable_sync).
// A player writes them at two bodies: the camera's actionOptionIndex with the use action (it negates `active`) and
// the laptop widget's camera-light click (it negates `isLight` on the camera selected there). On a client both are
// refused and the press is sent here; the host judges the sender's reach and sets the bit to the value that client
// wanted, and its camera lanes send the result to every peer. A bit already at that value changes nothing and is
// sent again to the requester alone, so a client whose copy diverged converges and its button is never dead. The
// cursed camera rolls `active` in its own tick: a client's tick is refused, the host's result crosses through the
// lane. A press on a camera the lane does not index (an empty or class-default key, or not scanned yet) is refused
// and sends nothing. MTA precedent: SetVehicleEngineState, a server-held bit carried to joiners in the element's
// add packet (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:7260). The intent's
// shape is the door's (coop/interactables/door_verb_intent).

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct CameraToggleIntentPayload;
}  // namespace coop::net

namespace coop::camera_toggle_intent {

// Registers the three refusals' name watches. Called every pump tick (coop/interactables/verb_lanes) and latched:
// a watch registers once, and one the gate refused stays refused for the process, said once. Game thread.
void Install(coop::net::Session* session);

// Both roles: settles the watches and says once when all are live. Host: runs the queued requests in order,
// each sender at a bounded rate from its own IntentQueue; a sender whose first pose has not arrived has no body
// to measure its reach from, and its requests wait for it. Game thread, once per pump tick.
void Tick(coop::net::Session& session);

// Host: a client's request from the wire, its format already checked by the dispatcher. Queued, and run by
// Tick; a full queue refuses it. Game thread.
void OnCameraToggleIntent(coop::net::Session& session, const coop::net::CameraToggleIntentPayload& payload,
                          uint8_t senderSlot);

// CLIENT: how many requests this client has sent the host this session, for a drill that reads whether a press
// went. Game thread.
uint64_t SentCount();

// The session ended: the counters, with one summary line. A sender's queue, its rate and its said-once latches
// live in its per-slot state and go with its occupant.
void OnDisconnect();

}  // namespace coop::camera_toggle_intent
