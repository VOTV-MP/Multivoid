// coop/dev/world_event_drill.cpp -- see coop/dev/world_event_drill.h.

#include "coop/dev/world_event_drill.h"

#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/skysphere.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstring>

namespace coop::dev::world_event_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace SKY = ue_wrap::skysphere;

constexpr const wchar_t* kTowerPath = L"/Game/objects/arirShipAppear.arirShipAppear_C";
constexpr const wchar_t* kLakePath = L"/Game/objects/lakeglow.lakeglow_C";
constexpr const wchar_t* kLockerPath = L"/Game/objects/locker.locker_C";
constexpr const wchar_t* kCorpsePath = L"/Game/objects/misc/lockerCorpse.lockerCorpse_C";
constexpr const wchar_t* kRainPath = L"/Game/objects/event_fleshRain.event_fleshRain_C";
// Where a stood level object goes when the save lacks one: the host's fixed place (the only one a drill may use), the same on
// both peers, a little off the ground so it settles alike.
constexpr float kStandX = -1692.f + 900.f, kStandY = 560.f, kStandZ = 6318.f + 150.f;

uint64_t g_clientSeenMs = 0;
bool     g_clientStood = false, g_clientRolled = false;
bool     g_hostStood = false, g_hostCalled1 = false, g_hostCalled2 = false, g_hostDone = false;
int      g_hostTries = 0;
uint64_t g_lastSayHost = 0, g_lastSayClient = 0;
void*    g_stood = nullptr;

void* LiveInstanceOf(const wchar_t* cls) {
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) return obj;
    return nullptr;
}

size_t CountLive(const wchar_t* cls) {
    size_t n = 0;
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++n;
    return n;
}

bool ReadFlag(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return obj && R::FindBoolProperty(R::ClassOf(obj), name, off, mask) && off >= 0 &&
           (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
}

void* Stand(const wchar_t* path, ue_wrap::FVector at) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(path);
    return cls ? ue_wrap::engine::SpawnActor(cls, at) : nullptr;
}

// A function of no arguments, or of one bool, `obj` declares or inherits; false when it did not resolve or run.
bool CallVerb(void* obj, const wchar_t* fnName, const wchar_t* boolArg = nullptr, bool value = false) {
    void* fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fnName) : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || (boolArg && !f.Set<bool>(boolArg, value))) return false;
    return ue_wrap::Call(obj, f);
}

bool RingLive(void* panel) {
    const int32_t off = panel ? R::FindPropertyOffset(R::ClassOf(panel), L"timerHandle_redphone") : -1;
    uint64_t handle = 0;
    if (off >= 0) std::memcpy(&handle, static_cast<const uint8_t*>(panel) + off, sizeof(handle));
    return handle != 0;
}

size_t HeadCount() {
    size_t n = 0;
    for (void* l : R::FindObjectsByClass(L"locker_C"))
        if (l && R::IsLive(l) && !R::NameStartsWith(R::NameOf(l), L"Default__") && ReadFlag(l, L"head")) ++n;
    return n;
}

