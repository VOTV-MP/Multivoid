// coop/creatures/kerfur_color_sync.h -- a custom-colour Kerfur Omega's and Kerfus's colour is the host's.
//
// The colour is the actor's `hsv` (prop_kerfurOmega_col_C, kerfurOmega_col_C, p_kerfus_col_C): the game's picker calls
// the prop's intComs_applyColor, which stores RGBToHSV of the picked colour and repaints that copy alone, so a colour
// one player picked was seen by nobody else. HOST: its own pick, a client's asked pick, a kerfur's change of form and a
// joiner each get the actor's colour (KerfurColorState). CLIENT: its pick is refused on its copy and asked of the host
// (KerfurColorIntent); it writes the host's colour and repaints with the class's own function. Game thread.

#pragma once

namespace coop::net {
class Session;
struct KerfurColorPayload;
struct KerfurColorIntentPayload;
}  // namespace coop::net

namespace coop::kerfur_color_sync {

// Store the session. Idempotent; called at each session start.
void Install(coop::net::Session* session);

// The watches, retried until settled; a client's coalesced asks and its parked colours. Game thread, each pump tick.
void Tick();

// HOST: `actor`'s colour to every peer, when it is a coloured kerfur with an element id; nothing otherwise.
void HostAnnounce(void* actor);

// A client's receiver: an actor's colour.
void OnState(const coop::net::KerfurColorPayload& p);

// The host's receiver: a client's pick.
void OnIntent(int senderSlot, const coop::net::KerfurColorIntentPayload& p);

// HOST: every coloured kerfur's colour, to a joiner at its ready edge.
void SendJoin(int slot);

// The session ended: everything is forgotten.
void OnDisconnect();

}  // namespace coop::kerfur_color_sync
