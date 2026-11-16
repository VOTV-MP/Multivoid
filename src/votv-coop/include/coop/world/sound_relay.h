// coop/world/sound_relay.h -- a sound the game plays at a place on one machine alone, played there on every peer.
//
// GameplayStatics::PlaySoundAtLocation is a local one-shot. A player's melee hit (mainPlayer_C::attack, the struck
// material's impact sound) is heard only by the hitter, and the vent knocker (kocker_C::knock), an event actor that
// lives on the host alone, only by the host (Ghezra/Multivoid#41, #39). A pre hook on that call reads the Sound, its
// place, volume, pitch and attenuation when the calling Blueprint function is on this lane's list -- a player's own
// (its object this machine's player, either role) or the host's world (sent by the host) -- and sends them (SoundAt):
// a client to the host, which plays it and relays it to the other clients; the host to all. A receiver loads the two
// assets by path (cached) and plays the same sound at the same place. The relayed play has no Blueprint caller, so it
// is never sent again. Game thread.

#pragma once

namespace coop::net {
class Session;
struct SoundAtPayload;
}  // namespace coop::net

namespace coop::sound_relay {

// Store the session, put the hook on once and arm it while a session is connected. Called every load tick. Game thread.
void Install(coop::net::Session* session);

// Another peer's sound: play it here.
void OnSoundAt(int senderSlot, const coop::net::SoundAtPayload& p);

void OnDisconnect();

}  // namespace coop::sound_relay
