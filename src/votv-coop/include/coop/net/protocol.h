// coop/net/protocol.h -- the wire format (Phase 3, serialization layer).
//
// Methodology 3.3: this sits ABOVE the pure-I/O transport and BELOW the session
// application layer. It is just POD structs + (de)serialization; it knows nothing
// about sockets and nothing about the engine. Layout is fixed, packed, and
// little-endian (LAN-first: both peers are x86-64 Windows, so we send the structs
// raw -- no endian swap needed; the magic+version guard a future mismatch).
//
// Phase 3.4 "position-only first": the only state packet is a PoseSnapshot
// (x,y,z,yaw,speed). Input/equipment/entity packets (Phase 4) get new MsgTypes.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop::net {

// Opaque magic guard (rejects stray datagrams that hit our port). Both peers
// just agree on the constant; the spelling is "VMTP" (VoTv MultiPlayer).
inline constexpr uint32_t kMagic = 0x564D5450u;
// v5 (2026-05-24 post-compact): PropRelease replaces throw-impulse with
// inherited-physics-velocity (FVector linVel + FVector angVel). Root-cause RE
// (research/findings/votv-throw-release-pipeline-RE-2026-05-24.md) showed the
// "вжух" mouse-flick launch is NOT a discrete AddImpulse call -- it is the
// kinematic-tracking velocity PhysX accumulates while the player flicks the
// camera (UPhysicsHandleComponent.SetTargetLocation jumps the target each tick;
// PhysX integrates a tracking velocity in the constrained body to follow it);
// on ReleaseComponent the body re-enters dynamic sim INHERITING that velocity.
// v4 shipped only the (optional) AddImpulse boost and missed the dominant
// inherited velocity entirely (Bug B). Capture is via
// UPrimitiveComponent.GetPhysicsLinearVelocity / GetPhysicsAngularVelocityInDegrees
// on the release edge; apply via the matching Set... pair after
// SetSimulatePhysics(true).
// v4 (2026-05-24): physics-object grab replication added.
//   - MsgType::PropPose = per-frame world transform of the prop the sender
//     currently holds (unreliable; receiver lookup-by-Key on first arrival,
//     SetSimulatePhysics(false) + drive transform per packet)
//   - ReliableKind::PropRelease = one-shot release signal; v5 carries inherited
//     linear+angular velocity (replaces v4's throw-impulse FVector); receiver
//     re-enables SimulatePhysics + SetPhysicsLinearVelocity + SetPhysicsAngularVelocityInDegrees
//   - WireKey carries the FName string (cross-peer stable, idx is NOT --
//     see research/findings/votv-physics-interaction-deep-re-2026-05-23.md)
// v9 (2026-05-27 Phase 5G Inc 2): NonPropEntityState/Destroy packets shipped
// then retired the same day -- chipPile/clump/trashBitsPile families ride
// the existing Aprop_C pipeline via the IsKeyedInteractable extension
// instead. ReliableKind slots 16 and 17 are dormant (see comment near the
// enum). The version stays at 9 because peers with v9 may still receive
// stale packets at those slots from old hosts; the dispatch ignores them.
// v8 (2026-05-27): PoseSnapshot grew 28 -> 32 bytes -- added `stateBits`
// (bit 0 = isInAir, derived from source CMC MovementMode == MOVE_Falling).
// Receiver drives the puppet AnimBP's useLegIK/rise off this bit so the foot
// IK trace doesn't plant feet during jumps. Same expansion shape leaves
// bits 1..7 reserved for future per-pose flags (crouched, KO, etc.).
// v7 (2026-05-26 NIGHT): Phase 5W weather sync. Adds
//   ReliableKind::WeatherState = 13 (continuous rain/snow/fog/wind state)
//   ReliableKind::LightningStrike = 14 (discrete strike events)
// Host-authoritative model: client suppresses its 5 scheduler UFunctions on
// AdaynightCycle_C via multi-slot interceptor and receives state via reliable
// pushes. See research/findings/votv-weather-DESIGN-2026-05-26.md.
// v6 (2026-05-26 PM): ItemActivatePayload grew 16 -> 24 bytes -- added float
// intensity + outerConeAngle + innerConeAngle + uint8 mode so the receiver can
// mirror the sender's EXACT cone shape (Phase 5F user feedback: puppet's
// flashlight was too bright AND focused-vs-spread mode wasn't synced).
// v3 (2026-05-23 PM): PoseSnapshot grew from 24 -> 28 bytes (added headYawDelta).
// Both peers must run v9; older packets are rejected at the header check. No
// back-compat layer (RULE 2 -- mod is pre-ship; bump cleanly).
// v10 (2026-05-28 PR-2): wire layer migrated to GameNetworkingSockets. The
// custom Hello / HelloAck / Bye / Ping / Pong / ReliableAck dispatch tags are
// gone (GNS owns handshake, RTT, acks); an old peer's packet would land
// on a dead MsgType branch even if the version check passed, so we bump
// the version to make the mismatch explicit at ParseHeader time.
// v11 (2026-05-28): adds ReliableKind::AssignPeerSlot. Host sends it
// to each client right after Connected with the slot it was assigned.
// Without this, clients would self-stamp peerSessionId=1 and two
// clients would silently self-echo-drop each other's ItemActivate.
// v12 (2026-05-28): MTA CClientElement adoption -- EntitySpawn.sessionId
// + EntityDestroy.sessionId renamed to .elementId (cosmetic; semantics
// already host-allocated Registry ids since the Tier 3 NPC migration).
// PropSpawnPayload + PropDestroyPayload gain an `elementId` field
// (uint32, +4 bytes each) carrying the SENDER's allocation-range
// ElementId for the prop.
//
// A2 (2026-05-29) -- payload-compatible: no field changes, but the
// sender-side rule for PropSpawn/PropDestroy.elementId is now ROLE-
// AWARE: host minted ids land in host range [1, kHostRangeSize); client
// minted ids land in peer range [kHostRangeSize, kMaxElements). Receivers
// call Registry::RegisterMirror at the sender's eid; cross-peer
// Registry::Get(eid) resolves to the wire-identity's actor on both sides.
// EntitySpawn / EntityDestroy still host-range only (NPC sync is host-
// authoritative; clients never broadcast NPC entity packets).
//
// v13 (2026-05-29) -- A4 Player Element wire migration:
//   - AssignPeerSlotPayload gains `hostElementId` (uint32, grows 4->8B).
//     Host stamps its local Player Element id; client RegisterMirrors
//     it in slot 0 so subsequent packets bearing host's senderElementId
//     resolve to a Player on the client side via Registry::Get.
//   - Join payload prepends `uint32 senderElementId` before the existing
//     [uint8 nicklen][nick bytes] format (grows by 4 bytes). Receiver
//     RegisterMirrors that eid in the sender's puppet slot.
//   - ItemActivatePayload, WeatherStatePayload, RedSkyPayload,
//     LightningStrikePayload: `peerSessionId` (uint8) field RETIRED in
//     favor of `senderElementId` (uint32). Sender stamps own local
//     Player Element id; receivers resolve via
//     coop::element::Registry::Get -> Player::PeerSlot() for routing,
//     compare against own local Element id for self-echo guard, and
//     (Weather/RedSky/Lightning) validate PeerSlot()==0 for host trust-
//     boundary. Element mirror creation handshake lives in AssignPeerSlot
//     + Join above. ItemActivatePayload grows 24->28B; WeatherStatePayload
//     20->24B; RedSky + LightningStrike stay the same total size.
//
// v14 (2026-05-29) -- B1 syncContext on wire (MTA m_ucSyncTimeContext
// adoption). Each packet that already carries `senderElementId` (or
// `hostElementId` for AssignPeerSlot) now also carries a `senderContext`
// uint8 (or `hostContext` for AssignPeerSlot) -- the 8-bit generation
// counter from the SENDER's local Player Element (Element::GetSyncContext).
// Receiver compares the wire byte against its mirror's local context (set
// at RegisterMirror via EstablishMirrorForSlot from the handshake byte)
// and drops the packet on mismatch. Defends against ElementId reuse:
// when a peer disconnects and reconnects on the same slot, their NEW
// Player Element gets a fresh per-process-monotonic context, so stale
// pre-disconnect packets stamped with the OLD context are visibly
// distinguishable from the new generation's packets. Without this byte
// an in-flight ItemActivate from the previous incarnation could be
// routed to the new puppet and toggle its flashlight at the wrong moment.
// Layout: 5 of 6 packets reuse existing `_pad` bytes (no size change);
// LightningStrikePayload grows 16->20B (no existing pad to steal).
// Sender stamps `senderContext = Registry::Get(selfEid)->GetSyncContext()`
// (via the lock-free `players::Registry::LocalPlayerSyncContext()`
// accessor that mirrors LocalPlayerElementId). Receivers fall through
// (no compare) when senderElementId is the 0 sentinel (boot/seed race).
//
// v15 (2026-05-29) -- audit follow-up E-2: extend B1 syncContext coverage
// to the prop-lifecycle packets that B1 missed. PropSpawnPayload and
// PropDestroyPayload already carry `elementId` (sender's allocation-range
// Prop Element id, v12/A2) but they do NOT carry the SENDER's Player
// Element generation byte. Same hazard as ItemActivate: a peer that
// disconnects mid-pickup and reconnects gets a fresh per-process-
// monotonic context; an in-flight stale PropSpawn/PropDestroy stamped
// with the previous incarnation's eid could be honored against the new
// generation. Wire shape: both payloads have spare pad after `elementId`
// already (PropSpawnPayload had `uint8_t _pad[3]` after physFlags;
// PropDestroyPayload had a trailing `uint32_t _pad`). Steal one byte
// for senderContext, keep the rest as pad -- ZERO size change to either
// payload, just a field rename + protocol bump. ParseHeader rejects
// pre-v15 packets so a stale ItemActivate-shape PropSpawn (mismatched
// pad byte meaning) can't slip through.
//
// v16 (2026-05-29) -- PR-FOUNDATION-1b: per-peer 32-bit session epoch in
// the packet HEADER, replacing the 8-bit per-payload senderContext byte.
// Two motivating defects in the v14/v15 design:
//   1. 8-bit aliases at 256 reconnect cycles. MTA shipped this width
//      for 15+ years and the wrap was masked by per-entity (not per-
//      peer) granularity + frequent state-change increments. VOTV's
//      Per-peer-Player-Element scheme increments once per process,
//      so the wrap was a real hazard within a long session.
//   2. 0-on-either-side passthrough (the "boot/seed race" gate at
//      event_feed.cpp:86 + the manual compares at lines 321/400)
//      defeated the defense entirely whenever the sender's local
//      Player Element hadn't been minted yet -- an in-flight stale
//      packet stamped 0 paired with a freshly-minted mirror at the
//      receiver bypassed every compare. W-1 audit finding.
//
// New design:
//   - PacketHeader.token (uint64_t, ALWAYS 0 since PR-2 GNS migration --
//     all WriteHeader call sites hardcoded /*token*/0; ParseHeader caller
//     reads into `uint64_t tokenUnused`) is split: low 4 bytes become
//     `uint32_t senderEpoch`, high 4 bytes become `uint32_t _reserved`
//     (kept 0 on send; future use TBD). PacketHeader stays 20 bytes.
//   - Session mints `ownEpoch_` via std::random_device at Start() --
//     non-zero (re-rolls on the 1/2^32 zero) so 0 unambiguously means
//     "not yet latched" at the receiver.
//   - Sender stamps `ownEpoch_` on every outbound packet header (4
//     WriteHeader sites: SendReliableToSlot, SendReliable fan-out,
//     SendMessageToConnection PoseSnapshot, SendMessageToConnection
//     PropPose).
//   - Receiver, per peerSlot, maintains `m_expectedEpoch[slot]` --
//     latched on first packet from the slot when value is 0; subsequent
//     packets must match exactly; mismatch -> drop with log.
//   - On disconnect (ResetPeerRemoteState) `m_expectedEpoch[slot] = 0`
//     so the next packet from the new connection at that slot re-latches.
//   - No separate epoch-handshake message: the host's AssignPeerSlot
//     packet's HEADER already carries the host's m_ownEpoch, and the
//     client's first Join carries the client's m_ownEpoch -- the
//     receiver latches via the header on the first packet, period.
//
// Per RULE 2 (no migration baggage), v14/v15 per-payload senderContext
// + Element::m_syncContext + players::Registry::LocalPlayerSyncContext
// + VerifySenderContext + the manual context compares at event_feed.cpp
// PropSpawn/PropDestroy sites are FULLY DELETED in this bump (not
// retained as a parallel gate). senderContext fields gone from
// PropSpawnPayload, PropDestroyPayload, ItemActivatePayload,
// WeatherStatePayload, RedSkyPayload, LightningStrikePayload, plus
// hostContext from AssignPeerSlotPayload, plus the Join payload's
// context byte. Pad bytes coalesced back; LightningStrikePayload
// shrinks 20->16 (it had no existing pad pre-v14 -- per the v14 comment
// block above -- so the byte goes with no trailing pad).
//
// v17 (2026-05-29) -- PR-FOUNDATION Tier 2 (host-relay topology) T2-1:
// adds ReliableKind::PlayerJoined (=19), the host's cross-peer identity
// broadcast so clients learn about EACH OTHER (not just the host). New
// wire vocabulary only -- no struct changes. The version bump makes a
// v16 peer's silent ignore of PlayerJoined (which would leave cross-peer
// puppets unidentified) a visible ParseHeader mismatch instead.
//
// v18 (2026-05-29) -- PR-FOUNDATION Tier 2 T2-2 (pose relay): carves a
// `senderSlot` byte out of the header's old `_reserved` (zero size change,
// header stays 20 B) so the host can RELAY a client's PoseSnapshot /
// PropPose to other clients tagged with the logical origin slot. Receiving
// clients route relayed poses by senderSlot. See PacketHeader doc.
//
// v19 (2026-05-30) -- player vitals pillar Inc1 (continuous display vitals):
// PoseSnapshot's 3 trailing `_pad` bytes become `healthFrac`/`foodFrac`/
// `sleepFrac` (each a [0,1] fraction quantized 0..255). ZERO wire-size change
// (PoseSnapshot stays 32 B). Per-peer-authoritative: each peer packs its OWN
// vitals (host packs host's, each client its own) off this machine's
// UsaveSlot_C via ue_wrap::vitals. DISPLAY-ONLY on the receiver -- drives the
// remote puppet's nameplate health bar; NEVER written to any saveSlot (one
// saveSlot per machine; a write would corrupt the local player's persisted
// health). Reliable death/ragdoll authority is a later increment (these bytes
// are continuous, lossy, and intentionally never trigger a DEAD transition).
// See research/findings/votv-player-vitals-death-RE-2026-05-30.md.
//
// v20 (2026-05-31) -- vitals pillar Inc2b (ragdoll/faint DISPLAY sync):
// PoseSnapshot.stateBits gains bit 1 `kStateBitRagdoll` (per-pose, per-peer-
// authoritative). The sender sets it from its OWN AmainPlayer_C::isRagdoll
// (excluding death -- death = native SP menu flow, ends the session). isRagdoll
// is flipped by EVERY ragdoll cause: manual C-key (InpActEvt_ragdoll_..._25),
// exhaustion faint(), and KO -- so this one bit covers all of them. The
// receiver reconciles the peer's puppet against the bit (ragdollMode(1,1,0) on
// rising, forceGetUp() on falling), self-healing + idempotent. ZERO wire-size
// change (stateBits byte already exists; bit 1 was reserved). DELIBERATE
// DIVERGENCE from the banked reliable-event design (PlayerRagdollState
// ReliableKind): faint is a TRANSIENT, lossy-tolerant, per-peer DISPLAY state
// with multiple causes and no readable passOut/death distinction, so the
// self-healing continuous-bit shape (sibling of kStateBitInAir + the v19 vitals
// piggyback, and MTA's CPlayerPuresyncPacket flag shape) is the root-cause-
// correct fit -- it rides the proven per-peer pose relay (no new ReliableKind /
// relay-whitelist / lane / defer-latch / connect-replay machinery, which existed
// only to survive the reliable design's permanent-desync failure mode).
//
// v22 (2026-06-01) -- ragdoll PHYSICS sync (the pelvis-attached puppet ragdoll
// learns the ragdolling peer's ACTUAL physics instead of free-simulating its own).
// v20's kStateBitRagdoll only synced the on/off STATE; the host then spawned an
// INDEPENDENT playerRagdoll_C body that flopped on the HOST's local PhysX -- so a
// client who ragdolled off a ledge and flew forward looked like a body flopping in
// place on the host (the gross motion did NOT match). This bump adds a new
// unreliable MsgType::RagdollPose (=16) -- a sibling of PropPose, sent only WHILE a
// peer is ragdolling -- carrying that peer's ragdoll PELVIS world transform +
// linear + angular velocity (read off its native AmainPlayer_C::ragdollActor's
// SkeletalMesh pelvis bone). The receiver feeds the velocity onto its mirror body's
// pelvis each packet (SetPhysicsLinearVelocity + SetPhysicsAngularVelocityInDegrees
// -- the SAME UPrimitiveComponent ops PropRelease uses) so the mirror body tumbles
// to TRACK the client, and drives the puppet's rotation from the streamed pelvis
// rotation directly (exact, no reliance on the host sim matching). MTA precedent:
// dead-reckoning a networked entity from a streamed transform + velocity is
// CClientPed's time-based interpolation shape (CClientPed.cpp:218 / 5405). Mirrors
// the prop pipeline exactly (PropPose continuous transform + PropRelease velocity,
// fused into one per-frame ragdoll packet). ParseHeader rejects pre-v22 peers so a
// v21 host (which has no RagdollPose dispatch) is a visible version mismatch rather
// than a silently-dropped packet.
// v23 (2026-06-01): WeatherStatePayload gains a fog flags2 byte (active-fog bits
// kFogActive/kSuperFogActive/kPermanentFog) for host-authoritative fog. Fog is
// driven by event ACTORS (AweatherFogController_C / AsuperFog_C), not the config
// enable_* bits -- the wire now carries actor PRESENCE so the client can destroy a
// stray fog actor on host-clear + mirror-spawn on host-fog. No wire-size change
// (the byte reuses a former _pad slot); ParseHeader rejects pre-v23 peers.
// v24 (2026-06-02): late-joiner WEATHER-LEVEL SNAP. WeatherStatePayload grows
// 24->40 (+rain target, +finalFogDensity, +fogAlpha, +fogStrength) so a joining
// client snaps to the host's CURRENT fog/rain levels instead of ramping up over
// ~3 min (the rolling-fog actor ramps its density from 0; a fresh client mirror
// did the same). The host copies its rolling-fog actor's Alpha+Strength onto the
// client mirror (fogprobe-proven the actor accepts the write + keeps ramping in
// lockstep) + the cycle's finalFogDensity, and anchors the rain ease target.
// ParseHeader rejects pre-v24 peers (the 16-byte payload growth would misparse).
// v25 (2026-06-02): held-clump ATTACH sync. The trash ball / chipPile-clump
// (prop_garbageClump_C) is non-Aprop_C AND non-keyable (autotest 33e7f25 proved
// setKey doesn't stick on it), so it can't ride the keyed PropSpawn/PropPose path
// the mannequin uses. Instead the holder broadcasts two reliable edges --
// HeldClumpGrab (=21) when a clump enters its hands, HeldClumpRelease (=22) when
// it leaves -- and each peer mirrors the clump by SPAWNING a copy and
// K2_AttachToComponent-ing it to the holder puppet's hand bone (the MTA attach
// model, same primitive as AttachActorToRagdollBody). The puppet hand is already
// pose-synced, so the attached clump follows the hand for free; on release the
// mirror detaches + re-enables physics + inherits the holder's throw velocity
// ("physics like the mannequin"). No per-tick clump stream, no key needed.
// ParseHeader rejects pre-v25 peers. [[project-bug-trash-chippile-uaf-crash]]
// v26 (2026-06-03): the held trash-clump is reworked to ride the MANNEQUIN prop
// pose pipeline (PropSpawn/PropPose/PropRelease) instead of the v25 hand-attach
// (RULE 2 -- attach was the wrong model: VOTV carries the clump via the physics
// grab, floating in front, like the mannequin). The clump is non-keyable, so
// PropPoseSnapshot grows 56->60 with an `elementId` and the receiver resolves the
// mirror by OUR eid when the Key is None. The v25 ReliableKinds HeldClumpGrab(21) /
// HeldClumpRelease(22) are RETIRED (RULE 2); their slots stay reserved. The clump is
// visible on its own (a bare spawn renders the 'dirtball' mesh), so no mesh transfer
// is needed. ParseHeader rejects pre-v26 peers (PropPosePacket grew 76->80).
// v27 (2026-06-03): Phase 5D keyed-interactable state sync -- base doors, light
// switches, and storage-container lids, all driven by ONE generic
// coop::interactable_sync Channel (no per-feature duplication, RULE 2). Three new
// ReliableKinds share KeyedTogglePayload (WireKey instance Key + action on/off):
//   DoorState (=9)      -- Adoor_C::doorOpen/doorClose
//   LightState (=10)    -- Atrigger_lightRoot_C::SetActive
//   ContainerState (=11)-- Aprop_swinger_C::Open/Close (cabinet/fridge/safe lids)
// All SYMMETRIC (any peer's local edge broadcasts; host relays client<->client)
// + a host connect-snapshot of every OPEN instance to a late joiner. Identity =
// the instance's cross-peer-stable Key (AtriggerBase_C::Key for doors+lights;
// Aprop_C::Key for swinger lids). RE:
// research/findings/votv-doors-and-lightswitches-RE-2026-05-25.md. ParseHeader
// rejects pre-v27 peers. [[project-coop-doors-lights-sync]]
// v28 (2026-06-03): trash-clump DESPAWN + VARIANT fix. (a) PropDestroy is now
// EID-ROUTABLE: the non-keyable clump (prop_garbageClump_C, key=None) rides our eid
// for spawn (v26) but its morph-destroy is UNOBSERVABLE (native/BP-internal, bypasses
// the K2_DestroyActor observer -- proven hands-on), so the owner death-WATCHES each
// clump it broadcast and sends PropDestroy{key=None, elementId} the tick its actor
// dies; OnDestroy resolves the target by eid when the key is empty (symmetric with the
// eid-spawn; MTA Packet_EntityRemove resolves by element ID only). (b) PropSpawnPayload
// steals one _pad byte for `chipType` (the 14-value trash-variant enum at struct 0x0238
// on chipPile/clump) so a mirrored clump shows the SAME variant the owner grabbed
// (a bare spawn defaulted to variant 0 = the wrong-type bug). Struct size UNCHANGED
// (168 B). [[project-bug-trash-chippile-uaf-crash]]
// v29 (2026-06-03): trash LANDING SOUND. The owner-authoritative landed pile (the
// chipPile the owner's clump re-piled into, broadcast by trash_collect_sync's death-
// watcher) is a bare SpawnActor on the peer -- silent, because the flying mirror is
// despawned, never physically landed. Now BroadcastLandedPileNear stamps physFlags
// kFreshLanded + carries the clump's landing velocity in initLinVel, and the receiver
// dispatches AactorChipPile_C::turnToPile(Velocity) -- the SAME BP entry the real
// clump->pile landing calls (sets spwnd + fires impact dust+sound; operates on `this`,
// spawns nothing -> no dupe). Gated on kFreshLanded so the connect path can't replay it.
// Struct size UNCHANGED (168 B; reuses initLinVel). [[project-bug-trash-chippile-uaf-crash]]
// v30 (2026-06-04): SHARED HOST-AUTHORITATIVE BALANCE. New ReliableKinds BalanceSync (23,
// host->client absolute-total mirror, broadcast on Points change + connect-edge) +
// BalanceDelta (24, client->host signed-delta request, host applies via AddPoints). New
// 4-byte BalancePayload. Both peers now MIRROR the host's saveSlot.Points; client-side
// credits (the +1000 dev button) route to the host. [[project-coop-shared-balance]]
// v34 (2026-06-06): CLIENT LOADING SCREEN brackets. Two new host->client markers wrap the
// connect-edge world snapshot so the joiner can show a determinate progress cover (a
// Source-style loading screen) instead of watching the world build itself: SnapshotBegin
// (27, sent FIRST in StartEnumerationFor carrying the prop candidate count) + SnapshotComplete
// (28, sent LAST after the per-tick prop drain finishes). Both ride Lane::Bulk -- the SAME
// ordered lane as PropSpawn -- so delivery is strictly Begin -> [every PropSpawn] -> Complete,
// and the cover lifts only after the final prop lands (the Lane::High TeleportClient that
// places the joiner arrives earlier, behind the cover). MTA precedent: CTransferBox's
// determinate MAP_DOWNLOAD box (host establishes the total up front, bar fills as the reliable
// stream drains, snaps complete at the end) -- reference/mtasa-blue/Client/mods/deathmatch/
// logic/CTransferBox.cpp + CClientGame.cpp:6051 (NotifyBigPacketProgress). Host-originated
// point-to-point (not relayable). New 4-byte SnapshotBeginPayload + SnapshotEndPayload.
// [[project-host-game-save-picker]].
// v39 (2026-06-07): KERFUR HEAD-LOOK sync. EntityPoseSnapshot grows 28 -> 40 (adds the
// kerfur AnimBP `lookAt` FVector @0x2D90 -- the WORLD location the head/neck FAnimNode_LookAt
// nodes aim at) + a hasLookAt stateBit. RE (2 agents CONVERGED, byte-exact): the head-look is
// natively recomputed PER-PEER each tick as GetPlayerCameraManager(self,0).GetActorLocation()
// (the LOCAL player camera) unless the AnimBP `customLookAt` bool @0x2E49 is set -- so without
// sync each peer's kerfur stares at its own local player (user hands-on: "looked in another
// direction on client"). The host streams its kerfur's resolved lookAt; the client writes it
// onto the mirror's AnimInstance + sets customLookAt=true so the mirror's own BUA stops
// overwriting it and the native LookAt nodes aim at the host's target. The same native
// lookAt/customLookAt path is the queued mechanism for the player-puppet "camera look = head
// look" follow-on. [[project-npc-sync-pose-and-snapshot]] / docs:
// research/findings/votv-kerfur-headlook-AnimBP-RE-and-coop-sync-2026-06-07.md.
// v40 (2026-06-07): KERFUR BODY-facing sync (the head-then-body tracking's BODY half).
// EntityPoseSnapshot grows 40 -> 44 (adds `bodyYaw`) + a hasBodyYaw stateBit. RE (2 agents
// CONVERGED, byte-exact): the kerfur ACTOR BP (ExecuteUbergraph_kerfurOmega @28614) rotates its
// ACharacter::Mesh@0x0280 component's WORLD yaw via `floor_lerp`@0x07B8 toward the LOCAL player
// (idle branch = FindLookAtRotation(self, GetPlayerPawn(0)).Yaw, engaging when the player is past
// the head clamp -- dot < -0.6). This is DECOUPLED from the actor root yaw we already sync, and
// the mirror's actor tick is OFF so it never runs there -> the visible body points opposite ways
// on host vs client (user RE: back-to-keypad on host, front-to-keypad on client). The host streams
// the resolved mesh world yaw; the client writes it onto the mirror's mesh (after SetActorRotation;
// no gate flag -- mirror tick off can't clobber it). SEPARATE from the v39 head `lookAt` sync.
// research/findings/votv-kerfur-bodyfacing-RE-2026-06-07.md (pending).
inline constexpr uint16_t kProtocolVersion = 40;

