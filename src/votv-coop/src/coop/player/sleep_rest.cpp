// coop/player/sleep_rest.cpp -- see coop/player/sleep_rest.h.

#include "coop/player/sleep_rest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/player/sleep_sync.h"

#include "ue_wrap/actors/effects.h"
#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_singleton.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

namespace coop::sleep_rest {
namespace {

namespace V  = ue_wrap::vitals;
namespace SLP = ue_wrap::sleep;
namespace FX = ue_wrap::effects;
namespace EN = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;

// The factor the game's own sleep sets is kNativeSleepDilation (the header; mainGamemode.cpp:5428,
// SetGlobalTimeDilation(20)). The row below is the census's entry for the `sleep` body that holds that literal
// (include/ue_wrap/facts/fingerprints_0.9.0n.inc), so a game update that changes the body is named at boot; the
// watch compares the factor it finds in the same call.
constexpr int kTagSleepRest = 0x534C5253;  // 'SLRS'
constexpr int kTagWakeRest = 0x574B5253;   // 'WKRS'
UE_FACT_FN(kRowSleep, "sleep_rest", L"mainGamemode_C", L"sleep", Declared);
// The rest's end: the game sets isSleep false in one place, the body `wakeup` runs (mainGamemode.cpp:5282, reached from
// :5395-5409 and :5447), and every wake (the sleep need at 100, an event, a nightmare's own call) goes through it.
UE_FACT_FN(kRowWakeup, "sleep_rest", L"mainGamemode_C", L"wakeup", Declared);

std::atomic<coop::net::Session*> g_session{nullptr};

// One script-gate watch, registered and asked about until it is live or settled dead.
struct Watch {
    const ue_wrap::facts::Function* row;
    int tag;
    sg::PostFn post;
    const char* what;
    bool registered = false;
    bool settled = false;
    bool live = false;
    bool saidFail = false;
};
void OnSleepPost(const sg::Call&);
void OnWakePost(const sg::Call&);
Watch g_sleepWatch{&kRowSleep, kTagSleepRest, &OnSleepPost, "sleep"};
Watch g_wakeWatch{&kRowWakeup, kTagWakeRest, &OnWakePost, "wakeup"};
uint64_t g_nextWatchMs = 0;  // the watches are asked about at most once a second
bool g_saidFactor = false;  // a factor other than the pinned one is said once, whatever the cause

// The rest in progress. The gamemode and the body are held by identity: a world change or a new body ends it.
bool g_resting = false;
bool g_hostDeclared = false;  // a host's sleep was declared once since its session began
ue_wrap::CachedObjRef g_gm;
ue_wrap::CachedObjRef g_body;
// Each effect actor the rest dilated, so the end puts back those and not the list as it stands then:
// effect_poo leaves the list with its actor alive. The game keeps one effect per kind.
constexpr int kMaxDilatedEffects = 16;
ue_wrap::CachedObjRef g_dilated[kMaxDilatedEffects];
int  g_dilatedCount = 0;
bool g_saidDilatedFull = false;
// Food and coffee as the last tick read them (after our own write). Unset on the first tick of a rest, which
// takes the baselines and corrects nothing.
bool  g_haveFood = false;
bool  g_haveCoffee = false;
float g_foodBase = 0.f;
float g_coffeeBase = 0.f;

bool ConnectedClient() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() && s->role() == coop::net::Role::Client;
}

// The session while this peer hosts one that runs, else null.
coop::net::Session* HostSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Host ? s : nullptr;
}

void HostEval();

// The local player's body: the registry's (warm while the controller holds the sleeping pawn), else the
// pawn `sleep` took from the controller.
void* CurrentBody() {
    if (void* b = coop::players::Registry::Get().Local()) return b;
    return SLP::OrigPawn();
}

// An effect that ended mid-rest (lsd and foodPoison destroy themselves) leaves a dead entry; its slot is freed.
void ReclaimDeadEffects() {
    for (int i = 0; i < g_dilatedCount;) {
        if (g_dilated[i].Alive()) { ++i; continue; }
        g_dilated[i] = g_dilated[--g_dilatedCount];
        g_dilated[g_dilatedCount].Reset();
    }
}

// Dilate one effect and remember it. One the table cannot hold, even after the dead entries are reclaimed, is
// left alone and said once: an actor not remembered could not be put back.
void DilateEffect(void* effect, void* ctx) {
    bool known = false;
    for (int i = 0; i < g_dilatedCount && !known; ++i) known = g_dilated[i].Is(effect);
    if (!known) {
        if (g_dilatedCount >= kMaxDilatedEffects) ReclaimDeadEffects();
        if (g_dilatedCount >= kMaxDilatedEffects) {
            if (!g_saidDilatedFull) {
                g_saidDilatedFull = true;
                UE_LOGW("sleep_rest: more than %d effect actors in the world -- the extra ones stay at factor 1",
                        kMaxDilatedEffects);
            }
            return;
        }
        g_dilated[g_dilatedCount++].Set(effect);
    }
    EN::SetCustomTimeDilation(effect, *static_cast<const float*>(ctx));
}

