// coop/dev/gamemode_drill.cpp -- see coop/dev/gamemode_drill.h.

#include "coop/dev/gamemode_drill.h"

#include "coop/dev/easter_drill.h"
#include "coop/dev/level_fixture_drill.h"
#include "coop/dev/level_prop_drill.h"
#include "coop/dev/vending_drill.h"
#include "coop/dev/erie_drill.h"

#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstring>

namespace coop::dev::gamemode_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

struct Spec {
    const char*    name;
    const wchar_t* fn;         // the gamemode's timer handler or custom event
    const wchar_t* product;    // the class it mints (or, for Midas, the level actor it switches on)
    const wchar_t* probe;      // a scene component of the product that moves by itself, or null
    int32_t        hour, day;  // the clock for the length of one call
    bool           hold;       // the host stops the product's tick, so it outlasts its flight
    bool           standIn;    // the roll cannot come up in a drill: the host stands the class and enrols it
    bool           bothRoll;   // both peers roll their own (a per-player creature)
    bool           isFlag;     // the product is a level actor, "rolled" when its `active` flag is up
};

constexpr Spec kSpecs[] = {
    {"gmUfoBall",     L"ufo_ball",      L"ufo_ballfo_C",       L"balls",    22, 40, false, false, false, false},
    {"gmUfoBallHold", L"ufo_ball",      L"ufo_ballfo_C",       L"balls",    22, 40, true,  false, false, false},
    {"gmUfoBoo",      L"ufo_boo",       L"ufo_boofo_spawn1_C", L"ufo",      22, 40, false, false, false, false},
    {"gmUfoJoel",     L"ufo_joel",      L"ufo_joel_C",         L"point_a1", 12, 40, false, true,  false, false},
    {"gmUfoPill",     L"ufo_pill",      L"ufo_pillfo_C",       L"ufo",      22, 40, false, false, false, false},
    {"gmUfoMidas",    L"ufo_midas",     L"midasUfoTest_C",     L"forward",  22, 40, false, false, false, true},
    {"gmBody",        L"tickerBody",    L"theBody_C",          nullptr,     22, 40, false, false, false, false},
    {"gmFunguy",      L"tickerFunguy",  L"npc_funguy_C",       nullptr,     22, 40, false, false, false, false},
    {"gmFigura",      L"CustomEvent_3", L"figura_C",           nullptr,     22, 40, false, false, true,  false},
};

const Spec* SpecOf(const std::string& n) {
    for (const Spec& s : kSpecs)
        if (n == s.name) return &s;
    return nullptr;
}

constexpr uint64_t kClientRollMs = 30000, kStandInMs = 6000, kHoldMs = 12000, kSayEveryMs = 5000, kSayUntilMs = 150000;
constexpr int kCallsPerTick = 40, kMaxCalls = 6000;
// The Midas UFO is a level actor the test save's level may not hold: each peer then stands one at the same fixed point, and the
// host calls its own `setActive(true)`, the call the gamemode's ufo_midas makes through the trigger interface (the roll finds it
// by a key, which a stood one lacks).
constexpr const wchar_t* kMidasPath = L"/Game/objects/misc/midasufotest.midasufotest_C";
constexpr float kStandX = -1692.f + 900.f, kStandY = 560.f, kStandZ = 6318.f + 150.f;
bool g_hostStood = false, g_clientStood = false, g_hostCalledOn = false;
int  g_hostBase = -1;   // a per-player creature: what the host held before its own roll (the other peer's mirror counts)

uint64_t g_clientSeenMs = 0, g_lastSayHost = 0, g_lastSayClient = 0;
bool     g_clientRolled = false, g_hostDone = false, g_heldDone = false, g_stoodIn = false;
int      g_hostCalls = 0, g_clientCalls = 0;

size_t CountLive(const wchar_t* cls) {
    size_t n = 0;
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++n;
    return n;
}

void* FirstLive(const wchar_t* cls) {
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) return obj;
    return nullptr;
}

bool ReadFlag(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return obj && R::FindBoolProperty(R::ClassOf(obj), name, off, mask) && off >= 0 &&
           (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
}

float ReadFloat(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    float v = -1.f;
    if (off >= 0) std::memcpy(&v, static_cast<const uint8_t*>(obj) + off, sizeof(v));
    return v;
}

void StandMidasIfAbsent(const char* who, const Spec& s) {
    if (FirstLive(s.product)) return;
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kMidasPath);
    void* a = cls ? E::SpawnActor(cls, {kStandX, kStandY, kStandZ}) : nullptr;
    UE_LOGI("[GM-DRILL] [%s] the level holds no %ls: this peer stood %p (class %p)", who, s.product, a, cls);
}

