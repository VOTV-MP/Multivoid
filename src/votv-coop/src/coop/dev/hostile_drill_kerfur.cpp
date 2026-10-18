// coop/dev/hostile_drill_kerfur.cpp -- see coop/dev/hostile_drill_kerfur.h.

#include "coop/dev/hostile_drill_kerfur.h"

#include "coop/creatures/kerfur_command.h"
#include "coop/creatures/kerfur_convert_host.h"
#include "coop/dispatch/event_feed.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::hostile_drill::kerfur {
namespace {

using Clock = std::chrono::steady_clock;
using Kind  = coop::net::ReliableKind;
namespace EL = coop::element;

// The flood: per kind, kPerKind messages in one tick. 121 is far over either cap and over what either pace pays in
// the span of a burst, so a lane that bounds nothing runs them all.
constexpr uint32_t kPerKind = 121;

// The judge's numbers, literals written from the lanes' budgets (no symbol of the change itself): the pace's burst
// and per-second refill, and the cap of messages a lane queues per sender.
struct KindSpec {
    const char* name;
    uint32_t    cap;
    float       burst;
    float       perSecond;
};
constexpr KindSpec kSpecs[2] = {{"kerfur_command", 8, 4.f, 2.f}, {"kerfur_convert", 4, 2.f, 1.f}};
constexpr auto     kFailBound = std::chrono::seconds(180);  // a failure bound, never a wait

// A command value the lane relays: follow, idle, patrol, fix_servers, get_reports, fix_transformers.
constexpr uint32_t kCommandValues = 6;

// ---- client (sender) ----
bool g_sent = false;

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// A mirror of either form on this client names a live element; a flood id must not.
bool HeldHere(EL::ElementId id) {
    return EL::MirrorManager<EL::Npc>::Instance().Get(id) != nullptr ||
           EL::MirrorManager<EL::Prop>::Instance().Get(id) != nullptr;
}

// ---- host (judge) ----
enum class Step { WaitArm, WaitAccounted, Done };
Step g_step = Step::WaitArm;
int  g_slot = -1;
uint16_t g_playerNo = 0;  // the judged slot's occupant at arm: the counters reset when it changes
Clock::time_point g_armAt{};

struct KindState {
    uint32_t          runsBase = 0;
    uint32_t          refusedBase = 0;
    uint32_t          drainedBase = 0;
    uint32_t          runs = 0;
    uint32_t          refused = 0;
    Clock::time_point t0{};     // the first read that saw a run
    Clock::time_point tLast{};  // the last read at which the runs grew
    std::string       paceFail;  // the first read the pace was exceeded, said
    std::string       capFail;   // the first read the queue depth was over the cap, said
};
KindState g_kind[2];

uint32_t RunsNow(int i) {
    return i == 0 ? coop::kerfur_command::RunsFor(g_slot) : coop::kerfur_convert_host::RunsFor(g_slot);
}

uint32_t RefusedNow(int i) {
    return i == 0 ? coop::kerfur_command::RefusedFor(g_slot) : coop::kerfur_convert_host::RefusedFor(g_slot);
}

// The messages the drain handed to a handler since arm. event_feed counts a slot's message as it pulls it, before
// the router; every message of this flood is well-formed, from a known slot, on the host, so each one reaches
// OnCommandRequest / OnConvertRequest and this is exactly what the lane was handed.
uint32_t DrainedNow(int i) {
    return coop::event_feed::DrainedFrom(g_slot, i == 0 ? Kind::KerfurCommand : Kind::KerfurConvertRequest);
}

double SecondsBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

void Sample(int i, Clock::time_point now) {
    KindState& k = g_kind[i];
    const uint32_t prev = k.runs;
    k.runs = RunsNow(i) - k.runsBase;
    k.refused = RefusedNow(i) - k.refusedBase;
    // What the lane holds unserved and unrefused, at every read: handed over, neither run nor refused.
    const int64_t depth = static_cast<int64_t>(DrainedNow(i) - k.drainedBase) - k.runs - k.refused;
    if (depth > static_cast<int64_t>(kSpecs[i].cap) && k.capFail.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "cap (depth %lld > cap %u)", static_cast<long long>(depth), kSpecs[i].cap);
        k.capFail = buf;
    }
    if (k.runs > prev) {
        if (prev == 0) k.t0 = now;
        k.tLast = now;
    }
    if (k.runs == 0 || !k.paceFail.empty()) return;
    // From the first run at t0, at every read: the burst, what the refill paid since, and one for a refill inside
    // the frame the read did not see.
    const double sinceFirst = SecondsBetween(k.t0, now);
    const double most = kSpecs[i].burst + kSpecs[i].perSecond * sinceFirst + 1.0;
    if (k.runs > most) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "pace (%u runs %.0f ms after the first, at most %.1f)", k.runs,
                      sinceFirst * 1000.0, most);
        k.paceFail = buf;
    }
}

