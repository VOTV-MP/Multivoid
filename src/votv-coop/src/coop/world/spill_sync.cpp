// coop/world/spill_sync.cpp -- see coop/world/spill_sync.h.

#include "coop/world/spill_sync.h"

#include "coop/interactables/grime_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_sound.h"
#include "coop/props/remote_prop.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/grime_spill.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::spill_sync {
namespace {

namespace E = ue_wrap::engine;
namespace GS = ue_wrap::grime_spill;
namespace R = ue_wrap::reflection;
using Payload = coop::net::SpillPayload;

UE_FACT_FN(kRowFinishSpawningActor, "spill_sync", L"GameplayStatics", L"FinishSpawningActor", Declared);

constexpr float kReachCm = 1500.f;       // a client's spill from its body: a cup tipped or thrown near it
constexpr float kSameSpotCm = 0.001f;    // a decal of the class this near is the one a state names: a copy, and the save's
                                         // own, lie at the host's floats, and a bottle's pours land a millimetre apart
constexpr size_t kCap = 64;
constexpr size_t kMaxSpills = 512;       // the host's spills kept for a joiner
constexpr float kGone = -1.f;            // a payload's process: the spill was wiped away (state) or taken (intent)

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_live{false}, g_host{false};
ue_wrap::facts::Resolved g_finish;
bool g_hooked = false;
uint32_t g_world = 0;

// Filled inside the FinishSpawningActor callback, which only reads and stamps; drained on the next tick.
struct Flight {
    ue_wrap::CachedObjRef projectile;
    bool held = false;   // CLIENT: the liquid that spilled it was in this player's hand
};
std::vector<Flight> g_flights;
// HOST: a projectile born of a prop another player's hold drives here. That player's own copy spills it and sends its
// intent, so the host's copy of the same spill is not the host's to send.
struct ForeignFlight {
    ue_wrap::CachedObjRef projectile;
    int slot = 0;
    std::wstring cls;   // the held prop's class, for the removal's line
};
std::vector<ForeignFlight> g_foreign;
// A bottle's pour, seen as its projectile is born: the bottle, and the projectile for where it was if the bottle ended.
struct Pour {
    ue_wrap::CachedObjRef bottle, projectile;
    bool held = false;
};
std::vector<Pour> g_pours;
std::vector<coop::net::SpillPourPayload> g_pourIntents;   // HOST: clients' pours to play and relay
float g_pourSize = -1.f;    // grimeProjectile_C's default size: a break leaves it, a pour writes 1
struct Landed {
    ue_wrap::CachedObjRef decal;
    bool held = false;
    int foreignSlot = 0;     // HOST: > 0 when the projectile was a foreign flight: the slot whose hold drove its prop
    std::wstring foreignCls;
};
std::vector<Landed> g_landed;

// HOST: every spill it sent, for its end and a joiner. CLIENT: every host spill made here, for this player's wipe.
struct Sent {
    ue_wrap::CachedObjRef decal;
    Payload p{};
};
std::vector<Sent> g_spills;
std::vector<Sent> g_copies;
struct Intent {
    int slot = 0;
    Payload p{};
};
std::vector<Intent> g_intents;
// CLIENT: the host's spills waiting to be made, and our own spills waiting to be sent.
std::vector<Payload> g_inbox, g_outbox;

uint32_t g_sent = 0, g_made = 0, g_refused = 0, g_removed = 0;
std::atomic<uint32_t> g_splashes{0}, g_poursHeard{0}, g_poursSent{0};

// A host is live while its session runs, with or without a client: the spills it records are its world's, and a
// client that joins later is sent them (measured: a spill made while the only client was away never reached it).
coop::net::Session* Live() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s) return nullptr;
    if (s->role() == coop::net::Role::Host) return s->running() ? s : nullptr;
    return s->connected() ? s : nullptr;
}

bool Finite3(const float* v, float bound) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > bound) return false;
    return true;
}

bool Valid(const Payload& p) {
    if (!Finite3(p.loc, coop::net::kMaxCoord) || !Finite3(p.rot, 1000.f) || !Finite3(p.decalRot, 1000.f) ||
        !Finite3(p.scale, 20.f) || !std::isfinite(p.process) || (p.process < 0.f && p.process != kGone) ||
        p.process > 10000.f)
        return false;
    for (float s : p.scale)
        if (s < 0.01f) return false;
    return p.cls.len > 0 && p.cls.len < sizeof(p.cls.data) && p.splash <= 1;
}

