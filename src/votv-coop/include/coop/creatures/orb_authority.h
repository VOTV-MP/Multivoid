// coop/creatures/orb_authority.h -- the black orb's encounter (npc_orborb_C) is the host's.
//
// In the game a mainPlayer_C's touch calls the orb's begin (it stops and plays timeline "b"), and the timeline's end
// reads player 0's camera: the orb behind it destroys itself, the orb in front ends the run with a quit. Here a client's
// copy never reacts to a touch and its timeline end runs nothing. The host lets its own player's touch run, begins the
// encounter itself for a client's body, and sends every client the begin (OrbEvent Begin) so each copy runs its own.
// At the timeline's end the host reads the toucher's view (its camera, or a client's streamed eye and aim) with the
// orb's test and destroys the orb either way; a caught toucher is revived through coop/player/death_revive, on the host
// directly or on the client by OrbEvent Death, instead of the game's quit. The orb's spawner counts player 0's laps
// around it; a client refuses its tick (coop/world/spawn_authority), and the host counts each client's laps too, from
// the client's streamed eye, by the spawner's own rule, minting that client's orb as the spawner would. Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct OrbEventPayload;
}  // namespace coop::net
namespace ue_wrap::script_gate { struct Call; enum class Verdict : uint8_t; }

namespace coop::orb_authority {

// Register the watches on the orb's begin and its timeline's end; keep the session. Idempotent.
void Install(coop::net::Session* session);

// Settle the watches' registration (say once whether they are live). Per frame.
void Tick();

// Session end: the records are cleared.
void OnDisconnect();

// The orb's touch stub (puppet_identity's row for npc_orborb_C calls it): `orb` the stub's object, `toucher` its
// OtherActor. A client cancels every touch. The host lets its own player's touch run, and begins the encounter
// for a client's body itself and cancels the stub.
ue_wrap::script_gate::Verdict OnTouchPre(void* orb, void* toucher);

// CLIENT: the host's OrbEvent, its length checked by the dispatcher.
void OnEvent(const coop::net::OrbEventPayload& p);

// [dev] HOST: start every client's lap count at `laps` (a drill's way to the spawn without 241 laps). -1 stops it.
void DevSetClientLaps(int laps);

}  // namespace coop::orb_authority
