// coop/player/hand_item.cpp -- see coop/player/hand_item.h for the design.

#include "coop/player/hand_item.h"

#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/trash_collect_sync.h"  // EnsureHeldItemBroadcast (hand-edge world release)
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/puppet.h"  // GetSkeletalMeshComponent (owner head-bone anchor for the hold measure)
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

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

// Display placement (user 2026-07-10: "attached to the root of the camera --
// same as the SP local player: the item just appears in front of the camera,
// but seen from third person on the puppet"). Natively the held item is
// welded into the FP-arms VIEWMODEL chain (updateHold K2_AttachToComponent ->
// 'weapon' <- arms <- arms_lag <- viewmodel, mainPlayer SCS) which is drawn
// camera-relative -- that chain is bHiddenInGame and never ticks on a puppet
// (ref-pose socket put the mirror on the BACK, hands-on 2026-07-06 14:2x),
// and the puppet's Camera component never pitches (pitch is controller-fed).
// So the SP look is reproduced by the per-tick drive: eye anchor +
// synced-look basis, following yaw AND pitch -- functionally "attached to
// the camera root".
//
// MEASURED, NOT TUNED (user 2026-07-11: keljoy face pointed away on the
// puppet -- hand-tuned constants can never carry the per-item weld pose):
// the item's pose relative to the camera is ITEM-SPECIFIC (the 'weapon'
// socket basis + per-item in-hand offsets), so the OWNER MEASURES its held
// actor's transform in its own view space and ships it with the HandItem
// announce; DriveMirror re-composes it onto the puppet's synced view basis.
// The two view-space definitions match by construction: origin = 'head' bone
// + kHeadAnchorLiftCm on BOTH sides, basis = control yaw/pitch with roll 0
// (the pose stream's curYaw+headYawDelta == control yaw, curPitch == control
// pitch -- local_streams.cpp). Every item then reads exactly like the
// owner's first-person hold, face direction included.
constexpr float kHeadAnchorLiftCm = 33.f;  // == remote_player.cpp kPlateLiftCm (GetHeadPosition's lift)
// Fallback placement until the owner's first measure lands (mesh not ticked
// yet / measure declined): the pre-2026-07-11 camera-front constants.
constexpr float kFallbackRelPos[3] = {45.f, 10.f, -40.f};  // fwd/right/up cm

// ---- per-slot wire state (what each peer's hand holds) -------------------
struct SlotHand {
    bool has = false;
    std::wstring cls;   // BP class name, e.g. L"prop_physgun_C"
    std::wstring name;  // Aprop_C 'name' FName (drives the generic prop mesh), may be empty
    // Measured view-relative hold transform (owner-authored, wire-carried).
    float relPos[3] = {kFallbackRelPos[0], kFallbackRelPos[1], kFallbackRelPos[2]};
    float relRot[3] = {0.f, 0.f, 0.f};  // item rotator {pitch,yaw,roll} in the view frame
    bool  haveRel = false;              // owner measured (vs the fallback defaults)
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
    if (!prev || !R::IsLiveByIndex(prev, prevIdx)) {
        // [ROCK-DROP DIAG 2026-07-08, RULE-2-exempt] prev DEAD at the hand edge => the
        // ex-hand actor was DESTROYED in-hand (updateHold stow/switch OR the R-drop's
        // simulateDrop which destroys the hand display actor + spawns a SEPARATE fresh
        // world actor). Either way this "release the SAME surviving actor" path does NOT
        // fire -- confirming the v106 "R-drop is not a spawn" model is false for this item.
        // Do NOT deref prev here (UAF on a dead actor); log the pointer only.
        if (prev) {
            UE_LOGI("[ROCK-DROP] hand-edge: ex-hand actor %p DEAD (destroyed in-hand) -- the "
                    "'release surviving actor' express does NOT run; a fresh world spawn (if any) "
                    "is authored elsewhere (client Aprop spawns are host-auth-skipped)", prev);
        }
        return;
    }
    // prev SURVIVED the hand edge => a genuine same-actor release into the world.
    if (coop::trash_collect_sync::EnsureHeldItemBroadcast(prev, &session)) {
        UE_LOGI("hand_item: released ex-hand actor %p expressed as a world prop "
                "(R-drop/place -- peers see it this tick)", prev);
    } else {
        UE_LOGI("[ROCK-DROP] hand-edge: ex-hand actor %p SURVIVED but EnsureHeldItemBroadcast "
                "DECLINED it (see the decline reason above) -- not expressed", prev);
    }
}

// ---- view-space rotation math (UE FRotationMatrix conventions) -------------
constexpr float kDeg2Rad  = 0.01745329252f;
constexpr float kRadToDeg = 57.2957795f;

