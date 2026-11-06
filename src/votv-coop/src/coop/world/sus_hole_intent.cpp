// coop/world/sus_hole_intent.cpp -- see coop/world/sus_hole_intent.h.

#include "coop/world/sus_hole_intent.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace coop::sus_hole_intent {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace R = ue_wrap::reflection;
namespace UH = ue_wrap::ufunction_hook;
using Payload = coop::net::SusHoleIntentPayload;

// The spawner makes its hole with BeginDeferredActorSpawnFromClass and FinishSpawningActor, both called from its own graph.
UE_FACT_FN(kRowFinishSpawningActor, "ambient_fixture_sync", L"GameplayStatics", L"FinishSpawningActor", Declared);

// The spawner's ring around its camera (100 to 500 m), the trace's own height window (10 m either way of the rolled point)
// and the host's tolerance for where its trace meets the ground against where the client's did.
constexpr float kRingNearUU = 10000.f;
constexpr float kRingFarUU = 50000.f;
constexpr float kGrassToleranceUU = 200.f;
// A player's queue, and the holes the host made for it that may stand. The spawner's own pace is one roll in 300 to 600 s.
constexpr size_t kMaxPending = 4;
constexpr size_t kMaxStanding = 4;
constexpr coop::net::IntentBudget kBudget{2.0f, 1.0f / 300.0f};
constexpr uint64_t kRetryMs = 1000;
constexpr int kMaxTries = 30;

// A class by name, held by its pointer: a pointer compare per spawn, and the listing identity says when it went stale. A
// class still null is asked again when the index's class set has moved since the last ask, and once per lane tick besides:
// a class answers null while it loads and its finishing moves no number (object_index.h:55-58). Never per spawn. The
// residual: a hole spawned in the very tick its class finished loading is missed; the hole class loads with its
// hard-referencing spawner.
struct ClassCache {
    const wchar_t* name;
    void*          cls = nullptr;
    uint64_t       listing = 0;
    uint64_t       setAsked = ~uint64_t{0};
    uint64_t       tickAsked = ~uint64_t{0};
};
uint64_t g_laneTick = 0;  // counted by Tick
ClassCache g_holeClass{L"susDirtHole_C"};
ClassCache g_spawnerClass{L"ticker_susHoleSpawner_C"};

bool IsOfClass(ClassCache& c, void* cls) {
    if (!cls) return false;
    if (c.cls && OI::ClassListingId(c.cls) != c.listing) {
        c.cls = nullptr;
        c.setAsked = ~uint64_t{0};
    }
    if (!c.cls) {
        const uint64_t set = OI::ClassSetVersion();
        if (set != c.setAsked || g_laneTick != c.tickAsked) {
            c.setAsked = set;
            c.tickAsked = g_laneTick;
            c.cls = OI::ClassByName(c.name);
            c.listing = c.cls ? OI::ClassListingId(c.cls) : 0;
        }
    }
    return c.cls == cls;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool Finite3(float a, float b, float c, float bound) {
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::fabs(a) <= bound && std::fabs(b) <= bound &&
           std::fabs(c) <= bound;
}

// The totals the drill reads.
Counters g_counters;
uint32_t g_world = 0;

// ---- CLIENT ----

// The holes the hook stamped, waiting for the tick: stamped inside FinishSpawningActor, which only reads and stamps.
std::vector<ue_wrap::CachedObjRef> g_stamped;
// Intents read from a destroyed hole, waiting to go out.
std::vector<Payload> g_outbox;

bool g_hooked = false;
bool g_armed = false;       // the hook calls back only while a client session runs: it sits on a native every Blueprint spawn calls
void* g_hookFn = nullptr;
bool g_gaveUp = false;
int g_tries = 0;
uint64_t g_nextTryMs = 0;

// Inside FinishSpawningActor: a compare of the spawned class, then of the calling actor's, and a stamp.
void OnFinishSpawnPost(void* /*context*/, void* source, void* spawned) {
    if (!spawned || !coop::spawn_authority::IsActiveClientSession()) return;
    if (!IsOfClass(g_holeClass, R::ClassOf(spawned))) return;
    if (!source || !IsOfClass(g_spawnerClass, R::ClassOf(source))) return;
    ue_wrap::CachedObjRef hole;
    hole.Set(spawned);
    g_stamped.push_back(hole);
    ++g_counters.stamped;
}

void ClientTick(coop::net::Session& s) {
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_world) {  // a hole of the world the join's load discarded is gone with it
        g_world = world;
        g_stamped.clear();
        g_outbox.clear();
    }
    if (g_stamped.empty() && g_outbox.empty()) return;
    if (!coop::net_pump::HasAnnouncedWorldReady() || coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle ||
        !s.connected())
        return;
    for (ue_wrap::CachedObjRef& ref : g_stamped) {
        void* hole = ref.Get();
        if (!hole) continue;
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(hole, at)) continue;
        AC::Destroy(hole);  // the host makes the one set; this player's roll is only its word
        g_outbox.push_back(Payload{at.X, at.Y, at.Z});
        UE_LOGI("[SUS-HOLE] this player's spawner rolled a hole at (%.0f %.0f %.0f) -- the host makes it", at.X, at.Y, at.Z);
    }
    g_stamped.clear();
    // The drill's red arm: the hole is destroyed as ever and its word is never sent, so the host makes nothing.
    static const bool s_redNoIntent = coop::config::ResolveEnum(coop::config_registry::rows::sus_hole_red) == "no_intent";
    for (auto it = g_outbox.begin(); it != g_outbox.end();) {
        if (!s_redNoIntent && !s.SendReliable(coop::net::ReliableKind::SusHoleIntent, &*it, sizeof(*it))) break;
        if (!s_redNoIntent) {
            ++g_counters.sent;
            g_counters.sentX = it->x;
            g_counters.sentY = it->y;
            g_counters.sentZ = it->z;
        }
        it = g_outbox.erase(it);
    }
}