// How many of the product the roll counts: the live instances, or for a level actor whether it is switched on.
size_t Held(const Spec& s) {
    if (!s.isFlag) return CountLive(s.product);
    return ReadFlag(FirstLive(s.product), L"active") ? 1 : 0;
}

// One call of the gamemode's handler with the clock put at (hour, day) for its length: the cycle's time vector holds the hour,
// the minute and the day as three ints, and the first and the last are the two the handlers read.
bool RollOnce(const Spec& s) {
    void* gm = ue_wrap::world_singleton::Gamemode();
    void* cycle = FirstLive(L"daynightCycle_C");
    void* fn = gm ? R::FindDispatchFunctionCached(R::ClassOf(gm), s.fn) : nullptr;
    const int32_t off = cycle ? R::FindPropertyOffset(R::ClassOf(cycle), L"timeZ") : -1;
    if (!fn || off < 0) return false;
    int32_t* clock = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(cycle) + off);
    const int32_t saved[3] = {clock[0], clock[1], clock[2]};
    clock[0] = s.hour;
    clock[2] = s.day;
    ue_wrap::ParamFrame f(fn);
    const bool ran = f.valid() && ue_wrap::Call(gm, f);
    clock[0] = saved[0];
    clock[1] = saved[1];
    clock[2] = saved[2];
    return ran;
}

// What this peer shows for the drill now: how many of the product it holds, and, for each, its place and the place of one
// moving component of it. A flyer's actor stays at its spawn point and its component flies, so the component is the sample.
void Say(const char* who, const Spec& s, uint64_t sinceMs) {
    const unsigned t = static_cast<unsigned>(sinceMs / 1000);
    size_t n = 0;
    for (void* obj : R::FindObjectsByClass(s.product)) {
        if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        ++n;
        ue_wrap::FVector at{}, comp{};
        E::TryGetActorLocation(obj, at);
        const int32_t off = s.probe ? R::FindPropertyOffset(R::ClassOf(obj), s.probe) : -1;
        void* c = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(obj) + off) : nullptr;
        const bool haveComp = c && R::IsLive(c) && E::TryGetComponentLocation(c, comp);
        if (s.isFlag)
            UE_LOGI("[GM-DRILL] [%s] %s t=%us: %ls #%zu active %d progress %.3f, %ls at (%.0f, %.0f, %.0f)", who, s.name, t, s.product, n,
                    ReadFlag(obj, L"active") ? 1 : 0, ReadFloat(obj, L"a"), s.probe, comp.X, comp.Y, comp.Z);
        else if (haveComp)
            UE_LOGI("[GM-DRILL] [%s] %s t=%us: %ls #%zu at (%.0f, %.0f, %.0f), %ls at (%.0f, %.0f, %.0f)", who, s.name, t, s.product, n, at.X,
                    at.Y, at.Z, s.probe, comp.X, comp.Y, comp.Z);
        else
            UE_LOGI("[GM-DRILL] [%s] %s t=%us: %ls #%zu at (%.0f, %.0f, %.0f)", who, s.name, t, s.product, n, at.X, at.Y, at.Z);
    }
    UE_LOGI("[GM-DRILL] [%s] %s t=%us: %zu %ls live", who, s.name, t, n, s.product);
}

bool SayDue(uint64_t sinceMs, uint64_t& last) {
    if (sinceMs > kSayUntilMs) return false;
    const uint64_t bucket = sinceMs / kSayEveryMs + 1;
    if (bucket <= last) return false;
    last = bucket;
    return true;
}


// ---- evilEnd: the Evil's ending, forced on the host -----------------------------------------------------------------
// The host stands a theEvil_C as the base computer does (its `active` up), with `force` set and five seconds on its clock, so
// its own tick reaches the ending without a signal on a drive. Both peers then say what they hold every five seconds.
constexpr const char* kEvilName = "evilEnd";
bool g_evilStood = false;

void WriteBool(void* obj, const wchar_t* name, bool on) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return;
    uint8_t* b = static_cast<uint8_t*>(obj) + off;
    *b = on ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
}

