// coop/dev/clean_drill.cpp -- see coop/dev/clean_drill.h. The tick, and the `clean` arm: a client cleans away one decal of each
// grime kind and every peer says whether its copy ended. The `leaves` arm is clean_drill_leaves.cpp, the shared reads, marks and
// verdict clean_drill_common.cpp.
//
// The `clean` arm, in the order the peers do it, three peers (host, first client, second client), each step on an event:
//   every peer: once its world is up and the grime lane has indexed it, it marks itself up and waits for the others' marks.
//   the ACTOR (the first client; VOTVCOOP_CLEAN_ACTOR=host: the host) picks, around the save's bucket, the decal of each grime
//   kind that stands nearest it and one more cleanable, rain-proof decal (the SENTINEL), and marks them. Every other peer finds
//   each of them standing on its own copy and says so.
//   the actor cleans every target until it ends (a client actor with a sponge in its hand, walking to each decal, strokes it
//   until the host's word ends it; a host actor makes the rain's call form, no sponge), waits until the lane has
//   sent each end, then cleans the sentinel a little: the lane's wire is ordered, so a peer that sees the sentinel fall has
//   applied every end the actor sent before it. Each other peer judges at that event: a target that ended on the actor and
//   stands on this copy fails the run, named. The host says PASS once every peer's copy has ended them all.
// Every peer dumps the grime decals it holds, before and after, for a set compare.

#include "coop/dev/clean_drill.h"

#include "clean_drill_detail.h"   // co-located private header (src tree, not include/)