Payload ToWire(const GS::Spill& s) {
    Payload p{};
    coop::net::WireKeyFromString(s.className, p.cls);
    p.loc[0] = s.location.X; p.loc[1] = s.location.Y; p.loc[2] = s.location.Z;
    p.rot[0] = s.rotation.Pitch; p.rot[1] = s.rotation.Yaw; p.rot[2] = s.rotation.Roll;
    p.scale[0] = s.scale.X; p.scale[1] = s.scale.Y; p.scale[2] = s.scale.Z;
    p.decalRot[0] = s.decalRotation.Pitch; p.decalRot[1] = s.decalRotation.Yaw; p.decalRot[2] = s.decalRotation.Roll;
    p.process = s.process;
    p.type = s.type;
    return p;
}

GS::Spill FromWire(const Payload& p) {
    GS::Spill s;
    s.className = coop::net::StringFromWireKey(p.cls);
    s.location = {p.loc[0], p.loc[1], p.loc[2]};
    s.rotation = {p.rot[0], p.rot[1], p.rot[2]};
    s.scale = {p.scale[0], p.scale[1], p.scale[2]};
    s.decalRotation = {p.decalRot[0], p.decalRot[1], p.decalRot[2]};
    s.process = p.process;
    s.type = p.type;
    return s;
}

// The grab state's two hands: a liquid in either is this player's.
bool HeldHere(void* actor) {
    void* me = coop::players::Registry::Get().Local();
    E::MainPlayerGrabState gs{};
    return actor && me && E::ReadMainPlayerGrabState(me, gs) && (gs.holdingActor == actor || gs.grabbingActor == actor);
}

// The slot of another player whose hold this copy is: a prop its hold drives through the pose stream, or the
// mirror of the item in its hand (measured: a client's cup in hand spilled on the host through the hand mirror, which
// no pose drive names). 0 when no other player holds it. Game thread, reads only.
int RemoteHolderOf(void* source) {
    if (!source) return 0;
    const int driving = coop::remote_prop::DrivingSlotOf(source);
    if (driving > 0) return driving;
    for (uint8_t s = 1; s < coop::net::kMaxPeers; ++s)
        if (coop::hand_item::MirrorActorForSlot(s) == source) return s;
    return 0;
}

// Inside FinishSpawningActor: reads and stamps only, no engine call.
void OnFinishSpawnPost(void* /*context*/, void* source, void* spawned) {
    if (!g_live.load(std::memory_order_relaxed) || !spawned) return;
    if (GS::IsProjectile(spawned)) {
        float size = 0.f;
        // A bottle's pour plays its sound where it pours; its break, which leaves the default size, plays none.
        if (GS::IsBottle(source) && g_pours.size() < kCap && GS::ReadProjectileSize(spawned, size) &&
            (g_pourSize == 1.f || size == 1.f)) {
            Pour p;
            p.bottle.Set(source);
            p.projectile.Set(spawned);
            p.held = HeldHere(source);
            g_pours.push_back(p);
        }
        if (g_host.load(std::memory_order_relaxed)) {
            // The holder authors this spill through its intent; the host's copy only flew because the hold turned it over.
            const int slot = RemoteHolderOf(source);
            if (slot > 0 && g_foreign.size() < kCap) {
                ForeignFlight ff;
                ff.projectile.Set(spawned);
                ff.slot = slot;
                ff.cls = R::ClassNameOf(source);
                g_foreign.push_back(std::move(ff));
            }
            return;
        }
        if (g_flights.size() >= kCap) return;
        Flight f;
        f.projectile.Set(spawned);
        f.held = HeldHere(source);
        g_flights.push_back(f);
        return;
    }
    if (!source || !GS::IsProjectile(source) || !GS::IsDecal(spawned) || g_landed.size() >= kCap) return;
    Landed l;
    l.decal.Set(spawned);
    for (const Flight& f : g_flights)
        if (f.projectile.Is(source)) l.held = f.held;
    for (const ForeignFlight& f : g_foreign)
        if (f.projectile.Is(source)) {
            l.foreignSlot = f.slot;
            l.foreignCls = f.cls;
        }
    g_landed.push_back(l);
}

// A decal that was never this world's own: gone, and its going is no wipe of this peer's.
void RemoveOwn(void* decal) {
    coop::grime_sync::ForgetDecal(decal);
    E::DestroyActor(decal);
    ++g_removed;
}

void* Body(int slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
    return body && R::IsLive(body) ? body : nullptr;
}

