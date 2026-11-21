// coop/dev/hostile_drill_grime_host.cpp -- the grime family's judge: see coop/dev/hostile_drill_grime.h. The host picks the
// decal the arm needs from the client's puppet and says it in the lobby chat, waits for the client to say it stands ready
// (walked, a sponge in its hand), checks what its own record holds and the weather the arm needs, prepares the decal for
// `last`, says it is ready, and then judges: the judge's counters since the baseline against the arm's table, and the
// value the host holds for the decal (and the one a joiner's snapshot would carry) against what the arm leaves.

#include "hostile_drill_grime_detail.h"  // co-located private header (src tree, not include/)

#include "clean_drill_detail.h"  // Clean: the host's own reflected clean, which the lane sends as a fall

#include "coop/dev/drill_chat.h"
#include "coop/interactables/grime_judge.h"
#include "coop/interactables/grime_sync.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/world/weather_rain.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/grime.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::hostile_drill::grime::detail {
namespace {

namespace DC = coop::dev::drill_chat;
namespace G  = ue_wrap::grime;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace GJ = coop::grime_judge;
namespace GS = coop::grime_sync;
namespace HI = coop::hand_item;

enum class HStep { WaitSlot, Weather, PickDecal, WaitClient, Judge, Rejoin, RejoinJudge, Done };
HStep                 g_step = HStep::WaitSlot;
Clock::time_point     g_armAt{}, g_judgeAt{};
const char*           g_waiting = "slot 1's world-ready";
uint16_t              g_playerNo = 0;     // slot 1's occupant when the host said ready
bool                  g_rainAsked = false;
bool                  g_clientReady = false;
bool                  g_life1Done = false;   // the join-leak arm's first life judged: its decal and bases stand for the second
ue_wrap::CachedObjRef g_decal;
ue_wrap::CachedObjRef g_crack;            // the measure arm's second decal
Target                g_target;
Target                g_crackTarget;
GJ::Stats             g_base{};
float                 g_decalBase = 0.f;  // the decal's process when the host said ready
float                 g_snapBase = 0.f;   // the value a joiner's snapshot would carry for it then
float                 g_strength = 1.f;   // the decal's cleanStrength

void Finish(const Spec& sp, const char* what, const std::string& why) {
    Verdict(sp.arm, what, why);
    g_step = HStep::Done;
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

float ProcessOf(void* decal) {
    float p = -1.f;
    return decal && R::IsLive(decal) && G::ReadProcess(decal, p) ? p : -1.f;
}

float SnapOf(const Target& t, void* decal) {
    float v = -1.f;
    return decal && R::IsLive(decal) && GS::HostValueOf(Wide(t.key), decal, v) ? v : -1.f;
}

// ---- the host's half before the ready line ----

// Slot 1 is world-ready with its join window closed and a posed puppet: the host can place the sender.
bool PuppetReady(coop::net::Session* s) {
    if (!s->IsSlotWorldReady(kSlot)) { g_waiting = "slot 1 to be world-ready"; return false; }
    if (!coop::prop_snapshot::IsBracketClosed(kSlot) || coop::join_window_baseline::IsLateWindowOpen(kSlot)) {
        g_waiting = "slot 1's join window to close";
        return false;
    }
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kSlot);
    if (!rp.valid() || !rp.HasPose()) { g_waiting = "a posed puppet for slot 1"; return false; }
    return true;
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float x = a.X - b.X, y = a.Y - b.Y, z = a.Z - b.Z;
    return std::sqrt(x * x + y * y + z * z);
}

struct PickCtx {
    Pick             pick;
    bool             crack;       // the measure arm's second pick: the nearest crack
    ue_wrap::FVector from;
    float            best;
    void*            actor;
    float            process;
};

void Visit(void* ctx, void* d) {
    auto& c = *static_cast<PickCtx*>(ctx);
    float process = 0.f;
    bool cleanable = false, resist = false;
    ue_wrap::FVector at{};
    if (!G::ReadProcess(d, process) || !G::ReadIsCleanable(d, cleanable) || !G::ReadResistRain(d, resist) ||
        !E::TryGetActorLocation(d, at))
        return;
    const float dist = Dist(at, c.from);
    if (c.crack) {
        if (!G::IsCrack(d)) return;
    } else {
        switch (c.pick) {
        case Pick::Near:  if (!cleanable || G::IsCrack(d) || process < kFreshProcess) return; break;
        case Pick::Far:   if (!cleanable || G::IsCrack(d) || process < kFreshProcess || dist < kFarMinCm) return; break;
        case Pick::Any:   break;
        case Pick::Wash:
            if (resist || !cleanable || process < kFreshProcess || dist > kWideMaxCm || !GJ::RoofClear(d)) return;
            break;
        case Pick::Wide:  if (resist || !cleanable || process < kFreshProcess || dist > kWideMaxCm) return; break;
        }
    }
    if (dist >= c.best) return;
    // The lane must resolve the decal by its key to this one: a place two decals share names the first the index took.
    const std::wstring key = GS::DebugPosKeyForActor(d);
    if (key.empty() || GS::DecalOfKey(key) != d) return;
    c.best = dist;
    c.actor = d;
    c.process = process;
}

// The nearest decal the arm's rule admits to the client's puppet, with its key and place. False when none stands.
bool PickDecal(const Spec& sp, bool crack, Target& out, ue_wrap::CachedObjRef& ref) {
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kSlot);
    PickCtx c{sp.pick, crack, {}, 1e30f, nullptr, 0.f};
    if (!E::TryGetActorLocation(rp.GetActor(), c.from)) return false;
    G::ForEachDecal(&Visit, &c);
    if (!c.actor) return false;
    const std::wstring key = GS::DebugPosKeyForActor(c.actor);
    out.key.assign(key.begin(), key.end());
    E::TryGetActorLocation(c.actor, out.at);
    out.process = c.process;
    ref.Set(c.actor);
    return true;
}

