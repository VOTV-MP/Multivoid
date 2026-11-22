// coop/dev/hostile_drill_bucket.cpp -- the bucket_ask arm: see coop/dev/hostile_drill_bucket.h. The host stages the fixtures with the
// bucket drill's stage (a filled bucket, two cement bags and two sponges before the client's puppet) and judges each phase by
// accounting at the client's line that ends it; the client holds the items honestly, then sends raw PropActRequest and PropActCommit
// messages through the session's own send, bypassing the families' watches, one at a time and each after the host's reply to the one
// before (a reply that matches no open record is the lane's stale one, and carries what the host answered).

#include "coop/dev/hostile_drill_bucket.h"

#include "bucket_drill_detail.h"  // the bucket drill's stage and the player's own verbs (co-located private header)

#include "coop/dev/drill_chat.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/bucket_sync.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/bucket.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::dev::hostile_drill::bucket {
namespace {

namespace DC  = coop::dev::drill_chat;
namespace PAH = coop::prop_act_host;
namespace PAR = coop::prop_act_request;
namespace PS  = coop::prop_save_data;
namespace BS  = coop::bucket_sync;
namespace B   = ue_wrap::bucket;
namespace D   = coop::dev::bucket_drill::detail;
using Clock = std::chrono::steady_clock;
using Fam   = coop::net::PropActFamily;
using Kind  = coop::net::ReliableKind;
using R     = coop::net::PropActReason;

constexpr auto kBound = std::chrono::seconds(180);   // a bound says INCONCLUSIVE, never a verdict
constexpr uint8_t kSlot = 1;                         // the one slot the family acts on

// The slots the fixtures stand in: the bucket, then the items by index.
constexpr int kBag = 0, kBag2 = 1, kSponge = 2, kSponge2 = 3;

// The counters the host judges, every one a delta since the phase's baseline.
enum class F : uint8_t {
    Granted, Gone, WrongFamily, Held, Reach, QueueFull, FitArg, FitRule, FitHand, FitBudget, FitUnread, FitBadFamily,
    CommitApply, CommitAbort, CommitBad, CommitGone, NotAwaited, Count
};
constexpr size_t kFields = static_cast<size_t>(F::Count);
using Tally = std::array<uint32_t, kFields>;
constexpr const char* kNames[kFields] = {
    "granted", "gone", "wrong_family", "held", "reach", "queue_full", "fit_family_arg", "fit_family_rule", "fit_family_hand",
    "fit_family_budget", "fit_family_unread", "fit_bad_family", "commit_apply", "commit_abort", "commit_bad", "commit_target_gone",
    "records_not_awaited",
};

Tally Now() {
    using T = PAH::Tally;
    using S = PAH::internal::CommitStep;
    Tally t{};
    auto set = [&t](F f, uint32_t n) { t[static_cast<size_t>(f)] = n; };
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
    set(F::NotAwaited, static_cast<uint32_t>(coop::bucket_sync::CountOf(coop::bucket_sync::Count::RecordsNotAwaited)));
    return t;
}

// The three phases, each judged at the client's line that ends it. The client holds the cement bag in the first, a sponge in the second
// and a second cement bag in the third, which the host staged with 5 units: its take seeded the ledger with them.
//   asks     six asks, one at a time, the bucket full of water and holding no soap:
//              1 a soap of 99                                    WrongFamily, FitFamilyArg
//              2 a mix of 7 units, the bucket's water gives 20   WrongFamily, FitFamilyRule
//              3 a transfer in direction 2 to a world bag        WrongFamily, FitFamilyArg
//              4 a mix aimed at a sponge                         WrongFamily, FitFamilyArg
//              5 the honest mix                                  Granted (the bucket is reserved)
//              6 the same mix, sent while 5 holds the bucket     waits: the host counts a reservation wait with no sixth reply
//                                                                sent; the client then aborts 5 (CommitAbort), and 6 is answered
//                                                                Granted after that abort; the client aborts 6 (CommitAbort)
//   dip      the sponge held, a dip with the soap branch on a bucket with no soap: WrongFamily, FitFamilyRule
//   ledger   the 5-unit bag held, a mix the water gives 20 for, twice: WrongFamily, FitFamilyHand each; between them the client sends
//            the host a save record for the key of the first bag, which no drop awaits (counted as not awaited): under a gate that
//            ended the sender's ledger at any bag's record the second mix would be granted
constexpr uint32_t kLedgerUnits = 5;   // what the host writes into its staged second bag before the client takes it

// A phase's baseline is taken at the client's line that says its item is held, so the take's grant is no part of the phase; the client
// takes the next item only after the host's judged line, so no later phase's grant lands in a judged window either.
struct Phase {
    const char* name;
    Tally want;
};
// The Held refusal (a target another holds) is the registry's, proven by the act drill's race phase (rows "client hand" and "host
// held", prop_act_drill_race.cpp:77,79) and by prop_act_selftest's Decide checks; no row here asks it.
const Phase kPhases[3] = {
    {"asks", {2, 0, 4, 0, 0, 0, 3, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0}},
    {"dip", {0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"ledger", {0, 0, 2, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1}},
};

void Verdict(const char* what, const std::string& why) {
    BS::dev::PinCostLine();   // the peer's pin cost, before its verdict: the rig kills the peers, so no teardown line follows
    if (std::strcmp(what, "PASS") == 0) UE_LOGI("[HOSTILE-DRILL] bucket_ask PASS -- %s", why.c_str());
    else UE_LOGW("[HOSTILE-DRILL] bucket_ask %s: %s", what, why.c_str());
}

// Lines no save can hold, as a prop class or a place could.
std::string Line(const char* what) { return std::string("hostile-drill bucket_ask ") + what; }

// ---- the host ----

enum class HStep { Begin, Stage, WaitHeld, PhaseA, WaitSponge, PhaseB, WaitBag2, PhaseC, Done };
HStep g_h = HStep::Begin;
Tally g_base{};
uint32_t g_waitBase = 0, g_answeredBase = 0;   // the reservation waits and the replies the host had counted at the phase's start
Clock::time_point g_armAt{};
bool g_staged = false, g_said = false, g_waitJudged = false;
uint16_t g_playerNo = 0;
const char* g_waiting = "the stage";

void Finish(const char* what, const std::string& why) {
    Verdict(what, why);
    g_h = HStep::Done;
}

// A phase begins: its counters' baseline, and its bound (INCONCLUSIVE, never a verdict) counts from here, not from the arm.
void StartPhase() {
    g_base = Now();
    g_waitBase = PAH::WaitsOf(kSlot, PAH::WaitCause::Reservation);
    g_answeredBase = PAH::AnsweredTotal(kSlot);
    g_armAt = Clock::now();
}

bool Bounded() {
    if (Clock::now() - g_armAt <= kBound) return false;
    Finish("INCONCLUSIVE", std::string("the precondition never held: waiting for ") + g_waiting);
    return true;
}

std::string Mismatch(const Phase& ph, const Tally& d) {
    std::string bad;
    for (size_t i = 0; i < kFields; ++i)
        if (d[i] != ph.want[i])
            bad += std::string(" ") + kNames[i] + " " + std::to_string(d[i]) + " (wanted " + std::to_string(ph.want[i]) + ");";
    return bad;
}

std::string Summary(const Tally& d) {
    std::string out;
    for (size_t i = 0; i < kFields; ++i)
        if (d[i]) out += (out.empty() ? "" : ", ") + std::string(kNames[i]) + "=" + std::to_string(d[i]);
    return out.empty() ? "nothing counted" : out;
}

// The client's line that ends the phase: judge the counters. True once the phase matches its table; false while the line is awaited
// and after a verdict.
bool JudgePhase(coop::net::Session* s, int index, const char* doneLine) {
    if (!DC::SeenHas(Line(doneLine))) {
        g_waiting = "the client's phase line";
        Bounded();
        return false;
    }
    if (!s->HasPeerConn(kSlot)) {
        Finish("INCONCLUSIVE", "the judged slot left before the verdict");
        return false;
    }
    if (coop::roster_ledger::Get(kSlot).playerNo != g_playerNo) {
        Finish("INCONCLUSIVE", "slot-replaced (the judged slot's occupant changed)");
        return false;
    }
    const Tally now = Now();
    Tally d{};
    for (size_t i = 0; i < kFields; ++i) d[i] = now[i] >= g_base[i] ? now[i] - g_base[i] : g_base[i] - now[i];
    const std::string bad = Mismatch(kPhases[index], d);
    if (!bad.empty()) {
        Finish("FAIL", std::string("phase ") + kPhases[index].name + ": the counters differ from the table:" + bad + " got " + Summary(d));
        return false;
    }
    UE_LOGI("[HOSTILE-DRILL] bucket_ask phase %s matches the table: %s", kPhases[index].name, Summary(d).c_str());
    return true;
}

// ---- the client ----

enum class CStep { WaitStage, WaitHand, WaitGo, Asking, WaitJudged, Swap, WaitGo2, WaitGo3, Done };
CStep g_c = CStep::WaitStage;
bool g_idleSaid = false, g_historyOn = false;
Clock::time_point g_since{};
const char* g_cwaiting = nullptr;
int g_ask = 0, g_win = 0, g_swap = 0, g_phase = 0;
uint32_t g_staleBase = 0;
std::wstring g_bagKey;     // the key of the first bag the client held: the world bag it left behind answers to it
int g_forge = 0;           // 0 not sent, 1 the unawaited record is on its way and its answer is waited for
uint64_t g_rowsBase = 0;

// The asks, in the table's order: the phase, the family, the target's slot, the verb (an int, a byte, or the need the client's own
// copy gives), and what the host must answer: granted, or the reason. `abortId` names the ask whose reservation the client releases
// once this one is answered. `forge`: the client first sends the host a save record for the key of the world bag it left behind,
// which no drop of its awaits, and waits for the host's row of that bag, the host's answer to a refused record. `releases` names
// the earlier ask whose reservation this ask is sent under: the host must wait it (it says so), and only then does the client abort
// that ask, so this ask's answer can come only after the abort.
enum class Verb : uint8_t { Int, Byte, Need };
struct Ask {
    int phase;
    Fam family;
    int slot;
    Verb verb;
    int value;
    uint8_t granted, reason;
    int abortId;
    const char* name;
    bool forge = false;
    int releases = 0;
};
// The target slots: 0 the bucket, kMaxPlains + the item slot otherwise.
constexpr int kTBucket = 0, kTBag2 = D::kMaxPlains + kBag2, kTSponge2 = D::kMaxPlains + kSponge2;
const Ask kAsks[] = {
    {0, Fam::BucketSoap, kTBucket, Verb::Int, 99, 0, 2, 0, "a soap of 99"},
    {0, Fam::BucketMix, kTBucket, Verb::Int, 7, 0, 2, 0, "a mix of 7 units"},
    {0, Fam::BagTransfer, kTBag2, Verb::Byte, 2, 0, 2, 0, "a transfer in direction 2"},
    {0, Fam::BucketMix, kTSponge2, Verb::Int, 20, 0, 2, 0, "a mix aimed at a sponge"},
    {0, Fam::BucketMix, kTBucket, Verb::Need, 0, 1, 0, 0, "the honest mix"},
    {0, Fam::BucketMix, kTBucket, Verb::Need, 0, 1, 0, 6, "the same mix, sent while the first holds the bucket", false, 5},
    {1, Fam::BucketDip, kTBucket, Verb::Byte, 1, 0, 2, 0, "a dip with the soap branch on a bucket with no soap"},
    {2, Fam::BucketMix, kTBucket, Verb::Need, 0, 0, 2, 0, "a mix the held bag's ledger cannot pay"},
    {2, Fam::BucketMix, kTBucket, Verb::Need, 0, 0, 2, 0, "the same mix after a record for another bag's key", true},
};
constexpr int kAskCount = sizeof(kAsks) / sizeof(kAsks[0]);

// The replies the host has sent before the waiting ask is sent: the asks of its phase that precede it, every one answered.
uint32_t AnsweredBeforeWait() {
    uint32_t n = 0;
    for (int i = 0; i < kAskCount && kAsks[i].releases == 0; ++i)
        if (kAsks[i].phase == 0) ++n;
    return n;
}

// The index of the first ask of a phase.
int FirstAskOf(int phase) {
    for (int i = 0; i < kAskCount; ++i)
        if (kAsks[i].phase == phase) return i;
    return kAskCount;
}

// The host, in the first phase: the second mix has reached the host and waits behind the first's reservation, no reply sent for it.
// The client aborts the first mix only after the host's line, so the second's answer follows the abort. True once judged.
bool JudgeWait() {
    if (PAH::WaitsOf(kSlot, PAH::WaitCause::Reservation) <= g_waitBase) {
        g_waiting = "the second mix to wait behind the first's reservation";
        Bounded();
        return false;
    }
    const uint32_t answered = PAH::AnsweredTotal(kSlot) - g_answeredBase;
    if (answered != AnsweredBeforeWait()) {
        Finish("FAIL", "the host waited the second mix but had sent " + std::to_string(answered) + " replies, " +
                           std::to_string(AnsweredBeforeWait()) + " wanted before it");
        return false;
    }
    DC::Say(Line("w-ok"));
    g_waitJudged = true;
    return true;
}

void Inconclusive(const std::string& why) {
    Verdict("INCONCLUSIVE", why);
    g_c = CStep::Done;
}

bool Late(const char* what) {
    if (!g_cwaiting || std::strcmp(g_cwaiting, what) != 0) {
        g_cwaiting = what;
        g_since = Clock::now();
    }
    return Clock::now() - g_since > kBound;
}

int Need(void* bucket) {
    B::Plain p;
    return bucket && B::ReadPlain(bucket, p) && p.maxHeight > 0.f ? static_cast<int>(std::floor(p.height / p.maxHeight * 20.f + 0.5f)) : 0;
}

bool SendAsk(coop::net::Session* s, const Ask& a, int eid, uint16_t id) {
    coop::net::PropActRequestPayload p{};
    p.id = id;
    p.family = static_cast<uint8_t>(a.family);
    p.eid = static_cast<uint32_t>(eid);
    if (a.verb == Verb::Byte) {
        p.verb[0] = static_cast<char>(a.value);
    } else {
        const int32_t x = a.verb == Verb::Need ? Need(D::ClientBucket(0)) : a.value;
        std::memcpy(p.verb, &x, sizeof(x));
    }
    return s->SendReliable(Kind::PropActRequest, &p, sizeof(p));
}

bool SendAbort(coop::net::Session* s, uint16_t id, uint32_t eid) {
    coop::net::PropActCommitPayload p{};
    p.id = id;
    p.committed = 0;
    p.eid = eid;
    return s->SendReliable(Kind::PropActCommit, &p, sizeof(p));
}

// The ask `g_ask` of this phase, one reply at a time: the id is the ask's number; the reply is the lane's stale one.
void RunAsks(coop::net::Session* s, int phase) {
    if (g_ask >= kAskCount || kAsks[g_ask].phase != phase) {
        DC::Say(Line(phase == 0 ? "p1-done" : phase == 1 ? "p2-done" : "p3-done"));
        UE_LOGI("[HOSTILE-DRILL] bucket_ask sent the asks of phase %d", phase + 1);
        g_cwaiting = nullptr;
        g_win = g_swap = 0;
        g_phase = phase + 1;
        // The next item is taken only after the host has judged this phase.
        g_c = phase < 2 ? CStep::WaitJudged : CStep::Done;
        return;
    }
    const Ask& a = kAsks[g_ask];
    const uint16_t id = static_cast<uint16_t>(g_ask + 1);
    const PAR::dev::LastReply last = PAR::dev::LastStaleReply();
    if (g_win == 0 && a.forge) {
        if (g_forge == 0) {
            void* const bag = D::ByKey(g_bagKey);
            g_rowsBase = BS::CountOf(BS::Count::RowsApplied);
            if (!bag || !PS::Publish(s, bag, g_bagKey)) return Inconclusive("the record for the world bag's key could not be sent");
            g_forge = 1;
            return;
        }
        if (BS::CountOf(BS::Count::RowsApplied) <= g_rowsBase) {   // the host judged the record before it sent the row
            if (Late("the host's row of the bag whose record it refused")) Inconclusive("the host never answered the unawaited record");
            return;
        }
        g_cwaiting = nullptr;
    }
    if (g_win == 0) {   // not yet sent
        g_staleBase = last.count;
        if (!SendAsk(s, a, D::ClientEid(a.slot), id)) return Inconclusive(std::string("the ask '") + a.name + "' was not sent");
        g_win = 1;
        g_since = Clock::now();
        g_cwaiting = "the host's reply";
        return;
    }
    if (a.releases != 0 && g_win == 1) {   // sent under the first ask's reservation: no reply may come before that ask is aborted
        if (last.count != g_staleBase) {
            Verdict("FAIL", std::string("the ask '") + a.name + "' was answered (id " + std::to_string(last.id) +
                                ") while the ask before it still held the bucket");
            DC::Say(Line("reply-fail"));
            g_c = CStep::Done;
            return;
        }
        if (!DC::SeenHas(Line("w-ok"))) {
            if (Late("the host's line that the second mix waits")) Inconclusive("the host never said the second mix waited");
            return;
        }
        if (!SendAbort(s, static_cast<uint16_t>(a.releases), static_cast<uint32_t>(D::ClientEid(kAsks[a.releases - 1].slot))))
            return Inconclusive("the abort of the first mix was not sent");
        g_win = 2;
        g_since = Clock::now();
        g_cwaiting = "the host's reply";
        return;
    }
    if (last.count == g_staleBase) {
        if (Late("the host's reply")) Inconclusive(std::string("the host never answered the ask '") + a.name + "'");
        return;
    }
    if (last.id != id || last.granted != a.granted || last.reason != a.reason) {
        Verdict("FAIL", std::string("the ask '") + a.name + "' was answered granted=" + std::to_string(last.granted) + " reason=" +
                            std::to_string(last.reason) + " (id " + std::to_string(last.id) + "), wanted granted=" +
                            std::to_string(a.granted) + " reason=" + std::to_string(a.reason));
        DC::Say(Line("reply-fail"));
        g_c = CStep::Done;
        return;
    }
    if (a.abortId > 0 && !SendAbort(s, static_cast<uint16_t>(a.abortId), static_cast<uint32_t>(D::ClientEid(kAsks[a.abortId - 1].slot)))) {
        Inconclusive("the abort of the reserved ask was not sent");
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] bucket_ask the ask '%s' was answered granted=%u reason=%u", a.name, static_cast<unsigned>(last.granted),
            static_cast<unsigned>(last.reason));
    ++g_ask;
    g_win = 0;
}

}  // namespace

