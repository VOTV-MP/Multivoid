// coop/dev/flood_drill_host.cpp -- the host's steps of the flood drill: it bursts the first pipe on the pipe's own addDamage,
// says what its pipes and its level read after each event the lane announces (a send, a judge), and decides the arms that
// the judge decides; see coop/dev/flood_drill.h. The log, chat and verdict are flood_drill.cpp.

#include "flood_drill_detail.h"

#include "coop/net/session.h"
#include "coop/save/save_transfer.h"
#include "coop/world/flood_sync.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"

#include <string>

namespace coop::dev::flood_drill::detail {

namespace R  = ue_wrap::reflection;
namespace FS = coop::flood_sync::dev;

namespace {

enum class HS {
    Start,
    WaitState1Ok, WaitRise, WaitState2Ok, WaitGone,   // burst, and the join arm's first life
    WaitJudge, WaitFall, WaitFellOk,                  // mend and far
    WaitJoinOk,                                       // the join arm's second life
    WaitOldOk                                         // mendold
};
HS g_h = HS::Start;
bool g_burstDone = false;   // the join arm: the first life's burst and rise were read by the client (outlives a disconnect)
bool g_windowBurst = false; // the join arm's second connection: the second pipe was burst in the rejoined client's window
Reading g_before;           // the pipes and the level before the burst
std::string g_burstBits;    // the pipes as the burst left them
float g_mendLevel = 0.f;
FS::Counts g_c0, g_c1;

void HGo(HS s) {
    g_h = s;
    Go();
}

// The pipe's own addDamage, as the drain's hourly roll calls it: its body runs State(true).
bool Burst(void* pipe) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(pipe), L"addDamage"));
    return f.valid() && f.Set<float>(L"damage", 1.f) && ue_wrap::Call(pipe, f);
}

bool HasBurst(const std::string& bits) { return bits.find('1') != std::string::npos; }

void StartBurst() {
    void* const pipe = FS::PipeByRank(0);
    if (!pipe) return Invalid("the world holds no burst pipe");
    if (!Read(g_before)) return;
    if (HasBurst(g_before.bits)) return Invalid("a pipe is burst before the drill's burst");
    g_c0 = FS::Read();
    if (!Burst(pipe)) return Invalid("the pipe's addDamage did not run");
    Reading now;
    if (!Read(now)) return;
    if (now.bits == g_before.bits) return Invalid("addDamage did not burst the pipe");
    g_burstBits = now.bits;
    Fact("burst: pipes %s -> %s, level %.1f; FloodState to all sent %u time(s) at the burst", g_before.bits.c_str(),
         now.bits.c_str(), now.level, FS::Read().pipesSent - g_c0.pipesSent);
    Say(Line("h-state1", now));
    HGo(HS::WaitState1Ok);
}

// The rejoined client's window: its world is captured and not ready. A change made now reaches it by the ready edge's
// replay alone (its world was taken before the change, and a FloodState is not sent to a slot that is not ready), so the
// second pipe is burst here, and the level has moved since the capture as the flood rises.
void BurstInWindow() {
    void* const pipe = FS::PipeByRank(1);
    Reading before;
    if (!pipe || !Read(before) || before.bits.size() < 2) return Invalid("the world holds no second burst pipe");
    if (before.bits[1] == '1') return Invalid("the second pipe is burst before the rejoined client's window");
    if (!Burst(pipe)) return Invalid("the second pipe's addDamage did not run");
    g_windowBurst = true;
    Reading now;
    if (!Read(now)) return;
    Fact("the rejoined client's window: the second pipe burst, pipes %s -> %s, level %.1f", before.bits.c_str(),
         now.bits.c_str(), now.level);
}

void StartJoinCheck() {
    Reading now;
    if (!Read(now)) return;
    if (!HasBurst(now.bits)) return Invalid("the flood did not outlive the client's leaving: no pipe is burst");
    if (now.level < FS::LevelEpsilon()) return Invalid("the flood had not risen when the client rejoined");
    Fact("the rejoined client's turn: pipes %s, level %.1f", now.bits.c_str(), now.level);
    Say(Line("h-joinstate", now));
    HGo(HS::WaitJoinOk);
}

