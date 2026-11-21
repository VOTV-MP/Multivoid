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
// (prop_sponge.cpp:756-761). Set from the measure arm: `strength`'s 1.5 (prop_sponge.cpp:9) is the property's default and
// getStrength's body does not show it as the value it returns, so the host cannot prove it from the Blueprint.
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
// Provisional, set from the measure arm: how much of the rain's period a per-key cadence forgives (frame jitter and the
// transport's batching), and what a reach adds beyond the tool's trace, its overlap and the decal's box (the pose
// allowance is the authority's own).
constexpr uint64_t kRainPeriodSlackMs = 250;
constexpr float kReachSlackCm = 100.0f;
// A fall that lands within this of the host's value minus the step is no cut: float arithmetic on both sides.
constexpr float kStepEps = 0.01f;

// A sender's asks run from one ordered queue at a bounded rate. Provisional, set from the measure arm: the sponge's
// stroke rate times the decals one overlap touches.
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
    if (value >= host - kStepEps) {
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
    Route rain = RainRoute(s, slot, it.key, decal, centre, raining);
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
    if (ProbeLog())
        UE_LOGI("grime_judge: GRANT slot=%u key='%ls' asked %.2f of %.2f -> %.2f%s (step %.2f; tool %.0f uu, rain %.0f uu)",
                static_cast<unsigned>(slot), it.key.c_str(), value, host, granted, cut ? " cut" : "", step, tool.dist,
                rain.dist);
    return consume();
}

// ---- the client's hold -----------------------------------------------------------------------
// grime_C::clean destroys the decal inside its own body once its process is below zero (the K2_DestroyActor call at line
// 241 of the grime_C dump), and wallFixer_fix does the same (line 892 of the grime_crack dump), both ahead of anything the
// host could say. K2_DestroyActor is a native, which the script gate never sees, so the hold stands on the native
// function seam (ue_wrap/core/ufunction_hook), whose PRE can refuse a call with no result and no out parameter. It is armed
// only while a scope is open, and acts only on the scope's decal from a call whose own frame is clean or wallFixer_fix, so
// every other destroy of every actor, and the lane's own apply of the host's end, passes untouched. Whether the PRE fires
// for the destroy inside clean's body is what the measure arm reads.
UE_FACT_FN(kRowK2Destroy, "grime_judge", L"Actor", L"K2_DestroyActor", Declared);

namespace UH = ue_wrap::ufunction_hook;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};
bool  g_seamSettled = false;
void* g_seamFn = nullptr;

struct Scope {
    void* decal = nullptr;  // the grime decal whose clean or fix is running; null outside one
    bool  held = false;     // the scope cancelled that decal's destroy
};
Scope g_scope;

// The dev red arm of the hold: the client's destroy runs as the game wrote it, so the decal is gone before the host's word.
bool HoldOff() {
    static const bool s = ::coop::config::ResolveFlag(::coop::config_registry::rows::grime_hold_off);
    return s;
}

sg::Verdict OnDestroyPre(void* context, void* /*sourceObject*/, const uint8_t* /*parms*/) {
    if (!g_scope.decal || context != g_scope.decal) return sg::Verdict::Run;
    void* const caller = UH::CurrentCallerFrame().function;
    if (!caller || !(R::NameEquals(R::NameOf(caller), L"clean") || R::NameEquals(R::NameOf(caller), L"wallFixer_fix")))
        return sg::Verdict::Run;
    if (HoldOff()) {
        ++g_stats.heldOff;
        return sg::Verdict::Run;
    }
    g_scope.held = true;
    if (++g_stats.held <= 8)
        UE_LOGI("grime_judge: held the destroy of decal %p inside %ls (#%llu) -- the host is asked to end it", context,
                R::ToString(R::NameOf(caller)).c_str(), static_cast<unsigned long long>(g_stats.held));
    return sg::Verdict::Cancel;
}

