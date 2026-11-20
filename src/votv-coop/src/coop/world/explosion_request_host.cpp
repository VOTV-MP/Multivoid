// coop/world/explosion_request_host.cpp -- the host's judgement of a client's shot and blast, and the spawn of the
// blast: see coop/world/explosion_request.h. Each judge answers Done for an accepted or a refused message and Wait
// only where an honest sender's message is early (the hand lane still owes the slot's state, or the shot a blast
// belongs to is still in the slot's own Fired queue). Game thread.

#include "explosion_request_internal.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/atv_sync.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/hand_history.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/world/explosion_recipe.h"
#include "coop/world/explosion_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/explosion.h"
#include "ue_wrap/world/explosion_source.h"
#include "ue_wrap/world/game_mode.h"

#include <cmath>
#include <cstdint>
#include <cwchar>

namespace coop::explosion_request::detail {
namespace {

namespace RC = coop::explosion_recipe;
namespace XS = ue_wrap::explosion_source;
namespace X  = ue_wrap::explosion;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
using ue_wrap::FVector;
using coop::net::Served;

// The launcher must have been in the slot's hand within this window of the shot: an honest switch-then-shoot is
// judged against the hand lane's applied history, which trails the client's own hand by the lane's pace.
constexpr uint64_t kLauncherWindowMs = 2000;
// A grenade or pipe bomb is held to its fuse; this much more is allowed on top.
constexpr uint64_t kFuseSlackMs = 2000;
constexpr uint64_t kGrenadeFuseMs  = 3000;
constexpr uint64_t kPipebombFuseMs = 6000;

// How far from the shooter a shot's origin or a hand blast may be, before the speed travelled in one round trip.
constexpr float kShotOriginCm = 400.0f;
constexpr float kHandBlastCm  = 300.0f;

// A rocket's speed from each launcher that fires one, cm/s, and the slack on its reach.
constexpr float kRpgRocketSpeed  = 5000.0f;
constexpr float kWitchRocketSpeed = 10000.0f;
constexpr float kRocketReachSlackCm = 500.0f;
// The slack on a rocket's flight time: the shot's and the blast's receive times can drift apart (a stalled drain,
// the queue's pacing) though the lane delivers them in order.
constexpr float kRetransmitSlackMs = 1000.0f;
// A sticky bomb rides whatever it struck, so its point is unbounded but by the world.
constexpr float kStickyReachCm = 20000.0f;

// The round trip when the session has not measured one yet, and the most of it a bound is widened by. Source caps
// one-way latency plus interpolation at sv_maxunlag, 1 s (reference/source-sdk-2013/src/game/server/
// player_lagcompensation.cpp:34, :399); here the round trip is capped at the same second.
constexpr float kUnmeasuredRttMs = 500.0f;
constexpr float kMaxRttMs        = 1000.0f;

// The fastest a player and the witch car move, cm/s: a bound is widened by this times the round trip, never by the
// speed the host last read (a puppet's pose is set each frame and carries none). Source sizes its lag-compensation
// radius the same way, a max speed times the 1 s lag cap (reference/source-sdk-2013/src/game/server/player.cpp:765).
// The player's value is the engine's default terminal speed, so a falling shooter is covered, not a sprint times a
// factor. Both values are provisional, for the drill to measure.
constexpr float kMaxPlayerSpeedCmS = 4000.0f;
constexpr float kMaxCarSpeedCmS    = 3000.0f;

constexpr const wchar_t* kWitchCarClass = L"car1_witch_C";

float Dist(const FVector& a, const float b[3]) {
    const float dx = a.X - b[0], dy = a.Y - b[1], dz = a.Z - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float RttMs(uint8_t slot) {
    coop::net::Session* s = SessionPtr();
    const int rtt = s ? s->rttMsForSlot(slot) : -1;
    const float ms = rtt < 0 ? kUnmeasuredRttMs : static_cast<float>(rtt);
    return ms > kMaxRttMs ? kMaxRttMs : ms;
}

// The slot's puppet and its location, or null.
coop::RemotePlayer* PuppetOf(uint8_t slot, FVector& loc) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    if (!rp || !rp->valid() || !rp->GetActor() || !rp->TryGetLocation(loc)) return nullptr;
    return rp;
}

// The reach a body may travel in one round trip from where the host last saw it: its base, widened by its top speed.
float Slack(float maxSpeedCmS, uint8_t slot, float base) {
    return base + maxSpeedCmS * RttMs(slot) / 1000.0f;
}

const char* ProjName(uint8_t projClass) {
    return projClass == static_cast<uint8_t>(RC::ProjClass::Rocket) ? "rocket" : "sticky";
}

// Remove the entry at `i`, keeping the arrival order of the rest.
void EraseShot(Slot& s, size_t i) {
    for (size_t j = i + 1; j < s.count; ++j) s.shots[j - 1] = s.shots[j];
    --s.count;
}

// The oldest sticky entry, else the oldest.
size_t EvictionVictim(const Slot& s) {
    for (size_t i = 0; i < s.count; ++i)
        if (s.shots[i].projClass == static_cast<uint8_t>(RC::ProjClass::Sticky)) return i;
    return 0;
}

const char* KindName(RC::RequestKind kind) {
    switch (kind) {
        case RC::RequestKind::Projectile: return "projectile";
        case RC::RequestKind::Hand:       return "hand";
        case RC::RequestKind::Self:       return "self";
        case RC::RequestKind::Atv:        return "atv";
    }
    return "unknown";
}

// A judge's Wait for a hand state it is owed, counted: a head that waits again on the next tick counts again. The
// wait on a shot still queued is not counted.
Served Wait() {
    ++MutCounters().waited;
    return Served::Wait;
}

// A blast of one of the three judged families was spawned: the kind's accept count and the accept line.
void NoteAccepted(uint8_t slot, RC::RequestKind kind, XS::Kind src, const float loc[3]) {
    ++MutCounters().requestsAccepted[KindSlot(kind)];
    UE_LOGI("[EXPLOSION] request accepted kind=%s class=%ls slot=%u at (%.0f,%.0f,%.0f)", KindName(kind),
            XS::ClassName(src), static_cast<unsigned>(slot), loc[0], loc[1], loc[2]);
}

}  // namespace

// The blast itself: the host's own parameters for the source, spawned natively inside the lane's scope so the birth
// seam leaves it alone; the host's capture of its begin-play carries it to every peer. Refuses and counts only its
// own two failures; the caller counts and says the accept.
bool HostSpawn(uint8_t slot, XS::Kind src, const float loc[3], float level) {
    if (!X::ResolveNow(X::Cls::Explosion)) {
        Refuse(slot, Refusal::Unresolved);
        return false;
    }
    X::Params params{};
    float scale[3];
    RC::RecipeFor(src, level, params, scale);
    const float rot[3] = {0.0f, 0.0f, 0.0f};
    void* actor = nullptr;
    {
        coop::explosion_sync::LaneSpawnScope scope;
        actor = X::SpawnDeferred(X::Cls::Explosion, loc, rot, scale, params, nullptr);
    }
    if (!actor) {
        Refuse(slot, Refusal::SpawnFailed);
        return false;
    }
    return true;
}

Served JudgeShot(uint8_t slot, Slot& s, const FiredItem& it) {
    const coop::net::ProjectileFiredPayload& p = it.p;
    for (size_t i = 0; i < s.count; ++i)
        if (s.shots[i].shotId == p.shotId) {
            Refuse(slot, Refusal::DuplicateShot);
            return Served::Done;
        }

    // Not built: MTA relays the projectile to the other players whose camera is in range
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2953-3016). This lane does not relay: a relay
    // needs the projectile's velocity, rotation and target on the wire, and the shot carries the origin only, so the
    // other peers see only the blast.
    // MTA checks no weapon on its server (CGame.cpp:2831-2932); here the launcher must be in the shooter's hand or seat.
    void* car = nullptr;
    if (p.launcher == static_cast<uint8_t>(RC::Launcher::WitchCar)) {
        car = coop::atv_sync::ActorSeatedBy(slot);
        if (!car || ::_wcsicmp(R::ClassNameOf(car).c_str(), kWitchCarClass) != 0) {
            Refuse(slot, Refusal::NotSeated);
            return Served::Done;
        }
    } else {
        if (!coop::hand_history::HeldWithinMatching(slot, &coop::hand_item::ClassNameIsA, RC::LauncherClassName(p.launcher),
                                                   kLauncherWindowMs, it.recvMs)) {
            if (coop::hand_item::HandStateOwed(slot)) return Wait();
            static bool s_said = false;   // the first refusal names what the host knew of the hand
            if (!s_said) {
                s_said = true;
                UE_LOGW("[EXPLOSION] no_launcher slot=%u recv=%llu now=%llu held=%d history=%s", slot,
                        static_cast<unsigned long long>(it.recvMs), static_cast<unsigned long long>(coop::net::NowMs()),
                        coop::hand_item::HoldsItem(slot) ? 1 : 0, coop::hand_history::Describe(slot).c_str());
            }
            Refuse(slot, Refusal::NoLauncher);
            return Served::Done;
        }
        if (!ue_wrap::game_mode::FungunHoldAllowed()) {
            Refuse(slot, Refusal::FungunRule);
            return Served::Done;
        }
    }

