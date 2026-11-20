// coop/dev/hostile_drill_dingus.cpp -- see coop/dev/hostile_drill_dingus.h. The arm table, the counters the arm is judged on,
// the verdict print, the lobby-chat lines and the family's entry points; the client's sender is
// hostile_drill_dingus_client.cpp and the host's setup and judge hostile_drill_dingus_host.cpp.

#include "coop/dev/hostile_drill_dingus.h"

#include "hostile_drill_dingus_detail.h"  // co-located private header (src tree, not include/)

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::dev::hostile_drill::dingus {
namespace detail {
namespace {

using F = Field;

// The arm: thirteen reports in one tick, in this order, and what each ends in. The judge's budget is four at once and one in two
// seconds, spent in order on whatever reaches it.
//   key.len 0, key.len 32, a 20-byte payload   three dispatcher drops, none reaches the judge
//   the plain prop's key                       a key that resolves to a live prop that is no dingus: NotDingus (a token)
//   the far dingus's key                       a live dingus 2000 cm from the sender: Unreachable (a token)
//   the far dingus's key, eight times          the two tokens left give Unreachable twice; the other six are Budget
// So the judge sees ten: NotDingus 1, Unreachable 3, Budget 6; nothing is accepted and no key joins the found set.
const Spec kSpecs[] = {
    {"dingus_report", Arm::Report, 13, {{F::Dropped, 3}, {F::NotDingus, 1}, {F::Unreachable, 3}, {F::Budget, 6}}},
};

constexpr const char* kNames[kFields] = {
    "dropped", "not_ready", "dead", "budget", "known", "no_copy", "not_dingus", "unreachable", "accepted", "found_keys",
};

}  // namespace

const char* FieldName(Field f) { return kNames[static_cast<size_t>(f)]; }

Tally TallyOf(const coop::dingus_sync::dev::JudgeView& v, size_t foundKeys) {
    Tally t{};
    t[static_cast<size_t>(F::Dropped)] = v.dropped;
    t[static_cast<size_t>(F::NotReady)] = v.refused[0];
    t[static_cast<size_t>(F::Dead)] = v.refused[1];
    t[static_cast<size_t>(F::Budget)] = v.refused[2];
    t[static_cast<size_t>(F::Known)] = v.refused[3];
    t[static_cast<size_t>(F::NoCopy)] = v.refused[4];
    t[static_cast<size_t>(F::NotDingus)] = v.refused[5];
    t[static_cast<size_t>(F::Unreachable)] = v.refused[6];
    t[static_cast<size_t>(F::Accepted)] = v.accepted;
    t[static_cast<size_t>(F::FoundKeys)] = static_cast<uint32_t>(foundKeys);
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
    const char* const name = SpecOf(a) ? SpecOf(a)->name : "off";
    if (std::strcmp(what, "PASS") == 0) UE_LOGI("[HOSTILE-DRILL] %s PASS -- %s", name, why.c_str());
    else UE_LOGW("[HOSTILE-DRILL] %s %s: %s", name, what, why.c_str());
}

// Lines no save can hold, as a prop class or a place could.
std::string KeyLine(Arm a, const char* label) {
    return std::string("hostile-drill ") + SpecOf(a)->name + " key " + label + " ";
}
std::string DoneLine(Arm a) { return std::string("hostile-drill ") + SpecOf(a)->name + " client-done"; }

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

}  // namespace coop::dev::hostile_drill::dingus
