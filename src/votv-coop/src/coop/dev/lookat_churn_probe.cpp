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

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::lookat_churn_probe);
    return s;
}

void Tick() {
    if (!IsEnabled()) return;
    ++g_tick;
    if (!g_watchAsked) {
        // The gate is a session lane's to enable, and this probe can run outside one, so it asserts
        // its own. Registration is cheap and idempotent; the watches stay inert until the gate has
        // resolved the names on the game thread, which is why the verdict prints their liveness.
        sg::SetEnabled(true);
        g_watchAsked = true;
        for (const Watched& w : kWatched)
            if (!sg::WatchName(w.name, w.tag, &OnWatched, nullptr)) g_watchAsked = false;
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
    g_builds = g_dropped = 0;
    g_firstMs = 0;
    g_lines = g_changeLines = g_buildLines = 0;
    g_tick = 0;
    g_any = false;
    g_lastVerdictMs = 0;
}

}  // namespace coop::dev::lookat_churn_probe
