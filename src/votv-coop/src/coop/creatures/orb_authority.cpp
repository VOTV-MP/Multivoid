// coop/creatures/orb_authority.cpp -- see coop/creatures/orb_authority.h.
//
// The orb's graph, from its bytecode (0.9.0n): ReceiveBeginPlay starts a wander (AIMoveTo a random reachable point,
// again 1 s after each move ends while beginevent is false), saves, and raises the event. The overlap stub casts its
// OtherActor to mainPlayer_C and calls begin: StopActiveMovement, beginevent = true, timeline "b" from its start.
// The timeline's update fades the orb's material in and swells its sounds; its end (b__FinishedFunc) takes player
// 0's camera, and dot(camera forward, normal(camera - orb)) > 0 -- the orb behind the camera -- destroys the orb;
// otherwise a burst of 2D sounds, the glow and the light, then QuitGame and lib_C::End.

#include "coop/creatures/orb_authority.h"

#include "coop/creatures/npc_sync.h"
#include "ue_wrap/actors/spawner_stamp.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/engine/engine_nav.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/death_revive.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <cmath>
#include <random>
#include <vector>

namespace coop::orb_authority {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;
namespace N  = coop::net;

UE_FACT_FN(kRowOrbBegin, "orb_authority", L"npc_orborb_C", L"begin", Declared);
UE_FACT_FN(kRowOrbFinished, "orb_authority", L"npc_orborb_C", L"b__FinishedFunc", Declared);

constexpr int kTagBegin = 0x4F524201;     // 'ORB' 1
constexpr int kTagFinished = 0x4F524202;  // 'ORB' 2

coop::net::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;

// HOST: an orb whose encounter began, who touched it (a slot; 0 the host), and whether its Begin was sent.
struct Encounter {
    ue_wrap::CachedObjRef orb;
    uint8_t toucher = 0;
    bool    sent = false;
};
std::vector<Encounter> g_encounters;

bool IsHost() { return g_session && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->role() == N::Role::Client; }

Encounter* Find(void* orb) {
    for (Encounter& e : g_encounters)
        if (e.orb.Is(orb)) return &e;
    return nullptr;
}

Encounter& Record(void* orb, uint8_t toucher) {
    if (Encounter* e = Find(orb)) return *e;
    g_encounters.emplace_back();
    g_encounters.back().orb.Set(orb);
    g_encounters.back().toucher = toucher;
    return g_encounters.back();
}

bool BeganAlready(void* orb) {
    int32_t off = -1;
    uint8_t mask = 0;
    return R::FindBoolProperty(R::ClassOf(orb), L"beginevent", off, mask) && off >= 0 &&
           (*(static_cast<uint8_t*>(orb) + off) & mask) != 0;
}

bool CallBegin(void* orb) {
    void* fn = R::FindFunction(R::ClassOf(orb), L"begin");
    return fn && R::CallFunction(orb, fn, nullptr);
}

void Send(void* orb, uint8_t op, int slot) {
    if (!g_session) return;
    N::OrbEventPayload p{};
    p.eid = static_cast<uint32_t>(EL::Registry::Get().EidForActor(orb));
    p.op = op;
    if (p.eid == static_cast<uint32_t>(EL::kInvalidId)) {
        UE_LOGW("orb_authority: an orb with no element id -- its %hs reaches no client",
                op == N::kOrbEventBegin ? "begin" : "death");
        return;
    }
    const bool ok = slot >= 0 ? g_session->SendReliableToSlot(static_cast<uint8_t>(slot), N::ReliableKind::OrbEvent, &p,
                                                              sizeof(p))
                              : g_session->SendReliable(N::ReliableKind::OrbEvent, &p, sizeof(p));
    UE_LOGI("orb_authority: HOST sent orb eid %u %hs to %hs (ok=%d)", p.eid,
            op == N::kOrbEventBegin ? "begin" : "death", slot >= 0 ? "its toucher" : "every client", ok ? 1 : 0);
}

// The orb's own facing test against a viewer at `cam` looking along `fwd`: true when the orb is behind the view.
bool BehindView(const ue_wrap::FVector& cam, const ue_wrap::FVector& fwd, const ue_wrap::FVector& orb) {
    const float dx = cam.X - orb.X, dy = cam.Y - orb.Y, dz = cam.Z - orb.Z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-4f) return false;
    return (fwd.X * dx + fwd.Y * dy + fwd.Z * dz) / len > 0.f;
}

