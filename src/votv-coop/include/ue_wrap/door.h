// ue_wrap/door.h -- standalone engine access for VOTV base doors (Adoor_C /
// Adoor_pryable_C). Principle-7 engine-wrapper layer: it wraps the reflection /
// struct-offset / UFunction-thunk details of a door actor. NO network logic, NO
// gameplay/coop state -- coop::door_sync owns those and talks to the engine
// through here.
//
// A door is an AtriggerBase_C descendant. Its open/closed truth is the inherited-
// layout bool `isOpened`; its cross-peer-stable identity is the inherited
// AtriggerBase_C::Key FName (assigned deterministically by intComs_gamemodeMakeKeys
// + save-persistent). The canonical open/close entry points are doorOpen(bool
// bypassCheck) / doorClose(bool bypassCheck). Adoor_pryable_C inherits all of
// these unchanged, so resolving against door_C covers both classes.
//
// RE: research/findings/votv-doors-and-lightswitches-RE-2026-05-25.md.

#pragma once

#include <string>

namespace ue_wrap::door {

// Resolve the door_C UClass + the inherited Key / isOpened field offsets +
// the doorOpen / doorClose / settime UFunctions. Idempotent; returns true once
// everything resolved (false while the door_C BP class is not yet loaded -- the
// caller retries on a later tick, same wait-and-retry shape as the other
// Install() paths). Game thread.
bool EnsureResolved();

// The door_C UClass pointer (nullptr until EnsureResolved succeeds). Exposed for
// IsDoor's descendant check.
void* DoorClass();

// True iff `obj`'s class is door_C or a subclass (e.g. door_pryable_C). Cheap
// (a bounded SuperStruct walk; no allocation). False if not yet resolved.
bool IsDoor(void* obj);

// Read the door's AtriggerBase_C::Key FName as a wide string. Empty on failure
// (null / not resolved). An unkeyed door returns L"None".
std::wstring GetKeyString(void* door);

// Read the door's `isOpened` bool into `open`. Returns false if the read could
// not be made (null door / not resolved); leaves `open` untouched on failure.
bool TryReadOpen(void* door, bool& open);

// True iff a player's manual E-press would open/toggle this door RIGHT NOW, per the
// door's OWN engine logic. BYTE-EXACT from the BP disassembly (door CFG, 2026-06-06):
// the real E-press path gates on the door's `Active` (power) at ubergraph offset 3533
// BEFORE toggling -- `Active==true` -> toggle (doorOpen/doorClose(bypassCheck=true));
// `Active==false` -> no open (in the normal story gamemode, `usesp_light==true`). The
// toggle itself then needs `!jammed && !superClosed` (the doorOpen open-condition). So:
//   CanOpen == Active && !jammed && !superClosed
// This REPLACES the prior IsLocked, which read the wrong field: it keyed on the PASSLOCK's
// `isAcc` -- proven by the disassembly to be a crosshair-HOVER flag, not accept state -- so
// `IsLocked = passlock.Active && !passlock.isAcc` wrongly reported a POWERED door (Active=1,
// isAcc=0 when not hovering the accept button) as locked and the host DENIED the client's
// open (the user's "first E-press doesn't work"). Here we read the DOOR's own `Active`
// (0x0352), the field the gamemode power trigger + the keypad's setActive actually drive.
// Reads only struct fields (no UFunction dispatch); cheap, call per open-request. Game
// thread. Returns true (fail-OPEN) if door is null / offsets unresolved, so a resolution
// failure never silently locks every door.
bool CanOpen(void* door);

// Canonical open/close. `bypass` -> the BP's bypassCheck param (skip the
// keycard/password/jam guards -- always true on the receiver, since the SENDER
// already validated). Both dispatch a UFunction via ProcessEvent and MUST run on
// the game thread. Return false on null door / unresolved UFunction.
bool CallDoorOpen(void* door, bool bypass);
bool CallDoorClose(void* door, bool bypass);

// Write the door's `Active` (power) flag -- the field CanOpen gates on. The keypad's
// accept unlocks its door by setting this true (SP: passwordLock.open -> setActive ->
// door.Active = true). Used by the host-authoritative keypad accept (coop::keypad_sync)
// so an unlocked door stays openable (CanOpen) after the code, independent of whether
// the native keypad chain completed. Plain field write; no UFunction. Game thread.
void SetActive(void* door, bool on);

// The doorOpen / doorClose UFunction pointers (for POST-observer registration by
// coop::door_sync). nullptr until EnsureResolved succeeds.
void* DoorOpenFn();
void* DoorCloseFn();

// --- Host-authoritative client suppression (oscillation fix 2026-06-04) ----------
// A door's isOpened is re-driven every tick by its LOCAL sensor + autoclose logic
// (checkSensor: empty sensor + autoclose -> doorClose). On a CLIENT this fights the
// host's authoritative state (the host's real player holds a door open; the client's
// door, whose sensor the host-player puppet doesn't trigger, autocloses) -> infinite
// open/close oscillation. The MTA fix (single-syncer: the non-authority disables its
// local simulation -- CClientVehicle m_bAllowDoorRatioSetting / CObjectSync syncer
// gate) is to make CLIENT doors render-only: SuppressClientAutonomy writes
// autoclose=false so the client door cannot auto-revert an applied host state. The
// original value is cached so RestoreClientAutonomy can put it back at disconnect.
// Idempotent per door (a door already suppressed is left alone). Game thread.
void SuppressClientAutonomy(void* door);
void RestoreClientAutonomy(void* door);

// --- Force-snap to a state, PROXIMITY-INDEPENDENT (deep-dig 2026-06-04) -----------
// A door's open/close is a `move` UTimelineComponent animation that ONLY advances while the
// door actor TICKS -- and UE throttles tick for actors far from a player. So doorOpen() on a
// door whose (local) player is far FREEZES mid-animation and isOpened never gets set (probe-
// proven across 8 doors). ForceOpen/ForceClose complete the state WITHOUT the animation:
// write the move-timeline alpha (move_a @0x0340) to the end + the direction (move__Direction
// @0x0344) + call door_C::move__FinishedFunc(), which is the animation-completion handler that
// sets isOpened + snaps the mesh to the final pose. Probe-proven to set isOpened reliably on
// every far/frozen door. THIS is how a renderer/host sets a door's state regardless of where
// its own player is (the foundation of the host-managed door-streaming model). Game thread.
void ForceOpen(void* door);
void ForceClose(void* door);

// Apply a door state with the RIGHT visual for this peer: if the door is near the local CAMERA
// (visible + within tick range) play the NATIVE animated swing (CallDoorOpen/CallDoorClose --
// smooth); if it is FAR, ForceOpen/ForceClose (snap -- invisible anyway, and the native
// animation would just freeze out of tick range). This is the "near peers animate, far peers
// snap" relevancy that gives smooth doors where they're seen and correct state everywhere.
// Skips re-triggering a door already animating toward the same target. Game thread.
void SmartApply(void* door, bool open);

// Drain SmartApply's verify list: doors whose native swing has completed are dropped; doors
// whose swing froze (beyond tick range) past their deadline are force-snapped so their state is
// still correct. Cheap no-op when nothing is mid-apply. Call once per net-pump tick. Game thread.
void TickSmartApply();

// --- Host-side held-door suppression (cycle fix 2026-06-04) -----------------------
// The cycle's deeper half: when a CLIENT opens a door, the HOST opens its copy too,
// but the host has no local player at that door (only the client's render puppet, which
// does NOT hold the host's sensor -- GetController()==nullptr orphan). The host's native
// checkSensor then finds an empty sensor + autoclose and closes the door it just opened
// for the client -> broadcasts OFF -> the client re-requests open -> ~1 Hz cycle (RE-
// confirmed: votv-doors-keypad-npc-auto-open-RE; the close arrives via checkSensor, the
// sensor-disable is the load-bearing lever, autoclose=false alone is not enough). The fix
// (MTA single-syncer: disable the non-relevant local simulation while a remote intent owns
// the entity) is to make the HOST treat a door a remote client is holding open as render-
// only too -- same proven recipe as the client: autoclose=false + sensor overlap events
// off. SuppressHostHeldDoor is called ONCE per door when the host applies a remote open
// (lazy, per-door, never per-tick/bulk -- the black-screen lesson); ReleaseHostHeldDoor
// restores the authored autoclose + re-enables the sensor AND closes the door (the client
// released its hold), so native autonomy resumes. Cached in a host-side map distinct from
// the client one. Game thread.
void SuppressHostHeldDoor(void* door);
void ReleaseHostHeldDoor(void* door);

}  // namespace ue_wrap::door
