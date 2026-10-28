// coop/dev/hostile_drill_arms.cpp -- see coop/dev/hostile_drill_arms.h. The arm selection from the config value, the
// arm names, and the routes of the families' sender, judge, install and disconnect calls.

#include "hostile_drill_arms.h"  // co-located private header (src tree, not include/)

#include "coop/dev/hostile_drill_act.h"  // the take-or-use request arms
#include "coop/dev/hostile_drill_atv.h"  // the ATV lanes' refusal arms
#include "coop/dev/hostile_drill_chat.h"  // the chat arms
#include "coop/dev/hostile_drill_coffee.h"  // the coffee lane's refusal arms
#include "coop/dev/hostile_drill_cord.h"  // the cord link lane's refusal arms
#include "coop/dev/hostile_drill_explosion.h"  // the explosion request lane's refusal arms
#include "coop/dev/hostile_drill_hand.h"  // the hand-item class arm
#include "coop/dev/hostile_drill_ignite.h"  // the salt rocket ignite lane's refusal arms
#include "coop/dev/hostile_drill_latest.h"  // the keep-the-latest arm
#include "coop/dev/hostile_drill_world.h"   // the world levels' keep-the-latest arm
#include "coop/dev/hostile_drill_kerfur.h"  // the kerfur verbs' flood arm
#include "coop/dev/hostile_drill_remover.h"  // the remover request's far, hidden and flood arms
#include "coop/dev/hostile_drill_walk.h"  // the four walk arms
#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include <string>

