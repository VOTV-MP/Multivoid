#include "coop/session/teleport_client.h"

#include "coop/player/players_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cmath>
#include <string>

namespace coop::teleport_client {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

namespace {

std::atomic<coop::net::Session*> g_session{nullptr};

// Cached local mainPlayer_C pointer + cached teleportWObackrooms UFunction.
// 2026-05-25 NIGHT (user retest +2): user reported "client just jumps not
// changing his location" after retest -- K2_TeleportTo was being called,
// returned true, but the actual position didn't stick. VOTV's own player
// constraints revert large-distance K2_TeleportTo / SetActorLocation calls
// silently (the autotest comment in harness.cpp:762-772 documents the same
// symptom: client's 14-m autotest teleport "never stuck while the host's
// 50-cm one did -- the engine snapped the actor back near the save spawn").
//
// Root-cause fix (RULE 1): use VOTV's own teleportWObackrooms UFunction
// (mainPlayer.hpp:471). It's the function the game's own BP graph uses for
// in-game teleports (e.g. backrooms exit, door teleports). It bypasses the
// CMC constraints that K2_TeleportTo loses to. Falls back to TeleportTo +
// SetActorLocation only if the VOTV function can't be resolved.
void* g_teleportWObackroomsFn = nullptr;  // mainPlayer_C::teleportWObackrooms UFunction*

// Local-vs-puppet lookup + caching now lives in coop::local_player.
// This module is a thin wrapper that calls Get().

// Resolve mainPlayer_C::teleportWObackrooms once. The signature
// (per mainPlayer.hpp:471) is:
//   void teleportWObackrooms(FTransform NewTransform, bool useRotation, bool trueRotation);
// We pass useRotation=false (we set actor/controller rotation separately,
// avoiding an FRotator->FQuat conversion in the parameter frame). The function
// pointer is class-level (same UFunction* for all instances), cached forever.
void* ResolveTeleportFn(void* anyMainPlayer) {
    if (g_teleportWObackroomsFn) return g_teleportWObackroomsFn;
    if (!anyMainPlayer) return nullptr;
    void* cls = R::ClassOf(anyMainPlayer);
    if (!cls) return nullptr;
    g_teleportWObackroomsFn = R::FindFunction(cls, L"teleportWObackrooms");
    if (!g_teleportWObackroomsFn) {
        UE_LOGW("teleport_client: teleportWObackrooms UFunction not found on mainPlayer_C -- "
                "will fall back to K2_TeleportTo");
    }
    return g_teleportWObackroomsFn;
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void ApplyLocally(const ApplyArgs& args) {
    void* local = coop::players::Registry::Get().Local();
    if (!local) {
        UE_LOGW("teleport_client: no local mainPlayer_C found (not in gameplay?)");
        return;
    }
    const ue_wrap::FVector  loc{args.locX, args.locY, args.locZ};
    const ue_wrap::FRotator rot{args.rotPitch, args.rotYaw, args.rotRoll};

    // Primary path: VOTV's own teleportWObackrooms. Translation only in the
    // FTransform (identity rotation + unit scale defaults from MakeTransform).
    // useRotation=false -> the BP graph keeps whatever rotation the player /
    // controller already had; we set the target rotation explicitly below.
    void* fn = ResolveTeleportFn(local);
    bool moved = false;
    if (fn) {
        const ue_wrap::FTransform xform = ue_wrap::MakeTransform(loc);
        ue_wrap::ParamFrame f(fn);
        f.SetRaw(L"NewTransform", &xform, sizeof(xform));
        f.Set<bool>(L"useRotation", false);
        f.Set<bool>(L"trueRotation", false);
        moved = ue_wrap::Call(local, f);
        if (!moved) {
            UE_LOGW("teleport_client: teleportWObackrooms call failed -- falling back");
        }
    }
    // Fallback path: K2_TeleportTo (large-distance-aware but can be reverted
    // by VOTV constraints), then SetActorLocation as last resort.
    if (!moved) {
        if (!E::TeleportTo(local, loc, rot)) {
            UE_LOGW("teleport_client: TeleportTo failed too -- using SetActorLocation");
            E::SetActorLocation(local, loc);
        }
    }
    // Always update the actor + controller rotation. Without the controller
    // ControlRotation update, the camera stays aimed in the old direction even
    // after the body moves -- the same fix the harness autotest uses
    // (harness.cpp:782-784).
    E::SetActorRotation(local, rot);
    if (void* ctrl = E::GetController(local)) {
        E::SetControlRotation(ctrl, rot);
    }
    UE_LOGI("teleport_client: applied (local=%p path=%s loc=(%.0f,%.0f,%.0f) rot=(p=%.1f y=%.1f r=%.1f))",
            local, fn ? (moved ? "teleportWObackrooms" : "fallback") : "fallback-noFn",
            args.locX, args.locY, args.locZ, args.rotPitch, args.rotYaw, args.rotRoll);
}

namespace {

// Snapshot the host's own mainPlayer pose into a TeleportClientPayload. Game
// thread only (reads the live actor). Returns false if there's no local player.
bool SnapshotHostPose(coop::net::TeleportClientPayload& p) {
    void* local = coop::players::Registry::Get().Local();
    if (!local) {
        UE_LOGW("teleport_client: no local mainPlayer_C to snapshot");
        return false;
    }
    const ue_wrap::FVector  loc = E::GetActorLocation(local);
    const ue_wrap::FRotator rot = E::GetActorRotation(local);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
    return true;
}

void SnapshotAndSend(coop::net::Session* s) {
    coop::net::TeleportClientPayload p{};
    if (!SnapshotHostPose(p)) return;
    const bool sent = s->SendReliable(coop::net::ReliableKind::TeleportClient, &p, sizeof(p));
    if (!sent) UE_LOGW("teleport_client: broadcast failed (channel busy or not connected)");
    else       UE_LOGI("teleport_client: sent host pose to all clients (loc=(%.0f,%.0f,%.0f))",
                       p.locX, p.locY, p.locZ);
}

void SnapshotAndSendToSlot(coop::net::Session* s, int slot) {
    coop::net::TeleportClientPayload p{};
    if (!SnapshotHostPose(p)) return;
    // Single-target reliable (default senderSlot=0 -> the receiving client sees
    // senderPeerSlot==0 == host, passing event_feed's host-only TeleportClient
    // trust gate). Only this peer moves.
    const bool sent = s->SendReliableToSlot(slot, coop::net::ReliableKind::TeleportClient,
                                            &p, sizeof(p));
    if (!sent) UE_LOGW("teleport_client: per-slot send to slot %d failed (not connected?)", slot);
    else       UE_LOGI("teleport_client: sent host pose to slot %d (loc=(%.0f,%.0f,%.0f))",
                       slot, p.locX, p.locY, p.locZ);
}

}  // namespace

void TeleportClientsToHost() {
    // Sender-gates to Role::Host only -- on a client this is a no-op (per scope:
    // "teleport clients to the host when the host triggers it"). The snapshot runs
    // on the game thread; the client applies on receive.
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGI("teleport_client: ignored -- this action is host-only (local role is not Host)");
        return;
    }
    GT::Post([s] { SnapshotAndSend(s); });
}

void TeleportSlotToHost(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGI("teleport_client: per-slot ignored -- host-only (local role is not Host)");
        return;
    }
    if (peerSlot < 1 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) {
        UE_LOGW("teleport_client: per-slot TP rejected -- slot %d out of range", peerSlot);
        return;
    }
    GT::Post([s, peerSlot] { SnapshotAndSendToSlot(s, peerSlot); });
}

}  // namespace coop::teleport_client
