// coop/prop_stick_sync.h -- wall-attachable surface-stick mirror (v68,
// 2026-06-12). Fixes the user report: "client sticks the camera and it stays,
// but host sees it as falling down."
//
// GROUND TRUTH (kismet RE, research/findings/votv-camera-stick-RE +
// votv-kerfur-convert-RE-2026-06-12.md for the dispatch rules): every camera
// variant inherits the stick from Aprop_wallAttachable_C, whose
// Ucomp_wallAttachable_C component polls for a stickable surface WHILE the
// player holds the prop (sticking(), 10 Hz latent loop). The COMMIT (ubergraph
// entry 45) sets prop.frozen=true (or .Static when pryingRequired), runs
// prop->init() (SetSimulatePhysics(false)), K2_AttachToComponent KeepWorld,
// spawns the eff_OC_freeze "closing circle" VFX and glides the prop to the
// surface over 0.25 s. Unstick = re-grabbing (playerGrabbed_pre ->
// comp->unstick: frozen=false + static=false + init()). ALL of it is
// BP-internal (local calls / native finals) -- invisible to our ProcessEvent
// detour -- EXCEPT the commit's entry itself: offset 45 is reached ONLY via a
// latent Delay resume, and FLatentActionManager resumes ubergraphs via
// ProcessEvent -> our observer on ExecuteUbergraph_comp_wallAttachable sees
// every player-driven stick. (forceStick jumps to 45 LOCALLY -- BeginPlay/
// loadData self-sticks don't fire the observer; both peers run those anyway.)
//
// WHY IT DESYNCED: the receiving peer's prop pipeline knew nothing of frozen/
// static changing mid-session, and its release/timeout paths unconditionally
// re-enabled SimulatePhysics when the sticking peer's hold broke -- the mirror
// fell. (Late JOIN was already correct: the snapshot carries kFrozen.)
//
// THE FIX:
//   - SENDER (any peer; symmetric like doors): the EntryPoint==45 POST
//     observer records {prop, idx}; Tick() drains on the NEXT net-pump pass
//     and, if the prop ended frozen/static (the commit's re-trace can bail),
//     broadcasts PropStickState{key, eid, flags, commit pose}. Deliberately
//     NO settle delay: the sender's hold breaks 0-100 ms after the commit,
//     and the stick must reach peers BEFORE that PropRelease (same reliable
//     lane = FIFO). Draining in TickGameplay -- which runs before
//     local_streams' release edge in the same pump pass -- makes the order
//     structural; the receiver re-derives the settled pose itself.
//   - RECEIVER: stop any active drive on the actor (no physics re-enable),
//     re-pose to the wire pose, re-enable simulate (the BP's canStick
//     precondition) and PE-dispatch the comp's OWN forceStick(true) -- SP
//     itself then does the field write + attach + OnDestroyed binding + VFX +
//     glide (principle 6). If the replay's re-trace diverges (frozen still
//     false), fall back to the raw write + init() + re-pose -- exactly SP's
//     own save-load degraded mode (frozen at pose, un-attached).
//   - UNSTICK: deliberately NO message. The unstick is a grab, and grabs
//     already stream PropPose: remote_prop unsticks a stuck WALL-ATTACHABLE
//     mirror via UnstickForDrive() once the stream proves SUSTAINED (>=5
//     fresh poses -- the 1-2 stale packets in flight between the sender's
//     commit and its hold-break must not unstick it), and its release/timeout
//     physics re-enables are gated on !frozen/!static. The sender's own pose
//     stream also ENDS at the commit (local_streams treats a frozen/static
//     held wall-attachable as stream-end), so a lingering engine hold can
//     never stream the 5 poses that would rip the just-stuck mirror back off.
//
// MTA precedent: element-state RPCs SET_ELEMENT_FROZEN + ATTACH_ELEMENTS
// (CElementRPCs.cpp) -- state messages, not event replay. Principle 7:
// coop/ network module; engine access via ue_wrap.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropStickStatePayload;
}  // namespace coop::net

namespace coop::prop_stick_sync {

// Idempotent install, retried each net-pump tick until comp_wallAttachable_C
// loads: resolves the component class + ExecuteUbergraph_comp_wallAttachable +
// the comp's `prop` field + prop_wallAttachable_C (+ its comp_wallAttachable
// field) + forceStick/init UFunctions, then registers the POST observer.
// Caches `session`.
void Install(coop::net::Session* session);

// Drain the stick commits recorded by the observer since the last pass:
// verify (still live + frozen/static) and broadcast as PropStickState. MUST
// run before local_streams::Tick within the pump pass (stick-before-release
// ordering). Cheap no-op when empty. Game thread (net pump).
void Tick();

// Receiver for PropStickState (wired in event_dispatch_entity). Game thread.
void OnStickState(const coop::net::PropStickStatePayload& payload,
                  uint8_t senderPeerSlot);

// True iff `actor` is an Aprop_wallAttachable_C descendant (the only lineage
// whose frozen/static a pose stream may legitimately clear). False until the
// class resolves. Cheap SuperStruct walk; any thread.
bool IsWallAttachable(void* actor);

// Clear a stuck wall-attachable for an incoming kinematic drive: frozen=false
// + static=false + PE prop->init() (re-enables simulate, which in UE4.27 also
// detaches an attached root -- the SP unstick shape). Returns true if the
// actor WAS stuck and is now clear. The caller (remote_prop) gates the call on
// IsWallAttachable + its sustained-stream check. Game thread.
bool UnstickForDrive(void* actor);

// Clear per-session state (the commit-pending list). Net disconnect.
void OnDisconnect();

}  // namespace coop::prop_stick_sync
