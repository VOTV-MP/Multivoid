// coop/dev/hostile_drill_grime_client.cpp -- the grime family's sender: see coop/dev/hostile_drill_grime.h. The client
// reads the decal the host picked, walks to it when the arm needs it near (the bot director, never a teleport), spawns and
// holds a sponge when the arm needs one, says it is ready, waits for the host's ready line, then acts: a raw GrimeState
// through the session's own send, bypassing its lane, or the real clean call a sponge makes (`last`, `measure`), and tells
// the host what its own copy reads where the arm judges that.

#include "hostile_drill_grime_detail.h"  // co-located private header (src tree, not include/)

#include "clean_drill_detail.h"  // Clean: grime_C::clean with the sponge as its first argument, as prop_sponge calls it

#include "coop/dev/director/clear_hand.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/config/config.h"
#include "coop/dev/drill_chat.h"
#include "coop/interactables/grime_judge.h"
#include "coop/interactables/grime_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/grime.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
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
namespace CD = coop::dev::clean_drill::detail;

enum class CStep : uint8_t { WaitJoin, WaitPick, Walk, Equip, WaitHost, Act, Await, Done };
CStep       g_step = CStep::WaitJoin;
bool        g_idleSaid = false, g_started = false;
Clock::time_point g_since{};
const char* g_waiting = nullptr;
Target      g_target;
std::wstring g_wkey;
float       g_hostValue = 0.f;             // what the host said the decal holds when it said ready
float       g_hostStrength = 1.f;          // and its cleanStrength, which sets the rain's step
float       g_start = 0.f;                 // this copy's reading when the arm acted
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
ue_wrap::CachedObjRef g_sponge;

// The measure arm's state.
enum class MStep : uint8_t { Strokes, AwaitEnd, CrackWait, CrackWalk, CrackClear, CrackReady, CrackFix };
coop::director::ClearHand g_clear;
MStep       g_m = MStep::Strokes;
Target      g_crack;
int         g_strokes = 0, g_overlap = 0;
uint64_t    g_heldAtStart = 0, g_heldAfterSponge = 0;
int         g_goneMs = -1;
Clock::time_point g_strokeAt{}, g_heldAt{};
size_t      g_crackIdx = 0;                  // the host's crack being tried
std::vector<ue_wrap::FVector> g_crackRing;   // the standpoints a route reaches about it
size_t      g_crackStand = 0;
bool        g_crackRingBuilt = false;
constexpr float kCrackRingCm = 400.f;       // inside the judge's reach of the fixer's overlap, the crack on a wall above
constexpr float kCrackReachCm = 100.f;      // how near a standpoint the walker must read
constexpr int   kCrackStandpoints = 3;

void Invalid(const Spec& sp, const std::string& why) {
    DC::Say(Line(sp.arm, "client-invalid") + why);
    UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: client: %s", sp.name, why.c_str());
    g_step = CStep::Done;
}

// True once the wait named `what` has run past the failure bound; a different wait restarts the clock.
bool Late(const char* what) {
    if (!g_waiting || std::strcmp(g_waiting, what) != 0) {
        g_waiting = what;
        g_since = Clock::now();
    }
    return Clock::now() - g_since > kFailBound;
}

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

void* DecalOf() { return g_wkey.empty() ? nullptr : GS::DecalOfKey(g_wkey); }

float ProcessOf(void* decal) {
    float p = -1.f;
    return decal && R::IsLive(decal) && G::ReadProcess(decal, p) ? p : -1.f;
}

void Report(const Spec& sp, const std::string& text) { DC::Say(Line(sp.arm, "client-report") + text); }

std::string F(float v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", v);
    return b;
}

// ---- the hand ----