    // The origin: a launcher fires from the player's hand or the car's mount.
    FVector anchor{};
    if (car) {
        if (!E::TryGetActorLocation(car, anchor)) {  // the anchor has no place to measure from
            Refuse(slot, Refusal::NoPuppet);
            return Served::Done;
        }
    } else if (!PuppetOf(slot, anchor)) {
        Refuse(slot, Refusal::NoPuppet);
        return Served::Done;
    }
    // Written so that a NaN fails.
    const float anchorSpeed = car ? kMaxCarSpeedCmS : kMaxPlayerSpeedCmS;
    if (!(Dist(anchor, p.origin) <= Slack(anchorSpeed, slot, kShotOriginCm))) {
        Refuse(slot, Refusal::OriginBound);
        return Served::Done;
    }

    if (s.count == kShotsMax) {
        EraseShot(s, EvictionVictim(s));
        ++MutCounters().evicted;
    }
    Shot& sh = s.shots[s.count++];
    sh.shotId    = p.shotId;
    sh.projClass = p.projClass;
    sh.launcher  = p.launcher;
    sh.origin[0] = p.origin[0];
    sh.origin[1] = p.origin[1];
    sh.origin[2] = p.origin[2];
    sh.recvMs    = it.recvMs;
    ++MutCounters().shotsAccepted;
    if (p.launcher < RC::kLauncherSlots) ++MutCounters().shotsAcceptedBy[p.launcher];  // LegalPair passed 1..3 only
    UE_LOGI("[EXPLOSION] shot accepted slot=%u id=%u class=%s launcher=%u", static_cast<unsigned>(slot),
            static_cast<unsigned>(p.shotId), ProjName(p.projClass), static_cast<unsigned>(p.launcher));
    return Served::Done;
}

