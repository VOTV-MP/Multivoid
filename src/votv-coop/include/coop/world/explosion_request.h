// coop/world/explosion_request.h -- a client's shots and explosion requests, and the host that receives them. A
// client's own game dissolves the explosions it would spawn (coop/world/explosion_sync.h); what its own player caused
// is asked of the host in two kinds, ProjectileFired (its launcher fired) and ExplosionRequest (the blast that
// followed), which ride one ordered lane and are never relayed. A fourth family, an ATV the client syncs that exploded
// (explosion_request_atv.cpp), is its own kind, AtvExplodeRequest.
//   - CLIENT (explosion_request_client.cpp): the launcher fire bodies are watched, so a rocket or sticky bomb born
//     inside one is the player's shot, sent once the player's hand announce of its launcher is out; the dissolved
//     birth of an explosion whose source the player owns (a projectile, a grenade, the pilled body, an ATV) is its request.
//   - HOST: each is checked strictly -- length, the class pairs the game's Blueprints produce, finite floats, a
//     field the kind does not use zero -- and queued on its sender's slot, then judged one per token of its queue's
//     own bucket (explosion_request_host.cpp; an ATV's sender must be its syncer) and spawned natively. A sender
//     whose world is not ready is refused at the receive. Every refusal is counted by reason and said through a
//     per-lane tally. The slot's state clears with its occupant.
//   - The source classes (ue_wrap/world/explosion_source.h) are resolved on a client, the only role that reads them:
//     by pointer once held, by class name until then. The counters are public (GetCounters), by kind. Game thread.

#pragma once

#include "coop/world/explosion_recipe.h"

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::explosion_request {

// Why a message was refused, one counter each.
enum class Refusal : uint8_t {
    Malformed, DuplicateShot, QueueFull, NotWorldReady,
    NoLauncher, FungunRule, NotSeated, OriginBound, UnknownShot, Reach, NoExplosive, HandBound, NoPuppet, PuppetDead,
    Unresolved, SpawnFailed,
    NotSyncer, Blown, UnknownAtv, NoTp,
    Count
};

// Snake case: "malformed", "duplicate_shot", "queue_full", "not_world_ready", "no_launcher", "fungun_rule",
// "not_seated", "origin_bound", "unknown_shot", "reach", "no_explosive", "hand_bound", "no_puppet", "puppet_dead",
// "unresolved", "spawn_failed", "not_syncer", "blown", "unknown_atv", "no_tp".
const char* RefusalName(Refusal r);

// What this process's half of the lane has counted since the session began. The per-kind arrays are indexed by the
// request's kind less one: 0 projectile, 1 hand, 2 self, 3 atv.
struct Counters {
    // The host: refusals by reason, shots and requests accepted (the shots also by launcher, indexed by the
    // Launcher's value), shots evicted from the table by the cap, shots that
    // aged out unrequested, the ticks a judge returned Wait for a hand state still owed (a head that waits again
    // counts again; the wait on a shot still queued is not counted), ATV requests accepted from the syncer a
    // hand-back had just replaced, and ATVs marked blown (the mark itself can clear within a frame: a trap death at
    // health 0 re-arms on the killer's next report, so a reader counts marks instead of polling the mark).
    uint64_t refused[static_cast<int>(Refusal::Count)];
    uint64_t shotsAccepted, shotsAcceptedBy[coop::explosion_recipe::kLauncherSlots], requestsAccepted[4], evicted,
        expired, waited, lateAccepted, atvBlownMarked;
    // The client: shots and requests sent, pending births pushed out by the cap, pending births whose projectile
    // died inside the fire body, pending births left by a body that ended without its post, messages dropped unsent
    // (a location that did not read, a send that failed, the outbox's cap), projectiles other than a host's copy that
    // exploded with no shot sent, and messages dropped because the hand lane looked at the hand after their shot and
    // did not announce its launcher, and ATV explosions born while the hit guard's owned set was not published (a
    // tick's early return, a world change in flight): no request could be made for them and the blast was dissolved.
    uint64_t shotsSent, requestsSent[4], pendingDropped, pendingDead, pendingOrphaned, clientDropped,
        unmappedProjectile, shotNotAnnounced, atvBirthUnowned;
};
Counters GetCounters();

// Store the session and take the client half's watches; idempotent, called every load tick.
void Install(coop::net::Session* s);

// On a client resolve the source classes and send, head first, what the hand announce now allows; flush the refusal
// tally and, on the host, serve each slot's four queues.
void Tick();

// A new world: every slot's queues and judged shots, and the client half's pending births and sent shots, are
// forgotten with the outbox (they name projectiles of the world that ended). The counters and one-shot logs stay.
void OnWorldChange();

// Log the accepted, sent and refusal counts, then clear every per-slot state, the client half's state and the
// counters.
void OnDisconnect();

// Host receive: raw bytes, as the dispatch hands them; the format check and its refusal live here.
void OnFiredReceived(int senderSlot, const uint8_t* bytes, uint16_t len);
void OnRequestReceived(int senderSlot, const uint8_t* bytes, uint16_t len);
void OnAtvRequestReceived(int senderSlot, const uint8_t* bytes, uint16_t len);

// Client, every birth (before the class filter): records a rocket or sticky born inside one of this client's
// launcher fire bodies. A class compare, then the script gate's body test only for those two classes.
void OnClientProjectileBirth(void* actor);

// Client: the dissolved birth of an explosion whose spawning Blueprint is sourceObject; sends at most one request.
void OnClientBirth(void* sourceObject, void* explosionActor);

// Client, first in OnClientBirth: the dissolved explosion of an ATV this client owns the tick of (an ATV's own
// `explode`) is asked of the host as an AtvExplodeRequest, and the birth is then handled: true. Any other source: false.
bool OnClientAtvBirth(void* sourceObject);

}  // namespace coop::explosion_request
