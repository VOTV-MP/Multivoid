// coop/dev/hostile_drill_kdress.cpp -- see coop/dev/hostile_drill_kdress.h. The phase table, the counters a phase is judged
// on, the verdict print, the lobby-chat lines and the family's entry points; the client's sender is
// hostile_drill_kdress_client.cpp and the host's setup and judge hostile_drill_kdress_host.cpp.

#include "coop/dev/hostile_drill_kdress.h"

#include "hostile_drill_kdress_detail.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/kerfur_dress_sync.h"
#include "coop/net/protocol.h"
#include "coop/props/prop_act_host.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::dev::hostile_drill::kdress {
namespace detail {
namespace {

namespace PAH = coop::prop_act_host;
namespace KS  = coop::kerfur_dress_sync;
using F = Field;
using R = coop::net::PropActReason;
using T = PAH::Tally;
using S = PAH::internal::CommitStep;

// The three phases, each judged at the client's line that ends it. Every field a phase does not name stays 0.
//   asks     five asks of the client, which holds the glasses, one at a time, the near Omega naked:
//              1 a put-on of a row no table has                    WrongFamily, FitFamilyArg
//              2 a put-on of the bucket, the hand holds the glasses WrongFamily, FitFamilyHand
//              3 a put-on of the glasses on the Omega out of reach  Reach
//              4 a put-on of the glasses on the near Omega          Granted, then the client's commit: CommitApply
//              5 the same put-on again                              WrongFamily, FitFamilyRule (worn)
//   takeoffs the host gives the near Omega the glasses, the bucket and the maid; three take-offs, in that order:
//              the glasses and the bucket are granted (the drop budget is two at once), the maid is past the budget:
//              Granted 2, QueueFull 1, FitFamilyBudget 1; the Omega is left wearing the maid
//   pats     eight 273 messages in one tick: a short payload (a dispatcher drop), the pat of the Omega out of reach
//            (unreachable), then six pats of the near Omega: the budget is four at once, one token is spent on the far one,
//            so three are accepted and three are past it
const Phase kTable[kPhases] = {
    {"asks", {{F::Granted, 1}, {F::WrongFamily, 3}, {F::Reach, 1}, {F::FitArg, 1}, {F::FitHand, 1}, {F::FitRule, 1},
              {F::CommitApply, 1}}},
    {"takeoffs", {{F::Granted, 2}, {F::QueueFull, 1}, {F::FitBudget, 1}}},
    {"pats", {{F::PatDropped, 1}, {F::PatUnreachable, 1}, {F::PatAccepted, 3}, {F::PatBudget, 3}}},
};

constexpr const char* kNames[kFields] = {
    "granted", "gone", "wrong_family", "held", "reach", "queue_full",
    "fit_family_arg", "fit_family_rule", "fit_family_hand", "fit_family_budget", "fit_family_unread", "fit_bad_family",
    "commit_apply", "commit_abort", "commit_bad", "commit_target_gone",
    "pat_dropped", "pat_accepted", "pat_not_ready", "pat_dead", "pat_budget", "pat_no_omega", "pat_unreachable", "pat_no_pat",
};

}  // namespace

const char* FieldName(Field f) { return kNames[static_cast<size_t>(f)]; }

Tally Now() {
    Tally t{};
    auto set = [&t](Field f, uint32_t n) { t[static_cast<size_t>(f)] = n; };
    set(F::Granted, PAH::AnsweredFor(kSlot, R::Granted));
    set(F::Gone, PAH::AnsweredFor(kSlot, R::Gone));
    set(F::WrongFamily, PAH::AnsweredFor(kSlot, R::WrongFamily));
    set(F::Held, PAH::AnsweredFor(kSlot, R::Held));
    set(F::Reach, PAH::AnsweredFor(kSlot, R::Reach));
    set(F::QueueFull, PAH::AnsweredFor(kSlot, R::QueueFull));
    set(F::FitArg, PAH::TallyOf(T::FitFamilyArg));
    set(F::FitRule, PAH::TallyOf(T::FitFamilyRule));
    set(F::FitHand, PAH::TallyOf(T::FitFamilyHand));
    set(F::FitBudget, PAH::TallyOf(T::FitFamilyBudget));
    set(F::FitUnread, PAH::TallyOf(T::FitFamilyUnread));
    set(F::FitBadFamily, PAH::TallyOf(T::FitBadFamily));
    set(F::CommitApply, PAH::CommitsOf(kSlot, S::FamilyApply));
    set(F::CommitAbort, PAH::CommitsOf(kSlot, S::Abort));
    set(F::CommitBad, PAH::CommitsOf(kSlot, S::BadCommit));
    set(F::CommitGone, PAH::CommitsOf(kSlot, S::TargetGone));
    const KS::dev::PatView v = KS::dev::Pats();
    set(F::PatDropped, v.dropped);
    set(F::PatAccepted, v.accepted);
    set(F::PatNotReady, v.notReady);
    set(F::PatDead, v.dead);
    set(F::PatBudget, v.budget);
    set(F::PatNoOmega, v.noOmega);
    set(F::PatUnreachable, v.unreachable);
    set(F::PatNoPat, v.noPat);
    return t;
}

const Phase& PhaseAt(int i) { return kTable[i]; }

Tally Expected(const Phase& p) {
    Tally t{};
    for (const Entry& e : p.want)
        if (e.count) t[static_cast<size_t>(e.field)] = e.count;
    return t;
}

void Verdict(const char* what, const std::string& why) {
    if (std::strcmp(what, "PASS") == 0) UE_LOGI("[HOSTILE-DRILL] kdress_ask PASS -- %s", why.c_str());
    else UE_LOGW("[HOSTILE-DRILL] kdress_ask %s: %s", what, why.c_str());
}

// Lines no save can hold, as a prop class or a place could.
std::string Line(const char* what) { return std::string("hostile-drill kdress_ask ") + what; }

}  // namespace detail

Arm ArmOf(const std::string& value) { return value == "kdress_ask" ? Arm::Ask : Arm::None; }

const char* ArmName(Arm a) { return a == Arm::Ask ? "kdress_ask" : "off"; }

void TickHostJudge(coop::net::Session* s, Arm a) {
    if (a == Arm::Ask) detail::HostTick(s);
}

void TickClient(coop::net::Session* s, Arm a) {
    if (a == Arm::Ask) detail::ClientTick(s);
}

void OnDisconnect() {
    detail::ClientReset();
    detail::HostReset();
}

}  // namespace coop::dev::hostile_drill::kdress
