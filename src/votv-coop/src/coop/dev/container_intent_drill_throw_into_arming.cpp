// coop/dev/container_intent_drill_throw_into_arming.cpp -- the arming measure of a container's box (the dev row
// container_arming_measure, default off; BOTH peers, with container_intent_drill=throwinto). Measures only.
//   m1  a native post hook on UPrimitiveComponent::SetCollisionEnabled (ue_wrap/core/ufunction_hook). A call whose context is a
//       shared container's `overlap` component (the throwinto baskets, a roomba) is printed with the class, the collision read
//       before and after (the new value is the read after) and the caller; every call game-wide is counted, and the rate is
//       printed every 5 s. The observer's first statement is the pointer-set lookup; on a miss only this round's own counter runs.
//   m2  a PRE watch on prop_container_C::dingus (client): whether it sees the call made inline by getObject's bytecode, and from
//       which caller frame.
//   m3  the tail (after the five sub-runs): the host fills a basket, the client takes from it through the opened screen (the slot's
//       pressButton), and a drive is put INSIDE a disarmed basket's box before dingus arms it;
//       the route census's R4 counters say whether BeginOverlap or putObjectIn_overlap follows.
// The host ends with one answer line prefixed `arming`.

#include "container_intent_drill_throw_into.h"
#include "container_route_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_route_census.h"
#include "coop/dev/drill_chat.h"
#include "coop/props/container_act_request.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/actor_end_play.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::container_intent_drill::detail::into {

UE_FACT_FN(kRowSetCollision, "container_arming_measure", L"PrimitiveComponent", L"SetCollisionEnabled", Declared);
UE_FACT_FN(kRowDingus, "container_arming_measure", L"prop_container_C", L"dingus", Declared);

namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace UH  = ue_wrap::ufunction_hook;
namespace sg  = ue_wrap::script_gate;
namespace DC  = coop::dev::drill_chat;
namespace RD  = coop::dev::container_route_drill::detail;
namespace ci  = ue_wrap::container_inventory;
namespace CV  = ue_wrap::container_view;
namespace AR  = coop::props::container_act_request;

constexpr int      kDinguspreTag  = 72;
constexpr int      kSlots         = 64;        // a power of two: the pointer set's open-addressed table
constexpr uint64_t kRateEveryMs   = 5000;
constexpr uint64_t kRouteWindowMs = 1000;      // matched calls this long after a route began belong to it
constexpr uint64_t kTailBoundMs   = 60000;
constexpr int      kStillChecks   = 4;
constexpr const char* kTailStart  = "ci-tinto host tail-start";
constexpr const char* kFilled     = "ci-tinto host filled-pack";
constexpr const char* kTook       = "ci-tinto client took-pack";
constexpr const char* kPress      = "ci-tinto client press-pack";   // the client presses the slot: the host labels its hook prints "ui"
constexpr uint64_t kUiWindowMs    = 6000;      // the take's ask, grant, replay and commit, the BP's delay: matched calls in it belong to the UI route
constexpr uint64_t kUiSettleMs    = 1500;      // after the commit is sent: the host's arming edge and the mirror's box
constexpr const char* kNumbers    = "ci-tinto client tail-numbers ";

// The pointer set: the `overlap` components of the shared containers measured, and what each last read.
struct Slot { void* comp = nullptr; void* owner = nullptr; int state = -1; };
Slot g_slots[kSlots];

Slot* Find(void* comp) {
    unsigned i = static_cast<unsigned>(reinterpret_cast<uintptr_t>(comp) >> 4) & (kSlots - 1);
    for (int n = 0; n < kSlots; ++n) {
        Slot& s = g_slots[(i + static_cast<unsigned>(n)) & (kSlots - 1)];
        if (s.comp == comp) return &s;
        if (!s.comp) return nullptr;
    }
    return nullptr;
}

bool OwnerTracked(void* owner) {
    for (const Slot& s : g_slots)
        if (s.owner == owner && owner) return true;
    return false;
}

struct Counts { uint64_t dd = 0, ourcall = 0, ui = 0, roomba = 0, preSeen = 0, preInline = 0; };
Counts   g_c;
uint64_t g_calls = 0, g_matched = 0;                // every call game-wide; the matched ones
uint64_t g_t0 = 0, g_rateAt = 0, g_callsAt = 0, g_matchedAt = 0;
bool     g_hookOn = false, g_preOn = false, g_hookTried = false;
const char* g_label = "";
uint64_t g_labelUntil = 0;

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t ch : w) s.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    return s;
}

