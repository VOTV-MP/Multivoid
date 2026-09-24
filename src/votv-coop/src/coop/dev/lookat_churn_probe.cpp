// coop/dev/lookat_churn_probe.cpp -- see coop/dev/lookat_churn_probe.h.

#include "coop/dev/lookat_churn_probe.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>

namespace coop::dev::lookat_churn_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;

// Game-thread only. No mutex.
// Per run, and per KIND of line: the aim-episode lines and the change lines share nothing, because
// a drill that sweeps the camera opens and closes an episode at every heading and would otherwise
// spend the whole budget before the held aim -- on the only lines that say WHICH field moved.
constexpr int      kMaxLines        = 40;
constexpr uint64_t kVerdictPeriodMs = 15000;
// A window shorter than this is somebody turning around, not a held look: the reading names it
// separately rather than reporting a rate over a fraction of a second.
constexpr uint64_t kHeldMs = 2000;

// The rebuild itself, watched where it happens. ui_UI_C::buildActions destroys every action button
// and creates them again from scratch, so one call IS one visible reset of the row, and counting
// the calls measures the symptom directly where the field set below only measures the look-at
// path's reason for causing one. Name watches, not exact ones: a name watch re-resolves itself
// across a world load, where an exact one would sit on a function that no longer exists.
//
// Three verbs, because one of them alone does not name the trigger. buildActions is the symptom.
// Its caller is always buildActionList, so buildActionList is watched too and ITS caller frame is
// the answer: LookAtFunction means the trace changed its mind, the ubergraph means an input path
// fired, lookAtLookedAway means something dropped the aim outright -- which is the third watch,
// since that path nulls lookAtActor and the UI closes with it.
struct Watched { const wchar_t* name; int tag; const char* what; };
constexpr Watched kWatched[] = {
    { L"buildActions",     0x4C4B4241, "the action row was rebuilt" },      // 'LKBA'
    { L"buildActionList",  0x4C4B424C, "the player asked for a rebuild" },  // 'LKBL'
    { L"lookAtLookedAway", 0x4C4B4157, "the aim was dropped" },             // 'LKAW'
};

// The fourth watch, and the only one with two phases: the comparison that decides the rebuild,
// read at the moment it is made. Its two operands are never both readable from outside the call --
// the stored five stop being the operand as soon as the body overwrites them, and the locals are
// zero until the body writes them -- which is why every earlier reading of this lane could only
// say that the fresh side stood still, never what the comparison actually saw.
//
//   PRE  reads the five STORED fields off the instance. The compare's right-hand side, intact.
//   POST reads the five frame LOCALS (the left-hand side) and the stored fields AGAIN, so one
//        call answers three questions: which pair disagreed, whether the body then wrote the
//        fresh set back, and whether a rebuild followed at all.
//
// A name watch, like the three above, so it survives a world load.
constexpr const wchar_t* kCompareFn  = L"LookAtFunction";
constexpr int            kCompareTag = 0x4C4B4C46;  // 'LKLF'

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One run of ticks with the same actor under the crosshair, and inside it the STEADY WINDOW: the
// part since the camera last moved. Only that window is evidence. A turning camera re-traces onto
// another part of the same actor, which changes the hit component honestly, and counting that as
// churn would report every sweep of the drill as a defect.
struct Episode {
    void*        actor = nullptr;
    std::wstring cls, key;
    uint64_t     startMs = 0, endMs = 0;
    unsigned     changes = 0, builds = 0;    // the whole time the actor was the target

    uint64_t steadySinceMs = 0, steadySinceTick = 0;
    unsigned camMoves = 0;
    unsigned sChanges = 0, sBuilds = 0;      // and these, only since the camera last stopped
    unsigned nComp = 0, nBounds = 0, nVerify = 0, nState = 0, nBlind = 0;
    // The aimed prop's own state, for context only. Aprop_C::lookAt answers from
    // `IsSimulatingPhysics || attached || frozen || sleep || returnLookAt`, but that bool is NOT one
    // of the five fields the rebuild is gated on -- it is consumed downstream of that branch and
    // both of its outcomes reach the rebuild -- so a move here explains what the hovertext SAYS,
    // never whether the row was rebuilt. Two of its six terms are sampled; the rest are not.
    unsigned nPropState = 0;
    bool     propFrozen = false, propStatic = false, propSleeping = false, havePropState = false;
    uint64_t lastChangeMs = 0, lastChangeTick = 0;
    uint64_t minGapMs = 0, maxGapMs = 0, sumGapMs = 0;
    unsigned gaps = 0;
    uint64_t minGapTicks = 0, maxGapTicks = 0;