// UE FRotationMatrix: rows = the rotation's world-space X (forward), Y (right),
// Z (up) axes. A roll-0 call yields the view basis {fwd, right, up} directly.
void RotMatrixRows(float pitchDeg, float yawDeg, float rollDeg, float M[3][3]) {
    const float SP = std::sin(pitchDeg * kDeg2Rad), CP = std::cos(pitchDeg * kDeg2Rad);
    const float SY = std::sin(yawDeg * kDeg2Rad),   CY = std::cos(yawDeg * kDeg2Rad);
    const float SR = std::sin(rollDeg * kDeg2Rad),  CR = std::cos(rollDeg * kDeg2Rad);
    M[0][0] = CP * CY;                   M[0][1] = CP * SY;                   M[0][2] = SP;
    M[1][0] = SR * SP * CY - CR * SY;    M[1][1] = SR * SP * SY + CR * CY;    M[1][2] = -SR * CP;
    M[2][0] = -(CR * SP * CY + SR * SY); M[2][1] = CY * SR - CR * SP * SY;    M[2][2] = CR * CP;
}

// Inverse of RotMatrixRows (UE FMatrix::Rotator()): axis rows -> {Pitch,Yaw,Roll}.
ue_wrap::FRotator RotatorFromRows(const float M[3][3]) {
    ue_wrap::FRotator r{};
    r.Pitch = std::atan2(M[0][2], std::sqrt(M[0][0] * M[0][0] + M[0][1] * M[0][1])) * kRadToDeg;
    r.Yaw   = std::atan2(M[0][1], M[0][0]) * kRadToDeg;
    float S[3][3];
    RotMatrixRows(r.Pitch, r.Yaw, 0.f, S);  // roll-0 frame; roll = Z/Y vs its right axis
    const float zy = M[2][0] * S[1][0] + M[2][1] * S[1][1] + M[2][2] * S[1][2];
    const float yy = M[1][0] * S[1][0] + M[1][1] * S[1][1] + M[1][2] * S[1][2];
    r.Roll = std::atan2(zy, yy) * kRadToDeg;
    return r;
}

// Measure the OWNER's natively-welded held actor in its own view space (see the
// placement block comment). False = don't ship (mesh/bone not readable yet, or
// a wild pre-weld transform) -- the 0.5 s re-check retries.
bool MeasureLocalHoldRelative(void* local, void* held, float outPos[3], float outRot[3]) {
    void* mesh = ue_wrap::puppet::GetSkeletalMeshComponent(local);
    ue_wrap::FVector head{};
    if (!mesh || !R::IsLive(mesh) ||
        !E::GetBoneWorldLocationByName(mesh, L"head", head))
        return false;
    head.Z += kHeadAnchorLiftCm;
    void* ctl = E::GetController(local);
    const ue_wrap::FRotator view =
        ctl ? E::GetControlRotation(ctl) : E::GetActorRotation(local);
    float V[3][3];
    RotMatrixRows(ue_wrap::NormalizeAxis(view.Pitch), ue_wrap::NormalizeAxis(view.Yaw), 0.f, V);
    const ue_wrap::FVector iloc = E::GetActorLocation(held);
    const float d[3] = {iloc.X - head.X, iloc.Y - head.Y, iloc.Z - head.Z};
    for (int i = 0; i < 3; ++i) {
        outPos[i] = d[0] * V[i][0] + d[1] * V[i][1] + d[2] * V[i][2];
        // A held item sits within arm's reach of the head; a wild read (the
        // pre-weld spawn frame, a dying component) must never ship.
        if (!(outPos[i] > -300.f && outPos[i] < 300.f)) return false;
    }
    float I[3][3], Q[3][3];
    const ue_wrap::FRotator irot = E::GetActorRotation(held);
    RotMatrixRows(irot.Pitch, irot.Yaw, irot.Roll, I);
    for (int r = 0; r < 3; ++r)      // item axis r expressed in the view frame
        for (int c = 0; c < 3; ++c)
            Q[r][c] = I[r][0] * V[c][0] + I[r][1] * V[c][1] + I[r][2] * V[c][2];
    const ue_wrap::FRotator rel = RotatorFromRows(Q);
    outRot[0] = rel.Pitch; outRot[1] = rel.Yaw; outRot[2] = rel.Roll;
    return true;
}

