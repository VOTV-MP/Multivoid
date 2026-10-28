// coop/dev/container_intent_drill_put_join.cpp -- the HOST's half of putjoin, the container intent drill's two-client put
// arm (run with --observer); see container_intent_drill_put_host.cpp for the fixture it shares with the other put arms and
// container_intent_drill_put2.cpp for the clients. A join takes longer than a reservation lasts, so the arm measures two
// things apart:
//   A. CONVERGENCE. A second slot is connected but not world-ready -> the host says go; client 1's put commits at once. The
//      host reads whether the target's publication was refused by the no-ready guard, and the joiner, once world-ready, is
//      told its fixture and says what its seeded mirror of the target holds. Its seed is the contents before the put or after
//      it; both are green, the line says which path ran. Each client then holds its mirror to the put (+1).
//   B. THE MID-JOIN REFUSAL. The host says second; client 1 asks a second put with its replay parked and the reservation
//      stands; the joiner asks a put on the same target, which waits at the head and is counted; 5 s after it waits the host
//      says release; client 1 commits, then the joiner is granted and commits.
// The host judges the order (client 1's second record is in before the joiner's grant), the judgements (at most w/250 + 1
// over w ms), the seed, the mirrors and its own count (+3), and the strays. Every wait is bounded, and the 30 s reservation
// deadline bounds part B: a join slower than it FAILs with the phase it was in.
// Lines are tagged [CI-DRILL] and end in `host putjoin PASS` or the drill's `host putjoin FAIL: <why>`.

#include "container_intent_drill_put.h"
#include "container_route_drill_detail.h"

#include "coop/dev/drill_chat.h"
#include "coop/net/session.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_contents_sync.h"

#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace AH = coop::props::container_act_host;
namespace AS = coop::props::container_act_state;
namespace CC = coop::props::container_contents_sync;
namespace DC = coop::dev::drill_chat;
namespace RD = coop::dev::container_route_drill::detail;

constexpr uint64_t kBoundMs        = 300000;  // the first client's walk, and the second client's connecting
constexpr uint64_t kAskMs          = 20000;   // the go to client 1's first commit; the second line to client 1's second grant
constexpr uint64_t kJoinReadyMs    = 150000;  // the first commit to the second client being world-ready
constexpr uint64_t kSafeMs         = 26000;   // the second grant to the joiner's put waiting: the reservation lapses at 30 s
constexpr uint64_t kHoldMs         = 5000;    // the joiner's head waits this long before the release
constexpr uint64_t kSecondGrantMs  = 15000;   // the release to the joiner's grant
constexpr uint64_t kDoneMs         = 60000;   // the joiner's grant to both clients being done
constexpr uint64_t kCountMs        = 5000;    // the dones to the count reading +3
constexpr uint64_t kTellMs         = 3000;    // NOT MEASURED said on chat to the end, so the client prints its own

enum class J : uint8_t { Joining, Commit1, Joiner, Grant2, JoinerAsk, Hold, SecondGrant, Dones, Judge, NotMeasured };
J        g_j = J::Joining;
uint64_t g_enterMs = 0, g_goMs = 0, g_commitMs = 0, g_secondMs = 0, g_grantMs = 0, g_waitMs = 0, g_relMs = 0, g_doneMs = 0;
int      g_slot1 = -1, g_slot2 = -1;
int      g_seed = -1;
bool     g_refused = false;   // the target's publication met the no-ready guard before the joiner was world-ready
int      g_ok[2] = {-1, -1}, g_stray[2] = {-1, -1}, g_mirror[2] = {-1, -1};   // client 1, the joiner

uint32_t TargetEid() { return RD::EidOf(PutHostViewOf().target); }

void Verdicts(uint32_t& grants, uint32_t& gones) { AH::dev::Verdicts(TargetEid(), grants, gones); }

