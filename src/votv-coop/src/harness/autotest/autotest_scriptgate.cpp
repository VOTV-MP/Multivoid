// harness/autotest/autotest_scriptgate.cpp -- the script-body gate drill, solo or host. Two
// signal servers stand in for "one instance of two": their fix verb is refused on one and runs
// on the other, then, with fix running on both, the check it calls on itself and the gamemode's
// calcServerEff it calls through a context switch are refused for one box by instance and by
// caller, and the ubergraph entry a sendName event reaches through a local final call is read
// with its entry-point argument and its caller. Every refusal has a state observable outside the
// gate (the break flag, an efficiency sentinel) or, for the two Blueprint-internal routes, the
// post callback that only a run body reaches. A negative arm shows the same verb running once
// the watch is gone. Env VOTVCOOP_RUN_SCRIPT_GATE_DRILL=1; the lines are tagged [SCRIPTGATE].

#include "harness/autotest.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/serverbox.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace SG = ue_wrap::script_gate;
namespace SB = ue_wrap::serverbox;

constexpr DWORD kWorldWaitMs = 240'000;
constexpr DWORD kPollMs = 2000;
constexpr int   kTagFix = 1, kTagCheck = 2, kTagCalc = 3, kTagUber = 4, kTagHealth = 5;
constexpr int   kSendNameEntry = 3501;   // the ubergraph entry sendName's stub names
constexpr float kSentinel = -1.0f;       // no efficiency the game computes is negative

// The drill's state, written on the game thread only.
void* g_boxA = nullptr;
void* g_boxB = nullptr;
void* g_fnFix = nullptr;
void* g_fnCheck = nullptr;
void* g_fnCalc = nullptr;
void* g_fnUber = nullptr;
void* g_fnSendName = nullptr;
void* g_fnHealth = nullptr;
int32_t g_entryOff = -1;
int32_t g_healthOff = -1;
int32_t g_healthSeen = -12345;   // what the post callback read through the out-parameter record
const std::uint8_t* g_healthPtr = nullptr;   // where the record pointed
bool g_cancelFixOnA = false;
bool g_cancelCheckOnA = false;
bool g_cancelCalcFromA = false;
bool g_cancelUberOnA = false;

// The watched functions belong to the class, so every signal server in the world reaches them;
// only the two drill boxes are tallied, the rest are counted as other and always run.
struct Counts {
    int preA = 0, preB = 0, postA = 0, postB = 0;
    int fromFix = 0;        // pre fires whose caller frame is fix on the same box
    int fromEngine = 0;     // pre fires with no caller frame (ProcessEvent)
    int callerMismatch = 0; // a caller frame that is neither
    int other = 0;          // calls on or from a box that is not A or B, or another ubergraph entry
    int entryOk = 0;        // ubergraph pre fires with the sendName entry and sendName as the caller
};
Counts g_fix, g_check, g_calc, g_uber;

bool IsDrillBox(void* o) { return o == g_boxA || o == g_boxB; }

void Tally(Counts& c, const SG::Call& call, bool post) {
    if (!IsDrillBox(call.object)) { ++c.other; return; }
    const bool a = call.object == g_boxA;
    if (post) { (a ? c.postA : c.postB)++; return; }
    (a ? c.preA : c.preB)++;
    if (!call.callerFunction) ++c.fromEngine;
    else if (call.callerFunction == g_fnFix && call.callerObject == call.object) ++c.fromFix;
    else ++c.callerMismatch;
}

// The fix arm refuses only the drill's own reflected call on A: a game-made fix on A stays the
// game's.
SG::Verdict PreFix(const SG::Call& call) {
    Tally(g_fix, call, false);
    const bool ours = call.object == g_boxA && call.fromOurCode;
    return (g_cancelFixOnA && ours) ? SG::Verdict::Cancel : SG::Verdict::Run;
}
void PostFix(const SG::Call& call) { Tally(g_fix, call, true); }