    uint64_t SteadyMs() const { return endMs > steadySinceMs ? endMs - steadySinceMs : 0; }
};

E::MainPlayerLookAt g_prev{};
E::MainPlayerLookAt g_seam{};   // the set as the rebuild seam saw it last, transients included
ue_wrap::FRotator   g_prevCam{};
bool     g_havePrev = false;
uint64_t g_tick = 0;

Episode  g_cur{};     // live, or empty when nothing is aimed at
Episode  g_best{};    // the widest STEADY window of the run: what the reading is drawn from
void*    g_lastEpisodeActor = nullptr;  // identity only, never dereferenced
unsigned g_flipBacks = 0;   // A -> B -> A: the aim returned to the actor it had just left
unsigned g_episodes  = 0;
unsigned g_changes   = 0;   // over the whole run, episodes included or not
unsigned g_builds    = 0;   // buildActions calls over the whole run
unsigned g_lists     = 0;   // buildActionList calls: the verb LookAtFunction itself reaches
unsigned g_dropped   = 0;   // lookAtLookedAway calls: the aim given up, not merely rebuilt
uint64_t g_firstMs   = 0;   // when this run's first tick ran, so a rate can be a rate
int      g_lines      = 0;   // aim episodes
int      g_changeLines = 0;  // the field that moved
int      g_buildLines  = 0;  // the caller frames
bool     g_any = false;
bool     g_watchAsked = false;   // the name watch is registered; LIVE is a separate question
uint64_t g_lastVerdictMs = 0;

// Keep the widest steady window of the run, with the counters that belong to it.
// Compared before it is copied: this runs on every tick the camera moves, and an Episode carries
// two std::wstring past the small-string buffer, so copying it first would allocate twice a frame
// for the whole time a player is looking around.
void ConsiderBest(const Episode& e, uint64_t nowMs) {
    if (!e.actor) return;
    const uint64_t span = nowMs > e.steadySinceMs ? nowMs - e.steadySinceMs : 0;
    if (span <= g_best.SteadyMs()) return;
    g_best = e;
    g_best.endMs = nowMs;
}

void StartSteadyWindow(uint64_t nowMs) {
    g_cur.steadySinceMs   = nowMs;
    g_cur.steadySinceTick = g_tick;
    g_cur.lastChangeMs    = nowMs;
    g_cur.lastChangeTick  = g_tick;
    g_cur.sChanges = g_cur.sBuilds = 0;
    g_cur.nComp = g_cur.nBounds = g_cur.nVerify = g_cur.nState = g_cur.nBlind = 0;
    g_cur.nPropState = 0;
    g_cur.gaps = 0;
    g_cur.minGapMs = g_cur.maxGapMs = g_cur.sumGapMs = 0;
    g_cur.minGapTicks = g_cur.maxGapTicks = 0;
}

void CloseEpisode(uint64_t nowMs) {
    if (!g_cur.actor) return;
    ConsiderBest(g_cur, nowMs);
    g_lastEpisodeActor = g_cur.actor;
    if (g_lines < kMaxLines) {
        ++g_lines;
        UE_LOGW("lookat_churn_probe: AIM END '%ls' key='%ls' after %llu ms -- %u change(s) and %u "
                "rebuild(s) while it was the target, camera moved %u time(s)",
                g_cur.cls.c_str(), g_cur.key.c_str(),
                static_cast<unsigned long long>(nowMs > g_cur.startMs ? nowMs - g_cur.startMs : 0),
                g_cur.changes, g_cur.builds, g_cur.camMoves);
    }
    g_cur = Episode{};
}

