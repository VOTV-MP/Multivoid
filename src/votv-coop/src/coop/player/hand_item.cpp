// coop/player/hand_item.cpp -- see coop/player/hand_item.h for the design.

#include "coop/player/hand_item.h"

#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/trash_collect_sync.h"  // EnsureHeldItemBroadcast (hand-edge world release)
#include "ue_wrap/engine.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace coop::hand_item {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// Wire caps. BP class names ("prop_physgun_C") and item names ("grenade_p")
// are short ASCII; 63 is generous headroom, one byte each on the wire.
constexpr size_t kMaxStr = 63;

// Stow announce is EDGE-INSTANT (user 2026-07-06 per rule 1: "it's supposed
// to be instant"). The bytecode proves a quick-slot switch is ONE synchronous
// updateHold call (destroy @935 -> spawn @1023 -> holding_name @3183 all in
// the same invocation), so a poll never observes a mid-switch null -- the
// original 15-tick (~250 ms) debounce guarded a flicker that does not exist
// and made the stowed item linger in the peer's hand a quarter second after
// it was already on the ground. Value 1 = announce on the first null tick.
// (If some exotic flow ever DOES split a switch across two updateHold calls,
// the worst case is a single-frame mirror destroy+respawn on the peer.)
constexpr int kEmptyDebounceTicks = 1;

// Display placement (user 2026-07-10: "attached to the root of the camera --
// same as the SP local player: the item just appears in front of the camera,
// but seen from third person on the puppet"). Natively the held item is
// welded into the FP-arms VIEWMODEL chain (updateHold K2_AttachToComponent ->
// 'weapon' <- arms <- arms_lag <- viewmodel, mainPlayer SCS) which is drawn
// camera-relative -- that chain is bHiddenInGame and never ticks on a puppet
// (ref-pose socket put the mirror on the BACK, hands-on 2026-07-06 14:2x),
// and the puppet's Camera component never pitches (pitch is controller-fed).
// So the SP look is reproduced by the per-tick drive: eye anchor +
// synced-look basis x {forward, right, down}, following yaw AND pitch --
// functionally "attached to the camera root". Offsets = camera-front, the
// SP viewmodel's bottom-center-right feel (was: right-shoulder placement,
// superseded by the 2026-07-10 ask). Anchor = GetHeadPosition() (head bone
// + its +33 nameplate lift).
constexpr float kViewForwardCm = 45.f;   // out along the look direction (in front of the face)
constexpr float kViewRightCm   = 10.f;   // slight right bias, like the SP viewmodel
constexpr float kViewUpCm      = -40.f;  // chin height below the lifted head anchor

// ---- per-slot wire state (what each peer's hand holds) -------------------
struct SlotHand {
    bool has = false;
    std::wstring cls;   // BP class name, e.g. L"prop_physgun_C"
    std::wstring name;  // Aprop_C 'name' FName (drives the generic prop mesh), may be empty
};
SlotHand g_hands[coop::players::kMaxPeers];  // GT-only

// ---- per-slot display mirror ---------------------------------------------
struct Mirror {
    void*        actor = nullptr;
    int32_t      idx   = -1;  // GUObjectArray index (IsLiveByIndex validation)
    std::wstring cls;
    std::wstring name;
    int32_t      puppetIdx = -1;  // the puppet we attached to (re-attach on respawn)
};
Mirror g_mirrors[coop::players::kMaxPeers];  // GT-only

// The session, cached by TickOwner (net_pump tick site) for the connect-replay
// entry that only gets a slot (subsystems fanout convention). GT-only.
coop::net::Session* g_session = nullptr;

// ---- owner-side change detection ------------------------------------------
bool         g_ownHas = false;
std::wstring g_ownCls;
std::wstring g_ownName;
int          g_ownEmptyStreak = 0;
// The last-seen holding_actor identity (pointer + captured GUObjectArray index —
// updateHold recycles addresses). While it is unchanged, TickOwner skips the
// ClassNameOf/ReadItemName renders entirely (audit 2026-07-06 MEDIUM: FName::
// ToString allocates engine-side per call; 2 renders x 60 Hz while holding was
// the exact per-tick pattern local_streams already latches for the held eid).
void*        g_ownHeldPtr = nullptr;
int32_t      g_ownHeldIdx = -1;

