// coop/creatures/kerfur_command.h -- the kerfur's radial menu, relayed host-authoritatively; the kerfur
// serves whoever picked a verb.
//
// The game's actionName sets the kerfur's State and calls move(), and its verbs read the player as player 0 (the follow
// MoveTo and the patrol on GetPlayerPawn(0), the disc insert on lib.getMainPlayer()), which on the host is always the
// host. Both roles CANCEL the local dispatch (a client's mirror is a parked actor) and the host runs the real actionName
// with the requester recorded in served_player first, so those reads answer that player's puppet; a request waits while
// the host has no body for its sender. A command the game refuses (kill mode, a job) changes nobody's service, the host's
// own hands the kerfur back to player 0, a leaver's ends, and murder mode's at its startKill. A client's verb runs inside
// its requester_feedback scope, so its hint, gloss or sound goes to that client, and a command refused before it runs,
// or waiting, is answered to it (kerfur_command_answer). A get_reports reads the hand, which the host cannot see for a
// client: the press carries the held disc's record (kerfur_command_wire) and the host runs the verb with a copy of it in
// the puppet's hand (kerfur_disc_carry). turn_off stays in kerfur_convert. Page: docs/npcs-and-kerfur.md.
//
// Principle 7: gameplay/network module; engine access through ue_wrap/kerfur and game_thread only.

#pragma once

#include "coop/net/intent_bucket.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::kerfur_command {

// A sender's requests run in arrival order from a bounded queue at a bounded rate: up to kMaxPending wait, and the
// bucket lets kBudget.burst through at once. The client sizes the ids it holds to match (kerfur_command_answer).
inline constexpr size_t kMaxPending = 8;
inline constexpr coop::net::IntentBudget kBudget{4.0f, 2.0f};

// The relayed State-changing radial verbs (values are the WIRE command id, NOT enum_kerfurCommand).
enum class Command : uint8_t {
    Follow = 0,
    Idle = 1,
    Patrol = 2,
    FixServers = 3,
    GetReports = 4,
    FixTransformers = 5,
    Invalid = 0xFF,
};

// Map a radial actionName verb string, without regard to case (the radial and a reflected call differ), to a relayed
// Command (Invalid if it is not one we relay -- the seat then lets that dispatch run). turn_off maps to Invalid here
// (handled by kerfur_convert); take_object/equipment/pat are Invalid (per-player / UI / montage, out of scope).
Command CommandFromActionName(const std::wstring& name);

// Record a menu verb and cancel its local dispatch. One seam calls it: the actionName watch in
// kerfur_command_seat, on every route of the call. If `name` is a relayed verb AND a session is connected, records
// the request (Tick sends it from a client, runs it from the host) and returns true, so the seat cancels the
// dispatch and the host runs the verb. Returns false, leaving the dispatch alone, for an unrelayed verb, outside a
// connected session, and when re-entered from our own host-side actionName replay, which a guard catches. Game
// thread.
bool TryRecordMenuCommand(void* self, const std::wstring& name, bool isClient);

// Store the session pointer, re-cached on every call so a reconnect lands, and on a host register the
// startKill watch once and settle it at once; a refused or dead watch is said once, and from then an Omega
// serves no client. The actionName watch belongs to kerfur_command_seat. Game thread.
void Install(coop::net::Session* session);

// HOST-only receiver for a chunk of a KerfurCommand blob (kerfur_command_wire), wired in event_dispatch_kerfur_command.
// A blob that completes malformed, an unknown sender, an unknown command and an eid outside the host-allocated range
// are refused into the lane's refusal tally, and the rest is queued per sender, up to a cap, refused past it. Tick
// serves one queued request per sender at a bounded rate, in arrival order: the head waits while the host has no posed
// body for the sender or the Omega's seams are still settling, then resolves the eid to a kerfur and drops the request
// unless the actor is live, it is a kerfur, and the kerfur is not in kill mode or on a job -- the last two matching the
// game's own guards. Then the kerfur serves the sending slot and the real actionName runs; a verb that did not run
// restores whom it served. Game thread.
void OnCommandChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderPeerSlot);

// Game thread (net-pump tick): drain the recorded actions (client -> send request; host -> execute
// locally), serve each sender's queued request, say the refusals' summed line, and settle the startKill watch.
// Cheap no-op when nothing is pending.
void Tick();

// Instruments (the kerfur_flood drill, coop/dev/hostile_drill_kerfur), game thread, per client slot (0 for any
// other, the host's included): how many
// requests entered RunRequest, and how many the lane refused before the serve: the router's refusals (a short payload),
// an unknown sender or command, an eid outside the host range, and a full queue. A refusal at the serve (the kerfur
// gone, not a kerfur, in kill mode) is a run and is not counted here.
uint32_t RunsFor(int slot);
uint32_t RefusedFor(int slot);

// A refusal the router made before the lane saw the request (a short payload), said through the lane's tally. The
// slot is the router's, 0xFF for an unknown sender. Game thread.
void NoteRefused(uint8_t senderPeerSlot, const char* why);

// Clear the recorded actions and the tally. Net disconnect (all peers gone); served_player drops the served
// kerfurs, and a sender's queue goes with its slot's occupant.
void OnDisconnect();

}  // namespace coop::kerfur_command
