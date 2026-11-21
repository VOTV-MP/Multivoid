// coop/interactables/grime_judge.cpp -- see coop/interactables/grime_judge.h.

#include "coop/interactables/grime_judge.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/interactables/grime_sync.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/session/net_pump.h"      // IsInAnnouncedWorld: a client's own world load runs natively
#include "coop/world/weather_rain.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/devices/grime.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace coop::grime_judge {
namespace {

namespace E  = ue_wrap::engine;
namespace G  = ue_wrap::grime;
namespace HI = coop::hand_item;

// ---- the game's own numbers, each with the line of the Blueprint dump it stands on ----------------------------------
// The look-at trace every hand verb reads through: mainPlayer.armLength, 200 (mainPlayer.cpp:33, set to 200 again with
// nothing equipped, :15086; a weapon's row sets its own, :14960, and a sponge is no weapon).
constexpr float kArmLengthCm = 200.0f;
// The sponge overlaps a sphere of this radius at the trace's hit (prop_sponge.cpp:134); the wall fixer one of 5, and
// repairs the nearest of what it holds (prop_wallfixer.cpp:134-157).
constexpr float kSpongeRadiusCm = 15.0f;
constexpr float kFixerRadiusCm = 5.0f;
// One call's fall of a sponge: grime_C::clean takes `Sub * cleanStrength * 1.2` (lines 220-222 of the grime_C dump); the sponge's Sub is
// getStrength * 4 (prop_sponge.cpp:134), and getStrength is the sponge's `strength` and double that with soap
// (prop_sponge.cpp:756-761). `strength`'s 1.5 (prop_sponge.cpp:9) is the property's default and getStrength's body does not show it as
// the value it returns; the measure run agrees with it: a stroke of Sub 6 fell 14.4 on a decal of cleanStrength 2, the judge's step read 28.8.
constexpr float kSpongeStrength = 1.5f;
constexpr float kSpongeSoapFactor = 2.0f;
constexpr float kSpongeSubFactor = 4.0f;
constexpr float kCleanFactor = 1.2f;
// The wall fixer's fall is a flat 10 (grime_crack.cpp:875; its `concrete > 0` is the client's, which the host does not read).
constexpr float kFixerStepCm = 10.0f;
// The rain's call is clean(nullptr, 2, true) (daynightCycle.cpp:973): a fall of 2 * cleanStrength * 1.2.
constexpr float kRainSub = 2.0f;
// The rain's own test, repeated on the host's copy: a pass washes every grime decal within 10000 of the LOCAL camera
// (daynightCycle.cpp:1609), skips resistRain ones (:945), and a decal whose trace 2500 up from 25 ahead of it hits a
// WorldStatic object is roofed and left alone (:953-963); the pass runs from a 2 s looping timer (:1384), and each tick
// takes a random part of what is in range (:1620-1624), so a decal is washed at most once a period.
constexpr float kRainRadiusCm = 10000.0f;
constexpr float kRainRoofForwardCm = 25.0f;
constexpr float kRainRoofTraceCm = 2500.0f;
constexpr uint64_t kRainPeriodMs = 2000;
// Provisional: a per-key rain cadence forgives this much of the period (frame jitter, the transport's batching); no run has
// read it. A reach adds this beyond the tool's trace, its overlap and the decal's box (the pose allowance is the authority's
// own); the measure run's strokes stood 432 uu from the decal, and every grant logs that distance with the reach allowed, so
// the slack is read from the next run.
constexpr uint64_t kRainPeriodSlackMs = 250;
constexpr float kReachSlackCm = 100.0f;
// A fall that lands within this of the host's value minus the step is no cut: float arithmetic on both sides.
constexpr float kStepEps = 0.01f;

// A sender's asks run from one ordered queue at a bounded rate. The measure run read about 3.3 asks a second per decal (the
// drill's own 300 ms stroke; the game's sponge loop is 4 a second, prop_sponge dump line 87) and one decal under a sponge's
// overlap, so 16 a second with a burst of 16 holds several decals at once; a human's sponge over a cluster of decals is the
// reading still to take.
constexpr coop::net::IntentBudget kBudget{16.0f, 16.0f};
// The keys one sender may have queued; an ask of a key already queued is folded into it.
constexpr size_t kMaxPending = 64;
// How long a head waits for its sender's first pose before it is refused.
constexpr uint64_t kNoBodyBoundMs = 10000;

constexpr const wchar_t* kSpongeCls = L"prop_sponge_C";
constexpr const wchar_t* kFixerCls = L"prop_wallfixer_C";

Stats g_stats{};
coop::net::RefusalTally g_refusals{"grime_judge"};

bool ProbeLog() {
    static const bool s = ::coop::config::ResolveFlag(::coop::config_registry::rows::grime_log);
    return s;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One queued key of one sender; its lowest asked value waits in the slot's map, since a later ask of the key lowers it.
struct Item {
    std::wstring key;
    uint64_t     waitSinceMs = 0;  // since when the head waits for its sender's body; 0 = not waiting
};

struct Slot {
    coop::net::IntentQueue<Item> queue;
    std::unordered_map<std::wstring, float> asked;
    uint64_t lastAskMs = 0;  // when this sender's previous ask arrived: the grime_log line's stroke cadence
    std::unordered_set<std::wstring> owed;   // keys the queue could not take: each is answered with the host's value
    uint64_t windowStartMs = 0;
    uint32_t windowAsks = 0;
    size_t   windowPeak = 0;
    size_t   windowOwed = 0;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// The last rain grant per key, so a key is washed at most once in the rain's own period.
std::unordered_map<std::wstring, uint64_t> g_rainLast;

// The dev red arm of the judge: the host applies a client's fall as it came, as before the judge. Resolved once.
bool JudgeOff() {
    static const bool s = ::coop::config::ResolveFlag(::coop::config_registry::rows::grime_judge_off);
    return s;
}

// The dev red arm of the rain route: its checks are skipped, so an ask the host's own weather would refuse is granted.
bool RainOpen() {
    static const bool s = ::coop::config::ResolveFlag(::coop::config_registry::rows::grime_rain_open);
    return s;
}

// One route's verdict: held or not, the fall it allows, the sender's measured distance and why it did not hold.
struct Route {
    bool        ok = false;
    float       step = 0.f;
    float       dist = -1.f;
    float       allowed = -1.f;  // the reach the authority allowed for it, its pose allowance included
    const char* why = nullptr;
};

// The tool route: the verb's own tool in the sender's hand and the decal within the trace and overlap of it. A crack is the
// wall fixer's (a sponge's clean returns without a fall on it), every other decal the sponge's.
Route ToolRoute(coop::net::Session& s, uint8_t slot, void* decal, const ue_wrap::FVector& centre, float halfDiag,
                bool& inHand) {
    Route r;
    const bool crack = G::IsCrack(decal);
    inHand = HI::HeldClassIs(slot, crack ? kFixerCls : kSpongeCls);
    if (!inHand) { r.why = "no tool in hand"; return r; }
    if (!crack) {
        bool cleanable = false;
        if (!G::ReadIsCleanable(decal, cleanable) || !cleanable) { r.why = "a decal a sponge cannot clean"; return r; }
    }
    const float overlap = crack ? kFixerRadiusCm : kSpongeRadiusCm;
    const float reach = kArmLengthCm + overlap + halfDiag + kReachSlackCm;
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, reach).AuthorizeSegment(centre, centre);
    r.dist = subject.distUU;
    r.allowed = subject.reachUU;
    if (!subject) { r.why = coop::element::OutcomeName(subject.outcome); return r; }
    float cleanStrength = 1.0f;
    if (!crack && !G::ReadCleanStrength(decal, cleanStrength)) { r.why = "the decal's strength does not read"; return r; }
    r.ok = true;
    // A sponge with soap is the strongest call it makes; the host does not read the client's soap.
    r.step = crack ? kFixerStepCm
                   : kSpongeStrength * kSpongeSoapFactor * kSpongeSubFactor * cleanStrength * kCleanFactor;
    return r;
}

enum class Roof : uint8_t { Clear, Roofed, Unread };

// The rain's roof test, as the game runs it: a trace up from a little ahead of the decal against WorldStatic.
Roof RoofOf(void* decal) {
    const ue_wrap::FVector fwd = E::GetActorForwardVector(decal);
    ue_wrap::FVector loc{};
    if (!E::TryGetActorLocation(decal, loc) || (fwd.X == 0.f && fwd.Y == 0.f && fwd.Z == 0.f)) return Roof::Unread;
    const ue_wrap::FVector from{loc.X + fwd.X * kRainRoofForwardCm, loc.Y + fwd.Y * kRainRoofForwardCm,
                                loc.Z + fwd.Z * kRainRoofForwardCm};
    const ue_wrap::FVector to{from.X, from.Y, from.Z + kRainRoofTraceCm};
    const uint8_t worldStatic = 0;
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitForObjects(decal, from, to, &worldStatic, 1, nullptr, 0, &hit)) return Roof::Unread;
    return hit.blocked ? Roof::Roofed : Roof::Clear;
}

// The rain route: the host is itself raining, and the decal is one the rain's pass would wash on the host's copy and the
// sender's body is within the rain's radius. `applicable` says the host rains, so a refusal names the rain.
Route RainRoute(coop::net::Session& s, uint8_t slot, const std::wstring& key, void* decal,
                const ue_wrap::FVector& centre, bool& applicable) {
    Route r;
    bool found = false;
    const bool raining = coop::weather_rain::ReadLocalIsRaining(&found) && found;
    applicable = raining;
    if (!raining) { r.why = "the host is not raining"; return r; }
    // The rain's pass does nothing in winter: it returns when getSeason reads 2 (lines 1593-1597 of the daynightCycle dump),
    // which the gamemode's isWinter holds, Christmas included.
    bool winter = false;
    if (!G::ReadWinter(winter)) { r.why = "the season does not read"; return r; }
    if (winter) { r.why = "winter: the rain's pass cleans nothing"; return r; }
    bool resist = false;
    bool cleanable = false;
    float cleanStrength = 1.0f;
    if (!G::ReadResistRain(decal, resist) || !G::ReadIsCleanable(decal, cleanable) ||
        !G::ReadCleanStrength(decal, cleanStrength)) {
        r.why = "the decal's rain settings do not read";
        return r;
    }
    if (resist) { r.why = "a decal the rain leaves alone"; return r; }
    if (!cleanable) { r.why = "a decal a clean cannot lower"; return r; }
    const Roof roof = RoofOf(decal);
    if (roof == Roof::Unread) { r.why = "the roof trace does not run"; return r; }
    if (roof == Roof::Roofed) { r.why = "a roofed decal"; return r; }
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, kRainRadiusCm).AuthorizeSegment(centre, centre);
    r.dist = subject.distUU;
    r.allowed = subject.reachUU;
    if (!subject) { r.why = coop::element::OutcomeName(subject.outcome); return r; }
    const auto last = g_rainLast.find(key);
    if (last != g_rainLast.end() && NowMs() - last->second + kRainPeriodSlackMs < kRainPeriodMs) {
        r.why = "washed by the rain a moment ago";
        return r;
    }
    r.ok = true;
    r.step = kRainSub * cleanStrength * kCleanFactor;
    return r;
}