ue_wrap::FVector Forward(const ue_wrap::FRotator& r) {
    constexpr float kDeg = 3.14159265f / 180.f;
    const float cp = std::cos(r.Pitch * kDeg), sp = std::sin(r.Pitch * kDeg);
    const float cy = std::cos(r.Yaw * kDeg), sy = std::sin(r.Yaw * kDeg);
    return {cp * cy, cp * sy, sp};
}

// The toucher's view: the host's camera, or a client's streamed eye and newest aim. False when it cannot be read.
bool ViewOf(uint8_t slot, ue_wrap::FVector& cam, ue_wrap::FVector& fwd) {
    if (slot == 0) {
        cam = E::GetCameraLocation();
        fwd = Forward(E::GetCameraRotation());
        return true;
    }
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(slot);
    if (!rp.valid() || !rp.TryGetEyePosition(cam)) return false;
    fwd = rp.GetLatestAimDirection();
    return true;
}

// Begin's watch. HOST: send every client the encounter's start, once per orb.
sg::Verdict OnBeginPre(const sg::Call& c) {
    if (!IsHost() || !c.object) return sg::Verdict::Run;
    Encounter& e = Record(c.object, 0);
    if (!e.sent && !BeganAlready(c.object)) {
        e.sent = true;
        Send(c.object, N::kOrbEventBegin, -1);
    }
    return sg::Verdict::Run;
}

// ---- every player's laps (HOST) -------------------------------------------------------------------------------
// blackballSpawner_C's tick, from its bytecode (0.9.0n), for player 0's camera `cam`:
//   vec   = normal(flatten(inverse spawner rotation * (cam - spawner)))
//   now   = atan2(-vec.y, -vec.x) + 180                      -- the camera's bearing round the spawner, 0..360
//   if now - angle < -180: laps += 1, angle = now, and past maxlaps: a point 200 m from cam at a random bearing,
//       projected to the navmesh in a (10 m, 10 m, 200 m) box; when it lands, laps = 0 and an orb 90 m above it
//   if dot(vec, dir(angle)) > 0.9 and dot(vec, cross(dir(angle), -Z)) > 0: angle = now   -- it follows one way only
// The spawner runs it for the host. The host runs it for each client with that client's eye as `cam`.
struct Laps {
    ue_wrap::CachedObjRef spawner;
    uint8_t slot = 0;
    float   angle = 0.f;
    int32_t laps = 0;
};
std::vector<Laps> g_laps;
int g_devLaps = -1;
std::mt19937 g_rng{std::random_device{}()};

Laps& LapsOf(void* spawner, uint8_t slot, float now) {
    for (Laps& l : g_laps)
        if (l.slot == slot && l.spawner.Is(spawner)) return l;
    g_laps.emplace_back();
    Laps& l = g_laps.back();
    l.spawner.Set(spawner);
    l.slot = slot;
    l.angle = now;  // a client met mid-lap starts its count where it stands
    l.laps = g_devLaps >= 0 ? g_devLaps : 0;
    return l;
}

// The orb the spawner mints, minted by the host for a client: through GameplayStatics' deferred spawn, the call
// npc_sync's interceptor sees, so the orb is enrolled and sent to every client; stamped as the spawner stamps it.
void* MintOrb(const ue_wrap::FVector& at) {
    coop::npc_sync::DevSpawnRefs refs;
    void* const cls = R::FindClass(L"npc_orborb_C");
    void* const ctx = E::GetWorldContext();
    if (!cls || !ctx || !coop::npc_sync::GetDevSpawnRefs(refs)) return nullptr;
    ue_wrap::FTransform xf{};
    xf.TX = at.X;
    xf.TY = at.Y;
    xf.TZ = at.Z;
    void* orb = nullptr;
    {
        ue_wrap::ParamFrame begin(refs.beginDeferredFn);
        if (!begin.valid()) return nullptr;
        begin.Set<void*>(L"WorldContextObject", ctx);
        begin.Set<void*>(L"ActorClass", cls);
        begin.SetRaw(L"SpawnTransform", &xf, sizeof(xf));
        begin.Set<uint8_t>(L"CollisionHandlingOverride", 2);  // the spawner's own: adjust, always spawn
        begin.Set<void*>(L"Owner", nullptr);
        if (!ue_wrap::Call(refs.gsCdo, begin)) return nullptr;
        orb = begin.Get<void*>(L"ReturnValue");
    }
    if (!orb) return nullptr;
    if (!ue_wrap::spawner_stamp::Stamp(cls, orb)) UE_LOGW("orb_authority: the orb a client's laps minted was not stamped");
    ue_wrap::ParamFrame finish(refs.finishSpawnFn);
    if (finish.valid()) {
        finish.Set<void*>(L"Actor", orb);
        finish.SetRaw(L"SpawnTransform", &xf, sizeof(xf));
        ue_wrap::Call(refs.gsCdo, finish);
    }
    return orb;
}