// Default LAN port (overridable via votv-coop.ini "net.port=").
inline constexpr uint16_t kDefaultPort = 47621;

// PR-2 v10 (2026-05-28): GNS owns handshake (Hello), graceful disconnect
// (Bye), RTT (Ping/Pong), and reliable acks (ReliableAck). Those five
// MsgType values are deleted per RULE 2 -- the underlying mechanism
// moved entirely to GameNetworkingSockets and the wire-byte tags are
// gone. The remaining MsgType values keep their numeric tags so that
// PR-2 builds reading a stray pre-PR-2 packet would parse and ignore-
// dispatch via the default case (the version check rejects them first
// anyway).
enum class MsgType : uint8_t {
    PoseSnapshot = 2,  // player pose (unreliable, newest wins)
    Reliable = 4,      // ordered+delivered message wrapping a ReliableKind payload
    PropPose = 8,      // held-prop world transform (unreliable, per-frame while held)
    RagdollPose = 16,  // v22: ragdolling peer's pelvis transform + velocity
                       //      (unreliable, per-frame WHILE that peer is ragdolled).
                       //      Sibling of PropPose: the receiver feeds the velocity
                       //      onto its mirror ragdoll body's pelvis so it tracks the
                       //      sender's real ragdoll instead of free-simulating.
    EntityPose = 32,   // v37 (2026-06-07): HOST->all NPC pose BATCH (unreliable, ~sendHz).
                       //      Body: EntityPoseBatchHeader (count) + N EntityPoseSnapshot.
                       //      Makes the client NPC mirrors MOVE (they otherwise sit frozen at
                       //      spawn). Keyed per-entry by Npc Element id; newest-wins. The host
                       //      reads each live NPC's transform+CMC velocity (npc_sync::
                       //      TickPoseStream); the client interpolates + drives the mirror's CMC
                       //      (npc_mirror, the RemotePlayer-twin element::Npc). One datagram for
                       //      all NPCs (<=kMaxNpcBatchEntries, MTU-capped).
};

