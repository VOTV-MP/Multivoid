// coop/dev/pr64_drill_coin_credit.cpp -- see coop/dev/pr64_drill.h: the coin arm's instrument on the client. An observe-only
// name watch on lib_C::addPoints, the game's own credit: a call the client makes after its press, other than ours, is a
// local credit. The host's balance reaches a client as an absolute write that calls no addPoints, so a client that
// credits itself at the press (the behaviour the press verdict's Cancel removed) is the only way to count one. Game thread.

#include "pr64_drill_detail.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>

namespace coop::dev::pr64_drill::detail {
namespace {

namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowAddPoints, "dev.pr64_drill", L"lib_C", L"addPoints", Declared);

constexpr int kTagAddPoints = 0x50363441;   // 'P64A'

struct Watch {
    bool registered = false, dead = false, armed = false;
    int calls = 0;
};
Watch g_w;

sg::Verdict OnAddPointsPre(const sg::Call& c) {
    if (c.fromOurCode || !g_w.armed) return sg::Verdict::Run;
    ++g_w.calls;
    UE_LOGI("[PR64-DRILL] client lib_C::addPoints called after the press (call %d, not ours)", g_w.calls);
    return sg::Verdict::Run;
}

}  // namespace

bool CreditWatchStart() {
    if (g_w.registered) return !g_w.dead;
    g_w.registered = true;
    g_w.dead = !sg::WatchName(kRowAddPoints, kTagAddPoints, &OnAddPointsPre, nullptr);
    return !g_w.dead;
}

int CreditWatchState() {
    if (!g_w.registered || g_w.dead) return -1;
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(kRowAddPoints, kTagAddPoints)) return 1;
    if (sg::NameWatchSettled(kRowAddPoints, kTagAddPoints)) {
        g_w.dead = true;
        return -1;
    }
    return 0;
}

void CreditWatchArm() {
    g_w.calls = 0;
    g_w.armed = true;
}

int CreditWatchCalls() { return g_w.calls; }

void CreditWatchStop() {
    if (g_w.registered && !g_w.dead) sg::UnwatchName(kRowAddPoints, kTagAddPoints, &OnAddPointsPre, nullptr);
    g_w = Watch{};
}

}  // namespace coop::dev::pr64_drill::detail