// A spawned prop of `path` in this client's own hand, as a player's pickup puts it there; the host's record of it
// follows the hand announce. The spawn is the client's own, so no request opens.
const char* HoldNew(void* player, const wchar_t* path, void** out) {
    void* const cls = ue_wrap::asset_load::LoadObjectByPath(path);
    ue_wrap::FVector at{};
    if (!cls || !E::TryGetActorLocation(player, at)) return "the prop class did not load or the player's place is unread";
    void* const prop = E::SpawnActor(cls, {at.X, at.Y, at.Z + 60.f});
    bool collected = false;
    if (!prop || !E::CallMainPlayerHoldObject(player, prop, collected) || !collected)
        return "this client's own prop could not be spawned and held";
    // The hand's own item is what a verb names, not the world prop the pickup consumed.
    E::MainPlayerGrabState hand{};
    *out = (E::ReadMainPlayerGrabState(player, hand) && hand.holdingActor) ? hand.holdingActor : prop;
    return nullptr;
}

// ---- the walk ----

bool StartWalk(const Target& t) {
    g_walk = coop::director::StartBackgroundWalk(t.at, kWalkReachCm, kWalkDeadlineS);
    return g_walk != nullptr;
}

// ---- the arms' messages ----

bool SendValue(coop::net::Session* s, float value, uint8_t adopt = 0) {
    coop::net::KeyedScalarPayload p{};
    coop::net::WireKeyFromString(g_wkey, p.key);
    p.value = value;
    p.adopt = adopt;
    return s->SendReliable(coop::net::ReliableKind::GrimeState, &p, sizeof(p));
}

// ---- the measure arm ----

// How many decals a sponge's overlap at this decal's centre could touch: every decal whose box could meet the sphere of
// radius 15 about it (the sum of the two boxes' half diagonals and the radius).
struct OverlapCtx {
    ue_wrap::FVector centre;
    float            half;
    int              count;
};
void CountOverlap(void* ctx, void* d) {
    auto& c = *static_cast<OverlapCtx*>(ctx);
    ue_wrap::FVector centre{};
    float half = 0.f;
    if (!G::ReadBox(d, centre, half)) return;
    const float x = centre.X - c.centre.X, y = centre.Y - c.centre.Y, z = centre.Z - c.centre.Z;
    if (std::sqrt(x * x + y * y + z * z) <= 15.f + half + c.half) ++c.count;
}

void MeasureStrokes(coop::net::Session*, const Spec& sp, void* decal) {
    if (g_strokes == 0) {
        OverlapCtx oc{};
        if (G::ReadBox(decal, oc.centre, oc.half)) G::ForEachDecal(&CountOverlap, &oc);
        g_overlap = oc.count;
        g_heldAtStart = GJ::GetStats().held;
        UE_LOGI("[GRIME-MEASURE] decal '%s' process %.2f: %d decal(s) a sponge's overlap here could touch", g_target.key.c_str(),
                ProcessOf(decal), g_overlap);
    }
    {
        // A hand that has emptied ends the arm here, not after strokes the host would refuse for no tool.
        E::MainPlayerGrabState hand{};
        if (!E::ReadMainPlayerGrabState(coop::players::Registry::Get().Local(), hand) || !hand.holdingActor)
            return Invalid(sp, "the sponge left the hand mid-measure");
    }
    const auto now = Clock::now();
    if (g_strokes > 0 && now - g_strokeAt < std::chrono::milliseconds(300)) return;   // the sponge's own 0.25 s and a margin
    const float before = ProcessOf(decal);
    const long long dtMs = g_strokes ? std::chrono::duration_cast<std::chrono::milliseconds>(now - g_strokeAt).count() : 0;
    g_strokeAt = now;
    const uint64_t heldBefore = GJ::GetStats().held;
    const bool ran = CD::Clean(decal, g_sponge.Get(), kStrokeSub);
    ++g_strokes;
    const bool alive = R::IsLive(decal);
    const float after = ProcessOf(decal);
    UE_LOGI("[GRIME-MEASURE] stroke %d (+%lld ms): ran=%d process %.2f -> %.2f alive=%d held+%llu", g_strokes, dtMs, ran ? 1 : 0,
            before, after, alive ? 1 : 0, static_cast<unsigned long long>(GJ::GetStats().held - heldBefore));
    if (!ran) return Invalid(sp, "the sponge's clean did not run");
    if (GJ::GetStats().held > heldBefore || !alive || g_strokes >= 60) {
        g_heldAfterSponge = GJ::GetStats().held - g_heldAtStart;
        g_heldAt = now;
        g_m = MStep::AwaitEnd;
    }
}