// m4: what the host measured of each try; the order is the last thrown try's.
struct M4Agg {
    int tries = 0, fired = 0;
    uint64_t noSim = 0, hostPreMs = 0, clientPreMs = 0, destroyMs = 0;
    float maxStep = 0.f, boxMin = 0.f;
};
M4Agg g_m4;
void* g_watchCopy = nullptr;   // the client's thrown copy (a pointer for an equality test, never dereferenced)
uint64_t g_copyDestroyMs = 0;

// The engine's end of play of the watched copy, stamped where it happens (the destroy seam); every other actor returns at once.
void OnEndPlay(void* actor, ue_wrap::actor_end_play::Reason reason) {
    if (actor != g_watchCopy || reason != ue_wrap::actor_end_play::Reason::Destroyed || g_copyDestroyMs) return;
    g_copyDestroyMs = cen::UnixMs();
}

std::string ClassText(void* obj) { return obj && R::IsLive(obj) ? Narrow(R::ClassNameOf(obj)) : std::string("?"); }

std::string FnText(void* fn) { return fn ? Narrow(R::ToString(R::NameOf(fn))) : std::string("-"); }

// The post observer: first statement the pointer-set lookup. A miss does nothing more than this round's own counter, which a
// production hook would not have.
void OnSetCollision(void* ctx, void* src, void*) {
    Slot* s = Find(ctx);
    ++g_calls;
    if (!s) return;
    ++g_matched;
    const int before = s->state;
    const int after = CollisionOf(s->comp);
    s->state = after;
    const bool roomba = ClassText(s->owner).find("roomba") != std::string::npos;
    const bool labelled = ::GetTickCount64() < g_labelUntil;
    if (roomba) ++g_c.roomba;
    else if (src == s->owner) ++g_c.dd;
    if (labelled && g_label[0] == 'o') ++g_c.ourcall;
    if (labelled && g_label[0] == 'u') ++g_c.ui;
    UE_LOGI("[CI-DRILL] arming m1 %s: SetCollisionEnabled on the overlap box of %s: before %d, after %d (the new value), caller %s in %s, "
            "route %s", Role(), ClassText(s->owner).c_str(), before, after, ClassText(src).c_str(),
            FnText(UH::CurrentCallerFrame().function).c_str(), labelled ? g_label : (roomba ? "roomba" : "none"));
}

// The client's PRE on dingus: seen from bytecode (a caller frame) or called by us straight; never cancelled.
sg::Verdict DingusPre(const sg::Call& c) {
    if (!OwnerTracked(c.object)) return sg::Verdict::Run;
    ++g_c.preSeen;
    const bool inl = c.callerFunction != nullptr;
    if (inl) ++g_c.preInline;
    UE_LOGI("[CI-DRILL] arming m2 %s: the PRE on dingus of %s ran %s (caller %s in %s, from our code %d)", Role(), ClassText(c.object).c_str(),
            inl ? "INLINE from bytecode" : "as a call of ours", ClassText(c.callerObject).c_str(), FnText(c.callerFunction).c_str(),
            c.fromOurCode ? 1 : 0);
    return sg::Verdict::Run;
}

double Per(uint64_t n, uint64_t ms) { return ms ? static_cast<double>(n) * 1000.0 / static_cast<double>(ms) : 0.0; }

}  // namespace


bool ArmingOn() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::container_arming_measure);
    return on;
}

void ArmingInstall(bool host) {
    if (!ArmingOn() || g_hookTried) return;
    g_hookTried = true;
    g_t0 = g_rateAt = ::GetTickCount64();
    g_hookOn = UH::InstallPostHook(ue_wrap::facts::Resolve(kRowSetCollision), &OnSetCollision);
    if (!host) g_preOn = sg::WatchName(kRowDingus, kDinguspreTag, &DingusPre, nullptr);
    ue_wrap::actor_end_play::AddSink(&OnEndPlay);   // the thrown copy's destroy, stamped by the engine's own end of play
    UE_LOGI("[CI-DRILL] arming %s: the SetCollisionEnabled hook is %s%s", Role(), g_hookOn ? "live" : "NOT installed",
            host ? "" : (g_preOn ? ", the dingus PRE watch is live" : ", the dingus PRE watch was refused"));
    // A roomba in the world is measured too: its box is written by its own upd(). Those present now join the set.
    for (void* r : R::FindObjectsByClass(L"prop_roomba_C")) ArmingTrack(r);
}