// The host's own repair of a pipe whose last send it no longer knows (a pipe burst before the session's first send): the
// pipe's own toolboxFix, which needs no toolbox to run State(false). The client reads the host's pipes at the chat line
// that follows; the send, if the lane makes one, goes before it on the one ordered channel.
void MendOnHost() {
    void* const pipe = FS::PipeByRank(0);
    if (!pipe) return Invalid("the world holds no burst pipe");
    FS::ForgetSent();
    const uint32_t sent = FS::Read().pipesSent;
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(pipe), L"toolboxFix"));
    if (!f.valid() || !ue_wrap::Call(pipe, f) || !f.Get<bool>(L"return")) return Invalid("the host's own toolboxFix did not mend");
    Reading now;
    if (!Read(now)) return;
    if (now.bits != g_before.bits) return Invalid("the host's pipes do not read whole after its own repair");
    Fact("the host mended its own pipe with no record of its last send: pipes %s, FloodState to all sent %u time(s)",
         now.bits.c_str(), FS::Read().pipesSent - sent);
    Say(Line("h-old", now));
    HGo(HS::WaitOldOk);
}

// The client has read the burst: the arm goes on by its own event. The counts the arm compares against stand from the
// burst (StartBurst), not from this line: a client acts the moment it has said it, and its ask can be judged before the
// chat line that confirms the burst is read here.
void AfterState1() {
    if (ArmOf() == Arm::MendOld) {
        MendOnHost();
        return;
    }
    if (ArmOf() == Arm::Mend || ArmOf() == Arm::Far) {
        HGo(HS::WaitJudge);
        return;
    }
    HGo(HS::WaitRise);
}

void JudgedMend(const FS::Counts& c) {
    if (c.mended == g_c0.mended) {
        Fail("the host refused the client's repair: %s", c.lastWhy);
        return;
    }
    Reading now;
    if (!Read(now)) return;
    if (now.bits != g_before.bits) {
        Fail("the host's pipes read %s after the client's repair, %s before the burst", now.bits.c_str(), g_before.bits.c_str());
        return;
    }
    if (now.level < 2.f * FS::LevelEpsilon()) return Invalid("the flood had not risen when the pipe was mended");
    g_mendLevel = now.level;
    g_c1 = c;
    Fact("the client's repair mended the host's pipe: pipes %s, level %.1f", now.bits.c_str(), now.level);
    Say(Line("h-mended", now));
    HGo(HS::WaitFall);
}

void JudgedFar(const FS::Counts& c) {
    if (c.mended != g_c0.mended) {
        Fail("the host applied a repair asked from out of reach: its pipe is mended");
        return;
    }
    if (c.refusedTarget == g_c0.refusedTarget) {
        Invalid((std::string("the host refused the ask for '") + c.lastWhy + "', not at the target check").c_str());
        return;
    }
    Reading now;
    if (!Read(now)) return;
    if (now.bits != g_burstBits) {
        Fail("the host's pipes read %s after the refused ask, %s after the burst", now.bits.c_str(), g_burstBits.c_str());
        return;
    }
    Fact("the host refused the ask at the target check (%s) and its pipes still read %s", c.lastWhy, now.bits.c_str());
    Pass("a repair asked from out of reach was refused at the host's target check and its pipe stayed burst");
}

