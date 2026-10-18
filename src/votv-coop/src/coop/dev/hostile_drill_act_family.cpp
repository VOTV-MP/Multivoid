// coop/dev/hostile_drill_act_family.cpp -- the act_wrongfamily arm: a client forges acts the game would never perform,
// and the host's judge refuses each by its own term. See coop/dev/hostile_drill_act.h.
//
//   HOST   -- spawns four targets on four different columns round slot 1's puppet (a crowbar, a heater, a shrimp, the
//             erie plush), takes the judge's counters, says their eids, and judges when the client says it sent.
//   CLIENT -- reads the eids and sends six requests, one at a time, each after the host's reply to the one before.

#include "coop/dev/hostile_drill_act.h"

#include "hostile_drill_act_detail.h"  // co-located private header (src tree, not include/)

#include "coop/comms/chat_log.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine_attach.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace coop::dev::hostile_drill::act {
using namespace detail;
namespace {

using Clock  = std::chrono::steady_clock;
using Family = coop::net::PropActFamily;
using Reason = coop::net::PropActReason;
namespace EL  = coop::element;
namespace PAH = coop::prop_act_host;
namespace PAR = coop::prop_act_request;

constexpr Arm kArm = Arm::ActWrongFamily;
constexpr int kSlot = 1;   // the client this arm drives, as the act drill's single client
constexpr const wchar_t* kHeaterPath = L"/Game/objects/prop_heater.prop_heater_C";
constexpr const wchar_t* kShrimpPath = L"/Game/objects/prop_food_shrimp.prop_food_shrimp_C";
constexpr const wchar_t* kPlushPath  = L"/Game/objects/prop_eriePlush.prop_eriePlush_C";
constexpr auto kReplyBound = std::chrono::seconds(10);   // CLIENT: a request's send to the host's reply
constexpr auto kSentBound = std::chrono::seconds(60);    // HOST: its ready line to the client's sent line
constexpr auto kReadyBound = std::chrono::seconds(180);  // a failure bound on the announce, and on the client's wait for it

// The four targets, in the order of the ready line.
enum Tgt { kCrowbar, kHeater, kShrimp, kPlush, kTargets };
constexpr const char* kKeys[kTargets] = {"crowbar", "heater", "shrimp", "plush"};
constexpr const wchar_t* kPaths[kTargets] = {kCrowbarPath, kHeaterPath, kShrimpPath, kPlushPath};

// The six forged requests, each of which the judge must refuse by a term of its own.
struct Req {
    const char* name;
    Tgt target;
    uint8_t family;
};
constexpr uint8_t kBadFamily = 4;   // past the last family byte
constexpr Req kReqs[] = {
    {"crowbar Use", kCrowbar, static_cast<uint8_t>(Family::Use)},
    {"heater Pocket", kHeater, static_cast<uint8_t>(Family::Pocket)},
    {"heater Hold", kHeater, static_cast<uint8_t>(Family::Hold)},
    {"shrimp Use", kShrimp, static_cast<uint8_t>(Family::Use)},
    {"plush Use", kPlush, static_cast<uint8_t>(Family::Use)},
    {"crowbar family 4", kCrowbar, kBadFamily},
};
constexpr size_t kSix = sizeof(kReqs) / sizeof(kReqs[0]);

// The judge's counters of the four refusing terms, in the order the ready line is judged.
constexpr PAH::Tally kTerms[] = {PAH::Tally::FitNoUseRow, PAH::Tally::FitHeavy, PAH::Tally::FitDenied,
                                 PAH::Tally::FitBadFamily};
constexpr uint32_t kTermWants[] = {1, 2, 2, 1};
constexpr size_t kTermCount = sizeof(kTerms) / sizeof(kTerms[0]);

const std::string kTargetsHead = "hostile-drill act targets ";
const std::string kSentLine = "hostile-drill act sent";

// ---- client (sender) ----
enum class CStep { Idle, AwaitReply, Done };
CStep g_cstep = CStep::Idle;
EL::ElementId g_ceids[kTargets] = {};
size_t g_sent = 0;           // requests sent so far; the reply awaited is the last one's
uint32_t g_staleBase = 0;
Clock::time_point g_sentAt{};
Clock::time_point g_idleAt{};   // the client's first Idle tick on its own slot: the targets line's wait counts from it

void ClientEnd(const char* what, const std::string& why) {
    LogVerdict(kArm, what, why);
    g_cstep = CStep::Done;
}

// The host's line is `hostile-drill act targets crowbar=<eid> heater=<eid> shrimp=<eid> plush=<eid>`; false when a
// key is missing.
bool ParseTargets(const std::string& line, EL::ElementId (&eids)[kTargets], const char** missing) {
    const size_t at = line.find(kTargetsHead);
    for (int i = 0; i < kTargets; ++i) {
        const std::string key = std::string(kKeys[i]) + "=";
        const size_t k = at == std::string::npos ? std::string::npos : line.find(key, at + kTargetsHead.size());
        if (k == std::string::npos) {
            *missing = kKeys[i];
            return false;
        }
        eids[i] = static_cast<EL::ElementId>(std::strtoul(line.c_str() + k + key.size(), nullptr, 10));
    }
    return true;
}

bool SendNext(coop::net::Session* s) {
    const Req& r = kReqs[g_sent];
    g_staleBase = PAR::dev::LastStaleReply().count;
    if (!SendRequest(s, static_cast<uint16_t>(g_sent + 1), static_cast<Family>(r.family), g_ceids[r.target])) {
        ClientEnd("INCONCLUSIVE", std::string("the request ") + r.name + " was not sent");
        return false;
    }
    UE_LOGI("[HOSTILE-DRILL] %s sent %s (family %u) for eid %u", ArmName(kArm), r.name, static_cast<unsigned>(r.family),
            static_cast<unsigned>(g_ceids[r.target]));
    g_sentAt = Clock::now();
    ++g_sent;
    return true;
}

// ---- host (judge) ----
enum class HStep { WaitArm, Announce, AwaitSent, Done };
HStep g_hstep = HStep::WaitArm;
uint16_t g_playerNo = 0;   // slot 1's occupant at arm: the counters reset when it changes
Clock::time_point g_armAt{};
ue_wrap::CachedObjRef g_targets[kTargets];
EL::ElementId g_eids[kTargets] = {};
uint32_t g_answeredBase = 0, g_totalBase = 0;
uint32_t g_termBase[kTermCount] = {};

void Finish(const char* what, const std::string& why) {
    LogVerdict(kArm, what, why);
    g_hstep = HStep::Done;
}

bool Bounded(const Clock::time_point& since, std::chrono::seconds bound, const char* what) {
    if (Clock::now() - since <= bound) return false;
    Finish("FAIL", what);
    return true;
}

// Four targets on four different columns: each takes the first direction with a floor after the one before's.
void SpawnTargets(coop::RemotePlayer& rp) {
    int dir = 0;
    for (int i = 0; i < kTargets; ++i) {
        void* actor = nullptr;
        int used = 0;
        if (const char* why = SpawnOnFloor(rp, kPaths[i], false, dir, &used, &actor)) {
            Finish("INCONCLUSIVE", std::string(kKeys[i]) + ": " + why);
            return;
        }
        g_targets[i].Set(actor);
        dir = used + 1;
    }
    g_playerNo = coop::roster_ledger::Get(kSlot).playerNo;
    g_armAt = Clock::now();
    g_hstep = HStep::Announce;
}

void TakeBaselines() {
    g_answeredBase = PAH::AnsweredFor(kSlot, Reason::WrongFamily);
    g_totalBase = PAH::AnsweredTotal(kSlot);
    for (size_t i = 0; i < kTermCount; ++i) g_termBase[i] = PAH::TallyOf(kTerms[i]);
}

// The targets are announced when each has an eid; then the heater must simulate, or the pocket never reaches the heavy
// term. The baselines are taken and the eids said.
void AnnounceTargets() {
    if (Bounded(g_armAt, kReadyBound, "the targets never became ready")) return;
    for (int i = 0; i < kTargets; ++i) {
        void* const actor = g_targets[i].Get();
        if (!actor) {
            Finish("INCONCLUSIVE", std::string("the spawned ") + kKeys[i] + " was gone before it was announced");
            return;
        }
        g_eids[i] = EL::Registry::Get().EidForActor(actor);
        if (g_eids[i] == EL::kInvalidId) return;   // not announced yet
    }
    if (!ue_wrap::engine::IsActorRootSimulatingPhysics(g_targets[kHeater].Get())) {
        Finish("FAIL", "the heater does not simulate at rest");
        return;
    }
    TakeBaselines();
    std::string line = kTargetsHead;
    for (int i = 0; i < kTargets; ++i)
        line += std::string(i ? " " : "") + kKeys[i] + "=" + std::to_string(static_cast<unsigned>(g_eids[i]));
    Say(line);
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %d: %s", ArmName(kArm), kSlot, line.c_str());
    g_armAt = Clock::now();
    g_hstep = HStep::AwaitSent;
}

bool ClientSaidSent() {
    bool found = false;
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) {
        if (!found && r.slot == kSlot && r.text.compare(0, kSentLine.size(), kSentLine) == 0) found = true;
    });
    return found;
}