void OpenEpisode(void* actor, uint64_t nowMs) {
    g_cur = Episode{};
    g_cur.actor = actor;
    // Resolved once per episode, off the pointer read from the live pawn this same tick: a name is
    // a dispatch for some classes, and the key is one for a chipPile.
    g_cur.cls = R::IsLive(actor) ? R::ClassNameOf(actor) : L"<dead>";
    g_cur.key = R::IsLive(actor) ? ue_wrap::prop::GetInteractableKeyString(actor) : L"";
    g_cur.startMs = nowMs;
    // Only a prop may be read at prop offsets. Those three readers are raw field reads at fixed
    // Aprop_C offsets, and most of what a player can aim at is not an Aprop_C -- a door, the ATV, a
    // generator, a keyhole all answer lookAt() -- so without this gate the bytes read are somebody
    // else's state, or past the end of a smaller object.
    if (R::IsLive(actor) && ue_wrap::prop::IsDescendantOfProp(actor)) {
        g_cur.propFrozen   = ue_wrap::prop::IsFrozen(actor);
        g_cur.propStatic   = ue_wrap::prop::IsStatic(actor);
        g_cur.propSleeping = ue_wrap::prop::IsSleeping(actor);
        g_cur.havePropState = true;
    }
    StartSteadyWindow(nowMs);
    ++g_episodes;
    if (actor == g_lastEpisodeActor && actor != nullptr) ++g_flipBacks;
    if (g_lines < kMaxLines) {
        ++g_lines;
        UE_LOGW("lookat_churn_probe: AIM START '%ls' key='%ls'%hs", g_cur.cls.c_str(),
                g_cur.key.c_str(),
                (actor == g_lastEpisodeActor) ? " -- the SAME actor the aim just left" : "");
    }
}

void NoteGap(uint64_t nowMs) {
    const uint64_t gapMs    = nowMs - g_cur.lastChangeMs;
    const uint64_t gapTicks = g_tick - g_cur.lastChangeTick;
    if (g_cur.gaps == 0 || gapMs < g_cur.minGapMs) g_cur.minGapMs = gapMs;
    if (g_cur.gaps == 0 || gapMs > g_cur.maxGapMs) g_cur.maxGapMs = gapMs;
    if (g_cur.gaps == 0 || gapTicks < g_cur.minGapTicks) g_cur.minGapTicks = gapTicks;
    if (g_cur.gaps == 0 || gapTicks > g_cur.maxGapTicks) g_cur.maxGapTicks = gapTicks;
    g_cur.sumGapMs += gapMs;
    ++g_cur.gaps;
    g_cur.lastChangeMs   = nowMs;
    g_cur.lastChangeTick = g_tick;
}

void LogChange(const E::MainPlayerLookAt& now, uint64_t gapMs, uint64_t gapTicks) {
    if (g_changeLines >= kMaxLines) return;
    ++g_changeLines;
    char what[80];
    std::snprintf(what, sizeof(what), "%hs%hs%hs%hs",
                  now.component     != g_prev.component     ? "component " : "",
                  now.boundsReplace != g_prev.boundsReplace ? "bounds "    : "",
                  now.verify        != g_prev.verify        ? "verify "    : "",
                  now.state         != g_prev.state         ? "state"      : "");
    UE_LOGW("lookat_churn_probe: CHANGE on '%ls' key='%ls' -- %hs moved, %llu ms (%llu tick(s)) "
            "after the last one | state %u->%u verify %u->%u component %p->%p bounds %p->%p",
            g_cur.cls.c_str(), g_cur.key.c_str(), what,
            static_cast<unsigned long long>(gapMs), static_cast<unsigned long long>(gapTicks),
            g_prev.state, now.state, g_prev.verify, now.verify,
            g_prev.component, now.component, g_prev.boundsReplace, now.boundsReplace);
}

// The watch's own callback. Counting only: the body always runs, and nothing here touches the
// widget or the player.
const Watched* ByTag(int tag) {
    for (const Watched& w : kWatched)
        if (w.tag == tag) return &w;
    return nullptr;
}