void Refuse(uint8_t slot, const char* why) {
    g_refusals.Note(slot, why, coop::net::RefusalTally::Clock::now());
}

// False keeps the head at its sender's queue: the host has no body for the sender yet, so its reach cannot be measured,
// and the head waits for the body, up to a bound. Everything else is granted or refused, and consumed.
bool Execute(coop::net::Session& s, Slot& sl, Item& it, uint8_t slot) {
    const auto asked = sl.asked.find(it.key);
    if (asked == sl.asked.end()) return true;  // every queued key has its entry; nothing to judge without one
    const float value = asked->second;
    const auto consume = [&]() { sl.asked.erase(it.key); return true; };

    void* decal = coop::grime_sync::DecalOfKey(it.key);
    float host = 0.f;
    if (!decal || !coop::grime_sync::HostValueOf(it.key, decal, host)) {
        ++g_stats.refusedNodecal;
        Refuse(slot, "refused nodecal: a key naming no decal the host holds");
        return consume();
    }
    // A player who is not alive cleans nothing and is not answered: judged before reach and the routes, as the coffee and
    // woodchipper judges do.
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        ++g_stats.refusedDead;
        Refuse(slot, "refused dead: the sender's player is not alive");
        return consume();
    }
    // A stroke that reports zero is a decal's last, held on its client: zero is always judged, even where the host already
    // holds the decal near zero, so the decal the client holds at zero ends on the host too.
    if (value > 0.f && value >= host - kStepEps) {
        ++g_stats.already;
        return consume();
    }
    // A refused ask on a known decal is answered with the host's value, which raises the asker's optimistic copy back.
    const auto answerHost = [&]() { coop::grime_sync::AnswerAsker(s, it.key, host, slot); };
    if (!coop::element::IntentTarget::ForClientIntent(s, slot, kArmLengthCm).HasBody()) {
        const uint64_t now = NowMs();
        if (!it.waitSinceMs) it.waitSinceMs = now;
        if (now - it.waitSinceMs <= kNoBodyBoundMs) return false;
        ++g_stats.refusedNobody;
        Refuse(slot, "refused nobody: the sender took no pose in the wait");
        answerHost();
        return consume();
    }
    ue_wrap::FVector centre{};
    float halfDiag = 0.f;
    if (!G::ReadBox(decal, centre, halfDiag)) {
        ++g_stats.refusedReach;
        Refuse(slot, "refused reach: the decal's box does not read");
        answerHost();
        return consume();
    }
    bool inHand = false;
    bool raining = false;
    const Route tool = ToolRoute(s, slot, decal, centre, halfDiag, inHand);
    // The rain's step never exceeds a tool's, so its roof trace runs only when the tool route did not hold.
    Route rain;
    if (!tool.ok || RainOpen()) rain = RainRoute(s, slot, it.key, decal, centre, raining);
    if (RainOpen() && !rain.ok) {  // the red arm: the rain route skips its checks
        rain.ok = true;
        rain.step = kRainSub * kCleanFactor;
    }
    if (!tool.ok && !rain.ok) {
        // The reason is the route the sender was on: the tool in its hand, or the rain the host is in, else no tool.
        if (inHand) {
            ++g_stats.refusedReach;
            Refuse(slot, "refused reach: the decal lies beyond the tool's reach");
        } else if (raining) {
            ++g_stats.refusedRain;
            Refuse(slot, "refused rain: the host's rain would not wash that decal");
        } else {
            ++g_stats.refusedNotool;
            Refuse(slot, "refused notool: nothing in hand that cleans, and the host is not raining");
        }
        if (ProbeLog())
            UE_LOGI("grime_judge: DENY slot=%u key='%ls' asked %.2f of %.2f -- tool: %s (%.0f uu); rain: %s (%.0f uu)",
                    static_cast<unsigned>(slot), it.key.c_str(), value, host, tool.why ? tool.why : "-", tool.dist,
                    rain.why ? rain.why : "-", rain.dist);
        answerHost();
        return consume();
    }
    const float step = std::max(tool.ok ? tool.step : 0.f, rain.ok ? rain.step : 0.f);
    const float floorValue = host - step;
    const bool cut = value < floorValue - kStepEps;
    const float granted = cut ? floorValue : value;
    if (tool.ok) ++g_stats.toolGrants;
    else { ++g_stats.rainGrants; g_rainLast[it.key] = NowMs(); }
    if (cut) ++g_stats.clamped;
    if (granted <= 0.f) {
        // The game destroys one past zero (lines 236-241 of the grime_C dump); a stroke that reports zero is its last, so zero is the end.
        ++g_stats.ended;
        coop::grime_sync::GrantEnd(decal);
    } else {
        ++g_stats.granted;
        coop::grime_sync::GrantFall(s, it.key, decal, granted, slot, cut);
    }
    // The distance each grant was measured at, with the reach allowed for it: the first of a session's grants always, the rest
    // under grime_log, so the reach slack reads from any run.
    static uint32_t s_grantsSaid = 0;
    if (ProbeLog() || s_grantsSaid < 64) {
        ++s_grantsSaid;
        UE_LOGI("grime_judge: GRANT slot=%u key='%ls' asked %.2f of %.2f -> %.2f%s (step %.2f; tool %.0f of %.0f uu, rain %.0f of "
                "%.0f uu)", static_cast<unsigned>(slot), it.key.c_str(), value, host, granted, cut ? " cut" : "", step,
                tool.dist, tool.allowed, rain.dist, rain.allowed);
    }
    return consume();
}