bool BeginRest() {
    void* const gm = ue_wrap::world_singleton::Gamemode();
    void* const body = CurrentBody();
    if (!gm || !body) {
        // The degrade: no body to dilate, so the world stays at 1 and the sleep runs at 1/20 of the game's speed.
        UE_LOGW("sleep_rest: a rest began with no local body (%p) or gamemode (%p) -- it restores at the world's rate",
                body, gm);
        return false;
    }
    if (!EN::SetCustomTimeDilation(body, kNativeSleepDilation)) return false;
    g_gm.Set(gm);
    g_body.Set(body);
    g_resting = true;
    g_haveFood = g_haveCoffee = false;
    UE_LOGI("sleep_rest: rest begun -- the world stays at 1, the body and the world's effects run at %.0f",
            kNativeSleepDilation);
    return true;
}

void EndRest(const char* why) {
    if (void* b = g_body.Get()) EN::SetCustomTimeDilation(b, 1.f);
    for (int i = 0; i < g_dilatedCount; ++i) {
        if (void* e = g_dilated[i].Get()) EN::SetCustomTimeDilation(e, 1.f);
        g_dilated[i].Reset();
    }
    g_dilatedCount = 0;
    g_resting = false;
    g_gm.Reset();
    g_body.Reset();
    g_haveFood = g_haveCoffee = false;
    g_saidDilatedFull = false;  // said once per rest
    UE_LOGI("sleep_rest: rest ended (%s) -- the body and the effects are back at 1", why);
}

// The call has run: the game set the world's dilation and the sleeping flag inside it. A refusal leaves the
// flag down and nothing to undo. Our own reflected calls come through here as the game's do. The game has no
// isSleep guard on `sleep` (mainGamemode.cpp:5495-5583), so a second call while resting sets the world's
// factor again: it is put back to 1 here as well, and begins no new rest. A second call the game refused
// leaves the world at the 1 the rest holds, which is legal and returns before the factor check.
void OnSleepPost(const sg::Call&) {
    if (HostSession()) return HostEval();  // the host's own sleep begins: the row and who is awake decide its factor
    if (!ConnectedClient() || !g_wakeWatch.live) return;
    float asleep = 0.f;
    if (!V::Read(V::Field::Sleeping, &asleep) || asleep == 0.f) return;
    if (!SLP::EnsureResolved()) {
        UE_LOGE("sleep_rest: a sleep began and the dilation calls do not resolve -- the world stays at the game's factor");
        return;
    }
    const float found = SLP::GetGlobalTimeDilation();
    // Inside a rest the world already runs at 1: a second call the game refused set nothing, and there is
    // nothing to compare or undo.
    if (g_resting && std::fabs(found - 1.f) <= 0.01f) return;
    if (std::fabs(found - kNativeSleepDilation) > 0.01f && !g_saidFactor) {
        g_saidFactor = true;
        UE_LOGW("sleep_rest: the game's sleep set a world dilation of %.3f, not the pinned %.1f -- a game update "
                "changed the speed-up; the body and the effects are given the pinned factor all the same",
                found, kNativeSleepDilation);
    }
    if (!SLP::SetGlobalTimeDilation(1.f)) {
        UE_LOGE("sleep_rest: the world's dilation could not be set back to 1 after the game's sleep");
        return;
    }
    if (!g_resting) BeginRest();
}

// The call has run: the game's own wake, whoever called it (the sleep need at 100, an event, the nightmare's own
// call), has put the world's dilation and the sleeping flag back. A call that left the flag up (a refusal) ends
// nothing.
void OnWakePost(const sg::Call&) {
    if (HostSession()) return HostEval();  // the host woke (or a refused call left it asleep: nothing changes)
    if (!g_resting || !ConnectedClient()) return;
    float asleep = 0.f;
    if (!V::Read(V::Field::Sleeping, &asleep) || asleep != 0.f) return;
    EndRest("the game's wakeup ran");
}

// Register the watch, then ask the gate to resolve its name until it is live or settled dead. A registration the
// gate refuses is said once, and asked again at the caller's pace.
void SettleWatch(Watch& w) {
    if (w.settled) return;
    if (!w.registered) w.registered = sg::WatchName(*w.row, w.tag, nullptr, w.post);
    if (!w.registered) {
        if (!w.saidFail) {
            w.saidFail = true;
            UE_LOGW("sleep_rest: the gate did not take the %s watch -- asked again each second", w.what);
        }
        return;
    }
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(*w.row, w.tag)) {
        w.settled = w.live = true;
        UE_LOGI("sleep_rest: this peer's %s is seen at the call (a script-gate watch on mainGamemode_C::%s)", w.what,
                w.what);
    } else if (sg::NameWatchSettled(*w.row, w.tag)) {
        w.settled = true;
        UE_LOGE("sleep_rest: the %s watch is dead -- this peer's rest is not carried out", w.what);
    }
}