SG::Verdict PreCheck(const SG::Call& call) {
    Tally(g_check, call, false);
    const bool fromFixOnA = call.object == g_boxA && call.callerFunction == g_fnFix;
    return (g_cancelCheckOnA && fromFixOnA) ? SG::Verdict::Cancel : SG::Verdict::Run;
}
void PostCheck(const SG::Call& call) { Tally(g_check, call, true); }

// calcServerEff runs on the gamemode: the instance is never a box, so the arm keys on the
// CALLER, which is the box whose fix called it.
SG::Verdict PreCalc(const SG::Call& call) {
    if (!IsDrillBox(call.callerObject)) { ++g_calc.other; return SG::Verdict::Run; }
    const bool a = call.callerObject == g_boxA;
    (a ? g_calc.preA : g_calc.preB)++;
    if (call.callerFunction == g_fnFix) ++g_calc.fromFix; else ++g_calc.callerMismatch;
    return (g_cancelCalcFromA && a) ? SG::Verdict::Cancel : SG::Verdict::Run;
}
void PostCalc(const SG::Call& call) {
    if (!IsDrillBox(call.callerObject)) return;
    (call.callerObject == g_boxA ? g_calc.postA : g_calc.postB)++;
}

// The ubergraph arm counts only the entry the sendName stub names: a box's own latent resumes
// re-enter the ubergraph at other entries throughout, and those always run.
int32_t EntryOf(const SG::Call& call) {
    if (g_entryOff < 0 || !call.locals) return -1;
    int32_t entry = 0;
    std::memcpy(&entry, call.locals + g_entryOff, sizeof(entry));
    return entry;
}
SG::Verdict PreUber(const SG::Call& call) {
    if (EntryOf(call) != kSendNameEntry) { ++g_uber.other; return SG::Verdict::Run; }
    Tally(g_uber, call, false);
    if (IsDrillBox(call.object) && call.callerFunction == g_fnSendName && call.callerObject == call.object)
        ++g_uber.entryOk;
    return (g_cancelUberOnA && call.object == g_boxA) ? SG::Verdict::Cancel : SG::Verdict::Run;
}
void PostUber(const SG::Call& call) {
    if (EntryOf(call) != kSendNameEntry) return;
    Tally(g_uber, call, true);
}

// The out-parameter arm: after countHealth runs, its `health` out parameter is read through the
// frame's out-parameter record, which points at the caller's storage, and must equal what the
// caller's frame reads back.
void PostHealth(const SG::Call& call) {
    if (!IsDrillBox(call.object)) return;
    if (const std::uint8_t* p = SG::OutParamPtr(call, g_healthOff)) {
        g_healthPtr = p;
        std::memcpy(&g_healthSeen, p, sizeof(g_healthSeen));
    }
}

// Run `fn` on the game thread and wait for it; false when the pump never ran it.
bool OnGameThread(std::function<void()> fn) {
    std::atomic<bool> done{false};
    GT::Post([&] { fn(); done.store(true, std::memory_order_release); });
    for (int i = 0; i < 300 && !done.load(std::memory_order_acquire); ++i) ::Sleep(100);
    return done.load(std::memory_order_acquire);
}

bool CallVerb(void* box, void* fn) {
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(box, f);
}

bool CallSendName(void* box) {
    ue_wrap::ParamFrame f(g_fnSendName);
    if (!f.valid()) return false;
    const R::FName n = ue_wrap::fname_utils::StringToFName(L"scriptgate");
    return f.Set<R::FName>(L"name", n) && ue_wrap::Call(box, f);
}

float EfficiencyCalc() {
    SB::Aggregates a;
    return SB::ReadAggregates(a) ? a.efficiencyCalc : -2.0f;
}