// The weather the arm needs, on the host: a dry arm is invalid in rain, a rain arm starts it. True once it holds.
bool WeatherHolds(const Spec& sp) {
    bool found = false;
    const bool raining = coop::weather_rain::ReadLocalIsRaining(&found) && found;
    if (!found) { g_waiting = "the host's weather to read"; return false; }
    if (sp.weather == Weather::Dry && raining) {
        Finish(sp, "INCONCLUSIVE", "the host is raining, and this arm needs a dry host");
        return false;
    }
    bool winter = false;
    if (sp.weather == Weather::Raining && G::ReadWinter(winter) && winter) {
        Finish(sp, "INCONCLUSIVE", "the season is winter, when the rain's pass cleans nothing");
        return false;
    }
    if (sp.weather == Weather::Raining && !raining) {
        if (!g_rainAsked) {
            g_rainAsked = true;
            coop::weather_rain::DebugForceRain(true, 0.6f);
        }
        g_waiting = "the host's rain to start";
        return false;
    }
    return true;
}

void SayReady(const Spec& sp, void* decal) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", ProcessOf(decal));
    DC::Say(Line(sp.arm, "host-ready") + text);
}

// ---- the wait for the client ----

// The client stands ready: walked, equipped. The host checks its own record, prepares and says ready.
void WaitClient(coop::net::Session* s, const Spec& sp) {
    if (Clock::now() - g_armAt > kFailBound) {
        Finish(sp, "INCONCLUSIVE", std::string("the precondition never held: waiting for ") + g_waiting);
        return;
    }
    std::string why;
    if (DC::SeenRestOf(Line(sp.arm, "client-invalid"), &why)) {
        Finish(sp, "INCONCLUSIVE", "the client: " + why);
        return;
    }
    if (!g_clientReady) {
        g_clientReady = DC::SeenHas(Line(sp.arm, "client-ready"));
        if (!g_clientReady) { g_waiting = "the client's ready line"; return; }
    }
    if (sp.sponge && !HI::HeldClassIs(kSlot, kSpongeCls)) { g_waiting = "the host's record of a sponge in slot 1's hand"; return; }
    void* const decal = g_decal.Get();
    if (!decal || !R::IsLive(decal)) { Finish(sp, "INCONCLUSIVE", "the picked decal was gone before the ready line"); return; }
    if (!G::ReadCleanStrength(decal, g_strength)) { Finish(sp, "INCONCLUSIVE", "the decal's strength did not read"); return; }
    if (sp.arm == Arm::Last) {
        // The host's own clean, which the lane sends as a fall: the client's copy follows it to where one stroke ends it.
        const float sub = (ProcessOf(decal) - kLastLeft) / (1.2f * g_strength);
        if (!coop::dev::clean_drill::detail::Clean(decal, nullptr, sub) || ProcessOf(decal) > kLastLeft + kValueEps) {
            Finish(sp, "INCONCLUSIVE", "the host's own clean did not leave the decal where one stroke ends it");
            return;
        }
    }
    g_base = GJ::GetStats();
    g_decalBase = ProcessOf(decal);
    g_snapBase = SnapOf(g_target, decal);
    g_playerNo = coop::roster_ledger::Get(kSlot).playerNo;
    SayReady(sp, decal);
    g_judgeAt = Clock::now();
    g_step = HStep::Judge;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %u: decal '%s' at %.2f (snapshot value %.2f)", sp.name,
            static_cast<unsigned>(kSlot), g_target.key.c_str(), g_decalBase, g_snapBase);
    (void)s;
}

