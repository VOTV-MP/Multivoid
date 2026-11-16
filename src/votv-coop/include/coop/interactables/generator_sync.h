// coop/interactables/generator_sync.h -- a portable generator's fuel and its on/off are the host's, on every peer.
//
// prop_generatorP_C keeps `fuel` (0..100) and `isActive` in its own record. A gas can fills it through gascanFuel (or a
// can lying in its fuelIn box), action 8 runs tryActivate (on while there is fuel, else off) and activateOutlets powers
// its two sockets, and its tick burns fuel while it runs and switches it off at 0. Each copy ran all of it for itself, so a
// client's pour and start reached no other peer (Trello #50).
//
// CLIENT: its copy runs no tick and no fuelIn overlap (the burn and a lying can are the host copy's); its player's pour
// and press still run on its copy, for the feel, and are asked of the host (GeneratorIntent: the gain, the state wanted).
// HOST: an intent is judged -- the world ready, a generator by key and class, a living sender in reach, a gas can in a
// pour's hand -- and done on its copy (the fuel added up to 100; the game's own tryActivate). Its generators are read twice
// a second, and one that changed, or runs, is sent (GeneratorState; a running one once a second); a refused intent is
// answered with the state to its sender; a joiner gets every generator's state at its ready edge. A state is written
// onto the client's copy, with activateOutlets when the on/off changed. Game thread.

#pragma once

namespace coop::net {
class Session;
struct GeneratorPayload;
}  // namespace coop::net

namespace coop::generator_sync {

// Store the session and register the client's watches. Idempotent; called at each session start.
void Install(coop::net::Session* session);

// The host's reads and sends; the client's watches retried until registered. Game thread, once per pump tick.
void Tick();

// A client's receiver: the host's generator.
void OnState(const coop::net::GeneratorPayload& p);

// The host's receiver: a client's pour or press.
void OnIntent(int senderSlot, const coop::net::GeneratorPayload& p);

// HOST: every generator's state, to a joiner at its ready edge.
void SendJoin(int slot);

// The session ended: everything is forgotten.
void OnDisconnect();

}  // namespace coop::generator_sync