void ArmingTrack(void* container) {
    void* comp = TriggerComponent(container);
    if (!ArmingOn() || !comp || Find(comp)) return;
    unsigned i = static_cast<unsigned>(reinterpret_cast<uintptr_t>(comp) >> 4) & (kSlots - 1);
    for (int n = 0; n < kSlots; ++n) {
        Slot& s = g_slots[(i + static_cast<unsigned>(n)) & (kSlots - 1)];
        if (s.comp) continue;
        s = Slot{comp, container, CollisionOf(comp)};
        return;
    }
}

void ArmingRoute(const char* label, uint64_t windowMs) {
    if (!ArmingOn()) return;
    g_label = label;
    g_labelUntil = ::GetTickCount64() + (windowMs ? windowMs : kRouteWindowMs);
}


void ArmingWatchCopy(void* actor) {
    g_watchCopy = actor;
    g_copyDestroyMs = 0;
}

uint64_t ArmingCopyDestroyMs() { return g_copyDestroyMs; }

void ArmingM4Note(const M4Try& m) {
    if (!ArmingOn()) return;
    char destroy[24] = "none";
    if (m.clientDestroyMs) std::snprintf(destroy, sizeof(destroy), "%llu", static_cast<unsigned long long>(m.clientDestroyMs));
    UE_LOGI("[CI-DRILL] arming m4 n=%d try %d: the host's copy moved at most %.1f cm per sample within 100 cm of the box (%d samples that near), "
            "in %llu ms between those two samples = %.0f cm/s; the mean sample interval (the tick's own dt) is %.1f ms, so the per-FRAME step at "
            "that speed is %.1f cm (a per-ROW step would be nearer that speed x the rows' interval); the box's smallest extent is %.0f cm; the "
            "host's putObjectIn_overlap PRE was entered %llu time(s) by an item, %llu with the entering component not simulating; order: "
            "host-pre=%llu client-pre=%llu client-destroy=%s", m.r, m.t, static_cast<double>(m.maxStep), m.nearSamples,
            static_cast<unsigned long long>(m.stepDtMs), static_cast<double>(m.speed), static_cast<double>(m.frameMs),
            static_cast<double>(m.perFrame), static_cast<double>(m.boxMin), static_cast<unsigned long long>(m.hostOverlaps),
            static_cast<unsigned long long>(m.hostNoSim), static_cast<unsigned long long>(m.hostPreMs),
            static_cast<unsigned long long>(m.clientPreMs), destroy);
    ++g_m4.tries;
    if (m.hostOverlaps > 0) ++g_m4.fired;
    g_m4.noSim += m.hostNoSim;
    g_m4.maxStep = (std::max)(g_m4.maxStep, m.maxStep);
    g_m4.boxMin = g_m4.boxMin > 0.f ? (std::min)(g_m4.boxMin, m.boxMin) : m.boxMin;
    if (m.r == 1) {   // the order is the thrown try's
        g_m4.hostPreMs = m.hostPreMs;
        g_m4.clientPreMs = m.clientPreMs;
        g_m4.destroyMs = m.clientDestroyMs;
    }
}
void ArmingTick() {
    if (!g_hookTried) return;
    const uint64_t now = ::GetTickCount64();
    if (now < g_rateAt + kRateEveryMs) return;
    UE_LOGI("[CI-DRILL] arming rate %s: %.1f SetCollisionEnabled calls/s game-wide (matched %.2f/s) over the last %.1f s; %llu calls in all",
            Role(), Per(g_calls - g_callsAt, now - g_rateAt), Per(g_matched - g_matchedAt, now - g_rateAt),
            static_cast<double>(now - g_rateAt) / 1000.0, static_cast<unsigned long long>(g_calls));
    g_rateAt = now;
    g_callsAt = g_calls;
    g_matchedAt = g_matched;
}

// ---- the tail ---------------------------------------------------------------------------------------------------------------