// (The 0.5 s reliable drift refresh was retired 2026-07-11 same-day, RULE 2:
// the v109 MsgType::HandPose unreliable stream carries the live transform per
// tick -- the swing motion the drift refresh rendered at "1 frame per second",
// user report. The reliable announce keeps the identity + initial transform.)

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
// + when has: [f32 relPos x3][f32 relRot x3] (the measured view-relative hold).
std::vector<uint8_t> BuildPayload(uint8_t slot, const SlotHand& h) {
    std::vector<uint8_t> out;
    const size_t cl = h.has ? (h.cls.size() > kMaxStr ? kMaxStr : h.cls.size()) : 0;
    const size_t nl = h.has ? (h.name.size() > kMaxStr ? kMaxStr : h.name.size()) : 0;
    out.reserve(4 + cl + nl + (h.has ? 24 : 0));
    out.push_back(slot);
    out.push_back(h.has ? 1 : 0);
    out.push_back(static_cast<uint8_t>(cl));
    for (size_t i = 0; i < cl; ++i) out.push_back(static_cast<uint8_t>(h.cls[i]));
    out.push_back(static_cast<uint8_t>(nl));
    for (size_t i = 0; i < nl; ++i) out.push_back(static_cast<uint8_t>(h.name[i]));
    if (h.has) {
        const float rel[6] = {h.relPos[0], h.relPos[1], h.relPos[2],
                              h.relRot[0], h.relRot[1], h.relRot[2]};
        const size_t base = out.size();
        out.resize(base + sizeof(rel));
        std::memcpy(out.data() + base, rel, sizeof(rel));
    }
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
// puppet_carry_drive shape): rebuild the roll-0 view basis from the synced aim
// and re-compose the owner-measured relative transform onto it. With the
// fallback rel (haveRel=false) this degrades to the legacy camera-front
// placement with the item facing along the look.
void DriveMirror(void* mirrorActor, coop::RemotePlayer* pup, const SlotHand& hand) {
    const ue_wrap::FVector eye = pup->GetHeadPosition();
    const ue_wrap::FVector fwd = pup->GetSyncedAimDirection();
    // Degenerate near-vertical aim keeps the last-tick placement.
    const float rl = std::sqrt(fwd.X * fwd.X + fwd.Y * fwd.Y);
    if (rl < 1e-3f) return;
    const float yawDeg   = std::atan2(fwd.Y, fwd.X) * kRadToDeg;
    const float pitchDeg = std::atan2(fwd.Z, rl) * kRadToDeg;
    float V[3][3];
    RotMatrixRows(pitchDeg, yawDeg, 0.f, V);  // rows: fwd / right / up
    const float* rp = hand.relPos;
    const ue_wrap::FVector pos{
        eye.X + V[0][0] * rp[0] + V[1][0] * rp[1] + V[2][0] * rp[2],
        eye.Y + V[0][1] * rp[0] + V[1][1] * rp[1] + V[2][1] * rp[2],
        eye.Z + V[0][2] * rp[0] + V[1][2] * rp[1] + V[2][2] * rp[2],
    };
    // World item axes: row i of the composed matrix = sum_c Q[i][c] * V[c].
    float Q[3][3], W[3][3];
    RotMatrixRows(hand.relRot[0], hand.relRot[1], hand.relRot[2], Q);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            W[i][j] = Q[i][0] * V[0][j] + Q[i][1] * V[1][j] + Q[i][2] * V[2][j];
    E::SetActorLocation(mirrorActor, pos);
    E::SetActorRotation(mirrorActor, RotatorFromRows(W));
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
        // v109: measure + latch the live view-relative hold transform EVERY tick
        // while holding -- the net thread streams it at sendHz (MsgType::HandPose),
        // so a melee swing reads as continuous motion on the puppet mirror. Same
        // per-tick engine-read cost class as the local pose capture. Runs BEFORE
        // the identity-latch early-return below: motion must flow even when the
        // held identity is unchanged.
        {
            float sp[3], sr[3];
            if (MeasureLocalHoldRelative(local, holdingProp, sp, sr)) {
                coop::net::HandPoseSnapshot hp{};
                std::memcpy(hp.relPos, sp, sizeof(sp));
                std::memcpy(hp.relRot, sr, sizeof(sr));
                session.SetLocalHandPose(true, hp);
            }
        }
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
            // First measure of the native weld's view-relative transform (may land
            // a tick early -- the 0.5 s re-check below self-corrects once settled).
            // Measure into scratch and RESET on failure (audit 2026-07-11 M/H): on a
            // quick-slot switch a failed measure would otherwise ship the PREVIOUS
            // item's transform under the new item's name -- plausible values the
            // receiver's sanity check cannot reject.
            float p[3], rr[3];
            if (MeasureLocalHoldRelative(local, holdingProp, p, rr)) {
                std::memcpy(h.relPos, p, sizeof(p));
                std::memcpy(h.relRot, rr, sizeof(rr));
                h.haveRel = true;
            } else {
                h.relPos[0] = kFallbackRelPos[0];
                h.relPos[1] = kFallbackRelPos[1];
                h.relPos[2] = kFallbackRelPos[2];
                h.relRot[0] = h.relRot[1] = h.relRot[2] = 0.f;
                h.haveRel = false;
            }
            SendState(session, self, h);
            UE_LOGI("hand_item: local hand -> cls='%ls' name='%ls' (announced, rel %s)",
                    cls.c_str(), name.c_str(), h.haveRel ? "measured" : "fallback");
            // (v106: no reconcile request here -- the R-pickup's ground-actor death
            // is caught at the K2_DestroyActor Func seam the moment it happens.)
            // (v109: no reliable drift refresh -- the HandPose stream above carries
            // the live transform; the announce rel is only the pre-stream frame.)
        }
    } else if (g_ownHas) {
        session.SetLocalHandPose(false, {});  // v109: stop the stream on the empty edge
        // The hand is empty this tick: if the ex-hand actor survived the edge it
        // was RELEASED into the world (R-drop / place) -- express it now; then
        // drop the identity latch so a re-appear re-renders + re-compares.
        ExpressReleasedHandActor(session, g_ownHeldPtr, g_ownHeldIdx);
        g_ownHeldPtr = nullptr;
        g_ownHeldIdx = -1;
        // Stow announce is EDGE-INSTANT (user 2026-07-06 per rule 1): the bytecode
        // proves a quick-slot switch is ONE synchronous updateHold call (destroy
        // @935 -> spawn @1023 -> holding_name @3183), so a poll never observes a
        // mid-switch null -- no debounce needed. (A 15-tick debounce guarded a
        // flicker that does not exist and made the stowed item linger a quarter
        // second; its 1-tick residue was pure dust -- retired 2026-07-10.)
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
        // v109: the freshest STREAMED hold transform overrides the announce-time
        // one (newest-wins; the announce rel only covers the pre-stream frame).
        if (g_session) {
            coop::net::HandPoseSnapshot hp;
            bool isNew = false;
            if (g_session->TryGetRemoteHandPose(slot, hp, &isNew) && isNew) {
                SlotHand& wh = g_hands[slot];
                std::memcpy(wh.relPos, hp.relPos, sizeof(hp.relPos));
                std::memcpy(wh.relRot, hp.relRot, sizeof(hp.relRot));
                wh.haveRel = true;
            }
        }
        const int32_t pupIdx = R::InternalIndexOf(puppetActor);
        if (mirrorLive && m.cls == want.cls && m.name == want.name &&
            m.puppetIdx == pupIdx) {
            DriveMirror(m.actor, pup, want);  // steady state: re-command the view hold
            continue;
        }
        if (mirrorLive || m.actor) DestroyMirror(slot, "state/puppet changed");
        SpawnMirror(slot, puppetActor);
        Mirror& nm = g_mirrors[slot];
        if (nm.actor) DriveMirror(nm.actor, pup, want);  // no first-frame pop at spawn pos
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

    // Measured view-relative hold transform (present whenever has). Insane values
    // (non-finite / out of arm's reach) keep the fallback placement.
    float relPos[3] = {kFallbackRelPos[0], kFallbackRelPos[1], kFallbackRelPos[2]};
    float relRot[3] = {0.f, 0.f, 0.f};
    if (has) {
        if (off + 24 > static_cast<size_t>(msg.payloadLen)) {
            UE_LOGW("hand_item: missing hold-transform field -- dropping");
            return true;
        }
        float rel[6];
        std::memcpy(rel, p + off, sizeof(rel));
        off += sizeof(rel);
        bool sane = true;
        for (int i = 0; i < 6; ++i) if (!std::isfinite(rel[i])) sane = false;
        for (int i = 0; i < 3; ++i) if (!(rel[i] > -300.f && rel[i] < 300.f)) sane = false;
        if (sane) {
            relPos[0] = rel[0]; relPos[1] = rel[1]; relPos[2] = rel[2];
            relRot[0] = rel[3]; relRot[1] = rel[4]; relRot[2] = rel[5];
        }
    }

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
        std::memcpy(h.relPos, relPos, sizeof(relPos));
        std::memcpy(h.relRot, relRot, sizeof(relRot));
        h.haveRel = has;
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
    std::memcpy(h.relPos, relPos, sizeof(relPos));
    std::memcpy(h.relRot, relRot, sizeof(relRot));
    h.haveRel = has;
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
    // v109: stop the hand-pose stream before the session cache drops -- a stale
    // hasLocalHand_ latch would otherwise stream the dead session's transform
    // into the next one until the first hold updates it.
    if (g_session) g_session->SetLocalHandPose(false, {});
    for (uint8_t s = 0; s < coop::players::kMaxPeers; ++s) {
        DestroyMirror(s, "session reset");
        g_hands[s] = SlotHand{};
    }
    g_ownHas = false;
    g_ownCls.clear();
    g_ownName.clear();
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
