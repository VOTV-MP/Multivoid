#include "dev/teleport_client.h"

#include "coop/players_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/shutdown.h"
#include "dev/common.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <string>

namespace dev::teleport_client {

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

bool KeyDown(int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; }

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

void SnapshotAndSend(coop::net::Session* s) {
    void* local = coop::players::Registry::Get().Local();
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
    while (!coop::shutdown::IsShuttingDown()) {
        const bool f4 = ::dev::IsOurWindowForeground() && KeyDown(VK_F4);
        if (f4 && !prevF4) {
            auto* s = g_session.load(std::memory_order_acquire);
            if (s && s->role() == coop::net::Role::Host) {
                GT::Post([s] { SnapshotAndSend(s); });
            } else if (s) {
                UE_LOGI("teleport_client: F4 pressed but local role is Client -- "
                        "this keybind is host-only (per scope project-dev-features-F3-F4)");
            }
        }
        prevF4 = f4;
        ::Sleep(8);
    }
    return 0;
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