void MeasureAwaitEnd(const Spec& sp, void* decal) {
    // The held decal stands at 0 until the host's word ends it; one that never held is already gone.
    if (!decal || !R::IsLive(decal)) {
        g_goneMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_heldAt).count());
        UE_LOGI("[GRIME-MEASURE] the decal ended on this copy %d ms after the last stroke; the hold fired %llu time(s)", g_goneMs,
                static_cast<unsigned long long>(g_heldAfterSponge));
        g_m = MStep::CrackWait;
        return;
    }
    if (Late("the host's end of the held decal")) Invalid(sp, "the host never ended the decal the last stroke asked for");
}

// One wall fixer use, with the fixer in the hand as a player has it: the sponge is put down, a fixer this client spawns is
// held, the host's record of it is awaited (the judge's tool route reads that record), the crack's copy is lowered to where
// one use ends it (the host lowers its own to the same), and wallFixer_fix is called with the hand's fixer. The ask the use
// sends must be granted; the host says so in the verdict.
void MeasureCrack(coop::net::Session*, const Spec& sp, void* player) {
    static R::InstanceOffset sConcrete{L"concrete"};
    std::string rest;
    switch (g_m) {
    case MStep::CrackWait: {
        // The host offers its nearest cracks, each as a line; this client tries them in turn, each by the standpoints about it
        // that a route reaches (the crack itself is on a wall, off the NavMesh), the next standpoint when a walk fails and the
        // next crack when none is left. Only when the host has said it offered no more is the arm INVALID.
        std::vector<std::string> lines;
        DC::SeenRestsOf(Line(sp.arm, "crack"), &lines);
        if (g_crackIdx >= lines.size()) {
            if (DC::SeenHas(Line(sp.arm, "crack-end")))
                return Invalid(sp, "no crack the host offered has a standpoint a route reaches and a walk got to");
            return;
        }
        if (!ParseTarget(lines[g_crackIdx], g_crack)) return Invalid(sp, "the host's crack line could not be read: " + lines[g_crackIdx]);
        if (!g_crackRingBuilt) {
            g_crackRing = coop::director::ReachableStandpoints(player, g_crack.at, kCrackRingCm, kCrackStandpoints, kCrackReachCm);
            g_crackRingBuilt = true;
            g_crackStand = 0;
            UE_LOGI("[GRIME-MEASURE] crack %zu '%s': a route reaches %zu of the %d standpoints about it", g_crackIdx,
                    g_crack.key.c_str(), g_crackRing.size(), kCrackStandpoints);
        }
        if (g_crackStand >= g_crackRing.size()) {
            ++g_crackIdx;
            g_crackRingBuilt = false;
            return;
        }
        g_walk = coop::director::StartBackgroundWalk(g_crackRing[g_crackStand], kCrackReachCm, kWalkDeadlineS);
        if (!g_walk) {
            ++g_crackStand;
            return;
        }
        g_m = MStep::CrackWalk;
        return;
    }
    case MStep::CrackWalk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        g_walk.reset();
        if (st == 2) {   // this standpoint was not reached: the next, then the next crack
            ++g_crackStand;
            g_m = MStep::CrackWait;
            return;
        }
        DC::Say(Line(sp.arm, "crack-chosen") + g_crack.key);   // the crack this client stands at: the host lowers its copy
        g_m = MStep::CrackClear;
        return;
    }
    case MStep::CrackClear: {
        // The hand holds the sponge: it is put down as a player puts it down, then the fixer is taken.
        const coop::director::ClearState cs = g_clear.Tick();
        if (cs == coop::director::ClearState::Working) return;
        if (cs == coop::director::ClearState::Failed) return Invalid(sp, std::string("the hand did not clear: ") + g_clear.Why());
        void* fixer = nullptr;
        if (const char* why = HoldNew(player, kFixerPath, &fixer)) return Invalid(sp, why);
        g_m = MStep::CrackReady;
        return;
    }
    case MStep::CrackReady: {
        if (!DC::SeenHas(Line(sp.arm, "fixer-ready"))) {
            if (Late("the host's record of the wall fixer")) Invalid(sp, "the host never saw the wall fixer in the hand");
            return;
        }
        g_waiting = nullptr;
        g_m = MStep::CrackFix;
        return;
    }
    default:
        break;
    }
    E::MainPlayerGrabState hand{};
    std::wstring ckey(g_crack.key.begin(), g_crack.key.end());
    void* const crack = GS::DecalOfKey(ckey);
    if (!crack || !E::ReadMainPlayerGrabState(player, hand) || !hand.holdingActor)
        return Invalid(sp, "the crack or the fixer in the hand did not resolve");
    void* const fixer = hand.holdingActor;
    const int32_t off = sConcrete.Of(fixer);
    if (off < 0) return Invalid(sp, "the wall fixer's concrete did not resolve");
    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(fixer) + off) = 3;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(crack), L"wallFixer_fix");
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<void*>(L"wallFixer", fixer)) return Invalid(sp, "wallFixer_fix did not resolve");
    G::WriteProcessAndApply(crack, kCrackLeft);
    const uint64_t heldBefore = GJ::GetStats().held;
    const bool ran = ue_wrap::Call(crack, f);
    const bool alive = R::IsLive(crack);
    UE_LOGI("[GRIME-MEASURE] wall fixer use on '%s': ran=%d alive=%d process %.2f held+%llu", g_crack.key.c_str(), ran ? 1 : 0,
            alive ? 1 : 0, ProcessOf(crack), static_cast<unsigned long long>(GJ::GetStats().held - heldBefore));
    char text[160];
    std::snprintf(text, sizeof(text), "done held=%llu fixheld=%llu strokes=%d overlap=%d gone_ms=%d",
                  static_cast<unsigned long long>(g_heldAfterSponge),
                  static_cast<unsigned long long>(GJ::GetStats().held - heldBefore), g_strokes, g_overlap, g_goneMs);
    Report(sp, text);
    g_step = CStep::Done;
}