// The six answers were WrongFamily, each by the term the request earns, and nothing the client forged is gone.
void JudgeSix() {
    const uint32_t total = Since(PAH::AnsweredTotal(kSlot), g_totalBase);
    if (total != kSix) {
        Finish("FAIL", "the host answered " + std::to_string(total) + " requests, " + std::to_string(kSix) + " sent");
        return;
    }
    const uint32_t wrong = Since(PAH::AnsweredFor(kSlot, Reason::WrongFamily), g_answeredBase);
    if (wrong != kSix) {
        Finish("FAIL", "the host answered WrongFamily " + std::to_string(wrong) + " times, " + std::to_string(kSix) +
                           " expected");
        return;
    }
    for (size_t i = 0; i < kTermCount; ++i) {
        const uint32_t n = Since(PAH::TallyOf(kTerms[i]), g_termBase[i]);
        if (n != kTermWants[i]) {
            Finish("FAIL", std::string(PAH::TallyName(kTerms[i])) + " rose by " + std::to_string(n) + ", " +
                               std::to_string(kTermWants[i]) + " expected");
            return;
        }
    }
    for (int i = 0; i < kTargets; ++i) {
        if (EL::LivePropActor(g_eids[i]) != g_targets[i].Get()) {
            Finish("FAIL", std::string("the ") + kKeys[i] + " is gone after a refused act");
            return;
        }
    }
    Finish("PASS", "six forged acts answered WrongFamily by fit_no_use_row 1, fit_heavy 2, fit_denied 2 and "
                   "fit_bad_family 1, and all four targets stand");
}

}  // namespace