// ---- the client's hold -----------------------------------------------------------------------
// A client never loses a decal the host keeps, whatever destroys it. What destroys a grime decal, in the Blueprint dumps:
// grime_C::clean at a process below zero (line 241 of the grime_C dump; called by prop_sponge, lines 136 and 261, and by
// the rain, daynightCycle line 973), wallFixer_fix (line 892 of the grime_crack dump; prop_wallfixer, line 157), and a
// gasoline decal's fuel burnt out (line 21 of the grime_gasoline dump, reached by fireDamage); the save slot's grime reset
// edits save data and no actor, and no other Blueprint holds a destroy of a grime decal. All three are the native
// K2_DestroyActor, which the script gate never sees, so the hold stands on the native function seam
// (ue_wrap/core/ufunction_hook), whose PRE can refuse a call with no result and no out parameter. It is armed for a client in
// a session and holds the destroy of any grime decal in an announced world, except the lane's own (the host's end applied
// here, a spill the lane removes: AllowDestroy). A destroy inside a clean or a wallFixer_fix is a player's or the
// rain's fall, which the host judges: the lane's POST asks zero for it. Any other is no ask, and the host's world decides:
// its own end of the decal reaches this peer as kGrimeGone. The verb's frame is read from the scope the lane marks on the
// body (the script gate's RAII mark), never from a counter a fault could leave raised.
UE_FACT_FN(kRowK2Destroy, "grime_judge", L"Actor", L"K2_DestroyActor", Declared);