void CountSpawner(void* spawner) {
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(spawner, at) || !E::TryGetActorRotation(spawner, rot)) return;
    const int32_t maxOff = R::FindPropertyOffset(R::ClassOf(spawner), L"maxlaps");
    const int32_t maxLaps = maxOff >= 0 ? *reinterpret_cast<int32_t*>(static_cast<char*>(spawner) + maxOff) : 240;
    constexpr float kDeg = 3.14159265f / 180.f;
    const float cy = std::cos(-rot.Yaw * kDeg), sy = std::sin(-rot.Yaw * kDeg);
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(slot);
        ue_wrap::FVector cam{};
        if (!rp.valid() || !rp.TryGetEyePosition(cam)) continue;
        const float wx = cam.X - at.X, wy = cam.Y - at.Y;
        float vx = wx * cy - wy * sy, vy = wx * sy + wy * cy;
        const float len = std::sqrt(vx * vx + vy * vy);
        if (len < 1e-4f) continue;
        vx /= len;
        vy /= len;
        const float now = std::atan2(-vy, -vx) / kDeg + 180.f;
        Laps& l = LapsOf(spawner, slot, now);
        if (now - l.angle < -180.f) {
            ++l.laps;
            l.angle = now;
            if (l.laps > maxLaps) {
                std::uniform_real_distribution<float> bearing(0.f, 360.f);
                const float b = bearing(g_rng) * kDeg;
                const ue_wrap::FVector aim{cam.X + 20000.f * std::cos(b), cam.Y + 20000.f * std::sin(b), cam.Z};
                ue_wrap::FVector ground{};
                if (E::ProjectToNav(spawner, aim, {1000.f, 1000.f, 20000.f}, &ground) &&
                    (ground.X != 0.f || ground.Y != 0.f || ground.Z != 0.f)) {
                    l.laps = 0;
                    void* orb = MintOrb({ground.X, ground.Y, ground.Z + 9000.f});
                    UE_LOGI("orb_authority: HOST slot %u ran %d laps round the orb spawner -- its orb %hs at "
                            "(%.0f,%.0f,%.0f)", static_cast<unsigned>(slot), maxLaps + 1, orb ? "minted" : "NOT minted",
                            ground.X, ground.Y, ground.Z + 9000.f);
                }
            }
        }
        const float dx = std::cos(l.angle * kDeg), dy = std::sin(l.angle * kDeg);
        if (vx * dx + vy * dy > 0.9f && vx * -dy + vy * dx > 0.f) l.angle = now;
    }
}

void CountSpawnerVisit(void*, void* obj, int32_t) {
    if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) CountSpawner(obj);
}

