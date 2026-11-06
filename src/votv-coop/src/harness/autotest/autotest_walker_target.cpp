// harness/autotest/autotest_walker_target.cpp -- TEST-ONLY: a walking mannequin goes for the nearest player.
//
// The host spawns a walker on open ground and both peers look away. The peers stand where each step needs them, and each step
// reads which way the walker closes: the host nearest, the client nearest, the host nearest again, the host out of its range,
// the host down. The client
// then angers its own copy, which must anger the host's, the walker's hit must take the client's health with its sound played
// there and not on the host, and a client lying down must take no more. Last the host angers a second walker of its own, which
// must anger the client's copy. Gated by VOTVCOOP_RUN_WALKER_TARGET; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/dev/mannequin_drill.h"
#include "coop/player/player_damage.h"
#include "coop/player/player_hit.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_nav.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E  = ue_wrap::engine;
namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace V  = ue_wrap::vitals;

std::string g_dir;
int g_fails = 0;

// The game's clip check: a camera that crosses static geometry between two frames sends its player to the backrooms, after a
// save. This drill's teleports can cross walls, so each one turns that off first; what the switch was the first time is given
// back when the drill ends. The gamemode is the world's, so this is asked for each teleport, never before the world stands.
bool g_backroomsHeld = false, g_backroomsWas = false;

uint8_t* BackroomsFlag(uint8_t& mask) {
    void* gm = ue_wrap::world_singleton::Gamemode();
    int32_t off = -1;
    if (!gm || !ue_wrap::reflection::IsLive(gm) ||
        !ue_wrap::reflection::FindBoolProperty(ue_wrap::reflection::ClassOf(gm), L"backroomsEnabled", off, mask))
        return nullptr;
    return static_cast<uint8_t*>(gm) + off;
}

void HoldBackroomsOff() {
    uint8_t mask = 0;
    uint8_t* b = BackroomsFlag(mask);
    if (!b) return;
    if (!g_backroomsHeld) {
        g_backroomsHeld = true;
        g_backroomsWas = (*b & mask) != 0;
    }
    *b = static_cast<uint8_t>(*b & ~mask);
}