Served JudgeShotBlast(uint8_t slot, Slot& s, const RequestItem& it) {
    const coop::net::ExplosionRequestPayload& p = it.p;
    const XS::Kind src = RC::KindOfSrcClass(p.srcClass);
    const uint8_t wanted = static_cast<uint8_t>(src == XS::Kind::Rocket ? RC::ProjClass::Rocket : RC::ProjClass::Sticky);
    size_t at = s.count;
    for (size_t i = 0; i < s.count; ++i)
        if (s.shots[i].shotId == p.shotId && s.shots[i].projClass == wanted) {
            at = i;
            break;
        }
    if (at == s.count) {
        // Its shot may still be queued behind the shots paced before it, but only while the slot's Fired queue holds any.
        if (!s.fired.empty()) return Served::Wait;
        Refuse(slot, Refusal::UnknownShot);
        return Served::Done;
    }

    // MTA sends a rocket's blast relative to the closest remote player and the server re-bases it on that player's
    // server position, which compensates lag. This lane compensates nothing: the point is absolute, and the host
    // bounds it by the shot's origin. reference/mtasa-blue/Client/mods/deathmatch/logic/CClientExplosionManager.cpp:159-185
    // (the origin choice), reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2843-2867 (the re-base).
    const Shot sh = s.shots[at];
    EraseShot(s, at);  // the shot's one blast is spent, accepted or not
    float reach = kStickyReachCm;
    if (wanted == static_cast<uint8_t>(RC::ProjClass::Rocket)) {
        const float speed = sh.launcher == static_cast<uint8_t>(RC::Launcher::WitchCar) ? kWitchRocketSpeed
                                                                                        : kRpgRocketSpeed;
        const float flightMs = it.recvMs > sh.recvMs ? static_cast<float>(it.recvMs - sh.recvMs) : 0.0f;
        reach = speed * (flightMs + RttMs(slot) + kRetransmitSlackMs) / 1000.0f + kRocketReachSlackCm;
    }
    FVector origin{sh.origin[0], sh.origin[1], sh.origin[2]};
    if (!(Dist(origin, p.loc) <= reach)) {
        Refuse(slot, Refusal::Reach);
        return Served::Done;
    }
    if (HostSpawn(slot, src, p.loc, 0.0f)) NoteAccepted(slot, RC::RequestKind::Projectile, src, p.loc);
    return Served::Done;
}

