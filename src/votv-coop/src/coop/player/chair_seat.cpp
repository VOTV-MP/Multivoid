// coop/player/chair_seat.cpp -- a player sitting on a chair, on every peer. See the header.

#include "coop/player/chair_seat.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/local_streams.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_seat.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/actors/prop_flags.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/world/world_instances.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::chair_seat {
namespace {

namespace N = coop::net;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace P = ue_wrap::profile;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr int kSlots = coop::players::kMaxPeers;
constexpr uint32_t kNone = 0xFFFFFFFFu;
// How far a sitter's body may be from the chair it says it sat on: the game's own use reach is 2.5 m, and the stream
// lags.
constexpr float kReachCm = 500.f;
// How far below the sitter's capsule its body goes: the seat pose draws the body that far above its actor (seen on the
// rig: a body at the sitter's capsule floated over the chair).
constexpr float kBodyDropCm = 145.f;
// And how far ahead of it, along the seat's facing: the seat pose draws the hips that far behind its actor (seen on the
// rig, a body 80 cm back of an office chair's seat).
constexpr float kBodyAheadCm = 80.f;

N::Session* g_session = nullptr;
uint32_t g_seat[kSlots];   // HOST: the chair each slot sits on, kNone when none
uint32_t g_shown[kSlots];  // every peer: the chair each other slot's body sits on here
void* g_mySeat = nullptr;  // the chair this peer's own player sat on, as last seen; compared, never dereferenced
uint32_t g_myEid = kNone;
// The chair this peer's player sits on, frozen here while it sits, and the flags it had: the sitter's own body would
// otherwise push its copy over where no other copy feels it. The laptop's own seat does the same (stabilizeSeat).
ue_wrap::CachedObjRef g_frozen;
bool g_frozenWasStatic = false;

void Unfreeze() {
    if (void* chair = g_frozen.Get()) ue_wrap::prop::CallSetPropProps(chair, g_frozenWasStatic, false, false, false);
    g_frozen.Reset();
}

void Freeze(void* chair) {
    Unfreeze();
    if (!chair || ue_wrap::prop::IsFrozen(chair)) return;
    g_frozenWasStatic = ue_wrap::prop::IsStatic(chair);
    if (ue_wrap::prop::CallSetPropProps(chair, g_frozenWasStatic, true, false, false)) g_frozen.Set(chair);
}

void ResetTables() {
    for (int i = 0; i < kSlots; ++i) g_seat[i] = g_shown[i] = kNone;
    g_mySeat = nullptr;
    g_myEid = kNone;
}

std::wstring KeyOf(uint32_t eid) { return std::wstring(coop::puppet_seat::kChairKeyPrefix) + std::to_wstring(eid); }

uint32_t EidOf(void* chair) {
    coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(chair);
    if (eid == coop::element::kInvalidId) eid = coop::remote_prop::ResolveMirrorEidByActor(chair);
    return eid == coop::element::kInvalidId ? kNone : static_cast<uint32_t>(eid);
}

void* ObjField(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off < 0 ? nullptr : *reinterpret_cast<void**>(static_cast<uint8_t*>(obj) + off);
}

bool IsBox(void* obj) { return obj && R::ClassNameOf(obj) == L"sitBox_C"; }

// The sit box on `chair`: a sitBox_C attached to it. First down the chair's own components' attach children, where a
// child another actor owns names that actor; then, should none be found there, among every box.
void* BoxOf(void* chair) {
    void* stack[32];
    int top = 0, visited = 0;
    if (void* root = E::RootComponentOf(chair)) stack[top++] = root;
    while (top > 0 && visited++ < 64) {
        const uint8_t* comp = static_cast<const uint8_t*>(stack[--top]);
        void* const* children = *reinterpret_cast<void* const* const*>(comp + P::off::USceneComponent_AttachChildren);
        const int32_t num = *reinterpret_cast<const int32_t*>(comp + P::off::USceneComponent_AttachChildren + 8);
        if (!children || num <= 0 || num > 256) continue;
        for (int32_t i = 0; i < num; ++i) {
            void* child = children[i];
            if (!child || !R::IsLive(child)) continue;
            void* owner = R::OuterOf(child);
            if (owner == chair) { if (top < 32) stack[top++] = child; }
            else if (IsBox(owner)) return owner;
        }
    }
    static void* boxes[1024];
    const int32_t n = ue_wrap::world_instances::Find(L"sitBox_C", boxes, 1024);
    for (int32_t i = 0; i < n; ++i) {
        void* parent = nullptr;
        if (E::GetAttachParentActor(boxes[i], parent) && parent == chair) return boxes[i];
    }
    return nullptr;
}

// `v` turned by `r`, the engine's rotation matrix.
FVector Rotate(const FRotator& r, const FVector& v) {
    constexpr float kRad = 0.01745329252f;
    const float sp = std::sin(r.Pitch * kRad), cp = std::cos(r.Pitch * kRad);
    const float sy = std::sin(r.Yaw * kRad), cy = std::cos(r.Yaw * kRad);
    const float sr = std::sin(r.Roll * kRad), cr = std::cos(r.Roll * kRad);
    const FVector x{cp * cy, cp * sy, sp};
    const FVector y{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    const FVector z{-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
    return FVector{v.X * x.X + v.Y * y.X + v.Z * z.X, v.X * x.Y + v.Y * y.Y + v.Z * z.Y, v.X * x.Z + v.Y * y.Z + v.Z * z.Z};
}

// puppet_seat's chair resolver: the chair under the key and where a body sits on it. The game's sit puts the sitter's
// capsule at the box's transform of its `sitting` component's `location`, turned as the box is.
bool ResolveChair(const std::wstring& key, uint8_t, void*& chair, int32_t& chairIdx, FVector& loc, FRotator& rot) {
    const std::wstring digits = key.substr(std::wcslen(coop::puppet_seat::kChairKeyPrefix));
    if (digits.empty()) return false;
    chair = coop::remote_prop::ResolveLiveActorByEid(static_cast<uint32_t>(std::wcstoul(digits.c_str(), nullptr, 10)));
    void* box = chair ? BoxOf(chair) : nullptr;
    FVector boxAt{};
    FRotator boxRot{};
    if (!box || !E::TryGetActorLocation(box, boxAt) || !E::TryGetActorRotation(box, boxRot)) {
        UE_LOGW("chair_seat: %ls resolves to no seat (chair %p, box %p)", key.c_str(), chair, box);
        return false;
    }
    FVector local{};
    if (void* comp = ObjField(box, L"sitting")) {
        const int32_t off = R::FindPropertyOffset(R::ClassOf(comp), L"location");
        if (off >= 0) std::memcpy(&local, static_cast<uint8_t*>(comp) + off, sizeof(local));
    }
    const FVector d = Rotate(boxRot, local);
    const float yaw = boxRot.Yaw * 0.01745329252f;
    loc = FVector{boxAt.X + d.X + std::cos(yaw) * kBodyAheadCm, boxAt.Y + d.Y + std::sin(yaw) * kBodyAheadCm,
                  boxAt.Z + d.Z - kBodyDropCm};
    rot = FRotator{0.f, boxRot.Yaw, 0.f};
    chairIdx = R::InternalIndexOf(chair);
    return true;
}

// Shows `slot`'s seat on this peer: its body on the chair, or standing.
void Show(int slot, uint32_t eid) {
    if (slot < 0 || slot >= kSlots || slot == coop::players::Registry::Get().LocalPeerId()) return;
    if (g_shown[slot] == eid) return;
    if (g_shown[slot] != kNone) coop::puppet_seat::Clear(static_cast<uint8_t>(slot), KeyOf(g_shown[slot]));
    g_shown[slot] = eid;
    if (eid != kNone) coop::puppet_seat::Set(static_cast<uint8_t>(slot), KeyOf(eid), 0);
}

void Send(uint8_t op, int slot, uint32_t eid, int to) {
    if (!g_session) return;
    N::ChairSeatPayload p{};
    p.eid = eid;
    p.op = op;
    p.slot = static_cast<uint8_t>(slot);
    if (to >= 0) g_session->SendReliableToSlot(to, N::ReliableKind::ChairSeatState, &p, sizeof(p));
    else g_session->SendReliable(N::ReliableKind::ChairSeatState, &p, sizeof(p));
}

// The player's own `unsit`: detached, set down at the box's get-off point, walking again. True when the call ran.
bool Unsit(void* player) {
    void* f = R::FindFunction(R::ClassOf(player), L"unsit");
    ue_wrap::ParamFrame pf(f);
    return f && pf.valid() && ue_wrap::Call(player, pf);
}

void StandMeUp(const char* why) {
    void* me = coop::players::Registry::Get().Local();
    void* on = nullptr;
    if (!me || !E::ReadMainPlayerSittingOn(me, on) || !on || ue_wrap::atv::IsAtv(on)) return;
    const bool ran = Unsit(me);
    UE_LOGI("chair_seat: this player is stood up (%s, ran=%d)", why, ran ? 1 : 0);
}

// HOST: files `slot`'s seat and tells every peer.
void File(int slot, uint32_t eid) {
    if (g_seat[slot] == eid) return;
    if (g_seat[slot] != kNone) Send(N::kChairSeatStand, slot, g_seat[slot], -1);
    g_seat[slot] = eid;
    if (eid != kNone) Send(N::kChairSeatSit, slot, eid, -1);
    Show(slot, eid);
}

// HOST: a sit refused or ended by the chair taken: the seat goes and the sitter stands.
void GetUp(int slot, uint32_t eid, const char* why) {
    UE_LOGI("chair_seat: HOST slot %d off chair eid=%u (%s)", slot, eid, why);
    if (g_seat[slot] == eid) File(slot, kNone);
    if (slot == coop::players::Registry::Get().LocalPeerId()) StandMeUp(why);
    else Send(N::kChairSeatGetUp, slot, eid, slot);
}

// HOST: whether a player other than `slot` has `chair` in hand.
bool HeldByOther(void* chair, int slot) {
    if (coop::remote_prop::DrivingSlotOf(chair, slot) >= 0) return true;
    return slot != coop::players::Registry::Get().LocalPeerId() && coop::local_streams::LastHeldActor() == chair;
}

// HOST: judges `slot`'s sit on chair `eid`.
void Judge(int slot, uint32_t eid) {
    void* chair = coop::remote_prop::ResolveLiveActorByEid(eid);
    if (!chair) { GetUp(slot, eid, "no such chair here"); return; }
    for (int i = 0; i < kSlots; ++i)
        if (i != slot && g_seat[i] == eid) { GetUp(slot, eid, "the chair is taken"); return; }
    if (HeldByOther(chair, slot)) { GetUp(slot, eid, "the chair is in another's hand"); return; }
    if (slot != coop::players::Registry::Get().LocalPeerId()) {
        coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
        FVector body{}, at{};
        if (p && p->valid() && E::TryGetActorLocation(p->actor(), body) && E::TryGetActorLocation(chair, at)) {
            const float dx = body.X - at.X, dy = body.Y - at.Y, dz = body.Z - at.Z;
            if (dx * dx + dy * dy + dz * dz > kReachCm * kReachCm) { GetUp(slot, eid, "out of reach"); return; }
        }
    }
    UE_LOGI("chair_seat: HOST slot %d sits on chair eid=%u", slot, eid);
    File(slot, eid);
}

}  // namespace

void Install(N::Session* session) {
    if (g_session != session) ResetTables();
    g_session = session;
    coop::puppet_seat::SetChairResolver(&ResolveChair);
}

void Tick() {
    N::Session* s = g_session;
    if (!s || !s->running() || !s->connected()) return;
    const bool host = s->role() == N::Role::Host;
    const int me = coop::players::Registry::Get().LocalPeerId();

    // This peer's own sit and stand edges.
    void* local = coop::players::Registry::Get().Local();
    void* on = nullptr;
    if (local && E::ReadMainPlayerSittingOn(local, on) && on && ue_wrap::atv::IsAtv(on)) on = nullptr;
    if (on != g_mySeat) {
        g_mySeat = on;
        if (on) Freeze(on);
        else Unfreeze();
        const uint32_t eid = on ? EidOf(on) : kNone;
        if (eid != g_myEid) {
            const uint32_t was = g_myEid;
            g_myEid = eid;
            if (host) {
                if (eid != kNone) Judge(me, eid);
                else if (was != kNone) File(me, kNone);
            } else {
                N::ChairSeatPayload p{};
                p.eid = eid != kNone ? eid : was;
                p.op = eid != kNone ? N::kChairSeatSit : N::kChairSeatStand;
                if (p.eid != kNone) s->SendReliable(N::ReliableKind::ChairSeatIntent, &p, sizeof(p));
                UE_LOGI("chair_seat: this player %s chair eid=%u -- told the host", eid != kNone ? "sat on" : "got up from",
                        p.eid);
            }
        }
    }

    // HOST: a chair another player takes in hand stands its sitter up.
    if (!host) return;
    for (int i = 0; i < kSlots; ++i) {
        if (g_seat[i] == kNone) continue;
        void* chair = coop::remote_prop::ResolveLiveActorByEid(g_seat[i]);
        if (!chair) GetUp(i, g_seat[i], "the chair is gone");
        else if (HeldByOther(chair, i)) GetUp(i, g_seat[i], "another player took the chair");
    }
}

void OnIntent(int slot, const N::ChairSeatPayload& p) {
    if (slot <= 0 || slot >= kSlots) return;
    if (p.op == N::kChairSeatSit) Judge(slot, p.eid);
    else if (p.op == N::kChairSeatStand && g_seat[slot] == p.eid) {
        UE_LOGI("chair_seat: HOST slot %d got up from chair eid=%u", slot, p.eid);
        File(slot, kNone);
    }
}

void OnState(const N::ChairSeatPayload& p) {
    if (p.slot >= kSlots) return;
    if (p.op == N::kChairSeatSit) Show(p.slot, p.eid);
    else if (p.op == N::kChairSeatStand) { if (g_shown[p.slot] == p.eid) Show(p.slot, kNone); }
    else if (p.op == N::kChairSeatGetUp && p.slot == coop::players::Registry::Get().LocalPeerId()) {
        // The host's seat for this player is gone; the next sit edge asks again.
        g_mySeat = nullptr;
        g_myEid = kNone;
        Unfreeze();
        StandMeUp("the host's word");
    }
}

void SendJoin(int slot) {
    for (int i = 0; i < kSlots; ++i)
        if (g_seat[i] != kNone && i != slot) Send(N::kChairSeatSit, i, g_seat[i], slot);
}

void OnPeerLeft(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    if (g_session && g_session->role() == N::Role::Host && g_seat[slot] != kNone) File(slot, kNone);
    g_shown[slot] = kNone;  // puppet_seat::Forget drops the body's seat
}

void OnDisconnect() {
    Unfreeze();
    ResetTables();
}

}  // namespace coop::chair_seat