// ---- the judge ----

// The judge's counters moved since the baseline, in the order the arm tables list them.
enum Field : size_t {
    kAsked, kGranted, kClamped, kEnded, kTool, kRain, kAlready, kKind, kQueue, kNodecal, kNobody, kReach, kNotool,
    kRainRefused, kFields
};
using Tally = uint64_t[kFields];

void Diff(const GJ::Stats& now, Tally out) {
    const GJ::Stats& b = g_base;
    const uint64_t n[kFields] = {now.asked, now.granted, now.clamped, now.ended, now.toolGrants, now.rainGrants, now.already,
                                 now.refusedKind, now.refusedQueue, now.refusedNodecal, now.refusedNobody,
                                 now.refusedReach, now.refusedNotool, now.refusedRain};
    const uint64_t o[kFields] = {b.asked, b.granted, b.clamped, b.ended, b.toolGrants, b.rainGrants, b.already,
                                 b.refusedKind, b.refusedQueue, b.refusedNodecal, b.refusedNobody,
                                 b.refusedReach, b.refusedNotool, b.refusedRain};
    for (size_t i = 0; i < kFields; ++i) out[i] = n[i] >= o[i] ? n[i] - o[i] : 0;
}

const char* kNames[kFields] = {"asked", "granted", "cut", "ended", "tool_grants", "rain_grants", "already", "refused_kind",
                               "refused_queue", "refused_nodecal", "refused_nobody", "refused_reach", "refused_notool",
                               "refused_rain"};

// What each arm's one message must end as: every field not listed stays at 0.
void Want(Arm a, Tally want) {
    for (size_t i = 0; i < kFields; ++i) want[i] = 0;
    want[kAsked] = 1;
    switch (a) {
    case Arm::Far: case Arm::Joinleak: want[kReach] = 1; break;
    case Arm::Notool: case Arm::Dry:   want[kNotool] = 1; break;
    case Arm::Gone:                    want[kKind] = 1; break;
    case Arm::Fast:                    want[kGranted] = 1; want[kClamped] = 1; want[kTool] = 1; break;
    case Arm::Rain:                    want[kGranted] = 1; want[kRain] = 1; break;
    case Arm::Last:                    want[kEnded] = 1; want[kTool] = 1; break;
    default: break;
    }
}

uint64_t Outcomes(const Tally d) {
    return d[kGranted] + d[kEnded] + d[kAlready] + d[kKind] + d[kQueue] + d[kNodecal] + d[kNobody] + d[kReach] +
           d[kNotool] + d[kRainRefused];
}