namespace detail {

void TickFamilyClient(coop::net::Session* s) {
    if (g_cstep == CStep::Done) return;
    if (g_cstep == CStep::Idle) {
        if (!OwnJoinOver() || !PollDue()) return;
        const uint8_t self = coop::players::Registry::Get().LocalPeerId();
        if (self == 0) return;
        if (self != kSlot) {
            UE_LOGI("[HOSTILE-DRILL] %s idle -- this arm runs on c%d only", ArmName(kArm), kSlot);
            g_cstep = CStep::Done;
            return;
        }
        if (g_idleAt == Clock::time_point{}) g_idleAt = Clock::now();
        std::string line;
        if (!ChatLine(kTargetsHead, &line)) {
            if (Clock::now() - g_idleAt > kReadyBound) ClientEnd("FAIL", "the host never said its targets");
            return;
        }
        const char* missing = "";
        if (!ParseTargets(line, g_ceids, &missing)) {
            ClientEnd("FAIL", std::string("the host's targets line names no ") + missing);
            return;
        }
        if (SendNext(s)) g_cstep = CStep::AwaitReply;
        return;
    }
    // AwaitReply: the forged ids match no record, so each reply is a stale one.
    const Req& r = kReqs[g_sent - 1];
    if (PAR::dev::LastStaleReply().count == g_staleBase) {
        if (Clock::now() - g_sentAt > kReplyBound)
            ClientEnd("FAIL", std::string("the host never answered the request ") + r.name);
        return;
    }
    if (g_sent < kSix) {
        SendNext(s);
        return;
    }
    Say(kSentLine);
    UE_LOGI("[HOSTILE-DRILL] %s sent all %u requests", ArmName(kArm), static_cast<unsigned>(kSix));
    g_cstep = CStep::Done;
}

void TickFamilyHost(coop::net::Session* s) {
    if (g_hstep == HStep::Done) return;
    if (g_hstep == HStep::WaitArm) {
        if (!s->IsSlotWorldReady(kSlot) || !coop::prop_snapshot::IsBracketClosed(kSlot) ||
            coop::join_window_baseline::IsLateWindowOpen(kSlot))
            return;
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kSlot);
        if (!rp.valid() || !rp.HasPose()) return;
        SpawnTargets(rp);
        return;
    }
    if (!s->HasPeerConn(kSlot)) {
        Finish("INCONCLUSIVE", "the judged slot left before the verdict");
        return;
    }
    // The per-slot counters reset when the slot's occupant changes, so a replaced slot is not judged on them.
    if (coop::roster_ledger::Get(kSlot).playerNo != g_playerNo) {
        Finish("INCONCLUSIVE", "slot-replaced (the judged slot's occupant changed)");
        return;
    }
    if (g_hstep == HStep::Announce) {
        AnnounceTargets();
        return;
    }
    if (!PollDue()) return;
    if (!ClientSaidSent()) {
        Bounded(g_armAt, kSentBound, "the client never said it sent its requests");
        return;
    }
    JudgeSix();
}

void ResetFamily() {
    g_cstep = CStep::Idle;
    for (EL::ElementId& e : g_ceids) e = EL::kInvalidId;
    g_sent = 0;
    g_staleBase = 0;
    g_sentAt = {};
    g_idleAt = {};
    g_hstep = HStep::WaitArm;
    g_playerNo = 0;
    g_armAt = {};
    for (ue_wrap::CachedObjRef& t : g_targets) t.Reset();
    for (EL::ElementId& e : g_eids) e = EL::kInvalidId;
    g_answeredBase = g_totalBase = 0;
    for (uint32_t& n : g_termBase) n = 0;
}

}  // namespace detail

}  // namespace coop::dev::hostile_drill::act