#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/director/routes.h"
#include "coop/interactables/grime_sync.h"
#include "coop/player/hand_item.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::clean_drill {
namespace {

using namespace detail;
namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
using coop::rig_verdict::Outcome;

constexpr const char* kTag = "grime-clean";
constexpr float    kRadiusCm = 3000.f;   // the targets stand within this of the bucket
constexpr int      kMaxStrokes = 40;     // a decal that does not end in this many is no target
constexpr float    kStrokeSub = 50.f;
constexpr float    kMinProcess = 20.f;   // the sentinel's partial of 10 must leave it standing
constexpr float    kSentinelSub = 10.f;
constexpr float    kFallEps = 0.5f;
constexpr uint32_t kLanePasses = 3;      // the lane's index has settled: each target has its cached key

constexpr const wchar_t* kTargetClasses[] = {L"grime_dyn_C",      L"grime_dusty_C", L"grime_leaky_C",
                                             L"grime_blood2_drip_C", L"grime_coffee_C", L"grime_fallenLeaves_C",
                                             L"grime_grainy_C"};
constexpr int kMaxTargets = static_cast<int>(sizeof(kTargetClasses) / sizeof(kTargetClasses[0]));

// The classes a sentinel is picked from, and the classes the dumps list.
constexpr const wchar_t* kAllClasses[] = {
    L"grime_asoLogoShadow_C", L"grime_beer_C",  L"grime_blood_C",        L"grime_blood2_C",         L"grime_blood2_drip_C",
    L"grime_coffee_C",        L"grime_crack_C", L"grime_dusty_C",        L"grime_dyn_C",            L"grime_explosionScorch_C",
    L"grime_fallenLeaves_C",  L"grime_grainy_C", L"grime_leaky_C",       L"grime_leaky_rusty_C",    L"grime_leaky_wet_C",
    L"grime_light_C",         L"grime_oil_C",   L"grime_poo_C",          L"grime_C"};

struct Target {
    std::wstring cls;
    FVector at{};
    float start = 0.f;
};

enum class Ph { Arrive, Choose, AwaitTargets, AwaitSeen, Clean, Ends, Sentinel, AwaitLine, AwaitDone, Judge, AwaitResults,
                AwaitEnd, Done };
Ph g_ph = Ph::Arrive;
FVector g_anchor{};
std::vector<Target> g_targets;
Target g_sentinel;
uint32_t g_ends0 = 0;
size_t g_ended = 0;
bool g_dumpedAfter = false;

void Go(Ph p, const char* name) {
    g_ph = p;
    Enter(name);
}

// ---- a client actor cleans with a sponge in its hand ------------------------------------------------------------------
// The host judges a client's fall (coop/interactables/grime_judge): a clean with no sponge is the rain's call, which the
// host grants only while it rains, and every fall must come from a body within reach of the decal. So a client actor, at
// each decal, walks to a standpoint about it, THEN spawns and holds a sponge (the director's walk drops whatever the hand
// holds when it starts, so a sponge held on the way would be thrown), waits for the host's record of it, and strokes the decal
// as a sponge does (the call prop_sponge makes, Sub 6, the sponge as its first argument) every 0.3 s until the host's word
// ends it on its copy. A hand that empties mid-work ends the run INVALID at once. The host actor cleans its own decals
// natively, with no sponge.
constexpr float kSpongeSub = 6.f;          // getStrength * 4 without soap: prop_sponge dump line 134
constexpr float kWalkReachCm = 100.f;      // how near a standpoint the walker must read
constexpr int   kWalkDeadlineS = 240;
constexpr auto  kStrokeGap = std::chrono::milliseconds(300);
constexpr const wchar_t* kSpongePath = L"/Game/objects/prop_sponge.prop_sponge_C";
constexpr const wchar_t* kSpongeCls = L"prop_sponge_C";

std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::chrono::steady_clock::time_point g_strokeAt{};
size_t g_ti = 0;   // the target being cleaned
std::vector<FVector> g_ring;   // the standpoints a route reaches about it, shortest route first
size_t g_stand = 0;            // the one being walked to
bool g_ringBuilt = false;
constexpr int   kStandpoints = 3;
constexpr float kStandRingCm = 400.f;   // the decal stays inside the judge's reach: 400 on the floor, its height above it
// Where the actor is at the target: walking to a standpoint, equipping there, waiting for the host's record, stroking.
enum class Sw : uint8_t { Walk, Equip, AwaitRecord, Strokes };
Sw g_sw = Sw::Walk;
int g_equips = 0;              // sponges this actor has put in its hand: the host's record counts the same
int g_recorded = 0;            // the host's count of sponges it saw taken up, as the host last said it
bool g_handHeld = false;       // the host: the record held a sponge at the last tick

bool UsesSponge() { return IsActor() && !IsHost(); }

// A sponge spawned beside the player and taken up as a pickup does; the hand's own item is what a stroke names.
const char* HoldSponge(void* player, void** handItem) {
    void* const cls = ue_wrap::asset_load::LoadObjectByPath(kSpongePath);
    FVector at{};
    if (!cls || !E::TryGetActorLocation(player, at)) return "the sponge class did not load or the player's place is unread";
    void* const sponge = E::SpawnActor(cls, {at.X, at.Y, at.Z + 60.f});
    bool collected = false;
    if (!sponge || !E::CallMainPlayerHoldObject(player, sponge, collected) || !collected)
        return "this client's own sponge could not be spawned and held";
    E::MainPlayerGrabState hand{};
    *handItem = E::ReadMainPlayerGrabState(player, hand) ? hand.holdingActor : nullptr;
    return nullptr;
}

// The item in the actor's hand, null when the hand is empty.
void* HandItem(void* player) {
    E::MainPlayerGrabState hand{};
    return E::ReadMainPlayerGrabState(player, hand) ? hand.holdingActor : nullptr;
}

// One tick of the actor's work on the decal of `cls` at `at`: 1 when it ended on this copy (or, `once`, after one stroke),
// 0 working, -1 the run is over.
int SpongeWork(const std::wstring& cls, const FVector& at, bool once) {
    void* const d = DecalAt(cls, at);
    if (!d || !R::IsLive(d)) {
        g_walk.reset();
        g_ringBuilt = false;
        g_sw = Sw::Walk;
        return 1;
    }
    void* const player = coop::players::Registry::Get().Local();
    const auto fail = [&](const char* fmt) {
        Decide(Outcome::Invalid, fmt, Narrow(cls).c_str());
        g_ph = Ph::AwaitEnd;
        return -1;
    };
    if (g_sw == Sw::Walk) {
        if (!g_walk) {
            // Where the decal stands is no place to walk to: a ceiling decal is metres above the NavMesh, and a route to it
            // is none, so the stage could neither compare routes nor place the walker at the bed. The walk goes to a
            // standpoint about the decal that a route reaches, and the stage then has a goal it can measure.
            if (!g_ringBuilt) {
                g_ring = coop::director::ReachableStandpoints(player, at, kStandRingCm, kStandpoints, kWalkReachCm);
                g_ringBuilt = true;
                g_stand = 0;
                UE_LOGI("[CLN] a route reaches %zu of the %d standpoints about the %ls decal", g_ring.size(), kStandpoints,
                        cls.c_str());
            }
            if (g_stand >= g_ring.size()) return fail("no standpoint about the %s decal could be walked to");
            g_walk = coop::director::StartBackgroundWalk(g_ring[g_stand], kWalkReachCm, kWalkDeadlineS);
            if (!g_walk) return fail("the walk to the %s decal did not start");
        }
        const int st = g_walk->state.load();
        if (st == 2) {   // this standpoint was not reached: the next
            ++g_stand;
            g_walk.reset();
        } else if (st == 1) {
            g_sw = Sw::Equip;
        }
        return 0;
    }
    if (g_sw == Sw::Equip) {
        void* item = nullptr;
        if (const char* why = HoldSponge(player, &item)) {
            Decide(Outcome::Invalid, "%s", why);
            g_ph = Ph::AwaitEnd;
            return -1;
        }
        ++g_equips;
        g_sw = Sw::AwaitRecord;
        return 0;
    }
    if (g_sw == Sw::AwaitRecord) {
        // The first stroke waits for the host's record of this sponge in the hand: its count has reached the actor's own.
        std::string text;
        if (!PeekShared("cl_hand", text)) return 0;
        g_recorded = std::atoi(text.c_str());
        if (g_recorded >= g_equips) g_sw = Sw::Strokes;
        return 0;
    }
    // Strokes. A hand that has emptied (the sponge thrown, taken, dropped) ends the run here, not after strokes the host
    // would refuse for no tool.
    void* const item = HandItem(player);
    if (!item) return fail("the sponge left the actor's hand before the %s decal ended");
    const auto now = std::chrono::steady_clock::now();
    if (now - g_strokeAt < kStrokeGap) return 0;
    g_strokeAt = now;
    // A decal held at zero is asked for its end. The event that moves the actor on is the host's own copy ending (its count of
    // ended targets in the shared marks), not this copy's: a copy the host's end never reaches stands at zero for good, and
    // the verdict on that is the host's line below.
    if (!once && ProcessOf(d) <= 0.01f) {
        std::string hostEnded;
        if (PeekShared("cl_hostend", hostEnded) && static_cast<size_t>(std::atoi(hostEnded.c_str())) > g_ti) {
            g_walk.reset();
            g_ringBuilt = false;
            g_sw = Sw::Walk;
            return 1;
        }
        return 0;
    }
    // A decal held at zero waits for the host's word; a stroke on it would only ask again.
    if (ProcessOf(d) > 0.01f && !Clean(d, item, kSpongeSub)) return fail("the sponge's clean did not run on the %s decal");
    if (once) {
        g_walk.reset();
        g_ringBuilt = false;
        g_sw = Sw::Walk;
        return 1;
    }
    return 0;
}

// ---- the targets on the wire -------------------------------------------------------------------------------------------
// "S,<cls>,x,y,z,process;T,<cls>,x,y,z,process;..." : the sentinel first, then each target.
std::string Pack() {
    std::string s;
    char buf[160];
    auto one = [&](char kind, const Target& t) {
        std::snprintf(buf, sizeof(buf), "%c,%s,%.3f,%.3f,%.3f,%.2f;", kind, Narrow(t.cls).c_str(), t.at.X, t.at.Y, t.at.Z,
                      t.start);
        s += buf;
    };
    one('S', g_sentinel);
    for (const Target& t : g_targets) one('T', t);
    return s;
}

bool Unpack(const std::string& text) {
    g_targets.clear();
    bool sentinel = false;
    for (const std::string& item : Split(text, ';')) {
        if (item.empty()) continue;
        const std::vector<std::string> f = Split(item, ',');
        float x = 0.f, y = 0.f, z = 0.f, p = 0.f;
        if (f.size() != 6 || f[0].size() != 1 || !ToFloat(f[2], x) || !ToFloat(f[3], y) || !ToFloat(f[4], z) ||
            !ToFloat(f[5], p))
            return false;
        Target t;
        t.cls.assign(f[1].begin(), f[1].end());
        t.at = {x, y, z};
        t.start = p;
        if (f[0][0] == 'S') {
            g_sentinel = t;
            sentinel = true;
        } else {
            g_targets.push_back(t);
        }
    }
    return sentinel && !g_targets.empty();
}

// ---- the actor's picks ---------------------------------------------------------------------------------------------------
// The nearest cleanable decal of `cls` to the bucket within kRadiusCm that has dirt and is none of `skip`'s places.
bool NearestOf(const wchar_t* cls, bool needRainProof, const std::vector<Target>& skip, Target& out) {
    float best = kRadiusCm * kRadiusCm;
    bool found = false;
    for (void* d : All(cls)) {
        FVector p{};
        if (!E::TryGetActorLocation(d, p) || !BoolOf(d, L"isCleanable")) continue;
        if (needRainProof && !BoolOf(d, L"resistRain")) continue;
        const float process = ProcessOf(d);
        if (process <= (needRainProof ? kMinProcess : 0.f)) continue;
        bool taken = false;
        for (const Target& s : skip) taken = taken || (s.cls == cls && Dist2(s.at, p) < 1.f);
        const float d2 = Dist2(p, g_anchor);
        if (taken || d2 >= best) continue;
        best = d2;
        out = Target{cls, p, process};
        found = true;
    }
    return found;
}

bool ChooseTargets() {
    g_targets.clear();
    for (const wchar_t* cls : kTargetClasses) {
        Target t;
        if (NearestOf(cls, false, g_targets, t)) g_targets.push_back(t);
        else UE_LOGI("[CLN] no %ls decal with dirt within %.0f cm of the bucket", cls, kRadiusCm);
    }
    bool sentinel = false;
    for (const wchar_t* cls : kAllClasses) {
        Target t;
        if (NearestOf(cls, true, g_targets, t) && (!sentinel || Dist2(t.at, g_anchor) < Dist2(g_sentinel.at, g_anchor))) {
            g_sentinel = t;
            sentinel = true;
        }
    }
    for (size_t i = 0; i < g_targets.size(); ++i)
        UE_LOGI("[CLN] target %zu %ls at (%.1f,%.1f,%.1f) process=%.1f", i, g_targets[i].cls.c_str(), g_targets[i].at.X,
                g_targets[i].at.Y, g_targets[i].at.Z, g_targets[i].start);
    return sentinel && !g_targets.empty();
}

// ---- reads on this peer's copy ---------------------------------------------------------------------------------------
// The targets that stand on this copy, as a text for a verdict; empty when none does.
std::string StandingTargets(int& count) {
    std::string s;
    count = 0;
    for (size_t i = 0; i < g_targets.size(); ++i) {
        void* const d = DecalAt(g_targets[i].cls, g_targets[i].at);
        if (!d) continue;
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%s#%zu at (%.0f,%.0f,%.0f) process=%.2f; ", Narrow(g_targets[i].cls).c_str(), i,
                      g_targets[i].at.X, g_targets[i].at.Y, g_targets[i].at.Z, ProcessOf(d));
        s += buf;
        ++count;
    }
    return s;
}