void TickLaps() {
    if (!IsHost()) return;
    void* const cls = ue_wrap::object_index::ClassByName(L"blackballSpawner_C");
    if (cls) ue_wrap::object_index::ForEachInstance(cls, &CountSpawnerVisit, nullptr);
    for (size_t i = 0; i < g_laps.size();) {  // a spawner gone, or a slot whose player left, drops its count
        const bool gone = !g_laps[i].spawner.Alive() || !coop::puppet_drive::Puppet(g_laps[i].slot).valid();
        if (gone) g_laps.erase(g_laps.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

// The timeline's end. A client's copy runs nothing; the host judges the toucher and destroys the orb.
sg::Verdict OnFinishedPre(const sg::Call& c) {
    if (IsClient()) return sg::Verdict::Cancel;
    if (!IsHost() || !c.object) return sg::Verdict::Run;
    void* const orb = c.object;
    const Encounter* e = Find(orb);
    const uint8_t slot = e ? e->toucher : 0;
    ue_wrap::FVector at{}, cam{}, fwd{};
    const bool read = E::TryGetActorLocation(orb, at) && ViewOf(slot, cam, fwd);
    // A view that cannot be read (the toucher left) spares the toucher, as nobody is there to end.
    const bool survives = !read || BehindView(cam, fwd, at);
    UE_LOGI("orb_authority: HOST orb's verdict for slot %u -- %hs%hs", static_cast<unsigned>(slot),
            survives ? "spared (the orb is behind its view)" : "caught (the orb is in its view)",
            read ? "" : ", its view did not read");
    if (!survives) {
        if (slot == 0) {
            if (coop::death_revive::ReviveAvailable())
                coop::death_revive::NoteRunEndCancelled(nullptr, L"the black orb", false);
        } else {
            Send(orb, N::kOrbEventDeath, slot);
        }
    }
    for (auto it = g_encounters.begin(); it != g_encounters.end(); ++it)
        if (it->orb.Is(orb)) { g_encounters.erase(it); break; }
    E::DestroyActor(orb);
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    if (g_watched) return;
    g_watched = true;
    const bool b = sg::WatchName(kRowOrbBegin, kTagBegin, &OnBeginPre, nullptr);
    const bool f = sg::WatchName(kRowOrbFinished, kTagFinished, &OnFinishedPre, nullptr);
    if (!b || !f) UE_LOGE("orb_authority: a watch was refused (begin=%d finished=%d)", b ? 1 : 0, f ? 1 : 0);
}

void Tick() {
    TickLaps();
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowOrbBegin, kTagBegin) || !sg::NameWatchSettled(kRowOrbFinished, kTagFinished)) return;
    g_settled = true;
    const bool b = sg::NameWatchLive(kRowOrbBegin, kTagBegin), f = sg::NameWatchLive(kRowOrbFinished, kTagFinished);
    if (b && f) UE_LOGI("orb_authority: the watches on npc_orborb_C begin and b__FinishedFunc are live");
    else UE_LOGE("orb_authority: a watch is dead (begin=%d finished=%d) -- the orb is judged by each machine", b ? 1 : 0,
                 f ? 1 : 0);
}

void OnDisconnect() {
    g_encounters.clear();
    g_laps.clear();
}

void DevSetClientLaps(int laps) {
    g_devLaps = laps;
    for (Laps& l : g_laps) l.laps = laps >= 0 ? laps : l.laps;
    UE_LOGI("orb_authority: [dev] every client's orb-spawner laps start at %d", laps);
}

sg::Verdict OnTouchPre(void* orb, void* toucher) {
    if (!orb || !toucher) return sg::Verdict::Run;
    if (IsClient()) return sg::Verdict::Cancel;  // the host judges every touch
    if (!IsHost()) return sg::Verdict::Run;      // single player
    auto& reg = coop::players::Registry::Get();
    if (!reg.IsPuppetBody(toucher)) {
        if (toucher == reg.Local() && !BeganAlready(orb)) Record(orb, 0);
        return sg::Verdict::Run;  // the host's own player, as the game runs it
    }
    const uint8_t slot = reg.PeerIdOfBody(toucher);
    if (slot == 0 || slot >= coop::players::kMaxPeers || BeganAlready(orb) || Find(orb)) return sg::Verdict::Cancel;
    Record(orb, slot);
    UE_LOGI("orb_authority: HOST slot %u's body touched an orb -- its encounter begins on the host",
            static_cast<unsigned>(slot));
    ue_wrap::CachedObjRef ref;
    ref.Set(orb);
    ue_wrap::game_thread::Post([ref] {
        if (void* o = ref.Get()) CallBegin(o);
    });
    return sg::Verdict::Cancel;
}

void OnEvent(const coop::net::OrbEventPayload& p) {
    if (!IsClient()) return;
    if (p.op == N::kOrbEventDeath) {
        UE_LOGI("orb_authority: CLIENT the host's orb eid %u caught this player -- the run's end is answered as a death",
                p.eid);
        if (coop::death_revive::ReviveAvailable())
            coop::death_revive::NoteRunEndCancelled(nullptr, L"the black orb", false);
        return;
    }
    if (p.op != N::kOrbEventBegin) return;
    void* orb = EL::LiveActorOfType(static_cast<EL::ElementId>(p.eid), EL::ElementType::Npc);
    const bool ran = orb && CallBegin(orb);
    UE_LOGI("orb_authority: CLIENT orb eid %u's encounter began on the host -- this copy's begin ran=%d", p.eid,
            ran ? 1 : 0);
}

}  // namespace coop::orb_authority