// Puts the PRE on K2_DestroyActor, disarmed, once the Actor class is loaded: one attempt, since a patch that failed is
// not mended by trying it again (prop_lifecycle settles the same function the same way).
void SettleSeam() {
    if (g_seamSettled) return;
    void* const actorCls = ue_wrap::object_index::ClassByName(ue_wrap::profile::name::ActorClassName);
    if (!actorCls) return;
    g_seamSettled = true;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::ResolveOn(kRowK2Destroy, actorCls);
    if (!fn || !UH::InstallPreHook(fn, &OnDestroyPre, /*armed=*/false)) {
        UE_LOGE("grime_judge: the K2_DestroyActor pre hook did not go on -- a client's last stroke destroys its decal "
                "before the host is asked");
        return;
    }
    g_seamFn = fn.Fn();
    UE_LOGI("grime_judge: the client's destroy hold is on K2_DestroyActor @ %p", g_seamFn);
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

bool RoofClear(void* decal) { return decal && RoofOf(decal) == Roof::Clear; }

Stats GetStats() { return g_stats; }

void HoldOpen(void* decal) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!g_seamFn || !s || !s->connected() || s->role() != coop::net::Role::Client) return;
    // A clean of an object of a world this client has not announced ready is that world's load: it runs natively.
    if (!coop::net_pump::IsInAnnouncedWorld(decal)) return;
    g_scope = Scope{decal, false};
    UH::SetArmed(g_seamFn, &OnDestroyPre, true);
}

bool HoldClose(void* decal) {
    if (!g_seamFn || g_scope.decal != decal) return false;
    const bool held = g_scope.held;
    g_scope = Scope{};
    UH::SetArmed(g_seamFn, &OnDestroyPre, false);
    return held;
}

void Tick(coop::net::Session& session) {
    SettleSeam();
    g_refusals.Flush(coop::net::RefusalTally::Clock::now());
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBudget, NowMs, [&](Item& it) {
            return Execute(session, sl, it, slot) ? coop::net::Served::Done : coop::net::Served::Wait;
        });
    }
}

void OnAsk(coop::net::Session& session, const coop::net::KeyedScalarPayload& payload, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    ++g_stats.asked;
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
        return;
    }
    sl.asked.emplace(std::move(key), payload.value);
}

void OnDisconnect() {
    const Stats& t = g_stats;
    if (t.asked || t.held || t.heldOff)
        UE_LOGI("grime_judge: session end -- asked=%llu coalesced=%llu granted=%llu cut=%llu ended=%llu (tool=%llu rain=%llu) "
                "already=%llu refused: kind=%llu queue=%llu nodecal=%llu nobody=%llu reach=%llu notool=%llu rain=%llu; "
                "client destroys held=%llu left to run=%llu",
                static_cast<unsigned long long>(t.asked), static_cast<unsigned long long>(t.coalesced),
                static_cast<unsigned long long>(t.granted), static_cast<unsigned long long>(t.clamped),
                static_cast<unsigned long long>(t.ended), static_cast<unsigned long long>(t.toolGrants),
                static_cast<unsigned long long>(t.rainGrants), static_cast<unsigned long long>(t.already),
                static_cast<unsigned long long>(t.refusedKind), static_cast<unsigned long long>(t.refusedQueue),
                static_cast<unsigned long long>(t.refusedNodecal), static_cast<unsigned long long>(t.refusedNobody),
                static_cast<unsigned long long>(t.refusedReach), static_cast<unsigned long long>(t.refusedNotool),
                static_cast<unsigned long long>(t.refusedRain), static_cast<unsigned long long>(t.held),
                static_cast<unsigned long long>(t.heldOff));
    g_stats = Stats{};
    g_rainLast.clear();
    g_refusals.Clear();
    // A scope the session ended inside never closes: the seam goes back to disarmed.
    if (g_seamFn) HoldClose(g_scope.decal);
    g_scope = Scope{};
}

}  // namespace coop::grime_judge