// Payload kinds carried inside a Reliable message. The chat/event-feed groundwork:
// Join announces a player (its nickname) so the peer can post "<nick> joined" and
// label the remote player. (Chat text is a future ReliableKind.)
enum class ReliableKind : uint8_t {
    Join = 1,         // v16 payload: [uint32 senderElementId][uint8 nicklen][nick
                      //     UTF-8]. senderElementId is the SENDER's local Player
                      //     Element id (host range from host; peer range from
                      //     client). Receiver calls EstablishMirrorForSlot and
                      //     SetNickname on the puppet. v14 had a senderContext
                      //     byte before nicklen; v16 PR-FOUNDATION-1b moved the
                      //     stale-generation defense to the packet HEADER
                      //     (senderEpoch), so this byte is gone. ParseHeader
                      //     rejects pre-v16 packets so a stale pre-v16 Join's
                      //     senderContext byte cannot be misread as nicklen.
    PropRelease = 2,  // v5: host released a held prop. Payload: PropReleasePayload
                      //     (WireKey + FVector linVel cm/s + FVector angVel deg/s).
                      //     Receiver: SetSimulatePhysics(true) +
                      //     SetPhysicsLinearVelocity + SetPhysicsAngularVelocityInDegrees;
                      //     fires Aprop_C.thrown if |linVel| > kThrownThreshold so
                      //     the BP's natural whoosh + particle FX dispatch.
    PropSpawn = 3,    // v5: peer dropped an inventory item into the world (a new
                      //     Aprop_X_C instance with a fresh Key). Payload:
                      //     PropSpawnPayload (WireClassName + WireKey +
                      //     loc/rot/scale + physFlags + initLinVel + initAngVel).
                      //     Receiver: FindClass(className) +
                      //     BeginDeferredActorSpawnFromClass + setKey(receivedKey)
                      //     BEFORE FinishSpawningActor (so Aprop_C.Init doesn't
                      //     overwrite Key with NewGuid) + FinishSpawningActor +
                      //     SetSimulatePhysics + optional initial velocity.
                      //     NO echo loop: receiver spawns directly, not through
                      //     UpropInventory_C.takeObj, so the takeObj POST observer
                      //     never fires for receiver-applied spawns. Phase 5S0 Inc2:
                      //     HOST broadcasts via the Aprop_C::Init POST observer for
                      //     ALL spawns (mushroom growth, world-gen, inventory drops);
                      //     CLIENT broadcasts only via takeObj observer (its own
                      //     inventory drops). Echo-suppressed via incoming-spawn-set.
    PropDestroy = 4,  // v5: peer destroyed a prop (food eaten, container broken,
                      //     mushroom harvested, etc.). Payload: PropDestroyPayload
                      //     (WireKey only). Receiver: FindByKeyString + K2_DestroyActor.
                      //     Echo-suppressed by incoming-destroy-set so the receiver's
                      //     K2_DestroyActor doesn't bounce back to the sender.
    EntitySpawn = 5,  // Phase 5N1 Inc2 (NPC sync, 2026-05-25): host detected a
                      //     new NPC instance (one of the 12 allowlisted enemy
                      //     classes; tracked via the host-side PRE observer on
                      //     BeginDeferredActorSpawnFromClass that fires alongside
                      //     the client-side interceptor). Payload:
                      //     EntitySpawnPayload (WireClassName + uint32_t
                      //     sessionId + loc + rot). Receiver (Inc3, not wired
                      //     yet): MarkIncomingNpcSpawn(cls) + BeginDeferred +
                      //     FinishSpawningActor + cache under sessionId. The
                      //     sessionId is host-assigned monotonic, NOT a save-
                      //     UUID like Aprop_C.Key -- NPCs are runtime-spawned
                      //     without persistent identity, so the host owns the
                      //     ID-space.
    EntityDestroy = 6, // Phase 5N1 Inc2: host detected an NPC destruction via
                      //     the existing AActor::K2_DestroyActor PRE observer
                      //     (filtered by host-side tracked-NPC set). Payload:
                      //     EntityDestroyPayload (sessionId only -- 8 bytes).
    RestoreVitals = 7, // 2026-05-25 LATE +5h: F3 dev-key triggered by host.
                      //     Host applies vitals max-out locally + broadcasts
                      //     this packet so both peers' food/sleep/health are
                      //     restored simultaneously (coffeePower is
                      //     intentionally excluded -- writing it triggers a
                      //     screen-shake BP side-effect; user-retest finding
                      //     2026-05-25 NIGHT, commit 5421d6f). No payload
                      //     beyond the ReliableHeader -- the action is fixed
                      //     (max-out the 3 vitals on the local UsaveSlot_C).
                      //     [dev] devkeys=1 gated.
    TeleportClient = 8, // 2026-05-25 LATE +5h: F4 dev-key triggered by host.
                       //     Host snapshots own pose + sends to client; client
                       //     applies via K2_TeleportTo on its local mainPlayer.
                       //     Host->client only; no echo. Payload:
                       //     TeleportClientPayload (24 bytes).
    DoorState = 9,     // Phase 5D (2026-06-03, v27): base door open/close sync.
                       //     SYMMETRIC -- any peer who runs Adoor_C::doorOpen /
                       //     doorClose locally (E-press OR NPC sensor auto-open)
                       //     broadcasts the resulting isOpened state keyed by the
                       //     door's AtriggerBase_C::Key (FName, deterministic +
                       //     save-persistent). Host relays client-originated edges
                       //     to the other clients (IsClientRelayableReliableKind).
                       //     Receiver resolves the door by Key (door_sync index
                       //     w/ GUObjectArray-scan self-heal) + idempotently
                       //     applies doorOpen/doorClose(bypassCheck=true) only if
                       //     not already in the target state -- so a simultaneous
                       //     double-open can't double-animate, and an apply is
                       //     echo-suppressed so it doesn't bounce back. On a new
                       //     client's connect edge the host snapshots EVERY door's
                       //     current state to the joiner (same idempotent apply --
                       //     only doors that diverge actually animate). Payload:
                       //     KeyedTogglePayload (40 bytes). RE: research/findings/
                       //     votv-doors-and-lightswitches-RE-2026-05-25.md.
    LightState = 10,   // Phase 5D (2026-06-03, v27): light-switch group on/off
                       //     sync. SYMMETRIC. Sender POST-observes
                       //     Atrigger_lightRoot_C::SetActive(bool) -- the group
                       //     controller that fires for any source (wall switch,
                       //     power board, script). Identity = the root's
                       //     AtriggerBase_C::Key. Receiver resolves the lightRoot
                       //     by Key + idempotently calls SetActive(on) (which
                       //     fans out to every AceilingLamp_C / AambientLight_C in
                       //     the group). Same generic Channel as DoorState.
                       //     Payload: KeyedTogglePayload.
    ContainerState = 11, // Phase 5D (2026-06-03, v27): storage container / cabinet
                       //     LID open/close sync. SYMMETRIC. Sender POST-observes
                       //     Aprop_swinger_C::Open(bool)/Close() -- the generic
                       //     openable-lid prop base (fridge/safe/microwave/cabinet
                       //     doors all inherit it). Identity = Aprop_C::Key @0x02E0
                       //     (NOTE: child-actor lids may carry a per-peer NewGuid
                       //     Key -- the install-time keysHash diagnostic confirms
                       //     cross-peer stability; if a class proves unstable it
                       //     simply won't resolve + the apply expires harmlessly).
                       //     Receiver resolves the swinger by Key + idempotently
                       //     calls Open(false)/Close(). Same generic Channel.
                       //     Payload: KeyedTogglePayload.
    WeatherState = 13, // Phase 5W (2026-05-26): host-authoritative continuous
                       //     weather state push. Sender = host only; client
                       //     SUPPRESSES its own 5 scheduler UFunctions on
                       //     AdaynightCycle_C (timerRain / timerLightning /
                       //     fogEvent / superFogEvent / permaRain_timer) via
                       //     the multi-slot interceptor. Host's POST observer
                       //     on those same 5 UFunctions reads the post-mutation
                       //     state off the live AdaynightCycle_C and sends if
                       //     changed (FNV1a dedup). On connect-edge the host
                       //     also queues a forced broadcast so a newly-joined
                       //     client doesn't miss a storm that started before
                       //     it joined (mirrors Phase 5F Inc5 pattern).
                       //     Receiver applies state by calling
                       //     causeRain / setRainProperties / setWindParameters /
                       //     intComs_triggerSnow on its local AdaynightCycle_C
                       //     (NOT direct field writes -- intComs_triggerSnow
                       //     has 53 BP listeners that need the UFunction
                       //     dispatch fan-out). Save persists nothing live
                       //     (RE doc -- only gameRules.permanentRain/Fog), so
                       //     this packet is the ONLY way client weather
                       //     converges to host. RE: research/findings/votv-
                       //     weather-RE-{mainGamemode,effect-actors,scheduler}-
                       //     2026-05-26.md. Payload: WeatherStatePayload.
    RedSky          = 15, // Phase 5W Inc-fix-2 (2026-05-27): one-shot story
                       //     visual event. Lives on AmainGamemode_C (NOT
                       //     the cycle): spawnRedSky() spawns AredSkyEvent_C
                       //     and stashes it at mainGamemode.redSky @0x0888;
                       //     subsequent on/off toggles call redSky.set(bool)
                       //     to swap the 4 color-curve assets on the cycle
                       //     (fog_color_A/B, amb_color, sun_color) to the
                       //     "red" set. Pure deterministic (no Random in
                       //     set's BP body per IDA RE 2026-05-27). Chosen
                       //     as the autotest's visual signal because it
                       //     produces an UNAMBIGUOUS scene-wide tint shift
                       //     (entire sky goes red), unlike rain particles
                       //     whose visibility is ambiguous (turns out to
                       //     be mostly atmospheric mist, not particles --
                       //     user feedback 2026-05-27). Host POST observer
                       //     on spawnRedSky (first activation) + on
                       //     redSky.set (subsequent toggles); broadcasts.
                       //     Receiver invokes the same on its local
                       //     gamemode. Payload: RedSkyPayload (8 bytes).
    LightningStrike = 14, // Phase 5W Inc2 (2026-05-27): discrete strike event.
                       //     Host hooks the SpawnActor of AlightningStrike_C
                       //     (via K2_BeginDeferredActorSpawnFromClass class
                       //     filter, since the actual spawn is BP-internal to
                       //     AdaynightCycle_C::timerLightning). Sends
                       //     (location). Client suppresses local lightning via
                       //     the timerLightning interceptor (covered by Inc1's
                       //     5-fn suppression set); on receiving this packet,
                       //     spawns AlightningStrike_C locally at the received
                       //     world point. Self-destructs via the actor's own
                       //     Timeline -- no cleanup needed. Payload:
                       //     LightningStrikePayload.
    ItemActivate = 12, // Phase 5F (2026-05-25 NIGHT-3): unified item-activation
                       //     state sync. First instance: flashlight on/off
                       //     (Case b per the RE doc -- the world light cone is
                       //     mainPlayer_C::light_R, NOT a component on the item
                       //     actor). itemClassHash distinguishes flashlight vs
                       //     radio vs torch etc.; senderElementId (v13 -- was
                       //     peerSessionId) identifies the sender's Player
                       //     Element via Registry::Get -> PeerSlot for puppet
                       //     routing; actorKeyHash carries the Aprop_C::Key
                       //     hash for Case-a world props (radio, torch, lamp)
                       //     and is 0 for Case-b player-equipment items.
                       //     (DoorState=9 SHIPPED v27 2026-06-03; IDs 10/11 stay
                       //     reserved for the LightState/LockState follow-ups.)
                       //     Payload: ItemActivatePayload (28 bytes -- v13).
    AssignPeerSlot = 18, // v11: host-only send right after the Connected
                       //     callback fires on the host. Tells the
                       //     freshly-connected client which peer slot
                       //     (1..kMaxPeers-1) it was assigned by the
                       //     host's PollGroup bookkeeping. Client
                       //     receives -> calls
                       //     coop::players::Registry::SetLocalPeerId
                       //     so subsequent outbound messages stamp the
                       //     correct senderElementId for self-echo guards
                       //     in N-peer scope. Audit finding #9.
                       //     v13 (A4 2026-05-29): payload grew to 8B to
                       //     additionally carry the HOST's local Player
                       //     Element id. Client RegisterMirrors that id
                       //     in slot 0 so wire packets stamped with host
                       //     senderElementId resolve to a Player Element
                       //     via Registry::Get on the client. v14 added
                       //     a hostContext byte; v16 PR-FOUNDATION-1b
                       //     dropped it (stale-gen defense moved to
                       //     header senderEpoch). Payload:
                       //     AssignPeerSlotPayload (8 bytes, v16).
    PlayerJoined = 19, // v17 (PR-FOUNDATION Tier 2, host-relay topology):
                       //     HOST-only send. Carries a THIRD peer's identity
                       //     to a client so clients learn about each other
                       //     (not just about the host). MTA shape: mirrors
                       //     CGame::InitialDataStream's two-way broadcast --
                       //     reference/mtasa-blue/Server/.../CGame.cpp:1422
                       //     (BroadcastOnlyJoined) + :1435 (per-existing-peer
                       //     send to the joiner). When the host processes
                       //     client B's Join it (1) sends PlayerJoined{B} to
                       //     every OTHER connected client, and (2) sends
                       //     PlayerJoined{X} to B for every already-known
                       //     client X. Receiver calls EstablishMirrorForSlot
                       //     (slot, eid) + stores the nick, so when B's
                       //     relayed pose arrives (T2-2) the puppet spawns
                       //     into an already-identified slot. Distinct from
                       //     Join (Join = "I, the sender, am here"; PlayerJoined
                       //     = "this OTHER peer exists"), so it carries an
                       //     explicit `slot` field. Variable-length payload
                       //     (parsed field-by-field, no fixed struct -- same
                       //     approach as Join):
                       //       [uint8 slot][uint32 eid][uint8 nicklen][nick UTF-8]
                       //     slot is the described peer's coop::players slot
                       //     (1..kMaxPeers-1; never 0 -- the host is delivered
                       //     via AssignPeerSlot, not PlayerJoined). eid is that
                       //     peer's local Player Element id (peer range,
                       //     validated via IsAllowedPeerAllocatedEid on
                       //     receipt). Stale-gen defense rides the header
                       //     senderEpoch (v16) like every packet.
    PlayerDamage = 20, // vitals Inc3-WIRE (2026-05-31): host-authoritative combat
                       //     damage relay. HOST-only send: the host detects a host-
                       //     side enemy hitting peer N's PUPPET and sends this to
                       //     slot N (via SendReliableToSlot). N's receiver runs
                       //     "Add Player Damage" on its OWN possessed mainPlayer_C
                       //     (per-peer-authoritative: N's private armor/inventory BP
                       //     mitigates) -> N's saveSlot.health drops -> the existing
                       //     Inc1 health stream + Inc3 hurt-flash fire automatically.
                       //     MTA precedent = victim-authoritative damage (the victim
                       //     applies + reports its own resulting health; server
                       //     trusts + relays -- CNetAPI.cpp:1238 / CPlayerPuresync
                       //     Packet.cpp:325). NOT relayable (absent from
                       //     IsClientRelayableReliableKind) + receiver gates
                       //     senderPeerSlot==0 (must-fix #2). Payload:
                       //     PlayerDamagePayload (8 bytes). DETECTION (the real
                       //     enemy->puppet hook) is deferred behind a runtime probe;
                       //     the relay path is e2e-tested via DebugForceHitPuppet.
    BalanceSync = 23,  // 2026-06-04 (v30): host-authoritative SHARED BALANCE. The host
                       //     owns the canonical saveSlot.Points; it POLLS Points each
                       //     game-thread tick (catches every writer -- orders, sells,
                       //     task rewards, the +1000 dev button) and broadcasts this on
                       //     CHANGE (+ to a new client on its connect edge) so every peer
                       //     MIRRORS the host's balance. Receiver (client) writes
                       //     saveSlot.Points = value DIRECTLY (no AddPoints side-effects
                       //     -- the laptop shop reads Points live, so no "ka-ching"/email
                       //     spam on a mirror sync). HOST->client only; not relayable.
                       //     Payload: BalancePayload (4 bytes).
    BalanceDelta = 24, // 2026-06-04 (v30): CLIENT->host balance-change REQUEST. A
                       //     client-side credit (the +1000 dev button; a future shop buy)
                       //     can't write its own mirror -- the next BalanceSync overwrites
                       //     it -- so it sends the signed delta to the host, which applies
                       //     it via mainGamemode::AddPoints(amount); the host's poll then
                       //     re-broadcasts the new total to all. CLIENT->host only; not
                       //     relayable. Payload: BalancePayload.
    KeypadState = 25,  // 2026-06-06 (v35): password-keypad INPUT mirror (ApasswordLock_C). v38: + active (cancel->red).
                       //     SYMMETRIC, MTA input-replication. The keypad verbs are BP-internal
                       //     (bypass our ProcessEvent detour), so the SENDER POLLS each keypad's
                       //     inPassword each tick and broadcasts on a buffer change, keyed by
                       //     AtriggerBase_C::Key. The RECEIVER REPLAYS inputNumber(digit) for the
                       //     typed-buffer delta (native display+beep; Reset() on a clear; upd()
                       //     to repaint) -- which drives the keypad's OWN native validator, so on
                       //     the HOST a client's correct code is accepted natively (no guessed
                       //     output mirror, no unreachable submit verb). isAcc/isDeny were REMOVED
                       //     v35: the door BP disassembly (2026-06-06) proved they are crosshair-
                       //     HOVER flags, not accept state -- mirroring both was the "PURPLE"; the
                       //     door lock keys on the door's own `Active` (DoorState/CanOpen). Host
                       //     relays client edges + connect-snapshots. Payload: KeypadSyncPayload
                       //     (56 B). Own module coop/keypad_sync.
                       //     RE: research/findings/votv-keypad-door-BP-disassembly-2026-06-06.md.
    DoorOpenRequest = 26, // 2026-06-04 (v32): CLIENT->host door open/close REQUEST.
                       //     Doors are now HOST-AUTHORITATIVE (MTA single-syncer model):
                       //     a door's isOpened is re-driven each tick by its LOCAL sensor +
                       //     autoclose, so a SYMMETRIC poll oscillates (host's real player
                       //     holds a door open; the client's door, whose sensor the host-
                       //     player puppet doesn't trip, autocloses -> infinite fight). FIX:
                       //     only the HOST polls + broadcasts DoorState; the CLIENT renders
                       //     it (autoclose suppressed so it sticks) and, when ITS local
                       //     player opens/closes a door, sends THIS request instead of state.
                       //     The host applies it (real lock/jam guards run) -> the host's
                       //     poll picks up the isOpened change -> broadcasts authoritative
                       //     DoorState back to everyone. CLIENT->host only; NOT relayed
                       //     (absent from IsClientRelayableReliableKind); receiver gates
                       //     senderPeerSlot!=0. Payload: KeyedTogglePayload (40 B). MTA
                       //     precedent: CUnoccupiedVehicleSync Packet_UnoccupiedVehiclePush
                       //     -> OverrideSyncer. [[project-coop-interactable-state-sync]].
    SnapshotBegin = 27, // 2026-06-06 (v34): HOST->client connect-snapshot OPEN marker, the
                       //     bracket that lets the joiner draw a determinate loading-screen
                       //     cover. Sent FIRST in prop_snapshot::StartEnumerationFor (before
                       //     the per-tick drain) carrying the enumerated prop candidate count
                       //     as the progress denominator. Rides Lane::Bulk (the SAME lane as
                       //     PropSpawn) so it is strictly ordered BEFORE every PropSpawn it
                       //     introduces. Client (join_progress::BeginSnapshot) flips the cover
                       //     from "Connecting" to a determinate "Receiving world X/N" bar.
                       //     HOST-only send; receiver trust-gates senderPeerSlot==0 + role!=Host;
                       //     NOT relayable. Payload: SnapshotBeginPayload (4 B). MTA precedent:
                       //     CClientGame::NotifyBigPacketProgress establishes the total up front
                       //     (AddToDownloadTotalSize) before the MAP_DOWNLOAD CTransferBox shows.
    SnapshotComplete = 28, // 2026-06-06 (v34): HOST->client connect-snapshot CLOSE marker. Sent
                       //     LAST -- from prop_snapshot's drain-complete path, AFTER the final
                       //     PropSpawn -- on Lane::Bulk, so GNS's in-lane ordering guarantees it
                       //     arrives only once the whole prop stream has landed. THE hide signal:
                       //     the client (join_progress::Complete) lifts the loading-screen cover
                       //     here (the joiner was already placed at the host by the earlier
                       //     Lane::High TeleportClient, behind the cover). Robust against props
                       //     the host skips mid-drain (wire-suppressed / unkeyed) -- the bar may
                       //     top out a hair under 100%, then this snaps it complete (MTA's box
                       //     hides the same way, on the final-fragment edge). HOST-only;
                       //     senderPeerSlot==0 + role!=Host gated; NOT relayable. Payload:
                       //     SnapshotEndPayload (4 B). [[project-host-game-save-picker]].
    TimeSync = 29,     // 2026-06-07 (v36): HOST-authoritative WORLD CLOCK (AdaynightCycle_C).
                       //     The clock (totalTime/Day/TimeScale) is NOT otherwise replicated, so a
                       //     fresh joiner free-runs its own day-0/night -> a DARK client world. The
                       //     host polls its cycle + broadcasts this on a throttle (the clock is
                       //     continuous, so it pushes periodically, not on-change) + on the connect
                       //     edge; the client direct-writes the three floats (the cycle's own
                       //     ReceiveTick then re-derives the sun -- we drive the CLOCK, never the
                       //     sun/light fields). Writing TimeScale lets the client free-run between
                       //     pushes so the sun doesn't step. HOST->client; not a client request, not
                       //     relayed. Payload: TimeSyncPayload (12 B). RE: research/findings/
                       //     votv-coop-class-clone-migration-roadmap-2026-06-06.md §2.
    // Slots 21/22 (HeldClumpGrab/Release) RETIRED 2026-06-03 (v26, RULE 2): the v25
    // hand-attach model for the trash clump was the wrong shape (VOTV carries the
    // clump via the physics grab, floating in front, like the mannequin -- not
    // socketed to the hand). The clump now rides the existing prop pose pipeline
    // (PropSpawn / PropPose / PropRelease) identified by our eid (PropPoseSnapshot.
    // elementId), since it renders on its own (bare spawn = 'dirtball'). The slot IDs
    // stay reserved; the dispatch ignores unknown ReliableKinds + ParseHeader rejects
    // pre-v26 peers. [[project-bug-trash-chippile-uaf-crash]]
    // Slots 16/17 (NonPropEntityState/Destroy) retired 2026-05-27 -- the
    // chipPile/clump/trashBitsPile families now ride the existing Aprop_C
    // pipeline (PropSpawn / PropDestroy / PropPose / PropRelease) via the
    // IsKeyedInteractable extension in ue_wrap::prop. RULE-2 cleanup: do
    // not reassign these IDs to a different packet without bumping the
    // protocol version (peer with older protocol may still send them; the
    // current server-side dispatch ignores unknown ReliableKind values, so
    // forward-compat is intact, but future use should pick fresh IDs).
};