sg::Verdict OnWatched(const sg::Call& call) {
    const Watched* w = ByTag(call.tag);
    if (!w) return sg::Verdict::Run;
    g_any = true;
    if (w->tag == kWatched[0].tag) {
        ++g_builds;
        if (g_cur.actor) { ++g_cur.builds; ++g_cur.sBuilds; }
        return sg::Verdict::Run;   // the count is the reading; the caller is the next watch's line
    }
    // Both counted BEFORE the line cap, because the compare watch below reads them as a delta
    // across a call: a counter that stopped when the printing did would make every later call look
    // like one that never rebuilt.
    if (w->tag == kWatched[1].tag) ++g_lists;
    if (w->tag == kWatched[2].tag) ++g_dropped;
    if (g_buildLines >= kMaxLines) return sg::Verdict::Run;
    ++g_buildLines;
    // Read the set AT THE SEAM, not once a frame. LookAtFunction stores the fresh trace and then
    // asks for the rebuild, so this sample is what it just decided differed -- and a field that
    // moves and moves back inside one frame, which a per-tick sample cannot see, shows up here as
    // a difference from the previous rebuild.
    std::wstring seam = L"<unreadable>";
    if (void* pl = coop::players::Registry::Get().Local()) {
        E::MainPlayerLookAt at{};
        if (E::ReadMainPlayerLookAt(pl, at)) {
            wchar_t buf[220];
            std::swprintf(buf, 220,
                          L"actor=%p%hs component=%p%hs bounds=%p%hs verify=%u%hs state=%u%hs",
                          at.actor,         at.actor != g_seam.actor ? " MOVED" : "",
                          at.component,     at.component != g_seam.component ? " MOVED" : "",
                          at.boundsReplace, at.boundsReplace != g_seam.boundsReplace ? " MOVED" : "",
                          at.verify,        at.verify != g_seam.verify ? " MOVED" : "",
                          at.state,         at.state != g_seam.state ? " MOVED" : "");
            seam = buf;
            g_seam = at;
        }
    }
    UE_LOGW("lookat_churn_probe: at the seam -- %ls", seam.c_str());
    // The caller frame is the whole point of the line: buildActionList has several call sites, and
    // which one fired says whether the rebuild came from the look-at trace, from an input path, or
    // from the aim being dropped. Both fields are null when the call arrived through ProcessEvent
    // rather than from a Blueprint body.
    const std::wstring cf = call.callerFunction ? R::ToString(R::NameOf(call.callerFunction)) : L"<none>";
    const std::wstring co = call.callerObject ? R::ClassNameOf(call.callerObject) : L"<none>";
    UE_LOGW("lookat_churn_probe: %hs -- from '%ls::%ls', aim on '%ls' (rebuilds so far %u, aims "
            "dropped %u)", w->what, co.c_str(), cf.c_str(),
            g_cur.actor ? g_cur.cls.c_str() : L"<nothing>", g_builds, g_dropped);
    return sg::Verdict::Run;
}

// ---- the compare, read at the moment it is made ---------------------------------------------

// One body's snapshot, taken in PRE and consumed in POST. Paired by FRAME IDENTITY, never by a
// counter: a pre/post counter is an assertion about the gate's control flow that the gate does not
// owe us -- another consumer's Cancel and a fault the ProcessEvent firewall absorbs both skip the
// post -- so the pairing is the frame pointer, and "am I nested?" is asked of the gate's own RAII
// scope, which unwinds on every path out.
struct CompareSlot {
    void*               stack  = nullptr;   // the FFrame this snapshot belongs to
    void*               object = nullptr;
    E::MainPlayerLookAt stored{};           // the compare's stored operand, before the body ran
    unsigned            listsAtEntry = 0;
    unsigned            dropsAtEntry = 0;
};
CompareSlot g_slot;

unsigned g_cmpBodies   = 0;  // LookAtFunction bodies seen whole (pre and post both ran)
unsigned g_cmpRebuilt  = 0;  // ... of which asked for a rebuild, the aim-drop route excluded
unsigned g_cmpAgreed   = 0;  // of the REBUILT ones, all five pairs equal: not this compare's doing
unsigned g_cmpNoStore  = 0;  // of the REBUILT ones, the body left the stored set as it found it
// Bodies that dropped the aim AND rebuilt: their locals are not the compare's operands. A drop that
// somehow did NOT rebuild is not counted here, which costs nothing only because lookAtLookedAway
// always calls buildActionList (mainPlayer.cpp:20273) -- true of the current cook, not by
// construction, so read this as "dropped and rebuilt", never as "dropped".
unsigned g_cmpDropPath = 0;
unsigned g_cmpNested   = 0;  // a LookAtFunction body inside another one (expected: 0)
unsigned g_cmpOrphan   = 0;  // a snapshot whose post never fired, replaced rather than left to stick
unsigned g_dActor = 0, g_dComp = 0, g_dBound = 0, g_dNum = 0, g_dState = 0;
int      g_cmpLines = 0;
bool     g_localsRead = false;   // the five locals resolved at least once
bool     g_localsFail = false;   // ... or did not, which a zero must not be read as agreement