// The seed the joiner said, read once and judged: the contents before client 1's put or after it. False after the FAIL.
bool ReadSeed(const PutHostView& v) {
    std::string rest;
    if (g_seed >= 0 || !DC::LiveRestOf(kPutSeedLine, &rest)) return true;
    if (!RD::IntAfter(rest, "n", g_seed)) g_seed = -1;
    const bool post = g_seed == v.count0 + 1;
    UE_LOGI("[CI-DRILL] host putjoin seed %s (joiner n %d, host before %d, after %d) refused-then-delivered=%d",
            post ? "post-put" : "pre-put", g_seed, v.count0, v.count0 + 1, g_refused && post ? 1 : 0);
    if (g_seed != v.count0 && !post) {
        Fail("the joiner's seed showed %d record(s), neither the %d before client 1's put nor the %d after it", g_seed,
             v.count0, v.count0 + 1);
        return false;
    }
    return true;
}

bool ReadDone(const char* prefix, int idx) {
    std::string rest;
    if (g_ok[idx] >= 0 || !DC::LiveRestOf(prefix, &rest) || !RD::IntAfter(rest, "ok", g_ok[idx])) return g_ok[idx] >= 0;
    if (!RD::IntAfter(rest, "stray", g_stray[idx])) g_stray[idx] = -1;
    if (!RD::IntAfter(rest, "mirror", g_mirror[idx])) g_mirror[idx] = -1;
    UE_LOGI("[CI-DRILL] host putjoin: %s is done (%s)", idx ? "the joiner" : "client 1", rest.c_str());
    return true;
}

// The slot, other than client 1's, that is connected and not world-ready; -1 when none. -2 when another slot is already
// world-ready, so the join this arm needs has passed.
int JoiningSlot(const coop::net::Session& s) {
    int joining = -1;
    for (int i = 1; i < coop::net::kMaxPeers; ++i) {
        if (i == g_slot1) continue;
        if (s.IsSlotWorldReady(i)) return -2;
        if (s.IsSlotReady(i)) joining = i;
    }
    return joining;
}


// The arm cannot run here (no second client, or it joined before the first asked): said to the client and the log, never a
// FAIL. The end waits kTellMs so the client prints its own line first.
void NotMeasured(const char* why) {
    UE_LOGI("[CI-DRILL] host putjoin NOT MEASURED (%s)", why);
    DC::Say(kPutNotMeasuredLine);
    g_doneMs = ::GetTickCount64();
    g_j = J::NotMeasured;
}
}  // namespace

void PutJoinNoteSlot1(int slot) { g_slot1 = slot; }

void PutJoinReset() {
    g_j = J::Joining;
    g_enterMs = g_goMs = g_commitMs = g_secondMs = g_grantMs = g_waitMs = g_relMs = g_doneMs = 0;
    g_slot1 = g_slot2 = -1;
    g_seed = -1;
    g_refused = false;
    g_ok[0] = g_ok[1] = g_stray[0] = g_stray[1] = g_mirror[0] = g_mirror[1] = -1;
}

