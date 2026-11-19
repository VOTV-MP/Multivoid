// coop/dev/pried_measure.cpp -- see coop/dev/pried_measure.h.

#include "coop/dev/pried_measure.h"

#include "coop/config/config.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/pried_drill.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/prop_key_index.h"
#include "coop/player/roster.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_events.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_physics.h"
#include "ue_wrap/facts/function_row.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::pried_measure {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace sg = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;
namespace DC = coop::dev::drill_chat;
using ue_wrap::FRotator;
using ue_wrap::FVector;

// The pry's body on any class, the radiator's two script bodies (its breaking sequence and the ubergraph the sequence's
// Delay resumes into), and the native call that breaks one constraint.
UE_FACT_FN(kRowCrowbarOpen, "pried_measure", nullptr, L"crowbarOpen", AnyClass);
UE_FACT_FN(kRowBreakingSequence, "pried_measure", L"prop_asologoPiece_C", L"breakingSequence", Declared);
UE_FACT_FN(kRowUbergraph, "pried_measure", L"prop_asologoPiece_C", L"ExecuteUbergraph_prop_asologoPiece", Declared);
UE_FACT_FN(kRowBreakConstraint, "pried_measure", L"PhysicsConstraintComponent", L"BreakConstraint", Declared);

constexpr const char* kVerdictTag = "pry-measure";

constexpr int kTagCrowbar = 0x504D5352;  // 'PMSR'
constexpr int kTagSequence = kTagCrowbar + 1;
constexpr int kTagUbergraph = kTagCrowbar + 2;

constexpr const wchar_t* kPlankClass    = L"prop_pryablePlank_C";
constexpr const wchar_t* kRadiatorClass = L"radiator_C";
constexpr const wchar_t* kConstraintClass = L"PhysicsConstraintComponent";

constexpr uint64_t kSampleMs      = 500;
constexpr uint64_t kQuietMs       = 5000;    // after the last event of the prop
constexpr uint64_t kBoundMs       = 90000;   // a step that waits on the other peer
constexpr uint64_t kBreakBoundMs  = 30000;   // a radiator's four breaks, up to 3 s apart, from the first pry
constexpr uint64_t kJoinBoundMs   = 300000;  // the host's wait for the client's relaunch

const char* const kSayTarget      = "pried target ";
const char* const kSayHostRest    = "pried measure host rest";
const char* const kSayJoinerReady = "pried measure joiner ready";
const char* const kSayJoinerDone  = "pried measure joiner done";

enum class Step { WaitReady, WaitName, Pry, Watch, WaitHostRest, WaitJoiner, WaitJoinerDone, JoinerName, JoinerWatch,
                  Done };

// One member component of the prop: held by slot and serial, its last world location for the moved list.
struct Part {
    ue_wrap::CachedObjRef ref;
    std::string cls, name;
    bool constraint = false;
    int hookBreaks = 0;
    bool hasPrev = false;
    FVector prev{};
};

Step g_step = Step::WaitReady;
uint64_t g_stepMs = 0;
uint64_t g_startMs = 0;
bool g_radiator = false;
bool g_installed = false;
bool g_hooksOk = false;
bool g_breakHook = false;
uint64_t g_breakTryMs = 0;

ue_wrap::CachedObjRef g_target;
std::wstring g_key;
std::vector<Part> g_parts;
int g_nConstraints = 0;

bool g_sampling = false;
int g_sampleN = 0;
uint64_t g_nextSampleMs = 0;
uint64_t g_lastEventMs = 0;
uint64_t g_firstPryMs = 0;
int g_crowbarPre = 0, g_crowbarPost = 0, g_breaks = 0;

uint64_t Ms() { return coop::net::NowMs(); }
char Who() { return coop::roster::LocalIsHost() ? 'H' : 'C'; }
void Go(Step s) {
    g_step = s;
    g_stepMs = Ms();
}
uint64_t InStepMs() { return Ms() - g_stepMs; }

std::string N(const std::wstring& w) { return std::string(w.begin(), w.end()); }

void Line(const char* fmt, ...) {
    char msg[480];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    UE_LOGI("[PRY-MEASURE] t=%llu [%c] %s", static_cast<unsigned long long>(Ms()), Who(), msg);
}

void Invalid(const char* fmt, ...) {
    char why[240];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    coop::rig_verdict::Say(kVerdictTag, coop::rig_verdict::Outcome::Invalid, "%s", why);
    g_sampling = false;
    Go(Step::Done);
}