bool Resolve() {
    std::vector<void*> boxes;
    if (SB::ReadServers(boxes) < 2 || !SB::EnsureBreakResolved()) return false;
    g_boxA = boxes[0];
    g_boxB = boxes[1];
    void* cls = R::ClassOf(g_boxA);
    void* gm = R::FindObjectByClass(L"mainGamemode_C");
    void* gmCls = gm ? R::ClassOf(gm) : nullptr;
    if (!cls || !gmCls) return false;
    g_fnFix = R::FindFunction(cls, L"fix");
    g_fnCheck = R::FindFunction(cls, L"check");
    g_fnUber = R::FindFunction(cls, L"ExecuteUbergraph_serverBox");
    g_fnSendName = R::FindFunction(cls, L"sendName");
    g_fnCalc = R::FindFunction(gmCls, L"calcServerEff");
    g_fnHealth = R::FindFunction(cls, L"countHealth");
    g_entryOff = g_fnUber ? R::FindParamOffset(g_fnUber, L"EntryPoint") : -1;
    g_healthOff = g_fnHealth ? R::FindParamOffset(g_fnHealth, L"health") : -1;
    return g_fnFix && g_fnCheck && g_fnUber && g_fnSendName && g_fnCalc && g_fnHealth &&
           g_entryOff >= 0 && g_healthOff >= 0;
}

struct Verdict { int pass = 0, fail = 0; };
void Check(Verdict& v, bool ok, const char* what) {
    (ok ? v.pass : v.fail)++;
    UE_LOGI("[SCRIPTGATE] %s -- %s", ok ? "ok  " : "FAIL", what);
}

}  // namespace