// What this peer shows for `name` now.
void SayState(const char* who, const std::string& name, uint64_t sinceMs) {
    const unsigned s = static_cast<unsigned>(sinceMs / 1000);
    if (name == "earthTp" || name == "earthTpHold") {
        bool hole = false;
        const bool read = SKY::ReadBlackHole(hole);
        UE_LOGI("[WORLD-DRILL] [%s] %s t=%us: black-hole sky %d (read %d)", who, name.c_str(), s, hole ? 1 : 0, read ? 1 : 0);
    } else if (name == "arirShip") {
        void* t = LiveInstanceOf(L"arirShipAppear_C");
        UE_LOGI("[WORLD-DRILL] [%s] arirShip t=%us: %zu tower(s), visible %d", who, s, CountLive(L"arirShipAppear_C"), ReadFlag(t, L"visible") ? 1 : 0);
    } else if (name == "lakeGlow") {
        void* l = LiveInstanceOf(L"lakeglow_C");
        UE_LOGI("[WORLD-DRILL] [%s] lakeGlow t=%us: %zu lake(s), isActive %d", who, s, CountLive(L"lakeglow_C"), ReadFlag(l, L"isActive") ? 1 : 0);
    } else if (name == "radioPoof") {
        void* tower = LiveInstanceOf(L"radiotower_C");
        UE_LOGI("[WORLD-DRILL] [%s] radioPoof t=%us: %zu radiotowerPoof_C live, the radio tower hidden %d", who, s, CountLive(L"radiotowerPoof_C"),
                ReadFlag(tower, L"bHidden") ? 1 : 0);
    } else if (name == "redPhone") {
        void* p = LiveInstanceOf(L"analogDScreenTest_C");
        UE_LOGI("[WORLD-DRILL] [%s] redPhone t=%us: %zu panel(s), ringing %d", who, s, CountLive(L"analogDScreenTest_C"), RingLive(p) ? 1 : 0);
    } else if (name == "lockerHead") {
        UE_LOGI("[WORLD-DRILL] [%s] lockerHead t=%us: %zu locker_C, %zu with a head, %zu lockerguy_C live", who, s, CountLive(L"locker_C"), HeadCount(),
                CountLive(L"lockerguy_C"));
    } else if (name == "lockerCorpse") {
        UE_LOGI("[WORLD-DRILL] [%s] lockerCorpse t=%us: %zu lockerCorpse_C, %zu prop_C, %zu prop_food_C, %zu grime_blood2_C live", who, s,
                CountLive(L"lockerCorpse_C"), CountLive(L"prop_C"), CountLive(L"prop_food_C"), CountLive(L"grime_blood2_C"));
    } else if (name == "fleshRain") {
        UE_LOGI("[WORLD-DRILL] [%s] fleshRain t=%us: %zu event_fleshRain_C, %zu prop_garbageClump_C live", who, s, CountLive(L"event_fleshRain_C"),
                CountLive(L"prop_garbageClump_C"));
    }
}

// How often a peer says its state, and until when: once per `everyMs` bucket, `last` being the latest bucket said, plus one.
bool SayDue(uint64_t sinceMs, uint64_t& last, uint64_t everyMs, uint64_t untilMs) {
    if (sinceMs > untilMs) return false;
    const uint64_t bucket = sinceMs / everyMs + 1;
    if (bucket <= last) return false;
    last = bucket;
    return true;
}

// A level object the save lacks, stood once per peer.
void StandIfAbsent(const char* who, const wchar_t* cls, const wchar_t* path, ue_wrap::FVector at) {
    if (LiveInstanceOf(cls)) return;
    void* a = Stand(path, at);
    UE_LOGI("[WORLD-DRILL] [%s] the save holds no %ls: this peer stood %p at (%.0f, %.0f, %.0f)", who, cls, a, at.X, at.Y, at.Z);
}

const wchar_t* LevelClassOf(const std::string& name, const wchar_t** path) {
    if (name == "arirShip") { *path = kTowerPath; return L"arirShipAppear_C"; }
    if (name == "lakeGlow") { *path = kLakePath; return L"lakeglow_C"; }
    if (name == "lockerHead") { *path = kLockerPath; return L"locker_C"; }
    return nullptr;
}

// One roll of the game's own timer handler (the gamemode's tickers, the panel's timer): the verb the game calls on its interval.
bool RollOnce(const std::string& name) {
    if (name == "radioPoof") return CallVerb(ue_wrap::world_singleton::Gamemode(), L"ticker_radioTowerPoof");
    if (name == "lockerHead") return CallVerb(ue_wrap::world_singleton::Gamemode(), L"ticker_lockerHead");
    if (name == "redPhone") return CallVerb(LiveInstanceOf(L"analogDScreenTest_C"), L"redPhoneEventTimer");
    return false;
}

bool RolledYet(const std::string& name) {
    if (name == "radioPoof") return CountLive(L"radiotowerPoof_C") > 0;
    if (name == "lockerHead") return HeadCount() > 0;
    if (name == "redPhone") return RingLive(LiveInstanceOf(L"analogDScreenTest_C"));
    return true;
}