// The local mainPlayer, cached by TickOwner (pointer + captured index) for
// LocalHandActor's fresh holding_actor read. GT-only.
void*        g_localPlayer    = nullptr;
int32_t      g_localPlayerIdx = -1;

// THE HAND-EDGE WORLD RELEASE (v106, 2026-07-07). An R-drop / quick-slot place
// is NOT a spawn: the game RELEASES the ex-hand view actor into the world (same
// actor, physics/collision re-enabled) -- no spawn seam fires and the census
// high-water guard is blind (NumObjects flat; the v105b root). But WE hold the
// previous hand actor's identity (g_ownHeldPtr + captured index): at the hand
// edge, an ex-hand actor that SURVIVED the edge (updateHold DESTROYS it on a
// stow/switch, so survival == world release) is expressed immediately through
// the canonical untracked-prop broadcast. Works on both roles (a client's own
// drop is client-authored, the same path local_streams uses for held props).
void ExpressReleasedHandActor(coop::net::Session& session, void* prev, int32_t prevIdx) {
    if (!prev || !R::IsLiveByIndex(prev, prevIdx)) return;  // stow/switch: destroyed in-hand
    if (coop::trash_collect_sync::EnsureHeldItemBroadcast(prev, &session)) {
        UE_LOGI("hand_item: released ex-hand actor %p expressed as a world prop "
                "(R-drop/place -- peers see it this tick)", prev);
    }
}

// Aprop_C 'name' FName property offset, resolved once (the field lives on the
// prop base class; every hotbar item is a descendant).
int32_t PropNameOffset() {
    static int32_t s_off = -1;
    if (s_off >= 0) return s_off;
    if (void* propCls = R::FindClass(L"prop_C"))
        s_off = R::FindPropertyOffset(propCls, L"name");
    return s_off;
}

std::wstring ReadItemName(void* actor) {
    const int32_t off = PropNameOffset();
    if (off < 0 || !actor) return {};
    R::FName n{};
    std::memcpy(&n, reinterpret_cast<const uint8_t*>(actor) + off, sizeof(n));
    std::wstring s = R::ToString(n);
    if (s == L"None") s.clear();
    return s;
}

// [u8 slot][u8 has][u8 clsLen][cls ascii][u8 nameLen][name ascii]
std::vector<uint8_t> BuildPayload(uint8_t slot, const SlotHand& h) {
    std::vector<uint8_t> out;
    const size_t cl = h.has ? (h.cls.size() > kMaxStr ? kMaxStr : h.cls.size()) : 0;
    const size_t nl = h.has ? (h.name.size() > kMaxStr ? kMaxStr : h.name.size()) : 0;
    out.reserve(4 + cl + nl);
    out.push_back(slot);
    out.push_back(h.has ? 1 : 0);
    out.push_back(static_cast<uint8_t>(cl));
    for (size_t i = 0; i < cl; ++i) out.push_back(static_cast<uint8_t>(h.cls[i]));
    out.push_back(static_cast<uint8_t>(nl));
    for (size_t i = 0; i < nl; ++i) out.push_back(static_cast<uint8_t>(h.name[i]));
    return out;
}

void SendState(coop::net::Session& session, uint8_t slot, const SlotHand& h) {
    const std::vector<uint8_t> p = BuildPayload(slot, h);
    if (session.role() == coop::net::Role::Host) {
        for (int x = 1; x < coop::net::kMaxPeers; ++x) {
            if (x == slot) continue;                 // never echo the originator
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, coop::net::ReliableKind::HandItem,
                                       p.data(), static_cast<int>(p.size()));
        }
    } else {
        session.SendReliableToSlot(0, coop::net::ReliableKind::HandItem,
                                   p.data(), static_cast<int>(p.size()));
    }
}