namespace UH = ue_wrap::ufunction_hook;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};
bool      g_seamSettled = false;
void*     g_seamFn = nullptr;
bool      g_seamArmed = false;
int       g_allow = 0;            // AllowDestroy scopes open: the lane's own destroys
uintptr_t g_verbBit = 0;          // the scope bit a clean or wallFixer_fix body carries
void*     g_heldInVerb = nullptr; // the decal whose destroy the PRE held inside a verb: the POST asks zero for it
const char g_verbOwner = 0;       // the address the scope bit is taken for

// The dev red arm of the hold: the client's destroy runs as the game wrote it, so the decal is gone before the host's word.
bool HoldOff() {
    static const bool s = ::coop::config::ResolveFlag(::coop::config_registry::rows::grime_hold_off);
    return s;
}

sg::Verdict OnDestroyPre(void* context, void* /*sourceObject*/, const uint8_t* /*parms*/) {
    if (g_allow > 0 || !G::IsGrime(context)) return sg::Verdict::Run;
    // A destroy of an object of a world this client has not announced ready is that world's load: it runs natively.
    if (!coop::net_pump::IsInAnnouncedWorld(context)) return sg::Verdict::Run;
    if (HoldOff()) {
        ++g_stats.heldOff;
        return sg::Verdict::Run;
    }
    void* const caller = UH::CurrentCallerFrame().function;
    const bool inVerb = caller && g_verbBit != 0 && (sg::ScopeBits(caller) & g_verbBit) != 0;
    if (inVerb) g_heldInVerb = context;
    if (++g_stats.held <= 8)
        UE_LOGI("grime_judge: held the destroy of decal %p (%s) (#%llu)%s", context,
                inVerb ? "inside a clean or a wall fixer's repair" : "by another path",
                static_cast<unsigned long long>(g_stats.held), inVerb ? " -- the host is asked to end it" : " -- the host's world decides");
    return sg::Verdict::Cancel;
}