#pragma pack(push, 1)

// Every datagram starts with this. seq is per-sender, monotonically increasing
// (ordering + stale-drop on the receiver; never trust an older seq than the last).
// senderEpoch is the SENDER's per-process session epoch (v16 PR-FOUNDATION-1b):
// minted non-zero by std::random_device at Session::Start(), stamped on every
// outbound header. The receiver latches the first non-zero epoch it sees from
// each peer slot and rejects subsequent packets whose epoch doesn't match --
// defends against stale-generation packets after disconnect/reconnect (an in-
// flight ItemActivate from the previous incarnation can't be honored against
// the new puppet, etc.).
//
// senderSlot (v18 PR-FOUNDATION Tier 2 T2-2, host-relay topology) is the
// LOGICAL origin peer slot of this packet's payload. On a DIRECT send it is
// the sender's own slot (host=0; a client stamps 0 -- don't-care, see below).
// When the HOST RELAYS a client's pose to other clients it REWRITES senderSlot
// to the true origin connection slot (and rewrites senderEpoch to the host's
// own epoch, since the packet now rides the host->client connection whose epoch
// the receiver latched). The receiving CLIENT routes the pose by senderSlot
// (all relayed packets arrive on its single host connection, so the connection
// can't distinguish originators); the receiving HOST ignores senderSlot for its
// own routing and trusts the GNS connection (m_nConnUserData) -- a client
// cannot spoof another peer's slot because the host re-derives it from the
// authenticated connection. The epoch latch ALWAYS keys on the connection slot,
// never senderSlot. _reserved2 is the remaining 3 bytes of the pre-v16 token
// field (0 on send; future use).
struct PacketHeader {
    uint32_t magic;        // kMagic
    uint16_t version;      // kProtocolVersion
    uint8_t  type;         // MsgType
    uint8_t  _pad;         // reserved
    uint32_t seq;          // per-sender sequence number
    uint32_t senderEpoch;  // v16: SENDER's m_ownEpoch (non-zero; 0 reserved as "not latched" sentinel at receiver)
    uint8_t  senderSlot;   // v18: logical origin peer slot (host rewrites on relay; see doc above)
    uint8_t  _reserved2[3];// v18: remaining bytes of the pre-v16 token field; 0 on send
};
static_assert(sizeof(PacketHeader) == 20, "PacketHeader must be 20 bytes");

