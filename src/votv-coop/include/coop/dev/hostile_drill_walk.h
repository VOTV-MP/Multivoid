// coop/dev/hostile_drill_walk.h -- drill arms proving a sender's chosen class name or prop key must not
// cost the host a walk of the whole object array. Both are judged by the HOST reading its own
// whole-array walk count (ue_wrap::walk_census) across a sentinel the burst rides behind, so the
// same judge reads a large walk count on the bytes before the fix and zero on it. record_bad_classes
// sends one PropSaveDataIntent for a real covered prop's key carrying ~1024 unknown names; bad_alarm
// sends an AlarmState burst (INCONCLUSIVE when the trigger is bound, where no walk happens either
// way); bad_prop_key sends PropDestroys for keys no prop holds; bad_prop_spawn sends keyed spawns of a
// loaded class away from every instance of it. Defensive testing of OUR OWN mod, per
// coop/dev/hostile_drill.h; nothing here ships enabled.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::walk {

// The sub-arms; None when the active drill arm is not one of ours.
enum class Arm { None, RecordBadClasses, BadAlarm, BadPropKey, BadPropSpawn };

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Client (sender), game thread: once this peer's own join is over, craft and send the attack and its
// sentinel, once. A no-op after it has sent (or reported it could not stage).
void TickClient(coop::net::Session* s, Arm a);

// Host (judge), game thread: baseline the walk count on the first world-ready slot, then read the
// delta when the sentinel lands and print the PASS / FAIL / INCONCLUSIVE marker.
void TickHostJudge(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::walk