void EvilSay(const char* who, uint64_t sinceMs) {
    size_t effects = 0;
    for (void* obj : R::FindObjectsByClass(L"theEvil_C")) {
        if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), L"postprocess");
        void* pp = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(obj) + off) : nullptr;
        if (pp && R::IsLive(pp) && ReadFlag(pp, L"bEnabled")) ++effects;
    }
    UE_LOGI("[GM-DRILL] [%s] evilEnd t=%us: %zu theEvil_C live (%zu with the screen effect on), %zu gameover_C live, paused %d", who,
            static_cast<unsigned>(sinceMs / 1000), CountLive(L"theEvil_C"), effects, CountLive(L"gameover_C"), E::IsGamePaused() ? 1 : 0);
}

void EvilHostStep(uint64_t since) {
    if (SayDue(since, g_lastSayHost)) EvilSay("H", since);
    if (g_evilStood || since < 10000) return;
    g_evilStood = true;
    void* cls = R::FindClass(L"theEvil_C");
    void* evil = cls ? E::SpawnActor(cls, {0.f, 0.f, 0.f}) : nullptr;
    if (evil) {
        WriteBool(evil, L"force", true);
        const int32_t off = R::FindPropertyOffset(R::ClassOf(evil), L"time");
        const float five = 5.f;
        if (off >= 0) std::memcpy(static_cast<uint8_t*>(evil) + off, &five, sizeof(five));
        WriteBool(evil, L"active", true);
    }
    UE_LOGI("[GM-DRILL] [H] evilEnd: a theEvil_C stood %p with force set and 5 s on its clock", evil);
}

}  // namespace

bool IsGamemodeDrill(const std::string& n) { return easter_drill::IsDrill(n) || n == kEvilName || level_fixture_drill::IsDrill(n) || level_prop_drill::IsDrill(n) || vending_drill::IsDrill(n) || erie_drill::IsDrill(n) || SpecOf(n) != nullptr; }

void ClientStep(const std::string& name) {
    if (easter_drill::IsDrill(name)) { easter_drill::ClientStep(); return; }
    if (level_fixture_drill::IsDrill(name)) { level_fixture_drill::ClientStep(); return; }
    if (level_prop_drill::IsDrill(name)) { level_prop_drill::ClientStep(name); return; }
    if (vending_drill::IsDrill(name)) { vending_drill::ClientStep(name); return; }
    if (erie_drill::IsDrill(name)) { erie_drill::ClientStep(name); return; }
    if (name == kEvilName) {
        const uint64_t t = coop::net::NowMs();
        if (!g_clientSeenMs) g_clientSeenMs = t;
        if (SayDue(t - g_clientSeenMs, g_lastSayClient)) EvilSay("C", t - g_clientSeenMs);
        return;
    }
    const Spec* s = SpecOf(name);
    if (!s) return;
    const uint64_t now = coop::net::NowMs();
    if (!g_clientSeenMs) g_clientSeenMs = now;
    const uint64_t since = now - g_clientSeenMs;
    if (SayDue(since, g_lastSayClient)) Say("C", *s, since);
    if (s->isFlag && !g_clientStood && since >= 12000) {
        g_clientStood = true;
        StandMidasIfAbsent("C", *s);
    }
    // This peer's own roll of the game's timer handler. For a host-owned thing the spawn authority refuses it, so the product
    // count must not rise by it; for the Figura it is the point, and runs until this peer's own creature stands.
    if (!g_clientRolled && since >= kClientRollMs) {
        const size_t before = Held(*s);
        const int limit = s->bothRoll ? kMaxCalls : 1500;
        for (int i = 0; i < kCallsPerTick && g_clientCalls < limit; ++i, ++g_clientCalls) {
            RollOnce(*s);
            if (s->bothRoll && Held(*s) > before) { ++g_clientCalls; break; }
        }
        if (g_clientCalls >= limit || (s->bothRoll && Held(*s) > before)) {
            g_clientRolled = true;
            UE_LOGI("[GM-DRILL] [C] %s: this peer's own %ls ran %d call(s) in all; %ls now held %zu (it held %zu before the first call)",
                    name.c_str(), s->fn, g_clientCalls, s->product, Held(*s), before);
        }
    }
}