// Puts the PRE on K2_DestroyActor, disarmed, once the Actor class is loaded: one attempt, since a patch that failed is
// not mended by trying it again (prop_lifecycle settles the same function the same way).
void SettleSeam() {
    if (g_seamSettled) return;
    void* const actorCls = ue_wrap::object_index::ClassByName(ue_wrap::profile::name::ActorClassName);
    if (!actorCls) return;
    g_seamSettled = true;
    g_verbBit = sg::ScopeBitFor(&g_verbOwner);
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::ResolveOn(kRowK2Destroy, actorCls);
    if (!g_verbBit || !fn || !UH::InstallPreHook(fn, &OnDestroyPre, /*armed=*/false)) {
        UE_LOGE("grime_judge: the client's destroy hold is OFF -- the K2_DestroyActor pre hook (or its scope bit) did not go on (a "
                "full native hook table says so in its own line above): a client's last stroke destroys its decal and asks "
                "zero after the fact, and any other destroy of its decals is not held");
        return;
    }
    g_seamFn = fn.Fn();
    int patched = 0;
    UH::DebugForEachHook([](void* ctx, void*, bool) { ++*static_cast<int*>(ctx); }, &patched);
    UE_LOGI("grime_judge: the client's destroy hold is on K2_DestroyActor @ %p (%d native hook slot(s) in use with it)", g_seamFn,
            patched);
}

