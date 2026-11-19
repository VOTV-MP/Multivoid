// coop/dev/oven_judge_drill.cpp -- see coop/dev/oven_judge_drill.h.

#include "coop/dev/oven_judge_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/oven_progress_drill.h"
#include "coop/interactables/interactable_sync.h"
#include "coop/interactables/oven_progress_sync.h"
#include "coop/interactables/oven_repair_judge.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/appliance.h"
#include "ue_wrap/devices/oven_screen.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::oven_judge_drill {
namespace {

namespace A  = ue_wrap::appliance;
namespace OS = ue_wrap::oven_screen;
namespace DC = coop::dev::drill_chat;
namespace DR = coop::director;
namespace J  = coop::oven_repair_judge;
namespace PS = coop::oven_progress_sync;
namespace IS = coop::interactable_sync;

enum class Arm { Off, Repair, Reach, Order, FixEarly };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::oven_progress_drill);
        return v == "repair" ? Arm::Repair : v == "reach" ? Arm::Reach : v == "order" ? Arm::Order
             : v == "fixearly" ? Arm::FixEarly : Arm::Off;
    }();
    return a;
}

bool Host() { return coop::roster::LocalIsHost(); }
char Who() { return Host() ? 'H' : 'C'; }
// Client 1 acts, the host reads, client 2 (the reach arm's sentinel reader) watches.
bool ActingClient() { return !Host() && coop::players::Registry::Get().LocalPeerId() == 1; }

// The walk's own bound and where it ends: within the interaction trace's length of the box, well inside the judge's reach.
constexpr int   kWalkDeadlineS = 300;
constexpr float kBoxStandCm    = 180.f;
// A wait ends on its event; this only says INVALID when the event never comes. It never decides a verdict.
constexpr uint64_t kInvalidBoundMs = 30000;

// The steps the acting client raises on its screen, one a tick, as the repair arm's real steps.
constexpr uint8_t kSteps[]   = {OS::kScrewTL, OS::kScrewTR | OS::kScrewBL | OS::kScrewBR, OS::kLid};
constexpr int     kStepCount = 3;
// What the reach arm's raw bit and the sentinel carry.
constexpr uint8_t kRawScrew  = OS::kScrewTL;
constexpr uint8_t kSentinel  = OS::kScrewBR;

const char* const kSayWalked    = "oven walked";
const char* const kSayWalkFail  = "oven walk failed";
const char* const kSayRawSent   = "oven raw sent";
const char* const kSayC2Ready   = "oven c2 ready";

ue_wrap::CachedObjRef g_oven;
std::shared_ptr<DR::BackgroundWalk> g_walk;
bool     g_done = false;
bool     g_started = false;     // the fixture check has passed
bool     g_walkSaid = false;
int      g_step = 0;            // the repair arm's next step
bool     g_fixCalled = false;
bool     g_rawSent = false;
bool     g_c2Said = false;
uint64_t g_waitMs = 0;
J::Stats g_base{};              // the judge's counters when the arm started

void* Oven() {
    if (void* o = g_oven.Get()) return o;
    std::vector<std::pair<std::wstring, void*>> ovens;
    IS::LiveOvens(ovens);
    if (ovens.empty()) return nullptr;
    g_oven.Set(ovens.front().second);
    return ovens.front().second;
}

void Verdict(bool pass, const char* fmt, ...) {
    char msg[360];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (pass) UE_LOGI("[OVEN-DRILL] [%c] PASS %s", Who(), msg);
    else      UE_LOGW("[OVEN-DRILL] [%c] FAIL %s", Who(), msg);
    g_done = true;
}

void Invalid(const char* fmt, ...) {
    char msg[360];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    UE_LOGW("[OVEN-DRILL] [%c] INVALID %s", Who(), msg);
    g_done = true;
}