// At most once a second. A rest begins only with both watches live: one that began and could not end would hold the
// body at the native factor for good.
void EnsureWatch() {
    if (g_sleepWatch.settled && g_wakeWatch.settled) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextWatchMs) return;
    g_nextWatchMs = now + 1000;
    SettleWatch(g_sleepWatch);
    SettleWatch(g_wakeWatch);
}

// ---- HOST: the world rule world.sleep_speedup ----

enum class Speedup { Host, Everyone, Never };

// The row's session value: host = the host's own sleep speeds the world up (the game's, as in single player);
// everyone = it does only while every connected player is asleep; never = no one's sleep does.
Speedup ReadSpeedup() {
    // [dev] The red arm of the sleep speed-up drill, latched once: the host ignores the row and acts as `host`.
    static const bool ruleOff = coop::config::ResolveFlag(coop::config_registry::rows::sleep_speedup_rule_off);
    if (ruleOff) return Speedup::Host;
    const std::string v = coop::config::ResolveEnum(coop::config_registry::rows::world_sleep_speedup);
    return v == "host" ? Speedup::Host : v == "never" ? Speedup::Never : Speedup::Everyone;
}

// The host's sleep rests at 1 as a client's does: under `never` always; under `everyone` while a connected client is
// awake (a joiner still connecting counts as awake, as it is not asleep). Under `host` never.
bool HostRestsAtOne(coop::net::Session& s) {
    switch (ReadSpeedup()) {
    case Speedup::Host: return false;
    case Speedup::Never: return true;
    case Speedup::Everyone: break;
    }
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot)
        if (s.IsSlotConnected(slot) && coop::sleep_sync::PeerState(slot) == coop::net::SleepPeerState::Awake) return true;
    return false;
}

// What the host's sleep should be, from the session, whether the host is asleep, the row and who is awake.
enum class Want { NotAsleep, RestAtOne, GameFactor };

// The world's dilation at `v`: read first, set only when it differs (the game has no guard on a second `sleep` call,
// which sets its own factor again, and nothing else is told of it). A failed set is logged. False when it is not at `v`.
bool HoldWorldAt(float v) {
    const float now = SLP::GetGlobalTimeDilation();
    if (now > 0.f && std::fabs(now - v) <= 0.01f) return true;
    if (SLP::SetGlobalTimeDilation(v)) {
        UE_LOGI("sleep_rest: the world's dilation set from %.2f to %.0f for the host's sleep", now, v);
        return true;
    }
    UE_LOGE("sleep_rest: the world's dilation could not be set to %.0f for the host's sleep (it reads %.2f)", v, now);
    return false;
}

// The host's sleep is declared, not stepped: the wanted state comes from (a session runs, the host is asleep, the row,
// a connected client awake) and is made so, whichever state it was left in. RestAtOne is the world at 1 with the body
// and the effects at the factor, a client's rest; GameFactor is the game's own sleep, the world at its factor. Every
// cause is announced, so each runs this and nothing polls: the host's own `sleep` and `wakeup` (the script gate's
// watches), a peer's rest state (sleep_sync's subscriber), a player's arrival or leaving (the roster's), the row (the
// config subscriber), the last client leaving (OnDisconnect), and a rest that ended with the host still asleep (Tick).
void HostEval() {
    auto* const s = HostSession();
    if (!s || !SLP::EnsureResolved()) return;
    Want want = !SLP::IsSleeping() ? Want::NotAsleep : HostRestsAtOne(*s) ? Want::RestAtOne : Want::GameFactor;
    // A rest the game's own wake could not end would hold the body at the factor for good: the game's sleep stands.
    if (want == Want::RestAtOne && !g_wakeWatch.live) want = Want::GameFactor;
    switch (want) {
    case Want::NotAsleep:  // the game's `wakeup` has put the world back
        if (g_resting) EndRest("the host woke");
        return;
    case Want::RestAtOne:
        if (!HoldWorldAt(1.f)) return;
        if (g_resting) return;
        if (BeginRest())
            UE_LOGI("sleep_rest: the host's sleep does not speed the world up now (world.sleep_speedup) -- the world "
                    "stays at 1");
        else
            UE_LOGW("sleep_rest: the host's sleep should rest at 1 (world.sleep_speedup) -- the world is at 1, "
                    "but no rest began, so the body and the effects run at the world's rate");
        return;
    case Want::GameFactor:
        if (g_resting) EndRest("the host's sleep speeds the world up now (world.sleep_speedup)");
        HoldWorldAt(kNativeSleepDilation);
        return;
    }
}