namespace {

enum class T : uint8_t { Start, Filled, Took, Spawn3, Settle3, Drive3, SettleDrive, Wait3, RoombaSpawn, RoombaSettle, RoombaToggle,
                         Numbers, Done, WaitFilled, WaitRecord, Open, WaitView, Press, WaitCommit, WaitDelay, WaitEnd };
T        g_t = T::Start;
uint64_t g_tMs = 0;
ue_wrap::CachedObjRef g_basket, g_b3, g_drive3, g_roomba;
uint32_t g_basketEid = 0;
int      g_still = 0;
FVector  g_last{};
TriggerBox g_box3;
uint64_t g_ov0 = 0, g_pre0 = 0, g_adds0 = 0;
int      g_m3 = -1;               // late-arm overlap: -1 not measured, 0 no, 1 yes
uint32_t g_take0 = 0;             // the take commits this peer had sent when the slot was pressed
bool     g_pressSeen = false;

void Go(T t) { g_t = t; g_tMs = ::GetTickCount64(); g_still = 0; g_last = FVector{}; }
bool Over(uint64_t ms) { return ::GetTickCount64() - g_tMs > ms; }

bool Still(void* actor) {
    FVector at{};
    if (!actor || !E::TryGetActorLocation(actor, at)) return false;
    const bool still = std::fabs(at.X - g_last.X) + std::fabs(at.Y - g_last.Y) + std::fabs(at.Z - g_last.Z) < 1.f;
    g_last = at;
    g_still = still ? g_still + 1 : 0;
    return g_still >= kStillChecks;
}

TailState TFail(char* why, size_t len, const char* text) {
    std::snprintf(why, len, "%s", text);
    return TailState::Failed;
}

void PrintNumbers(const char* who) {
    UE_LOGI("[CI-DRILL] arming %s numbers: dingus-delayed=%llu ourcall=%llu client-ui=%llu roomba=%llu; PRE saw dingus %llu time(s), %llu "
            "inline; %llu calls game-wide, %llu matched", who, static_cast<unsigned long long>(g_c.dd), static_cast<unsigned long long>(g_c.ourcall),
            static_cast<unsigned long long>(g_c.ui), static_cast<unsigned long long>(g_c.roomba), static_cast<unsigned long long>(g_c.preSeen),
            static_cast<unsigned long long>(g_c.preInline), static_cast<unsigned long long>(g_calls), static_cast<unsigned long long>(g_matched));
}

TailState HostTick(bool due, char* why, size_t len) {
    if (!due) return TailState::Working;
    if (g_t != T::Done && Over(kTailBoundMs)) return TFail(why, len, "the arming measure tail ran past its bound");
    switch (g_t) {
    case T::Start:
        DC::Say(std::string(kTailStart) + " eid=" + std::to_string(g_basketEid));   // the client takes from this very basket
        if (!FillBasket(g_basket.Get())) return TFail(why, len, "the basket did not take the tail's drive");
        DC::Say(kFilled);
        Go(T::Took);
        return TailState::Working;
    case T::Took:   // the client's take, through the opened screen
        if (DC::SeenHas(kPress) && !g_pressSeen) {   // the client presses the slot: this peer's hook prints belong to the UI route
            g_pressSeen = true;
            ArmingRoute("ui", kUiWindowMs);
        }
        if (DC::SeenHas(kTook)) Go(T::Spawn3);
        return TailState::Working;
    case T::Spawn3: {   // m3: a basket that is NOT armed
        void* cls = R::FindClass(L"prop_container_pbasket_C");
        if (!cls) cls = ue_wrap::asset_load::LoadObjectByPath(kBasketPath);
        FVector at{};
        if (!cls || !g_basket.Get() || !E::TryGetActorLocation(g_basket.Get(), at)) return TFail(why, len, "the tail's third basket has no place");
        void* b3 = E::SpawnActor(cls, {at.X, at.Y + 150.f, at.Z});
        if (!b3 || !ci::IsContainer(b3)) return TFail(why, len, "the tail's third basket did not spawn as a container");
        g_b3.Set(b3);
        Go(T::Settle3);
        return TailState::Working;
    }
    case T::Settle3:
        if (Still(g_b3.Get())) Go(T::Drive3);
        else if (!g_b3.Get()) return TFail(why, len, "the tail's third basket died");
        return TailState::Working;
    case T::Drive3: {   // a drive INSIDE the disarmed box
        void* cls = R::FindClass(RD::kDriveClass);
        if (!cls || !ReadTriggerBox(g_b3.Get(), g_box3)) return TFail(why, len, "the third basket's box does not read");
        void* d = E::SpawnActor(cls, g_box3.loc);
        if (!d) return TFail(why, len, "the tail's drive did not spawn");
        g_drive3.Set(d);
        UE_LOGI("[CI-DRILL] arming host m3: the third basket's box is collision-enabled %d (disarmed), a drive is spawned at its centre (%.0f,%.0f,%.0f)",
                TriggerState(g_b3.Get()), static_cast<double>(g_box3.loc.X), static_cast<double>(g_box3.loc.Y), static_cast<double>(g_box3.loc.Z));
        Go(T::SettleDrive);
        return TailState::Working;
    }
    case T::SettleDrive: {
        if (!g_drive3.Get()) return TFail(why, len, "the tail's drive died before the box was armed");
        if (!Still(g_drive3.Get())) return TailState::Working;
        FVector at{};
        E::TryGetActorLocation(g_drive3.Get(), at);
        UE_LOGI("[CI-DRILL] arming host m3: the drive rests at (%.0f,%.0f,%.0f), %s the box; arming the box with dingus now", static_cast<double>(at.X),
                static_cast<double>(at.Y), static_cast<double>(at.Z), PointInBox(g_box3, at) ? "INSIDE" : "outside");
        g_ov0 = cen::R4Overlaps();
        g_pre0 = cen::RouteEntries(3);
        g_adds0 = cen::RouteVerbs(3, false);
        ArmingTrack(g_b3.Get());
        ArmingRoute("m3");
        if (!CallDingus(g_b3.Get())) return TFail(why, len, "dingus did not run on the third basket");
        Go(T::Wait3);
        return TailState::Working;
    }
    case T::Wait3: {   // the BP's overlapDelay (0.1 s) and a BeginOverlap's tick
        if (!Over(kDingusWaitMs + 500)) return TailState::Working;
        const uint64_t ov = cen::R4Overlaps() - g_ov0, pre = cen::RouteEntries(3) - g_pre0, adds = cen::RouteVerbs(3, false) - g_adds0;
        g_m3 = (ov > 0 || pre > 0) ? 1 : 0;
        UE_LOGI("[CI-DRILL] arming host m3: the box read %d after dingus; the drive was inside it when the box armed: R4 overlaps +%llu, "
                "putObjectIn_overlap PRE +%llu, addObject on the overlap route +%llu -> late-arm overlap=%d", TriggerState(g_b3.Get()),
                static_cast<unsigned long long>(ov), static_cast<unsigned long long>(pre), static_cast<unsigned long long>(adds), g_m3);
        Go(T::RoombaSpawn);
        return TailState::Working;
    }
    case T::RoombaSpawn: {   // the roomba: its own upd() writes its box with no dingus, on every peer
        void* cls = R::FindClass(L"prop_roomba_C");
        if (!cls) cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_roomba.prop_roomba_C");
        FVector at{};
        if (!cls || !g_basket.Get() || !E::TryGetActorLocation(g_basket.Get(), at)) {
            UE_LOGI("[CI-DRILL] arming host roomba: prop_roomba_C does not resolve (class %p); the roomba is not measured", cls);
            Go(T::Numbers);
            return TailState::Working;
        }
        void* r = E::SpawnActor(cls, {at.X, at.Y - 150.f, at.Z + 40.f});
        if (!r) {
            UE_LOGI("[CI-DRILL] arming host roomba: the roomba did not spawn; it is not measured");
            Go(T::Numbers);
            return TailState::Working;
        }
        g_roomba.Set(r);
        ArmingTrack(r);
        Go(T::RoombaSettle);
        return TailState::Working;
    }
    case T::RoombaSettle:
        if (!g_roomba.Get()) {
            Go(T::Numbers);
            return TailState::Working;
        }
        if (Still(g_roomba.Get())) Go(T::RoombaToggle);
        return TailState::Working;
    case T::RoombaToggle: {   // its `active` bool switched on and then off, upd() called after each: the native hook catches each write
        void* r = g_roomba.Get();
        int32_t off = -1;
        uint8_t mask = 0;
        if (r && R::FindBoolProperty(R::ClassOf(r), L"active", off, mask)) {
            uint8_t* byte = static_cast<uint8_t*>(r) + off;
            const uint64_t roomba0 = g_c.roomba;
            const int before = TriggerState(r);
            for (const bool on : {true, false}) {
                *byte = static_cast<uint8_t>(on ? (*byte | mask) : (*byte & ~mask));
                ArmingRoute("roomba");
                const bool ran = CallParamless(r, L"upd");
                UE_LOGI("[CI-DRILL] arming host roomba: active=%d, upd() %s, the box reads %d", on ? 1 : 0, ran ? "ran" : "did not run", TriggerState(r));
            }
            UE_LOGI("[CI-DRILL] arming host roomba: the box read %d before, the native hook caught %llu write(s) of upd() on it", before,
                    static_cast<unsigned long long>(g_c.roomba - roomba0));
        } else {
            UE_LOGI("[CI-DRILL] arming host roomba: its `active` bool does not resolve; the roomba is not measured");
        }
        if (r) E::DestroyActor(r);
        Go(T::Numbers);
        return TailState::Working;
    }
    case T::Numbers: {
        std::string rest;
        if (!DC::SeenRestOf(kNumbers, &rest)) return TailState::Working;
        long long cdd = 0, coc = 0, cui = 0, crm = 0, cinl = 0;
        Num(rest, "dd", cdd);
        Num(rest, "ourcall", coc);
        Num(rest, "ui", cui);
        Num(rest, "roomba", crm);
        Num(rest, "inline", cinl);
        PrintNumbers("host");
        const uint64_t ms = ::GetTickCount64() - g_t0;
        char destroy[24] = "none";
        if (g_m4.destroyMs) std::snprintf(destroy, sizeof(destroy), "%llu", static_cast<unsigned long long>(g_m4.destroyMs));
        UE_LOGI("[CI-DRILL] arming m1 hook fires: dingus-delayed=%llu ourcall=%llu client-ui=%llu roomba=%llu; m2 pre sees inline=%d; m3 late-arm "
                "overlap=%d; rate %.1f (matched %.2f/s); m4 host-parked-overlap=%d/%d (sim=0 at entry %llu) maxstep=%.1f boxmin=%.0f; order "
                "host-pre=%llu client-pre=%llu client-destroy=%s", static_cast<unsigned long long>(g_c.dd + static_cast<uint64_t>(cdd)),
                static_cast<unsigned long long>(g_c.ourcall + static_cast<uint64_t>(coc)), static_cast<unsigned long long>(g_c.ui + static_cast<uint64_t>(cui)),
                static_cast<unsigned long long>(g_c.roomba + static_cast<uint64_t>(crm)), cinl > 0 ? 1 : 0, g_m3 > 0 ? 1 : 0, Per(g_calls, ms),
                Per(g_matched, ms), g_m4.fired, g_m4.tries, static_cast<unsigned long long>(g_m4.noSim), static_cast<double>(g_m4.maxStep),
                static_cast<double>(g_m4.boxMin), static_cast<unsigned long long>(g_m4.hostPreMs), static_cast<unsigned long long>(g_m4.clientPreMs),
                destroy);
        Go(T::Done);
        return TailState::Done;
    }
    default:
        return TailState::Working;
    }
}

TailState ClientTick(bool due, char* why, size_t len) {
    if (!due) return TailState::Working;
    if (Over(kTailBoundMs)) return TFail(why, len, "the arming measure tail ran past its bound");
    switch (g_t) {
    case T::WaitFilled:
        if (DC::SeenHas(kFilled)) Go(T::WaitRecord);
        return TailState::Working;
    case T::WaitRecord:   // the host's record reaches the mirror
        if (CountOf(g_basketEid) >= 1) Go(T::Open);
        return TailState::Working;
    case T::Open:   // the take is the game's own, as the take arms drive it: the screen opens on the basket, then the slot is pressed
        if (!CV::Open(g_basket.Get())) return TFail(why, len, "openPropInv did not dispatch on the client's mirror");
        Go(T::WaitView);
        return TailState::Working;
    case T::WaitView:
        if (CV::Viewed() == g_basket.Get()) Go(T::Press);
        else if (Over(10000)) return TFail(why, len, "the screen does not show the basket 10 s after openPropInv");
        return TailState::Working;
    case T::Press: {
        void* slot = CV::FirstSlot(CV::InventoryScreen());
        CV::Slot s{};
        if (!slot || !CV::ReadSlot(slot, s)) return TFail(why, len, "the open screen has no readable first container slot");
        g_take0 = AR::dev::LastTake().count;
        DC::Say(kPress);   // the host labels its hook prints from here
        ArmingRoute("ui", kUiWindowMs);
        const bool pressed = CV::PressSlot(slot);
        UE_LOGI("[CI-DRILL] arming client m1: the take route is the slot's pressButton (R1) on the opened screen, slot id %d: pressed=%d, pending act "
                "id %u", s.id, pressed ? 1 : 0, static_cast<unsigned>(AR::dev::PendingId()));
        if (!pressed) return TFail(why, len, "the slot's pressButton did not dispatch");
        Go(T::WaitCommit);
        return TailState::Working;
    }
    case T::WaitCommit:   // the take's commit is sent: the host's grant, the replay and the commit have run
        if (AR::dev::LastTake().count != g_take0) {
            UE_LOGI("[CI-DRILL] arming client m1: the take committed (%s); the host's arming edge follows", AR::dev::LastTake().committed ? "done" : "aborted");
            Go(T::WaitDelay);
        } else if (Over(15000)) {
            return TFail(why, len, "no take commit 15 s after the press (the grant or the ask was refused)");
        }
        return TailState::Working;
    case T::WaitDelay:   // the host's arming edge and the mirror's box
        if (!Over(kUiSettleMs)) return TailState::Working;
        UE_LOGI("[CI-DRILL] arming client m1: after the UI take the mirror's box reads collision-enabled %d (0 none, 1 query only)", TriggerState(g_basket.Get()));
        CV::Close();
        PrintNumbers("client");
        DC::Say(kTook);
        {
            char say[160];
            std::snprintf(say, sizeof(say), "%sdd=%llu ourcall=%llu ui=%llu roomba=%llu inline=%llu", kNumbers, static_cast<unsigned long long>(g_c.dd),
                          static_cast<unsigned long long>(g_c.ourcall), static_cast<unsigned long long>(g_c.ui),
                          static_cast<unsigned long long>(g_c.roomba), static_cast<unsigned long long>(g_c.preInline));
            DC::Say(say);
        }
        Go(T::WaitEnd);
        return TailState::Working;
    case T::WaitEnd:
        return DC::SeenHas(kDoneLine) ? TailState::Done : TailState::Working;
    default:
        return TailState::Working;
    }
}

}  // namespace