// Position + view-direction pose. Floats are UE4 cm / degrees (UE4.27's
// FVector/FRotator are float, not double).
//   yaw          -- body horizontal facing (actor.Yaw -- the BODY direction
//                   the puppet's mesh visually points at).
//   pitch        -- controller's view PITCH only. The actor itself never tilts
//                   (player stays upright), so this drives the puppet's HEAD-
//                   BONE look direction via AnimBP_kerfur_headLookAt.
//   headYawDelta -- controller.Yaw - actor.Yaw, in (-180, 180] (the source's
//                   head/camera yaw lead over its body yaw). Drives the puppet
//                   head bone's YAW component in AnimBP_kerfur_headLookAt so
//                   the puppet's head turns to wherever the source is looking
//                   (free-look / camera lead independent of body facing).
//                   The receiver disables the AnimBP's "lookingAtPlayer" path
//                   each tick so this directly controls head orientation
//                   instead of being overwritten by an automated track-local-
//                   player computation.
//   speed        -- horizontal velocity magnitude (cm/s) -> remote AnimBP
//                   locomotion blend.
//   stateBits    -- per-pose flags. Layout:
//                     bit 0 = isInAir (source CMC.MovementMode == MOVE_Falling).
//                             Drives the puppet's BUA-POST observer to clear
//                             useLegIK/rise so the foot-IK trace doesn't plant
//                             the puppet's feet to the satellite's grounded
//                             position while the source is airborne (jump
//                             stretching fix, 2026-05-27).
//                     bit 1 = isRagdoll (v20 Inc2b): source AmainPlayer_C::
//                             isRagdoll AND NOT dead. Set by every ragdoll cause
//                             (manual C-key, exhaustion faint, KO). The receiver
//                             reconciles the puppet (ragdollMode(1,1,0) on the
//                             rising edge, forceGetUp() on the falling edge).
//                             Death is excluded -- it uses the native SP menu
//                             flow + ends the session, never a synced pose flag.
//                     bits 2..7 reserved (crouched, ...).
struct PoseSnapshot {
    float   x, y, z;
    float   yaw;
    float   pitch;
    float   headYawDelta;
    float   speed;
    uint8_t stateBits;
    // v19 (2026-05-30): vitals piggyback in the 3 bytes that were `_pad` (ZERO
    // size change -- PoseSnapshot stays 32 B). Per-peer-authoritative: each peer
    // packs its OWN vitals off this machine's UsaveSlot_C (ue_wrap::vitals).
    // DISPLAY-ONLY on the receiver (puppet nameplate health bar); NEVER written
    // back to a saveSlot (one per machine -> would corrupt the local player's
    // persisted health). Continuous + lossy: reliable DEAD/ragdoll authority is
    // a later increment and these bytes never trigger a state transition.
    uint8_t healthFrac;  // health / maxHealth, quantized 0..255 (maxHealth is per-peer)
    uint8_t foodFrac;    // food  / kVitalScalarMax, quantized 0..255
    uint8_t sleepFrac;   // sleep / kVitalScalarMax, quantized 0..255
};
static_assert(sizeof(PoseSnapshot) == 32, "PoseSnapshot must be 32 bytes");

// PoseSnapshot.stateBits flags. Single-byte field; flags assigned bit-by-bit.
inline constexpr uint8_t kStateBitInAir   = 0x01;
inline constexpr uint8_t kStateBitRagdoll = 0x02;  // v20 Inc2b: source is ragdolled (faint/manual-C/KO, NOT dead)

// v19: VOTV vital scalars (food, sleep, and the default maxHealth) top out at
// 100.0. `health` is normalized by the per-peer `maxHealth` (upgrades/story can
// raise it) BEFORE quantization; food/sleep normalize by this constant.
inline constexpr float kVitalScalarMax = 100.0f;

// Encode a [0,1] fraction as a byte (round-to-nearest, clamped). The exact
// inverse pair lives here so sender + receiver can never drift.
inline uint8_t QuantizeUnitFraction(float f01) {
    if (f01 <= 0.f) return 0;
    if (f01 >= 1.f) return 255;
    return static_cast<uint8_t>(f01 * 255.f + 0.5f);
}
inline float DequantizeUnitFraction(uint8_t b) {
    return static_cast<float>(b) * (1.f / 255.f);
}

struct PosePacket {
    PacketHeader header;
    PoseSnapshot pose;
};
static_assert(sizeof(PosePacket) == 52, "PosePacket must be 52 bytes");

// PR-2 (2026-05-28): PingPacket deleted -- GameNetworkingSockets surfaces RTT
// via GetConnectionRealTimeStatus (sampled once per second from the net thread);
// the hand-rolled Ping/Pong round-trip went with it.

// A reliable message: standard header (type=Reliable, seq=the reliable seq) +
// ReliableHeader + variable UTF-8 payload. relSeq is a SEPARATE counter from the
// header seq space (the unreliable pose seq); it lives in the header's seq field
// for reliable packets and is what the ack references.
struct ReliableHeader {
    uint8_t  kind;     // ReliableKind
    uint8_t  _pad[3];
    uint16_t payloadLen;
    uint16_t _pad2;
};
static_assert(sizeof(ReliableHeader) == 8, "ReliableHeader must be 8 bytes");

// v4: FName carrier. The Aprop_C.Key save UUIDs are ~22 ASCII chars; 31 + null
// gives ample headroom without going to variable-length wire encoding. Empty
// keys (len=0) reserved for "not set" / sentinel. Bytes beyond `len` MUST be
// zero on the wire so equality compare works.
struct WireKey {
    uint8_t len;       // 0..31 (chars in `data`)
    char    data[31];  // UTF-8 (Aprop_C Keys are ASCII)
};
static_assert(sizeof(WireKey) == 32, "WireKey must be 32 bytes");

// v5: BP class leaf name carrier (e.g. "Aprop_equipment_flashlight_C"). Longest
// VOTV class names hit ~45 chars; 63 + length gives ample headroom. Same fixed-
// size approach as WireKey to keep the payload at a fixed offset (no variable-
// length parsing). Bytes beyond `len` MUST be zero on the wire so receiver
// FindClass sees a clean wide string.
struct WireClassName {
    uint8_t len;       // 0..63 chars in `data`
    char    data[63];  // ASCII (VOTV class names are ASCII)
};
static_assert(sizeof(WireClassName) == 64, "WireClassName must be 64 bytes");

// v4: held-prop world transform. Sent unreliable, ~sendHz, while the sender's
// mainPlayer.grabbing_actor is non-null. Receiver's first packet for a new key:
// FindByKeyString -> local Aprop_C*, SetSimulatePhysics(false) on its StaticMesh,
// cache the pointer. Subsequent packets: SetActorLocation+Rotation on the
// cached actor. Stream-stops (>500 ms gap) treated as implicit release.
struct PropPoseSnapshot {
    WireKey key;        // 32 -- which prop (cross-peer stable string)
    float   x, y, z;    // world cm
    float   pitch;
    float   yaw;
    float   roll;
    // v26 (2026-06-03): elementId carries the SENDER's Prop Element id so the
    // receiver can resolve the mirror by EID when the BP Key is None/unstable. The
    // trash clump (prop_garbageClump_C) is non-keyable (setKey doesn't stick -- the
    // source re-reads None), so its PropPose key is always None; the receiver matches
    // its mirror by this eid instead (KEY first, then eid fallback). 0 = "no eid"
    // (legacy keyed prop). Mirrors the mannequin pose path but with our own identity.
    // [[project-bug-trash-chippile-uaf-crash]]
    uint32_t elementId;
};
static_assert(sizeof(PropPoseSnapshot) == 60, "PropPoseSnapshot must be 60 bytes (v26: +elementId)");

struct PropPosePacket {
    PacketHeader     header;  // 20
    PropPoseSnapshot pose;    // 60
};
static_assert(sizeof(PropPosePacket) == 80, "PropPosePacket must be 80 bytes");

// v37: ONE NPC's pose in the EntityPose batch. Keyed by the Npc Element id (NPCs have
// no stable BP key -- their identity is the host-allocated elementId, like EntitySpawn).
// The host reads it from the live NPC's transform + CMC velocity each send tick; the
// client interpolates it (the element::Npc RemotePlayer-twin) and drives the mirror's CMC
// so its own AnimBP animates. Increment 1 = pose; v39 adds head-look (health/isDead are inc2).
struct EntityPoseSnapshot {
    uint32_t elementId;   // 4  -- Npc Element id (host range)
    float    x, y, z;     // 12 -- world cm (actor location = ACharacter capsule centre = pivot)
    float    yaw;         // 4  -- actor yaw deg (NormalizeAxis'd)
    float    speed;       // 4  -- horizontal velocity magnitude cm/s (drives the locomotion blend)
    float    lookAtX, lookAtY, lookAtZ;  // 12 -- v39: kerfur AnimBP `lookAt` WORLD target (head/neck
                          //      FAnimNode_LookAt aim point). VALID iff stateBits has kEntityPoseBitHasLookAt
                          //      (only kerfur-family NPCs carry it; non-kerfur NPCs leave it zero + the bit clear).
    float    bodyYaw;     // 4  -- v40: kerfur VISIBLE body (ACharacter::Mesh) WORLD yaw (deg). The actor BP
                          //      aims the mesh at the local player decoupled from the actor root; we mirror it.
                          //      VALID iff stateBits has kEntityPoseBitHasBodyYaw (kerfur-family only).
    uint8_t  stateBits;   // 1  -- bit0=inAir, bit1=hasLookAt, bit2=hasBodyYaw
    uint8_t  _pad[3];     // 3  -- 4-byte alignment
};
static_assert(sizeof(EntityPoseSnapshot) == 44, "EntityPoseSnapshot must be 44 bytes");

// EntityPoseSnapshot.stateBits flags. bit 0 reuses kStateBitInAir (same numeric/meaning as
// PoseSnapshot). bits 1-2 are EntityPose-specific (the PoseSnapshot 0x02 = kStateBitRagdoll does
// not apply to NPCs -- distinct struct, distinct meaning).
inline constexpr uint8_t kEntityPoseBitHasLookAt = 0x02;  // v39: lookAt{X,Y,Z} carries a valid head-look world target
inline constexpr uint8_t kEntityPoseBitHasBodyYaw = 0x04;  // v40: bodyYaw carries a valid visible-body world yaw

// v37: header of an EntityPose datagram -- N follows. N is derived from the datagram length
// (count is the authoritative entry count, validated <= kMaxNpcBatchEntries by the receiver).
struct EntityPoseBatchHeader {
    uint8_t count;       // 1  -- NPC entries that follow (0..kMaxNpcBatchEntries)
    uint8_t _pad[3];     // 3
};
static_assert(sizeof(EntityPoseBatchHeader) == 4, "EntityPoseBatchHeader must be 4 bytes");

// Max NPCs per EntityPose datagram, MTU-capped: (1400 - PacketHeader(20) - BatchHeader(4)) / 44 = 31.
// More NPCs than this in one tick truncate (logged); the realistic coop NPC count fits.
inline constexpr int kMaxNpcBatchEntries = 31;

// Worst-case EntityPose datagram size (full batch). Sizes both the send-loop
// stack buffer (session.cpp) and Session::SerializeLocalNpcBatch's output
// contract (session_npc.cpp). PacketHeader(20) + EntityPoseBatchHeader(4) +
// 31 * EntityPoseSnapshot(44) = 1388 bytes (< 1400 MTU).
inline constexpr int kNpcPoseDatagramMax =
    static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader)) +
    kMaxNpcBatchEntries * static_cast<int>(sizeof(EntityPoseSnapshot));