// ---- the acts ----

void Act(coop::net::Session* s, const Spec& sp, void* player) {
    // The rain arm's decal is the host's own, made for it: this copy holds none, and the ask names it by its key alone.
    void* const decal = DecalOf();
    if (sp.pick != Pick::Wash && (!decal || !R::IsLive(decal))) return Invalid(sp, "the picked decal does not resolve on this copy");
    switch (sp.arm) {
    case Arm::Far: case Arm::Notool: case Arm::Dry:
        if (!SendValue(s, g_hostValue - 5.f)) return Invalid(sp, "the raw fall was not sent");
        g_step = CStep::Done;
        return;
    case Arm::Joinleak:
        if (!SendValue(s, g_hostValue - 5.f)) return Invalid(sp, "the raw fall was not sent");
        g_step = CStep::Await;
        return;
    case Arm::Gone:
        if (!SendValue(s, coop::net::kGrimeGone)) return Invalid(sp, "the raw ended mark was not sent");
        g_step = CStep::Done;
        return;
    case Arm::Fast:
        g_start = ProcessOf(decal);
        if (!SendValue(s, 0.f)) return Invalid(sp, "the raw fall was not sent");
        g_step = CStep::Await;
        return;
    case Arm::Rain: {
        const float cs = g_hostStrength;   // the host's reading of the decal's cleanStrength
        if (!SendValue(s, g_hostValue - kRainFallPerStrength * cs)) return Invalid(sp, "the raw fall was not sent");
        g_step = CStep::Done;
        return;
    }
    case Arm::Last: {
        const bool ran = CD::Clean(decal, g_sponge.Get(), kStrokeSub);
        const bool alive = R::IsLive(decal);
        const float after = ProcessOf(decal);
        if (!ran) return Invalid(sp, "the sponge's clean did not run");
        if (!alive) {
            Report(sp, "early");   // the destroy ran: the decal is gone before the host was asked
            g_step = CStep::Done;
            return;
        }
        if (after > 0.01f) return Invalid(sp, "one stroke did not take the decal to its end (" + F(after) + ")");
        Report(sp, "held " + F(after));
        g_step = CStep::Await;
        return;
    }
    case Arm::Measure:
        g_step = CStep::Await;
        return;
    default:
        (void)player;
        return;
    }
}

