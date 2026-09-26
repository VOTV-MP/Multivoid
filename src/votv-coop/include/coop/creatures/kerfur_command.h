// coop/creatures/kerfur_command.h -- the kerfur's radial menu, relayed host-authoritatively; the kerfur
// serves whoever picked a verb.
//
// The game's own actionName sets the kerfur's State and calls move(), and its verbs read the player as
// player 0 -- the follow MoveTo and the patrol on GetPlayerPawn(0), the disc insert on lib.getMainPlayer()
// -- which on the host is always the host. Both roles CANCEL the local dispatch (a client's mirror is a
// parked actor) and the host runs the real actionName with the requester recorded in served_player first,
// so those reads answer that player's puppet and the game's own follow follows it; a request waits while
// the host has no body for its sender. A command the game refuses (kill mode, a job) changes nobody's
// service, the host's own hands the kerfur back to player 0, a leaver's ends, and murder mode's at its
// startKill, before the chase reads anyone. Nothing writes a puppet's hand, so a client's get_reports gets
// the game's refusal hint, shown on the host. turn_off stays in kerfur_convert. Page: docs/npcs-and-kerfur.md.
//
// Principle 7: gameplay/network module; engine access through ue_wrap/kerfur and game_thread only.

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

// Record a menu verb and cancel its local dispatch. Two seams call it: the actionName
// PRE-interceptor in kerfur_convert (one interceptor per UFunction) for a non-turn_off verb, and
// the client's radial E-input seam in kerfur_menu_input. If `name` is a relayed verb AND a session
// is connected, records the request -- worker-safe, memory reads and a leaf mutex, no engine call,
// no Post, no registry walk -- and returns true, so the caller cancels the dispatch and the host
// runs the verb. Returns false, leaving the dispatch alone, for an unrelayed verb, outside a
// connected session, and when re-entered from our own host-side actionName replay, which a
// thread-local guard catches.
bool TryRecordMenuCommand(void* self, const std::wstring& name, bool isClient);

// Store the session pointer, re-cached on every call so a reconnect lands, and on a host register the
// startKill watch once and settle it at once; a refused or dead watch is said once, and from then an Omega
// serves no client. The actionName interceptor belongs to kerfur_convert. Game thread.
void Install(coop::net::Session* session);

// HOST-only receiver for a KerfurCommand request, wired in event_dispatch_intent. It waits while the host
// has no posed body for the sender or the Omega's seams are still settling (the sender's latest request per
// kerfur, in arrival order), then resolves the eid to a kerfur and drops the request unless the eid is in
// the host-allocated range, the actor is live, it is a kerfur, the command is a known one, and the kerfur is
// not in kill mode or on a job -- the last two matching the game's own guards. Then the kerfur serves the
// sending slot and the real actionName runs; a verb that did not run restores whom it served. Game thread.
void OnCommandRequest(const coop::net::KerfurCommandPayload& payload, uint8_t senderPeerSlot);

// Game thread (net-pump tick): drain the recorded actions (client -> send request; host -> execute
// locally), run the waiting requests whose sender can now be served, and settle the startKill watch.
// Cheap no-op when nothing is pending.
void Tick();

// A peer left: its waiting requests go. Game thread, on the per-slot disconnect edge.
void OnPeerLeft(uint8_t slot);

// Clear the pending queue and the waiting requests. Net disconnect (all peers gone); served_player drops
// the served kerfurs.
void OnDisconnect();

}  // namespace coop::kerfur_command