void Flags(char* out, size_t n, void* prop) {
    std::snprintf(out, n, "static=%d frozen=%d sleep=%d sim=%d awake=%d", PR::IsStatic(prop) ? 1 : 0,
                  PR::IsFrozen(prop) ? 1 : 0, PR::IsSleeping(prop) ? 1 : 0,
                  E::IsActorRootSimulatingPhysics(prop) ? 1 : 0, E::ReadActorRootBodyAwake(prop));
}

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Collect(void*, void* comp) {
    Part p;
    p.ref.Set(comp);
    p.cls = N(R::ClassNameOf(comp));
    p.name = N(R::ToString(R::NameOf(comp)));
    p.constraint = R::ClassNameOf(comp) == kConstraintClass;
    if (p.constraint) ++g_nConstraints;
    g_parts.push_back(std::move(p));
}

bool Bind(const std::wstring& key) {
    void* o = coop::prop_key_index::Find(key);
    if (!o || !R::IsLive(o)) return false;
    g_target.Set(o);
    g_key = key;
    g_parts.clear();
    g_nConstraints = 0;
    E::ForEachMemberComponent(o, &Collect, nullptr);
    return true;
}

// How many of the constraints read as broken now, or -1 when the engine's IsBroken did not answer.
int ConstraintsBroken() {
    int broken = 0;
    for (const Part& p : g_parts) {
        if (!p.constraint) continue;
        bool b = false;
        if (!E::ReadConstraintBroken(p.ref.Get(), b)) return -1;
        if (b) ++broken;
    }
    return broken;
}

void PartLine(const char* label, size_t i) {
    const Part& p = g_parts[i];
    void* c = p.ref.Get();
    if (!c) return Line("%s PART %zu class=%s name=%s gone", label, i, p.cls.c_str(), p.name.c_str());
    FVector loc{};
    FRotator rot{};
    const bool okLoc = E::TryGetComponentLocation(c, loc), okRot = E::TryGetComponentWorldRotation(c, rot);
    char brk[48] = "";
    if (p.constraint) {
        bool b = false;
        if (E::ReadConstraintBroken(c, b)) std::snprintf(brk, sizeof(brk), " isBroken=%d", b ? 1 : 0);
        else std::snprintf(brk, sizeof(brk), " isBroken=?");
        std::snprintf(brk + std::strlen(brk), sizeof(brk) - std::strlen(brk), " hookBreaks=%d", p.hookBreaks);
    }
    Line("%s PART %zu class=%s name=%s primitive=%d sim=%d loc=(%.1f,%.1f,%.1f)%s rot=(%.1f,%.1f,%.1f)%s%s", label, i,
         p.cls.c_str(), p.name.c_str(), E::IsPrimitiveComponent(c) ? 1 : 0, E::IsComponentSimulatingPhysics(c) ? 1 : 0,
         loc.X, loc.Y, loc.Z, okLoc ? "" : "(unread)", rot.Pitch, rot.Yaw, rot.Roll, okRot ? "" : "(unread)", brk);
}

// The prop as this copy has it: the flags, the actor's pose, the constraints, then each part.
void ReadAll(const char* label) {
    void* t = g_target.Get();
    if (!t) return Line("%s the prop is gone on this copy", label);
    char fl[128];
    Flags(fl, sizeof(fl), t);
    FVector at{};
    FRotator rot{};
    const bool okLoc = E::TryGetActorLocation(t, at), okRot = E::TryGetActorRotation(t, rot);
    Line("%s key='%ls' class=%ls %s actor=(%.1f,%.1f,%.1f)%s rot=(%.1f,%.1f,%.1f)%s parts=%zu constraints=%d "
         "isBroken=%d hookBreaks=%d", label, g_key.c_str(), R::ClassNameOf(t).c_str(), fl, at.X, at.Y, at.Z,
         okLoc ? "" : "(unread)", rot.Pitch, rot.Yaw, rot.Roll, okRot ? "" : "(unread)", g_parts.size(), g_nConstraints,
         ConstraintsBroken(), g_breaks);
    for (size_t i = 0; i < g_parts.size(); ++i) PartLine(label, i);
}