// v22: ragdoll PELVIS physics state. Sent unreliable, ~sendHz, WHILE the sender's
// AmainPlayer_C::isRagdoll is set (the native C-key/faint/KO ragdoll). The sender
// reads its own ragdollActor's SkeletalMesh pelvis bone:
//   x,y,z              -- pelvis WORLD location (cm). Receiver uses this for a
//                         drift watchdog (mirror body pelvis vs streamed) -- not a
//                         hard correction in v22 (the velocity slaving keeps them
//                         tracking from a co-located ragdoll start; a teleport-
//                         correct of a simulating skeletal body is a follow-up if
//                         hands-on shows drift).
//   pitch,yaw,roll     -- pelvis WORLD rotation (deg). Receiver drives the puppet's
//                         rotation from THIS directly (exact -- the visible Dr. Kel
//                         is rigidly pelvis-attached, so its tumble orientation is
//                         the pelvis orientation), replacing the read-off-the-local-
//                         sim-body rotation that could diverge from the client's.
//   linVel{X,Y,Z}      -- pelvis linear velocity (cm/s), GetPhysicsLinearVelocity.
//   angVel{X,Y,Z}      -- pelvis angular velocity (deg/s),
//                         GetPhysicsAngularVelocityInDegrees.
// Receiver applies linVel+angVel to its mirror body's pelvis each packet
// (SetPhysicsLinearVelocity + SetPhysicsAngularVelocityInDegrees, bAddToCurrent=0)
// so the mirror body's gross motion slaves to the sender's real ragdoll. Same
// FVector cm/s + FVector deg/s units + capture/apply UFunction pair as
// PropReleasePayload -- the proven prop launch-energy path.
struct RagdollPoseSnapshot {
    float x, y, z;                    // pelvis world location (cm)
    float pitch, yaw, roll;           // pelvis world rotation (deg)
    float linVelX, linVelY, linVelZ;  // pelvis linear velocity (cm/s)
    float angVelX, angVelY, angVelZ;  // pelvis angular velocity (deg/s)
};
static_assert(sizeof(RagdollPoseSnapshot) == 48, "RagdollPoseSnapshot must be 48 bytes");

struct RagdollPosePacket {
    PacketHeader        header;  // 20
    RagdollPoseSnapshot pose;    // 48
};
static_assert(sizeof(RagdollPosePacket) == 68, "RagdollPosePacket must be 68 bytes");

// v5: PropRelease reliable payload. Sent ONCE when the sender's grab ends.
// Receiver re-enables SimulatePhysics on the cached prop, then sets linear +
// angular velocity (the body's INHERITED PhysX state at release: tracking
// velocity from the kinematic chase + any post-release AddImpulse boost the
// engine applied -- ONE number captures the body's full launch state). Carries
// the Key so a release that arrives out of order vs the last PropPose still
// finds the right prop. Fires Aprop_C.thrown(localPlayer) when |linVel| exceeds
// the throw threshold so the BP's natural whoosh + particle trail dispatch.
struct PropReleasePayload {
    WireKey key;
    float   linVelX;   // cm/s -- GetPhysicsLinearVelocity at release
    float   linVelY;
    float   linVelZ;
    float   angVelX;   // deg/s -- GetPhysicsAngularVelocityInDegrees at release
    float   angVelY;
    float   angVelZ;
};
static_assert(sizeof(PropReleasePayload) == 56, "PropReleasePayload must be 56 bytes");
// Reliable-fit guard: if the payload ever grows past one datagram's reliable
// budget, ReliableChannel::Send silently rejects (returns false) -- catch this
// at compile time. Audit-added 2026-05-24 (Bug B audit issue #4).
static_assert(sizeof(PropReleasePayload) <= 256 - 20 - 8,
              "PropReleasePayload must fit in one reliable datagram (kMaxReliablePayload)");

// Velocity magnitude (cm/s) above which a release is classified as a THROW
// (vs a passive drop) on the receiver -- fires Aprop_C.thrown(localPlayer) so
// the BP's whoosh sound + particle trail dispatch. 200 cm/s = 2 m/s, well above
// the residual velocity from a walking drop (camera moves ~30 cm/s at walk,
// rarely transferring even half that to a held prop) and well below any
// deliberate flick-throw. Calibrate vs hands-on if hover threshold matters.
inline constexpr float kThrownLinVelThreshold = 200.f;

// v5: PropSpawn reliable payload. Sent ONCE when the sender drops an inventory
// item into the world (POST-hook on UpropInventory_C::takeObj catches all 4
// drop paths -- simulateDrop A-D -- via the one bottom call they all funnel
// through). Per [[project-coop-inventory-private]] we do NOT serialize the
// full Fstruct_save (inventory contents are private); only the world-spawn
// identity + transform + initial physics state crosses. The Key string is
// the persistent cross-peer identifier (saved with the prop at pickup time;
// restored on the dropped instance via Aprop_C.loadData on the sender side
// and Aprop_C.setKey on the receiver side) so subsequent PropPose updates
// resolve normally via prop_wrap::FindByKeyString.
//
// Physics flags bits (physFlags):
//   bit 0: bSimulatePhysics  (always 1 for inventory drops -- the dropped
//                              prop is a simulating rigid body)
//   bit 1: bIsHeavy           (data-driven from Aprop_C.propData.heavy; the
//                              receiver may use this for grab-path priming)
//   bit 2: bFrozen            (Aprop_C.frozen -- quest-locked containers)
//   bits 3-7: reserved
struct PropSpawnPayload {
    WireClassName className;       // 64 -- "Aprop_equipment_flashlight_C" etc.
    WireKey       key;             // 32 -- the persistent cross-peer Key
    float         locX, locY, locZ;            // 12 -- world cm
    float         rotPitch, rotYaw, rotRoll;   // 12 -- FRotator (matches PropPose shape)
    float         scaleX, scaleY, scaleZ;      // 12 -- usually (1,1,1)
    uint8_t       physFlags;        // 1
    uint8_t       chipType;         // 1 (v28: trash-clump variant selector enum_chipPileType; 0 for non-trash props)
    uint8_t       _pad[2];          // 2 (v16: senderContext byte removed; v28: 1 byte -> chipType)
    float         initLinVelX, initLinVelY, initLinVelZ;  // 12 -- usually (0,0,0); v29: the
                                  //     clump's landing velocity (cm/s) when physFlags has
                                  //     kFreshLanded, fed to turnToPile for impact dust direction
    // (initAngVel follows; unused by the landed-pile path)
    float         initAngVelX, initAngVelY, initAngVelZ;  // 12
    // v12: ElementId of the prop in the SENDER's allocation range.
    //   Host sender -> elementId in host range [1, kHostRangeSize).
    //   Client sender (non-Aprop_C interactables only -- chipPile/clump/
    //     trashBits broadcast client->host; Aprop_C lineage is host-
    //     authoritative and doesn't broadcast from client) -> elementId
    //     in peer range [kHostRangeSize, kMaxElements).
    //   0 = "sender had no Element minted" (legacy / unkeyed / convergence
    //     window before MarkPropElement fired).
    // Receiver RegisterMirror()s this id to its Registry (A2 2026-05-29);
    // the slots are disjoint between ranges so host self-echo bounces
    // collide-fail RegisterMirror harmlessly (defensive).
    uint32_t      elementId;        // 4
    uint32_t      _pad2;            // 4 -- 8-byte alignment
};
static_assert(sizeof(PropSpawnPayload) == 168, "PropSpawnPayload must be 168 bytes");
static_assert(sizeof(PropSpawnPayload) <= 256 - 20 - 8,
              "PropSpawnPayload must fit in one reliable datagram");

namespace propspawn_flags {
inline constexpr uint8_t kSimulatePhysics = 0x01;
inline constexpr uint8_t kIsHeavy         = 0x02;
inline constexpr uint8_t kFrozen          = 0x04;
// v29: this PropSpawn is a freshly-landed trash pile (the owner's thrown clump just
// re-piled into it). The receiver dispatches AactorChipPile_C::turnToPile(initLinVel)
// on the spawned mirror so it plays the impact dust+sound (the bare spawn is otherwise
// silent). Set ONLY by trash_collect_sync::BroadcastLandedPileNear; never on a connect
// snapshot (so a late joiner can't replay a landing-sound storm). [[project-bug-trash-chippile-uaf-crash]]
inline constexpr uint8_t kFreshLanded     = 0x08;
}  // namespace propspawn_flags

// v5 Phase 5S0 Inc2: prop-destroy reliable payload. WireKey identifies the
// prop on the receiver via prop_wrap::FindByKeyString -> K2_DestroyActor.
// Tiny (32 bytes) -- no transform/state needed; destruction is just "this
// Key's prop is gone". Sender's K2_DestroyActor PRE observer captures the
// Key just before the engine destroys the actor; the receiver's
// K2_DestroyActor call on its local actor is echo-suppressed via the
// incoming-destroy-set so it doesn't bounce back.
struct PropDestroyPayload {
    WireKey  key;
    // v12 + A2 (2026-05-29): elementId in the SENDER's allocation range
    // (host range from host sender, peer range from client sender).
    // 0 = "sender had no Element" (matches PropSpawnPayload contract).
    // Receiver UnregisterMirror()s this id to drain the wire-identity
    // binding made by the matching PropSpawn (the engine actor is
    // resolved+destroyed via the key path; eid is identity bookkeeping).
    uint32_t elementId;       // 4
    uint32_t _pad;            // 4 -- 8-byte alignment (v16: senderContext + pad coalesced)
};
static_assert(sizeof(PropDestroyPayload) == 40, "PropDestroyPayload must be 40 bytes");
static_assert(sizeof(PropDestroyPayload) <= 256 - 20 - 8,
              "PropDestroyPayload must fit in one reliable datagram");

// KeyedTogglePayload -- the SHARED payload for every "keyed interactable open/
// close/on-off state" reliable kind: DoorState (9), LightState (10),
// ContainerState (11). Phase 5D (2026-06-03, v27). A peer toggled an
// interactable (a base door, a light-switch group, or a container/cabinet lid);
// broadcast the resulting boolean state so every peer's matching instance agrees.
// Identity = the instance's cross-peer-stable Key FName (AtriggerBase_C::Key for
// doors + light-roots; Aprop_C::Key for swinger lids). The receiver resolves the
// live actor by Key and idempotently applies. coop::interactable_sync drives all
// three via one generic Channel (no per-feature duplication, RULE 2). RE:
// research/findings/votv-doors-and-lightswitches-RE-2026-05-25.md.
struct KeyedTogglePayload {
    WireKey  key;        // 32 -- the instance's Key FName (string)
    uint8_t  action;     // 1  -- 0 = closed/off, 1 = open/on (the state AFTER the edge)
    uint8_t  _pad[7];    // 7  -- 8-byte alignment / reserved
};
static_assert(sizeof(KeyedTogglePayload) == 40, "KeyedTogglePayload must be 40 bytes");
static_assert(sizeof(KeyedTogglePayload) <= 256 - 20 - 8,
              "KeyedTogglePayload must fit in one reliable datagram");

// KeypadSyncPayload -- the password-keypad INPUT mirror (KeypadState=25, v35). Carries only
// the typed digit buffer of ONE keypad. The sender (any peer) polls every indexed
// ApasswordLock_C each tick and broadcasts this on a buffer change; the receiver REPLAYS the
// buffer via inputNumber(digit), which drives the keypad's OWN native validator (and on the
// host that is what accepts the code -- MTA input-replication, not a guessed output mirror).
// Identity = AtriggerBase_C::Key. Relayed by the host (symmetric); the connect-snapshot seeds
// a joiner. The accept/deny bools that used to ride here (isAcc/isDeny) were REMOVED in v35:
// the door BP disassembly (2026-06-06) proved isAcc/isDeny are crosshair-HOVER flags, not
// accept state -- mirroring both onto a keypad produced the non-native green+red "PURPLE" the
// user reported, and the door lock keys on the door's own `Active`, not the keypad's isAcc.
// RE: research/findings/votv-keypad-door-BP-disassembly-2026-06-06.md.
struct KeypadSyncPayload {
    WireKey  key;        // 32 -- the keypad's Key FName (string)
    uint8_t  bufLen;     // 1  -- digits in `buf` (0..16; codes are short)
    uint8_t  buf[16];    // 16 -- the typed digits, one per byte (each 0..9)
    uint8_t  active;     // 1  -- v38: keypad `active` @0x0330 (LED selector: 0=red/locked, 1=green/powered; mirrors cancel->red)
    uint8_t  _pad[6];    // 6  -- 8-byte alignment / reserved (was isAcc/isDeny + pad; removed v35)
};
static_assert(sizeof(KeypadSyncPayload) == 56, "KeypadSyncPayload must be 56 bytes");
static_assert(sizeof(KeypadSyncPayload) <= 256 - 20 - 8,
              "KeypadSyncPayload must fit in one reliable datagram");