void DestroyMirror(uint8_t slot, const char* why) {
    Mirror& m = g_mirrors[slot];
    if (m.actor) {
        // Consume the SpawnMirror-time MarkIncomingSpawn (2026-07-10 audit): the
        // mirror's Init runs BP-internally, so the Init-POST observer that would
        // normally consume the mark never fires -- unconsumed marks accumulate to
        // the 256 cap (clear-on-cap wipes in-flight marks) and a stale mark at a
        // RECYCLED address makes the non-destructive Peek misclassify a real world
        // prop as an incoming echo. The mark is a loan; the mirror's only exit
        // repays it.
        coop::prop_echo_suppress::ConsumeIncomingSpawn(m.actor);
    }
    if (m.actor && R::IsLiveByIndex(m.actor, m.idx)) {
        // Suppress the K2_DestroyActor PRE observer's echo (host would
        // otherwise broadcast a PropDestroy for a display-only actor).
        coop::prop_echo_suppress::MarkIncomingDestroy(m.actor);
        E::DestroyActor(m.actor);
        UE_LOGI("hand_item: slot %u mirror destroyed (%s)", static_cast<unsigned>(slot), why);
    }
    m = Mirror{};
}

void SpawnMirror(uint8_t slot, void* puppetActor) {
    const SlotHand& want = g_hands[slot];
    void* cls = R::FindClass(want.cls.c_str());
    if (!cls) {
        // BP classes load on demand; if this peer's game never loaded the item
        // class, we retry each tick. Warn once per class to keep the log sane.
        static std::wstring sWarned;
        if (sWarned != want.cls) {
            sWarned = want.cls;
            UE_LOGW("hand_item: class '%ls' not found (unloaded?) -- will retry", want.cls.c_str());
        }
        return;
    }
    const ue_wrap::FVector loc = E::GetActorLocation(puppetActor);
    const ue_wrap::FRotator rot{};
    void* actor = E::BeginDeferredSpawn(cls, loc, rot);
    if (!actor) {
        UE_LOGW("hand_item: BeginDeferredSpawn '%ls' failed", want.cls.c_str());
        return;
    }
    // Stamp the item 'name' BEFORE Init runs (generic prop_C meshes are
    // name-driven, same pattern as the game's own updateHold spawn).
    if (!want.name.empty()) {
        const int32_t off = PropNameOffset();
        if (off >= 0) {
            R::FName n = ue_wrap::fname_utils::StringToFName(want.name);
            std::memcpy(reinterpret_cast<uint8_t*>(actor) + off, &n, sizeof(n));
        }
    }
    // Suppress the Init POST spawn-catch echo (host_spawn_watcher /
    // prop_lifecycle must never express a display mirror as a world prop).
    coop::prop_echo_suppress::MarkIncomingSpawn(actor);
    if (!E::FinishDeferredSpawn(actor, loc, rot)) {
        UE_LOGW("hand_item: FinishDeferredSpawn '%ls' failed", want.cls.c_str());
        return;
    }
    E::SetActorSimulatePhysics(actor, false);
    E::SetActorEnableCollision(actor, false);
    Mirror& m = g_mirrors[slot];
    m.actor = actor;
    m.idx = R::InternalIndexOf(actor);
    m.cls = want.cls;
    m.name = want.name;
    m.puppetIdx = R::InternalIndexOf(puppetActor);
    UE_LOGI("hand_item: slot %u mirror SPAWNED cls='%ls' name='%ls' (display-only, view-anchored)",
            static_cast<unsigned>(slot), want.cls.c_str(), want.name.c_str());
}