void HostStep(const std::string& name, uint64_t since) {
    if (easter_drill::IsDrill(name)) { easter_drill::HostStep(since); return; }
    if (name == kEvilName) { EvilHostStep(since); return; }
    if (level_fixture_drill::IsDrill(name)) { level_fixture_drill::HostStep(since); return; }
    if (level_prop_drill::IsDrill(name)) { level_prop_drill::HostStep(name, since); return; }
    if (vending_drill::IsDrill(name)) { vending_drill::HostStep(name, since); return; }
    if (erie_drill::IsDrill(name)) { erie_drill::HostStep(name, since); return; }
    const Spec* s = SpecOf(name);
    if (!s) return;
    if (SayDue(since, g_lastSayHost)) Say("H", *s, since);
    if (s->isFlag) {
        if (!g_hostStood && since >= 2000) {
            g_hostStood = true;
            StandMidasIfAbsent("H", *s);
        }
        if (!g_hostCalledOn && since >= 20000) {
            g_hostCalledOn = true;
            void* ufo = FirstLive(s->product);
            void* fn = ufo ? R::FindDispatchFunctionCached(R::ClassOf(ufo), L"setActive") : nullptr;
            ue_wrap::ParamFrame f(fn);
            const bool ran = fn && f.valid() && f.Set<bool>(L"active", true) && ue_wrap::Call(ufo, f);
            UE_LOGI("[GM-DRILL] [H] %s: the flyer's setActive(true) called: %d", name.c_str(), ran ? 1 : 0);
        }
    }
    if (!s->isFlag && !g_hostDone && since >= 8000) {
        if (g_hostBase < 0) g_hostBase = s->bothRoll ? static_cast<int>(Held(*s)) : 0;
        // The drill restarts when a client rejoins: a flyer or a body the host still holds from before is what the joiner is owed,
        // so no second one is rolled.
        if (!s->bothRoll && g_hostCalls == 0 && Held(*s) > 0) {
            g_hostDone = true;
            UE_LOGI("[GM-DRILL] [H] %s: the host already holds %zu %ls from before, so none is rolled", name.c_str(), Held(*s), s->product);
        }
        for (int i = 0; i < kCallsPerTick && !g_hostDone; ++i) {
            RollOnce(*s);
            ++g_hostCalls;
            if (static_cast<int>(Held(*s)) > g_hostBase) {
                g_hostDone = true;
                UE_LOGI("[GM-DRILL] [H] %s: the game's own %ls came up after %d call(s): %ls", name.c_str(), s->fn, g_hostCalls, s->product);
            }
        }
        if (!g_hostDone && s->standIn && since >= kStandInMs + 8000 && !g_stoodIn) {
            // The roll needs a player at the radio tower's top. The class is stood 80 m above the host's player and enrolled as the
            // deferred-spawn catch enrols a gamemode's mint; the seam itself is read off the other UFOs' drills.
            g_stoodIn = true;
            void* cls = ue_wrap::object_index::ClassByName(s->product);
            void* player = coop::players::Registry::Get().Local();
            ue_wrap::FVector at{};
            void* actor = cls && player && E::TryGetActorLocation(player, at) ? E::SpawnActor(cls, {at.X, at.Y, at.Z + 8000.f}) : nullptr;
            const unsigned eid = actor ? coop::world_actor_sync::HostEnrollExSpawn(actor) : 0;
            g_hostDone = actor != nullptr;
            UE_LOGI("[GM-DRILL] [H] %s: %d call(s) of %ls made nothing; the class stood %p (%ls) and enrolled as world actor %u", name.c_str(),
                    g_hostCalls, s->fn, actor, s->product, eid);
        }
        if (!g_hostDone && g_hostCalls >= kMaxCalls) {
            g_hostDone = true;
            UE_LOGW("[GM-DRILL] [H] FAIL %s: %d calls of %ls made no %ls (the world may hold none, or the gate was not met)", name.c_str(),
                    g_hostCalls, s->fn, s->product);
        }
    }
    // A flyer held for the rejoin: its tick stops, so it neither flies on nor destroys itself.
    if (s->hold && g_hostDone && !g_heldDone && since >= kHoldMs + 8000) {
        void* ufo = FirstLive(s->product);
        if (ufo) {
            g_heldDone = true;
            E::SetActorTickEnabled(ufo, false);
            UE_LOGI("[GM-DRILL] [H] %s: the host's %ls held (its tick stopped) so it outlasts its flight", name.c_str(), s->product);
        }
    }
}

void OnDisconnect() {
    g_clientSeenMs = g_lastSayHost = g_lastSayClient = 0;
    g_clientRolled = g_hostDone = g_heldDone = g_stoodIn = false;
    g_hostStood = g_clientStood = g_hostCalledOn = false;
    g_evilStood = false;
    easter_drill::OnDisconnect();
    level_fixture_drill::OnDisconnect();
    level_prop_drill::OnDisconnect();
    vending_drill::OnDisconnect();
    erie_drill::OnDisconnect();
    g_hostBase = -1;
    g_hostCalls = g_clientCalls = 0;
}

}  // namespace coop::dev::gamemode_drill
