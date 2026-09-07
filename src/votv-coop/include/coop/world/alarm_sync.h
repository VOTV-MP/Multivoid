// coop/world/alarm_sync.h -- the base radar alarm as a shared-world toggle.
//
// There is ONE trigger_alarm_C per map, under the gamemode key 'alarmTrigger'. It goes ON when
// the screen test's radar sweep hits an important comp_radarPoint, and OFF from the radar
// panel's "Stop alarm" press or from the screen test's own resets. runTrigger is natively
// IDEMPOTENT -- it returns when IntToBool(index) already equals `active` -- and it fans out the
// WHOLE alarm: the alarm lamps, the klaxon loop, the basement grate, the ceiling lamps' flicker,
// and the native event registry through lib_C::setEvent.
//
// Every native call site dispatches runTrigger EX_VirtualFunction, which is ProcessEvent-
// invisible, so this lane POLLS trigger_alarm_C.active rather than hooking the verb.

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