void DumpAll(const char* when) {
    size_t n = 0;
    for (const wchar_t* cls : kAllClasses)
        for (void* dec : All(cls)) {
            FVector p{};
            if (!E::TryGetActorLocation(dec, p)) continue;
            UE_LOGI("[CLN] all %s %ls %.0f %.0f %.0f %.1f", when, cls, p.X, p.Y, p.Z, ProcessOf(dec));
            ++n;
        }
    UE_LOGI("[CLN] all %s: %zu decals", when, n);
}

// The lane is ready on this peer: its index has settled, so a target's end finds its cached key and is sent.
bool LaneReady() { return coop::grime_sync::Tallies().passes >= kLanePasses; }

// The line the host says on the lobby chat once every target has ended on its copy and the lane has sent each end.
constexpr const char* kHostEndedLine = "clean-drill the host ended every target";
bool g_lineSeen = false;       // a client: the line has arrived
bool g_lineSaid = false;       // the host: said
int  g_hostEnded = -1;         // the host: how many targets have ended on its copy, as last marked

// HOST, with a client as the actor: counts the targets that ended on its own copy (every 100 ms), marks the count when it moves,
// and says the line once all have and the lane's end of each has been sent.
void HostEnds() {
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(100)) return;
    last = now;
    int ended = 0;
    for (const Target& t : g_targets)
        if (!DecalAt(t.cls, t.at)) ++ended;
    if (ended != g_hostEnded) {
        g_hostEnded = ended;
        MarkShared("cl_hostend", std::to_string(ended));
    }
    if (!g_lineSaid && static_cast<size_t>(ended) == g_targets.size() &&
        coop::grime_sync::Tallies().endsSent - g_ends0 >= g_targets.size()) {
        g_lineSaid = true;
        coop::dev::drill_chat::Say(kHostEndedLine);
    }
}