void Broadcast(coop::net::Session& s, void* decal, const Payload& p) {
    // The record first: it is the host's world, replayed to every joiner; the send is only its delivery to the
    // clients connected now, and with none connected it fails.
    if (g_spills.size() >= kMaxSpills) g_spills.erase(g_spills.begin());
    Sent row;
    row.decal.Set(decal);
    row.p = p;
    g_spills.push_back(row);
    if (s.SendReliable(coop::net::ReliableKind::SpillState, &p, sizeof(p))) ++g_sent;
    UE_LOGI("spill: a %ls landed at (%.0f %.0f %.0f)", coop::net::StringFromWireKey(p.cls).c_str(), p.loc[0], p.loc[1],
            p.loc[2]);
}

// Where a pour was heard: its bottle, or the projectile it poured if the bottle has ended since.
bool PourAt(const Pour& p, ue_wrap::FVector& at) {
    void* b = p.bottle.Get();
    if (b && E::TryGetActorLocation(b, at)) return true;
    void* pr = p.projectile.Get();
    return pr && E::TryGetActorLocation(pr, at);
}

coop::net::SpillPourPayload PourWire(const ue_wrap::FVector& at, uint8_t origin) {
    coop::net::SpillPourPayload w{};
    w.loc[0] = at.X; w.loc[1] = at.Y; w.loc[2] = at.Z;
    w.cue = 1;
    w.origin = origin;
    return w;
}

void HostTick(coop::net::Session& s) {
    std::vector<Pour> pours;
    pours.swap(g_pours);
    for (const Pour& p : pours) {
        ue_wrap::FVector at{};
        if (!PourAt(p, at)) continue;
        const auto w = PourWire(at, 0);   // the host poured it and heard it
        if (s.SendReliable(coop::net::ReliableKind::SpillPour, &w, sizeof(w))) ++g_poursSent;
    }
    std::vector<coop::net::SpillPourPayload> relay;
    relay.swap(g_pourIntents);
    for (const auto& w : relay) {
        if (void* me = coop::players::Registry::Get().Local()) {
            coop::prop_sound::PlayBottlePourAt(me, {w.loc[0], w.loc[1], w.loc[2]});
            ++g_poursHeard;
        }
        if (s.SendReliable(coop::net::ReliableKind::SpillPour, &w, sizeof(w))) ++g_poursSent;
    }
    std::vector<Landed> landed;
    landed.swap(g_landed);
    for (const Landed& l : landed) {
        if (l.foreignSlot > 0) {   // the host's copy of a prop another player holds: that player's intent makes the spill
            if (void* d = l.decal.Get()) {
                RemoveOwn(d);
                UE_LOGI("spill: the host's copy of slot %d's held %ls spilled -- removed, the holder's intent makes it",
                        l.foreignSlot, l.foreignCls.c_str());
            }
            continue;
        }
        GS::Spill sp;
        if (void* d = l.decal.Get(); d && GS::ReadSpill(d, sp)) {
            Payload p = ToWire(sp);
            p.splash = 1;
            p.origin = 0;   // the host's own flight: it heard its splash
            Broadcast(s, d, p);
        }
    }
    std::vector<Intent> intents;
    intents.swap(g_intents);
    for (const Intent& in : intents) {
        void* body = Body(in.slot);
        ue_wrap::FVector by{};
        if (!s.IsSlotWorldReady(in.slot) || !body || !E::TryGetActorLocation(body, by)) continue;
        const float dx = in.p.loc[0] - by.X, dy = in.p.loc[1] - by.Y, dz = in.p.loc[2] - by.Z;
        const GS::Spill sp = FromWire(in.p);
        if (in.p.process == kGone) {   // the client wiped the host's spill away: the host's goes, and its end says so
            void* d = GS::FindSpill(sp.className, sp.location, kSameSpotCm);
            if (d && dx * dx + dy * dy + dz * dz <= kReachCm * kReachCm) {
                E::DestroyActor(d);
                UE_LOGI("spill: slot %d wiped a %ls away -- gone on the host", in.slot, sp.className.c_str());
            }
            continue;
        }
        if (dx * dx + dy * dy + dz * dz > kReachCm * kReachCm || !GS::IsDecalClass(ue_wrap::object_index::ClassByName(sp.className.c_str()))) {
            ++g_refused;
            UE_LOGW("spill: slot %d's %ls refused (class or reach)", in.slot, sp.className.c_str());
            continue;
        }
        void* d = GS::SpawnSpill(sp);
        if (!d) continue;
        ++g_made;
        UE_LOGI("spill: slot %d spilled a %ls -- made on the host", in.slot, sp.className.c_str());
        coop::prop_sound::PlaySpillSplashAt(d, sp.location);   // the client's flight splashed there alone
        ++g_splashes;
        GS::Spill made;
        Payload out = GS::ReadSpill(d, made) ? ToWire(made) : in.p;
        out.splash = 1;
        out.origin = static_cast<uint8_t>(in.slot);
        Broadcast(s, d, out);
    }
    g_flights.clear();   // the host flies its own: nothing to remember of them
    for (auto it = g_foreign.begin(); it != g_foreign.end();) it = it->projectile.Get() ? std::next(it) : g_foreign.erase(it);
    for (auto it = g_spills.begin(); it != g_spills.end();) {
        if (it->decal.Get()) {
            ++it;
            continue;
        }
        Payload gone = it->p;
        gone.process = kGone;
        gone.splash = 0;
        if (!s.SendReliable(coop::net::ReliableKind::SpillState, &gone, sizeof(gone))) break;
        UE_LOGI("spill: a %ls was wiped away", coop::net::StringFromWireKey(gone.cls).c_str());
        it = g_spills.erase(it);
    }
}

