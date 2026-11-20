// coop/world/sound_relay.h -- a sound the game plays at a place on one machine alone, played there on every peer.
//
// GameplayStatics::PlaySoundAtLocation is a local one-shot. A player's melee hit (mainPlayer_C::attack, the struck
// material's impact sound) is heard only by the hitter, and the vent knocker (kocker_C::knock) and the wandering church's
// bells (church_C), the host's world, only by the host (Ghezra/Multivoid#41, #39). A pre hook on that call finds the Sound
// in the sound table (coop/world/sound_catalog) when the calling Blueprint function is on this lane's list -- a player's
// own (its object this machine's player, either role) or the host's world (sent by the host) -- and sends its index, its
// place, volume and pitch (SoundAt): a client to the host, which judges it (coop/world/sound_judge), plays it and sends
// it to the other clients; the host to all. A client's own copy of a host-world actor plays nothing: the host's sound is
// the one it hears. A receiver plays the table's sound at the same place with the table's attenuation. The relayed play
// has no Blueprint caller, so it is never sent again. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct SoundAtPayload;
}  // namespace coop::net

namespace coop::sound_relay {

// Why a received sound was not played.
enum class Refusal : uint8_t {
    NotReady,   // this peer's sound table is not built
    Digest,     // the sender's table is not this one
    Index,      // the index names no sound
    Caller,     // the host: the sound's entry is not one a client's player plays (a knock, a bell)
    NoBody,     // the host: the sender has no body here that took a pose
    Reach,      // the host: the place is beyond a melee swing's reach of the sender's body
    Budget,     // the host: the sender has asked too often
    kCount
};
const char* RefusalName(Refusal r);

// What this peer has done with sounds, since the process began: for the drill, which decides on these.
struct Counts {
    uint32_t playedHere = 0;      // a catalogued sound the game played here for a listed caller
    uint32_t sent = 0;            // of those, sent
    uint32_t notCatalogued = 0;   // a listed caller's sound the table does not hold for that caller
    uint32_t notReadyHere = 0;    // a listed caller's sound this peer's table was not built for: nothing sent
    uint32_t played = 0;          // a peer's sound played here
    uint16_t lastPlayedHere = 0;  // the index of the last of the first
    uint16_t lastPlayed = 0;      // the index of the last of the fourth
    uint32_t refused[static_cast<int>(Refusal::kCount)] = {};
};
Counts GetCounts();

// Store the session, put the hook on once and arm it while a session is connected. Called every load tick. Game thread.
void Install(coop::net::Session* session);

// Another peer's sound: play it here.
void OnSoundAt(int senderSlot, const coop::net::SoundAtPayload& p);

void OnDisconnect();

}  // namespace coop::sound_relay
