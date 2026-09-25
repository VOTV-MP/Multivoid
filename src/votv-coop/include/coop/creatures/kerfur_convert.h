// coop/creatures/kerfur_convert.h -- host-authoritative kerfur conversion, NPC to prop and back,
// decided at its verb. The game's verbs: turn off runs dropKerfurProp, which spawns the prop (and a
// floppy if carried) at the NPC's transform, or at (0,0,20000) in the flesh room, and destroys the
// NPC, refused for a sentient kerfur; turn on runs spawnKerfuro, which spawns the NPC upright 50 cm
// above the prop's spawn point, keeping only its yaw, and destroys the prop. Both are Blueprint calls
// ProcessEvent never sees, and the script-body gate sees on every route. So a client's gate refuses
// the verb and asks the host, and no form of its own is ever made; the host's verb, its own or a
// request's, converges at its return on the successor its bracket captured (kerfur_form_assembler) --
// MTA's shape for an entity a client asks to change: a request, then the server's confirmation
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3018, Packet_Vehicle_InOut). The host
// executor lives in kerfur_convert_host.h, the client apply in kerfur_convert_client.h. Gameplay
// module: engine access through ue_wrap, game thread only.

#pragma once

#include "coop/element/element.h"  // ElementId (TryCaptureKerfurPropDestroy dyingEid)

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::kerfur_convert {

// Idempotent install, retried from the pump tick until the kerfur classes load: resolves the menu
// dispatcher (for the command relay) and its name parameter, the two verbs and the kill flag offset
// for the host's request path; registers the relay's interceptor and this lane's gate watches on the
// two verbs. Refuses to install if a verb grew parameters.
void Install(coop::net::Session* session);

// The host executor half lives in kerfur_convert_host.h, the client half in kerfur_convert_client.h.

// The host's answer at the generic destroy of a kerfur prop-form actor. The turn-on verb spawns the
// NPC and then destroys the prop, so the prop's destroy seam fires inside the verb with the successor
// already captured: that death is the conversion's, whose return converges it into one KerfurConvert,
// and the caller must not broadcast a destroy (true). A kerfur prop that dies outside a conversion
// verb, or inside one that captured no successor, is a plain death: its kerfur record goes with it and
// the generic relay proceeds (false). A client never runs a conversion verb, so on a client this is
// always false. Game thread, the destroy seam.
bool TryCaptureKerfurPropDestroy(void* actor, coop::element::ElementId dyingEid);

// Clear the open verbs and fan the disconnect to the host half.
void OnDisconnect();

}  // namespace coop::kerfur_convert