void Sample() {
    void* t = g_target.Get();
    if (!t) return Line("SAMPLE %d the prop is gone on this copy", g_sampleN++);
    char fl[128], moved[256] = "";
    Flags(fl, sizeof(fl), t);
    FVector at{};
    E::TryGetActorLocation(t, at);
    size_t used = 0;
    for (Part& p : g_parts) {
        FVector loc{};
        if (!E::TryGetComponentLocation(p.ref.Get(), loc)) continue;
        if (p.hasPrev && Dist(loc, p.prev) > 0.5f && used + p.name.size() + 2 < sizeof(moved))
            used += static_cast<size_t>(std::snprintf(moved + used, sizeof(moved) - used, "%s%s", used ? "," : "",
                                                     p.name.c_str()));
        p.prev = loc;
        p.hasPrev = true;
    }
    Line("SAMPLE %d %s actor=(%.1f,%.1f,%.1f) isBroken=%d/%d hookBreaks=%d moved=[%s]", g_sampleN++, fl, at.X, at.Y,
         at.Z, ConstraintsBroken(), g_nConstraints, g_breaks, moved);
}

void Pump() {
    if (!g_sampling || Ms() < g_nextSampleMs) return;
    Sample();
    g_nextSampleMs = Ms() + kSampleMs;
}

void BeginWatch(const char* label) {
    g_crowbarPre = g_crowbarPost = g_breaks = 0;
    g_firstPryMs = 0;
    g_lastEventMs = Ms();
    g_sampleN = 0;
    ReadAll(label);
    g_sampling = true;
    g_nextSampleMs = 0;
}

// ---- The watches ------------------------------------------------------------------------------

void Touch() { g_lastEventMs = Ms(); }

bool OnTarget(const sg::Call& c) { return c.object && c.object == g_target.Raw(); }

void CallLine(const char* what, const char* phase, const sg::Call& c) {
    char fl[128];
    Flags(fl, sizeof(fl), c.object);
    const std::wstring caller = c.callerFunction ? R::ToString(R::NameOf(c.callerFunction)) : std::wstring();
    Line("%s %s depth=%d ourCall=%d caller=%ls %s", what, phase, c.depth, c.fromOurCode ? 1 : 0,
         caller.empty() ? L"(process event)" : caller.c_str(), fl);
}

sg::Verdict CrowbarPre(const sg::Call& c) {
    if (!OnTarget(c)) return sg::Verdict::Run;
    if (!g_crowbarPre++) g_firstPryMs = Ms();
    Touch();
    CallLine("crowbarOpen", "PRE", c);
    return sg::Verdict::Run;
}

void CrowbarPost(const sg::Call& c) {
    if (!OnTarget(c)) return;
    ++g_crowbarPost;
    Touch();
    CallLine("crowbarOpen", "POST", c);
}

sg::Verdict SequencePre(const sg::Call& c) {
    if (!OnTarget(c)) return sg::Verdict::Run;
    Touch();
    CallLine("breakingSequence", "START", c);
    return sg::Verdict::Run;
}

void SequencePost(const sg::Call& c) {
    if (!OnTarget(c)) return;
    Touch();
    CallLine("breakingSequence", "END", c);
}

int Entry(const sg::Call& c) {
    static R::ParamOffset sEntry(L"EntryPoint");
    const int32_t off = c.locals && c.function ? sEntry.Of(c.function) : -1;
    return off < 0 ? -1 : *reinterpret_cast<const int32_t*>(c.locals + off);
}

sg::Verdict UbergraphPre(const sg::Call& c) {
    if (!OnTarget(c)) return sg::Verdict::Run;
    Touch();
    char what[48];
    std::snprintf(what, sizeof(what), "ExecuteUbergraph entry=%d", Entry(c));
    CallLine(what, "PRE", c);
    return sg::Verdict::Run;
}

void UbergraphPost(const sg::Call& c) {
    if (!OnTarget(c)) return;
    Touch();
    char what[64];
    std::snprintf(what, sizeof(what), "ExecuteUbergraph entry=%d broken=%d/%d", Entry(c), g_breaks, g_nConstraints);
    CallLine(what, "POST", c);
}

// UPhysicsConstraintComponent::BreakConstraint, native, so the gate cannot see it: a post hook on its Func.
void OnBreakPost(void* context, void*, void*) {
    void* t = g_target.Raw();
    if (!t || !context || R::OuterOf(context) != t) return;
    int idx = -1;
    for (size_t i = 0; i < g_parts.size(); ++i)
        if (g_parts[i].ref.Raw() == context) idx = static_cast<int>(i);
    if (idx >= 0) ++g_parts[idx].hookBreaks;
    ++g_breaks;
    Touch();
    const UH::CallerFrame f = UH::CurrentCallerFrame();
    const std::wstring caller = f.function ? R::ToString(R::NameOf(f.function)) : std::wstring();
    char fl[128];
    Flags(fl, sizeof(fl), t);
    Line("BreakConstraint part=%d name=%s break #%d of %d caller=%ls %s", idx,
         idx >= 0 ? g_parts[idx].name.c_str() : "?", g_breaks, g_nConstraints,
         caller.empty() ? L"(process event)" : caller.c_str(), fl);
}