std::string Summary(const Tally d) {
    std::string out;
    for (size_t i = 0; i < kFields; ++i)
        if (d[i]) out += (out.empty() ? "" : ", ") + std::string(kNames[i]) + "=" + std::to_string(d[i]);
    return out.empty() ? "nothing counted" : out;
}

std::string Mismatch(Arm a, const Tally d) {
    Tally want;
    Want(a, want);
    std::string bad;
    for (size_t i = 0; i < kFields; ++i)
        if (d[i] != want[i])
            bad += std::string(" ") + kNames[i] + " " + std::to_string(d[i]) + " (wanted " + std::to_string(want[i]) + ");";
    return bad;
}

std::string F(float v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", v);
    return b;
}

// What the client's own reports say: `held`/`gone` of `last`, `value` of `fast`, `done` of `measure`.
bool ReportHas(const Spec& sp, const char* word, std::string* rest = nullptr) {
    std::vector<std::string> rests;
    DC::SeenRestsOf(Line(sp.arm, "client-report"), &rests);
    for (const std::string& r : rests)
        if (r.rfind(word, 0) == 0) {
            if (rest) *rest = r.size() > std::strlen(word) ? r.substr(std::strlen(word) + 1) : std::string();
            return true;
        }
    return false;
}

void Judge(coop::net::Session* s, const Spec& sp) {
    if (!s->HasPeerConn(kSlot)) return Finish(sp, "INCONCLUSIVE", "the judged slot left before the verdict");
    if (coop::roster_ledger::Get(kSlot).playerNo != g_playerNo)
        return Finish(sp, "INCONCLUSIVE", "slot-replaced (the judged slot's occupant changed)");
    void* const decal = g_decal.Get();
    Tally d;
    Diff(GJ::GetStats(), d);
    const bool live = decal && R::IsLive(decal);
    const float now = live ? ProcessOf(decal) : -1.f;
    const float snap = live ? SnapOf(g_target, decal) : -1.f;

    // The arm's word from the client, an event of its own: `last`'s decal gone ahead of the host's word.
    if (sp.arm == Arm::Last && ReportHas(sp, "early"))
        return Finish(sp, "FAIL", "the client's decal was gone before the host's word: its destroy ran");
    if (sp.arm == Arm::Measure) {
        std::string rest;
        if (!ReportHas(sp, "done", &rest)) {
            if (Clock::now() - g_judgeAt > kFailBound) Finish(sp, "INCONCLUSIVE", "the client never reported its measures");
            return;
        }
        return Finish(sp, rest.rfind("held=0 ", 0) == 0 ? "FAIL" : "PASS", "client's measures: " + rest + "; host " + Summary(d));
    }
    if (d[kAsked] == 0) {
        if (Clock::now() - g_judgeAt > kFailBound) Finish(sp, "INCONCLUSIVE", "the client's message never arrived");
        return;
    }

    // A value that left where the arm leaves it is a verdict at once: nothing later can undo what a refusal let through.
    const bool destroys = sp.arm == Arm::Last;
    const bool refusal = sp.arm == Arm::Far || sp.arm == Arm::Notool || sp.arm == Arm::Dry || sp.arm == Arm::Gone ||
                         sp.arm == Arm::Joinleak;
    if (refusal) {
        const float have = sp.arm == Arm::Joinleak ? snap : now;
        const float want = sp.arm == Arm::Joinleak ? g_snapBase : g_decalBase;
        if (!live || std::fabs(have - want) > kValueEps)
            return Finish(sp, "FAIL", std::string(sp.arm == Arm::Joinleak ? "the value a joiner's snapshot carries for the decal"
                                                                           : "the host's decal") +
                                          " moved from " + F(want) + " to " + (live ? F(have) : "gone") + " on a refused message: " +
                                          Summary(d));
    } else if (sp.arm == Arm::Fast || sp.arm == Arm::Rain) {
        const float expect = g_decalBase - (sp.arm == Arm::Fast ? kSoapFallPerStrength : kRainFallPerStrength) * g_strength;
        const bool moved = std::fabs(now - g_decalBase) > kValueEps;
        if (!live || (moved && sp.arm == Arm::Fast && std::fabs(now - expect) > kValueEps) ||
            (moved && sp.arm == Arm::Rain && std::fabs(now - (g_decalBase - kRainFallPerStrength * g_strength)) > kValueEps)) {
            return Finish(sp, "FAIL", "the host's decal went from " + F(g_decalBase) + " to " + (live ? F(now) : "gone") +
                                          ", where one step leaves " + F(expect) + ": " + Summary(d));
        }
    }
    if (Outcomes(d) == 0 && !(destroys && !live)) {
        if (Clock::now() - g_judgeAt > kFailBound) Finish(sp, "INCONCLUSIVE", "the message was never judged: " + Summary(d));
        return;
    }
    const std::string bad = Mismatch(sp.arm, d);
    if (!bad.empty()) return Finish(sp, "FAIL", "the counters differ from the arm's table:" + bad + " got " + Summary(d));
    if (destroys && live) return;   // ended is counted as the destroy is called; the decal's end of play follows
    if (sp.arm == Arm::Fast) {
        std::string rest;
        if (!ReportHas(sp, "value", &rest)) {
            if (Clock::now() - g_judgeAt > kFailBound) Finish(sp, "INCONCLUSIVE", "the asker never reported what it reads");
            return;
        }
        const float asker = std::strtof(rest.c_str(), nullptr);
        if (std::fabs(asker - now) > kValueEps)
            return Finish(sp, "FAIL", "the asker reads " + F(asker) + " where the host holds " + F(now));
    }
    if (sp.arm == Arm::Last && !(ReportHas(sp, "held") && ReportHas(sp, "gone"))) {
        if (Clock::now() - g_judgeAt > kFailBound) Finish(sp, "INCONCLUSIVE", "the client's decal never reported held then gone");
        return;
    }
    if (sp.arm == Arm::Joinleak) {
        // The first life's verdict is only half the arm: the refused ask left the decal and the snapshot's value as they were. The
        // client leaves now (its life-done line, which the rig's rejoin waits on) and comes back; its decal is read at its
        // world-ready.
        UE_LOGI("[HOSTILE-DRILL] %s life 1: the far ask was refused and the value a joiner's snapshot carries stands at %.2f: %s",
                sp.name, snap, Summary(d).c_str());
        DC::Say(Line(sp.arm, "life1-done") + "ok");
        g_life1Done = true;
        g_armAt = Clock::now();
        g_step = HStep::Rejoin;
        return;
    }
    Finish(sp, "PASS", "the message ended as the arm's table: " + Summary(d) + "; host's decal " + F(g_decalBase) + " -> " +
                           (live ? F(now) : "gone"));
}