// The seam is armed for a client in a session and idle otherwise: no peer pays a PRE on every destroy of every actor
// outside it.
void ArmSeam(coop::net::Session& session) {
    if (!g_seamFn) return;
    const bool want = session.connected() && session.role() == coop::net::Role::Client;
    if (want == g_seamArmed) return;
    g_seamArmed = want;
    UH::SetArmed(g_seamFn, &OnDestroyPre, want);
}

}  // namespace

bool RoofClear(void* decal) { return decal && RoofOf(decal) == Roof::Clear; }

Stats GetStats() { return g_stats; }

AllowDestroy::AllowDestroy() { ++g_allow; }
AllowDestroy::~AllowDestroy() { --g_allow; }

uintptr_t OpenVerb() {
    g_heldInVerb = nullptr;
    return g_verbBit;
}

bool TakeHeld(void* decal) {
    const bool held = g_heldInVerb != nullptr && g_heldInVerb == decal;
    if (held) g_heldInVerb = nullptr;
    return held;
}

bool HoldIsOff() { return g_seamSettled && !g_seamFn; }

// The keys a sender's queue could not take, answered with the host's value, a few a tick: a refusal of a known decal is never
// silent, so the asker's lowered copy is raised back, and a key refused again before its answer is one answer (a set).
constexpr size_t kOwedPerTick = 32;

void AnswerOwed(coop::net::Session& s, uint8_t slot, Slot& sl) {
    size_t sent = 0;
    for (auto it = sl.owed.begin(); it != sl.owed.end() && sent < kOwedPerTick;) {
        void* const decal = coop::grime_sync::DecalOfKey(*it);
        float host = 0.f;
        if (decal && coop::grime_sync::HostValueOf(*it, decal, host)) {
            coop::grime_sync::AnswerAsker(s, *it, host, slot);
            ++sent;
        }
        it = sl.owed.erase(it);
    }
}

// What one sender asked in a window of the rain's own period: how many asks, and the most keys its queue held at once. A
// rain pass washes a random share of every decal near the client's camera in one tick, so this is the number the queue's cap
// and the budget are sized on.
constexpr uint64_t kWindowMs = kRainPeriodMs;

void SayWindow(uint8_t slot, Slot& sl, uint64_t now) {
    if (!sl.windowStartMs) sl.windowStartMs = now;
    sl.windowPeak = std::max(sl.windowPeak, sl.asked.size());
    if (now - sl.windowStartMs < kWindowMs) return;
    if (sl.windowAsks >= 4)
        UE_LOGI("grime_judge: slot %u asked %u time(s) in the last %llu ms; its queue held at most %zu key(s) (cap %zu), %zu "
                "answered unqueued", static_cast<unsigned>(slot), sl.windowAsks, static_cast<unsigned long long>(now - sl.windowStartMs),
                sl.windowPeak, kMaxPending, sl.windowOwed);
    sl.windowStartMs = now;
    sl.windowAsks = 0;
    sl.windowPeak = sl.asked.size();
    sl.windowOwed = 0;
}

void Tick(coop::net::Session& session) {
    SettleSeam();
    ArmSeam(session);
    g_refusals.Flush(coop::net::RefusalTally::Clock::now());
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    const uint64_t now = NowMs();
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBudget, NowMs, [&](Item& it) {
            return Execute(session, sl, it, slot) ? coop::net::Served::Done : coop::net::Served::Wait;
        });
        if (!sl.owed.empty()) AnswerOwed(session, slot, sl);
        SayWindow(slot, sl, now);
    }
}

