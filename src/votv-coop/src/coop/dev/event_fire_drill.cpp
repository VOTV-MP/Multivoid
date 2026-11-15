// coop/dev/event_fire_drill.cpp -- see coop/dev/event_fire_drill.h.

#include "coop/dev/event_fire_drill.h"

#include "coop/cheats/item_spawn.h"
#include "coop/config/config.h"
#include "coop/dev/cave_drill.h"
#include "coop/dev/event_force.h"
#include "coop/dev/event_trigger.h"
#include "coop/dev/signal_drill.h"
#include "coop/dev/gamemode_drill.h"
#include "coop/dev/join_leftovers_drill.h"
#include "coop/dev/world_event_drill.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace coop::dev::event_fire_drill {
namespace {

namespace ET = coop::dev::event_trigger;

struct Plan {
    std::string name;
    uint64_t    watchMs = 60000;
    std::wstring standAt;  // a class whose first actor the host player is moved to before the fire
};

const Plan& ThePlan() {
    static const Plan p = [] {
        Plan out;
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::event_fire_drill);
        const size_t colon = v.find(':');
        out.name = v.substr(0, colon);
        if (colon != std::string::npos) {
            const long s = std::strtol(v.c_str() + colon + 1, nullptr, 10);
            if (s > 0 && s <= 3600) out.watchMs = static_cast<uint64_t>(s) * 1000;
            const size_t second = v.find(':', colon + 1);
            if (second != std::string::npos) out.standAt.assign(v.begin() + second + 1, v.end());
        }
        return out;
    }();
    return p;
}

constexpr uint64_t kSettleMs = 6000;

uint64_t g_readyMs = 0;
uint64_t g_firedMs = 0;
bool     g_done = false;
bool     g_walked = false;  // the host's player was put at event_fire_walk after the fire
int      g_craftStep = 0;   // soltoClean: 0 before the craft's box overlap is called, 1 before its `opened`, 2 after
bool     g_ghosts = false;  // spawnBlackFog: the fog's spawnGhost was called
bool     g_forced = false;  // the event's level box, when it has one, was forced after the arm

// A random ticker no event name can fire: the drill runs the ticker's own tick on the host, as the game does each
// interval, with the clock set where the roll wants it for the length of the call, until the creature it mints
// stands. `yellowWispTick` is ticker_yellowWispSpawner_C (the ambient Killer Wisp: night, days 25 to 29) and
// `fossilhoundTick` ticker_fossilhoundSpawner_C (the fossilhound: night, day 7 on, a moon that is not new), and
// `stealsGunTick` event_arirStealsGun_C (the dropped gun's Ariral: dusk to dawn, day 24 on, with a prop_arirGun_C about:
// the drill stands both the gun and the event actor beside the host's player first, 6 s on, when the save has none).
struct TickerDrill {
    const char*    name;
    const wchar_t* ticker;
    const wchar_t* product;
    int32_t        firstDay, days;  // the day numbers tried, one per attempt
};
constexpr TickerDrill kTickerDrills[] = {
    {"yellowWispTick", L"ticker_yellowWispSpawner_C", L"killerwisp_C", 25, 5},
    {"fossilhoundTick", L"ticker_fossilhoundSpawner_C", L"fossilhound_C", 7, 28},
    {"stealsGunTick", L"event_arirStealsGun_C", L"npc_arirGunStealer_C", 24, 5},
};
constexpr int kTickerAttempts = 600;

const TickerDrill* TickerDrillOf(const std::string& name) {
    for (const TickerDrill& t : kTickerDrills)
        if (name == t.name) return &t;
    return nullptr;
}

bool     g_deployed = false;  // graystankUnfold: the saucer's forceDeploy was called
bool     g_unfolded = false;  // graystankUnfold: the box's spawnParts was called
uint64_t g_unfoldNextMs = 0;
int      g_unfoldLooks = 0;
int      g_tickerTries = 0;
bool     g_tickerMade = false;
uint64_t g_tickerNextMs = 0;
bool     g_gunStood = false;        // stealsGunTick: the gun and the event actor were stood beside the host's player
int      g_treehouseCalls = 0;      // treehouseShooter, treehousePig: calls of the treehouse's trySpawnPigShooter so far
uint64_t g_clientSeenMs = 0;        // the client's pass: when it first saw its session connected
bool     g_clientTreehouse = false; // the client stood its own treehouse (see StandTreehouse)