void ClientTick(coop::net::Session& s) {
    std::vector<Landed> landed;
    landed.swap(g_landed);
    for (const Landed& l : landed) {
        void* d = l.decal.Get();
        if (!d) continue;
        GS::Spill sp;
        if (l.held && GS::ReadSpill(d, sp) && g_outbox.size() < kCap) {
            g_outbox.push_back(ToWire(sp));
            UE_LOGI("spill: this player spilled a %ls -- the host makes it", sp.className.c_str());
        }
        RemoveOwn(d);
    }
    for (auto it = g_flights.begin(); it != g_flights.end();) it = it->projectile.Get() ? std::next(it) : g_flights.erase(it);
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;
    for (auto it = g_copies.begin(); it != g_copies.end();) {
        if (it->decal.Get()) {
            ++it;
            continue;
        }
        Payload wiped = it->p;   // this player wiped the host's spill away here
        wiped.process = kGone;
        wiped.splash = 0;
        if (g_outbox.size() < kCap) g_outbox.push_back(wiped);
        UE_LOGI("spill: this player wiped a %ls away -- the host wipes its own", coop::net::StringFromWireKey(wiped.cls).c_str());
        it = g_copies.erase(it);
    }
    std::vector<Pour> pours;
    pours.swap(g_pours);
    for (const Pour& p : pours) {   // a copy's own pour is parked away; one in this player's hand goes to the host
        ue_wrap::FVector at{};
        if (!p.held || !PourAt(p, at)) continue;
        const auto w = PourWire(at, 0);
        if (s.SendReliable(coop::net::ReliableKind::SpillPourIntent, &w, sizeof(w))) ++g_poursSent;
    }
    for (auto it = g_outbox.begin(); it != g_outbox.end();) {
        if (!s.SendReliable(coop::net::ReliableKind::SpillIntent, &*it, sizeof(*it))) break;
        it = g_outbox.erase(it);
    }
    std::vector<Payload> inbox;
    inbox.swap(g_inbox);
    for (const Payload& p : inbox) {
        const GS::Spill sp = FromWire(p);
        void* here = GS::FindSpill(sp.className, sp.location, kSameSpotCm);
        if (p.process == kGone) {
            for (auto it = g_copies.begin(); it != g_copies.end();) it = it->decal.Is(here) ? g_copies.erase(it) : std::next(it);
            if (here) {
                coop::grime_sync::ForgetDecal(here);   // the host's end, not a wipe of this peer's
                E::DestroyActor(here);
                UE_LOGI("spill: the host's %ls was wiped away here", sp.className.c_str());
            }
            continue;
        }
        if (here && !p.splash) continue;   // a replay of a spill the save already gave this world
        if (void* d = GS::SpawnSpill(sp)) {
            Sent copy;
            copy.decal.Set(d);
            copy.p = p;
            g_copies.push_back(copy);
            ++g_made;
            if (p.splash && p.origin != coop::players::Registry::Get().LocalPeerId()) {
                coop::prop_sound::PlaySpillSplashAt(d, sp.location);
                ++g_splashes;
            }
            UE_LOGI("spill: the host's %ls made here at (%.0f %.0f %.0f)", sp.className.c_str(), p.loc[0], p.loc[1],
                    p.loc[2]);
        }
    }
}

