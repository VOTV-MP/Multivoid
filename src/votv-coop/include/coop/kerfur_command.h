// coop/kerfur_command.h -- host-authoritative kerfur radial-menu command relay +
// ownership-aware Follow (v74, 2026-06-14).
//
// User requirements: the kerfur interaction menu must be fully synced/mirrored, and "Follow"
// must follow the player who picked it. RE: research/findings/votv-kerfurOmega-coop-double-and-
// camera-RE-2026-06-14.md sec 7 + the menu-command RE.
//
// PROBLEM: actionName's command branches set State + move(), and the FOLLOW branch hard-pins
// `GetPlayerPawn(self,0)` -- which on the host is always the HOST's pawn (remote players are
// bare skeletal-mesh puppets with no pawn). So vanilla follow always follows the host, and a
// client picking any verb on its (adopted) mirror would run it LOCALLY on a parked actor.
//
// DESIGN (generalizes kerfur_convert's turn_off relay): the actionName PRE-interceptor
// (registered in kerfur_convert -- one interceptor per UFunction) routes the State-changing
// verbs here. We CANCEL the local dispatch on both roles and run the verb HOST-AUTHORITATIVELY:
//   - non-follow verbs (idle/patrol/fix_servers/get_reports/fix_transformers): the host re-runs
//     the REAL actionName via ProcessEvent (they ignore the Player param); the resulting State
//     change streams to mirrors through the existing pose stream (Inc-2).
//   - FOLLOW: the host does NOT run the BP branch (it would pin player-0). It sets State=idle to
//     silence the BP's own mover, records the requesting player as the owner (senderPeerSlot, or
//     the host itself), and drives a CreateMoveToProxyObject loop toward THAT player's body.
// turn_off stays in kerfur_convert (it is an NPC<->prop conversion, not a State change).
//
// Principle 7: gameplay/network module; engine access via ue_wrap/kerfur + game_thread only.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct KerfurCommandPayload;
}  // namespace coop::net

namespace coop::kerfur_command {

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

// Map a radial actionName verb string to a relayed Command (Invalid if it is not one we relay --
// the interceptor then leaves that dispatch untouched). turn_off maps to Invalid here (handled by
// kerfur_convert); take_object/equipment/pat are Invalid (per-player / UI / montage, out of scope).
Command CommandFromActionName(const std::wstring& name);

// Called by the actionName PRE-interceptor (kerfur_convert) for a non-turn_off verb. If `name`
// maps to a relayed Command AND we are connected, RECORDS the action (worker-safe: memory reads +
// a leaf mutex only -- NO engine calls / Post / registry walk) and returns true so the interceptor
// CANCELS the local dispatch. Returns false (interceptor leaves the dispatch) for an unrelayed verb,
// when not in a connected session, or when re-entered from our own host-side actionName replay
// (the thread-local guard -- prevents the host's RunActionName from re-recording itself).
bool TryRecordMenuCommand(void* self, const std::wstring& name, bool isClient, bool isHost);

// Idempotent install (caches the session). The actionName interceptor itself is owned by
// kerfur_convert (one interceptor per UFunction); this just wires the session + resolves nothing
// heavy (the verb/move UFunctions resolve lazily on first use in ue_wrap/kerfur).
void Install(coop::net::Session* session);

// HOST-only receiver for KerfurCommand (wired in event_dispatch_state). Resolves eid -> kerfur,
// validates (live + kerfur class + the BP `kill` guard), then executes: Follow -> owner=senderSlot
// + State=idle + follow loop; others -> run the real verb via ProcessEvent. Game thread.
void OnCommandRequest(const coop::net::KerfurCommandPayload& payload, uint8_t senderPeerSlot);

// Game thread (net-pump tick): drain the recorded actions (client -> send request; host -> execute
// locally) AND advance the ownership-follow loop (re-issue MoveTo toward each owner's body on a
// cadence). Cheap no-op when nothing is pending and no kerfur is in owned-follow.
void Tick();

// A single peer (client `slot`) left: end every owned-follow the leaver held -- restore those
// kerfurs to State=follow so the host's own BP resumes (follows the host) and our MoveTo loop stops
// driving them. Without this the kerfur stays pinned to idle + the loop chases a null puppet body
// forever (audit 2026-06-14). Game thread (the per-slot disconnect edge -> subsystems::DisconnectSlot).
void OnPeerDisconnect(uint8_t slot);

// Clear per-session state (pending queue + owned-follow map). Net disconnect (all peers gone).
void OnDisconnect();

}  // namespace coop::kerfur_command
