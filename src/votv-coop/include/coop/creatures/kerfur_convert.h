// coop/creatures/kerfur_convert.h -- host-authoritative kerfur conversion, NPC to prop and
// back; without it a client's toggle left a second kerfur lying on the host. The game's
// verbs: turn off runs dropKerfurProp, which spawns the prop (and a floppy if carried) at the
// NPC's transform and destroys the NPC, refused for a sentient kerfur or one flagged kill;
// turn on runs spawnKerfuro, which spawns the NPC and destroys the prop on success. Every
// spawn and destroy inside them is blueprint-internal, dispatched past ProcessEvent, so no
// interceptor sees a conversion. The host detects its own conversions event-driven at the
// chokepoints (the fresh prop's expression edge for turn off, the prop's destroy edge for turn
// on) and converges them onto the wire; a client's conversion is detected by a 5 Hz
// death-watch poll (a kerfur mirror whose actor died while its element is still present),
// which sends a request for the host to run the real verb and claims the local ghost the
// invisible spawn made; the poll is also the solo host's backstop. The host executor lives in
// kerfur_convert_host.h, the client apply and the ghost custody in kerfur_convert_client.h.
// Gameplay module: engine access through ue_wrap, game thread only.

#pragma once

#include "coop/element/element.h"  // ElementId (TryCaptureKerfurPropDestroy dyingEid)

#include <cstdint>

namespace coop::net {
class Session;
struct KerfurConvertPayload;
struct KerfurConvertBroadcastPayload;
}  // namespace coop::net

namespace coop::kerfur_convert {

// Idempotent install, retried from the pump tick until the kerfur classes load: resolves the
// menu dispatcher (for the command relay) and its name parameter, the two verbs and the kill
// flag offset for the host execution path, and the prop and floppy classes for the converge
// walk; registers the one interceptor. Refuses to install if a verb grew parameters.
void Install(coop::net::Session* session);

// The host executor half (the request handler, the converge, the request-verb bracket) lives
// in kerfur_convert_host.h.

// The client half (the wire apply and the conversion-ghost custody) lives in
// kerfur_convert_client.h.

// Drive the death-watch poll: the client's conversion detector (the request plus the ghost
// claim) and the solo host's backstop, since a host prop's element is drained synchronously
// with its death and the poll's premise never holds there. A cheap no-op between the 5 Hz
// passes. Game thread, the pump tick.
void Tick();

// Host: first refusal on the generic expression of a kerfur prop-form actor. The turn-off
// verb's fresh prop spawns invisibly and the generic pipeline would claim and broadcast it as
// a keyed prop within a tick, leaving the client with its NPC mirror and a prop mirror, the
// duplicate; so every generic express lane offers a kerfur prop here first. If the actor is
// untracked and a dead, unhandled kerfur NPC watch sits within the verb's spawn radius, this
// is the conversion product: converge it now (mint the eid silently, release the dead NPC
// element, rebind the form, broadcast the convert, floppies) and return true, and the caller
// must not express. Otherwise false (a tracked prop is an established identity; a hand-placed
// or bought one is an ordinary spawn) and the generic keyed-prop path is correct. The kerfur
// is one entity and the convert is its sole conversion wire signal. Host, game thread.
bool TryAdoptFreshKerfurProp(void* actor);

// Both roles: first refusal on the generic destroy of a kerfur prop-form actor, the destroy
// twin of TryAdoptFreshKerfurProp. The turn-on verb spawns the NPC and then destroys the
// prop, so when the prop's destroy seam fires inside the verb the conversion-product NPC
// already exists, zero ticks old, a synchronous premise no periodic enroll lane can beat. If
// a fresh unowned kerfur NPC sits within the spawn radius, this death is conversion churn the
// kerfur layer owns and the caller must not broadcast a destroy: the client suppresses the
// keyed-destroy relay (it would kill the host's authoritative prop before the turn-on request
// lands, and the kerfur would vanish on every peer), the poll's request and ghost machinery
// staying its driver; the host converges inline (register the NPC silently, rebind the form,
// one convert broadcast), since its prop element is drained synchronously with the death and
// the poll's premise never holds. No adjacent fresh NPC: false, and a genuine destroy keeps
// the generic relay. `dyingEid` is the seam's element id before the unmark; invalid means the
// host declines (no wire identity to converge). Game thread, the destroy seam.
bool TryCaptureKerfurPropDestroy(void* actor, coop::element::ElementId dyingEid);

// Clear per-session state (the poll watch and its throttle) and fan the disconnect to the
// client and host halves (parked ghosts; the request bracket).
void OnDisconnect();

}  // namespace coop::kerfur_convert