// ---- the join-leak arm's second life ----

// The client is back: a new occupant of slot 1, world-ready, its puppet posed. The host says the decal again for it.
void RejoinWait(coop::net::Session* s, const Spec& sp) {
    if (Clock::now() - g_armAt > kFailBound) {
        Finish(sp, "INCONCLUSIVE", "the client never rejoined");
        return;
    }
    const uint16_t occupant = coop::roster_ledger::Get(kSlot).playerNo;
    if (occupant == 0 || occupant == g_playerNo || !PuppetReady(s)) return;
    g_playerNo = occupant;
    DC::Say(Line(sp.arm, "rejoin-pick") + FormatTarget(g_target));
    g_judgeAt = Clock::now();
    g_step = HStep::RejoinJudge;
    UE_LOGI("[HOSTILE-DRILL] %s life 2: the client is back as occupant %u; its decal is read at its world-ready", sp.name,
            static_cast<unsigned>(occupant));
}

// What the rejoined client's decal read at its world-ready is the value the host held before the refused ask.
void RejoinJudge(const Spec& sp) {
    void* const decal = g_decal.Get();
    std::string rest;
    if (!ReportHas(sp, "joined", &rest)) {
        if (Clock::now() - g_judgeAt > kFailBound) Finish(sp, "INCONCLUSIVE", "the rejoined client never reported its decal");
        return;
    }
    const float joiner = std::strtof(rest.c_str(), nullptr);
    const float now = decal && R::IsLive(decal) ? ProcessOf(decal) : -1.f;
    if (std::fabs(joiner - g_decalBase) > kValueEps || std::fabs(now - g_decalBase) > kValueEps)
        return Finish(sp, "FAIL", "the joiner's decal reads " + F(joiner) + ", the host's " + F(now) + ", where the host held " +
                                      F(g_decalBase) + " before the refused fall");
    Finish(sp, "PASS", "the refused fall left no trace: the rejoined client's decal reads " + F(joiner) + ", the host's value " +
                           F(g_decalBase));
}
}  // namespace