// Re-command the mirror to the puppet's VIEW-space hold point every tick (the
// puppet_carry_drive shape). Eye anchor + synced-look basis; the item rotates
// with the look so it reads exactly like the owner's bottom-right hold.
void DriveMirror(void* mirrorActor, coop::RemotePlayer* pup) {
    const ue_wrap::FVector eye = pup->GetHeadPosition();
    const ue_wrap::FVector fwd = pup->GetSyncedAimDirection();
    // right = up x fwd (UE left-handed basis: fwd (1,0,0) -> right (0,1,0));
    // degenerate near-vertical aim keeps the last-tick placement.
    ue_wrap::FVector right{-fwd.Y, fwd.X, 0.f};
    const float rl = std::sqrt(right.X * right.X + right.Y * right.Y);
    if (rl < 1e-3f) return;
    right.X /= rl; right.Y /= rl;
    // View-up, closed form for a yaw/pitch basis:
    // fwd = (cosP*cosY, cosP*sinY, sinP)  ->  up = (-sinP*cosY, -sinP*sinY, cosP).
    const float sinP = fwd.Z;
    const float cosP = rl;  // |xy| of a unit fwd == cos(pitch)
    const float cosY = fwd.X / cosP, sinY = fwd.Y / cosP;
    const ue_wrap::FVector up{-sinP * cosY, -sinP * sinY, cosP};
    const ue_wrap::FVector pos{
        eye.X + fwd.X * kViewForwardCm + right.X * kViewRightCm + up.X * kViewUpCm,
        eye.Y + fwd.Y * kViewForwardCm + right.Y * kViewRightCm + up.Y * kViewUpCm,
        eye.Z + fwd.Z * kViewForwardCm + right.Z * kViewRightCm + up.Z * kViewUpCm,
    };
    constexpr float kRadToDeg = 57.2957795f;
    const float yawDeg   = std::atan2(fwd.Y, fwd.X) * kRadToDeg;
    const float pitchDeg = std::atan2(sinP, cosP) * kRadToDeg;
    E::SetActorLocation(mirrorActor, pos);
    E::SetActorRotation(mirrorActor, ue_wrap::FRotator{pitchDeg, yawDeg, 0.f});
}

}  // namespace

void TickOwner(coop::net::Session& session, void* local, void* holdingProp) {
    g_session = &session;
    if (local && local != g_localPlayer) {
        g_localPlayer    = local;
        g_localPlayerIdx = R::InternalIndexOf(local);
    }
    if (!session.connected()) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (self >= coop::players::kMaxPeers) return;

    if (holdingProp) {
        g_ownEmptyStreak = 0;
        // O(1) steady-state: same live actor as last tick -> skip the renders,
        // EXCEPT a slow re-check every 30 ticks (~0.5 s) — the game renames a
        // held prop IN PLACE (grenade arm: 'name' -> grenade_1), which a pure
        // pointer latch would never see. 2 renders/s vs 120/s steady-state.
        static uint32_t sSameStreak = 0;
        const int32_t idx = R::InternalIndexOf(holdingProp);
        if (holdingProp == g_ownHeldPtr && idx == g_ownHeldIdx &&
            (++sSameStreak % 30) != 0) {
            return;
        }
        if (holdingProp != g_ownHeldPtr && g_ownHeldPtr) {
            // Identity CHANGED with a previous actor latched: if the previous hand
            // actor survived, it was released to the world (place-then-next-slot).
            ExpressReleasedHandActor(session, g_ownHeldPtr, g_ownHeldIdx);
        }
        g_ownHeldPtr = holdingProp;
        g_ownHeldIdx = idx;
        const std::wstring cls = R::ClassNameOf(holdingProp);
        const std::wstring name = ReadItemName(holdingProp);
        if (!g_ownHas || cls != g_ownCls || name != g_ownName) {
            g_ownHas = true;
            g_ownCls = cls;
            g_ownName = name;
            SlotHand& h = g_hands[self];
            h.has = true; h.cls = cls; h.name = name;
            SendState(session, self, h);
            UE_LOGI("hand_item: local hand -> cls='%ls' name='%ls' (announced)",
                    cls.c_str(), name.c_str());
            // (v106: no reconcile request here -- the R-pickup's ground-actor death
            // is caught at the K2_DestroyActor Func seam the moment it happens.)
        }
    } else if (g_ownHas) {
        // The hand is empty this tick: if the ex-hand actor survived the edge it
        // was RELEASED into the world (R-drop / place) -- express it now; then
        // drop the identity latch so a re-appear re-renders + re-compares.
        ExpressReleasedHandActor(session, g_ownHeldPtr, g_ownHeldIdx);
        g_ownHeldPtr = nullptr;
        g_ownHeldIdx = -1;
        // Debounce the updateHold switch flicker: only a PERSISTENT null is a stow.
        if (++g_ownEmptyStreak >= kEmptyDebounceTicks) {
            g_ownHas = false;
            g_ownCls.clear();
            g_ownName.clear();
            SlotHand& h = g_hands[self];
            h = SlotHand{};
            SendState(session, self, h);
            UE_LOGI("hand_item: local hand -> EMPTY (announced)");
            // (v106: no reconcile request here -- the released world actor was
            // expressed at the hand edge above, and inventory-path spawns ride
            // the FinishSpawningActor Func seam.)
        }
    }
}

