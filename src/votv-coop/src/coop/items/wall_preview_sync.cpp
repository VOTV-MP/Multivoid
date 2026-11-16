// coop/items/wall_preview_sync.cpp -- see coop/items/wall_preview_sync.h.
//
// prop_wallbuilder_C's graph, from its bytecode (0.9.0n). ReceiveTick, while `isHeld`: setPivotLoc, which traces from
// player 0's camera, puts `pivot_a` at the snapped hit, and while `editing` stretches `cube` from `firstHit` to it (its
// world location and scale) and puts `pivot_b` at the far corner. playerHandUse_LMB shows `cube` and `pivot_b` and sets
// `editing`; playerHandRelease_LMB runs addWall when it hit, then hides both and clears `editing`; RMB cancels the same
// way. addWall spawns the wall at the `rot` component's transform with the cube's scale.

#include "coop/items/wall_preview_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <chrono>
#include <cmath>
#include <cstring>

namespace coop::wall_preview_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace N  = coop::net;

constexpr uint64_t kSendEveryMs = 100;  // a holder's preview, at most this often

coop::net::Session* g_session = nullptr;

bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool Connected() { return g_session && g_session->running() && g_session->connected(); }

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint8_t OwnSlot() { return coop::players::Registry::Get().LocalPeerId(); }

bool IsWallBuilder(void* obj) {
    void* const base = OI::ClassByName(L"prop_wallbuilder_C");
    return obj && base && R::IsLive(obj) && R::IsDescendantOfAny(R::ClassOf(obj), &base, 1);
}

void* Comp(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(obj) + off) : nullptr;
}

bool Editing(void* tool) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!tool || !R::FindBoolProperty(R::ClassOf(tool), L"editing", off, mask)) return false;
    return (static_cast<const uint8_t*>(tool)[off] & mask) != 0;
}

ue_wrap::FVector Scale(void* comp) {
    ue_wrap::ParamFrame f(comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"K2_GetComponentScale") : nullptr);
    if (!f.valid() || !ue_wrap::Call(comp, f)) return {1.f, 1.f, 1.f};
    return f.Get<ue_wrap::FVector>(L"ReturnValue");
}

void SetWorldLocation(void* comp, const ue_wrap::FVector& at) {
    ue_wrap::ParamFrame f(comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"K2_SetWorldLocation") : nullptr);
    if (f.valid() && f.Set<ue_wrap::FVector>(L"NewLocation", at) && f.Set<bool>(L"bTeleport", true)) ue_wrap::Call(comp, f);
}

void SetWorldScale(void* comp, const ue_wrap::FVector& s) {
    ue_wrap::ParamFrame f(comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"SetWorldScale3D") : nullptr);
    if (f.valid() && f.Set<ue_wrap::FVector>(L"NewScale", s)) ue_wrap::Call(comp, f);
}

void SetVisible(void* comp, bool on) {
    ue_wrap::ParamFrame f(comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"SetVisibility") : nullptr);
    if (f.valid() && f.Set<bool>(L"bNewVisibility", on) && f.Set<bool>(L"bPropagateToChildren", true)) ue_wrap::Call(comp, f);
}

void Put(float* out, const ue_wrap::FVector& v) { out[0] = v.X; out[1] = v.Y; out[2] = v.Z; }
ue_wrap::FVector Vec(const float* v) { return {v[0], v[1], v[2]}; }

// The holder's preview as the tool's components hold it.
bool Read(void* tool, N::WallPreviewPayload& p) {
    void* const cube = Comp(tool, L"cube");
    void* const a = Comp(tool, L"pivot_a");
    void* const b = Comp(tool, L"pivot_b");
    if (!cube || !a || !b) return false;
    p.editing = Editing(tool) ? 1 : 0;
    Put(p.cubeLoc, E::GetComponentLocation(cube));
    const ue_wrap::FRotator r = E::GetComponentWorldRotation(cube);
    p.cubeRot[0] = r.Pitch;
    p.cubeRot[1] = r.Yaw;
    p.cubeRot[2] = r.Roll;
    Put(p.cubeScale, Scale(cube));
    Put(p.pivotA, E::GetComponentLocation(a));
    Put(p.pivotB, E::GetComponentLocation(b));
    return true;
}

bool Same(const N::WallPreviewPayload& x, const N::WallPreviewPayload& y) {
    if (x.editing != y.editing || x.held != y.held) return false;
    const float* a = x.cubeLoc;
    const float* b = y.cubeLoc;
    for (int i = 0; i < 15; ++i)
        if (std::fabs(a[i] - b[i]) > 0.5f) return false;  // the five vectors lie one after the other
    return true;
}