bool IsTreehouseDrill(const std::string& n) { return n == "treehouseShooter" || n == "treehousePig"; }

void* LiveInstanceOf(const wchar_t* cls) {
    namespace R = ue_wrap::reflection;
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) return obj;
    return nullptr;
}

size_t CountLive(const wchar_t* cls) {
    namespace R = ue_wrap::reflection;
    size_t n = 0;
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++n;
    return n;
}

// The treehouse drills' set-up, on each peer: the test save holds no treehouse_C (a level actor of a built camp), so each
// peer stands one of its own at the same fixed point, as a level that held one would have it on both sides. The point is
// the world's origin, 6 km from the host's fixed place, so no body is near it.
void StandTreehouse(const char* who) {
    if (LiveInstanceOf(L"treehouse_C")) return;
    void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/treehouse.treehouse_C");
    void* actor = cls ? ue_wrap::engine::SpawnActor(cls, {0.f, 0.f, 0.f}) : nullptr;
    UE_LOGI("[EVENT-FIRE-DRILL] [%s] treehouse: class %p, spawned %p at the origin", who, cls, actor);
}

// `count` gray boars at the treehouse, for the roll that follows at once (the origin has no ground, so a boar falls out of
// the treehouse's 100 m sensor within a few seconds): with one boar about, the roll is the pig beater, with two or more the
// shooter.
void StandBoars(int count) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/misc/grayboar.grayboar_C");
    int made = 0;
    for (int i = 0; cls && i < count; ++i)
        if (ue_wrap::engine::SpawnActor(cls, {100.f * static_cast<float>(i), 300.f, 300.f})) ++made;
    UE_LOGI("[EVENT-FIRE-DRILL] treehouse drill: boar class %p, %d of %d spawned", cls, made, count);
}

// stealsGunTick's set-up: a prop_arirGun_C beside the host's player, and an event actor of its own when the save holds none.
void StandGunAndEventActor() {
    namespace R = ue_wrap::reflection;
    if (!LiveInstanceOf(L"prop_arirGun_C")) {
        int made = 0;
        const auto r = coop::cheats::item_spawn::Spawn("arirGun", 1, 0, true, &made);
        UE_LOGI("[EVENT-FIRE-DRILL] [H] stealsGunTick: the gun spawned, result %d, %d made", static_cast<int>(r), made);
    }
    if (!LiveInstanceOf(L"event_arirStealsGun_C")) {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/event_arirStealsGun.event_arirStealsGun_C");
        void* player = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        void* actor = cls && player && ue_wrap::engine::TryGetActorLocation(player, at) ? ue_wrap::engine::SpawnActor(cls, at) : nullptr;
        UE_LOGI("[EVENT-FIRE-DRILL] [H] stealsGunTick: the event actor class %p, spawned %p", cls, actor);
    }
    UE_LOGI("[EVENT-FIRE-DRILL] [H] stealsGunTick: %zu prop_arirGun_C and %zu event_arirStealsGun_C live", CountLive(L"prop_arirGun_C"),
            CountLive(L"event_arirStealsGun_C"));
}

// One attempt: the clock's hour is put at 22 and its day at `day` (the cycle's time vector holds hour, minute and day as
// three ints), the ticker's tick runs, and the clock is put back before the call returns, so no sample of it is sent.
void RunTickerOnce(const TickerDrill& t, int32_t day) {
    namespace R = ue_wrap::reflection;
    void* ticker = LiveInstanceOf(t.ticker);
    void* cycle = LiveInstanceOf(L"daynightCycle_C");
    void* fn = ticker ? R::FindDispatchFunctionCached(R::ClassOf(ticker), L"ReceiveTick") : nullptr;
    const int32_t off = cycle ? R::FindPropertyOffset(R::ClassOf(cycle), L"timeZ") : -1;
    if (!fn || off < 0) {
        if (g_tickerTries == 1)
            UE_LOGW("[EVENT-FIRE-DRILL] [H] %s: no live %ls or no clock to set (ticker %p, clock %p)", t.name, t.ticker, ticker, cycle);
        return;
    }
    int32_t* clock = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(cycle) + off);
    const int32_t saved[3] = {clock[0], clock[1], clock[2]};
    clock[0] = 22;
    clock[2] = day;
    ue_wrap::ParamFrame f(fn);
    if (f.valid()) {
        f.Set<float>(L"DeltaSeconds", 0.016f);
        ue_wrap::Call(ticker, f);
    }
    clock[0] = saved[0];
    clock[1] = saved[1];
    clock[2] = saved[2];
}