namespace coop::dev::hostile_drill {

// Resolved once: with the environment variable unset every read would scan the ini on disk, and the
// drill is asked per tick.
Arm ArmOf() {
    static const Arm a = [] {
        const std::string v =
            coop::config::ResolveEnum(::coop::config_registry::rows::hostile_drill);
        return v == "signal_park"         ? Arm::SignalPark
             : v == "chat_ill_formed"     ? Arm::ChatIllFormed
             : v == "prop_convert"        ? Arm::PropConvert
             : v == "reliable_flood"      ? Arm::ReliableFlood
             : v == "repeat_join"         ? Arm::RepeatJoin
             : v == "repeat_world_ready"  ? Arm::RepeatWorldReady
             : v == "repeat_save_request" ? Arm::RepeatSaveRequest
             : v == "record_bad_classes"  ? Arm::RecordBadClasses
             : v == "bad_alarm"           ? Arm::BadAlarm
             : v == "bad_prop_key"        ? Arm::BadPropKey
             : v == "bad_prop_spawn"      ? Arm::BadPropSpawn
             : v == "bad_hand_class"      ? Arm::BadHandClass
             : v == "bad_hand_family"     ? Arm::BadHandFamily
             : v == "keep_latest"         ? Arm::KeepLatest
             : v == "chat_flood"          ? Arm::ChatFlood
             : v == "world_latest"        ? Arm::WorldLatest
             : v == "kerfur_flood"        ? Arm::KerfurFlood
             : v == "remover_far"         ? Arm::RemoverFar
             : v == "remover_hidden"      ? Arm::RemoverHidden
             : v == "remover_flood"       ? Arm::RemoverFlood
             : v == "act_far"             ? Arm::ActFar
             : v == "act_gone"            ? Arm::ActGone
             : v == "act_wrongfamily"     ? Arm::ActWrongFamily
             : v == "act_bad_commit"      ? Arm::ActBadCommit
             : v == "act_notyet"          ? Arm::ActNotYet
             : v == "act_flood"           ? Arm::ActFlood
             : v == "act_flood_early"     ? Arm::ActFloodEarly
             : v.rfind("explosion_", 0) == 0 ? Arm::Explosion
             : v.rfind("ignite_", 0) == 0    ? Arm::Ignite
             : v.rfind("atv_", 0) == 0       ? Arm::Atv
             : v.rfind("cord_", 0) == 0      ? Arm::Cord
             : v.rfind("coffee_", 0) == 0    ? Arm::Coffee
                                          : Arm::Off;
    }();
    return a;
}

namespace {

// The explosion family's sub-arm, read from the same config value once; None unless the drill's arm is that family's.
explosion::Arm ExplosionArmOf() {
    static const explosion::Arm a = ArmOf() == Arm::Explosion
        ? explosion::ArmOf(coop::config::ResolveEnum(::coop::config_registry::rows::hostile_drill))
        : explosion::Arm::None;
    return a;
}

// The ignite family's sub-arm, the same way.
ignite::Arm IgniteArmOf() {
    static const ignite::Arm a = ArmOf() == Arm::Ignite
        ? ignite::ArmOf(coop::config::ResolveEnum(::coop::config_registry::rows::hostile_drill))
        : ignite::Arm::None;
    return a;
}

// The ATV family's sub-arm, the same way.
atv::Arm AtvArmOf() {
    static const atv::Arm a = ArmOf() == Arm::Atv
        ? atv::ArmOf(coop::config::ResolveEnum(::coop::config_registry::rows::hostile_drill))
        : atv::Arm::None;
    return a;
}

// The coffee family's sub-arm, the same way.
coffee::Arm CoffeeArmOf() {
    static const coffee::Arm a = ArmOf() == Arm::Coffee
        ? coffee::ArmOf(coop::config::ResolveEnum(::coop::config_registry::rows::hostile_drill))
        : coffee::Arm::None;
    return a;
}

// The cord family's sub-arm, the same way.
cord::Arm CordArmOf() {
    static const cord::Arm a = ArmOf() == Arm::Cord
        ? cord::ArmOf(coop::config::ResolveEnum(::coop::config_registry::rows::hostile_drill))
        : cord::Arm::None;
    return a;
}

}  // namespace

const char* ArmName() {
    switch (ArmOf()) {
    case Arm::SignalPark:        return "signal_park";
    case Arm::ChatIllFormed:     return "chat_ill_formed";
    case Arm::PropConvert:       return "prop_convert";
    case Arm::ReliableFlood:     return "reliable_flood";
    case Arm::RepeatJoin:        return "repeat_join";
    case Arm::RepeatWorldReady:  return "repeat_world_ready";
    case Arm::RepeatSaveRequest: return "repeat_save_request";
    case Arm::RecordBadClasses:  return "record_bad_classes";
    case Arm::BadAlarm:          return "bad_alarm";
    case Arm::BadPropKey:        return "bad_prop_key";
    case Arm::BadPropSpawn:      return "bad_prop_spawn";
    case Arm::BadHandClass:      return "bad_hand_class";
    case Arm::BadHandFamily:     return "bad_hand_family";
    case Arm::KeepLatest:        return "keep_latest";
    case Arm::ChatFlood:         return "chat_flood";
    case Arm::WorldLatest:       return "world_latest";
    case Arm::KerfurFlood:       return "kerfur_flood";
    case Arm::RemoverFar:        return "remover_far";
    case Arm::RemoverHidden:     return "remover_hidden";
    case Arm::RemoverFlood:      return "remover_flood";
    case Arm::ActFar:            return "act_far";
    case Arm::ActGone:           return "act_gone";
    case Arm::ActWrongFamily:    return "act_wrongfamily";
    case Arm::ActBadCommit:      return "act_bad_commit";
    case Arm::ActNotYet:         return "act_notyet";
    case Arm::ActFlood:          return "act_flood";
    case Arm::ActFloodEarly:     return "act_flood_early";
    case Arm::Explosion:         return explosion::ArmName(ExplosionArmOf());
    case Arm::Ignite:            return ignite::ArmName(IgniteArmOf());
    case Arm::Atv:               return atv::ArmName(AtvArmOf());
    case Arm::Cord:              return cord::ArmName(CordArmOf());
    case Arm::Coffee:            return coffee::ArmName(CoffeeArmOf());
    default:                     return "off";
    }
}

namespace {

// The two walk arms are host-JUDGED (the host reads its own whole-array walk count), so they route to
// hostile_drill_walk, not to the Step machine below. None when the active arm is not one of them.
walk::Arm WalkArmOf() {
    switch (ArmOf()) {
    case Arm::RecordBadClasses: return walk::Arm::RecordBadClasses;
    case Arm::BadAlarm:         return walk::Arm::BadAlarm;
    case Arm::BadPropKey:       return walk::Arm::BadPropKey;
    case Arm::BadPropSpawn:     return walk::Arm::BadPropSpawn;
    default:                    return walk::Arm::None;
    }
}
bool IsWalkArm() { return WalkArmOf() != walk::Arm::None; }

// The take-or-use arms: each peer's half is hostile_drill_act's.
act::Arm ActArmOf() {
    switch (ArmOf()) {
    case Arm::ActFar:         return act::Arm::ActFar;
    case Arm::ActGone:        return act::Arm::ActGone;
    case Arm::ActWrongFamily: return act::Arm::ActWrongFamily;
    case Arm::ActBadCommit:   return act::Arm::ActBadCommit;
    case Arm::ActNotYet:      return act::Arm::ActNotYet;
    case Arm::ActFlood:       return act::Arm::ActFlood;
    case Arm::ActFloodEarly:  return act::Arm::ActFloodEarly;
    default:                  return act::Arm::None;
    }
}

}  // namespace

// reliable_flood, repeat_world_ready and repeat_save_request are judged by the SENDER (the host's answer
// is this session ending, with a code): the host runs no judge, the client reads the end on OnDisconnect.
// repeat_join is host-judged: its answer is the host refusing the repeat and KEEPING the link.
bool ClientJudged() {
    return ArmOf() == Arm::ReliableFlood || ArmOf() == Arm::RepeatWorldReady ||
           ArmOf() == Arm::RepeatSaveRequest;
}

void InstallFamilies(coop::net::Session* session) {
    act::Install(session, ActArmOf());   // a pre-announce arm holds this client's announce (once per session)
    explosion::Install(session, ExplosionArmOf());   // explosion_not_ready holds it the same way
    ignite::Install(session, IgniteArmOf());   // ignite_not_ready holds it the same way
}

bool TickFamilyHost(coop::net::Session* s) {
    const hand::Arm handArm = ArmOf() == Arm::BadHandClass  ? hand::Arm::BadHandClass
                            : ArmOf() == Arm::BadHandFamily ? hand::Arm::BadHandFamily
                                                            : hand::Arm::None;
    const latest::Arm latestArm = ArmOf() == Arm::KeepLatest ? latest::Arm::KeepLatest : latest::Arm::None;
    const world::Arm worldArm = ArmOf() == Arm::WorldLatest ? world::Arm::WorldLatest : world::Arm::None;
    const kerfur::Arm kerfurArm = ArmOf() == Arm::KerfurFlood ? kerfur::Arm::KerfurFlood : kerfur::Arm::None;
    const remover::Arm removerArm = ArmOf() == Arm::RemoverFar      ? remover::Arm::RemoverFar
                                  : ArmOf() == Arm::RemoverHidden   ? remover::Arm::RemoverHidden
                                  : ArmOf() == Arm::RemoverFlood    ? remover::Arm::RemoverFlood
                                                                    : remover::Arm::None;
    const act::Arm actArm = ActArmOf();
    const explosion::Arm explosionArm = ExplosionArmOf();
    const ignite::Arm igniteArm = IgniteArmOf();
    const atv::Arm atvArm = AtvArmOf();
    const cord::Arm cordArm = CordArmOf();
    const coffee::Arm coffeeArm = CoffeeArmOf();
    const chat::Arm chatArm = ArmOf() == Arm::ChatIllFormed ? chat::Arm::ChatIllFormed
                            : ArmOf() == Arm::ChatFlood     ? chat::Arm::ChatFlood
                                                            : chat::Arm::None;
    if (explosionArm != explosion::Arm::None) explosion::TickHostJudge(s, explosionArm);
    else if (igniteArm != ignite::Arm::None) ignite::TickHostJudge(s, igniteArm);
    else if (atvArm != atv::Arm::None)       atv::TickHostJudge(s, atvArm);
    else if (cordArm != cord::Arm::None)     cord::TickHostJudge(s, cordArm);
    else if (coffeeArm != coffee::Arm::None) coffee::TickHostJudge(s, coffeeArm);
    else if (IsWalkArm())                    walk::TickHostJudge(s, WalkArmOf());  // host counts its own walks
    else if (handArm != hand::Arm::None)     hand::TickHostJudge(s, handArm);
    else if (latestArm != latest::Arm::None) latest::TickHostJudge(s, latestArm);
    else if (worldArm != world::Arm::None)   world::TickHostJudge(s, worldArm);
    else if (kerfurArm != kerfur::Arm::None) kerfur::TickHostJudge(s, kerfurArm);
    else if (removerArm != remover::Arm::None) remover::TickHostJudge(s, removerArm);
    else if (actArm != act::Arm::None)       act::TickHostJudge(s, actArm);
    else if (chatArm != chat::Arm::None)     chat::TickHostJudge(s, chatArm);
    else return false;
    return true;
}

bool TickFamilyClient(coop::net::Session* s) {
    const hand::Arm handArm = ArmOf() == Arm::BadHandClass  ? hand::Arm::BadHandClass
                            : ArmOf() == Arm::BadHandFamily ? hand::Arm::BadHandFamily
                                                            : hand::Arm::None;
    const latest::Arm latestArm = ArmOf() == Arm::KeepLatest ? latest::Arm::KeepLatest : latest::Arm::None;
    const world::Arm worldArm = ArmOf() == Arm::WorldLatest ? world::Arm::WorldLatest : world::Arm::None;
    const kerfur::Arm kerfurArm = ArmOf() == Arm::KerfurFlood ? kerfur::Arm::KerfurFlood : kerfur::Arm::None;
    const remover::Arm removerArm = ArmOf() == Arm::RemoverFar      ? remover::Arm::RemoverFar
                                  : ArmOf() == Arm::RemoverHidden   ? remover::Arm::RemoverHidden
                                  : ArmOf() == Arm::RemoverFlood    ? remover::Arm::RemoverFlood
                                                                    : remover::Arm::None;
    const act::Arm actArm = ActArmOf();
    const explosion::Arm explosionArm = ExplosionArmOf();
    const ignite::Arm igniteArm = IgniteArmOf();
    const atv::Arm atvArm = AtvArmOf();
    const cord::Arm cordArm = CordArmOf();
    const coffee::Arm coffeeArm = CoffeeArmOf();
    const chat::Arm chatArm = ArmOf() == Arm::ChatIllFormed ? chat::Arm::ChatIllFormed
                            : ArmOf() == Arm::ChatFlood     ? chat::Arm::ChatFlood
                                                            : chat::Arm::None;
    if (explosionArm != explosion::Arm::None) explosion::TickClient(s, explosionArm);
    else if (igniteArm != ignite::Arm::None) ignite::TickClient(s, igniteArm);
    else if (atvArm != atv::Arm::None)       atv::TickClient(s, atvArm);
    else if (cordArm != cord::Arm::None)     cord::TickClient(s, cordArm);
    else if (coffeeArm != coffee::Arm::None) coffee::TickClient(s, coffeeArm);
    else if (IsWalkArm())                    walk::TickClient(s, WalkArmOf());
    else if (handArm != hand::Arm::None)     hand::TickClient(s, handArm);
    else if (latestArm != latest::Arm::None) latest::TickClient(s, latestArm);
    else if (worldArm != world::Arm::None)   world::TickClient(s, worldArm);
    else if (kerfurArm != kerfur::Arm::None) kerfur::TickClient(s, kerfurArm);
    else if (removerArm != remover::Arm::None) remover::TickClient(s, removerArm);
    else if (actArm != act::Arm::None)       act::TickClient(s, actArm);
    else if (chatArm != chat::Arm::None)     chat::TickClient(s, chatArm);
    else return false;
    return true;
}

void OnDisconnectFamilies() {
    walk::OnDisconnect();
    hand::OnDisconnect();
    latest::OnDisconnect();
    world::OnDisconnect();
    kerfur::OnDisconnect();
    remover::OnDisconnect();
    act::OnDisconnect();
    explosion::OnDisconnect();
    ignite::OnDisconnect();
    atv::OnDisconnect();
    cord::OnDisconnect();
    coffee::OnDisconnect();
    chat::OnDisconnect();
}

}  // namespace coop::dev::hostile_drill