void Wait(const char* what) {
    const uint64_t now = coop::net::NowMs();
    if (!g_waitMs) { g_waitMs = now; return; }
    if (now - g_waitMs > kInvalidBoundMs)
        Invalid("%s did not come in %llu ms", what, static_cast<unsigned long long>(kInvalidBoundMs));
}
void Progress() { g_waitMs = 0; }

// The fixture each arm starts from: an unrepaired oven with its screen, every screw in, and nothing in the host's record.
bool Fixture(void* oven, const std::wstring& key) {
    uint8_t live = 0;
    bool fixed = false;
    if (!OS::TryReadProgress(oven, live) || !A::TryReadOvenFixed(oven, fixed)) {
        Invalid("fixture: the oven has no repair screen here (repaired in this save, or not found)");
        return false;
    }
    if (fixed || live != 0 || PS::KnownMask(key) != 0) {
        Invalid("fixture: the oven must start unrepaired with every screw in; here fixed=%d screen 0x%02X record 0x%02X",
                fixed ? 1 : 0, live, PS::KnownMask(key));
        return false;
    }
    return true;
}

// A raw message of the client, straight onto the wire: what a client that skips its screen's own sends.
bool SendRawSteps(coop::net::Session& s, const std::wstring& key, uint8_t mask) {
    coop::net::OvenProgressStatePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.mask = mask;
    return s.SendReliable(coop::net::ReliableKind::OvenProgressState, &p, sizeof(p));
}

bool SendRawRepair(coop::net::Session& s, const std::wstring& key) {
    coop::net::KeyedTogglePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.action = 1;
    return s.SendReliable(coop::net::ReliableKind::OvenRepairState, &p, sizeof(p));
}

// ---- the acting client ----------------------------------------------------------------------
void TickActing(coop::net::Session& s, void* oven, const std::wstring& key) {
    const Arm arm = ArmOf();
    if (!g_started) {
        if (!Fixture(oven, key)) return;
        g_started = true;
        g_base = J::GetStats();
    }
    // The reach arm stays where the client spawned; the others walk to the box first.
    if (arm != Arm::Reach && !g_walkSaid) {
        switch (WalkToBox(oven)) {
        case Walk::Going: return;
        case Walk::Failed:
            DC::Say(kSayWalkFail);
            return Invalid("the director's walk to the oven's switch box failed");
        case Walk::Arrived:
            g_walkSaid = true;
            DC::Say(kSayWalked);
            UE_LOGI("[OVEN-DRILL] [C] walked to the oven's switch box");
            return;
        }
    }
    if (arm == Arm::Reach) {
        if (!DC::SeenHas(kSayC2Ready)) return;  // the sentinel's reader is in the world: run with --clients 2
        if (!g_rawSent) {
            g_rawSent = true;
            const bool ok = SendRawSteps(s, key, kRawScrew);
            DC::Say(kSayRawSent);
            UE_LOGI("[OVEN-DRILL] [C] sent a raw screw bit 0x%02X from where it stands, out of the box's reach (%d)", kRawScrew,
                    ok ? 1 : 0);
        }
        g_done = true;
        return;
    }
    if (arm == Arm::Order) {
        const bool ok = SendRawSteps(s, key, OS::kLid);
        DC::Say(kSayRawSent);
        UE_LOGI("[OVEN-DRILL] [C] sent a raw lid bit 0x%02X with no screw in, from the box (%d)", OS::kLid, ok ? 1 : 0);
        g_done = true;
        return;
    }
    if (arm == Arm::FixEarly) {
        const bool ok = SendRawRepair(s, key);
        DC::Say(kSayRawSent);
        UE_LOGI("[OVEN-DRILL] [C] sent a raw repair with no lid in the host's record, from the box (%d)", ok ? 1 : 0);
        g_done = true;
        return;
    }
    // repair: the screws and the lid on this screen as the game's own steps leave them, then the wiring's end, fix().
    if (g_step < kStepCount) {
        const bool ok = OS::RaiseProgress(oven, kSteps[g_step]);
        UE_LOGI("[OVEN-DRILL] [C] step %d: raised 0x%02X on this screen (%d)", g_step, kSteps[g_step], ok ? 1 : 0);
        ++g_step;
        return;
    }
    if (!g_fixCalled) {
        g_fixCalled = true;
        const bool ran = A::CallOvenFix(oven);
        const J::Stats st = J::GetStats();
        bool fixed = false;
        A::TryReadOvenFixed(oven, fixed);
        UE_LOGI("[OVEN-DRILL] [C] called the oven's fix(): dispatched=%d held=%llu ran here=%llu fixed here=%d", ran ? 1 : 0,
                static_cast<unsigned long long>(st.held - g_base.held),
                static_cast<unsigned long long>(st.ranHere - g_base.ranHere), fixed ? 1 : 0);
        if (st.ranHere != g_base.ranHere || fixed)
            return Verdict(false, "repair: this client's own fix() ran here before the host's grant (fixed here=%d)",
                           fixed ? 1 : 0);
        if (st.held == g_base.held)
            return Invalid("repair: the client's fix() was neither held nor run -- the repair gate did not see it");
        Progress();
        return;
    }
    bool fixed = false;
    if (A::TryReadOvenFixed(oven, fixed) && fixed) {
        const J::Stats st = J::GetStats();
        return Verdict(true, "repair: the oven reads fixed on this client from the host's grant; its own fix() was held "
                       "(%llu) and never ran here", static_cast<unsigned long long>(st.held - g_base.held));
    }
    Wait("the host's grant of the repair");
}