void TickTickerDrill(const TickerDrill& t, uint64_t now) {
    if (g_tickerMade || g_tickerTries >= kTickerAttempts || now < g_tickerNextMs) return;
    g_tickerNextMs = now + 150;
    const size_t before = CountLive(t.product);
    RunTickerOnce(t, t.firstDay + g_tickerTries % t.days);
    ++g_tickerTries;
    if (CountLive(t.product) > before) {
        g_tickerMade = true;
        UE_LOGI("[EVENT-FIRE-DRILL] [H] %s: the ticker's own tick made a %ls after %d attempt(s)", t.name, t.product,
                g_tickerTries);
    } else if (g_tickerTries == kTickerAttempts) {
        UE_LOGW("[EVENT-FIRE-DRILL] [H] FAIL %s: %d ticks of the ticker made no %ls", t.name, g_tickerTries, t.product);
    }
}

bool ClientInWorld(coop::net::Session& s) {
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
        if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
            !coop::join_window_baseline::IsLateWindowOpen(slot))
            return true;
    return false;
}

// `host_place` is "x,y,z": where the host's own player is put once its world is up, before anyone joins. A drill
// whose client joins at the host and then walks starts from there, whatever spot the save left the player on.
struct Place {
    bool             set = false;
    ue_wrap::FVector at{};
};
const Place& ThePlace() {
    static const Place p = [] {
        Place out;
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::host_place);
        float x = 0, y = 0, z = 0;
        if (std::sscanf(v.c_str(), "%f,%f,%f", &x, &y, &z) == 3) {
            out.set = true;
            out.at = {x, y, z};
        }
        return out;
    }();
    return p;
}
uint64_t g_placeSeenMs = 0;
bool     g_placed = false;