// ---- HOST ----

struct Standing {
    void*   obj = nullptr;
    int32_t index = -1;
};

// One client: its queued intents with their bucket, the holes made for it that still stand, and what was said of it. All of
// it clears with the occupant.
struct Slot {
    coop::net::IntentQueue<Payload> queue;
    std::vector<Standing> standing;
    uint32_t said = 0;     // a bit per Reason, said once for this occupant
    bool waitSaid = false;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

void Refuse(Reason why, int slot, const Payload& p, const char* more) {
    ++g_counters.refused[why];
    if (slot < 1 || slot >= coop::players::kMaxPeers) return;
    Slot& sl = g_slots[slot];
    if (sl.said & (1u << why)) return;
    sl.said |= 1u << why;
    UE_LOGI("[SUS-HOLE] DENY slot=%d at (%.0f %.0f %.0f) -- %s%s%s (said once for this player)", slot, p.x, p.y, p.z, ReasonName(why),
            more ? ": " : "", more ? more : "");
}

// False keeps the intent at its sender's queue head: the host has no body for the sender yet, so the ring cannot be measured,
// and the intent waits for it. Everything else is made or refused, and consumed.
bool Execute(coop::net::Session& s, const Payload& p, int slot) {
    Slot& sl = g_slots[slot];
    const ue_wrap::FVector named{p.x, p.y, p.z};
    // The point's distance from the body: the outer bound and the pose pad are the authority's, over the degenerate segment
    // (start == end) that a single point is; the inner bound is the measured distance.
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, static_cast<uint8_t>(slot), kRingFarUU).AuthorizeSegment(named, named);
    if (subject.outcome == coop::element::IntentOutcome::NoBody) {
        if (!sl.waitSaid) {
            sl.waitSaid = true;
            UE_LOGI("[SUS-HOLE] slot %d's intents wait: the host has no body for it yet", slot);
        }
        return false;
    }
    sl.waitSaid = false;
    if (!subject) {
        Refuse(kTooFar, slot, p, coop::element::OutcomeName(subject.outcome));
        return true;
    }
    // The drill's red arm: the ring's inner bound is not judged, so a point beside its player goes on to the trace.
    static const bool s_redNearAccepted =
        coop::config::ResolveEnum(coop::config_registry::rows::sus_hole_red) == "out_of_ring_accepted";
    if (subject.distUU < kRingNearUU && !s_redNearAccepted) {
        Refuse(kTooNear, slot, p, nullptr);
        return true;
    }
    if (sl.standing.size() >= kMaxStanding) {
        Refuse(kStanding, slot, p, nullptr);
        return true;
    }
    // The spawner's own test of a spot, run here on the named point: grass, at the height the client named.
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!AC::HoleSpot(named, at, rot) || std::fabs(at.Z - p.z) > kGrassToleranceUU) {
        Refuse(kNoGrass, slot, p, nullptr);
        return true;
    }
    void* hole = AC::SpawnFixture(false, at, rot);
    if (!hole) {
        Refuse(kNotMade, slot, p, nullptr);
        return true;
    }
    sl.standing.push_back(Standing{hole, R::InternalIndexOf(hole)});
    ++g_counters.accepted;
    UE_LOGI("[SUS-HOLE] slot %d's hole made at (%.0f %.0f %.0f), %.0f m from its player (%zu stand for it)", slot, at.X, at.Y, at.Z,
            subject.distUU / 100.f, sl.standing.size());
    return true;
}