std::string Summary() {
    std::string out;
    for (int i = 0; i < 2; ++i) {
        const KindState& k = g_kind[i];
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s%s runs %u refused %u span %.0f ms", i ? "; " : "", kSpecs[i].name,
                      k.runs, k.refused, k.runs ? SecondsBetween(k.t0, k.tLast) * 1000.0 : 0.0);
        out += buf;
    }
    return out;
}

void Finish(Arm a, const char* what, const char* kind, const std::string& why) {
    if (kind) {
        UE_LOGW("[HOSTILE-DRILL] %s %s %s %s -- %s", ArmName(a), what, kind, why.c_str(), Summary().c_str());
    } else {
        UE_LOGI("[HOSTILE-DRILL] %s %s -- %s", ArmName(a), what, Summary().c_str());
    }
    g_step = Step::Done;
}

void ArmHost(coop::net::Session* s, Arm a) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && g_slot < 0; ++slot)
        if (s->IsSlotWorldReady(slot)) g_slot = slot;
    if (g_slot < 0) return;
    for (int i = 0; i < 2; ++i) {
        g_kind[i] = KindState{};
        g_kind[i].runsBase = RunsNow(i);
        g_kind[i].refusedBase = RefusedNow(i);
        g_kind[i].drainedBase = DrainedNow(i);
    }
    g_playerNo = coop::roster_ledger::Get(g_slot).playerNo;
    g_armAt = Clock::now();
    g_step = Step::WaitAccounted;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %d (runs %u/%u, refused %u/%u)", ArmName(a), g_slot,
            g_kind[0].runsBase, g_kind[1].runsBase, g_kind[0].refusedBase, g_kind[1].refusedBase);
}

// One kind's verdict, once all of it is accounted for: empty is a pass, else the marker word and why.
struct KindVerdict {
    const char* what = nullptr;  // "FAIL" / "INCONCLUSIVE"; null passes
    std::string why;
};

KindVerdict Judge(int i) {
    const KindState& k = g_kind[i];
    const KindSpec& sp = kSpecs[i];
    KindVerdict v;
    char buf[200];
    if (k.runs == 0) {
        // All 121 were refused before any could run: the lane never served, which says nothing of its cap.
        v.what = "INCONCLUSIVE";
        v.why = "never-ran (refused before it could run)";
    } else if (k.runs < sp.cap) {
        // All 121 are accounted for and fewer than the cap ran: the lane refused while it had room.
        std::snprintf(buf, sizeof(buf), "under-cap (%u < %u)", k.runs, sp.cap);
        v.what = "FAIL";
        v.why = buf;
    } else if (!k.paceFail.empty()) {
        v.what = "FAIL";
        v.why = k.paceFail;
    }
    return v;
}

