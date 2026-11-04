// coop/world/salt_rocket_ignite.h -- a client's light on a salt rocket is the host's to perform. A rocket's fuse and
// thrust run on one machine's copy (coop/world/salt_rocket_flight.h streams them), so a client whose player lights a
// rocket must not light its own copy.
//   - CLIENT (salt_rocket_ignite.cpp): on an active client session the rocket's attemptIgnite and any prop_C ignite
//     whose object is a salt rocket are refused, whatever called them. A refusal is the player's when the local
//     player's own lighter or torch verb is running on the tool in the engine's hand; it is queued, in click order,
//     and sent as PropIgniteIntent once this client's own hand announce of the tool is out and the rocket's host eid is
//     known. Any other refusal (a world fire) is counted and dropped: the host's own world fire lights its copy.
//   - HOST (salt_rocket_ignite_host.cpp): the intent joins its sender's queue (a per-sender budget and cap), is
//     checked -- the sender's world, the form of the message, the rocket, that the sender lives, its reach, the tool
//     its hand held -- and the same verb then runs on the host's own copy, as single player would. The rocket's
//     fire() then claims the flight, and the blast reaches every peer through the explosion lane.
// MTA's shape, request, check, perform and reply (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:
// 3018-3140, Packet_Vehicle_InOut); no reply is sent here: a refused light is counted and dropped.
// Game thread throughout.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::salt_rocket_ignite {

// Why the host refused an intent, one counter each.
enum class Refusal : uint8_t { NotWorldReady, Malformed, QueueFull, Gone, NotRocket, Reach, NoTool, SenderDead, Count };

// Snake case: "not_world_ready", "malformed", "queue_full", "gone", "not_rocket", "reach", "no_tool", "sender_dead".
const char* RefusalName(Refusal r);

// What this process's half of the lane has counted since the session began.
// The client: playerRefused, calls taken as the player's own and queued; worldRefused, calls refused that were not;
// pendingFull, lights dropped because the queue of sixteen was full; pendingDead, lights whose rocket died before they
// were sent; notAnnounced, lights dropped because the hand lane looked at the hand after the click and did not announce
// its tool; unspawned, lights dropped because the rocket's host eid did not come in time; sent, messages sent.
// The host: waited, ticks a head waited for a hand state still owed; performed, lights run on the host's copy by verb
// (0 attemptIgnite, 1 ignite); refused, by reason.
struct Counters {
    uint32_t playerRefused, worldRefused, pendingFull, pendingDead, notAnnounced, unspawned, sent, waited;
    uint32_t performed[2];
    uint32_t refused[static_cast<int>(Refusal::Count)];
};
Counters GetCounters();

// Store the session and register the gate rows, once for the process. Idempotent; called every load tick.
void Install(coop::net::Session* s);

// Resolve and settle the watches; on a client send the queue's head first; on the host serve each slot's queue; say
// the counters once a minute while any moved. Every role, every gameplay tick.
void Tick();

// A new world: every slot's queue and the client's queue are forgotten (they name rockets of the world that ended).
void OnWorldChange();

// One line of the counters when any moved, then everything cleared and the counters zeroed.
void OnDisconnect();

// Host receive: raw bytes, as the dispatch hands them; the format check and its refusal live here.
void OnIntentReceived(int senderSlot, const uint8_t* bytes, uint16_t len);

// Whether the local player's own lighter or torch verb is running now, on the tool in the engine's hand: a light inside it
// is the player's. Read by the prop fire lane for the props that are not rockets. Game thread.
bool LocalLightRunning();

// Whether `obj` is a salt rocket, whose light this lane owns. Game thread.
bool IsSaltRocket(void* obj);

}  // namespace coop::salt_rocket_ignite