sg::Verdict OnComparePre(const sg::Call& call) {
    // Nesting is asked FIRST, and of the gate's own scope rather than a flag of ours: at PRE the
    // scope for THIS call is not pushed yet, so a true answer means an OUTER LookAtFunction body is
    // running -- on any pawn -- and its snapshot must survive untouched.
    if (sg::IsBodyActive(call.function)) { ++g_cmpNested; return sg::Verdict::Run; }
    // Nothing of ours is open, so whatever is still in the slot belongs to a body that ended
    // without its post (another consumer cancelled it, or the firewall absorbed a fault) and can
    // never be consumed. It is counted and dropped HERE, before the pawn test, because an FFrame
    // address is stack memory and recurs: left in place, a stale snapshot could be matched by a
    // later body at the same address on the same pawn once the local pawn had changed underneath
    // it, and the reading would be built from another body's operands.
    if (g_slot.stack) { ++g_cmpOrphan; g_slot = CompareSlot{}; }
    // The local pawn only. A puppet is a mainPlayer_C too and runs this same body, and its look-at
    // set is nobody's UI.
    if (!call.object || call.object != coop::players::Registry::Get().Local())
        return sg::Verdict::Run;
    E::MainPlayerLookAt stored{};
    if (!E::ReadMainPlayerLookAt(call.object, stored)) return sg::Verdict::Run;
    g_slot.stored       = stored;
    g_slot.stack        = call.stack;
    g_slot.object       = call.object;
    g_slot.listsAtEntry = g_lists;
    g_slot.dropsAtEntry = g_dropped;
    g_any = true;
    return sg::Verdict::Run;
}

void OnComparePost(const sg::Call& call) {
    if (!g_slot.stack || g_slot.stack != call.stack || g_slot.object != call.object) return;
    const CompareSlot s = g_slot;
    g_slot = CompareSlot{};   // consumed: the next body takes its own snapshot

    E::MainPlayerLookAtLocals fresh{};
    if (!E::ReadMainPlayerLookAtLocals(call.function, call.locals, fresh)) {
        g_localsFail = true;
        return;
    }
    g_localsRead = true;
    ++g_cmpBodies;

    const bool rebuilt = (g_lists != s.listsAtEntry);
    const bool dropped = (g_dropped != s.dropsAtEntry);
    if (!rebuilt) return;            // the compare agreed, or the body returned before reaching it
    if (dropped) { ++g_cmpDropPath; return; }  // the aim-drop route: these locals never held the operands
    ++g_cmpRebuilt;

    const bool dA = fresh.actor       != s.stored.actor;
    const bool dC = fresh.component   != s.stored.component;
    const bool dB = fresh.boundObject != s.stored.boundsReplace;
    const bool dN = fresh.number      != s.stored.verify;
    const bool dS = fresh.stateByte   != s.stored.state;
    if (dA) ++g_dActor;
    if (dC) ++g_dComp;
    if (dB) ++g_dBound;
    if (dN) ++g_dNum;
    if (dS) ++g_dState;
    if (!dA && !dC && !dB && !dN && !dS) ++g_cmpAgreed;

    // And what the body LEFT. The stored set is written back inside this same call, so an
    // unchanged set after a failed compare means the store did not run -- which is the difference
    // between "a field moves every tick" and "the fresh answer never matches what is kept", and
    // the per-tick sampling of every earlier reading could not tell those apart.
    E::MainPlayerLookAt after{};
    const bool haveAfter = E::ReadMainPlayerLookAt(call.object, after);
    const bool storedMoved = haveAfter &&
        (after.actor != s.stored.actor || after.component != s.stored.component ||
         after.boundsReplace != s.stored.boundsReplace || after.verify != s.stored.verify ||
         after.state != s.stored.state);
    if (haveAfter && !storedMoved) ++g_cmpNoStore;

    if (g_cmpLines >= kMaxLines) return;
    ++g_cmpLines;
    UE_LOGW("lookat_churn_probe: COMPARE failed on %hs%hs%hs%hs%hs%hs| fresh actor=%p component=%p "
            "bound=%p number=%u state=%u | stored actor=%p component=%p bounds=%p verify=%u "
            "state=%u | the body left the stored set %hs",
            dA ? "actor " : "", dC ? "component " : "", dB ? "bound " : "", dN ? "number " : "",
            dS ? "state " : "", (!dA && !dC && !dB && !dN && !dS) ? "NOTHING " : "",
            fresh.actor, fresh.component, fresh.boundObject, fresh.number, fresh.stateByte,
            s.stored.actor, s.stored.component, s.stored.boundsReplace, s.stored.verify,
            s.stored.state,
            !haveAfter ? "unreadable" : storedMoved ? "CHANGED" : "exactly as it found it");
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::lookat_churn_probe);
    return s;
}