// The watches are registered once per process; every one must be live before the pry, or the arm measures nothing.
bool EnsureHooks(bool radiator) {
    if (g_hooksOk) return true;
    const uint64_t now = Ms();
    if (!g_installed) {
        g_installed = true;
        bool ok = sg::WatchName(kRowCrowbarOpen, kTagCrowbar, &CrowbarPre, &CrowbarPost);
        if (radiator) {
            ok = sg::WatchName(kRowBreakingSequence, kTagSequence, &SequencePre, &SequencePost) && ok;
            ok = sg::WatchName(kRowUbergraph, kTagUbergraph, &UbergraphPre, &UbergraphPost) && ok;
        }
        if (!ok) {
            Invalid("the script gate refused a watch");
            return false;
        }
    }
    if (radiator && !g_breakHook && now >= g_breakTryMs) {
        g_breakTryMs = now + 1000;  // the class is looked up through the object array on a miss
        const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowBreakConstraint);
        g_breakHook = fn && UH::InstallPostHook(fn, &OnBreakPost);
    }
    bool settled = sg::NameWatchSettled(kRowCrowbarOpen, kTagCrowbar);
    bool live = sg::NameWatchLive(kRowCrowbarOpen, kTagCrowbar);
    if (radiator) {
        settled = settled && sg::NameWatchSettled(kRowBreakingSequence, kTagSequence) &&
                  sg::NameWatchSettled(kRowUbergraph, kTagUbergraph);
        live = live && sg::NameWatchLive(kRowBreakingSequence, kTagSequence) &&
               sg::NameWatchLive(kRowUbergraph, kTagUbergraph);
    }
    if (settled && !live) {
        Invalid("a script-gate watch settled dead (the gate logged which)");
        return false;
    }
    if (settled && (!radiator || g_breakHook)) {
        g_hooksOk = true;
        Line("watches live: crowbarOpen%s", radiator ? ", breakingSequence, ExecuteUbergraph, BreakConstraint" : "");
        return true;
    }
    if (now - g_startMs > kBoundMs)
        Invalid("a watch never went live in %llu ms (settled=%d breakConstraintHook=%d)",
                static_cast<unsigned long long>(kBoundMs), settled ? 1 : 0, g_breakHook ? 1 : 0);
    return false;
}

bool RejoinedLife() {
    static const bool second = coop::config::ReadEnv("VOTVCOOP_PRIED_MEASURE_REJOIN") == "1";
    return second;
}

// A watcher's end: the pry came, a radiator's constraints all broke, and the prop was quiet for kQuietMs.
// True when the sampler is done, false while it runs; an overrun is INVALID.
bool WatcherQuiet() {
    const uint64_t now = Ms();
    if (!g_crowbarPre) {
        if (InStepMs() > kBoundMs) Invalid("no crowbarOpen ran on this copy in %llu ms", static_cast<unsigned long long>(kBoundMs));
        return false;
    }
    const bool allBroken = !g_radiator || g_breaks >= g_nConstraints;
    if ((!g_crowbarPost || !allBroken) && now - g_firstPryMs > kBreakBoundMs) {
        Invalid("after %llu ms the pry is incomplete here: crowbarOpen POST %d, constraints broken %d of %d",
                static_cast<unsigned long long>(kBreakBoundMs), g_crowbarPost, g_breaks, g_nConstraints);
        return false;
    }
    return g_crowbarPost && allBroken && now - g_lastEventMs >= kQuietMs;
}

void Finish() {
    UE_LOGI("rig: VERDICT %s DONE -- measured, nothing judged", kVerdictTag);
    Go(Step::Done);
}