// ---- the host -------------------------------------------------------------------------------
void TickHost(void* oven, const std::wstring& key) {
    const Arm arm = ArmOf();
    if (!g_started) {
        if (!Fixture(oven, key)) return;
        g_started = true;
        g_base = J::GetStats();
    }
    if (DC::SeenHas(kSayWalkFail)) return Invalid("the client's walk to the oven's switch box failed");
    const J::Stats st = J::GetStats();
    bool fixed = false;
    A::TryReadOvenFixed(oven, fixed);
    const uint8_t record = PS::KnownMask(key);
    if (arm == Arm::Repair) {
        if (!DC::SeenHas(kSayWalked)) return;
        if (!fixed) return Wait("the repair's grant");
        if (st.grantedRepairs == g_base.grantedRepairs)
            return Verdict(false, "repair: the host's oven reads fixed with no grant by the judge");
        return Verdict(true, "repair: the judge granted %llu step message(s) and the repair; the record holds 0x%02X and the "
                       "host reads fixed", static_cast<unsigned long long>(st.grantedSteps - g_base.grantedSteps), record);
    }
    if (!DC::SeenHas(kSayRawSent)) return;
    const uint64_t dReach = st.refusedReach - g_base.refusedReach, dOrder = st.refusedOrder - g_base.refusedOrder;
    const uint64_t dOther = (st.refusedNooven - g_base.refusedNooven) + (st.refusedNobody - g_base.refusedNobody) +
                            (st.refusedBucket - g_base.refusedBucket) + (st.refusedAction - g_base.refusedAction);
    if (arm == Arm::Reach) {
        if (record & kRawScrew) return Verdict(false, "reach: the far client's raw screw bit reached the host's record 0x%02X", record);
        if (dReach) {
            // The sentinel rides the same ordered lane to client 2: a relayed raw bit would have reached it first.
            const bool ok = OS::RaiseProgress(oven, kSentinel);
            return Verdict(true, "reach: refused_reach moved by %llu and the record lacks the bit (0x%02X); the sentinel 0x%02X "
                           "is raised for client 2 (%d)", static_cast<unsigned long long>(dReach), record, kSentinel, ok ? 1 : 0);
        }
    } else if (arm == Arm::Order) {
        if (record & OS::kLid) return Verdict(false, "order: the lid with no screw in reached the host's record 0x%02X", record);
        if (dOrder) return Verdict(true, "order: refused_order moved by %llu and the record lacks the lid (0x%02X)",
                                   static_cast<unsigned long long>(dOrder), record);
        if (dReach) return Invalid("order: the raw lid was refused for reach, not order -- the client was not at the box");
    } else {
        if (fixed || st.grantedRepairs != g_base.grantedRepairs)
            return Verdict(false, "fixearly: a repair with no lid in the record fixed the host's oven");
        if (dOrder) return Verdict(true, "fixearly: refused_order moved by %llu and the host's oven is not fixed",
                                   static_cast<unsigned long long>(dOrder));
        if (dReach) return Invalid("fixearly: the raw repair was refused for reach, not order -- the client was not at the box");
    }
    if (dOther) return Invalid("the raw message was refused for another reason (nooven/nobody/bucket/action)");
    Wait("the host's refusal of the raw message");
}

