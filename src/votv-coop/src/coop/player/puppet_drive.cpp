// coop/player/puppet_drive.cpp -- see coop/player/puppet_drive.h.
// The per-slot drive step: spawn a puppet on a peer's first pose, apply the
// interpolated stream to it, and retire it when the slot goes away. net_pump
// owns the loop that calls in here; this file owns what happens per slot.

#include "coop/player/puppet_drive.h"

#include "coop/creatures/wisp_grab_hold.h"
#include "coop/dev/perf_probe.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/player_handshake.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"

#include <array>
#include <chrono>
#include <cmath>

namespace coop::puppet_drive {
namespace {

// Per-slot remote-player puppets. Indexed by the coop::players::Registry
// slot convention: slot 0 = host (only used on CLIENT processes to hold the
// host's puppet); slots 1..kMaxPeers-1 = clients (used on HOST processes to
// hold each connected client's puppet). On HOST, slot 0 is unused
// (representing host self); on CLIENT, slots 1..kMaxPeers-1 are unused.
// Each entry's actor / spawn-retry / Tick state is independent.
std::array<coop::RemotePlayer, coop::players::kMaxPeers> g_puppets{};

}  // namespace

coop::RemotePlayer& Puppet(int slot) {
    // g_puppets is a GT-only-by-convention side-table (no mutex). Enforce it.
    UE_ASSERT_GAME_THREAD("g_puppets (puppet_drive::Puppet)");
    return g_puppets[slot];
}

bool DestroySlot(int slot) {
    UE_ASSERT_GAME_THREAD("g_puppets (puppet_drive::DestroySlot)");
    // UnregisterPuppet drops the Player Element from players::Registry. If the puppet was never
    // spawned -- the peer disconnected after Join but before any PoseSnapshot -- then
    // g_puppets[slot].valid() is false while playerBySlot_[slot] may still hold a mirror Element
    // installed by EstablishMirrorForSlot. Calling UnregisterPuppet unconditionally is safe:
    // DropPlayerElement_ is a no-op when playerBySlot_ is null.
    coop::players::Registry::Get().UnregisterPuppet(static_cast<uint8_t>(slot));
    if (g_puppets[slot].valid()) {
        g_puppets[slot].Destroy();
        return true;
    }
    return false;
}

void DriveTick(coop::net::Session& session, bool worldReadyAnnounced) {
    UE_ASSERT_GAME_THREAD("g_puppets (puppet_drive::DriveTick)");
    namespace PP = coop::dev::perf_probe;

    // Pose-apply diagnostic. net-diag measures the wire; this measures the APPLY side. Per remote
    // puppet, accumulate the FRESH-pose count (isNew per second) and the latest stream target
    // below, then log once a second the target against the puppet's rendered position and the
    // trailing distance. Healthy is about sendHz fresh per second with a small trail; a big or
    // growing trail means the interpolation and engine apply lag an on-time stream, and a low
    // fresh count means upstream staleness. The Tick is game-thread-only, so the static locals
    // need no atomics.
    static std::array<int, coop::players::kMaxPeers> sPoseFresh{};
    static std::array<coop::net::PoseSnapshot, coop::players::kMaxPeers> sPoseTarget{};
    static std::chrono::steady_clock::time_point sNextPoseDiag{};

    {
        PP::Scope _s{PP::Bucket::Puppets};
        using namespace std::chrono;
        // Per-slot spawn retry timer (static-local: harmless across session
        // restarts because the timer is monotonic and a stale "wait until X"
        // either lets us spawn immediately if X is in the past, or makes us
        // wait the bounded remaining time -- no risk of carrying state that
        // affects correctness).
        static std::array<steady_clock::time_point, coop::players::kMaxPeers> sNextSpawnAttempt{};
        const auto now = steady_clock::now();
        // Host self-assigns slot 0 in the registry. Idempotent setter so it's
        // fine to run every tick. Client LocalPeerId is set asynchronously by
        // the wire-layer AssignPeerSlot handler (event_feed.cpp).
        if (session.role() == coop::net::Role::Host) {
            coop::players::Registry::Get().SetLocalPeerId(coop::players::kPeerIdHost);
        }
        const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
        for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
            // Never drive a puppet for our own slot. On the host localSlot==0;
            // on a client localSlot is its assigned slot (kPeerIdUnknown=0xFF
            // until AssignPeerSlot lands, which matches no slot -> all slots
            // eligible, but TryGetRemotePose gates spawning anyway).
            if (static_cast<uint8_t>(slot) == localSlot) continue;
            coop::net::PoseSnapshot remote;
            bool isNew = false;
            if (!session.TryGetRemotePose(slot, remote, &isNew)) continue;
            sPoseTarget[slot] = remote;  // pose-diag: latest stored pose (the interp target source)
            if (!g_puppets[slot].valid()) {
                // A save-transfer joiner receives poses while still at the MENU (downloading
                // and loading the host save) -- never spawn a puppet into a non-gameplay
                // world. The pose keeps streaming and the spawn happens on the first pose
                // after world-ready.
                if (session.role() == coop::net::Role::Client &&
                    !worldReadyAnnounced) continue;
                // Spawn-retry backoff: BeginDeferredActorSpawnFromClass refuses
                // if the world is mid-transition (OMEGA->story under multi-
                // instance CPU contention can take many seconds; refused
                // spawns reach hundreds/sec at 60 Hz pump rate = pure log
                // noise + wasted reflection calls). Only retry once per second
                // after a failure (RULE 1: don't crutch the engine, just wait).
                if (now < sNextSpawnAttempt[slot]) continue;
                UE_LOGI("net: first remote pose on slot %d -> auto-spawning puppet", slot);
                // The puppet wears the skin this peer announced (the Join field
                // or a later SkinChange), any slot incl. the
                // host. Empty (not yet announced) -> kel baseline; the skin re-applies
                // live when it lands (player_handshake::StoreSkinForSlot).
                if (!g_puppets[slot].Spawn(coop::player_handshake::SkinForSlot(slot))) {
                    UE_LOGW("net: slot %d puppet spawn failed; will retry in 1 s", slot);
                    sNextSpawnAttempt[slot] = now + seconds(1);
                    continue;
                }
                // Register with the central Registry. peerId == slot directly.
                coop::players::Registry::Get().RegisterPuppet(
                    static_cast<uint8_t>(slot), &g_puppets[slot]);
                // Apply the cached nickname now that the puppet exists. The identity
                // (Join / PlayerJoined) can arrive BEFORE the first pose spawns the
                // puppet -- in that race the handler cached the nick and found no
                // puppet to label. Reading the cache here closes the gap for the host
                // puppet and for cross-peer puppets alike. Falls back to the
                // placeholder if no identity has landed yet; the handler re-applies
                // on arrival.
                g_puppets[slot].SetNickname(
                    coop::player_handshake::NicknameForSlot(slot));
                // Join announcement at the APPEARANCE seam: the puppet just spawned, which
                // is the moment a player actually sees the peer -- announcing on world-ready
                // instead runs seconds ahead of the puppet in the measured live flow.
                // CLIENT: announce here -- "Joined <host>'s game" (slot 0, +5s: own loading
                // screen) or cross-peer "<nick> joined the game" (immediate); the whole block
                // is gated on the client's own g_worldReadyAnnounced (the `continue` above).
                // HOST: announce here only once the slot is world-ready, because a pre-world
                // menu or loading pose can spawn the puppet early; in that order
                // OnClientWorldReady announces instead (both funnel through the same latch).
                if (session.role() == coop::net::Role::Client ||
                    session.IsSlotWorldReady(slot))
                    coop::player_handshake::AnnouncePeerSpawned(session.role(), slot);
            }
            // Only RE-BASE the interpolation on a NEW packet; re-pushing the
            // latest every frame would zero `errorPos_` mid-window and freeze
            // motion. The per-frame advance happens in Tick() below.
            if (isNew) {
                g_puppets[slot].SetTargetPose(remote);
                ++sPoseFresh[slot];  // pose-diag: a fresh target this second
            }
        }
    }
    // Per-slot ragdoll PELVIS-physics drive. Decoupled from the pose stream above, so a momentary
    // pose-packet gap cannot stall the ragdoll velocity feed. Only FRESH packets apply -- the
    // puppet's mirror body integrates between them. SetRagdollPose applies the velocity to the
    // body and stamps the streamed pelvis rotation for the next Tick's ApplyToEngine. Runs BEFORE
    // the Tick loop so the velocity is set before the same-frame ApplyToEngine reads the body.
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (!g_puppets[slot].valid()) continue;
        coop::net::RagdollPoseSnapshot rdoll;
        bool rdollNew = false;
        if (session.TryGetRemoteRagdollPose(slot, rdoll, &rdollNew) && rdollNew) {
            g_puppets[slot].SetRagdollPose(rdoll);
        }
    }

    // Tick every live puppet (independent of which slot received pose data
    // this frame -- the per-puppet interpolation needs Tick every frame even
    // when no fresh pose arrived).
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (g_puppets[slot].valid()) g_puppets[slot].Tick();
    }

    // Killer-wisp grab-window body placement. MUST run AFTER the puppet Tick loop above: a held
    // victim puppet is snapped to the wisp's 'playerGrab' socket, overwriting the streamed pose
    // for the hold window (the module's own liveness guards release it). Cheap no-op when
    // nothing is held.
    coop::wisp_grab_hold::Tick();

    // Pose-apply diagnostic emit (once/sec). target = the latest pose received for this slot;
    // puppet = its rendered location AFTER this frame's interp Tick; trail = how far the
    // rendered puppet is behind the target it should be converging to.
    {
        const auto pdNow = std::chrono::steady_clock::now();
        if (pdNow >= sNextPoseDiag) {
            sNextPoseDiag = pdNow + std::chrono::seconds(1);
            for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
                if (!g_puppets[slot].valid()) { sPoseFresh[slot] = 0; continue; }
                const ue_wrap::FVector cur = g_puppets[slot].GetLocation();
                const float dx = sPoseTarget[slot].x - cur.X, dy = sPoseTarget[slot].y - cur.Y;
                const float trail = std::sqrt(dx * dx + dy * dy);
                UE_LOGI("pose-diag[slot %d]: fresh=%d/s targetSpeed=%.0f target=(%.0f,%.0f) "
                        "puppet=(%.0f,%.0f) trail=%.0fcm", slot, sPoseFresh[slot],
                        sPoseTarget[slot].speed, sPoseTarget[slot].x, sPoseTarget[slot].y,
                        cur.X, cur.Y, trail);
                if (trail > 500.f)
                    UE_LOGW("pose-diag[slot %d]: puppet TRAILS its target by %.0f cm (fresh=%d/s) "
                            "-- the apply/interp is behind the on-time pose stream", slot, trail,
                            sPoseFresh[slot]);
                sPoseFresh[slot] = 0;
            }
        }
    }
}

}  // namespace coop::puppet_drive