// ---- one tick ----------------------------------------------------------------------------------------------------------
void TickClean(coop::net::Session& s) {
    if (g_ph == Ph::Done) return;
    if (g_ph != Ph::Arrive && Over() && g_ph < Ph::AwaitEnd) g_ph = Ph::AwaitEnd;   // another peer decided: stop the steps
    if (g_ph > Ph::Arrive && Overdue()) return;
    const bool host = IsHost();
    const bool acts = IsActor();
    // The host counts the sponges its record of the first client's hand takes up, each at its rising edge, and says the count:
    // the client actor strokes only once the count has reached the number of sponges it has put in its hand.
    if (host && !acts && g_ph > Ph::Arrive) {
        const bool held = coop::hand_item::HeldClassIs(1, kSpongeCls);
        if (held && !g_handHeld) MarkShared("cl_hand", std::to_string(++g_recorded));
        g_handHeld = held;
    }
    // The host says when every target has ended on its own copy: each ended count is a shared mark (the actor moves on at
    // it), and once all have ended and the lane has sent each end, a line on the lobby chat, which rides the ordered lane the
    // ends were sent on: a client that reads the line has applied every end. The client actor judges its copy at that line.
    if (host && !acts && !g_targets.empty() && g_ph >= Ph::AwaitDone) HostEnds();
    if (!host && !g_lineSeen && g_ph > Ph::Arrive) g_lineSeen = coop::dev::drill_chat::SeenHas(kHostEndedLine);
    std::string text;

    switch (g_ph) {
    case Ph::Arrive: {
        const bool ready = LaneReady() && FindAnchor(g_anchor);
        const int a = Arrive(s, kTag, "cl_up", ready);
        if (a < 0) { g_ph = Ph::Done; return; }
        if (a == 0) return;
        Go(acts ? Ph::Choose : Ph::AwaitTargets, acts ? "the actor's picks" : "the actor's targets");
        return;
    }
    case Ph::Choose:
        if (!ChooseTargets()) {
            Decide(Outcome::Invalid, "the world has no cleanable decal to clean near the bucket and no rain-proof one to end on");
            g_ph = Ph::AwaitEnd;
            return;
        }
        MarkShared("cl_targets", Pack());
        DumpAll("before");
        UE_LOGI("[CLN] the actor marked %zu target(s) and the sentinel %ls at (%.1f,%.1f,%.1f)", g_targets.size(),
                g_sentinel.cls.c_str(), g_sentinel.at.X, g_sentinel.at.Y, g_sentinel.at.Z);
        Go(Ph::AwaitSeen, "every peer finding the targets");
        return;
    case Ph::AwaitTargets: {
        if (!PeekShared("cl_targets", text)) return;
        if (!Unpack(text)) {
            Decide(Outcome::Invalid, "the actor's targets could not be read: '%s'", text.c_str());
            g_ph = Ph::AwaitEnd;
            return;
        }
        // Each decal of the actor's picks must stand on this copy: one that does not is a difference of the saves.
        std::string missing;
        for (size_t i = 0; i < g_targets.size(); ++i)
            if (!DecalAt(g_targets[i].cls, g_targets[i].at)) missing += std::to_string(i) + " ";
        if (!DecalAt(g_sentinel.cls, g_sentinel.at)) missing += "sentinel";
        DumpAll("before");
        if (!missing.empty()) {
            Decide(Outcome::Invalid, "the %s does not hold the actor's decal(s) %s at their places", SlotName(MySlot()),
                   missing.c_str());
            g_ph = Ph::AwaitEnd;
            return;
        }
        g_ends0 = coop::grime_sync::Tallies().endsSent;   // the host counts the ends its lane sends from here
        MarkMine("cl_seen", "ok");
        Go(Ph::AwaitDone, "the actor's cleaning");
        return;
    }
    case Ph::AwaitSeen: {
        for (int slot = 0; slot < kPeers; ++slot)
            if (slot != MySlot() && !PeekOf("cl_seen", slot, text)) return;
        g_ti = 0;
        g_ended = 0;
        Go(Ph::Clean, "the cleaning");
        return;
    }
    case Ph::Clean: {
        if (UsesSponge()) {
            if (g_ti >= g_targets.size()) {
                Go(Ph::Sentinel, "the sentinel");
                return;
            }
            const int w = SpongeWork(g_targets[g_ti].cls, g_targets[g_ti].at, /*once=*/false);
            if (w == 1) {
                UE_LOGI("[CLN] target %zu %ls ended on this copy by the host's word", g_ti, g_targets[g_ti].cls.c_str());
                ++g_ended;
                ++g_ti;
                Enter("the cleaning");   // each target has its own bound
            }
            return;
        }
        g_ends0 = coop::grime_sync::Tallies().endsSent;
        g_ended = 0;
        for (size_t i = 0; i < g_targets.size(); ++i) {
            void* const d = DecalAt(g_targets[i].cls, g_targets[i].at);
            int strokes = 0;
            while (d && strokes < kMaxStrokes && R::IsLive(d) && Clean(d, nullptr, kStrokeSub)) ++strokes;
            const bool ended = !d || !R::IsLive(d);
            UE_LOGI("[CLN] target %zu %ls cleaned in %d stroke(s): ended=%d", i, g_targets[i].cls.c_str(), strokes,
                    ended ? 1 : 0);
            if (!ended) {
                Decide(Outcome::Invalid, "target %zu (%s) did not end in %d strokes", i, Narrow(g_targets[i].cls).c_str(),
                       kMaxStrokes);
                g_ph = Ph::AwaitEnd;
                return;
            }
            ++g_ended;
        }
        Go(Ph::Ends, "the lane sending each end");
        return;
    }
    case Ph::Ends:
        // The decals' ends are sent when the engine's end of play is drained, after the clean calls: the sentinel waits for
        // them, so that its fall is the last word of the actor on the wire.
        if (coop::grime_sync::Tallies().endsSent - g_ends0 < g_ended) return;
        Go(Ph::Sentinel, "the sentinel");
        return;
    case Ph::Sentinel: {
        void* const d = DecalAt(g_sentinel.cls, g_sentinel.at);
        const float before = ProcessOf(d);
        bool ran = false;
        if (UsesSponge()) {
            // The sentinel's one stroke is the sponge's, from beside it; the lane's order puts the host's ends first.
            const int w = d ? SpongeWork(g_sentinel.cls, g_sentinel.at, /*once=*/true) : -1;
            if (w == 0) return;
            if (w < 0 && g_ph == Ph::AwaitEnd) return;
            ran = w == 1;
        } else {
            ran = d && Clean(d, nullptr, kSentinelSub);
        }
        if (!ran || ProcessOf(d) >= before - kFallEps) {
            Decide(Outcome::Invalid, "the sentinel %s did not fall by a clean of %.0f", Narrow(g_sentinel.cls).c_str(),
                   kSentinelSub);
            g_ph = Ph::AwaitEnd;
            return;
        }
        UE_LOGI("[CLN] the sentinel fell %.2f -> %.2f after the %zu end(s)", before, ProcessOf(d), g_ended);
        MarkShared("cl_done", "ok");
        g_ph = UsesSponge() ? Ph::AwaitLine : Ph::Judge;   // a client actor judges its copy at the host's line; a host actor's own ended them
        return;
    }
    case Ph::AwaitLine:
        // The client actor's copy is judged where it cannot change any more: at the host's line, which follows the host's
        // own end of every target on the ordered lane, so a copy standing then is one the host's end never reached.
        if (!g_lineSeen) return;
        g_ph = Ph::Judge;
        return;
    case Ph::AwaitDone: {
        if (!PeekShared("cl_done", text)) return;
        void* const sd = DecalAt(g_sentinel.cls, g_sentinel.at);
        if (sd && ProcessOf(sd) > g_sentinel.start - kFallEps) return;   // the sentinel's fall has not come yet
        g_ph = Ph::Judge;
        return;
    }
    case Ph::Judge: {
        int stood = 0;
        const std::string names = StandingTargets(stood);
        char res[300];
        if (stood > 0) {
            std::snprintf(res, sizeof(res), "F %d of the actor's %zu cleaned decal(s) stand on the %s: %s", stood,
                          g_targets.size(), SlotName(MySlot()), names.c_str());
            MarkMine("cl_res", res);
            Decide(Outcome::Fail, "%d of the %zu decal(s) the actor cleaned away stand on the %s: %s", stood,
                   g_targets.size(), SlotName(MySlot()), names.c_str());
            g_ph = Ph::AwaitEnd;
            return;
        }
        MarkMine("cl_res", "P");
        UE_LOGI("[CLN] every one of the %zu cleaned decal(s) has ended on the %s", g_targets.size(), SlotName(MySlot()));
        Go(host ? Ph::AwaitResults : Ph::AwaitEnd, host ? "the peers' results" : "the verdict");
        return;
    }
    case Ph::AwaitResults: {
        for (int slot = 1; slot < kPeers; ++slot) {
            if (!PeekOf("cl_res", slot, text)) return;
            if (text[0] == 'F') {
                Decide(Outcome::Fail, "the %s: %s", SlotName(slot), text.c_str() + 2);
                g_ph = Ph::AwaitEnd;
                return;
            }
        }
        Decide(Outcome::Pass, "the %zu decal(s) the %s cleaned away ended on every peer, and none stands on one alone",
               g_targets.size(), acts ? "host" : "first client");
        g_ph = Ph::AwaitEnd;
        return;
    }
    case Ph::AwaitEnd:
        if (!Over()) return;
        if (!g_dumpedAfter) {
            g_dumpedAfter = true;
            DumpAll("after");
        }
        g_ph = Ph::Done;
        UE_LOGI("[CLN] DONE on the %s", SlotName(MySlot()));
        return;
    case Ph::Done:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    const detail::Arm arm = detail::ArmOf();
    if (arm == detail::Arm::Off || !session || !session->running() || !session->connected()) return;
    if (arm == detail::Arm::Clean) TickClean(*session);
    else if (arm == detail::Arm::Leaves) detail::TickLeaves(*session);
    else detail::TickLeavesJoin(*session);
}

}  // namespace coop::dev::clean_drill