void WaitAccounted(Arm a) {
    const Clock::time_point now = Clock::now();
    bool all = true;
    for (int i = 0; i < 2; ++i) {
        Sample(i, now);
        if (!g_kind[i].capFail.empty()) { Finish(a, "FAIL", kSpecs[i].name, g_kind[i].capFail); return; }
        all = all && g_kind[i].runs + g_kind[i].refused == kPerKind;
    }
    if (!all) {
        if (now - g_armAt <= kFailBound) return;
        for (int i = 0; i < 2; ++i) {
            if (g_kind[i].runs + g_kind[i].refused == kPerKind) continue;
            if (g_kind[i].runs == 0) {
                Finish(a, "INCONCLUSIVE", kSpecs[i].name, "never-ran (the lane never became ready)");
                return;
            }
            Finish(a, "FAIL", kSpecs[i].name,
                   "timeout (" + std::to_string(g_kind[i].runs + g_kind[i].refused) + " of " +
                       std::to_string(kPerKind) + " accounted for)");
            return;
        }
        return;
    }
    KindVerdict v[2] = {Judge(0), Judge(1)};
    for (int i = 0; i < 2; ++i)
        if (v[i].what && std::string(v[i].what) == "FAIL") { Finish(a, "FAIL", kSpecs[i].name, v[i].why); return; }
    for (int i = 0; i < 2; ++i)
        if (v[i].what) { Finish(a, v[i].what, kSpecs[i].name, v[i].why); return; }
    Finish(a, "PASS", nullptr, "");
}

}  // namespace

const char* ArmName(Arm a) { return a == Arm::KerfurFlood ? "kerfur_flood" : "off"; }

void TickClient(coop::net::Session* s, Arm a) {
    if (g_sent || !OwnJoinOver()) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (self == 0 || self >= coop::players::kMaxPeers) return;  // no slot yet; wait
    g_sent = true;
    // 2 * kPerKind distinct ids from the top of the host's range, none held on this client.
    EL::ElementId ids[2 * kPerKind];
    uint32_t have = 0;
    for (EL::ElementId id = EL::kHostRangeSize - 1; have < 2 * kPerKind && EL::Registry::IsAllowedHostAllocatedEid(id);
         --id)
        if (!HeldHere(id)) ids[have++] = id;
    if (have < 2 * kPerKind) {
        UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: only %u unheld host-range ids found", ArmName(a), have);
        return;
    }
    bool ok = true;
    for (uint32_t n = 0; n < kPerKind && ok; ++n) {
        coop::net::KerfurCommandPayload c{};
        c.elementId = static_cast<uint32_t>(ids[n]);
        c.command = static_cast<uint8_t>(n % kCommandValues);
        ok = s->SendReliableToSlot(0, coop::net::ReliableKind::KerfurCommand, &c, sizeof(c));
        if (!ok) break;
        coop::net::KerfurConvertPayload r{};
        r.elementId = static_cast<uint32_t>(ids[kPerKind + n]);
        r.toProp = static_cast<uint8_t>(n % 2);
        ok = s->SendReliableToSlot(0, coop::net::ReliableKind::KerfurConvertRequest, &r, sizeof(r));
    }
    if (!ok) {
        UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: a message of the flood was not sent", ArmName(a));
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] %s sent %u KerfurCommand and %u KerfurConvertRequest for slot %u (ids %u down to %u)",
            ArmName(a), kPerKind, kPerKind, static_cast<unsigned>(self), static_cast<unsigned>(ids[0]),
            static_cast<unsigned>(ids[2 * kPerKind - 1]));
}

void TickHostJudge(coop::net::Session* s, Arm a) {
    if (g_step == Step::WaitArm) { ArmHost(s, a); return; }
    if (g_step == Step::Done) return;
    if (!s->HasPeerConn(g_slot)) {
        Finish(a, "INCONCLUSIVE", "judge", "the judged slot left before the verdict");
        return;
    }
    // The per-slot counters reset when the slot's occupant changes, so a replaced slot is not judged on them.
    if (coop::roster_ledger::Get(g_slot).playerNo != g_playerNo) {
        const char* kind = "judge";  // the first kind not fully accounted for, else the judge
        for (int i = 1; i >= 0; --i)
            if (g_kind[i].runs + g_kind[i].refused != kPerKind) kind = kSpecs[i].name;
        Finish(a, "INCONCLUSIVE", kind, "slot-replaced (the judged slot's occupant changed)");
        return;
    }
    WaitAccounted(a);
}

void OnDisconnect() {
    g_sent = false;
    g_step = Step::WaitArm;
    g_slot = -1;
    g_playerNo = 0;
    g_armAt = {};
    for (KindState& k : g_kind) k = KindState{};
}

}  // namespace coop::dev::hostile_drill::kerfur