Served JudgeBody(uint8_t slot, Slot&, const RequestItem& it) {
    const coop::net::ExplosionRequestPayload& p = it.p;
    const XS::Kind src = RC::KindOfSrcClass(p.srcClass);
    FVector at{};
    coop::RemotePlayer* rp = nullptr;

    // MTA marks no player's explosion once and checks no weapon (CGame.cpp:2831-2932); here a hand blast needs its
    // explosive in the hand within its fuse, and a self blast needs a living puppet and the bucket's pace.
    if (p.kind == static_cast<uint8_t>(RC::RequestKind::Hand)) {
        const uint64_t fuse = src == XS::Kind::Pipebomb ? kPipebombFuseMs : kGrenadeFuseMs;
        if (!coop::hand_history::HeldWithin(slot, XS::ClassName(src), fuse + kFuseSlackMs, it.recvMs)) {
            if (coop::hand_item::HandStateOwed(slot)) return Wait();
            Refuse(slot, Refusal::NoExplosive);
            return Served::Done;
        }
        rp = PuppetOf(slot, at);
        if (!rp) {
            Refuse(slot, Refusal::NoPuppet);
            return Served::Done;
        }
        if (!(Dist(at, p.loc) <= Slack(kMaxPlayerSpeedCmS, slot, kHandBlastCm))) {
            Refuse(slot, Refusal::HandBound);
            return Served::Done;
        }
        if (HostSpawn(slot, src, p.loc, 0.0f)) NoteAccepted(slot, RC::RequestKind::Hand, src, p.loc);
        return Served::Done;
    }
    if (p.kind == static_cast<uint8_t>(RC::RequestKind::Self)) {
        rp = PuppetOf(slot, at);
        if (!rp) {
            Refuse(slot, Refusal::NoPuppet);
            return Served::Done;
        }
        // The same alive test as the hit judge (Registry::LifeOf).
        if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
            Refuse(slot, Refusal::PuppetDead);
            return Served::Done;
        }
        const float loc[3] = {at.X, at.Y, at.Z};
        if (HostSpawn(slot, src, loc, RC::ClampLevel(p.pillLevel)))
            NoteAccepted(slot, RC::RequestKind::Self, src, loc);
        return Served::Done;
    }
    Refuse(slot, Refusal::Malformed);  // the receive edge queues only these two kinds here
    return Served::Done;
}

}  // namespace coop::explosion_request::detail
