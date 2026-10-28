// coop/dev/hostile_drill_cord.cpp -- see coop/dev/hostile_drill_cord.h. The arm table, the counters an arm is judged on,
// the verdict print, the lobby-chat lines and the family's entry points; the client's sender is
// hostile_drill_cord_client.cpp and the host's setup and judge hostile_drill_cord_host.cpp.

#include "coop/dev/hostile_drill_cord.h"

#include "hostile_drill_cord_detail.h"  // co-located private header (src tree, not include/)

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::dev::hostile_drill::cord {
namespace detail {
namespace {

using F = Field;

// The arms: the corded prop each needs, the messages sent, then the host's expected deltas. A claim ends in exactly
// one of applied, equal and the refusals, so `Claims` is their sum; every claim is answered. The dispatcher's drops
// are by reason: a payload of the wrong size, a key longer than the wire's, an op a client may not send.
//   malformed  a claim one byte short; a claim whose cord key is 32 bytes (the wire holds 31)
//   bad_op     op 1 (a canonical) from a client; op 2 (the seed's end) from a client, with a seq and a cord key
//   reach      an unplug claim on a cord whose prop stands 2000 uu from the sender: refused for reach
//   flood      20 unplug claims on a cord within reach, in one tick: the rate window takes 12, which find the cord
//              already unplugged, and refuses the other 8
//   dead_sender  one well-formed unplug claim on a cord within reach, sent while the sender's player is dead: refused
//              as dead (that check runs before reach and the rest) and answered
const Spec kSpecs[] = {
    {"cord_malformed",   Arm::Malformed,  Fixture::None, Mode::Plain, 2,  {{F::DropSize, 1}, {F::DropKey, 1}}},
    {"cord_bad_op",      Arm::BadOp,      Fixture::None, Mode::Plain, 2,  {{F::DropOp, 2}}},
    {"cord_reach",       Arm::Reach,      Fixture::Far,  Mode::Plain, 1,  {{F::Claims, 1}, {F::Reach, 1}, {F::Answers, 1}}},
    {"cord_flood",       Arm::Flood,      Fixture::Near, Mode::Plain, 20,
     {{F::Claims, 20}, {F::Equal, 12}, {F::Rate, 8}, {F::Answers, 20}}},
    {"cord_dead_sender", Arm::DeadSender, Fixture::Near, Mode::Dead,  1,  {{F::Claims, 1}, {F::Dead, 1}, {F::Answers, 1}}},
};

constexpr const char* kNames[kFields] = {
    "claims", "applied", "equal", "refused_rate", "refused_dead", "refused_reach", "refused_host_drag", "unresolved",
    "key_pending", "lane_off", "answers", "drop_size", "drop_key", "drop_op", "drop_field",
};

}  // namespace

const char* FieldName(Field f) { return kNames[static_cast<size_t>(f)]; }

Tally TallyOf(const coop::cord_sync::DrillView& v) {
    Tally t{};
    t[static_cast<size_t>(F::Claims)] = v.claims;
    t[static_cast<size_t>(F::Applied)] = v.claimsApplied;
    t[static_cast<size_t>(F::Equal)] = v.claimsEqual;
    t[static_cast<size_t>(F::Rate)] = v.refusedRate;
    t[static_cast<size_t>(F::Dead)] = v.refusedDead;
    t[static_cast<size_t>(F::Reach)] = v.refusedReach;
    t[static_cast<size_t>(F::HostDrag)] = v.refusedHostDrag;
    t[static_cast<size_t>(F::Unresolved)] = v.unresolved;
    t[static_cast<size_t>(F::KeyPending)] = v.keyPending;
    t[static_cast<size_t>(F::LaneOff)] = v.laneOff;
    t[static_cast<size_t>(F::Answers)] = v.answers;
    t[static_cast<size_t>(F::DropSize)] = v.dropSize;
    t[static_cast<size_t>(F::DropKey)] = v.dropKey;
    t[static_cast<size_t>(F::DropOp)] = v.dropOp;
    t[static_cast<size_t>(F::DropField)] = v.dropField;
    return t;
}

const Spec* SpecOf(Arm a) {
    for (const Spec& s : kSpecs)
        if (s.arm == a) return &s;
    return nullptr;
}

Tally Expected(const Spec& sp) {
    Tally t{};
    for (const Entry& e : sp.want)
        if (e.count) t[static_cast<size_t>(e.field)] = e.count;
    return t;
}

void Verdict(Arm a, const char* what, const std::string& why) {
    if (std::strcmp(what, "PASS") == 0) UE_LOGI("[HOSTILE-DRILL] %s PASS -- %s", ArmName(a), why.c_str());
    else UE_LOGW("[HOSTILE-DRILL] %s %s: %s", ArmName(a), what, why.c_str());
}

// Lines no save can hold, as a prop class or a place could.
std::string ReadyLine(Arm a) { return std::string("hostile-drill ") + ArmName(a) + " host-ready life-1"; }
std::string ClientReadyLine(Arm a) { return std::string("hostile-drill ") + ArmName(a) + " client-ready"; }
std::string SawPrefix(Arm a) { return std::string("hostile-drill ") + ArmName(a) + " client-saw "; }

}  // namespace detail

Arm ArmOf(const std::string& value) {
    for (const detail::Spec& s : detail::kSpecs)
        if (value == s.name) return s.arm;
    return Arm::None;
}

const char* ArmName(Arm a) {
    const detail::Spec* s = detail::SpecOf(a);
    return s ? s->name : "off";
}

void TickHostJudge(coop::net::Session* s, Arm a) {
    if (const detail::Spec* sp = detail::SpecOf(a)) detail::HostTick(s, *sp);
}

void TickClient(coop::net::Session* s, Arm a) {
    if (const detail::Spec* sp = detail::SpecOf(a)) detail::ClientTick(s, *sp);
}

void OnDisconnect() {
    detail::ClientReset();
    detail::HostReset();
}

}  // namespace coop::dev::hostile_drill::cord
