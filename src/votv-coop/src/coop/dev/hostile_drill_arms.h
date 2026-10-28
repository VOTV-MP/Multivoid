// coop/dev/hostile_drill_arms.h -- what hostile_drill.cpp, the dispatcher, shares with the arm selection and the
// families' routes in hostile_drill_arms.cpp: the arm enum, the selected arm, and the four calls that route a tick, an
// install and a disconnect to the families' own modules. Private to coop/dev: hostile_drill*.cpp.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill {

// The walk arms (record_bad_classes, bad_alarm, bad_prop_key, bad_prop_spawn) live in
// hostile_drill_walk.cpp, the hand arms (bad_hand_class, bad_hand_family) in hostile_drill_hand.cpp, the chat arms
// (chat_ill_formed, chat_flood) in hostile_drill_chat.cpp, keep_latest in hostile_drill_latest.cpp and world_latest in
// hostile_drill_world.cpp, kerfur_flood in hostile_drill_kerfur.cpp and the remover arms in
// hostile_drill_remover.cpp, the take-or-use arms in hostile_drill_act.cpp and the explosion_* arms (one Arm value,
// the family resolving its own sub-arm) in hostile_drill_explosion.cpp and the ignite_* arms (likewise) in
// hostile_drill_ignite.cpp and the atv_* arms (likewise) in hostile_drill_atv.cpp and the cord_* arms (likewise) in
// hostile_drill_cord.cpp and the coffee_* arms (likewise) in hostile_drill_coffee.cpp -- hostile_drill.cpp is the
// dispatcher that owns the Step machine of its own arms, and hostile_drill_arms.cpp owns the arm selection and routes
// those to their module's sender and judge.
enum class Arm { Off, SignalPark, ChatIllFormed, PropConvert, ReliableFlood, RepeatJoin,
                 RepeatWorldReady, RepeatSaveRequest, RecordBadClasses, BadAlarm, BadPropKey,
                 BadPropSpawn, BadHandClass, BadHandFamily, KeepLatest, ChatFlood, WorldLatest, KerfurFlood,
                 RemoverFar, RemoverHidden, RemoverFlood,
                 ActFar, ActGone, ActWrongFamily, ActBadCommit, ActNotYet, ActFlood, ActFloodEarly, Explosion, Ignite,
                 Atv, Cord, Coffee };

// The selected arm, resolved once from the config value.
Arm ArmOf();

// The arm's name, for the verdict markers: the family's own name for a family arm.
const char* ArmName();

// reliable_flood, repeat_world_ready and repeat_save_request are judged by the sender, not by the host.
bool ClientJudged();

// Each family's calls, routed by the selected arm: the install, the host's and the client's tick (true when a family
// took the tick, false when the arm is the dispatcher's own) and the disconnect of every family.
void InstallFamilies(coop::net::Session* session);
bool TickFamilyHost(coop::net::Session* s);
bool TickFamilyClient(coop::net::Session* s);
void OnDisconnectFamilies();

}  // namespace coop::dev::hostile_drill
