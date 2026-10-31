// coop/save/save_notice.h -- every player learns that the host saved the world. Overview: docs/join.md.
//
// Persistence is the host's alone (coop/save/save_block): a client's world save is blocked, so the game
// shows its SAVED indicator on the host only and a client never knew a save happened. The host hears its
// own world saves at the engine's write (ue_wrap/engine/save_to_slot_hook) -- the hosted world's save
// object under a slot of the game's own, so a join's scratch capture and the save-slot menu's writes do
// not count -- and puts one line in its chat feed and sends WorldSaved. While the game counts an event as
// running, its save shows an error and returns without a write, so the host also watches mainGamemode.save;
// a save told to bypass the event writes the slot without gathering the world. Either way the line says the
// save did not take. A client puts the same line in its feed, naming the host. The game's own SAVED indicator stays
// the host's: calling mainGamemode.saveAnim on a client's gamemode was measured to show nothing.
//
// Game thread.

#pragma once

namespace coop::net {
class Session;
struct WorldSavedPayload;
}  // namespace coop::net

namespace coop::save_notice {

// Cache the session, subscribe to the engine's world-save notice and register the save watch. Idempotent.
// Game thread.
void Install(coop::net::Session* session);

// Per gameplay tick: resolve the save watch's name until it is live, and say if it never is.
void Tick();

// CLIENT: the host saved its world, or tried to during an event. Validated at the router.
void OnWorldSaved(const coop::net::WorldSavedPayload& p);

// Session end: the save count and the once-said latches. Game thread.
void OnDisconnect();

}  // namespace coop::save_notice
