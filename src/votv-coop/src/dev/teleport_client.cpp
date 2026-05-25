#include "dev/teleport_client.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "dev/common.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <windows.h>

#include <atomic>
#include <cmath>

namespace dev::teleport_client {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

namespace {

std::atomic<coop::net::Session*> g_session{nullptr};

// Cached local mainPlayer_C pointer. Pre-fix every press did a full
// GUObjectArray walk via FindObjectByClass (contributing to the same
// ~100-300 ms hitch documented in restore_vitals.cpp::Cache). Cleared on
// IsLive miss (level change). NOTE: on a client there are TWO mainPlayer_C
// in the scene (local + the puppet representing the host); GUObjectArray
// walk order returns the first non-CDO, which is the LOCAL because it loads
// from save before the puppet is spawned -- the right one for our teleport
// target. Same pattern as dev::pos_hud (which validated this empirically).
void* g_local = nullptr;

bool KeyDown(int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; }

// Cache-refresh helper. Returns nullptr if no live mainPlayer (still booting).
void* GetLocalMainPlayer() {
    if (g_local && !R::IsLive(g_local)) g_local = nullptr;
    if (!g_local) g_local = R::FindObjectByClass(P::name::MainPlayerClass);
    return g_local;
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void ApplyLocally(const ApplyArgs& args) {
    void* local = GetLocalMainPlayer();
    if (!local) {
        UE_LOGW("teleport_client: no local mainPlayer_C found (not in gameplay?)");
        return;
    }
    const ue_wrap::FVector  loc{args.locX, args.locY, args.locZ};
    const ue_wrap::FRotator rot{args.rotPitch, args.rotYaw, args.rotRoll};
    if (!E::TeleportTo(local, loc, rot)) {
        // TeleportTo can fail if the destination overlaps geometry; fall
        // back to SetActorLocation + SetActorRotation (the receiver-side
        // pose-driver pattern from RemotePlayer interp) which never fails
        // but doesn't resolve collision. Acceptable for a dev convenience.
        UE_LOGW("teleport_client: TeleportTo failed, falling back to SetActorLocation");
        E::SetActorLocation(local, loc);
        E::SetActorRotation(local, rot);
    }
    UE_LOGI("teleport_client: applied (local=%p loc=(%.0f,%.0f,%.0f) rot=(p=%.1f y=%.1f r=%.1f))",
            local, args.locX, args.locY, args.locZ, args.rotPitch, args.rotYaw, args.rotRoll);
}

namespace {

// Snapshot host's own mainPlayer pose, build TeleportClientPayload, broadcast.
// Game-thread for the snapshot (E::GetActorLocation / GetActorRotation are
// game-thread-only); the send_reliable call is wire-thread-safe and called
// from inside the GT::Post lambda.
void SnapshotAndSend(coop::net::Session* s) {
    void* local = GetLocalMainPlayer();
    if (!local) {
        UE_LOGW("teleport_client: F4 pressed but no local mainPlayer_C");
        return;
    }
    const ue_wrap::FVector  loc = E::GetActorLocation(local);
    const ue_wrap::FRotator rot = E::GetActorRotation(local);
    coop::net::TeleportClientPayload p{};
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
    const bool sent = s->SendReliable(coop::net::ReliableKind::TeleportClient, &p, sizeof(p));
    if (!sent) UE_LOGW("teleport_client: broadcast failed (channel busy or not connected)");
    else       UE_LOGI("teleport_client: sent host pose to client (loc=(%.0f,%.0f,%.0f))",
                       loc.X, loc.Y, loc.Z);
}

// Hotkey: polls F4 (rising edge). Sender-gates to Role::Host only -- F4 on
// the client is a no-op (per user scope: "Teleport client to host when host
// presses button"). The packet always identifies the host's pose at the
// time of press; the client applies on receive.
DWORD WINAPI HotkeyThread(LPVOID) {
    bool prevF4 = false;
    for (;;) {
        const bool f4 = ::dev::IsOurWindowForeground() && KeyDown(VK_F4);
        if (f4 && !prevF4) {
            auto* s = g_session.load(std::memory_order_acquire);
            if (s && s->role() == coop::net::Role::Host) {
                GT::Post([s] { SnapshotAndSend(s); });
            } else if (s) {
                // Press on client side -- visible logged drop so the user
                // knows the keybind only works on host. Avoid silent UX.
                UE_LOGI("teleport_client: F4 pressed but local role is Client -- "
                        "this keybind is host-only (per scope project-dev-features-F3-F4)");
            }
        }
        prevF4 = f4;
        ::Sleep(8);
    }
}

}  // namespace

void Init() {
    if (!::dev::MasterEnabled()) {
        UE_LOGI("teleport_client: disabled by master switch ([dev] enabled=0)");
        return;
    }
    if (!::dev::IsIniKeyTrue("devkeys")) {
        UE_LOGI("teleport_client: disabled (set [dev] devkeys=1 in votv-coop.ini to enable F4)");
        return;
    }
    if (HANDLE t = ::CreateThread(nullptr, 0, &HotkeyThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);
    }
    UE_LOGI("teleport_client: ENABLED (host F4 -> teleport client to host's pose)");
}

}  // namespace dev::teleport_client