bool IsRollDrill(const std::string& n) { return n == "radioPoof" || n == "lockerHead" || n == "redPhone"; }

}  // namespace

bool IsWorldEventDrill(const std::string& n) {
    return n == "earthTp" || n == "earthTpHold" || n == "arirShip" || n == "lakeGlow" || IsRollDrill(n) || n == "lockerCorpse" || n == "fleshRain";
}

bool IsStandalone(const std::string& n) { return IsWorldEventDrill(n) && n != "earthTp" && n != "arirShip"; }

void ClientStep(const std::string& name) {
    const uint64_t now = coop::net::NowMs();
    if (!g_clientSeenMs) g_clientSeenMs = now;
    const uint64_t since = now - g_clientSeenMs;
    const uint64_t window = name == "earthTp" ? 140000 : 70000;
    if (SayDue(since, g_lastSayClient, 10000, window)) SayState("C", name, since);
    // 12 s in the session: a level object the host's save carried, if this peer's world lacks it.
    if (!g_clientStood && since > 12000) {
        g_clientStood = true;
        const wchar_t* path = nullptr;
        if (const wchar_t* cls = LevelClassOf(name, &path)) StandIfAbsent("C", cls, path, {kStandX, kStandY, kStandZ});
    }
    // 14 s: this peer's own roll of the game's timer handler, which the spawn authority refuses, or its own meat rain, which the
    // deferred-spawn refusal does. The lines to read are the refusals and the state a few seconds on.
    if (!g_clientRolled && since > 14000) {
        g_clientRolled = true;
        if (IsRollDrill(name)) {
            const int tries = name == "redPhone" ? 600 : 1500;
            int ran = 0;
            for (int i = 0; i < tries; ++i) ran += RollOnce(name) ? 1 : 0;
            UE_LOGI("[WORLD-DRILL] [C] %s: this peer's own roll ran %d of %d times", name.c_str(), ran, tries);
            SayState("C", name, since);
        } else if (name == "fleshRain") {
            void* cls = ue_wrap::asset_load::LoadObjectByPath(kRainPath);
            void* made = cls ? ue_wrap::engine::SpawnActor(cls, {0.f, 0.f, 0.f}) : nullptr;
            UE_LOGI("[WORLD-DRILL] [C] fleshRain: this peer's own event_fleshRain_C spawn made %p (class %p)", made, cls);
        } else if (name == "lakeGlow") {
            UE_LOGI("[WORLD-DRILL] [C] lakeGlow: this peer's own ticker_lakeMonsert called: %d", CallVerb(ue_wrap::world_singleton::Gamemode(), L"ticker_lakeMonsert") ? 1 : 0);
        }
    }
}