void Await(coop::net::Session* s, const Spec& sp, void* player) {
    void* const decal = DecalOf();
    if (sp.arm == Arm::Joinleak) {
        // The host judges the refused ask first: this life ends only once it has, since a slot that leaves takes its queue along.
        if (DC::SeenHas(Line(sp.arm, "life1-done"))) {
            UE_LOGI("[HOSTILE-DRILL] grime_joinleak CLIENT LIFE DONE");
            g_step = CStep::Done;
        } else if (Late("the host's verdict on the refused ask")) {
            Invalid(sp, "the host never judged the refused ask");
        }
        return;
    }
    if (sp.arm == Arm::Fast) {
        const float v = ProcessOf(decal);
        if (v >= 0.f && std::fabs(v - g_start) > 0.5f) {
            Report(sp, "value " + F(v));
            g_step = CStep::Done;
        } else if (Late("the host's answer to the raw fall")) {
            Invalid(sp, "the host never answered the raw fall");
        }
        return;
    }
    if (sp.arm == Arm::Last) {
        if (!decal || !R::IsLive(decal)) {
            Report(sp, "gone");
            g_step = CStep::Done;
        } else if (Late("the host's end of the held decal")) {
            Invalid(sp, "the host never ended the decal the last stroke asked for");
        }
        return;
    }
    if (g_m == MStep::Strokes) {
        if (!decal) return Invalid(sp, "the measured decal does not resolve");
        MeasureStrokes(s, sp, decal);
    } else if (g_m == MStep::AwaitEnd) {
        MeasureAwaitEnd(sp, decal);
    } else {
        MeasureCrack(s, sp, player);
    }
}


// ---- the join-leak arm's second life ----

// The rig starts this life with VOTVCOOP_GRIME_DRILL_REJOIN=1 after the first's life-done line: the host names the decal again,
// and what this copy reads at its world-ready is the host's value, which the refused fall left as it was.
bool RejoinedLife() {
    static const bool second = coop::config::ReadEnv("VOTVCOOP_GRIME_DRILL_REJOIN") == "1";
    return second;
}

void RejoinedTick(const Spec& sp) {
    std::string rest;
    if (!DC::SeenRestOf(Line(sp.arm, "rejoin-pick"), &rest)) {
        if (Late("the host's line naming the decal")) Invalid(sp, "the host never named the decal for the rejoined life");
        return;
    }
    Target t;
    if (!ParseTarget(rest, t)) return Invalid(sp, "the host's rejoin line could not be read: " + rest);
    g_wkey.assign(t.key.begin(), t.key.end());
    void* const decal = DecalOf();
    const float v = ProcessOf(decal);
    if (v < 0.f) return Invalid(sp, "the decal does not resolve on the rejoined copy");
    Report(sp, "joined " + F(v));
    UE_LOGI("[HOSTILE-DRILL] grime_joinleak CLIENT LIFE 2 DONE: the decal reads %.2f at world-ready", v);
    g_step = CStep::Done;
}
}  // namespace