void* LocalHandActor() {
    if (!g_localPlayer || !R::IsLiveByIndex(g_localPlayer, g_localPlayerIdx))
        return nullptr;
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(g_localPlayer, gs)) return nullptr;
    void* ha = gs.holdingActor;
    if (!ha || !R::IsLive(ha)) return nullptr;
    // Same routing predicate as local_streams: only an Aprop_C descendant in
    // holding_actor is the hotbar hand (the clump/pile morph carry is a world
    // entity and MUST stay adoptable).
    if (!ue_wrap::prop::IsDescendantOfProp(ha)) return nullptr;
    return ha;
}

size_t CollectHandAxisActors(void* out[], size_t cap) {
    size_t n = 0;
    if (void* lh = LocalHandActor(); lh && n < cap) out[n++] = lh;
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers && n < cap; ++slot) {
        const Mirror& m = g_mirrors[slot];
        // IsLiveByIndex, not a bare pointer match: a dead mirror's recycled
        // address belongs to a DIFFERENT actor that must stay adoptable.
        if (m.actor && R::IsLiveByIndex(m.actor, m.idx)) out[n++] = m.actor;
    }
    return n;
}

bool IsHandAxisActor(void* actor) {
    if (!actor) return false;
    void* axis[1 + coop::players::kMaxPeers];
    const size_t n = CollectHandAxisActors(axis, 1 + coop::players::kMaxPeers);
    for (size_t i = 0; i < n; ++i)
        if (axis[i] == actor) return true;
    return false;
}

void TickMirrors() {
    auto& reg = coop::players::Registry::Get();
    const uint8_t self = reg.LocalPeerId();
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (slot == self) continue;  // never mirror our own hand
        const SlotHand& want = g_hands[slot];
        Mirror& m = g_mirrors[slot];
        const bool mirrorLive = m.actor && R::IsLiveByIndex(m.actor, m.idx);

        if (!want.has) {
            if (mirrorLive || m.actor) DestroyMirror(slot, "hand now empty");
            continue;
        }
        coop::RemotePlayer* pup = reg.Puppet(slot);
        void* puppetActor = pup ? pup->GetActor() : nullptr;
        if (!puppetActor || !R::IsLive(puppetActor)) {
            // Puppet not up yet (join window) or torn down: keep the state,
            // drop any orphaned mirror, retry next tick.
            if (mirrorLive || m.actor) DestroyMirror(slot, "puppet gone");
            continue;
        }
        const int32_t pupIdx = R::InternalIndexOf(puppetActor);
        if (mirrorLive && m.cls == want.cls && m.name == want.name &&
            m.puppetIdx == pupIdx) {
            DriveMirror(m.actor, pup);  // steady state: re-command the view hold
            continue;
        }
        if (mirrorLive || m.actor) DestroyMirror(slot, "state/puppet changed");
        SpawnMirror(slot, puppetActor);
        Mirror& nm = g_mirrors[slot];
        if (nm.actor) DriveMirror(nm.actor, pup);  // no first-frame pop at spawn pos
    }
}