void Reset() {
    g_pours.clear();
    g_pourIntents.clear();
    g_flights.clear();
    g_foreign.clear();
    g_landed.clear();
    g_spills.clear();
    g_copies.clear();
    g_intents.clear();
    g_inbox.clear();
    g_outbox.clear();
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    coop::net::Session* s = Live();
    g_live.store(s != nullptr, std::memory_order_relaxed);
    if (!s) return;
    g_host.store(s->role() == coop::net::Role::Host, std::memory_order_relaxed);
    if (!GS::EnsureResolved()) return;
    if (!g_hooked) {
        static uint32_t sTry = 0;
        if (sTry++ % 125u != 0u) return;   // GameplayStatics resolves by a walk of the whole array
        if (!g_finish) g_finish = ue_wrap::facts::Resolve(kRowFinishSpawningActor);
        if (!g_finish || !ue_wrap::ufunction_hook::InstallPostHook(g_finish, &OnFinishSpawnPost)) return;
        g_hooked = true;
        g_pourSize = GS::DefaultProjectileSize();
        UE_LOGI("spill: watching FinishSpawningActor for spilled decals (projectile default size %.2f)", g_pourSize);
    }
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_world) {
        g_world = world;
        Reset();
    }
    if (s->role() == coop::net::Role::Host) HostTick(*s);
    else if (s->role() == coop::net::Role::Client) ClientTick(*s);
}

void OnState(const coop::net::SpillPayload& p) {
    if (Valid(p) && g_inbox.size() < kMaxSpills) g_inbox.push_back(p);
}

void OnIntent(int senderSlot, const coop::net::SpillPayload& p) {
    if (senderSlot <= 0 || senderSlot >= static_cast<int>(coop::net::kMaxPeers) || !Valid(p) || g_intents.size() >= kCap) return;
    g_intents.push_back({senderSlot, p});
}

void QueueConnectBroadcastForSlot(int slot) {
    coop::net::Session* s = Live();
    if (!s || s->role() != coop::net::Role::Host) return;
    size_t sent = 0;
    for (auto it = g_spills.begin(); it != g_spills.end();) {
        if (!it->decal.Get()) {   // wiped away: the next tick says so to everyone
            ++it;
            continue;
        }
        Payload again = it->p;
        again.splash = 0;   // a replay: it landed long ago
        if (s->SendReliableToSlot(slot, coop::net::ReliableKind::SpillState, &again, sizeof(again))) ++sent;
        ++it;
    }
    if (sent) UE_LOGI("spill: %zu spills again for slot %d", sent, slot);
}

void OnDisconnect() {
    if (g_sent || g_made || g_refused || g_removed)
        UE_LOGI("spill: session end -- sent=%u made=%u refused=%u own removed=%u", g_sent, g_made, g_refused, g_removed);
    g_sent = g_made = g_refused = g_removed = 0;
    Reset();
    g_live.store(false);
    g_session.store(nullptr);
}

uint32_t SplashesPlayed() { return g_splashes.load(); }
uint32_t PoursHeard() { return g_poursHeard.load(); }
uint32_t PoursSent() { return g_poursSent.load(); }

void OnPour(const coop::net::SpillPourPayload& p) {
    if (p.cue != 1 || !Finite3(p.loc, coop::net::kMaxCoord)) return;
    if (p.origin == coop::players::Registry::Get().LocalPeerId()) return;   // this player poured it and heard it
    void* me = coop::players::Registry::Get().Local();
    if (!me || !coop::net_pump::HasAnnouncedWorldReady()) return;
    coop::prop_sound::PlayBottlePourAt(me, {p.loc[0], p.loc[1], p.loc[2]});
    ++g_poursHeard;
}

void OnPourIntent(int senderSlot, const coop::net::SpillPourPayload& p) {
    if (senderSlot <= 0 || senderSlot >= static_cast<int>(coop::net::kMaxPeers) || p.cue != 1 ||
        !Finite3(p.loc, coop::net::kMaxCoord) || g_pourIntents.size() >= kCap)
        return;
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(senderSlot));
    void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
    ue_wrap::FVector by{};
    if (!body || !E::TryGetActorLocation(body, by)) return;
    const float dx = p.loc[0] - by.X, dy = p.loc[1] - by.Y, dz = p.loc[2] - by.Z;
    if (dx * dx + dy * dy + dz * dz > kReachCm * kReachCm) return;   // a bottle in a hand is by its holder
    auto w = p;
    w.origin = static_cast<uint8_t>(senderSlot);
    g_pourIntents.push_back(w);
}

}  // namespace coop::spill_sync