Arm ArmOf(const std::string& value) { return value == "bucket_ask" ? Arm::Ask : Arm::None; }

const char* ArmName(Arm a) { return a == Arm::Ask ? "bucket_ask" : "off"; }

void TickHostJudge(coop::net::Session* s, Arm a) {
    if (a != Arm::Ask || !s || g_h == HStep::Done) return;
    if (g_h == HStep::Begin) {
        g_armAt = Clock::now();
        D::NameVerdict("ask");   // the stage's INVALID verdicts read `bucket_ask`
        g_h = HStep::Stage;
    }
    if (g_h > HStep::Stage && g_h < HStep::Done && DC::SeenHas(Line("reply-fail")))
        return Finish("FAIL", "a forged ask was answered other than the table says: the client's log names the ask");
    switch (g_h) {
    case HStep::Stage: {
        if (Bounded()) return;
        if (!g_staged) {
            D::Spec sp;
            sp.plains = 1;
            sp.items[0] = L"prop_cementBag";
            sp.items[1] = L"prop_cementBag";
            sp.items[2] = L"prop_sponge";
            sp.items[3] = L"prop_sponge";
            sp.nItems = 4;
            sp.itemFwdCm = -150.f;
            sp.itemSpreadCm = 45.f;
            D::StageBegin(sp);
            g_staged = true;
        }
        g_waiting = "the stage";
        if (!D::StageStep(*s)) {
            if (D::Ended()) g_h = HStep::Done;   // the stage has said INVALID: nothing after it runs
            return;
        }
        if (!g_said) {
            if (!D::StageSay()) return;
            g_said = true;
        }
        // The second bag is half spent before the client takes it: the host's count of its take is what the third phase's mix is judged by.
        if (!D::StageItem(kBag2) || !B::WriteBagUnits(D::StageItem(kBag2), static_cast<int32_t>(kLedgerUnits)))
            return Finish("INCONCLUSIVE", "the units of the second staged bag could not be written");
        g_playerNo = coop::roster_ledger::Get(kSlot).playerNo;
        DC::SeenHas(Line("held-bag"));   // starts the chat history
        UE_LOGI("[HOSTILE-DRILL] bucket_ask armed on slot %u: the bucket, two bags and two sponges stand before its puppet",
                static_cast<unsigned>(kSlot));
        g_h = HStep::WaitHeld;
        return;
    }
    case HStep::WaitHeld:
        if (!DC::SeenHas(Line("held-bag"))) {
            g_waiting = "the client's hold of the bag";
            Bounded();
            return;
        }
        StartPhase();
        DC::Say(Line("go1"));
        g_h = HStep::PhaseA;
        return;
    case HStep::PhaseA:
        if (!g_waitJudged && !JudgeWait()) return;
        if (JudgePhase(s, 0, "p1-done")) {
            DC::Say(Line("judged1"));   // the client takes the sponge only after this: its grant lands in no judged window
            g_h = HStep::WaitSponge;
        }
        return;
    case HStep::WaitSponge:
        if (!DC::SeenHas(Line("held-sponge"))) {
            g_waiting = "the client's hold of the sponge";
            Bounded();
            return;
        }
        StartPhase();
        DC::Say(Line("go2"));
        g_h = HStep::PhaseB;
        return;
    case HStep::PhaseB:
        if (JudgePhase(s, 1, "p2-done")) {
            DC::Say(Line("judged2"));   // the client takes the second bag only after this
            g_h = HStep::WaitBag2;
        }
        return;
    case HStep::WaitBag2:
        if (!DC::SeenHas(Line("held-bag2"))) {
            g_waiting = "the client's hold of the second bag";
            Bounded();
            return;
        }
        StartPhase();
        DC::Say(Line("go3"));
        g_h = HStep::PhaseC;
        return;
    case HStep::PhaseC:
        if (JudgePhase(s, 2, "p3-done"))
            Finish("PASS", "the forged asks were answered and counted as the table, one reason each, a second ask on a reserved bucket "
                           "waited and was answered after the first's abort, the ledger held against an unawaited record, and nothing a "
                           "client forged was written");
        return;
    default:
        return;
    }
}

