// coop/world/explosion_parking.cpp -- see coop/world/explosion_parking.h.

#include "coop/world/explosion_parking.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_wire_parity.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/explosion.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace coop::explosion_parking {
namespace {

namespace SG = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

// The four verbs, on any class: melee and guns call these names too, so the callback decides by the caller.
UE_FACT_FN(kRowVerbAddDamage, "explosion", nullptr, L"addDamage", AnyClass);
UE_FACT_FN(kRowVerbExploded, "explosion", nullptr, L"exploded", AnyClass);
UE_FACT_FN(kRowVerbReached, "explosion", nullptr, L"reachedByExplosion", AnyClass);
UE_FACT_FN(kRowVerbIgnite, "explosion", nullptr, L"ignite", AnyClass);
UE_FACT_FN(kRowRuneCheck, "explosion", L"explosion_C", L"runeCheck", Declared);
UE_FACT_FN(kRowNarrowBeginPlay, "explosion", L"explosion_C", L"ReceiveBeginPlay", Declared);

constexpr int kVerbs = 4;
constexpr int kTagVerb0 = 0x45585630;  // 'EXV0'..'EXV3'
constexpr int kTagRune = 0x45585243;   // 'EXRC'
constexpr int kTagNarrow = 0x45584E52; // 'EXNR'

SG::Verdict OnVerbPre(const SG::Call& c);
SG::Verdict OnRunePre(const SG::Call& c);
void        OnNarrowPost(const SG::Call& c);

struct Hook {
    const ue_wrap::facts::Function* row;
    int                             tag;
    SG::PreFn                       pre;
    SG::PostFn                      post;
    const char*                     name;
};
constexpr int kHooks = 6;
const Hook kTable[kHooks] = {
    {&kRowVerbAddDamage, kTagVerb0 + 0, &OnVerbPre, nullptr, "addDamage"},
    {&kRowVerbExploded, kTagVerb0 + 1, &OnVerbPre, nullptr, "exploded"},
    {&kRowVerbReached, kTagVerb0 + 2, &OnVerbPre, nullptr, "reachedByExplosion"},
    {&kRowVerbIgnite, kTagVerb0 + 3, &OnVerbPre, nullptr, "ignite"},
    {&kRowRuneCheck, kTagRune, &OnRunePre, nullptr, "runeCheck"},
    {&kRowNarrowBeginPlay, kTagNarrow, nullptr, &OnNarrowPost, "ReceiveBeginPlay"},
};

constexpr auto kRetry = std::chrono::seconds(1);
constexpr int  kMaxTries = 30;
using Clock = std::chrono::steady_clock;

bool              g_done[kHooks] = {};
bool              g_dead[kHooks] = {};
int               g_tries[kHooks] = {};
Clock::time_point g_nextTry{};
bool              g_liveLogged = false;
bool              g_settledDead = false;  // a watch settled dead in the gate: Live() can never hold

Counters g_counters{};

constexpr const char* kVerbNames[kVerbs] = {"addDamage", "exploded", "reachedByExplosion", "ignite"};

// One explosion instance, named by its address and its slot's serial so a new explosion at an old one's address is
// another instance. Called only for a live object.
struct Instance {
    void*   obj = nullptr;
    int32_t serial = 0;
};
Instance InstanceOf(void* obj) { return {obj, R::AllocateSlotSerial(R::InternalIndexOf(obj))}; }
bool     Same(const Instance& a, const Instance& b) { return a.obj == b.obj && a.serial == b.serial; }

// The refusals already logged for the current explosion: (verb, target class) pairs, 16 deep, forgotten when the
// calling explosion is another instance. True when the pair is new, so the refusal logs once and not per call.
struct Seen {
    uint8_t verb;
    void*   cls;
};
constexpr int kRing = 16;
Seen     g_ring[kRing] = {};
int      g_ringCount = 0;
int      g_ringNext = 0;
Instance g_ringOwner;

bool FirstRefusal(void* explosion, int verb, void* cls) {
    const Instance inst = InstanceOf(explosion);
    if (!Same(inst, g_ringOwner)) {
        g_ringOwner = inst;
        g_ringCount = g_ringNext = 0;
    }
    for (int i = 0; i < g_ringCount; ++i)
        if (g_ring[i].verb == verb && g_ring[i].cls == cls) return false;
    g_ring[g_ringNext] = {static_cast<uint8_t>(verb), cls};
    g_ringNext = (g_ringNext + 1) % kRing;
    if (g_ringCount < kRing) ++g_ringCount;
    return true;
}

Instance g_runeLogged;

// This machine's own player: a mainPlayer_C that is not a peer's puppet. The puppet test is the one the damage
// refusal makes, since a stale local pawn must never make this machine's player immune.
bool IsOwnPlayer(void* obj) {
    return obj && R::NameEquals(R::NameOf(R::ClassOf(obj)), L"mainPlayer_C") &&
           !coop::players::Registry::Get().IsPuppet(obj);
}

// MTA lets the game damage a remote copy and then restores the owner's state (CClientGame.cpp:4509, CNetAPI.cpp:895);
// VOTV has no owner-state restore, so this lane refuses the write at the verb instead.
SG::Verdict OnVerbPre(const SG::Call& c) {
    const int verb = c.tag - kTagVerb0;
    if (verb < 0 || verb >= kVerbs || !coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    ++g_counters.verbCalls[verb];
    // A ProcessEvent-routed call has no caller frame, and melee and guns call these names too: only a verb the
    // explosion's own graph calls is judged.
    if (!c.callerFunction || !R::NameEquals(R::NameOf(c.callerFunction), L"ExecuteUbergraph_explosion"))
        return SG::Verdict::Run;
    if (IsOwnPlayer(c.object)) return SG::Verdict::Run;
    ++g_counters.verbRefused[verb];
    void* cls = c.object ? R::ClassOf(c.object) : nullptr;
    if (c.callerObject && FirstRefusal(c.callerObject, verb, cls))
        UE_LOGI("[EXPLOSION] refused verb=%s target=%ls", kVerbNames[verb], R::ClassNameOf(c.object).c_str());
    return SG::Verdict::Cancel;
}

// The explosion's rune check writes the world; a client's copy does not run it.
SG::Verdict OnRunePre(const SG::Call& c) {
    if (!coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    ++g_counters.runeRefused;
    if (c.object) {
        const Instance inst = InstanceOf(c.object);
        if (!Same(inst, g_runeLogged)) {
            g_runeLogged = inst;
            UE_LOGI("[EXPLOSION] refused runeCheck");
        }
    }
    return SG::Verdict::Cancel;
}

// After the Blueprint's own begin-play writes, before the force fires: the force reaches pawns only.
void OnNarrowPost(const SG::Call& c) {
    if (!c.object || !coop::spawn_authority::IsActiveClientSession()) return;
    if (ue_wrap::explosion::NarrowForceToPawn(c.object)) {
        ++g_counters.narrowed;
        UE_LOGI("[EXPLOSION] narrowed");
    } else {
        // Fail closed: an un-narrowed force would push this client's props. The copy is a cue, so it is destroyed
        // by a posted task (the begin-play is still on the stack), before its force fires.
        ++g_counters.narrowFailed;
        UE_LOGE("[EXPLOSION] the force of an explosion copy could not be narrowed to pawns; the copy is destroyed");
        void* copy = c.object;
        const int32_t index = R::InternalIndexOf(copy);
        GT::Post([copy, index] {
            if (R::IsLiveByIndex(copy, index)) E::DestroyActor(copy);
        });
    }
}

// ---- the coast of flung props ---------------------------------------------------------------------------------------

UE_FACT_FN(kRowFireImpulse, "explosion", L"RadialForceComponent", L"FireImpulse", Declared);

// An explosion whose radial force fired, by pointer and the object index it had. The hook fires inside the
// explosion's own body; the props are read and handed on in Tick, outside it. No cap: every flung prop coasts, and a
// chain blast queues each of its explosions.
struct Fired {
    void*   explosion;
    int32_t index;
};
std::vector<Fired> g_fired;
std::vector<coop::element::Registry::ActorIdPair> g_props;  // reused by every Tick's walk

// One queued explosion read for a Tick: where it went off, its squared reach, and the props it flung.
struct Blast {
    ue_wrap::FVector at;
    float            reach2;
    uint32_t         flung;
    char             eids[96];  // the first eight ids, for the log
    size_t           used;
};
std::vector<Blast> g_blasts;  // reused by every Tick

bool     g_fireInstalled = false;
bool     g_fireGaveUp = false;
int      g_fireTries = 0;
Clock::time_point g_fireNextTry{};

// Every Blueprint call of FireImpulse arrives here; an explosion's is the one whose component's owner is one of
// ours. A component's outer is the actor that owns it.
void OnFireImpulsePost(void* component, void* /*sourceObject*/, void* /*result*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!component || !s || !s->connected() || s->role() != coop::net::Role::Host || s->connectedPeerCount() <= 0)
        return;
    void* owner = R::OuterOf(component);
    bool byName = false;
    if (!owner || ue_wrap::explosion::ClassIdOf(R::ClassOf(owner), byName) < 0) return;
    g_fired.push_back(Fired{owner, R::InternalIndexOf(owner)});
}

// What a dead parking watch costs, said only where it applies: a client in a session drops the host's copies.
const char* kNotApplied() {
    return coop::spawn_authority::IsActiveClientSession() ? "; the host's explosions are not applied here" : "";
}

// Install under the same policy as the watches: a refused install is retried each second, 30 times, then said once.
void InstallFireImpulse() {
    if (g_fireInstalled || g_fireGaveUp || Clock::now() < g_fireNextTry) return;
    g_fireNextTry = Clock::now() + kRetry;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowFireImpulse);
    if (fn && ue_wrap::ufunction_hook::InstallPostHook(fn, &OnFireImpulsePost)) {
        g_fireInstalled = true;
        g_fired.reserve(16);
        g_blasts.reserve(16);
        UE_LOGI("[EXPLOSION] coast seam installed on RadialForceComponent::FireImpulse");
    } else if (++g_fireTries >= kMaxTries) {
        g_fireGaveUp = true;
        UE_LOGE("[EXPLOSION] the FireImpulse hook did not install in %d tries; the props a host explosion flings are "
                "not driven to clients", kMaxTries);
    }
}

}  // namespace

void Install(coop::net::Session* s) {
    g_session.store(s, std::memory_order_release);
    InstallFireImpulse();
    if (g_liveLogged || g_settledDead) return;
    bool pending = false, dead = false;
    if (Clock::now() >= g_nextTry) {
        g_nextTry = Clock::now() + kRetry;
        for (int i = 0; i < kHooks; ++i) {
            if (g_done[i] || g_dead[i]) continue;
            if (SG::WatchName(*kTable[i].row, kTable[i].tag, kTable[i].pre, kTable[i].post)) {
                g_done[i] = true;
            } else if (++g_tries[i] >= kMaxTries) {
                g_dead[i] = true;
                UE_LOGE("[EXPLOSION] the %s watch was refused %d times%s", kTable[i].name, kMaxTries, kNotApplied());
            }
        }
    }
    for (int i = 0; i < kHooks; ++i)
        if (g_dead[i]) dead = true;
        else if (!g_done[i]) pending = true;
    if (pending || dead) return;
    SG::ResolvePendingNames();
    if (Live()) {
        g_liveLogged = true;
        UE_LOGI("[EXPLOSION] the explosion copy's parking watches are live");
        return;
    }
    // A watch the gate settled dead (its class declares no such body, or declares it native) never goes live:
    // said once per row, and the per-tick scans stop.
    for (int i = 0; i < kHooks; ++i) {
        if (!SG::NameWatchSettled(*kTable[i].row, kTable[i].tag) || SG::NameWatchLive(*kTable[i].row, kTable[i].tag))
            continue;
        g_settledDead = true;
        UE_LOGE("[EXPLOSION] the %s watch settled dead in the script gate%s", kTable[i].name, kNotApplied());
    }
}

bool Live() {
    for (int i = 0; i < kHooks; ++i)
        if (!g_done[i] || !SG::NameWatchLive(*kTable[i].row, kTable[i].tag)) return false;
    return true;
}

void Tick(coop::net::Session* s) {
    if (g_fired.empty()) return;
    const bool hosting = s && s->connected() && s->role() == coop::net::Role::Host;
    g_blasts.clear();
    for (const Fired& f : g_fired) {
        if (!hosting || !R::IsLiveByIndex(f.explosion, f.index)) continue;
        Blast b{};
        float radius = 0.f;
        if (!E::TryGetActorLocation(f.explosion, b.at) || !ue_wrap::explosion::ReadForceRadius(f.explosion, radius))
            continue;
        b.reach2 = radius * radius;
        g_blasts.push_back(b);
    }
    g_fired.clear();
    if (g_blasts.empty()) return;
    // One registry snapshot and one position read per prop for every explosion of this tick; a prop's physics flags
    // are read only when it lies inside some blast's radius.
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, g_props);
    for (const auto& prop : g_props) {
        if (!prop.actor || !R::IsLiveByIndex(prop.actor, prop.internalIdx)) continue;
        ue_wrap::FVector pos{};
        // A simulating prop is unattached, so its root's location is read from memory; an attached root (rare)
        // is read through the engine.
        if (!E::TryGetRootLocationRaw(prop.actor, pos) && !E::TryGetActorLocation(prop.actor, pos)) continue;
        int simulating = -1;  // unknown until a blast reaches the prop
        for (Blast& b : g_blasts) {
            const float dx = pos.X - b.at.X, dy = pos.Y - b.at.Y, dz = pos.Z - b.at.Z;
            if (dx * dx + dy * dy + dz * dz > b.reach2) continue;
            if (simulating < 0) {
                simulating = (coop::prop_wire_parity::PhysFlagsOf(prop.actor) &
                              coop::net::propspawn_flags::kSimulatePhysics) ? 1 : 0;
                // One tick after the impulse the prop already carries the blast's velocity, which the channel
                // streams.
                if (simulating) coop::prop_drive_host::Coast(prop.actor, "explosion");
            }
            if (!simulating) break;
            if (b.flung < 8 && b.used < sizeof(b.eids))
                b.used += static_cast<size_t>(std::snprintf(b.eids + b.used, sizeof(b.eids) - b.used, "%s%u",
                                                            b.flung ? "," : "", static_cast<unsigned>(prop.id)));
            ++b.flung;
        }
    }
    for (const Blast& b : g_blasts) UE_LOGI("[EXPLOSION] coast n=%u eids=%s", b.flung, b.eids);
}

void OnDisconnect() {
    g_fired.clear();
    const Counters c = GetCounters();
    if (c.verbCalls[0] || c.verbCalls[1] || c.verbCalls[2] || c.verbCalls[3])
        UE_LOGI("[EXPLOSION] parking end: verbCalls=%u/%u/%u/%u refused=%u/%u/%u/%u runeRefused=%u narrowed=%u "
                "narrowFailed=%u",
                c.verbCalls[0], c.verbCalls[1], c.verbCalls[2], c.verbCalls[3], c.verbRefused[0], c.verbRefused[1],
                c.verbRefused[2], c.verbRefused[3], c.runeRefused, c.narrowed, c.narrowFailed);
    g_counters = Counters{};
}

Counters GetCounters() { return g_counters; }

}  // namespace coop::explosion_parking