// Phase 5N1 Inc2 (2026-05-25, updated 2026-05-28 Tier 3 PoC): NPC spawn
// reliable payload. Host detects an NPC instantiation via the host-side
// PRE interceptor on BeginDeferredActorSpawnFromClass; POST observer on the
// same UFunction captures the spawned AActor* and binds it to the just-
// allocated Npc Element. Payload carries the class name + the Npc Element's
// ElementId + the spawn world transform. Receiver (Inc3 client mirror --
// not yet wired):
//   MarkIncomingNpcSpawn(cls)  // bypass the suppressor for this one call
//   actor = BeginDeferredActorSpawnFromClass(cls, transform)
//   FinishSpawningActor(actor, transform)  // BeginPlay runs naturally
//   g_npcMirrorByElementId[elementId] = actor  // keyed by ElementId now
//
// `sessionId` field name retained through this wave for wire compatibility;
// renamed to `elementId` at the v12 protocol bump. Semantics now:
// host-allocated via `coop::element::Registry::AllocHostId` from the host
// range [1, 32768). Value 0 is reserved as the wire-invalid sentinel
// (Registry will never emit 0). Receiver references the same id for
// subsequent EntityPose (Inc3) and EntityDestroy.
struct EntitySpawnPayload {
    WireClassName className;       // 64 -- "npc_zombie_C", "kerfurOmega_mannequin_C", etc.
    uint32_t      elementId;       // 4 -- v12 (was `sessionId`): host-allocated, [1, 32768); 0 = invalid
    uint32_t      _pad;            // 4 -- 8-byte alignment for the floats
    float         locX, locY, locZ;            // 12 -- world cm at spawn time
    float         rotPitch, rotYaw, rotRoll;   // 12 -- FRotator
};
static_assert(sizeof(EntitySpawnPayload) == 96, "EntitySpawnPayload must be 96 bytes");
static_assert(sizeof(EntitySpawnPayload) <= 256 - 20 - 8,
              "EntitySpawnPayload must fit in one reliable datagram");

// Phase 5N1 Inc2: NPC destroy reliable payload. Host detects an NPC
// destruction via the existing AActor::K2_DestroyActor PRE observer
// (filter by host-side tracked-NPC set lookup). Tiny -- just the
// sessionId to identify which mirror to tear down. Receiver: look up
// g_npcMirrorBySessionId[sessionId] -> K2_DestroyActor on the mirror.
// Same incoming-destroy-set echo suppression pattern as PropDestroy.
struct EntityDestroyPayload {
    uint32_t elementId;  // v12 (was `sessionId`): host-allocated, [1, 32768); 0 = invalid
    uint32_t _pad;       // 8-byte alignment
};
static_assert(sizeof(EntityDestroyPayload) == 8, "EntityDestroyPayload must be 8 bytes");
// Audit-fix Issue 1 (2026-05-25): tripwire for future growth. At 8 bytes
// this trivially fits, but every reliable payload in this file has the
// guard; absence breaks the established consistency.
static_assert(sizeof(EntityDestroyPayload) <= 256 - 20 - 8,
              "EntityDestroyPayload must fit in one reliable datagram (kMaxReliablePayload)");

// Phase 5G Inc 2 (NonPropEntity* packets) retired 2026-05-27 -- chipPile /
// clump / trashBitsPile families now ride the existing Aprop_C pipeline
// (PropSpawn / PropDestroy / PropPose / PropRelease) via the
// IsKeyedInteractable extension in ue_wrap::prop. The separate non-prop
// entity sync infrastructure was a misread of the BP class layout: even
// though those classes don't derive from Aprop_C in C++, they expose the
// same BP interaction protocol (GetKey, canBeUsedHold, playerTryToHold,
// ...) and the existing wire layer covers them with minor extensions.

// 2026-05-25 LATE +5h dev feature F4: teleport client to host's pose.
// Host snapshots its own mainPlayer Location + Rotation and sends; client
// applies via K2_TeleportTo. NaN/Inf rejected at the receiver before the
// engine call (same trust-boundary defensive pattern as PropRelease velocity
// bounds). Direction: host->client only; the receiver no-op's if it is the
// host itself (a stale loopback would teleport host to its own pose, harmless
// but pointless). [dev] devkeys=1 gated on the sender side.
struct TeleportClientPayload {
    float locX, locY, locZ;        // 12 -- world cm
    float rotPitch, rotYaw, rotRoll; // 12 -- degrees
};
static_assert(sizeof(TeleportClientPayload) == 24, "TeleportClientPayload must be 24 bytes");
static_assert(sizeof(TeleportClientPayload) <= 256 - 20 - 8,
              "TeleportClientPayload must fit in one reliable datagram");

// RestoreVitals (ReliableKind = 7) carries NO payload. The action is fixed:
// receiver max-outs food/sleep/health/coffeePower on its local UsaveSlot_C.

// ItemActivate (ReliableKind = 12) -- Phase 5F flashlight + future radio/torch/
// lamp / etc. Single unified packet for "item state with WORLD EFFECT changed"
// per the project-coop-inventory-private carve-out. RE docs:
//   research/findings/votv-flashlight-RE-2026-05-25.md
//
// Two cases the same packet covers:
//
//  Case (b) -- player equipment with effect on the player actor.
//    Example: Aprop_equipment_flashlight_C / _b_C. The world light is
//    mainPlayer_C::light_R @0x0678 (the puppet has it too). Sender hooks
//    AmainPlayer_C::updateFlashlight POST + sends. flags.has_actor_key=0,
//    actorKeyHash=0. Receiver: find puppet by peerSessionId, write
//    puppet.flashlight @0x0838 + toggle puppet.light_R visibility.
//
//  Case (a) -- world prop with own light/audio component.
//    Example: Aprop_radio_C (bool A toggles MediaSound), Aprop_torch_C
//    (bool burning + ignite/extinguishFire UFunctions), Aprop_lamp_C.
//    Sender hooks the appropriate toggle UFunction + sends.
//    flags.has_actor_key=1, actorKeyHash=CRC32(prop.Key string).
//    Receiver: lookup actor by actorKeyHash via a class-specific table
//    built at session connect, apply state.
//
// itemClassHash = CRC32 of the item UClass FName string (e.g.
//   "prop_equipment_flashlight_C"). Cross-peer stable because UClass
//   FNames are deterministic from the cooked content.
//
// v6 fields (2026-05-26 PM, Phase 5F user feedback after Option α landed):
//   intensity, outerConeAngle, innerConeAngle -- snapshot the sender's exact
//   light_R cone shape AFTER the BP toggle ran. Receiver applies via
//   SetIntensity / SetOuterConeAngle / SetInnerConeAngle UFunctions so the
//   puppet mirrors brightness AND focused-vs-spread mode without any
//   mode-byte-to-cone-angle mapping (data-driven).
//   mode -- the mp.flashlightMode value AFTER the toggle (puppet does NOT
//   write this byte to avoid hitting any BP listener tied to the field; mode
//   is carried for future telemetry / non-cone effects).
struct ItemActivatePayload {
    uint32_t itemClassHash;   // CRC32 of item UClass FName string (cross-peer stable)
    // v13 (A4 2026-05-29): was `uint8_t peerSessionId`. Now sender's
    // local Player Element id (host range from host, peer range from
    // client). Receiver resolves via coop::element::Registry::Get ->
    // coop::element::Player::PeerSlot() for routing into the per-puppet
    // pending-apply state map; compares to own local Player Element id
    // for the self-echo guard. 0 = "sender had no Player Element yet"
    // (boot/seed window before handshake completed); receiver falls
    // back to msg.senderPeerSlot for routing in that case.
    uint32_t senderElementId;
    uint8_t  state;           // 0 = off / inactive, 1 = on / active
    uint8_t  flags;           // bit0: has_actor_key (1 = use actorKeyHash)
    uint8_t  mode;            // v6: mp.flashlightMode (0=default spread, 1=focused, ...)
    uint8_t  _pad;            // 1 (v16: senderContext removed; reverted to pad as in v13)
    uint32_t actorKeyHash;    // CRC32(Aprop_C::Key string) when flags.has_actor_key=1; 0 otherwise
    float    intensity;       // v6: light_R.Intensity AFTER BP ran (Unitless scale ~0..10)
    float    outerConeAngle;  // v6: light_R.OuterConeAngle (degrees; ~40 default, ~12 focused)
    float    innerConeAngle;  // v6: light_R.InnerConeAngle (degrees; ~0 default, varies)
};
static_assert(sizeof(ItemActivatePayload) == 28,
              "ItemActivatePayload must be exactly 28 bytes (v16 wire-format)");
static_assert(sizeof(ItemActivatePayload) <= 256 - 20 - 8,
              "ItemActivatePayload must fit in one reliable datagram");

// flags bits for ItemActivatePayload.flags
inline constexpr uint8_t kItemActivateFlag_HasActorKey = 0x01;

// vitals Inc3-WIRE (2026-05-31): host-authoritative combat damage relay. The host
// stamps `targetElementId` = the OWNER peer's Player Element id (Registry::
// GetPlayerElement(ownerSlot)->GetId()) so the receiver self-verifies it is the
// addressed peer (targetElementId == Registry::LocalPlayerElementId()) before running
// the damage on its OWN possessed player. `damage` is the raw hit amount; the owner's
// own "Add Player Damage" BP applies its private armor/mitigation. Host-only send;
// receiver gates senderPeerSlot==0; not relayable.
struct PlayerDamagePayload {
    uint32_t targetElementId;  // the OWNER peer's Player Element id (host-stamped)
    float    damage;           // raw hit amount; owner BP mitigates per its inventory
};
static_assert(sizeof(PlayerDamagePayload) == 8,
              "PlayerDamagePayload must be exactly 8 bytes (v21 wire-format)");
static_assert(sizeof(PlayerDamagePayload) <= 256 - 20 - 8,
              "PlayerDamagePayload must fit in one reliable datagram");

// BalanceSync (23) / BalanceDelta (24) -- shared host-authoritative Points balance
// (2026-06-04, v30). One int32: the absolute TOTAL (BalanceSync, host->client mirror)
// OR a signed DELTA to apply (BalanceDelta, client->host request).
struct BalancePayload {
    int32_t value;
};
static_assert(sizeof(BalancePayload) == 4, "BalancePayload must be exactly 4 bytes");

// (v25 HeldClumpGrabPayload / HeldClumpReleasePayload RETIRED 2026-06-03 / v26,
// RULE 2 -- the hand-attach clump model was replaced by the prop pose pipeline
// keyed by PropPoseSnapshot.elementId. See the retired ReliableKind 21/22 note.)

// Host -> client slot assignment. Sent once right after the Connected
// callback fires on host (a Reliable single-target message). Client
// uses slot to call coop::players::Registry::SetLocalPeerId so the
// rest of the codebase (item_activate self-echo guard, future
// per-puppet addressing) sees the real peer id rather than a hardcoded
// 1v1 default.
//
// v13 (A4 2026-05-29): hostElementId added. Carries the HOST's local
// Player Element id (host range [1, kHostRangeSize)). The client calls
// players::Registry::EstablishMirrorForSlot(0, hostElementId) so the
// host's Player Element is materialized as a MIRROR in the client's
// element::Registry at the same id the host uses. After mirror
// creation, Registry::Get(hostElementId) on the client resolves to a
// coop::element::Player whose PeerSlot()==0. Wire packets bearing the
// host's senderElementId (ItemActivate / Weather / RedSky / Lightning)
// can then be routed and trust-validated symmetrically on both peers.
struct AssignPeerSlotPayload {
    uint8_t  slot;            // 1..kMaxPeers-1
    uint8_t  _pad[3];         // (v16: hostContext byte removed; reverted to pad as in v13)
    uint32_t hostElementId;   // v13: host's local Player Element id
};
static_assert(sizeof(AssignPeerSlotPayload) == 8,
              "AssignPeerSlotPayload must be exactly 8 bytes (v16)");

