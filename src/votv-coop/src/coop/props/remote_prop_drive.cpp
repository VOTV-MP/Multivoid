// coop/props/remote_prop_drive.cpp -- the per-slot drive cache and hold gate of the PropPose
// receiver, and the drive start (ResolveAndStartDrive). The pose Tick, OnRelease and the
// teardown stay in remote_prop.cpp; the seam between the two is remote_prop_drive.h.

#include "coop/props/remote_prop.h"
#include "remote_prop_drive.h"  // impl-private (src-local), NOT under include/

#include "coop/dev/swinger_drill.h"     // NotePropUnresolved
#include "coop/interactables/interactable_sync.h"  // ContainerActor: a dragged swinger's key to its actor
#include "coop/interactables/swinger_join_pose.h"   // Forget: a stream or release supersedes a parked connect pose
#include "coop/props/active_drive.h"   // the fixed-delay snapshot interp, shared with the trash carry stream
#include "coop/props/prop_sound.h"
#include "coop/props/trash_mirror.h"   // WeMade
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_stream.h"  // IsParked: the host's channel owns a parked copy
#include "coop/props/prop_park.h"  // Park / Unpark: a held prop, and a Character welded on it
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_key_index.h"  // a key on the wire to a live actor
#include "coop/props/prop_stick_sync.h"  // the stuck wall-attachable gates
#include "coop/props/unresolved_pose_ledger.h"  // a pose-before-spawn race vs a sustained identity gap
#include "coop/props/trash_channel.h"  // the per-eid sync-time context; stale carry and release drops
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <array>
#include <cstring>
#include <string>

namespace coop::remote_prop {

namespace R = ue_wrap::reflection;

namespace {
// ActiveDrive, BeginLerpToPose, AdvanceLerp, ResetDriveState, LerpAngle, NowMs and the lerp
// constants come from coop/props/active_drive.h.
using namespace coop::active_drive;
}  // namespace

std::array<coop::active_drive::ActiveDrive, coop::players::kMaxPeers> g_drives{};
std::array<HoldGate, coop::players::kMaxPeers> g_holds{};

// Wrapping order: `a` was minted after `b`.
bool GenAfter(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) > 0; }

bool IsHoldClosed(const HoldGate& h, uint16_t gen) {
    return gen != 0 && h.haveClosed && !GenAfter(gen, h.closedGen);
}

// A drive on `actor` belonging to a slot other than `slot` (-1 excludes none).
bool IsActorDrivenByOtherThan(void* actor, int slot) {
    UE_ASSERT_GAME_THREAD("remote_prop::IsActorDrivenByOtherThan");
    if (!actor) return false;
    for (int s = 0; s < static_cast<int>(coop::players::kMaxPeers); ++s)
        if (s != slot && g_drives[s].actor == actor) return true;
    return false;
}

// True when some slot's drive targets `actor`; the spawn receiver skips the transform converge
// for a driven prop (PropPose owns its position).
bool IsActorUnderAnyDrive(void* actor) {
    // g_drives is game-thread only, by assertion rather than a mutex.
    UE_ASSERT_GAME_THREAD("g_drives (IsActorUnderAnyDrive)");
    if (!actor) return false;
    for (const auto& d : g_drives) {
        if (d.actor == actor) return true;
    }
    return false;
}

bool IsOwnedHere(void* actor) {
    if (!actor) return false;
    if (IsActorUnderAnyDrive(actor) || coop::prop_drive_stream::IsParked(actor)) return true;
    void* const localPlayer = coop::players::Registry::Get().Local();
    return localPlayer && ue_wrap::engine::IsMainPlayerGrabbing(localPlayer, actor);
}

// The WireKey as a wstring, for the key index.
std::wstring KeyToWString(const coop::net::WireKey& k) {
    std::wstring s;
    s.reserve(k.len);
    for (uint8_t i = 0; i < k.len && i < 31; ++i) {
        s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(k.data[i])));
    }
    return s;
}

