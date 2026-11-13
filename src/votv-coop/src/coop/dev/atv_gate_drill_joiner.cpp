// coop/dev/atv_gate_drill_joiner.cpp -- step 10 of the ATV drill: a joiner's mirror is adopted AFTER the host's state for it, and
// takes it. A rejoining client adopts its save's ATV before the host's ready-edge state arrives, so the path the step judges
// (atv_host_sync's kept state for an ATV with no copy, applied at its adoption) is made here with an ATV the client's world
// never held: the host spawns a second ATV with the game's own spawn, away from every player, writes a distinct fuel on its copy
// and publishes it through the one publish point, so an AtvHostState for the new key goes out right after the spawn. The
// client runs with atv_park_first_spawn=1: the first arrival that would spawn a mirror is parked, and under this drill the park
// is not handed back until atv_host_sync has decided the state for that key (atv_spawn_park::HoldForDrill), so the state is
// kept first and the mirror is adopted after it. Decided on the client's events, never on a clock:
//   FAIL at a kUnadopted record for the key that was not kept (the red atv_joiner_rows_off drops it);
//   PASS at the kAdopted record for the key with a kept state applied that shows the host's fuel and health.

#include "atv_gate_drill_detail.h"

#include "atv_spawn_near.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/atv_sync.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

namespace coop::dev::atv_gate_drill::detail {
namespace {

namespace E = ue_wrap::engine;
namespace SN = coop::dev::atv_spawn_near;
using ue_wrap::FVector;

constexpr float kSeedFuel = 37.f;      // distinct from the other steps' values (20, 30, 10) and from a new ATV's own
constexpr float kSeedHealth = 77.f;
constexpr float kSame = 0.5f;          // the copy's fuel and health agree with the host's within this
constexpr float kClearCm = 700.f;      // the spawn point is this far, on the ground plane, from every player
constexpr float kRingsCm[] = {2000.f, 1500.f, 1000.f, 700.f};   // from the base ATV, the farthest first
constexpr int   kBearings = 8;
constexpr int   kKeyS = 20;            // the host's table holds the new ATV within this of its spawn

float Flat(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// Whether `p` is clear of the host's player and of every puppet this peer holds.
bool ClearOfPlayers(const FVector& p, const FVector& host) {
    if (Flat(p, host) < kClearCm) return false;
    for (uint8_t i = 0; i < coop::players::kMaxPeers; ++i) {
        coop::RemotePlayer* r = coop::players::Registry::Get().Puppet(i);
        FVector at{};
        if (r && r->valid() && r->actor() && E::TryGetActorLocation(r->actor(), at) && Flat(p, at) < kClearCm) return false;
    }
    return true;
}

// Game thread: the second ATV, born on the NavMesh at a bearing and distance from the base ATV that no player stands near.
bool SpawnApart(const std::wstring& baseKey, ue_wrap::CachedObjRef& out, std::string& why) {
    void* me = coop::players::Registry::Get().Local();
    coop::atv_sync::TableView tv;
    FVector mine{}, base{};
    if (!me || !E::TryGetActorLocation(me, mine) || !coop::atv_sync::ReadTable(baseKey, tv) ||
        !E::TryGetActorLocation(tv.actor, base)) {
        why = "the host's player or the base ATV does not read a place";
        return false;
    }
    for (const float ring : kRingsCm) {
        for (int d = 0; d < kBearings; ++d) {
            const float a = static_cast<float>(d) * 6.2831853f / static_cast<float>(kBearings);
            const FVector pt{base.X + ring * std::cos(a), base.Y + ring * std::sin(a), base.Z + SN::kLiftCm};
            if (!ClearOfPlayers(pt, mine)) continue;
            SN::Spawned sp;
            if (SN::SpawnAtNav(me, pt, sp)) {
                out.Set(sp.actor);
                return true;
            }
            if (!sp.noNav) {
                why = std::string(sp.what) + ": " + sp.got;
                return false;
            }
        }
    }
    why = "no NavMesh point at any of 8 bearings and 4 distances from the base ATV is clear of the players";
    return false;
}

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

bool UnadoptedOf(const DE::Rec& r, int32_t tag) { return r.i2 == tag; }
bool UnadoptedKept(const DE::Rec& r, int32_t tag) { return r.i2 == tag && r.i1 != 0; }

}  // namespace

// HOST, before the barrier: the second ATV spawned and published, its key and vitals said as a fact for the clients.
bool SpawnSecondAtv(Ctx& c, std::string& why) {
    ue_wrap::CachedObjRef atv;
    bool spawned = false;
    OnGt([&] { spawned = SpawnApart(c.key, atv, why); });
    if (!spawned) return false;
    std::wstring key;
    const char* bad = nullptr;
    SN::Key state = SN::Key::Wait;
    const bool keyed = WaitUntil("the second ATV in the host's table", kKeyS, [&] {
        OnGt([&] {
            void* a = atv.Get();
            if (!a) bad = "the second ATV is gone before it had a key";
            state = a ? SN::KeyOf(a, &key, &bad) : SN::Key::Bad;
        });
        return state != SN::Key::Wait;
    });
    if (!keyed) {
        why = "the second ATV did not enter the host's table";
        return false;
    }
    if (state == SN::Key::Bad) {
        why = bad ? bad : "the second ATV's key cannot be said";
        return false;
    }
    if (!SeedHost(key, kSeedFuel, kSeedHealth, why)) return false;
    const Vitals v = ReadVitals(key);
    if (std::fabs(v.fuel - kSeedFuel) > kSame) {
        why = "the second ATV's copy on the host does not hold the seeded fuel";
        return false;
    }
    UE_LOGI("atv-drill: step 10: the host's second ATV '%ls' is spawned and published at fuel %.1f health %.1f", key.c_str(), v.fuel,
            v.health);
    std::ostringstream f;
    f << Narrow(key) << ' ' << v.fuel << ' ' << v.health;
    Fact(c, f.str());
    return true;
}

// The step has no act: everything it does is its prelude.
bool ActNone(Ctx&, std::string&) { return true; }

// CLIENT: the state for the new ATV is kept at its arrival and applied at the mirror's adoption. The records are read from the
// first one: the state may have arrived before this peer read the step's barrier.
Result D10(Ctx& c) {
    if (c.me != kClient) return Pass("nothing to decide here");
    std::string t;
    if (!AwaitFact(c, kHost, t)) return Stopped();
    std::istringstream in(t);
    std::string keyText;
    float fuel = 0.f, health = 0.f;
    if (!(in >> keyText >> fuel >> health)) return Invalid("the host's fact does not read: '" + t + "'");
    const int32_t tag = DE::KeyTag(std::wstring(keyText.begin(), keyText.end()));
    c.cur[DE::kUnadopted] = 0;
    c.cur[DE::kAdopted] = 0;
    DE::Rec r;
    for (int i = 0; i < kEventS * 10; ++i) {
        if (Aborted()) return Stopped();
        while (DE::Next(DE::kUnadopted, c.cur[DE::kUnadopted], r))
            if (r.i2 == tag && r.i1 == 0) return Fail("the host's state for the ATV with no copy was dropped, not kept for its adoption");
        while (DE::Next(DE::kAdopted, c.cur[DE::kAdopted], r)) {
            if (r.i1 != tag) continue;
            if (!r.i0) {
                if (!DE::AnyBefore(DE::kUnadopted, 0, r.ord, &UnadoptedOf, tag))
                    return Invalid("the mirror was adopted before the host's state arrived");
                if (DE::AnyBefore(DE::kUnadopted, 0, r.ord, &UnadoptedKept, tag))
                    return Fail("the mirror was adopted and the state kept for it was not applied");
                return Fail("the host's state for the ATV with no copy was not kept");
            }
            if (std::fabs(r.f0 - fuel) > kSame || std::fabs(r.f1 - health) > kSame)
                return Fail("the mirror first showed fuel " + std::to_string(r.f0) + " health " + std::to_string(r.f1) + ", the host's " +
                            std::to_string(fuel) + " " + std::to_string(health));
            return Pass("the mirror was adopted with the state kept for it");
        }
        SleepStep();
    }
    End('I', "no decision event within " + std::to_string(kEventS) + " s");
    return Stopped();
}

}  // namespace coop::dev::atv_gate_drill::detail