// Phase 5W Inc1 (2026-05-26): host-authoritative weather state push. The host
// reads these fields off the live AdaynightCycle_C after its own scheduler
// UFunction runs; client receives + applies via the cycle's mutator
// UFunctions. See protocol.h's ReliableKind::WeatherState doc above and
// research/findings/votv-weather-DESIGN-2026-05-26.md for the field-by-field
// derivation. v7 stamped peerSessionId=0 for host validation; v13 (A4)
// switches to senderElementId resolved via Registry::Get -> Player.PeerSlot
// for the host trust-boundary check.
//
// `flags` bits (mapped one-to-one with the AdaynightCycle_C boolean fields).
// The receiver applies each by:
//   - writing the literal bit to the matching field offset (config bits with
//     no BP listeners), THEN
//   - dispatching the right APPLY UFunction so any subscribers fan out.
// E.g. isRaining bit is applied via causeRain(bool) so the BP-side particle
// system + audio cue start; isSnow bit via intComs_triggerSnow(bool) so the
// 53 listeners fan out (RE doc).
//   bit 0: isRaining          @0x02E4   (apply: causeRain(bool) UFunction)
//   bit 1: isSnow             @0x03B0   (apply: intComs_triggerSnow(bool))
//   bit 2: enable_rain        @0x044B   (config bool; direct write)
//   bit 3: enable_fog         @0x0449   (config bool)
//   bit 4: enable_superfog    @0x044A   (config bool)
//   bit 5: enableSunlight     @0x03D8   (config bool)
//   bit 6: enableMoonlight    @0x0448   (config bool)
//   bit 7: permanentRain      @0x042C   (config bool)
//
// Wind: NOT a separate field block on the wire. AdaynightCycle_C's
// setWindParameters() is no-arg -- it reads internal cycle state and
// propagates to AdirectionalWind_C. The only wind-state field we need to
// sync is rainWindSpeed @0x041C (already in payload via the rain block);
// the receiver writes it then calls setWindParameters() so the wind actor
// updates. A SEPARATE AdirectionalWind_C::setParameters wire path would
// be a parallel sync mechanism for the same state (RULE 2 violation).
struct WeatherStatePayload {
    // v13 (A4 2026-05-29): was uint8_t peerSessionId. Now sender's
    // local Player Element id. Receiver checks
    // Registry::Get(senderElementId) -> Player::PeerSlot() == 0 for the
    // host trust-boundary; drops if mirror missing OR PeerSlot != 0.
    uint32_t senderElementId;
    uint8_t  flags;              // see weather_flags bit layout above
    uint8_t  flags2;             // v23: fog_flags2 (active-fog actor presence; see below)
    uint8_t  _pad[2];            // (v23: flags2 reused 1 of the 3 v16 pad bytes) 4-byte align the float block
    float    rainStrength;        // AdaynightCycle_C::rainStrength @0x0404
    float    rainLightningChance; // AdaynightCycle_C::rainLightningChance @0x0408
    float    rainDeactivateChance;// AdaynightCycle_C::rainDeactivateChance @0x040C
    float    rainWindSpeed;       // AdaynightCycle_C::rainWindSpeed @0x041C
    // v24 (2026-06-02): late-joiner WEATHER-LEVEL SNAP. Carries the host's CURRENT
    // interpolated levels so a joining client SNAPS to them instead of letting its
    // local sim ramp up over minutes (the fog "warm-up" the user reported). These
    // ride the existing connect-broadcast + continuous-broadcast payload but are NOT
    // in the dedup signature (they change continuously; SignaturePayload hashes only
    // flags + the 4 rain scalars). See research/findings/votv-weather-RE-* + the
    // 2026-06-02 fogprobe runtime confirmation.
    float    rain;            // AdaynightCycle_C::rain @0x02E0 -- the rainStrength EASE
                              //   TARGET. Anchored on apply so ReceiveTick doesn't drag
                              //   the synced rainStrength back to the client's local target.
    float    finalFogDensity; // AdaynightCycle_C::finalFogDensity @0x0418 -- the visible
                              //   height-fog density (pushed via SetFogDensity). Snapped +
                              //   SetFogDensity so the joiner's fog is instant, not eased up.
    float    fogAlpha;        // AweatherFogController_C::Alpha @0x023C -- the rolling-fog
                              //   actor's ramp intensity. THE DRIVER (thickFog = Alpha*Strength,
                              //   fogprobe-confirmed). Copied onto the client mirror actor so it
                              //   renders at the host's fog level + keeps ramping in lockstep (the
                              //   actor ACCEPTS the write -- a plain accumulator, not Timeline-locked,
                              //   snaptest-proven). 0 when the host has no rolling-fog actor.
    float    fogStrength;     // AweatherFogController_C::Strength @0x024C -- the per-spawn
                              //   density scale. Snapped WITH Alpha (Strength is randomized per
                              //   fog event, so Alpha alone wouldn't reproduce the host's thickFog).
};
static_assert(sizeof(WeatherStatePayload) == 40, "WeatherStatePayload must be 40 bytes (v24: +rain +finalFogDensity +fogAlpha +fogStrength)");
static_assert(sizeof(WeatherStatePayload) <= 256 - 20 - 8,
              "WeatherStatePayload must fit in one reliable datagram");

// TimeSyncPayload -- the host-authoritative WORLD CLOCK (TimeSync=29, v36). The cycle's
// totalTime/Day/TimeScale (AdaynightCycle_C) are not otherwise replicated, so a fresh joiner
// free-runs its own clock -> a dark client world. The host broadcasts this periodically + on
// connect; the client direct-writes the three floats (its own ReceiveTick re-derives the sun).
// Writing timeScale lets the client free-run smoothly between pushes. RE: research/findings/
// votv-coop-class-clone-migration-roadmap-2026-06-06.md §2.
struct TimeSyncPayload {
    float totalTime;   // absolute elapsed game time (the authoritative continuous clock)
    float day;         // the day number
    float timeScale;   // clock advance rate (so the client advances at the host's rate)
};
static_assert(sizeof(TimeSyncPayload) == 12, "TimeSyncPayload must be 12 bytes");

namespace weather_flags {
inline constexpr uint8_t kIsRaining       = 0x01;
inline constexpr uint8_t kIsSnow          = 0x02;
inline constexpr uint8_t kEnableRain      = 0x04;
inline constexpr uint8_t kEnableFog       = 0x08;
inline constexpr uint8_t kEnableSuperfog  = 0x10;
inline constexpr uint8_t kEnableSunlight  = 0x20;
inline constexpr uint8_t kEnableMoonlight = 0x40;
inline constexpr uint8_t kPermanentRain   = 0x80;
}  // namespace weather_flags

// v23 fog active-state bits (WeatherStatePayload::flags2). Distinct from the
// kEnableFog/kEnableSuperfog CONFIG bits in `flags` above: those are persistent
// "rolls allowed" gates, NOT active fog. Fog is rendered by event ACTORS, so the
// host stamps the ACTORS' live presence here and the client asserts it (destroy a
// stray fog actor on host-clear; mirror-spawn on host-fog). See
// research/findings/votv-weather-RE-* + coop/weather_fog.{h,cpp}.
namespace fog_flags2 {
inline constexpr uint8_t kFogActive      = 0x01;  // host has a live rolling-fog actor (AweatherFogController_C @ cycle->fogEventObject)
inline constexpr uint8_t kSuperFogActive = 0x02;  // host has a live AsuperFog_C
inline constexpr uint8_t kPermanentFog   = 0x04;  // host's permanentFog gamerule (re-arms the scheduler)
}  // namespace fog_flags2

// Phase 5W Inc-fix-2 (2026-05-27): red sky one-shot/toggle event. Lives on
// AmainGamemode_C; receiver invokes the same UFunction chain (spawnRedSky
// on first ON if redSky is null; redSky.set(state) for subsequent
// toggles). Echo-suppression is via role gate (host POST observer is
// only registered on the host; client's same observer no-ops on the
// role check).
struct RedSkyPayload {
    // v13 (A4 2026-05-29): was uint8_t peerSessionId. Now sender's
    // local Player Element id; receiver validates PeerSlot()==0.
    uint32_t senderElementId;
    uint8_t  state;          // 0 = revert color curves, 1 = red
    uint8_t  _pad[3];        // (v16: senderContext byte removed; coalesced into pad)
};
static_assert(sizeof(RedSkyPayload) == 8, "RedSkyPayload must be 8 bytes (v16)");
static_assert(sizeof(RedSkyPayload) <= 256 - 20 - 8,
              "RedSkyPayload must fit in one reliable datagram");

// Phase 5W Inc2: lightning strike discrete event. Carries only the strike's
// world location -- AdaynightCycle_C::timerLightning ubergraph spawns
// AlightningStrike_C with the SpawnTransform at the strike point (per the
// effect-actors RE doc, lightningStrike.hpp shows the strike's location IS
// the actor's own world transform; no separate field). Receiver spawns the
// same class at the same point via the existing BeginDeferredActorSpawnFromClass
// + FinishSpawningActor pair pattern (see remote_prop.cpp for the reference).
// The strike's Timeline self-destructs after ~3s -- no cleanup wire needed.
struct LightningStrikePayload {
    // v13 (A4 2026-05-29): was uint8_t peerSessionId + 3B pad. Now
    // sender's local Player Element id; receiver validates PeerSlot()==0.
    // v16: shrunk back 20->16 -- senderContext byte and the alignment
    // pad that v14 added with it both go (no pre-v14 pad existed).
    uint32_t senderElementId;
    float    locX, locY, locZ; // world cm
};
static_assert(sizeof(LightningStrikePayload) == 16, "LightningStrikePayload must be 16 bytes (v16)");
static_assert(sizeof(LightningStrikePayload) <= 256 - 20 - 8,
              "LightningStrikePayload must fit in one reliable datagram");

// v34 (2026-06-06): client loading-screen brackets. SnapshotBegin carries the prop candidate
// count the host is about to drain (the progress denominator); SnapshotComplete carries the
// count it actually sent (for the client's diagnostic reconcile -- the bar hides on receipt
// regardless). See the SnapshotBegin / SnapshotComplete ReliableKind docs above.
struct SnapshotBeginPayload {
    uint32_t propTotal;   // enumerated keyed-prop candidates the drain will stream to this slot
};
static_assert(sizeof(SnapshotBeginPayload) == 4, "SnapshotBeginPayload must be exactly 4 bytes");

struct SnapshotEndPayload {
    uint32_t propSent;    // PropSpawn messages actually sent this drain (<= propTotal after skips)
};
static_assert(sizeof(SnapshotEndPayload) == 4, "SnapshotEndPayload must be exactly 4 bytes");

#pragma pack(pop)

// Largest datagram we ever send/receive. Recv buffers size to this.
inline constexpr int kMaxPacketBytes = 256;

// Max reliable payload that fits one datagram: 256 - 20 (PacketHeader) - 8 (ReliableHeader).
inline constexpr int kMaxReliablePayload = kMaxPacketBytes - 20 - 8;

// Coordinate / speed sanity bounds (cm). A pose outside these is garbage or a
// hostile teleport-spam and is REJECTED at the trust boundary so non-finite or
// absurd values never reach the engine transform (SetActorLocation). VOTV's map
// is a few km; +/-1e6 cm (10 km) is generous headroom.
inline constexpr float kMaxCoord = 1.0e6f;
inline constexpr float kMaxSpeed = 1.0e5f;  // cm/s (well above any real walk/sprint)

// Fill a header in-place. `senderEpoch` is the sender's m_ownEpoch (Session
// mints non-zero at Start()); the receiver latches on first sighting + rejects
// mismatches per peer slot (v16 stale-generation defense, see PacketHeader doc).
// `senderSlot` (v18) is the logical origin slot -- direct sends pass their own
// slot (host=0; clients pass 0, don't-care since the host re-derives from the
// connection); the host's relay path passes the true origin slot.
inline void WriteHeader(PacketHeader& h, MsgType type, uint32_t seq,
                        uint32_t senderEpoch, uint8_t senderSlot = 0) {
    h.magic = kMagic;
    h.version = kProtocolVersion;
    h.type = static_cast<uint8_t>(type);
    h._pad = 0;
    h.seq = seq;
    h.senderEpoch = senderEpoch;
    h.senderSlot = senderSlot;
    h._reserved2[0] = h._reserved2[1] = h._reserved2[2] = 0;
}

// Validate a received buffer as one of ours: enough bytes + magic + version.
// Returns the parsed header fields and true if the header is well-formed.
inline bool ParseHeader(const void* data, int len, MsgType& outType, uint32_t& outSeq,
                        uint32_t& outSenderEpoch, uint8_t& outSenderSlot) {
    if (len < static_cast<int>(sizeof(PacketHeader))) return false;
    PacketHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic || h.version != kProtocolVersion) return false;
    outType = static_cast<MsgType>(h.type);
    outSeq = h.seq;
    outSenderEpoch = h.senderEpoch;
    outSenderSlot = h.senderSlot;
    return true;
}

// Peek the protocol version field WITHOUT requiring kProtocolVersion to
// match. Returns the version (1..65535) if magic matches and the buffer
// is large enough, 0 otherwise. Lets the receiver distinguish "a peer
// talking an older/newer protocol" (recognize + close with a reason
// string) from "random garbage / spoofed packet" (silent drop).
inline uint16_t PeekProtocolVersion(const void* data, int len) {
    if (len < static_cast<int>(sizeof(PacketHeader))) return 0;
    PacketHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic) return 0;
    return h.version;
}

// Reject a pose that is non-finite (NaN/Inf) or outside sane world bounds, BEFORE
// it can reach the engine. true == safe to apply.
inline bool ValidatePose(const PoseSnapshot& p) {
    const float vals[7] = {p.x, p.y, p.z, p.yaw, p.pitch, p.headYawDelta, p.speed};
    for (float v : vals)
        if (!std::isfinite(v)) return false;
    if (std::fabs(p.x) > kMaxCoord || std::fabs(p.y) > kMaxCoord || std::fabs(p.z) > kMaxCoord)
        return false;
    if (p.speed < 0.f || p.speed > kMaxSpeed) return false;
    // Yaw/pitch invariant: canonical FRotator axis range (-180, 180]. Senders MUST
    // normalize via ue_wrap::NormalizeAxis at the wire boundary (harness.cpp::
    // ReadLocalPose) -- this is the wire contract, not a sanitizer. An earlier
    // version of this check used (-90, 90) on the assumption that UE4's
    // GetControlRotation returns a normalized small-magnitude pitch; that was
    // WRONG (UE4's control rotation is unnormalized -- looking 10 deg down reads
    // back as Pitch=350) and silently dropped EVERY packet while a peer looked
    // below horizontal. Two converging agents 2026-05-23. The pitch invariant is
    // widened to (-180, 180] to match what a normalized FRotator axis legitimately
    // carries; out-of-range still rejects (still no crutch).
    if (p.yaw          < -180.f || p.yaw          > 180.f) return false;
    if (p.pitch        < -180.f || p.pitch        > 180.f) return false;
    if (p.headYawDelta < -180.f || p.headYawDelta > 180.f) return false;
    return true;
}

}  // namespace coop::net