void Tick() {
    if (!IsEnabled()) return;
    ++g_tick;
    if (!g_watchAsked) {
        // The session's hold keeps the gate running for every tick this probe runs (it ticks inside
        // TickGameplay only). Registration is cheap and idempotent; the watches stay inert until the
        // gate has resolved the names on the game thread, which is why the verdict prints their
        // liveness.
        g_watchAsked = true;
        for (const Watched& w : kWatched)
            if (!sg::WatchName(w.name, w.tag, &OnWatched, nullptr)) g_watchAsked = false;
        if (!sg::WatchName(kCompareFn, kCompareTag, &OnComparePre, &OnComparePost))
            g_watchAsked = false;
    }
    if (g_firstMs == 0) g_firstMs = NowMs();
    void* player = coop::players::Registry::Get().Local();
    E::MainPlayerLookAt now{};
    if (!player || !E::ReadMainPlayerLookAt(player, now)) {
        // No possessed pawn, or the BP class has not loaded: the run has no reading here, and the
        // episode that was open ended when the pawn went away.
        if (g_cur.actor) CloseEpisode(NowMs());
        g_havePrev = false;
        return;
    }
    const ue_wrap::FRotator cam = E::GetCameraRotation();
    if (!g_havePrev) { g_prev = now; g_prevCam = cam; g_havePrev = true; }

    const uint64_t nowMs = NowMs();
    // The period runs off the same clock read, and from every tick rather than from a change: an
    // aim that holds perfectly still produces no changes at all, and that silence is the reading
    // the other peer is compared against.
    if (g_any && (g_lastVerdictMs == 0 || nowMs - g_lastVerdictMs >= kVerdictPeriodMs)) EmitVerdict();

    // The camera is never perfectly still even with nobody touching it -- the player's idle sway
    // moves it a fraction of a degree per frame -- so a bit-exact compare would call every tick a
    // turn and leave no steady window at all (measured: 296 "moves" in 4.9 s of a held aim). A
    // deliberate turn is orders of magnitude faster than the sway.
    float dYaw = std::fmod(cam.Yaw - g_prevCam.Yaw + 540.f, 360.f) - 180.f;  // the short way round
    if (std::fabs(dYaw) + std::fabs(cam.Pitch - g_prevCam.Pitch) > 1.0f) {
        // The player is looking around. Whatever the trace answers now is a new question, not a new
        // answer to the old one, so the steady window restarts here -- after banking the one that
        // just ended, which may still be the widest of the run.
        g_prevCam = cam;
        if (g_cur.actor) {
            ConsiderBest(g_cur, nowMs);
            ++g_cur.camMoves;
            StartSteadyWindow(nowMs);
        }
    }

    if (g_cur.actor && g_cur.havePropState && R::IsLive(g_cur.actor)) {
        const bool fz = ue_wrap::prop::IsFrozen(g_cur.actor);
        const bool st = ue_wrap::prop::IsStatic(g_cur.actor);
        const bool sl = ue_wrap::prop::IsSleeping(g_cur.actor);
        if (fz != g_cur.propFrozen || st != g_cur.propStatic || sl != g_cur.propSleeping) {
            ++g_cur.nPropState;
            if (g_changeLines < kMaxLines) {
                ++g_changeLines;
                UE_LOGW("lookat_churn_probe: the aimed prop's own state moved on '%ls' key='%ls' -- "
                        "frozen %d->%d static %d->%d sleeping %d->%d (this is what its lookAt() "
                        "answers from)", g_cur.cls.c_str(), g_cur.key.c_str(),
                        g_cur.propFrozen ? 1 : 0, fz ? 1 : 0, g_cur.propStatic ? 1 : 0, st ? 1 : 0,
                        g_cur.propSleeping ? 1 : 0, sl ? 1 : 0);
            }
            g_cur.propFrozen = fz; g_cur.propStatic = st; g_cur.propSleeping = sl;
        }
    }

    if (now.actor != g_prev.actor) {
        // A new target, or none. The rebuild the player sees happens either way; what tells them
        // apart is whether the aim came back to the same actor, which CloseEpisode/OpenEpisode
        // record between them.
        if (g_cur.actor) CloseEpisode(nowMs);
        if (now.actor) OpenEpisode(now.actor, nowMs);
        ++g_changes;
        g_any = true;
        g_prev = now;
        return;
    }
    if (now.component == g_prev.component && now.boundsReplace == g_prev.boundsReplace &&
        now.verify == g_prev.verify && now.state == g_prev.state) {
        return;  // the steady state: four compares and out
    }
    // The aim is on the same actor and the stored set still moved -- one rebuild of the action list
    // and one re-open of the hovertext.
    ++g_changes;
    g_any = true;
    if (g_cur.actor) {
        const uint64_t gapMs    = nowMs - g_cur.lastChangeMs;
        const uint64_t gapTicks = g_tick - g_cur.lastChangeTick;
        ++g_cur.changes;
        ++g_cur.sChanges;
        if (now.component     != g_prev.component)     ++g_cur.nComp;
        if (now.boundsReplace != g_prev.boundsReplace) ++g_cur.nBounds;
        if (now.verify        != g_prev.verify)        ++g_cur.nVerify;
        if (now.state         != g_prev.state)         ++g_cur.nState;
        if (now.state == 0xFF)                         ++g_cur.nBlind;
        LogChange(now, gapMs, gapTicks);
        NoteGap(nowMs);
    }
    g_prev = now;
}