void OnAsk(coop::net::Session& session, const coop::net::KeyedScalarPayload& payload, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    ++g_stats.asked;
    ++g_slots[senderSlot].windowAsks;
    if (ProbeLog()) {
        // The measure of a real stroke: the gap since the sender's previous ask (a sponge's overlap that touched several
        // decals asks each in one burst, so a gap near 0 is the same stroke).
        const uint64_t now = NowMs();
        Slot& from = g_slots[senderSlot];
        UE_LOGI("grime_judge: ask slot=%u value %.2f, %llu ms after its previous ask", static_cast<unsigned>(senderSlot),
                payload.value, static_cast<unsigned long long>(from.lastAskMs ? now - from.lastAskMs : 0));
        from.lastAskMs = now;
    }
    // The red arm: the host takes a client's message as it came, as the lane did before the judge.
    if (JudgeOff()) {
        coop::grime_sync::OnReliable(payload, senderSlot);
        return;
    }
    std::wstring key = coop::net::StringFromWireKey(payload.key);
    // Only the host ends a decal or sets a value as it is: a client's message of either kind, or naming nothing, is no fall.
    if (key.empty() || payload.adopt != 0 || payload.value == coop::net::kGrimeGone) {
        ++g_stats.refusedKind;
        Refuse(senderSlot, "refused kind: an ended mark, a snapshot or no key (only the host sends those)");
        return;
    }
    Slot& sl = g_slots[senderSlot];
    const auto queued = sl.asked.find(key);
    if (queued != sl.asked.end()) {
        // A later ask of a queued key is the decal's newer value: the lowest stands, and the one queue entry serves it.
        queued->second = std::min(queued->second, payload.value);
        ++g_stats.coalesced;
        return;
    }
    if (!sl.queue.Push(Item{key, 0}, kMaxPending)) {
        ++g_stats.refusedQueue;
        Refuse(senderSlot, "refused queue: the sender has the most keys queued it may");
        // Its copy of the decal stands lowered: the key is owed the host's value (a key the host holds no decal for is not).
        if (coop::grime_sync::DecalOfKey(key)) {
            sl.owed.insert(std::move(key));
            ++sl.windowOwed;
        }
        return;
    }
    sl.asked.emplace(std::move(key), payload.value);
}

void OnDisconnect() {
    const Stats& t = g_stats;
    if (t.asked || t.held || t.heldOff)
        UE_LOGI("grime_judge: session end -- asked=%llu coalesced=%llu granted=%llu cut=%llu ended=%llu (tool=%llu rain=%llu) "
                "already=%llu refused: kind=%llu queue=%llu nodecal=%llu nobody=%llu reach=%llu notool=%llu rain=%llu; "
                "dead=%llu; client destroys held=%llu left to run=%llu",
                static_cast<unsigned long long>(t.asked), static_cast<unsigned long long>(t.coalesced),
                static_cast<unsigned long long>(t.granted), static_cast<unsigned long long>(t.clamped),
                static_cast<unsigned long long>(t.ended), static_cast<unsigned long long>(t.toolGrants),
                static_cast<unsigned long long>(t.rainGrants), static_cast<unsigned long long>(t.already),
                static_cast<unsigned long long>(t.refusedKind), static_cast<unsigned long long>(t.refusedQueue),
                static_cast<unsigned long long>(t.refusedNodecal), static_cast<unsigned long long>(t.refusedNobody),
                static_cast<unsigned long long>(t.refusedReach), static_cast<unsigned long long>(t.refusedNotool),
                static_cast<unsigned long long>(t.refusedRain), static_cast<unsigned long long>(t.refusedDead),
                static_cast<unsigned long long>(t.held),
                static_cast<unsigned long long>(t.heldOff));
    g_stats = Stats{};
    g_rainLast.clear();
    g_refusals.Clear();
    g_heldInVerb = nullptr;   // a verb the session ended inside holds nothing for the next; Tick disarms the seam
}

}  // namespace coop::grime_judge