void RunScriptGateDrill() {
    if (!SG::IsInstalled()) {
        UE_LOGW("[SCRIPTGATE] the gate is not installed -- nothing to drill (FAIL)");
        UE_LOGI("script_gate_drill: VERDICT FAIL (not installed)");
        UE_LOGI("script_gate_drill: DONE");
        return;
    }
    DWORD waited = 0;
    bool resolved = false;
    while (!resolved && waited < kWorldWaitMs) {
        OnGameThread([&] { resolved = Resolve(); });
        if (!resolved) { ::Sleep(kPollMs); waited += kPollMs; }
    }
    if (!resolved) {
        UE_LOGW("[SCRIPTGATE] INCONCLUSIVE -- no two signal servers with the break group resolved "
                "within %lu s", static_cast<unsigned long>(kWorldWaitMs / 1000));
        UE_LOGI("script_gate_drill: DONE");
        return;
    }
    ::Sleep(3000);  // the world settles; the boxes' own begin-play has run
    Verdict v;
    SB::Aggregates base{};

    OnGameThread([&] {
        SB::ReadAggregates(base);
        SG::SetEnabled(true);
        UE_LOGI("[SCRIPTGATE] boxes A=%p B=%p fix=%p check=%p calc=%p uber=%p (EntryPoint@%d) sendName=%p",
                g_boxA, g_boxB, g_fnFix, g_fnCheck, g_fnCalc, g_fnUber, g_entryOff, g_fnSendName);

        // Pass 1: the ProcessEvent route, refused per instance. Both boxes broken; fix on A is
        // refused, fix on B runs; the break flag is the observable.
        g_cancelFixOnA = true;
        Check(v, SG::Watch(g_fnFix, kTagFix, &PreFix, &PostFix), "watch fix");
        SB::ApplyBreak(g_boxA, true);
        SB::ApplyBreak(g_boxB, true);
        Check(v, CallVerb(g_boxA, g_fnFix), "pass1: fix(A) dispatched");
        Check(v, SB::ReadIsBroken(g_boxA), "pass1: A stays broken -- fix(A) was refused (state)");
        Check(v, g_fix.preA == 1 && g_fix.postA == 0, "pass1: fix(A) pre fired once, post never");
        Check(v, CallVerb(g_boxB, g_fnFix), "pass1: fix(B) dispatched");
        Check(v, !SB::ReadIsBroken(g_boxB), "pass1: B repaired -- fix(B) ran (state)");
        Check(v, g_fix.preB == 1 && g_fix.postB == 1, "pass1: fix(B) pre and post fired once");
        Check(v, g_fix.fromEngine == 2, "pass1: both fix calls arrived with no caller frame (ProcessEvent)");
        g_cancelFixOnA = false;
        Check(v, SG::Unwatch(g_fnFix, kTagFix, &PreFix, &PostFix), "unwatch fix");

        // Pass 2: fix runs on both; the check it calls on itself (a local virtual call) is refused
        // for A by instance and caller, and the gamemode's calcServerEff it calls through a
        // context switch is refused for A by caller alone. The efficiency sentinel is the
        // observable for the second; the first is a post callback only a run body reaches.
        g_cancelCheckOnA = true;
        g_cancelCalcFromA = true;
        Check(v, SG::Watch(g_fnCheck, kTagCheck, &PreCheck, &PostCheck), "watch check");
        Check(v, SG::Watch(g_fnCalc, kTagCalc, &PreCalc, &PostCalc), "watch calcServerEff");
        SB::ApplyBreak(g_boxA, true);   // each apply calls check() through ProcessEvent
        SB::ApplyBreak(g_boxB, true);
        const int checkEngineBefore = g_check.fromEngine;
        Check(v, checkEngineBefore == 2 && g_check.postA == 1 && g_check.postB == 1,
              "pass2: the two applies reached check with no caller frame, and ran");
        SB::Aggregates s = base; s.efficiencyCalc = kSentinel; SB::WriteAggregates(s);
        Check(v, CallVerb(g_boxA, g_fnFix), "pass2: fix(A) dispatched");
        Check(v, !SB::ReadIsBroken(g_boxA), "pass2: fix(A) ran (its own body was not refused)");
        Check(v, g_check.preA == 2 && g_check.postA == 1 && g_check.fromFix == 1,
              "pass2: check from fix(A) fired pre, was refused, post never (bookkeeping)");
        Check(v, g_calc.preA == 1 && g_calc.postA == 0 && g_calc.fromFix == 1,
              "pass2: calcServerEff from fix(A) fired pre with fix as caller, was refused");
        Check(v, EfficiencyCalc() == kSentinel, "pass2: the efficiency sentinel survived fix(A) (state)");
        Check(v, CallVerb(g_boxB, g_fnFix), "pass2: fix(B) dispatched");
        Check(v, g_check.preB == 2 && g_check.postB == 2 && g_check.fromFix == 2,
              "pass2: check from fix(B) fired pre and post (ran)");
        Check(v, g_calc.preB == 1 && g_calc.postB == 1 && g_calc.fromFix == 2,
              "pass2: calcServerEff from fix(B) ran");
        Check(v, EfficiencyCalc() != kSentinel, "pass2: fix(B) recomputed the efficiency (state)");
        Check(v, g_check.callerMismatch == 0 && g_calc.callerMismatch == 0, "pass2: every caller frame named fix");
        g_cancelCheckOnA = false;
        g_cancelCalcFromA = false;
        Check(v, SG::Unwatch(g_fnCheck, kTagCheck, &PreCheck, &PostCheck), "unwatch check");
        Check(v, SG::Unwatch(g_fnCalc, kTagCalc, &PreCalc, &PostCalc), "unwatch calcServerEff");

        // Pass 3: the ubergraph through a local final call from the sendName stub, with its
        // entry-point argument read off the frame; refused for A.
        g_cancelUberOnA = true;
        Check(v, SG::Watch(g_fnUber, kTagUber, &PreUber, &PostUber), "watch ExecuteUbergraph_serverBox");
        Check(v, CallSendName(g_boxA), "pass3: sendName(A) dispatched");
        Check(v, CallSendName(g_boxB), "pass3: sendName(B) dispatched");
        Check(v, g_uber.preA == 1 && g_uber.preB == 1, "pass3: the sendName entry fired once per box");
        Check(v, g_uber.entryOk == 2, "pass3: EntryPoint read as 3501 with sendName as the caller, twice");
        Check(v, g_uber.postA == 0 && g_uber.postB == 1, "pass3: A's entry refused (no post), B's ran");
        g_cancelUberOnA = false;
        Check(v, SG::Unwatch(g_fnUber, kTagUber, &PreUber, &PostUber), "unwatch the ubergraph");

        // The out-parameter arm: countHealth writes its answer into the caller's storage, and
        // the post callback reads it through the frame's record.
        Check(v, SG::Watch(g_fnHealth, kTagHealth, nullptr, &PostHealth), "watch countHealth");
        {
            ue_wrap::ParamFrame f(g_fnHealth);
            const bool ran = f.valid() && ue_wrap::Call(g_boxA, f);
            const int32_t back = f.Get<int32_t>(L"health");
            // The record must point INTO the caller's frame: a callee-side copy or any zeroed
            // memory would also read 0 on an empty box, so the address is the check, not the value.
            const std::uint8_t* ours = f.data() ? static_cast<const std::uint8_t*>(f.data()) + f.ParamOffset(L"health") : nullptr;
            Check(v, ran, "out-param: countHealth(A) dispatched");
            Check(v, g_healthPtr != nullptr && g_healthPtr == ours,
                  "out-param: the record points at the caller's own frame slot");
            Check(v, g_healthSeen != -12345 && g_healthSeen == back,
                  "out-param: the value read through the record equals the caller's");
            UE_LOGI("[SCRIPTGATE] countHealth(A): caller read %d, the record read %d", back, g_healthSeen);
        }
        Check(v, SG::Unwatch(g_fnHealth, kTagHealth, nullptr, &PostHealth), "unwatch countHealth");

        // The negative arm: no watch, the same verb, the same box -- it must run, and the retired
        // watch must not see it (the count is taken BEFORE the call).
        SB::ApplyBreak(g_boxA, true);
        const int fixPreBefore = g_fix.preA;
        Check(v, CallVerb(g_boxA, g_fnFix), "negative: fix(A) dispatched with no watch");
        Check(v, !SB::ReadIsBroken(g_boxA), "negative: A repaired -- nothing refused without a watch");
        Check(v, g_fix.preA == fixPreBefore, "negative: the retired watch did not fire");

        const SG::Stats st = SG::GetStats();
        Check(v, st.cancelled == 4, "four refusals counted in the gate's own tally");
        UE_LOGI("[SCRIPTGATE] other-box traffic through the watched functions: fix=%d check=%d calc=%d "
                "ubergraph(other entries)=%d -- all ran", g_fix.other, g_check.other, g_calc.other, g_uber.other);
        Check(v, st.offGameThread == 0, "no watched body was reached off the game thread");
        Check(v, st.faults == 0, "no callback faulted");
        SB::WriteAggregates(base);   // the farm's totals as they were before the drill
    });

    // The tax base over a five-second window with counting armed.
    SG::SetPerfCounting(true);
    SG::Stats t0 = SG::GetStats();
    ::Sleep(5000);
    SG::Stats t1 = SG::GetStats();
    SG::SetPerfCounting(false);
    UE_LOGI("[SCRIPTGATE] script bodies over 5 s: %llu (%llu on the game thread) = %.0f/s",
            t1.calls - t0.calls, t1.callsGameThread - t0.callsGameThread,
            static_cast<double>(t1.calls - t0.calls) / 5.0);
    OnGameThread([] { SG::SetEnabled(false); });

    UE_LOGI("script_gate_drill: VERDICT %s (%d checks passed, %d failed)",
            v.fail == 0 ? "PASS" : "FAIL", v.pass, v.fail);
    UE_LOGI("script_gate_drill: DONE");
}

DWORD WINAPI ScriptGateDrillThread(LPVOID) {
    RunScriptGateDrill();
    return 0;
}

}  // namespace harness::autotest