void EmitVerdict() {
    if (!IsEnabled() || !g_any) return;
    g_lastVerdictMs = NowMs();
    // The live episode counts too: a rig peer is killed while still aiming, so the widest window of
    // the run is usually the one that never ended.
    Episode best = g_best;
    if (g_cur.actor) {
        Episode live = g_cur;
        live.endMs = NowMs();
        if (live.SteadyMs() > best.SteadyMs()) best = live;
    }
    bool live = true;
    for (const Watched& w : kWatched)
        if (!sg::NameWatchLive(w.name, w.tag)) live = false;
    const bool cmpLive = sg::NameWatchLive(kCompareFn, kCompareTag);
    const uint64_t runMs = (g_firstMs != 0 && NowMs() > g_firstMs) ? NowMs() - g_firstMs : 0;
    UE_LOGW("lookat_churn_probe: over %llu s of this run the action row was rebuilt %u time(s) "
            "(%.2f/s) and the aim was dropped %u time(s); %u of the %u aims went straight back to "
            "the actor they had just left",
            static_cast<unsigned long long>(runMs / 1000), g_builds,
            runMs ? static_cast<double>(g_builds) * 1000.0 / static_cast<double>(runMs) : 0.0,
            g_dropped, g_flipBacks, g_episodes);
    UE_LOGW("lookat_churn_probe: VERDICT ticks=%llu aims=%u changes=%u returns-to-the-actor-just-"
            "left=%u rebuilds=%u (watch %hs) | the stillest aim was '%ls' key='%ls', camera still "
            "for %llu ms, and in that window %u change(s) and %u rebuild(s)",
            static_cast<unsigned long long>(g_tick), g_episodes, g_changes, g_flipBacks, g_builds,
            live ? "LIVE" : "NOT LIVE -- a zero rebuild count here means nothing",
            best.cls.empty() ? L"<none>" : best.cls.c_str(), best.key.c_str(),
            static_cast<unsigned long long>(best.SteadyMs()), best.sChanges, best.sBuilds);
    if (best.nPropState > 0)
        UE_LOGW("lookat_churn_probe: the aimed prop's OWN state moved %u time(s) in that window -- "
                "frozen/static/sleeping is what Aprop_C::lookAt answers from, so each move flips "
                "the bool LookAtFunction gates its rebuild on", best.nPropState);
    if (best.sChanges > 0)
        UE_LOGW("lookat_churn_probe: inside that window -- component=%u bounds=%u verify=%u "
                "state=%u (of which the trace hit NOTHING %u times) | gap min=%llu ms max=%llu ms "
                "mean=%llu ms, in ticks min=%llu max=%llu",
                best.nComp, best.nBounds, best.nVerify, best.nState, best.nBlind,
                static_cast<unsigned long long>(best.minGapMs),
                static_cast<unsigned long long>(best.maxGapMs),
                static_cast<unsigned long long>(best.gaps ? best.sumGapMs / best.gaps : 0),
                static_cast<unsigned long long>(best.minGapTicks),
                static_cast<unsigned long long>(best.maxGapTicks));
    // What the numbers decide. Each branch claims only what was counted: the probe sees the rebuild,
    // never its cause, so the reading names the field that moved and leaves the seam to the lane
    // that owns it. The window's first frame is entitled to one rebuild -- it is the UI opening --
    // so the quiet case is one, not zero.
    const char* reading =
        (best.SteadyMs() < kHeldMs)
            ? "NO HELD AIM -- nothing stayed under this peer's crosshair, with the camera still, "
              "for two seconds. This run says nothing about the flicker"
        : (best.sBuilds > best.sChanges + 1)
            ? "THE UI IS REBUILT WITHOUT THE STORED SET MOVING -- the row was rebuilt more often "
              "than this probe saw the five fields change. The store runs BEFORE the rebuild, so "
              "these samples are the fresh values either way: something is writing one of the five "
              "fields between one rebuild's store and the next one's compare"
        : (best.sChanges == 0)
            ? "THE AIM HELD -- the look-at set stood still for the whole window and the action row "
              "was not rebuilt under it, so this peer showed no flicker here"
        : (best.nBlind > 0)
            ? "THE TRACE LOST ITS TARGET -- the state byte took its nothing-was-hit value while the "
              "SAME actor stayed the stored one, so the rebuild is a missed trace frame, not a "
              "changed interactable. The gaps above are the period to name the timer"
        : (best.nComp > 0 || best.nBounds > 0)
            ? "THE HIT COMPONENT CHANGED -- the aimed actor never moved and neither did the camera, "
              "and the component the trace answered with did, which is a collider being replaced or "
              "re-registered underneath a held aim"
            : "THE ACTOR'S OWN ANSWER CHANGED -- the trace kept naming the same actor and the same "
              "component, and the byte its lookAt() returns moved. For a food that byte is its "
              "`uses`, so the actor's own state is being rewritten under the aim";
    UE_LOGW("lookat_churn_probe: reading :: %hs", reading);

    // The compare's own reading. Kept apart from the one above because it answers a different
    // question: that one says whether the row was rebuilt without the stored set appearing to
    // move, this one says WHICH of the five pairs disagreed when it was.
    UE_LOGW("lookat_churn_probe: the compare (watch %hs, locals %hs) -- %u whole LookAtFunction "
            "bodies, %u of them rebuilt the row; the disagreeing operand was actor=%u "
            "component=%u bound=%u number=%u state=%u, all five agreed %u time(s), and the body "
            "left the stored set untouched %u time(s) | aim-drop route %u, nested %u, orphaned %u",
            cmpLive ? "LIVE" : "NOT LIVE -- these counts mean nothing",
            g_localsFail ? "UNREADABLE -- a local did not resolve, so a zero here is not agreement"
                         : g_localsRead ? "readable" : "never read",
            g_cmpBodies, g_cmpRebuilt, g_dActor, g_dComp, g_dBound, g_dNum, g_dState, g_cmpAgreed,
            g_cmpNoStore, g_cmpDropPath, g_cmpNested, g_cmpOrphan);
    if (cmpLive && !g_localsFail && g_cmpRebuilt > 0) {
        // Name the field, and nothing beyond it. Which lane writes that field is the next
        // question, and it is answered by grepping for the field, not by this probe guessing.
        unsigned top = g_dActor; const char* which = "lookAtActor vs the trace's hit actor";
        if (g_dComp  > top) { top = g_dComp;  which = "lookAtComponent vs the trace's hit component"; }
        if (g_dBound > top) { top = g_dBound; which = "lookAtBoundsReplace vs the bound object the aimed actor named"; }
        if (g_dNum   > top) { top = g_dNum;   which = "lookAtVerify vs the byte the aimed actor's lookAt() returned"; }
        if (g_dState > top) { top = g_dState; which = "lookAtState vs BitsToByte(!activeInterface)"; }
        if (top == 0)
            UE_LOGW("lookat_churn_probe: compare reading :: THE ROW WAS REBUILT WHILE ALL FIVE "
                    "PAIRS AGREED -- so the rebuild did not come from this comparison at all, and "
                    "the caller frame lines above name the route that asked for it");
        else
            UE_LOGW("lookat_churn_probe: compare reading :: THE PAIR THAT DISAGREED IS %hs, on %u "
                    "of %u rebuilt bodies. That field is the one to grep for a writer",
                    which, top, g_cmpRebuilt);
    }
}

void OnDisconnect() {
    EmitVerdict();
    g_prev = E::MainPlayerLookAt{};
    g_seam = E::MainPlayerLookAt{};
    g_prevCam = ue_wrap::FRotator{};
    g_havePrev = false;
    g_cur = Episode{};
    g_best = Episode{};
    g_lastEpisodeActor = nullptr;
    g_flipBacks = g_episodes = g_changes = 0;
    g_builds = g_lists = g_dropped = 0;
    g_slot = CompareSlot{};
    g_cmpBodies = g_cmpRebuilt = g_cmpAgreed = g_cmpNoStore = 0;
    g_cmpDropPath = g_cmpNested = g_cmpOrphan = 0;
    g_dActor = g_dComp = g_dBound = g_dNum = g_dState = 0;
    g_cmpLines = 0;
    g_localsRead = g_localsFail = false;
    g_firstMs = 0;
    g_lines = g_changeLines = g_buildLines = 0;
    g_tick = 0;
    g_any = false;
    g_lastVerdictMs = 0;
}

}  // namespace coop::dev::lookat_churn_probe