void HostTick(coop::net::Session* s, const Spec& sp) {
    if (g_step == HStep::Done) return;
    if (g_armAt == Clock::time_point{}) {
        g_armAt = Clock::now();
        DC::SeenHas(Line(sp.arm, "client-ready"));   // the first read starts the feed's history
    }
    switch (g_step) {
    case HStep::WaitSlot:
        if (PuppetReady(s)) g_step = HStep::Weather;
        else if (Clock::now() - g_armAt > kFailBound)
            Finish(sp, "INCONCLUSIVE", std::string("the precondition never held: waiting for ") + g_waiting);
        return;
    case HStep::Weather:
        if (WeatherHolds(sp)) g_step = HStep::PickDecal;
        else if (g_step != HStep::Done && Clock::now() - g_armAt > kFailBound)
            Finish(sp, "INCONCLUSIVE", std::string("the precondition never held: waiting for ") + g_waiting);
        return;
    case HStep::PickDecal:
        if (!G::EnsureResolved()) return;
        if (!PickDecal(sp, /*crack=*/false, g_target, g_decal)) {
            Finish(sp, "INCONCLUSIVE", "the world holds no decal this arm can use");
            return;
        }
        if (sp.arm == Arm::Measure && !PickDecal(sp, /*crack=*/true, g_crackTarget, g_crack)) {
            Finish(sp, "INCONCLUSIVE", "the world holds no crack decal for the wall fixer");
            return;
        }
        DC::Say(Line(sp.arm, "pick") + FormatTarget(g_target));
        if (sp.arm == Arm::Measure) DC::Say(Line(sp.arm, "crack") + FormatTarget(g_crackTarget));
        g_step = HStep::WaitClient;
        UE_LOGI("[HOSTILE-DRILL] %s picked '%s' (%.0f, %.0f, %.0f) process %.2f", sp.name, g_target.key.c_str(),
                g_target.at.X, g_target.at.Y, g_target.at.Z, g_target.process);
        return;
    case HStep::WaitClient:
        WaitClient(s, sp);
        return;
    case HStep::Judge:
        Judge(s, sp);
        return;
    case HStep::Rejoin:
        RejoinWait(s, sp);
        return;
    case HStep::RejoinJudge:
        RejoinJudge(sp);
        return;
    case HStep::Done:
        return;
    }
}

void HostReset() {
    if (g_life1Done) {   // the join-leak arm's client left between its lives: what the second reads stands
        g_step = HStep::Rejoin;
        g_armAt = g_judgeAt = {};
        return;
    }
    g_step = HStep::WaitSlot;
    g_armAt = g_judgeAt = {};
    g_waiting = "slot 1's world-ready";
    g_playerNo = 0;
    g_rainAsked = g_clientReady = g_life1Done = false;
    g_decal.Reset();
    g_crack.Reset();
    g_target = Target{};
    g_crackTarget = Target{};
    g_base = GJ::Stats{};
    g_decalBase = g_snapBase = 0.f;
    g_strength = 1.f;
}

}  // namespace coop::dev::hostile_drill::grime::detail