void HostTick(coop::net::Session& s) {
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        g_slots[slot].queue.Serve(kBudget, NowMs, [&](const Payload& p) {
            return Execute(s, p, slot) ? coop::net::Served::Done : coop::net::Served::Wait;
        });
    }
}

}  // namespace

const char* ReasonName(Reason why) {
    switch (why) {
        case kMalformed: return "the place is not a finite point in the world";
        case kQueueFull: return "the sender's queue is full";
        case kTooNear: return "the point is nearer than the spawner's ring";
        case kTooFar: return "the point is beyond the spawner's ring";
        case kStanding: return "the sender's holes already stand at the cap";
        case kNoGrass: return "the host's trace finds no grass there";
        case kNotMade: return "the hole did not spawn";
        case kReasonCount: break;
    }
    return "?";
}

void Install(coop::net::Session* session) {
    if (g_gaveUp || !session || session->role() != coop::net::Role::Client) return;
    if (g_hooked) {
        if (!g_armed) g_armed = UH::SetArmed(g_hookFn, &OnFinishSpawnPost, true);
        return;
    }
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return;
    g_nextTryMs = now + kRetryMs;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowFinishSpawningActor);
    if (fn && UH::InstallPostHook(fn, &OnFinishSpawnPost, /*armed=*/false)) {
        g_hooked = true;
        g_hookFn = fn.Fn();
        g_armed = UH::SetArmed(g_hookFn, &OnFinishSpawnPost, true);
        UE_LOGI("[SUS-HOLE] watching FinishSpawningActor for the sus hole spawner's holes (a client's roll goes to the host)");
    } else if (++g_tries >= kMaxTries) {
        g_gaveUp = true;
        UE_LOGE("[SUS-HOLE] FinishSpawningActor did not resolve or would not take a post hook in %d tries; a client's sus holes "
                "stay its own", kMaxTries);
    }
}

void Tick(coop::net::Session& session) {
    ++g_laneTick;
    if (!session.running()) return;
    if (session.role() == coop::net::Role::Host) HostTick(session);
    else if (session.role() == coop::net::Role::Client) ClientTick(session);
}

void OnIntent(coop::net::Session& session, int senderSlot, const Payload& p) {
    if (session.role() != coop::net::Role::Host) return;
    ++g_counters.received;
    if (senderSlot < 1 || senderSlot >= coop::players::kMaxPeers || !Finite3(p.x, p.y, p.z, coop::net::kMaxCoord)) {
        Refuse(kMalformed, senderSlot, p, nullptr);
        return;
    }
    Payload copy = p;
    if (!g_slots[senderSlot].queue.Push(std::move(copy), kMaxPending)) Refuse(kQueueFull, senderSlot, p, nullptr);
}

void OnHoleEnded(void* obj, int32_t index) {
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        std::vector<Standing>& v = g_slots[slot].standing;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (it->obj != obj || it->index != index) continue;
            v.erase(it);
            break;
        }
    }
}

void OnDisconnect() {
    uint64_t refused = 0;
    for (const uint64_t n : g_counters.refused) refused += n;
    if (g_counters.stamped || g_counters.sent || g_counters.received || g_counters.accepted || refused)
        UE_LOGI("[SUS-HOLE] session end -- stamped=%llu sent=%llu received=%llu made=%llu refused=%llu",
                static_cast<unsigned long long>(g_counters.stamped), static_cast<unsigned long long>(g_counters.sent),
                static_cast<unsigned long long>(g_counters.received), static_cast<unsigned long long>(g_counters.accepted),
                static_cast<unsigned long long>(refused));
    g_counters = Counters{};
    if (g_armed && UH::SetArmed(g_hookFn, &OnFinishSpawnPost, false)) g_armed = false;
    g_stamped.clear();
    g_outbox.clear();
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) g_slots[slot] = Slot{};
}

Counters Read() { return g_counters; }

}  // namespace coop::sus_hole_intent