// The WireKey against the drive's cached key; keys are ASCII, so a byte compare is exact.
bool KeyMatchesCache(int slot, const coop::net::WireKey& k) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return false;
    const auto& d = g_drives[slot];
    if (k.len != d.lastKey.size()) return false;
    return std::memcmp(k.data, d.lastKey.data(), k.len) == 0;
}

// Every release-shaped physics re-enable (PropRelease, the stream-stop timeout, the switched-prop
// release) is gated on the stick state: a wall-attachable that stuck while held stays frozen when
// the sender's hold breaks, or the host watches the camera fall off the wall.
// A trash body whose pose the HOST authors on this peer: a mirror we spawned, or this client's own
// save-loaded clump bound to the host's eid (parked at the bind: tick and physics off). Neither
// goes back to local physics when a drive ends -- a stream gap, the peer's hand moving on, a
// release: it would roll and be pushed on this peer alone. A bound PILE is not in this set: it is
// a Static, inert actor, and the 500 ms release is what un-sticks it if a reliable edge is lost.
bool HostAuthorsTrashBody(void* actor) {
    if (!actor) return false;
    if (coop::trash_mirror::WeMade(actor)) return true;
    return ue_wrap::prop::IsGarbageClump(actor) && coop::prop_element_tracker::IsBoundMirrorNative(actor);
}

bool StickHoldsPhysicsOff(void* actor) {
    return actor && ue_wrap::prop::IsDescendantOfProp(actor) &&
           (ue_wrap::prop::IsFrozen(actor) || ue_wrap::prop::IsStatic(actor));
}

// A door this peer's own player drags is moved by that drag on this copy, which wins at drive start and at every
// pose after it: a swinger has no eid and no arbiter. Said once per hold.
bool LocalDragsDoor(void* door, int slot, uint16_t gen) {
    void* const own = coop::players::Registry::Get().Local();
    // The grab compare is the cheap test and is false for nearly every held prop, so it goes first.
    if (!own || !ue_wrap::engine::IsMainPlayerGrabbing(own, door) || !ue_wrap::swinger::IsSwinger(door)) return false;
    if (g_holds[slot].refusedGen != gen)
        UE_LOGI("remote_prop: slot %d hold %d streams a door this peer drags -- not driven", slot, gen);
    g_holds[slot].refusedGen = gen;
    return true;
}

void* ResolveKeyedActor(const std::wstring& keyW) {
    // A dragged swinger streams under the container lane's key, and as a child actor it is not in the prop
    // key index. The portable token is minted only by element::PortableWireKey ("mv_" + hex,
    // portable_identity.cpp), and the other keys this mod mints start differently (cs_ and rk_ in
    // prop_synth_key.cpp, mvhook_ in hook_anchor.cpp), so a key that starts with it is not one we made for a
    // prop and skips that index, whose miss runs a catch-up drain.
    // A game Key is the game's own and could in principle start with it; the skip trades that for the miss's drain.
    if (keyW.rfind(L"mv_", 0) == 0) return coop::interactable_sync::ContainerActor(keyW);
    void* prop = coop::prop_key_index::Find(keyW);
    return prop ? prop : coop::interactable_sync::ContainerActor(keyW);
}

