// coop/player/puppet_seat.cpp -- see coop/player/puppet_seat.h.

#include "coop/player/puppet_seat.h"

#include "coop/dev/atv_riders_arm.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <cmath>

namespace coop::puppet_seat {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

constexpr int kSlots = coop::players::kMaxPeers;

struct SlotState {
    bool hasTarget = false;
    std::wstring key;            // the ATV the peer sits on
    uint8_t seatIndex = 0;
    void* seatedBody = nullptr;  // the body this owner seated: compared, never dereferenced
    void* atvAttached = nullptr; // the ATV the attach used, set only while the body is attached: compared, index-checked
    int32_t atvIdx = -1;
    std::wstring seatedKey;      // the ATV the body was seated under, for the release's record
    bool warned = false;         // a failed read was said for warnedKey
    std::wstring warnedKey;
};

SlotState g_slots[kSlots];
Resolver g_resolve = nullptr;
Observer g_observer = nullptr;

// Tells the observer an outcome, with what a drill judges it by: the body's distance from the ATV's root, its AnimBP's seat
// pose and its collision. The reads are made only while an observer is set.
void Notify(int slot, int outcome, const std::wstring& key, void* body, void* atv) {
    if (!g_observer) return;
    float offsetCm = 0.f;
    bool onAtv = false, collision = false;
    if (body) {
        ue_wrap::puppet::GetOnAtv(body, onAtv);
        collision = E::GetActorRootCollisionEnabled(body) > 0;
        ue_wrap::FVector at{}, root{};
        ue_wrap::FRotator rot{};
        if (atv && E::TryGetActorLocation(body, at) && ue_wrap::atv::GetRootTransform(atv, root, rot)) {
            const float dx = at.X - root.X, dy = at.Y - root.Y, dz = at.Z - root.Z;
            offsetCm = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    g_observer(static_cast<uint8_t>(slot), outcome, key, offsetCm, onAtv, collision);
}

// Says a failed read once per (slot, key); the next event for the slot tries again with no timer.
void WarnOnce(int slot, SlotState& s, const char* what) {
    if (s.warned && s.warnedKey == s.key) return;
    s.warned = true;
    s.warnedKey = s.key;
    UE_LOGW("puppet_seat: slot %d on %ls -- %s; its body is left to the stream", slot, s.key.c_str(), what);
}

void Release(int slot, SlotState& s, RemotePlayer& p, void* body) {
    void* parent = nullptr;
    // An ATV destroyed under the body detached it already; its pose and collision are restored all the same.
    const bool detach = E::GetAttachParentActor(body, parent) && parent && ue_wrap::atv::IsAtv(parent);
    if (detach) E::DetachActorFromParent(body);
    ue_wrap::puppet::SetOnAtv(body, false);
    E::SetActorEnableCollision(body, true);
    p.OnSeatReleased();
    s.seatedBody = nullptr;
    s.atvAttached = nullptr;
    UE_LOGI("puppet_seat: slot %d released (%s)", slot, detach ? "detached" : "already detached");
    Notify(slot, kReleased, s.seatedKey, body, nullptr);
}

void Seat(int slot, SlotState& s, RemotePlayer& p, void* body) {
    void* atv = nullptr;
    int32_t atvIdx = -1;
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!g_resolve || !g_resolve(s.key, s.seatIndex, atv, atvIdx, loc, rot)) {
        // A body this owner attached to its old ATV is let go the way a seat's end lets it go: left attached, with collision
        // off, it would be held by nobody while the stream writes its location.
        if (s.seatedBody == body && s.atvAttached) Release(slot, s, p, body);
        s.atvAttached = nullptr;
        WarnOnce(slot, s, "no live ATV or no seat transform under that key");
        Notify(slot, kResolveFailed, s.key, nullptr, nullptr);
        return;
    }
    void* parent = nullptr;
    // The parent is read here, at the event, never per pose.
    if (E::GetAttachParentActor(body, parent) && parent == atv) {
        s.atvAttached = atv;
        s.atvIdx = atvIdx;
    } else {
        E::SetActorLocation(body, loc);
        E::SetActorRotation(body, rot);
        // The riders drill's red: the body is placed at the seat and the engine attach is skipped.
        static const bool s_redAttach = coop::dev::atv_riders_arm::Is("body_attach_off");
        if (s_redAttach || E::AttachActorToActorKeepWorld(body, atv, false)) {
            ue_wrap::puppet::DriveCharacterMovement(body, ue_wrap::FVector{}, false);
            s.atvAttached = atv;
            s.atvIdx = atvIdx;
            UE_LOGI("puppet_seat: slot %d seated on %ls (attached)", slot, s.key.c_str());
        } else {
            s.atvAttached = nullptr;
            WarnOnce(slot, s, "the attach to the ATV did not run");
        }
    }
    E::SetActorEnableCollision(body, false);
    if (!ue_wrap::puppet::SetOnAtv(body, true)) WarnOnce(slot, s, "the AnimBP took no seat pose");
    s.seatedBody = body;
    s.seatedKey = s.key;
    Notify(slot, s.atvAttached ? kAttached : kResolveFailed, s.key, body, atv);
}

void Apply(int slot) {
    SlotState& s = g_slots[slot];
    coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* body = (p && p->valid()) ? p->actor() : nullptr;
    if (!body) {  // the target stays; the birth call applies it
        if (s.hasTarget) Notify(slot, kHeldNoBody, s.key, nullptr, nullptr);
        return;
    }
    if (s.seatedBody != body) s.seatedBody = s.atvAttached = nullptr;  // a new body starts from the engine's own state
    if (p->IsRagdollDisplayed()) return;  // both ragdoll stops call OnBodyChanged
    if (s.hasTarget) Seat(slot, s, *p, body);
    else if (s.seatedBody == body) Release(slot, s, *p, body);
}

void DropSlot(SlotState& s) {
    s.hasTarget = false;
    s.key.clear();
    s.seatedBody = s.atvAttached = nullptr;
    s.atvIdx = -1;
    s.seatedKey.clear();
    s.warned = false;
    s.warnedKey.clear();
}

}  // namespace

void SetResolver(Resolver fn) { g_resolve = fn; }

void SetObserver(Observer fn) { g_observer = fn; }

void Set(uint8_t slot, const std::wstring& key, uint8_t seatIndex) {
    UE_ASSERT_GAME_THREAD("puppet_seat::Set");
    if (slot >= kSlots) return;
    SlotState& s = g_slots[slot];
    s.hasTarget = true;
    s.key = key;
    s.seatIndex = seatIndex;
    Apply(slot);
}

void Clear(uint8_t slot, const std::wstring& key) {
    UE_ASSERT_GAME_THREAD("puppet_seat::Clear");
    if (slot >= kSlots) return;
    SlotState& s = g_slots[slot];
    if (!s.hasTarget || s.key != key) return;   // a stale record of another ATV changes nothing, and re-seats nothing
    s.hasTarget = false;
    s.key.clear();
    Apply(slot);
}

// MTA warps a ped into its stored vehicle when the ped's model is created (CClientPed::_CreateModel,
// CClientPed.cpp:3716-3717), the occupancy living apart from the game object (CClientVehicle::SetPedOccupiedVehicle,
// CClientVehicle.cpp:4147); here the stored target is attached at the body's birth.
void OnPuppetSpawned(uint8_t slot) {
    UE_ASSERT_GAME_THREAD("puppet_seat::OnPuppetSpawned");
    if (slot >= kSlots) return;
    g_slots[slot].seatedBody = g_slots[slot].atvAttached = nullptr;
    Apply(slot);
}

void OnBodyChanged(uint8_t slot) {
    UE_ASSERT_GAME_THREAD("puppet_seat::OnBodyChanged");
    if (slot >= kSlots) return;
    Apply(slot);
}

bool HoldsBody(uint8_t slot) {
    if (slot >= kSlots) return false;
    const SlotState& s = g_slots[slot];
    return s.hasTarget && s.atvAttached && R::IsLiveByIndex(s.atvAttached, s.atvIdx);
}

void Forget(uint8_t slot) {
    if (slot >= kSlots) return;
    DropSlot(g_slots[slot]);
}

void OnDisconnect() {
    for (SlotState& s : g_slots) DropSlot(s);
}

}  // namespace coop::puppet_seat