void TickClient(coop::net::Session* s, Arm a) {
    if (a != Arm::Ask || !s || g_c == CStep::Done) return;
    if (!g_historyOn) {
        g_historyOn = true;
        DC::SeenHas(Line("go1"));   // the first read starts the chat history
    }
    void* const me = D::Local();
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (!me || self == 0 || self >= coop::players::kMaxPeers) return;
    if (self != kSlot) {
        if (!g_idleSaid) {
            g_idleSaid = true;
            UE_LOGI("[HOSTILE-DRILL] bucket_ask idles: this client is slot %u, the family acts on slot %u", static_cast<unsigned>(self),
                    static_cast<unsigned>(kSlot));
        }
        g_c = CStep::Done;
        return;
    }
    switch (g_c) {
    case CStep::WaitStage: {
        if (!D::ClientBindStage(1, 4)) {
            if (Late("the host's stage")) Inconclusive("the host never staged the fixtures");
            return;
        }
        if (!D::Hold(me, D::ClientItem(kBag))) return Inconclusive("Hold Object did not run");
        g_cwaiting = nullptr;
        g_c = CStep::WaitHand;
        return;
    }
    case CStep::WaitHand:
        if (!D::HandReady()) {
            if (Late("the hand's hold")) Inconclusive("the hand never held the cement bag the lane announced");
            return;
        }
        g_bagKey = ue_wrap::prop::GetKeyString(coop::hand_item::LocalHandActor());
        if (g_bagKey.empty()) return Inconclusive("the held bag has no key");
        DC::Say(Line("held-bag"));
        g_cwaiting = nullptr;
        g_c = CStep::WaitGo;
        return;
    case CStep::WaitGo:
        if (!DC::SeenHas(Line("go1"))) {
            if (Late("the host's go line")) Inconclusive("the host never said go");
            return;
        }
        g_cwaiting = nullptr;
        g_win = 0;
        g_c = CStep::Asking;
        return;
    case CStep::Asking:
        return RunAsks(s, g_phase);
    case CStep::WaitJudged:   // g_phase is the phase just asked plus one
        if (!DC::SeenHas(Line(g_phase == 1 ? "judged1" : "judged2"))) {
            if (Late("the host's judgement of the phase")) Inconclusive("the host never said it judged the phase");
            return;
        }
        g_cwaiting = nullptr;
        g_c = CStep::Swap;
        return;
    case CStep::Swap:   // what is held is put down and the next phase's item taken
        if (g_swap == 0) {
            if (!D::HandEmpty() && !D::DropHeld(me)) return Inconclusive("the drop of the held item did not run");
            g_swap = 1;
            return;
        }
        if (g_swap == 1) {
            if (!D::HandEmpty()) {
                if (Late("the hand's emptying")) Inconclusive("the hand never emptied after the drop");
                return;
            }
            if (!D::Hold(me, D::ClientItem(g_phase == 1 ? kSponge : kBag2))) return Inconclusive("Hold Object did not run");
            g_swap = 2;
            return;
        }
        if (!D::HandReady()) {
            if (Late("the next item's hold")) Inconclusive("the hand never held the item the lane announced");
            return;
        }
        DC::Say(Line(g_phase == 1 ? "held-sponge" : "held-bag2"));
        g_cwaiting = nullptr;
        g_c = g_phase == 1 ? CStep::WaitGo2 : CStep::WaitGo3;
        return;
    case CStep::WaitGo2:
        if (!DC::SeenHas(Line("go2"))) {
            if (Late("the host's second go line")) Inconclusive("the host never said the second go");
            return;
        }
        g_cwaiting = nullptr;
        g_win = 0;
        g_c = CStep::Asking;
        g_ask = FirstAskOf(1);
        return RunAsks(s, g_phase);
    case CStep::WaitGo3:
        if (!DC::SeenHas(Line("go3"))) {
            if (Late("the host's third go line")) Inconclusive("the host never said the third go");
            return;
        }
        g_cwaiting = nullptr;
        g_win = 0;
        g_c = CStep::Asking;
        g_ask = FirstAskOf(2);
        return RunAsks(s, g_phase);
    case CStep::Done:
        return;
    }
}

void OnDisconnect() {
    g_h = HStep::Begin;
    g_base = Tally{};
    g_waitBase = g_answeredBase = 0;
    g_armAt = {};
    g_staged = g_said = g_waitJudged = false;
    g_playerNo = 0;
    g_waiting = "the stage";
    g_c = CStep::WaitStage;
    g_idleSaid = g_historyOn = false;
    g_since = {};
    g_cwaiting = nullptr;
    g_ask = g_win = g_swap = g_phase = 0;
    g_staleBase = 0;
    g_bagKey.clear();
    g_forge = 0;
    g_rowsBase = 0;
}

}  // namespace coop::dev::hostile_drill::bucket
