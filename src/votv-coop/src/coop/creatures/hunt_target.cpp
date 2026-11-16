// coop/creatures/hunt_target.cpp -- see coop/creatures/hunt_target.h.

#include "coop/creatures/hunt_target.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/hunt_target_rule.h"
#include "coop/creatures/served_player.h"
#include "coop/element/mirror_managers.h"
#include "coop/element/npc.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace coop::hunt_target {
namespace {

namespace E  = ue_wrap::engine;
namespace PL = coop::players;
namespace R  = ue_wrap::reflection;
namespace SP = coop::served_player;

// One creature class the host's hunt covers. `seams` are the player-0 reads its Blueprint makes (served_player's). `rangeCm` is the
// range its own Blueprint tests, measured from the creature (0: it has none). `reads` and `range` say, in the words of the bytecode
// capture, where each comes from: docs/npcs-and-kerfur.md names the hunters and their functions.
struct Row {
    const wchar_t* className;
    uint8_t        seams;
    float          rangeCm;
    const char*    note;
};

constexpr uint8_t kPawn = SP::kPlayerPawn, kChar = SP::kPlayerCharacter, kMain = SP::kMainPlayerAny, kCam = SP::kCameraManager;

constexpr Row kRows[] = {
    // GetPlayerPawn(0) in move (its stalking circle and its chase), preparejump, throwfireball, playerOverlap, scanForButter and the
    // move callbacks; getMainPlayer in playerOverlap (the line trace, the stab's target) and scanForButter; GetPlayerCameraManager(0)
    // in trytodespawn (GetDistanceTo only). The Blueprint tests no range for the hunt; the two distances it does use are 5000 cm: its
    // despawn leash (trytodespawn: GetDistanceTo > 0x459c4000) and the radius of the stalking circle it walks to (move, mode 0). It
    // stops on that circle, up to 1000 cm off it by the navmesh projection, so the range is that circle and half again: a player it
    // is stalking is never dropped for standing where it stopped, and one that went away is.
    {L"npc_krampus_C", kPawn | kMain | kCam, 7500.f, "krampus"},
    // gettarget (the move-to goal when no specificTarget is set), playerSwing (the distance test of its reputation hit), receivetick
    // and throw (the ballistic throw at the player), playerSpd (GetPlayerCharacter), GetPlayerCameraManager(0) as a trace origin
    // (K2_GetActorLocation only). Its Blueprint tests no range to follow.
    {L"npc_arirFollower_C", kPawn | kChar | kCam, 0.f, "ariral follower"},
    // Not a row, the gun stealer (npc_arirGunStealer_C): its only player read is the move-to goal when no gun is left to steal
    // (receivebeginplay and the move callbacks), and it goes for the gun while one stands, so in play that read comes once, at its begin
    // play, before any pass can serve it (the drill's stealer arm: it slapped the host at once, with no read answered, then fled and
    // ended). Its slap is an overlap of its punch sphere against mainPlayer_C and lands on whichever player it reaches.
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

constexpr long long kPassMs = 250;

struct Hunter {
    void*   actor;
    int32_t index;
    uint8_t row;
    uint8_t target;
    uint32_t stamp;
};
std::vector<Hunter> g_hunters;
uint32_t            g_stamp = 0;
long long           g_nextMs = 0;
bool                g_skipSaid = false;
bool                g_seamsSaid[kRowCount] = {};

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The row of `actor`'s class, or its first ancestor's that a row names: -1 for a class no row holds.
int RowOf(void* actor) {
    int hops = 0;
    for (void* cls = R::ClassOf(actor); cls && hops < 16; cls = R::SuperStructOf(cls), ++hops) {
        const R::FName name = R::NameOf(cls);
        for (int i = 0; i < kRowCount; ++i)
            if (R::NameEquals(name, kRows[i].className)) return i;
    }
    return -1;
}

void* PuppetBody(uint8_t slot) {
    coop::RemotePlayer* rp = PL::Registry::Get().Puppet(slot);
    return (rp && rp->valid() && rp->HasPose()) ? rp->GetActor() : nullptr;
}

// The host is always alive to a hunter; a client counts while its puppet lives.
bool Alive(uint8_t slot) { return slot == PL::kPeerIdHost || PL::Registry::Get().LifeOf(slot) == PL::PuppetLife::Alive; }

float Distance(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Each slot as the pass finds it from `at`: its body (the host's is the local pawn, a client's its puppet once posed) and its 3D
// distance, negative when the body did not read.
size_t ViewPlayers(const ue_wrap::FVector& at, PlayerView (&out)[PL::kMaxPeers]) {
    size_t n = 0;
    for (uint8_t slot = 0; slot < PL::kMaxPeers; ++slot) {
        void* body = slot == PL::kPeerIdHost ? PL::Registry::Get().Local() : PuppetBody(slot);
        ue_wrap::FVector p{};
        const bool read = body && R::IsLive(body) && E::TryGetActorLocation(body, p);
        out[n++] = PlayerView{slot, Alive(slot), read ? Distance(at, p) : -1.f};
    }
    return n;
}

Hunter* Find(void* actor) {
    for (Hunter& h : g_hunters)
        if (h.actor == actor) return &h;
    return nullptr;
}

// A creature's record goes: its reads answer the host again.
void Release(const Hunter& h) {
    if (h.target != kNoTarget && h.target != kHostSlot) SP::Unserve(h.actor);
}

void Apply(Hunter& h, uint8_t slot, float distCm) {
    const Row& row = kRows[h.row];
    if (slot == kHostSlot) {
        SP::Unserve(h.actor);
    } else if (!SP::Serve(h.actor, slot, row.seams)) {
        // A seam that is not ready (or one refused for this process, which served_player says) leaves the creature reading the host.
        if (!g_seamsSaid[h.row]) {
            g_seamsSaid[h.row] = true;
            UE_LOGW("hunt_target: a %s cannot be served slot %u (the player-0 seams are not all ready or one is refused); it "
                    "reads the host", row.note, static_cast<unsigned>(slot));
        }
        slot = kHostSlot;
    }
    const uint8_t was = h.target;
    h.target = slot;
    if (was != slot)
        UE_LOGI("hunt_target: %s %p hunts slot %d (was %d, %.0f cm away, own range %.0f cm)", row.note, h.actor,
                static_cast<int>(slot), was == kNoTarget ? -1 : static_cast<int>(was), distCm < 0.f ? -1.f : distCm,
                row.rangeCm);
}

}  // namespace

// The dev switch: the host is left out of the candidates. Read once.
bool SkipHost() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::hunt_skip_host);
    if (on && !g_skipSaid) {
        g_skipSaid = true;
        UE_LOGW("hunt_target: DRILL -- the host is left out of the hunters' candidates (dev.hunt_skip_host); a hunter picks a "
                "client, and the host only as the fallback");
    }
    return on;
}