void HostStep(const std::string& name, uint64_t since) {
    if (SayDue(since, g_lastSayHost, 10000, name == "earthTp" ? 140000 : 70000)) SayState("H", name, since);
    if (!g_hostStood && since >= 2000) {
        g_hostStood = true;
        const wchar_t* path = nullptr;
        if (const wchar_t* cls = LevelClassOf(name, &path)) StandIfAbsent("H", cls, path, {kStandX, kStandY, kStandZ});
    }
    if (name == "earthTpHold" && !g_hostCalled1 && since >= 4000) {
        g_hostCalled1 = true;
        UE_LOGI("[WORLD-DRILL] [H] earthTpHold: the sky put into the black hole by the game's own two writes: %d", SKY::CallBlackHole(true) ? 1 : 0);
    }
    if (name == "arirShip") {
        void* tower = LiveInstanceOf(L"arirShipAppear_C");
        for (int step = 0; step < 2 && tower; ++step) {
            bool& called = step == 0 ? g_hostCalled1 : g_hostCalled2;
            if (called || since < (step == 0 ? 16000u : 50000u)) continue;
            called = true;
            void* fn = R::FindDispatchFunctionCached(R::ClassOf(tower), L"runTrigger");
            ue_wrap::ParamFrame f(fn);
            bool ran = false;
            if (fn && f.valid()) {
                f.Set<void*>(L"owner", tower);
                f.Set<int32_t>(L"index", step);
                ran = ue_wrap::Call(tower, f);
            }
            UE_LOGI("[WORLD-DRILL] [H] arirShip: the tower's runTrigger(index %d) called: %d", step, ran ? 1 : 0);
        }
    }
    if (name == "lakeGlow") {
        void* lake = LiveInstanceOf(L"lakeglow_C");
        if (lake && !g_hostCalled1 && since >= 10000) {
            g_hostCalled1 = true;
            UE_LOGI("[WORLD-DRILL] [H] lakeGlow: setActive(true) called: %d", CallVerb(lake, L"setActive", L"activate", true) ? 1 : 0);
        }
        if (lake && !g_hostCalled2 && since >= 50000) {
            g_hostCalled2 = true;
            UE_LOGI("[WORLD-DRILL] [H] lakeGlow: setActive(false) called: %d", CallVerb(lake, L"setActive", L"activate", false) ? 1 : 0);
        }
    }
    if (IsRollDrill(name) && !g_hostDone && since >= 8000) {
        // Forty rolls to a tick, until the roll has come up once (a 0.5 % or 2.5 % or 8 % chance each).
        for (int i = 0; i < 40 && !g_hostDone; ++i) {
            RollOnce(name);
            ++g_hostTries;
            if (RolledYet(name)) {
                g_hostDone = true;
                UE_LOGI("[WORLD-DRILL] [H] %s: the game's own roll came up after %d call(s)", name.c_str(), g_hostTries);
            }
        }
        if (!g_hostDone && g_hostTries >= 4000) {
            g_hostDone = true;
            UE_LOGW("[WORLD-DRILL] [H] FAIL %s: %d calls of the game's roll made nothing (the world may hold no such object)", name.c_str(), g_hostTries);
        }
    }
    // A head's locker is opened 40 s on, the call a use makes: the lockerguy scare, and the open reaching the other peer's locker.
    if (name == "lockerHead" && g_hostDone && !g_hostCalled1 && since >= 40000) {
        g_hostCalled1 = true;
        for (void* l : R::FindObjectsByClass(L"locker_C")) {
            if (!l || !R::IsLive(l) || !ReadFlag(l, L"head")) continue;
            UE_LOGI("[WORLD-DRILL] [H] lockerHead: the head locker's open(true) called: %d", CallVerb(l, L"open", L"opened", true) ? 1 : 0);
            break;
        }
    }
    if (name == "lockerCorpse" && !g_hostCalled1 && since >= 6000) {
        g_hostCalled1 = true;
        void* player = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!player || !ue_wrap::engine::TryGetActorLocation(player, at)) {
            UE_LOGW("[WORLD-DRILL] [H] FAIL lockerCorpse: no host player to stand the corpse beside");
            return;
        }
        SayState("H", name, since);
        g_stood = Stand(kCorpsePath, {at.X + 700.f, at.Y, at.Z + 50.f});
        UE_LOGI("[WORLD-DRILL] [H] lockerCorpse: the game's own corpse class stood 7 m from the host's player: %p", g_stood);
    }
    if (name == "fleshRain" && !g_hostCalled1 && since >= 8000) {
        g_hostCalled1 = true;
        g_stood = Stand(kRainPath, {0.f, 0.f, 0.f});
        UE_LOGI("[WORLD-DRILL] [H] fleshRain: the game's own rain class stood at the origin, where the day cycle's roll puts it: %p", g_stood);
    }
}

void OnDisconnect() {
    g_clientSeenMs = 0;
    g_clientStood = g_clientRolled = false;
    g_hostStood = g_hostCalled1 = g_hostCalled2 = g_hostDone = false;
    g_hostTries = 0;
    g_lastSayHost = g_lastSayClient = 0;
    g_stood = nullptr;
}

}  // namespace coop::dev::world_event_drill
