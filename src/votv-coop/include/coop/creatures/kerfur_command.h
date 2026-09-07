// coop/creatures/kerfur_command.h -- the kerfur's radial menu, relayed host-authoritatively, with a follow
// that follows whoever picked it. The subsystem's page is docs/npcs-and-kerfur.md.
//
// The game's own actionName sets the kerfur's State and calls move(), and its follow branch pins
// `GetPlayerPawn(self, 0)` -- on the host, always the HOST's pawn, since a remote player is a bare
// skeletal-mesh puppet with no pawn of its own. So the stock follow always follows the host, and a
// client picking a verb on its adopted mirror would run it locally on a parked actor. Both roles
// therefore CANCEL the local dispatch and the host performs the verb: the State verbs by re-running
// the real actionName (they ignore the Player param, and the State change reaches the mirrors on
// the existing pose stream), follow by pinning State to idle -- silencing the game's own player-0
// mover -- and driving its own MoveTo loop toward the owner's body. turn_off stays in
// kerfur_convert, being a prop conversion rather than a State change.
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

// Store the session pointer, re-cached on every call so a reconnect lands. Nothing else installs
// here: the actionName interceptor belongs to kerfur_convert, and the verb and move UFunctions
// resolve lazily on first use inside ue_wrap/kerfur.
void Install(coop::net::Session* session);

// HOST-only receiver for a KerfurCommand request, wired in event_dispatch_intent. Resolves the eid
// to a kerfur and drops the request unless the eid is in the host-allocated range, the actor is
// live, it is a kerfur, the command is a known one, and the kerfur is not in kill mode -- the last
// matching the game's own actionName guard, where a murderfur refuses the menu. Then: follow sets
// the owner to the sending slot, State to idle and starts the loop; any other verb ends an
// owned-follow and runs the real actionName. Game thread.
void OnCommandRequest(const coop::net::KerfurCommandPayload& payload, uint8_t senderPeerSlot);

// Game thread (net-pump tick): drain the recorded actions (client -> send request; host -> execute
// locally) AND advance the ownership-follow loop (re-issue MoveTo toward each owner's body on a
// cadence). Cheap no-op when nothing is pending and no kerfur is in owned-follow.
void Tick();

// A single peer (client `slot`) left: end every owned-follow that leaver held, restoring those
// kerfurs to State=follow so the game's own branch resumes -- following the host -- and our MoveTo
// loop stops driving them. Without it the kerfur stays pinned to idle while the loop chases a
// puppet body that is gone. Game thread, on the per-slot disconnect edge in
// subsystems::DisconnectSlot.
void OnPeerDisconnect(uint8_t slot);

// Clear per-session state (pending queue + owned-follow map). Net disconnect (all peers gone).
void OnDisconnect();

}  // namespace coop::kerfur_command