uint8_t NearestTo(void* origin, float rangeCm) {
    ue_wrap::FVector at{};
    if (!origin || !E::TryGetActorLocation(origin, at)) return kHostSlot;
    PlayerView views[PL::kMaxPeers];
    const size_t n = ViewPlayers(at, views);
    return Decide(kNoTarget, views, n, Rule{rangeCm, SkipHost()});
}

void Install(coop::net::Session& /*session*/) {}

void Tick(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Host || !session.running()) return;
    const long long now = NowMs();
    if (now < g_nextMs) return;
    g_nextMs = now + kPassMs;

    std::vector<coop::element::Npc*> npcs;
    coop::element::NpcMirrors().Snapshot(npcs);
    ++g_stamp;
    const bool skip = SkipHost();
    for (coop::element::Npc* npc : npcs) {
        void* actor = npc ? npc->LiveActor() : nullptr;  // slot-validated
        if (!actor) continue;
        const int row = RowOf(actor);
        if (row < 0) continue;
        const int32_t index = R::InternalIndexOf(actor);
        Hunter* h = Find(actor);
        if (h && h->index != index) {  // a new creature at a dead one's address: the dead one's record is gone with it
            Release(*h);
            *h = g_hunters.back();
            g_hunters.pop_back();
            h = nullptr;
        }
        if (!h) {
            g_hunters.push_back(Hunter{actor, index, static_cast<uint8_t>(row), kNoTarget, 0});
            h = &g_hunters.back();
        }
        h->stamp = g_stamp;
        if (!SP::SeamsReady(kRows[row].seams)) continue;  // the seams install in the pump's first ticks
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(actor, at)) continue;
        PlayerView views[PL::kMaxPeers];
        const size_t n = ViewPlayers(at, views);
        // With no client to choose among, nothing is chosen: the host is what the Blueprint reads already, and a host picked then
        // would be kept for good, though a client who comes later may stand nearer.
        bool anyClient = false;
        for (size_t i = 0; i < n; ++i)
            if (views[i].slot != kHostSlot && views[i].alive && views[i].distanceCm >= 0.f) anyClient = true;
        if (!anyClient && h->target == kNoTarget) continue;
        const uint8_t slot = Decide(h->target, views, n, Rule{kRows[row].rangeCm, skip});
        float distCm = -1.f;
        for (size_t i = 0; i < n; ++i)
            if (views[i].slot == slot) distCm = views[i].distanceCm;
        if (slot != h->target) Apply(*h, slot, distCm);
    }
    // A creature that was not seen this pass has ended (or lost its element): its record goes.
    for (size_t i = 0; i < g_hunters.size();) {
        if (g_hunters[i].stamp == g_stamp) { ++i; continue; }
        Release(g_hunters[i]);
        g_hunters[i] = g_hunters.back();
        g_hunters.pop_back();
    }
}

uint8_t TargetOf(void* creature) {
    const Hunter* h = Find(creature);
    return h ? h->target : kNoTarget;
}

void OnPeerLeft(uint8_t slot) {
    for (Hunter& h : g_hunters)
        if (h.target == slot) h.target = kNoTarget;  // served_player dropped the record with the peer
}

void OnDisconnect() {
    g_hunters.clear();
    g_nextMs = 0;
    g_skipSaid = false;
}

}  // namespace coop::hunt_target