void ResolveAndStartDrive(int slot, const coop::net::PropPoseSnapshot& pose) {
    // A trash carry pose is held unless its ctx is the entity's current generation: a pose ahead of
    // its convert would drive the pre-convert rendering, a stale one after a re-pile or throw would
    // drive the settled pile to the dead clump position. ctx 0 or an unknown eid (a keyed prop) is
    // always fresh.
    if (pose.elementId != 0 &&
        !coop::trash_channel::IsInboundStreamCtxFresh(pose.elementId, pose.ctx, /*requireCurrentGen=*/true)) {
        // One line per (eid, ctx) burst.
        static uint32_t s_lastDropEid = 0; static uint8_t s_lastDropCtx = 0;
        if (pose.elementId != s_lastDropEid || pose.ctx != s_lastDropCtx) {
            UE_LOGI("[PILE] CLIENT HOLD carry pose eid=%u ctx=%u known=%u (not E's current generation -- either "
                    "AHEAD of its convert (would drive the pre-convert OLD rendering -> pile-jump + the triple "
                    "grab-cue) or STALE after a later transition; applies once the matching convert lands. "
                    "known=0 => the ToClump convert was NEVER adopted, so EVERY carry pose holds forever)",
                    pose.elementId, static_cast<unsigned>(pose.ctx),
                    static_cast<unsigned>(coop::trash_channel::CtxForEid(pose.elementId)));
            s_lastDropEid = pose.elementId; s_lastDropCtx = pose.ctx;
        }
        return;
    }
    const std::wstring keyW = KeyToWString(pose.key);
    // Key first (an Aprop_C), then eid (the clump streams key None).
    void* prop = (!keyW.empty() && keyW != L"None") ? ResolveKeyedActor(keyW) : nullptr;
    if (!prop && pose.elementId != 0)
        prop = ResolveLiveActorByEid(pose.elementId);
    if (!prop) {
        coop::dev::swinger_drill::NotePropUnresolved(slot, pose);
        // The ledger separates the ordinary pose-before-spawn race from a sustained stream this
        // peer never got a spawn for: one WARN per sustained stream, and the INFO once per burst, not per packet.
        bool newBurst = false;
        if (coop::unresolved_pose_ledger::Note(slot, keyW, pose.elementId, NowMs(), &newBurst)) {
            UE_LOGW("remote_prop: slot %d key '%ls' eid=%u -- SUSTAINED unresolved pose stream "
                    "(>=%u packets over >=%llu ms). This is NOT the ordinary pose-before-spawn "
                    "race: the sender is streaming an item this peer never received a spawn for. "
                    "eid=0 here means the sender had no wire identity for it either (see "
                    "local_streams' carry-only invariant).",
                    slot, keyW.c_str(), pose.elementId,
                    static_cast<unsigned>(coop::unresolved_pose_ledger::kSustainedCount),
                    static_cast<unsigned long long>(coop::unresolved_pose_ledger::kSustainedMs));
        } else if (newBurst) {
            UE_LOGI("remote_prop: slot %d incoming PropPose key '%ls' eid=%u -- no local match "
                    "(key or eid); usually the pose overtaking its own spawn broadcast",
                    slot, keyW.c_str(), pose.elementId);
        }
        return;
    }
    coop::unresolved_pose_ledger::Clear(slot, keyW, pose.elementId);
    // The half of the grab this peer never ran. A grab runs its prelude on the grabbing machine only:
    // prop_C's playerGrabbed_pre clears the prop's lifespan, broadcasts `touched` and runs
    // awakeUnfreeze on a frozen or sleeping prop, and a prop carrying the wall-attach component (the
    // wall-attachable lineage, the plasma TV) runs its component's unstick instead, which the copy
    // gets with the tool: the holder had it off the wall. So a copy frozen here -- a mounted fire
    // extinguisher, a drive in a slot, anything the toolgun froze -- or stuck, asleep or on a
    // spawner's lifespan, gets the same, once per hold, at the first pose that starts its drive. What a
    // subclass adds to its prelude is not replayed (the sprinkler's hose), and the glowstick's, which
    // replaces the base one, gets the base one here. A closed hold's late pose never gets this far. A
    // static prop cannot be held, so a stream for one that is static here and has no unstick means the
    // two copies disagree on the flag: refused, said once per hold.
    HoldGate& hold = g_holds[slot];
    if (LocalDragsDoor(prop, slot, pose.holdGen)) return;
    if (ue_wrap::prop::IsDescendantOfProp(prop)) {
        const bool attachable = coop::prop_stick_sync::IsWallAttachable(prop);
        if (ue_wrap::prop::IsStatic(prop) && !attachable) {
            if (hold.refusedGen != pose.holdGen) {
                hold.refusedGen = pose.holdGen;
                UE_LOGW("remote_prop: slot %d key '%ls' hold %u streams a prop that is STATIC here (%p) -- a "
                        "static body cannot be held, so the two copies disagree on the flag; not driven",
                        slot, keyW.c_str(), static_cast<unsigned>(pose.holdGen), prop);
            }
            return;
        }
        if (pose.holdGen == 0 || hold.preludeGen != pose.holdGen) {
            hold.preludeGen = pose.holdGen;
            const char* state = ue_wrap::prop::IsStatic(prop)   ? "static"
                                : ue_wrap::prop::IsFrozen(prop) ? "frozen"
                                : ue_wrap::prop::IsSleeping(prop) ? "asleep"
                                                                  : nullptr;
            const bool ok = attachable ? coop::prop_stick_sync::ReplayUnstick(prop)
                                       : ue_wrap::prop::CallBaseGrabPrelude(prop);
            if (!ok) {
                UE_LOGW("remote_prop: slot %d key '%ls' hold %u -- the grab's own prelude did not run here; the "
                        "copy keeps its flags and its lifespan", slot, keyW.c_str(),
                        static_cast<unsigned>(pose.holdGen));
            } else if (state) {
                UE_LOGI("remote_prop: slot %d key '%ls' hold %u grabs a prop %s here -- the grab's own prelude "
                        "applied (ok)", slot, keyW.c_str(), static_cast<unsigned>(pose.holdGen), state);
            }
        }
    }
    // GetStaticMesh is null for the clump by design (it is driven through the root physics); only
    // an Aprop_C without a mesh is an error.
    void* mesh = ue_wrap::prop::GetStaticMesh(prop);
    if (!mesh && ue_wrap::prop::IsDescendantOfProp(prop)) {
        UE_LOGW("remote_prop: slot %d prop %p (Aprop_C) has null StaticMesh -- cannot drive", slot, prop);
        return;
    }
    UE_LOGI("remote_prop: slot %d GRAB-IN key='%ls' eid=%u -> local actor=%p mesh=%p (%s)",
            slot, keyW.c_str(), pose.elementId, prop, mesh,
            mesh ? "Aprop physics-off" : "clump kinematic (generic physics-off)");
    // Both native grab sounds play only in the grabber's own input chain, so the receiver plays
    // them: the fixed `use` click and the per-material soft cue (a plain-actor clump resolves its
    // root material's physSound row; silent on a miss). Once per hold: a drive that restarts for the
    // same hold (a local drag of a door ended) is not a new grab, and plays no cue.
    if (pose.holdGen == 0 || hold.cueGen != pose.holdGen) {
        hold.cueGen = pose.holdGen;
        coop::prop_sound::PlayUseClick(prop);
        coop::prop_sound::PlayGrabSound(prop);
    }
    // Simulation off so the per-packet SetActorLocation sticks: a kinematic follow, the clump
    // through the generic root toggle, and a Character welded on the prop stopped (prop_park.h).
    coop::prop_park::Park(prop, mesh);
    coop::swinger_join_pose::Forget(keyW);   // the stream is newer than a connect pose parked for this lid
    g_drives[slot].actor = prop;
    g_drives[slot].actorIdx = R::InternalIndexOf(prop);  // live here; cache for LiveActor()
    g_drives[slot].mesh  = mesh;
    g_drives[slot].lastKey.assign(pose.key.data, pose.key.len);
    g_drives[slot].lastEid = pose.elementId;
    hold.driveGen = pose.holdGen;
    hold.relatches = 0;
    // A host-authoritative trash mirror freezes on a stream gap instead of timing out; it releases
    // only on the explicit reliable edge, and it interpolates rather than snapping. The test is
    // whether the host authors this body (HostAuthorsTrashBody), not what class it is: a client's
    // own save-loaded PILE is bound as a mirror too, and suppressing its 500 ms implicit release
    // would leave it kinematic and drive-held for the session if the reliable edge never arrived.
    g_drives[slot].isTrashMirror = HostAuthorsTrashBody(prop);
    // A fresh identity: the next pose primes (a snap, no drift-in from the rest position), later
    // poses interpolate.
    g_drives[slot].lerpSeeded   = false;
    g_drives[slot].haveTwoSnaps = false;
    // The timeout clock starts now; at zero the 500 ms stream-stop check in Tick would fire on the
    // first late packet after a fresh grab.
    g_drives[slot].lastApplyMs = NowMs();
}

}  // namespace coop::remote_prop