void PutJoinHost(const coop::net::Session& s) {
    const uint64_t now = ::GetTickCount64();
    if (!g_enterMs) g_enterMs = now;
    uint32_t grants = 0, gones = 0;
    const PutHostView v = PutHostViewOf();
    if (g_grantMs && g_j < J::Dones && AH::TallyOf(AH::Tally::ReservationLapsed)) {
        Fail("the 30 s reservation lapsed before part B finished (phase %d, %llu ms after the second grant)", static_cast<int>(g_j),
             static_cast<unsigned long long>(now - g_grantMs));
        return;
    }
    switch (g_j) {
    case J::Joining: {
        const int slot = JoiningSlot(s);
        if (slot == -2) {
            NotMeasured("the second client was already world-ready when the first was to ask: its join cannot overlap the first put");
            return;
        }
        if (slot < 0) {
            if (now - g_enterMs > kBoundMs) NotMeasured("no second client connected within its bound");
            return;
        }
        g_slot2 = slot;
        g_goMs = now;
        DC::Say(kPutGoLine);
        UE_LOGI("[CI-DRILL] host putjoin: slot %d is connected and not world-ready; client 1 (slot %d) is told to put, the target holds %d",
                slot, g_slot1, v.count0);
        g_j = J::Commit1;
        return;
    }
    case J::Commit1: {
        Verdicts(grants, gones);
        if (gones) {
            Fail("client 1's first put was answered Gone (%u)", gones);
            return;
        }
        const int count = CountOf(TargetEid());
        if (!grants || count != v.count0 + 1) {
            if (now - g_goMs > kAskMs)
                Fail("client 1's first put did not commit within %llu s of the go (grants %u, target %d, expected %d)",
                     static_cast<unsigned long long>(kAskMs / 1000), grants, count, v.count0 + 1);
            return;
        }
        g_commitMs = now;
        UE_LOGI("[CI-DRILL] host putjoin: client 1's first put is in %llu ms after the go (target %d -> %d); slot %d world-ready=%d",
                static_cast<unsigned long long>(now - g_goMs), v.count0, count, g_slot2, s.IsSlotWorldReady(g_slot2) ? 1 : 0);
        g_j = J::Joiner;
        return;
    }
    case J::Joiner: {
        if (!s.IsSlotWorldReady(g_slot2)) {
            if (now - g_commitMs > kJoinReadyMs)
                Fail("the second client was not world-ready %llu s after the first put", static_cast<unsigned long long>(kJoinReadyMs / 1000));
            return;
        }
        g_refused = CC::NoReadyRefusedEid(TargetEid());
        UE_LOGI("[CI-DRILL] host putjoin: slot %d is world-ready %llu ms after the first put; the target's publication refused by the "
                "no-ready guard=%d (refusals this session %llu); its fixture is told, client 1 is told second", g_slot2,
                static_cast<unsigned long long>(now - g_commitMs), g_refused ? 1 : 0,
                static_cast<unsigned long long>(CC::NoReadyRefusals()));
        char say[112];
        std::snprintf(say, sizeof(say), "%spack=%u item=%u item2=0 count=%d", kPutJoinLine, TargetEid(),
                      RD::EidOf(v.item3), v.count0);
        DC::Say(say);
        DC::Say(kPutSecondLine);
        AH::dev::CountPutJudgements(true);
        g_secondMs = now;
        g_j = J::Grant2;
        return;
    }
    case J::Grant2:
        if (!ReadSeed(v)) return;
        Verdicts(grants, gones);
        if (grants < 2) {
            if (now - g_secondMs > kAskMs)
                Fail("client 1's second put was not granted within %llu s of the second line", static_cast<unsigned long long>(kAskMs / 1000));
            return;
        }
        g_grantMs = now;
        UE_LOGI("[CI-DRILL] host putjoin: client 1's reservation stands %llu ms after the second line (holds=%zu, a hold on the target=%d)",
                static_cast<unsigned long long>(now - g_secondMs), AS::Live().Count(), AS::Live().HoldOn(TargetEid()) ? 1 : 0);
        g_j = J::JoinerAsk;
        return;
    case J::JoinerAsk: {
        if (!ReadSeed(v)) return;
        const AH::dev::PutJudged j = AH::dev::LastPutJudged(static_cast<uint8_t>(g_slot2));
        if (!j.n) {
            if (now - g_grantMs > kSafeMs) Fail("the joiner's put did not reach the host %llu ms after the second grant", static_cast<unsigned long long>(now - g_grantMs));
            return;
        }
        g_waitMs = now;
        UE_LOGI("[CI-DRILL] host putjoin: the joiner's put waits at the head, %llu ms after the second grant (judged %u)",
                static_cast<unsigned long long>(now - g_grantMs), j.n);
        g_j = J::Hold;
        return;
    }
    case J::Hold:
        if (!ReadSeed(v)) return;
        if (now - g_waitMs < kHoldMs) return;
        DC::Say(kPutReleaseLine);
        g_relMs = now;
        UE_LOGI("[CI-DRILL] host putjoin: client 1 is told to release, %llu ms after the second grant", static_cast<unsigned long long>(now - g_grantMs));
        g_j = J::SecondGrant;
        return;
    case J::SecondGrant: {
        Verdicts(grants, gones);
        if (gones) {
            Fail("a put was answered Gone (%u)", gones);
            return;
        }
        if (grants < 3) {
            if (now - g_relMs > kSecondGrantMs) Fail("the joiner's put was not granted %llu ms after the release", static_cast<unsigned long long>(now - g_relMs));
            return;
        }
        const int count = CountOf(TargetEid());
        const AH::dev::PutJudged j = AH::dev::LastPutJudged(static_cast<uint8_t>(g_slot2));
        const uint64_t w = j.lastMs - j.firstMs;
        UE_LOGI("[CI-DRILL] host putjoin: judged %u over %llu ms (limit %llu); client 1's second record was in before the joiner's grant: "
                "target %d -> %d", j.n, static_cast<unsigned long long>(w), static_cast<unsigned long long>(w / 250 + 1), v.count0 + 1, count);
        if (count != v.count0 + 2) {
            Fail("the joiner was granted with the target at %d, expected %d (client 1's second put in first)", count, v.count0 + 2);
            return;
        }
        if (j.n > w / 250 + 1) {
            Fail("the host judged the waiting head %u times over %llu ms, at most %llu", j.n, static_cast<unsigned long long>(w),
                 static_cast<unsigned long long>(w / 250 + 1));
            return;
        }
        g_doneMs = now;
        g_j = J::Dones;
        return;
    }
    case J::Dones: {
        if (!ReadSeed(v)) return;
        const bool a = ReadDone(kPutDoneLine, 0), b = ReadDone(kPutJoinDoneLine, 1);
        if (!(a && b)) {
            if (now - g_doneMs > kDoneMs) Fail("the clients were not both done %llu s after the joiner's grant (client 1 %d, joiner %d)", static_cast<unsigned long long>(kDoneMs / 1000), g_ok[0], g_ok[1]);
            return;
        }
        g_doneMs = now;
        g_j = J::Judge;
        return;
    }
    case J::NotMeasured:
        if (now - g_doneMs >= kTellMs) Finish();
        return;
    case J::Judge: {
        const int count = CountOf(TargetEid());
        const int want = v.count0 + PutHostDelta(Arm::PutJoin);
        if (count != want && now - g_doneMs < kCountMs) return;
        const int stray = PutStrayCount(PutItemClass(Arm::PutJoin), v.target);
        Verdicts(grants, gones);
        UE_LOGI("[CI-DRILL] host putjoin: target %d -> %d record(s); mirrors client 1 %d, joiner %d; seed %d; stray host=%d "
                "client 1=%d joiner=%d; grants=%u", v.count0, count, g_mirror[0], g_mirror[1], g_seed, stray, g_stray[0], g_stray[1], grants);
        if (g_ok[0] != 1 || g_ok[1] != 1) {
            Fail("a client's arm failed (client 1 ok=%d, joiner ok=%d; its FAIL line says why)", g_ok[0], g_ok[1]);
            return;
        }
        if (g_seed < 0) {
            Fail("the joiner never said what its seeded mirror held");
            return;
        }
        if (count != want || g_mirror[0] != want || g_mirror[1] != want) {
            Fail("the target went %d -> %d, expected %d; the mirrors hold %d and %d", v.count0, count, want, g_mirror[0], g_mirror[1]);
            return;
        }
        if (stray > 0 || g_stray[0] > 0 || g_stray[1] > 0) {
            Fail("stray drive props near the target: host=%d client 1=%d joiner=%d", stray, g_stray[0], g_stray[1]);
            return;
        }
        UE_LOGI("[CI-DRILL] host %s PASS", ArmName());
        Finish();
        return;
    }
    }
}

}  // namespace coop::dev::container_intent_drill::detail