void ClientTick(coop::net::Session* s, const Spec& sp) {
    if (g_step == CStep::Done) return;
    if (!g_started) {
        g_started = true;
        DC::SeenHas(Line(sp.arm, "pick"));   // the first read starts the feed's history
    }
    if (!OwnJoinOver()) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    void* const player = coop::players::Registry::Get().Local();
    if (!player || self == 0 || self >= coop::players::kMaxPeers) return;
    if (self != kSlot) {
        if (!g_idleSaid) {
            g_idleSaid = true;
            UE_LOGI("[HOSTILE-DRILL] %s idles: this client is slot %u, the family acts on slot %u", sp.name,
                    static_cast<unsigned>(self), static_cast<unsigned>(kSlot));
        }
        g_step = CStep::Done;
        return;
    }
    std::string rest;
    if (sp.arm == Arm::Joinleak && RejoinedLife()) return RejoinedTick(sp);
    switch (g_step) {
    case CStep::WaitJoin:
        g_step = CStep::WaitPick;
        return;
    case CStep::WaitPick:
        if (!DC::SeenRestOf(Line(sp.arm, "pick"), &rest)) {
            if (Late("the host's pick")) Invalid(sp, "the host never picked a decal");
            return;
        }
        if (!ParseTarget(rest, g_target)) return Invalid(sp, "the host's pick line could not be read: " + rest);
        g_wkey.assign(g_target.key.begin(), g_target.key.end());
        g_waiting = nullptr;
        if (sp.walk) {
            if (!StartWalk(g_target)) return Invalid(sp, "the walk to the decal did not start");
            g_step = CStep::Walk;
        } else {
            g_step = CStep::Equip;
        }
        return;
    case CStep::Walk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        if (st == 2) return Invalid(sp, "the walk to the decal ended without reaching it");
        g_step = CStep::Equip;
        return;
    }
    case CStep::Equip: {
        if (sp.sponge) {
            void* sponge = nullptr;
            if (const char* why = HoldNew(player, kSpongePath, &sponge)) return Invalid(sp, why);
            g_sponge.Set(sponge);
        }
        DC::Say(Line(sp.arm, "client-ready"));
        g_step = CStep::WaitHost;
        return;
    }
    case CStep::WaitHost: {
        if (!DC::SeenRestOf(Line(sp.arm, "host-ready"), &rest)) {
            if (Late("the host's ready line")) Invalid(sp, "the host never said it was ready");
            return;
        }
        g_hostValue = std::strtof(rest.c_str(), nullptr);
        if (const size_t sp2 = rest.find(' '); sp2 != std::string::npos) g_hostStrength = std::strtof(rest.c_str() + sp2 + 1, nullptr);
        // The decal this copy holds reads what the host holds (the host's own fall reaches it through the lane).
        const float mine = sp.pick == Pick::Wash ? g_hostValue : ProcessOf(DecalOf());
        if (mine < 0.f || std::fabs(mine - g_hostValue) > 0.5f) {
            if (Late("this copy's decal to read the host's value")) Invalid(sp, "this copy's decal never read the host's value");
            return;
        }
        g_waiting = nullptr;
        g_step = CStep::Act;
        return;
    }
    case CStep::Act:
        Act(s, sp, player);
        return;
    case CStep::Await:
        Await(s, sp, player);
        return;
    case CStep::Done:
        return;
    }
}

void ClientReset() {
    g_step = CStep::WaitJoin;
    g_idleSaid = g_started = false;
    g_since = {};
    g_waiting = nullptr;
    g_target = Target{};
    g_wkey.clear();
    g_hostValue = g_start = 0.f;
    g_hostStrength = 1.f;
    g_walk.reset();
    g_sponge.Reset();
    g_m = MStep::Strokes;
    g_crack = Target{};
    g_strokes = g_overlap = 0;
    g_heldAtStart = g_heldAfterSponge = 0;
    g_goneMs = -1;
    g_strokeAt = g_heldAt = {};
    g_crackIdx = 0;
    g_crackRing.clear();
    g_crackStand = 0;
    g_crackRingBuilt = false;
    g_clear = coop::director::ClearHand{};
}

}  // namespace coop::dev::hostile_drill::grime::detail