void HostStep(coop::net::Session& s) {
    if (g_h != HS::Start && PeerFailed()) return;
    switch (g_h) {
    case HS::Start:
        if (ArmOf() == Arm::Join && g_burstDone) {
            if (!g_windowBurst) {
                if (coop::save_transfer::WorldTakenFor(1) && !s.IsSlotWorldReady(1)) BurstInWindow();
                else if (HostMayStart(s)) Invalid("the rejoined client's world was ready before the host saw its window");
                else Late("the rejoined client's world was never captured", kJoinBoundMs);
            } else if (HostMayStart(s)) {
                StartJoinCheck();
            } else {
                Late("the rejoined client never became world-ready", kJoinBoundMs);
            }
            return;
        }
        if (!HostMayStart(s)) {
            Late("the client never became world-ready", kJoinBoundMs);
            return;
        }
        StartBurst();
        return;
    case HS::WaitState1Ok:
        if (!Seen("c-ok 1")) {
            Late("the client never confirmed the burst");
            return;
        }
        AfterState1();
        return;
    case HS::WaitRise: {
        // The first level sent to all at least a whole step above the level at the burst: the flood is rising.
        const FS::Counts c = FS::Read();
        if (c.levelsSent == g_c0.levelsSent || c.lastLevelSent < g_before.level + FS::LevelEpsilon()) {
            Late("the host's level never rose a whole step after the burst");
            return;
        }
        Reading now;
        if (!Read(now)) return;
        Fact("the level rose: sent %.1f, read %.1f", c.lastLevelSent, now.level);
        Say(Line("h-state2", now));
        HGo(HS::WaitState2Ok);
        return;
    }
    case HS::WaitState2Ok:
        if (!Seen("c-ok 2")) {
            Late("the client never confirmed the level");
            return;
        }
        if (ArmOf() == Arm::Burst) {
            Pass("the client's pipes read the host's after the burst and its level followed the rise");
            return;
        }
        g_burstDone = true;
        Fact("the client has read the flood; waiting for it to leave");
        HGo(HS::WaitGone);
        return;
    case HS::WaitGone:
        if (!s.IsSlotWorldReady(1)) HGo(HS::Start);
        else Late("the client never left", kBoundMs);
        return;
    case HS::WaitJudge: {
        const FS::Counts c = FS::Read();
        if (c.judged == g_c0.judged) {
            // The mend waits out the client's walk; the far ask is sent the moment the client has read the burst.
            Late("the client's repair never reached the host's judge", ArmOf() == Arm::Mend ? kWalkBoundMs : kBoundMs);
            return;
        }
        if (ArmOf() == Arm::Mend) JudgedMend(c);
        else JudgedFar(c);
        return;
    }
    case HS::WaitFall: {
        // The first level sent to all a whole step below the level at the mend: the flood is falling.
        const FS::Counts c = FS::Read();
        if (c.levelsSent == g_c1.levelsSent || c.lastLevelSent > g_mendLevel - FS::LevelEpsilon()) {
            Late("the host's level never fell a whole step after the mend");
            return;
        }
        Reading now;
        if (!Read(now)) return;
        Fact("the level fell: sent %.1f, read %.1f (%.1f at the mend)", c.lastLevelSent, now.level, g_mendLevel);
        Say(Line("h-fell", now));
        HGo(HS::WaitFellOk);
        return;
    }
    case HS::WaitFellOk:
        if (!Seen("c-ok 4")) {
            Late("the client never confirmed the fall");
            return;
        }
        Pass("the client's repair mended the host's pipe and both levels fell together");
        return;
    case HS::WaitJoinOk:
        if (!Seen("c-ok join")) {
            Late("the rejoined client never confirmed its state");
            return;
        }
        Pass("the joiner's pipes and level read the host's at its world-ready");
        return;
    case HS::WaitOldOk:
        if (!Seen("c-ok old")) {
            Late("the client never confirmed the mended pipe");
            return;
        }
        Pass("a pipe the host's record did not know was mended, and the client read it mended");
        return;
    }
}

}  // namespace

void HostTick(coop::net::Session& s) { HostStep(s); }

void ResetHost() {
    g_h = HS::Start;
    g_windowBurst = false;
    g_before = Reading{};
    g_burstBits.clear();
    g_mendLevel = 0.f;
    g_c0 = g_c1 = FS::Counts{};
}

}  // namespace coop::dev::flood_drill::detail
