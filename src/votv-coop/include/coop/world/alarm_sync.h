// coop/world/alarm_sync.h -- the base radar alarm as a shared-world toggle.
//
// The map carries more than one trigger_alarm_C; ours is the one keyed 'alarmTrigger', which
// owns the wiring. It goes ON from the screen test's radar sweep and from crafting a snusk
// loaf, and OFF from the radar panel's lever and the screen test's own resets. runTrigger is
// natively IDEMPOTENT -- it returns when IntToBool(index) already equals `active` -- and it
// fans out the WHOLE alarm: the basement grate, lib_C::setEvent, the alarm lamps, the klaxon
// loop and the ceiling lamps' flicker.
//
// Every native call site dispatches runTrigger EX_LocalVirtualFunction, which is
// ProcessEvent-invisible, so this lane POLLS trigger_alarm_C.active instead of hooking it.

#pragma once

namespace coop::net {
class Session;
struct AlarmStatePayload;
}  // namespace coop::net

namespace coop::alarm_sync {

// Cache the session. Resolution (class + active offset + runTrigger) is lazy in Tick.
void Install(coop::net::Session* session);

// Per net-pump tick, game thread, throttled to about 1 Hz, on BOTH roles. The host broadcasts
// an observed transition to everyone -- that is the canonical fanout. A client sends its own
// observed local transition (its scan fired early, or its player pressed Stop) to the host,
// which applies it natively and broadcasts from its own poll.
void Tick();

// HOST, game thread, at a joiner's world-ready edge: send the current state to this slot,
// unconditionally, so a mid-alarm joiner starts its klaxon on arrival.
void QueueConnectBroadcastForSlot(int slot);

// Game thread (event_feed drain): a peer's alarm state, applied as a reflected
// runTrigger(nullptr, active) on the local trigger. We dispatch that through ProcessEvent
// ourselves, so the blueprint-internal invisibility does not matter, and the native idempotency
// breaks every echo loop. A client applies; the host applies and lets its own poll broadcast.
void OnReliable(const coop::net::AlarmStatePayload& payload, int senderPeerSlot);

// Test/dev seam: post a game-thread native runTrigger(active) on the LOCAL trigger -- the
// lane's own poll then detects + broadcasts, so an e2e exercises the shipping path
// (harness/autotest_alarmforce; RULE-2-exempt diagnostics).
void DevForce(bool active);

// Teardown: drop the cached trigger + baseline, clear the session.
void OnDisconnect();

}  // namespace coop::alarm_sync