void GiveBackrooms() {
    uint8_t mask = 0;
    uint8_t* b = BackroomsFlag(mask);
    if (!b || !g_backroomsHeld) return;
    *b = g_backroomsWas ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
}

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name) {
    std::string tmp = g_dir + "\\" + name + ".tmp";
    { std::ofstream(tmp, std::ios::binary | std::ios::trunc) << "x"; }
    ::MoveFileExA(tmp.c_str(), (g_dir + "\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Seen(const char* name) { return static_cast<bool>(std::ifstream(g_dir + "\\" + name)); }

bool Await(const char* name, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (Seen(name)) return true;
        ::Sleep(100);
    }
    UE_LOGW("walker-target: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("walker-target: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

void* FindWalker(void* other = nullptr) {
    std::vector<void*> walkers;
    AC::ListWalkers(walkers);
    for (void* w : walkers)
        if (w != other) return w;
    return nullptr;
}

float Flat(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// The game's ticker_foodSleeper_C drops a player under 20 food into a ragdoll at random, which the rig save's players are.
void KeepFed() {
    V::Write(V::Field::Food, 100.f);
    V::Write(V::Field::Sleep, 100.f);
}

// Looks away from the walker, so no view here freezes it.
void FaceAway(void* walker) {
    KeepFed();
    void* pawn = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!pawn || !walker || !R::IsLive(walker) || !E::TryGetActorLocation(walker, at)) return;
    const ue_wrap::FVector me = E::GetCameraLocation();
    const float yaw = std::atan2(at.Y - me.Y, at.X - me.X) * 57.29578f + 180.f;
    E::SetControlRotation(E::GetController(pawn), ue_wrap::FRotator{0.f, yaw, 0.f});
}

// Keeps this peer's back to the walker until `name` appears.
void HoldAwayUntil(void* walker, const char* name, int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (Seen(name)) return;
        OnGameThread([&] { FaceAway(walker); });
        ::Sleep(100);
    }
}

// The host's body and the client's puppet, by where the walker stands now; false when any is unread.
bool Places(void* walker, ue_wrap::FVector& at, ue_wrap::FVector& host, ue_wrap::FVector& client) {
    auto& reg = coop::players::Registry::Get();
    coop::RemotePlayer* rp = reg.Puppet(1);
    void* puppet = rp && rp->valid() ? rp->GetActor() : nullptr;
    void* me = reg.Local();
    return me && puppet && E::TryGetActorLocation(walker, at) && E::TryGetActorLocation(me, host) &&
           E::TryGetActorLocation(puppet, client);
}

// Runs `seconds` with the host's back turned, and says how far the walker closed on each player and how far it walked.
void Measure(void* walker, int seconds, float& closedHost, float& closedClient, float& walked) {
    ue_wrap::FVector a0{}, h0{}, c0{}, a1{}, h1{}, c1{};
    OnGameThread([&] { Places(walker, a0, h0, c0); });
    for (int i = 0; i < seconds * 10; ++i) {
        OnGameThread([&] { FaceAway(walker); });
        ::Sleep(100);
    }
    OnGameThread([&] { Places(walker, a1, h1, c1); });
    closedHost = Flat(a0, h0) - Flat(a1, h1);
    closedClient = Flat(a0, c0) - Flat(a1, c1);
    walked = Flat(a0, a1);
    UE_LOGI("walker-target: walker (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f), host (%.0f,%.0f) client (%.0f,%.0f), closed host %.0f "
            "client %.0f, walked %.0f, looking=%d angry=%d", a0.X, a0.Y, a0.Z, a1.X, a1.Y, a1.Z, h1.X, h1.Y, c1.X, c1.Y, closedHost,
            closedClient, walked, AC::ReadWalkerLooking(walker) ? 1 : 0, AC::ReadWalkerAngry(walker) ? 1 : 0);
}

// A look or a teleport's turn holds it for a while: each step waits for its own timer and the lane's linger to let go.
bool WaitFree(void* walker) {
    bool free = false;
    for (int i = 0; i < 400 && !free; ++i) {
        OnGameThread([&] { FaceAway(walker); free = !AC::ReadWalkerLooking(walker); });
        ::Sleep(100);
    }
    return free;
}

// Stands this player on the ground at an offset from `at`, so a teleport is no fall and no ragdoll.
void StandAt(const ue_wrap::FVector& at, float dx, float dy) {
    OnGameThread([&] {
        void* pawn = coop::players::Registry::Get().Local();
        if (!pawn) return;
        // Its own level by a short box first: a tall one takes a drop below, and the route there is no straight walk.
        ue_wrap::FVector ground{at.X + dx, at.Y + dy, at.Z};
        if (!E::ProjectToNav(pawn, ground, {300.f, 300.f, 200.f}, &ground))
            E::ProjectToNav(pawn, ground, {300.f, 300.f, 3000.f}, &ground);
        HoldBackroomsOff();
        E::TeleportTo(pawn, {ground.X, ground.Y, ground.Z + 100.f}, {});
    });
}

void WritePlace(const char* name, const ue_wrap::FVector& at) {
    std::string tmp = g_dir + "\\" + name + ".tmp";
    { std::ofstream(tmp, std::ios::trunc) << at.X << ' ' << at.Y << ' ' << at.Z; }
    ::MoveFileExA(tmp.c_str(), (g_dir + "\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING);
}

ue_wrap::FVector ReadPlace(const char* name) {
    ue_wrap::FVector at{};
    std::ifstream(g_dir + "\\" + name) >> at.X >> at.Y >> at.Z;
    return at;
}

void SetDown(bool down) {
    OnGameThread([&] {
        void* pawn = coop::players::Registry::Get().Local();
        if (!pawn) return;
        if (down) E::SetMainPlayerRagdollMode(pawn, true, true, false);
        else E::ForceMainPlayerWakeup(pawn);
    });
}


void RunHost() {
    if (!Await("client_ready", 480)) return;
    const ue_wrap::FVector open{P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ};
    StandAt(open, 0.f, 0.f);
    ::Sleep(1500);
    coop::dev::mannequin_drill::SpawnWalker();
    void* walker = nullptr;
    for (int i = 0; i < 100 && !walker; ++i) {
        ::Sleep(100);
        OnGameThread([&] { walker = FindWalker(); });
    }
    Check(walker != nullptr, "a walker stands");
    if (!walker) return;
    // Its spawn counts as seen: the walker stands until its own timer lets go.
    Mark("walker");
    Check(WaitFree(walker), "with no viewer the walker goes free");
    float h = 0.f, c = 0.f, w = 0.f;
    ue_wrap::FVector at{};

    // The host nearest.
    OnGameThread([&] { E::TryGetActorLocation(walker, at); });
    StandAt(at, 0.f, 1000.f);
    WritePlace("place1", at);
    Mark("go1");
    if (!Await("there1")) return;
    Check(WaitFree(walker), "it goes free with the host nearest");
    Measure(walker, 2, h, c, w);
    Check(w > 100.f && h > 100.f && h > c, "with the host nearest it closes on the host");

    // The client nearest.
    OnGameThread([&] { E::TryGetActorLocation(walker, at); });
    StandAt(at, 0.f, 4000.f);
    WritePlace("place2", at);
    Mark("go2");
    if (!Await("there2")) return;
    Check(WaitFree(walker), "it goes free with the client nearest");
    Measure(walker, 2, h, c, w);
    Check(w > 100.f && c > 100.f && c > h, "with the client nearest it closes on the client");

    // The host nearest again: the walker turns back from the client.
    OnGameThread([&] { E::TryGetActorLocation(walker, at); });
    StandAt(at, 0.f, 1000.f);
    WritePlace("place2b", at);
    Mark("go2b");
    if (!Await("there2b")) return;
    Check(WaitFree(walker), "it goes free with the host nearest again");
    Measure(walker, 2, h, c, w);
    Check(w > 100.f && h > 100.f && h > c, "with the host nearest again it turns back to the host");

    // The host out of the walker's own range.
    OnGameThread([&] { E::TryGetActorLocation(walker, at); });
    StandAt(at, 25000.f, 0.f);
    WritePlace("place3", at);
    Mark("go3");
    if (!Await("there3")) return;
    Check(WaitFree(walker), "it goes free with the host out of range");
    Measure(walker, 2, h, c, w);
    Check(w > 100.f && c > 100.f, "with the host out of range it still closes on the client");

    // The host lying down does not hold a walker that goes for the client. Far off, so its fallen camera cannot see the walker.
    OnGameThread([&] { E::TryGetActorLocation(walker, at); });
    WritePlace("place4", at);
    Mark("go4");
    if (!Await("there4")) return;
    Check(WaitFree(walker), "it goes free before the host lies down");
    SetDown(true);
    ::Sleep(1000);
    int inView = 0;
    OnGameThread([&] {
        void* pawn = coop::players::Registry::Get().Local();
        inView = AC::WalkerInView(walker, pawn ? E::GetController(pawn) : nullptr) ? 1 : 0;
    });
    Measure(walker, 5, h, c, w);
    UE_LOGI("walker-target: host down, its view holds the walker=%d", inView);
    Check(w > 100.f && c > 100.f, "with the host down it still closes on the client");
    SetDown(false);
    ::Sleep(1000);

    // The client's anger reaches this walker, and its hit reaches the client.
    Check(!AC::ReadWalkerAngry(walker), "nobody has angered it yet");
    Mark("anger");
    bool angry = false;
    for (int i = 0; i < 100 && !angry; ++i) {
        OnGameThread([&] { FaceAway(walker); angry = AC::ReadWalkerAngry(walker); });
        ::Sleep(100);
    }
    Check(angry, "a client's anger reaches the host's walker");
    const uint32_t before = coop::player_damage::DeliveredCount();
    uint32_t hushedBefore = 0;
    OnGameThread([&] { hushedBefore = coop::player_damage::HushedSounds(); });
    Mark("angered");
    HoldAwayUntil(walker, "hit_done", 60);
    const uint32_t delivered = coop::player_damage::DeliveredCount() - before;
    uint32_t hushed = 0;
    OnGameThread([&] { hushed = coop::player_damage::HushedSounds() - hushedBefore; });
    UE_LOGI("walker-target: hits delivered %u, hit sounds refused here %u", delivered, hushed);
    Check(delivered > 0, "the walker's hit was delivered to the client");
    Check(hushed > 0, "the hit's sound did not play on the host");

    // A client lying down takes no more, as the game's own test spares a player in a ragdoll.
    if (!Await("client_down", 20)) return;
    // Only a span the puppet shows down from end to end is judged: a client that got up can be hit.
    uint32_t downHits = 0, lastHits = 0;
    bool lastDown = false;
    int downSpans = 0;
    for (int t = 0; t < 10; ++t) {
        HoldAwayUntil(walker, "never", 1);
        OnGameThread([&] {
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
            const bool down = rp && rp->IsRagdollDisplayed();
            const uint32_t hits = coop::player_damage::DeliveredCount();
            if (down && lastDown) {
                downHits += hits - lastHits;
                ++downSpans;
            }
            UE_LOGI("walker-target: +%ds the client's puppet shows it down=%d, hits so far %u", t + 1, down ? 1 : 0, hits);
            lastDown = down;
            lastHits = hits;
        });
    }
    Check(downSpans >= 3, "the client's puppet showed it down long enough to judge");
    UE_LOGI("walker-target: hits delivered while the client lay down: %u", downHits);
    Check(downHits == 0, "a client lying down takes no hit");
    Mark("down_done");

    // The host's own anger reaches a client's copy.
    if (!Await("up", 30)) return;
    coop::dev::mannequin_drill::SpawnWalker();
    void* second = nullptr;
    for (int i = 0; i < 100 && !second; ++i) {
        ::Sleep(100);
        OnGameThread([&] { second = FindWalker(walker); });
    }
    Check(second != nullptr, "a second walker stands");
    if (!second) return;
    Mark("walker2");
    if (!Await("calm2", 60)) return;
    OnGameThread([&] { AC::WriteWalkerAngry(second); });
    Mark("anger2");
    Await("angry2", 30);
    UE_LOGI("walker-target: DONE on the host fails=%d", g_fails);
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::players::Registry::Get().Local() && coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    Mark("client_ready");
    if (!Await("walker")) return;
    void* walker = nullptr;
    for (int i = 0; i < 300 && !walker; ++i) {
        ::Sleep(100);
        OnGameThread([&] { walker = FindWalker(); });
    }
    Check(walker != nullptr, "the walker crossed");
    if (!walker) return;
    HoldAwayUntil(walker, "go1", 120);
    StandAt(ReadPlace("place1"), -4000.f, 0.f);
    OnGameThread([&] { FaceAway(walker); });
    Mark("there1");
    HoldAwayUntil(walker, "go2", 60);
    StandAt(ReadPlace("place2"), -1000.f, 0.f);
    OnGameThread([&] { FaceAway(walker); });
    Mark("there2");
    HoldAwayUntil(walker, "go2b", 60);
    StandAt(ReadPlace("place2b"), -4000.f, 0.f);
    OnGameThread([&] { FaceAway(walker); });
    Mark("there2b");
    HoldAwayUntil(walker, "go3", 60);
    StandAt(ReadPlace("place3"), -1000.f, 0.f);
    OnGameThread([&] { FaceAway(walker); });
    Mark("there3");
    HoldAwayUntil(walker, "go4", 60);
    StandAt(ReadPlace("place4"), -1500.f, 0.f);
    OnGameThread([&] { FaceAway(walker); });
    Mark("there4");
    HoldAwayUntil(walker, "anger", 60);

    float before = 0.f;
    OnGameThread([&] {
        float maxHp = 100.f;
        V::Read(V::Field::MaxHealth, &maxHp);
        V::Write(V::Field::Health, maxHp);
        before = maxHp;
        AC::WriteWalkerAngry(walker);
    });
    HoldAwayUntil(walker, "angered", 20);
    // Stands in its way: the hit is what this step reads, not the walk.
    float low = before;
    uint32_t cuesBefore = 0;
    OnGameThread([&] { cuesBefore = coop::player_hit::CuesPlayed(); });
    for (int i = 0; i < 200 && low > before - 20.f; ++i) {
        OnGameThread([&] {
            if (i % 30 == 0) {
                ue_wrap::FVector w{};
                void* pawn = coop::players::Registry::Get().Local();
                HoldBackroomsOff();
                if (pawn && E::TryGetActorLocation(walker, w)) E::TeleportTo(pawn, {w.X + 60.f, w.Y, w.Z + 100.f}, {});
            }
            FaceAway(walker);
            float hp = 0.f;
            if (V::Read(V::Field::Health, &hp) && hp < low) low = hp;
        });
        ::Sleep(100);
    }
    ::Sleep(500);
    uint32_t cues = 0;
    OnGameThread([&] { cues = coop::player_hit::CuesPlayed() - cuesBefore; });
    UE_LOGI("walker-target: client health %.1f -> low %.1f, hit sounds played here %u", before, low, cues);
    Check(low <= before - 5.f, "the host's walker hit this client");
    Check(cues > 0, "the hit's sound played on this client");
    Mark("hit_done");

    SetDown(true);
    ::Sleep(500);
    float downFrom = 0.f;
    OnGameThread([&] { V::Read(V::Field::Health, &downFrom); });
    Mark("client_down");
    for (int i = 0; i < 150 && !Seen("down_done"); ++i) {
        OnGameThread([&] {
            FaceAway(walker);
            void* pawn = coop::players::Registry::Get().Local();
            bool rag = false, dead = false;
            if (pawn && E::ReadMainPlayerRagdollState(pawn, rag, dead)) {
                if (i % 10 == 0) UE_LOGI("walker-target: this client ragdoll=%d dead=%d", rag ? 1 : 0, dead ? 1 : 0);
                if (!rag) E::SetMainPlayerRagdollMode(pawn, true, true, false);
            }
        });
        ::Sleep(200);
    }
    float downTo = 0.f;
    OnGameThread([&] { V::Read(V::Field::Health, &downTo); });
    UE_LOGI("walker-target: lying down, client health %.1f -> %.1f", downFrom, downTo);
    SetDown(false);
    Mark("up");

    if (!Await("walker2", 60)) return;
    void* second = nullptr;
    for (int i = 0; i < 300 && !second; ++i) {
        ::Sleep(100);
        OnGameThread([&] { second = FindWalker(walker); });
    }
    Check(second != nullptr, "the second walker crossed");
    if (!second) return;
    bool calm = false;
    OnGameThread([&] { calm = !AC::ReadWalkerAngry(second); });
    Check(calm, "the second walker's copy is calm here");
    Mark("calm2");
    HoldAwayUntil(walker, "anger2", 30);
    bool angry = false;
    for (int i = 0; i < 100 && !angry; ++i) {
        OnGameThread([&] { FaceAway(second); angry = AC::ReadWalkerAngry(second); });
        ::Sleep(100);
    }
    Check(angry, "the host's anger reaches this client's copy");
    Mark("angry2");
    UE_LOGI("walker-target: DONE on the client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI WalkerTargetThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("walker-target: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else RunHost();
    ue_wrap::game_thread::RunAndWait([](std::atomic<int>& d) { GiveBackrooms(); d.store(1); });
    return 0;
}

}  // namespace harness::autotest