void TickPlace() {
    if (g_placed || !ThePlace().set || !coop::roster::LocalIsHost()) return;
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::FVector here{};
    if (!player || !ue_wrap::engine::TryGetActorLocation(player, here)) return;
    const uint64_t now = coop::net::NowMs();
    if (!g_placeSeenMs) g_placeSeenMs = now;
    if (now - g_placeSeenMs < 3000) return;
    g_placed = true;
    const bool ok = ue_wrap::engine::TeleportTo(player, ThePlace().at, {});
    UE_LOGI("[HOST-PLACE] the host's player moved from (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f): %d", here.X, here.Y, here.Z,
            ThePlace().at.X, ThePlace().at.Y, ThePlace().at.Z, ok ? 1 : 0);
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (session) TickPlace();
    // A treehouse drill's client stands its own treehouse once its session is up, so that the refusal of the treehouse's
    // timer body is shown on a peer that has one.
    if (!g_clientTreehouse && session && session->connected() && !coop::roster::LocalIsHost() && IsTreehouseDrill(ThePlan().name)) {
        const uint64_t t = coop::net::NowMs();
        if (!g_clientSeenMs) g_clientSeenMs = t;
        if (t - g_clientSeenMs > 12000) {
            g_clientTreehouse = true;
            StandTreehouse("C");
            // and rolls both shooter bodies by hand, with two boars about: a client that ran them would mint its own shooter
            // (their spawns are bytecode-internal, so no interceptor would stop it); the refusals are the lines to read.
            StandBoars(2);
            for (const wchar_t* fnName : {L"trySpawnPigShooter", L"trySpawnTankShooter"}) {
                void* tree = LiveInstanceOf(L"treehouse_C");
                void* fn = tree ? ue_wrap::reflection::FindDispatchFunctionCached(ue_wrap::reflection::ClassOf(tree), fnName) : nullptr;
                ue_wrap::ParamFrame f(fn);
                const bool ran = fn && f.valid() && ue_wrap::Call(tree, f);
                UE_LOGI("[EVENT-FIRE-DRILL] [C] the client's treehouse %ls called: %d", fnName, ran ? 1 : 0);
            }
        }
    }
    // The cave drills' client half: its own spawner, refused (coop/dev/cave_drill).
    if (session && session->connected() && !coop::roster::LocalIsHost() && coop::dev::cave_drill::IsCaveDrill(ThePlan().name))
        coop::dev::cave_drill::ClientStep(ThePlan().name);
    // The world-and-sky events' client half: what this peer shows, and its own roll, refused (coop/dev/world_event_drill).
    if (session && session->connected() && !coop::roster::LocalIsHost() && coop::dev::world_event_drill::IsWorldEventDrill(ThePlan().name))
        coop::dev::world_event_drill::ClientStep(ThePlan().name);
    // The game mode's timer events' client half: what this peer shows, and its own roll (coop/dev/gamemode_drill).
    if (session && session->connected() && !coop::roster::LocalIsHost() && coop::dev::gamemode_drill::IsGamemodeDrill(ThePlan().name))
        coop::dev::gamemode_drill::ClientStep(ThePlan().name);
    // The base computer's signal spawns and the prank spawners, the client half (coop/dev/signal_drill).
    if (session && session->connected() && !coop::roster::LocalIsHost()) coop::dev::signal_drill::ClientStep(ThePlan().name);
    // The actors a joiner is owed after the last client left: what this peer holds of the class (coop/dev/join_leftovers_drill).
    if (session && session->connected() && !coop::roster::LocalIsHost() && coop::dev::join_leftovers_drill::IsLeftoversDrill(ThePlan().name))
        coop::dev::join_leftovers_drill::ClientStep(ThePlan().name);
    if (ThePlan().name.empty() || !session || g_done || !coop::roster::LocalIsHost() || !session->connected()) return;
    const uint64_t now = coop::net::NowMs();
    if (!g_firedMs) {
        if (!ClientInWorld(*session)) return;
        if (!g_readyMs) g_readyMs = now;
        if (now - g_readyMs < kSettleMs) return;
        const ET::EventInfo* found = nullptr;
        // `graystankUnfold` is `graystank` plus the box's unfolding called by the drill (below). The treehouse drills fire
        // no event: each stands the treehouse and calls its own trySpawnPigShooter.
        std::string eventName = ThePlan().name == "graystankUnfold" ? "graystank" : ThePlan().name;
        // A join-leftovers drill fires its event once: after a client's rejoin the host still holds the actor, and a second
        // fire would mint another.
        if (const char* ev = coop::dev::join_leftovers_drill::EventOf(ThePlan().name)) {
            eventName = ev;
            if (coop::dev::join_leftovers_drill::AlreadyHeld(ThePlan().name)) {
                g_firedMs = now;
                UE_LOGI("[EVENT-FIRE-DRILL] [H] '%s': the host already holds its actor from before the rejoin, so '%s' is not fired again",
                        ThePlan().name.c_str(), ev);
                return;
            }
        }
        if (IsTreehouseDrill(ThePlan().name)) StandTreehouse("H");
        for (const ET::EventInfo& ev : ET::Events())
            if (eventName == ev.name) { found = &ev; break; }
        if (TickerDrillOf(ThePlan().name) || IsTreehouseDrill(ThePlan().name) || coop::dev::cave_drill::IsCaveDrill(ThePlan().name) ||
            coop::dev::world_event_drill::IsStandalone(ThePlan().name) || coop::dev::gamemode_drill::IsGamemodeDrill(ThePlan().name) ||
            coop::dev::signal_drill::IsSignalDrill(ThePlan().name)) {
            g_firedMs = now;
            UE_LOGI("[EVENT-FIRE-DRILL] [H] fired '%s' (1); watching %llu s", ThePlan().name.c_str(),
                    static_cast<unsigned long long>(ThePlan().watchMs / 1000));
            return;
        }
        if (!found) {
            UE_LOGW("[EVENT-FIRE-DRILL] [H] FAIL no event named '%s' in the dev event table", ThePlan().name.c_str());
            g_done = true;
            return;
        }
        if (!ThePlan().standAt.empty()) {
            void* at = ue_wrap::reflection::FindObjectByClass(ThePlan().standAt.c_str());
            void* player = coop::players::Registry::Get().Local();
            ue_wrap::FVector loc{};
            const bool moved = at && player && ue_wrap::engine::TryGetActorLocation(at, loc) &&
                               ue_wrap::engine::TeleportTo(player, {loc.X, loc.Y, loc.Z + 60.f}, {});
            UE_LOGI("[EVENT-FIRE-DRILL] [H] host player moved to the first '%ls' at (%.0f,%.0f,%.0f): %d",
                    ThePlan().standAt.c_str(), loc.X, loc.Y, loc.Z, moved ? 1 : 0);
        }
        const bool ok = ET::Trigger(*found);
        g_firedMs = now;
        UE_LOGI("[EVENT-FIRE-DRILL] [H] fired '%s' (%d); watching %llu s", found->name, ok ? 1 : 0,
                static_cast<unsigned long long>(ThePlan().watchMs / 1000));
        return;
    }
    // The Day-44 tank's saucer waits hidden for a body in its box, and its box falls from it and unfolds into the fire tank
    // some minutes after it lands. The drill calls the saucer's own `forceDeploy` 8 s on, as the eventer's deploy does, and
    // from 40 s on looks for the box and, once, calls the box's own `spawnParts`, the body the game calls when the unfolding ends.
    if (ThePlan().name == "graystankUnfold" && !g_deployed && now - g_firedMs >= 8000) {
        namespace R = ue_wrap::reflection;
        void* saucer = LiveInstanceOf(L"ufoDropper_tank_C");
        void* fn = saucer ? R::FindDispatchFunctionCached(R::ClassOf(saucer), L"forceDeploy") : nullptr;
        if (saucer) {
            g_deployed = true;
            ue_wrap::ParamFrame f(fn);
            const bool ran = fn && f.valid() && ue_wrap::Call(saucer, f);
            UE_LOGI("[EVENT-FIRE-DRILL] [H] graystankUnfold: the saucer's forceDeploy called: %d", ran ? 1 : 0);
        }
    }
    if (ThePlan().name == "graystankUnfold" && !g_unfolded && now - g_firedMs >= 40000 && now >= g_unfoldNextMs) {
        namespace R = ue_wrap::reflection;
        g_unfoldNextMs = now + 5000;
        void* box = LiveInstanceOf(L"firetankDeploy_C");
        void* fn = box ? R::FindDispatchFunctionCached(R::ClassOf(box), L"spawnParts") : nullptr;
        if (++g_unfoldLooks % 6 == 1)
            UE_LOGI("[EVENT-FIRE-DRILL] [H] graystankUnfold: %zu firetankDeploy_C box(es) live", CountLive(L"firetankDeploy_C"));
        if (box && fn) {
            ue_wrap::ParamFrame f(fn);
            const bool ran = f.valid() && ue_wrap::Call(box, f);
            g_unfolded = true;
            UE_LOGI("[EVENT-FIRE-DRILL] [H] graystankUnfold: the box's spawnParts called: %d (%zu firetank_C live)", ran ? 1 : 0,
                    CountLive(L"firetank_C"));
        }
    }
    // The treehouse rolls its pig beater, or its shooter when two boars are about, in trySpawnPigShooter when its sensor sees
    // them. The drill stands the boars (one for `treehousePig`, two for `treehouseShooter`) and calls that body itself in the
    // same pass, at 4 s and at 12 s, and says what stands before a call; the next line says what it made.
    if (IsTreehouseDrill(ThePlan().name) && g_treehouseCalls < 2 && now - g_firedMs >= (g_treehouseCalls == 0 ? 4000u : 12000u)) {
        namespace R = ue_wrap::reflection;
        StandBoars(ThePlan().name == "treehousePig" ? 1 : 2);
        void* tree = LiveInstanceOf(L"treehouse_C");
        void* fn = tree ? R::FindDispatchFunctionCached(R::ClassOf(tree), L"trySpawnPigShooter") : nullptr;
        UE_LOGI("[EVENT-FIRE-DRILL] [H] %s: before call %d: %zu grayboar_C, %zu npc_ariral_pigBeater_C, %zu npc_ariral_shooter_C live",
                ThePlan().name.c_str(), g_treehouseCalls + 1, CountLive(L"grayboar_C"), CountLive(L"npc_ariral_pigBeater_C"),
                CountLive(L"npc_ariral_shooter_C"));
        bool ran = false;
        if (fn) {
            ue_wrap::ParamFrame f(fn);
            ran = f.valid() && ue_wrap::Call(tree, f);
        }
        ++g_treehouseCalls;
        UE_LOGI("[EVENT-FIRE-DRILL] [H] %s: the treehouse's trySpawnPigShooter called: %d (treehouse %p)", ThePlan().name.c_str(),
                ran ? 1 : 0, tree);
    }
    // The dropped gun's Ariral: the gun and the event actor stand 6 s on, the tick runs from 8 s (below).
    if (ThePlan().name == "stealsGunTick" && !g_gunStood && now - g_firedMs >= 6000) {
        g_gunStood = true;
        StandGunAndEventActor();
    }
    // The cave drills' host half: the spawner or the egg, called by the drill (coop/dev/cave_drill).
    if (coop::dev::cave_drill::IsCaveDrill(ThePlan().name)) coop::dev::cave_drill::HostStep(ThePlan().name, now - g_firedMs);
    // The world-and-sky events' host half: the game's own roll or edge, called by the drill (coop/dev/world_event_drill).
    if (coop::dev::world_event_drill::IsWorldEventDrill(ThePlan().name))
        coop::dev::world_event_drill::HostStep(ThePlan().name, now - g_firedMs);
    // The game mode's timer events' host half: the game's own handler, called by the drill (coop/dev/gamemode_drill).
    if (coop::dev::gamemode_drill::IsGamemodeDrill(ThePlan().name))
        coop::dev::gamemode_drill::HostStep(ThePlan().name, now - g_firedMs);
    // The base computer's signal spawns and the prank spawners, the host half (coop/dev/signal_drill).
    coop::dev::signal_drill::HostStep(ThePlan().name, now - g_firedMs);
    // The actors a joiner is owed: the host says what it holds and stops the flyer's tick (coop/dev/join_leftovers_drill).
    if (coop::dev::join_leftovers_drill::IsLeftoversDrill(ThePlan().name))
        coop::dev::join_leftovers_drill::HostStep(ThePlan().name, now - g_firedMs);
    // A ticker drill rolls from 8 s on, once the client has settled in the world.
    if (const TickerDrill* ticker = TickerDrillOf(ThePlan().name))
        if (now - g_firedMs >= 8000) TickTickerDrill(*ticker, now);
    // An event that only arms a level box (the obelisk) waits for a player to walk in: the box is forced 3 s on.
    if (!g_forced && now - g_firedMs >= 3000) {
        g_forced = true;
        if (coop::dev::event_force::ForceNow(ThePlan().name.c_str()))
            UE_LOGI("[EVENT-FIRE-DRILL] [H] forced the level box of '%s'", ThePlan().name.c_str());
    }
    // An event whose actor waits for a player to walk into it: the host's player is put at event_fire_walk 5 s on.
    if (!g_walked && now - g_firedMs >= 5000) {
        g_walked = true;
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::event_fire_walk);
        float x = 0, y = 0, z = 0;
        void* player = coop::players::Registry::Get().Local();
        bool have = std::sscanf(v.c_str(), "%f,%f,%f", &x, &y, &z) == 3;
        // `<class>.<member>`: the place is that component of the class's first live actor (the hole's box).
        const size_t dot = v.find('.');
        if (!have && dot != std::string::npos) {
            const std::wstring cls(v.begin(), v.begin() + dot), member(v.begin() + dot + 1, v.end());
            for (void* obj : ue_wrap::reflection::FindObjectsByClass(cls.c_str())) {
                if (!obj || !ue_wrap::reflection::IsLive(obj)) continue;
                const int32_t off = ue_wrap::reflection::FindPropertyOffset(ue_wrap::reflection::ClassOf(obj), member.c_str());
                void* comp = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(obj) + off) : nullptr;
                ue_wrap::FVector at{};
                if (!comp || !ue_wrap::engine::TryGetComponentLocation(comp, at)) continue;
                x = at.X; y = at.Y; z = at.Z;
                have = true;
                break;
            }
        }
        if (player && have) {
            const bool ok = ue_wrap::engine::TeleportTo(player, {x, y, z}, {});
            UE_LOGI("[EVENT-FIRE-DRILL] [H] host player put at (%.0f,%.0f,%.0f): %d", x, y, z, ok ? 1 : 0);
        }
    }
    // The Soltomia craft waits for a body in its box and then for its door: the drill calls the craft's own two
    // events, as the box and the door do, 10 s and 40 s on, and moves no player.
    if (ThePlan().name == "soltoClean" && g_craftStep < 2 && now - g_firedMs >= (g_craftStep == 0 ? 10000u : 40000u)) {
        namespace R = ue_wrap::reflection;
        const wchar_t* const fnName = g_craftStep == 0
            ? L"BndEvt__soltomiaCleaning_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature"
            : L"opened";
        void* craft = R::FindObjectByClass(L"soltomiaCleaning_C");
        void* fn = craft && R::IsLive(craft) ? R::FindDispatchFunctionCached(R::ClassOf(craft), fnName) : nullptr;
        bool ran = false;
        if (fn) {
            ue_wrap::ParamFrame f(fn);
            ran = f.valid() && ue_wrap::Call(craft, f);
        }
        UE_LOGI("[EVENT-FIRE-DRILL] [H] the craft's %s called: %d", g_craftStep == 0 ? "box overlap" : "opened", ran ? 1 : 0);
        ++g_craftStep;
    }
    // The black fog mints its eyes once it is full, five minutes in: the drill makes the host's fog full 12 s on and
    // calls its own spawnGhost, three times (a roll whose point is off the navmesh mints nothing).
    if (ThePlan().name == "spawnBlackFog" && !g_ghosts && now - g_firedMs >= 12000) {
        namespace R = ue_wrap::reflection;
        g_ghosts = true;
        void* fog = R::FindObjectByClass(L"blackFog_C");
        void* fn = fog && R::IsLive(fog) ? R::FindDispatchFunctionCached(R::ClassOf(fog), L"spawnGhost") : nullptr;
        const int32_t aOff = fog ? R::FindPropertyOffset(R::ClassOf(fog), L"a") : -1;
        int ran = 0;
        if (fn && aOff >= 0) {
            *reinterpret_cast<float*>(static_cast<uint8_t*>(fog) + aOff) = 1.f;
            for (int i = 0; i < 3; ++i) {
                ue_wrap::ParamFrame f(fn);
                if (f.valid() && ue_wrap::Call(fog, f)) ++ran;
            }
        }
        UE_LOGI("[EVENT-FIRE-DRILL] [H] the fog made full and its spawnGhost called %d time(s)", ran);
    }
    if (now - g_firedMs < ThePlan().watchMs) return;
    UE_LOGI("[EVENT-FIRE-DRILL] [H] DONE '%s' after %llu s", ThePlan().name.c_str(),
            static_cast<unsigned long long>((now - g_firedMs) / 1000));
    g_done = true;
}

void OnDisconnect() {
    g_readyMs = 0;
    g_firedMs = 0;
    g_done = false;
    g_forced = false;
    g_walked = false;
    g_craftStep = 0;
    g_ghosts = false;
    g_tickerTries = 0;
    g_tickerMade = false;
    g_tickerNextMs = 0;
    g_gunStood = false;
    g_treehouseCalls = 0;
    g_clientSeenMs = 0;
    g_clientTreehouse = false;
    g_deployed = false;
    g_unfolded = false;
    g_unfoldNextMs = 0;
    g_unfoldLooks = 0;
    coop::dev::cave_drill::OnDisconnect();
    coop::dev::world_event_drill::OnDisconnect();
    coop::dev::gamemode_drill::OnDisconnect();
    coop::dev::signal_drill::OnDisconnect();
    coop::dev::join_leftovers_drill::OnDisconnect();
}

}  // namespace coop::dev::event_fire_drill