// ---- the holder ----------------------------------------------------------------------------------------------------
N::WallPreviewPayload g_sent{};
bool g_sentAny = false;
uint64_t g_nextSendMs = 0;

void Send(const N::WallPreviewPayload& p) {
    if (IsHost()) g_session->SendReliable(N::ReliableKind::WallPreview, &p, sizeof(p));
    else g_session->SendReliableToSlot(0, N::ReliableKind::WallPreview, &p, sizeof(p));
}

void TickHolder() {
    const uint64_t now = NowMs();
    if (now < g_nextSendMs) return;
    void* const hand = coop::hand_item::LocalHandActor();
    N::WallPreviewPayload p{};
    p.slot = OwnSlot();
    p.held = IsWallBuilder(hand) && Read(hand, p) ? 1 : 0;
    if (!p.held) p.editing = 0;
    if (!g_sentAny && !p.held) return;  // nothing ever shown: nothing to end
    if (g_sentAny && Same(p, g_sent)) return;
    g_nextSendMs = now + kSendEveryMs;
    Send(p);
    if (!g_sentAny || p.editing != g_sent.editing || p.held != g_sent.held)
        UE_LOGI("wall_preview: this player's preview %hs -- sent", !p.held ? "ended" : p.editing ? "stretches a wall" : "aims");
    g_sent = p;
    g_sentAny = p.held != 0;
}

// ---- every peer's mirrors ------------------------------------------------------------------------------------------
struct Shown {
    bool have = false;
    N::WallPreviewPayload p{};
};
Shown g_shown[N::kMaxPeers];

// The preview on the slot's mirror of its hand, every frame while it holds, so it stays where the holder aims.
void TickMirrors() {
    const uint8_t own = OwnSlot();
    for (uint8_t s = 0; s < N::kMaxPeers; ++s) {
        Shown& sh = g_shown[s];
        if (!sh.have || s == own) continue;
        void* const tool = coop::hand_item::MirrorActorForSlot(s);
        if (!IsWallBuilder(tool)) continue;
        void* const cube = Comp(tool, L"cube");
        void* const a = Comp(tool, L"pivot_a");
        void* const b = Comp(tool, L"pivot_b");
        const bool editing = sh.p.held && sh.p.editing;
        SetVisible(cube, editing);
        SetVisible(b, editing);
        if (!sh.p.held) {
            sh.have = false;
            continue;
        }
        SetWorldLocation(a, Vec(sh.p.pivotA));
        if (editing) {
            SetWorldLocation(cube, Vec(sh.p.cubeLoc));
            E::SetComponentWorldRotation(cube, {sh.p.cubeRot[0], sh.p.cubeRot[1], sh.p.cubeRot[2]});
            SetWorldScale(cube, Vec(sh.p.cubeScale));
            SetWorldLocation(b, Vec(sh.p.pivotB));
        }
    }
}

bool Finite(const N::WallPreviewPayload& p) {
    const float* v = p.cubeLoc;
    for (int i = 0; i < 15; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

}  // namespace

void Install(coop::net::Session* session) { g_session = session; }

void Tick() {
    if (!Connected()) return;
    TickHolder();
    TickMirrors();
}

void OnPreview(int senderSlot, const coop::net::WallPreviewPayload& p) {
    if (senderSlot < 0 || senderSlot >= static_cast<int>(N::kMaxPeers) || p.slot >= N::kMaxPeers || !Finite(p)) return;
    if (IsHost()) {
        // A client's own hand only, and only while its mirror here is a wall builder (or its preview ends).
        if (senderSlot == 0 || p.slot != senderSlot) return;
        if (p.held && !IsWallBuilder(coop::hand_item::MirrorActorForSlot(p.slot))) return;
        g_session->SendReliable(N::ReliableKind::WallPreview, &p, sizeof(p), senderSlot);
    } else if (senderSlot != 0) {
        return;  // a client takes previews from the host alone
    }
    Shown& sh = g_shown[p.slot];
    const bool was = sh.have && sh.p.editing;
    sh.have = true;
    sh.p = p;
    if (was != (p.held && p.editing))
        UE_LOGI("wall_preview: slot %u's preview %hs here", p.slot, p.held && p.editing ? "stretches a wall" : "stopped stretching");
}

void OnPeerLeft(uint8_t slot) {
    if (slot < N::kMaxPeers) g_shown[slot] = Shown{};
}

void OnDisconnect() {
    for (Shown& s : g_shown) s = Shown{};
    g_sentAny = false;
}

}  // namespace coop::wall_preview_sync