void TickStep(coop::net::Session& s) {
    const bool host = coop::roster::LocalIsHost();
    switch (g_step) {
    case Step::WaitReady: {
        if (!coop::dev::pried_drill::JoinOver(s)) {
            if (Ms() - g_startMs > kJoinBoundMs) Invalid("this peer's join did not finish in %llu ms", static_cast<unsigned long long>(kJoinBoundMs));
            return;
        }
        if (host) {
            const std::wstring key = coop::dev::pried_drill::PickByClass(g_radiator ? kRadiatorClass : kPlankClass);
            if (key.empty()) return Invalid("no keyed prop of the class stands on the host");
            if (!Bind(key)) return Invalid("the host's own copy of the prop does not resolve");
            if (g_radiator && !g_nConstraints) return Invalid("no PhysicsConstraintComponent member found on the radiator");
            DC::Say(std::string(kSayTarget) + N(key));
            BeginWatch("BEFORE");
            return Go(Step::Watch);
        }
        if (RejoinedLife()) {
            DC::Say(kSayJoinerReady);
            return Go(Step::JoinerName);
        }
        return Go(Step::WaitName);
    }
    case Step::WaitName: {
        std::string rest;
        if (!DC::SeenRestOf(kSayTarget, &rest)) {
            if (InStepMs() > kBoundMs) Invalid("the host never named the prop");
            return;
        }
        if (!Bind(std::wstring(rest.begin(), rest.end()))) {
            if (InStepMs() > kBoundMs) Invalid("the named prop does not resolve on the client");
            return;
        }
        if (g_radiator && !g_nConstraints) return Invalid("no PhysicsConstraintComponent member found on the radiator");
        BeginWatch("BEFORE");
        return Go(Step::Pry);
    }
    case Step::Pry: {  // one tick after BEFORE and the first sample, so the pre-pry state is on record
        void* t = g_target.Get();
        if (!t) return Invalid("the prop is gone on the client before its pry");
        const bool ran = PR::CallCrowbarOpen(t);
        Line("PRIED-CALL crowbarOpen dispatched=%d", ran ? 1 : 0);
        if (!ran) return Invalid("the client's crowbarOpen did not dispatch");
        return Go(Step::Watch);
    }
    case Step::Watch: {
        if (!WatcherQuiet()) return;
        g_sampling = false;
        ReadAll("REST");
        if (host) {
            DC::Say(kSayHostRest);
            return Go(Step::WaitJoiner);
        }
        return Go(Step::WaitHostRest);
    }
    case Step::WaitHostRest: {
        if (!DC::SeenHas(kSayHostRest)) {
            if (InStepMs() > kBoundMs + kBreakBoundMs) Invalid("the host never said its copy came to rest");
            return;
        }
        Line("LIFE1 DONE");
        UE_LOGI("pried-measure: REJOIN MARKER");
        return Go(Step::Done);
    }
    case Step::WaitJoiner: {
        if (!DC::SeenHas(kSayJoinerReady)) {
            if (InStepMs() > kJoinBoundMs) Invalid("no joiner said it was ready in %llu ms (the run needs --rejoin)", static_cast<unsigned long long>(kJoinBoundMs));
            return;
        }
        ReadAll("HOST-AT-JOINER");
        DC::Say(std::string(kSayTarget) + N(g_key));
        return Go(Step::WaitJoinerDone);
    }
    case Step::WaitJoinerDone: {
        if (!DC::SeenHas(kSayJoinerDone)) {
            if (InStepMs() > 2 * kBoundMs) Invalid("the joiner never said it read the prop");
            return;
        }
        ReadAll("HOST-AFTER-JOINER");
        return Finish();
    }
    case Step::JoinerName: {
        std::string rest;
        if (!DC::SeenRestOf(kSayTarget, &rest)) {
            if (InStepMs() > kBoundMs) Invalid("the host never named the prop to the joiner");
            return;
        }
        if (!Bind(std::wstring(rest.begin(), rest.end()))) {
            if (InStepMs() > kBoundMs) Invalid("the named prop does not resolve on the joiner");
            return;
        }
        BeginWatch("JOINER-READ");
        return Go(Step::JoinerWatch);
    }
    case Step::JoinerWatch: {
        if (Ms() - g_lastEventMs < kQuietMs) {
            if (InStepMs() > kBreakBoundMs + kQuietMs) Invalid("the joiner's copy was never quiet");
            return;
        }
        g_sampling = false;
        ReadAll("JOINER-REST");
        DC::Say(kSayJoinerDone);
        return Go(Step::Done);
    }
    case Step::Done:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session& session, bool radiator) {
    if (g_step == Step::Done) return;
    g_radiator = radiator;
    if (!g_startMs) {
        g_startMs = Ms();
        g_stepMs = g_startMs;
    }
    if (!EnsureHooks(radiator)) return;
    Pump();
    TickStep(session);
}

void OnDisconnect() {
    g_step = Step::WaitReady;
    g_stepMs = 0;
    g_startMs = 0;
    g_sampling = false;
    g_target.Reset();
    g_key.clear();
    g_parts.clear();
    g_nConstraints = 0;
    g_crowbarPre = g_crowbarPost = g_breaks = 0;
}

}  // namespace coop::dev::pried_measure
