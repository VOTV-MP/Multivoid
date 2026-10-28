// coop/dev/hostile_drill_coffee.cpp -- see coop/dev/hostile_drill_coffee.h. The arm table, the counters an arm is judged
// on, the verdict print, the lobby-chat lines and the family's entry points; the client's sender is
// hostile_drill_coffee_client.cpp and the host's setup and judge hostile_drill_coffee_host.cpp.

#include "coop/dev/hostile_drill_coffee.h"

#include "hostile_drill_coffee_detail.h"  // co-located private header (src tree, not include/)

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::dev::hostile_drill::coffee {
namespace detail {
namespace {

using F = Field;

// The arms: the prop each needs, the messages sent, then the host's expected deltas. A message ends in exactly one of
// the dispatcher's drops, a refusal, a run, or (the record arm) a refused record; `Claims` counts the presses that got
// past the dispatcher, queued or refused for a full queue, and equals the sum of the refusals and the runs.
//   malformed     a press one byte short; a press whose key is 32 bytes (the wire holds 31)
//   wrong_class   a press on the key of a crowbar within reach: the key resolves, the class is no coffee device
//   reach         a press on a maker whose prop stands 2000 uu from the sender: refused for reach
//   flood         20 presses on a maker within reach, in one tick: the sender's queue holds 8, which run (the maker's
//                 own gate declines each, there being no cup), and refuses the other 12
//   dead_sender   one well-formed press on a maker within reach, sent while the sender's player is dead: refused as
//                 dead (judged before reach and class)
//   forged_record the client's record of its copy of the host's maker with hasCup set, sent as an intent: refused by
//                 the maker's record gate, nothing published
const Spec kSpecs[] = {
    {"coffee_malformed",     Arm::Malformed,     Fixture::None,        Mode::Plain, 2,
     {{F::DropSize, 1}, {F::DropKey, 1}}},
    {"coffee_wrong_class",   Arm::WrongClass,    Fixture::NearCrowbar, Mode::Plain, 1,
     {{F::Claims, 1}, {F::WrongClass, 1}}},
    {"coffee_reach",         Arm::Reach,         Fixture::FarMaker,    Mode::Plain, 1,
     {{F::Claims, 1}, {F::OutOfReach, 1}}},
    {"coffee_flood",         Arm::Flood,         Fixture::NearMaker,   Mode::Plain, 20,
     {{F::Claims, 20}, {F::Ran, 8}, {F::QueueFull, 12}}},
    {"coffee_forged_record", Arm::ForgedRecord,  Fixture::NearMaker,   Mode::Plain, 1,
     {{F::RecordsRefused, 1}}},
    {"coffee_dead_sender",   Arm::DeadSender,    Fixture::NearMaker,   Mode::Dead,  1,
     {{F::Claims, 1}, {F::SenderDead, 1}}},
};

constexpr const char* kNames[kFields] = {
    "claims", "ran", "no_device", "wrong_class", "out_of_reach", "not_run", "queue_full", "sender_dead", "drop_size",
    "drop_key",
    "made_maker", "records_refused",
};

}  // namespace

const char* FieldName(Field f) { return kNames[static_cast<size_t>(f)]; }

Tally TallyOf(const coop::coffee_intent::DrillView& v, const coop::coffee_state::DrillView& st) {
    Tally t{};
    t[static_cast<size_t>(F::Claims)] = static_cast<uint32_t>(v.claims);
    t[static_cast<size_t>(F::Ran)] = static_cast<uint32_t>(v.ran);
    t[static_cast<size_t>(F::NoDevice)] = static_cast<uint32_t>(v.noDevice);
    t[static_cast<size_t>(F::WrongClass)] = static_cast<uint32_t>(v.wrongClass);
    t[static_cast<size_t>(F::OutOfReach)] = static_cast<uint32_t>(v.outOfReach);
    t[static_cast<size_t>(F::NotRun)] = static_cast<uint32_t>(v.notRun);
    t[static_cast<size_t>(F::QueueFull)] = static_cast<uint32_t>(v.queueFull);
    t[static_cast<size_t>(F::SenderDead)] = static_cast<uint32_t>(v.senderDead);
    t[static_cast<size_t>(F::DropSize)] = static_cast<uint32_t>(v.dropSize);
    t[static_cast<size_t>(F::DropKey)] = static_cast<uint32_t>(v.dropKey);
    t[static_cast<size_t>(F::MadeMaker)] = static_cast<uint32_t>(st.madeMaker);
    t[static_cast<size_t>(F::RecordsRefused)] = static_cast<uint32_t>(st.recordsRefused);
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

const wchar_t* FixtureClass(Fixture f) {
    return f == Fixture::NearCrowbar ? kCrowbarCls : kMakerCls;
}

void Verdict(Arm a, const char* what, const std::string& why) {
    if (std::strcmp(what, "PASS") == 0) UE_LOGI("[HOSTILE-DRILL] %s PASS -- %s", ArmName(a), why.c_str());
    else UE_LOGW("[HOSTILE-DRILL] %s %s: %s", ArmName(a), what, why.c_str());
}

// Lines no save can hold, as a prop class or a place could.
std::string ReadyLine(Arm a) { return std::string("hostile-drill ") + ArmName(a) + " host-ready"; }
std::string ClientReadyLine(Arm a) { return std::string("hostile-drill ") + ArmName(a) + " client-ready"; }

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

}  // namespace coop::dev::hostile_drill::coffee