// ---- client 2: the reach arm's sentinel reader ------------------------------------------------
void TickReader(void* oven, const std::wstring& key) {
    uint8_t live = 0;
    if (!g_started) {
        if (!Fixture(oven, key)) return;
        g_started = true;
    }
    if (!g_c2Said) {
        g_c2Said = true;
        DC::Say(kSayC2Ready);
        return;
    }
    if (!OS::TryReadProgress(oven, live)) return Invalid("this client's oven lost its repair screen");
    if (live & kRawScrew)
        return Verdict(false, "reach: the far client's raw screw bit 0x%02X reached this client (screen 0x%02X): the host "
                       "relayed or granted it", kRawScrew, live);
    if (!DC::SeenHas(kSayRawSent)) return;
    if (live & kSentinel)
        return Verdict(true, "reach: the host's sentinel 0x%02X reached this client (screen 0x%02X) and the far client's "
                       "bit did not", kSentinel, live);
    Wait("the host's sentinel");
}

}  // namespace

bool IsJudgeArm() { return ArmOf() != Arm::Off; }

Walk WalkToBox(void* oven) {
    if (!g_walk) {
        ue_wrap::FVector box{};
        if (!A::TryReadOvenSwitchLocation(oven, box)) return Walk::Failed;
        g_walk = DR::StartBackgroundWalk(box, kBoxStandCm, kWalkDeadlineS);
        UE_LOGI("[OVEN-DRILL] [C] the director walks this client to within %.0f cm of the oven's switch box at (%.0f, %.0f, %.0f)",
                kBoxStandCm, box.X, box.Y, box.Z);
        return Walk::Going;
    }
    const int state = g_walk->state.load();
    return state == 0 ? Walk::Going : state == 1 ? Walk::Arrived : Walk::Failed;
}

void Tick(coop::net::Session* session) {
    if (!IsJudgeArm() || !session || g_done || !session->connected()) return;
    DC::SeenHas(kSayWalked);  // the chat history records from its first read on
    if (!coop::dev::oven_progress_drill::PeersJoined(*session)) return;
    void* oven = Oven();
    const std::wstring key = oven ? IS::OvenKey(oven) : std::wstring();
    if (!oven || key.empty()) return Wait("the oven in the lane's index");
    Progress();
    if (Host()) return TickHost(oven, key);
    if (ActingClient()) return TickActing(*session, oven, key);
    if (ArmOf() == Arm::Reach) return TickReader(oven, key);
    g_done = true;  // a second client has no part in this arm
}

void OnDisconnect() {
    if (g_walk) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    g_oven.Reset();
    g_done = false;
    g_started = false;
    g_walkSaid = false;
    g_step = 0;
    g_fixCalled = false;
    g_rawSent = false;
    g_c2Said = false;
    g_waitMs = 0;
    g_base = J::Stats{};
}

}  // namespace coop::dev::oven_judge_drill