// Slot 0 is the host's own state, which its `sleep` and `wakeup` watches already announce.
void OnPeerRest(int slot, coop::net::SleepPeerState) {
    if (slot > 0) HostEval();
}

// A player arrives (the roster row is born when its slot is ready: it is awake, so under `everyone` the world drops to
// 1 at its join, the join of a peer mid-sleep included) or leaves (only connected slots count, so a leaver does not).
void OnSlotReplaced(int slot, const coop::roster_ledger::Row&, const coop::roster_ledger::Row&) {
    if (slot > 0) HostEval();
}

// A fall since the last read: food gets (1 - 1/F) of it back, coffee loses (F - 1) times it more. A rise
// is not touched, and the baseline is read again after the write.
void SettleNeeds() {
    constexpr float kF = kNativeSleepDilation;
    float food = 0.f, coffee = 0.f;
    if (V::Read(V::Field::Food, &food)) {
        const float fall = g_haveFood ? g_foodBase - food : 0.f;
        if (fall > 0.f && V::Write(V::Field::Food, food + fall * (1.f - 1.f / kF))) V::Read(V::Field::Food, &food);
        g_foodBase = food;
        g_haveFood = true;
    }
    if (V::Read(V::Field::CoffeePower, &coffee)) {
        const float fall = g_haveCoffee ? g_coffeeBase - coffee : 0.f;
        if (fall > 0.f && V::Write(V::Field::CoffeePower, std::max(coffee - fall * (kF - 1.f), 0.f)))
            V::Read(V::Field::CoffeePower, &coffee);
        g_coffeeBase = coffee;
        g_haveCoffee = true;
    }
}

// A rest's own tick, a client's or the host's at 1: its owner unchanged, the effects at the factor, the needs settled.
void TickRest() {
    void* const gm = ue_wrap::world_singleton::Gamemode();
    if (!gm || gm != g_gm.Raw() || !g_gm.Alive()) return EndRest("the gamemode changed");
    void* const body = CurrentBody();
    if (!body || body != g_body.Raw() || !g_body.Alive()) return EndRest("the body changed");
    // The wake is announced (OnWakePost); what this tick still ends on is a rest whose owner changed with no wake.
    float factor = kNativeSleepDilation;
    FX::ForEachWorldEffect(&DilateEffect, &factor);
    SettleNeeds();
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::sleep_sync::SubscribePeerRest(&OnPeerRest);
    coop::roster_ledger::SubscribeSlotReplaced(&OnSlotReplaced);
    coop::config::Subscribe(coop::config_registry::rows::world_sleep_speedup, &HostEval);
}

// The watches are asked about until live (a latched read after). The only per-tick work is a rest's own: the effects
// the world spawns while it lasts take the factor, and the food and coffee are settled; nothing announces either.
void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const bool host = s->role() == coop::net::Role::Host;
    const bool wasSettled = g_sleepWatch.settled && g_wakeWatch.settled;
    EnsureWatch();
    // A host asleep before its watches were live: the rest it owes is seen now (every later change is an edge).
    if (host && !wasSettled && g_sleepWatch.settled && g_wakeWatch.settled) HostEval();
    // A host asleep when its session starts running (no edge announces that): declared once per session.
    if (host && !g_hostDeclared) {
        g_hostDeclared = true;
        HostEval();
    }
    if (!g_resting) return;
    if (!host && !ConnectedClient()) return EndRest("the session left");
    TickRest();
    // A rest that ended with the host still asleep (its body or gamemode changed): declared again.
    if (host && !g_resting) HostEval();
}

bool Resting() { return g_resting; }

WatchState Watches() {
    const Watch* ws[] = {&g_sleepWatch, &g_wakeWatch};
    bool pending = false;
    for (const Watch* w : ws) {
        if (w->settled && !w->live) return WatchState::Dead;
        pending = pending || !w->live;
    }
    return pending ? WatchState::Pending : WatchState::Live;
}

// The last client left a host's session, which lives on: the host's sleep is declared again with nobody connected. A
// session that ended: a rest in progress goes back, and a host resting at 1 sleeps on by itself at the game's factor.
void OnDisconnect() {
    if (HostSession()) return HostEval();
    g_hostDeclared = false;  // the next session's start declares the host's sleep again
    auto* s = g_session.load(std::memory_order_acquire);
    const bool host = s && s->role() == coop::net::Role::Host;
    if (g_resting) EndRest("the session ended");
    // A host asleep sleeps on by itself, at the game's factor, whatever the end left the world at.
    if (host && SLP::IsSleeping()) HoldWorldAt(kNativeSleepDilation);
}

}  // namespace coop::sleep_rest