void ArmingTailBegin(bool host, void* basket, uint32_t basketEid) {
    g_basket.Set(basket);
    g_basketEid = basketEid;
    Go(host ? T::Start : T::WaitFilled);
}

TailState ArmingTailTick(bool host, bool due, char* why, size_t whyLen) {
    return host ? HostTick(due, why, whyLen) : ClientTick(due, why, whyLen);
}

bool ArmingTailAsked(uint32_t* eid) {
    std::string rest;
    long long e = 0;
    if (!DC::SeenRestOf(std::string(kTailStart) + " ", &rest)) return false;
    if (eid && Num(rest, "eid", e)) *eid = static_cast<uint32_t>(e);
    return true;
}

// A second arm in the same process starts from nothing: the pointer set, the counters, the route label and the tail's state. The hook
// and the end-of-play sink stay registered (the engine has no unpatch and no removal of a sink, and ArmingInstall is idempotent); with
// no watched copy the sink returns at once.
void ArmingReset() {
    for (Slot& s : g_slots) s = Slot{};
    g_c = Counts{};
    g_calls = g_matched = g_callsAt = g_matchedAt = 0;
    g_t0 = g_rateAt = ::GetTickCount64();
    g_label = "";
    g_labelUntil = 0;
    g_m4 = M4Agg{};
    g_watchCopy = nullptr;
    g_copyDestroyMs = 0;
    g_t = T::Start;
    g_tMs = 0;
    g_still = 0;
    g_last = FVector{};
    g_basket.Reset();
    g_b3.Reset();
    g_drive3.Reset();
    g_roomba.Reset();
    g_box3 = TriggerBox{};
    g_ov0 = g_pre0 = g_adds0 = 0;
    g_m3 = -1;
    g_take0 = 0;
    g_pressSeen = false;
    g_basketEid = 0;
}

}  // namespace coop::dev::container_intent_drill::detail::into