bool HandleHandItem(coop::net::Session& session,
                    const coop::net::Session::ReliableMessage& msg) {
    UE_ASSERT_GAME_THREAD("HandleHandItem");
    if (msg.payloadLen < 4) {
        UE_LOGW("hand_item: payload %zu B too short -- dropping",
                static_cast<size_t>(msg.payloadLen));
        return true;
    }
    const uint8_t* p = msg.payload;
    size_t off = 0;
    const uint8_t describedSlot = p[off++];
    const bool has = p[off++] != 0;
    const uint8_t clsLen = p[off++];
    if (off + clsLen + 1 > static_cast<size_t>(msg.payloadLen)) {
        UE_LOGW("hand_item: truncated class field -- dropping");
        return true;
    }
    std::wstring cls;
    for (uint8_t i = 0; i < clsLen; ++i) cls.push_back(static_cast<wchar_t>(p[off++]));
    const uint8_t nameLen = p[off++];
    if (off + nameLen > static_cast<size_t>(msg.payloadLen)) {
        UE_LOGW("hand_item: truncated name field -- dropping");
        return true;
    }
    std::wstring name;
    for (uint8_t i = 0; i < nameLen; ++i) name.push_back(static_cast<wchar_t>(p[off++]));

    if (describedSlot >= coop::net::kMaxPeers) return true;
    if (has && cls.empty()) return true;  // malformed: "has item" with no class

    if (session.role() == coop::net::Role::Host) {
        // Forgery guard: a client may only describe ITS OWN hand.
        if (msg.senderPeerSlot != describedSlot || describedSlot == 0) {
            UE_LOGW("hand_item: slot=%u from senderSlot=%d -- forged, dropping",
                    static_cast<unsigned>(describedSlot), msg.senderPeerSlot);
            return true;
        }
        SlotHand& h = g_hands[describedSlot];
        h.has = has; h.cls = cls; h.name = name;
        // Rebroadcast to every other ready client (originator excluded).
        const std::vector<uint8_t> out = BuildPayload(describedSlot, h);
        for (int x = 1; x < coop::net::kMaxPeers; ++x) {
            if (x == describedSlot) continue;
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, coop::net::ReliableKind::HandItem,
                                       out.data(), static_cast<int>(out.size()));
        }
        return true;
    }

    // Client: only the host relays hand states.
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("hand_item: HandItem from non-host senderPeerSlot=%d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    if (describedSlot == coop::players::Registry::Get().LocalPeerId()) return true;  // own echo
    SlotHand& h = g_hands[describedSlot];
    h.has = has; h.cls = cls; h.name = name;
    return true;
}

void ReplayPeerStatesToSlot(int slot) {
    coop::net::Session* session = g_session;
    if (!session || session->role() != coop::net::Role::Host) return;
    if (slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    for (uint8_t s = 0; s < coop::players::kMaxPeers; ++s) {
        if (s == slot) continue;
        if (!g_hands[s].has) continue;  // empty is the joiner's default -- nothing to send
        const std::vector<uint8_t> p = BuildPayload(s, g_hands[s]);
        session->SendReliableToSlot(slot, coop::net::ReliableKind::HandItem,
                                    p.data(), static_cast<int>(p.size()));
        UE_LOGI("hand_item: connect-replay -- slot %u holds '%ls' -> joiner slot %d",
                static_cast<unsigned>(s), g_hands[s].cls.c_str(), slot);
    }
}

void Reset() {
    for (uint8_t s = 0; s < coop::players::kMaxPeers; ++s) {
        DestroyMirror(s, "session reset");
        g_hands[s] = SlotHand{};
    }
    g_ownHas = false;
    g_ownCls.clear();
    g_ownName.clear();
    g_ownEmptyStreak = 0;
    g_ownHeldPtr = nullptr;
    g_ownHeldIdx = -1;
    // Cached engine pointers die with the session too (2026-07-10 audit LOW):
    // a stale g_localPlayer would satisfy IsLiveByIndex checks across a
    // world reload only by luck; TickOwner re-caches on the next session.
    g_localPlayer    = nullptr;
    g_localPlayerIdx = -1;
    g_session        = nullptr;
}

void OnSlotDisconnected(uint8_t slot) {
    if (slot >= coop::players::kMaxPeers) return;
    DestroyMirror(slot, "peer disconnected");
    g_hands[slot] = SlotHand{};
}

}  // namespace coop::hand_item
