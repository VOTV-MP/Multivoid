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
// (research/findings/physics-grab/votv-throw-release-pipeline-RE-2026-05-24.md) showed the
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
//     see research/findings/physics-grab/votv-physics-interaction-deep-re-2026-05-23.md)
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
// pushes. See research/findings/weather-wind/votv-weather-DESIGN-2026-05-26.md.
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
// See research/findings/player-puppet/votv-player-vitals-death-RE-2026-05-30.md.
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
// research/findings/computers-devices/votv-doors-and-lightswitches-RE-2026-05-25.md. ParseHeader
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
// research/findings/kerfur/votv-kerfur-headlook-AnimBP-RE-and-coop-sync-2026-06-07.md.
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
// research/findings/kerfur/votv-kerfur-bodyfacing-RE-2026-06-07.md (pending).
// v41 (2026-06-08): BASE-WINDOW DIRT sync (Request 1, Part B -- the base's "main huge window").
// Adds ReliableKind::WindowCleanState (=30) + the 40-byte WindowCleanPayload. AbaseWindow_C's
// whole-surface dirt is one scalar `clean`@0x0260 (a SetCustomPrimitiveDataFloat shader slot;
// higher = dirtier, wiped DOWN to 0, never re-raised). SYMMETRIC cooperative-clean: each peer
// POLLS every AbaseWindow_C's `clean` each tick (the cleanSponge verb is BP-internal -> bypasses
// our ProcessEvent detour, same as doors/keypad, so a POST observer can't see it) and broadcasts
// on a DECREASE, keyed by the window's Aactor_save_C::Key. The receiver applies MIN(local, wire)
// -- a wire update can only make a window CLEANER, so two peers wiping concurrently converge to
// the lowest value with NO oscillation/regression (clean is monotone + inert -> no HostAuth /
// hold-register needed, unlike doors). The host RELAYS a client's wipe to the other clients and,
// on a new client's connect edge, snapshots each window's current clean with adopt=1 (apply
// VERBATIM so the joiner adopts the host's world even if its own save was cleaner). Own module
// coop/window_sync + ue_wrap/base_window. RE: research/findings/votv-dirt-window-cleaning-RE-and-
// coop-sync-design-2026-06-07a.md. (Part A -- Agrime_C surface grime on walls/ceiling/floor -- is
// a later bump: it is UNKEYED -> host Element/eid, not this keyed scalar. Part C -- Ad_window_C
// signal panel render-target stroke replay -- is deferred, not requested.)
// v42 (2026-06-08): SURFACE GRIME dirt sync (Request 1, Part A -- walls/ceiling/floor). Adds
// ReliableKind::GrimeState (=31), riding the generalized KeyedScalarPayload (the v41
// WindowCleanPayload, renamed -- one keyed-scalar payload now serves window clean AND grime
// process, like KeyedTogglePayload serves 3 bool kinds; RULE 2). Agrime_C is a STATIC level-placed
// decal (saved transform, never moves) -> both peers load the same save and so place each decal at
// an IDENTICAL world position; that position IS the cross-peer identity, so grime keys by a
// QUANTIZED WORLD-POSITION string (no host-allocated eid / Element / spawn-interceptor needed --
// the decal's own position is its key; smoke-proven: host + client posHash MATCH at 936 decals).
// SYMMETRIC cooperative-clean like the window: poll each grime's `process`@0x0250, broadcast on a
// DECREASE keyed by the position string; the receiver applies MIN(local, wire) (`process` is
// monotone-decreasing + inert -- a sponge/rain only lowers it -> converges, no oscillation) +
// repaints via applyMaterial(), driving the mirror decal toward process/maxProcess -> 0 (clean).
// The host RELAYS a client's wipe + connect-snapshots each grime's process (adopt=1). The process
// stream is STREAM-safe (a streamed-out decal doesn't change process -> broadcasts nothing).
// DEFERRED: (slot 32) the FINAL decal removal (a wipe takes process<0 -> native K2_DestroyActor) --
// grime streams in/out of sublevels, so a vanished decal is NOT a reliable destroy signal (an
// IsLiveByIndex death-watch flooded false destroys on the connect-teleport stream-out, smoke-
// caught); the true removal needs a K2_DestroyActor PRE edge reading process<0. Also deferred:
// runtime grimeProjectile splatter (non-deterministic spawn position -> not position-identifiable).
// Own module coop/grime_sync (+ ue_wrap/grime). RE: research/findings/votv-dirt-window-cleaning-RE-
// and-coop-sync-design-2026-06-07a.md PART A.
// v44 (2026-06-08): TWO new keyed/host-auth syncs -- GarageDoorState=33 (base GARAGE door,
// SYMMETRIC, reuses KeyedTogglePayload) + SkyState=34 (host-auth night-sky orientation + moon
// phase, new 16-byte SkyStatePayload). Slot 32 stays RESERVED for the deferred GrimeDestroy.
// (The CLUMP-dupe fix needs NO protocol change -- it deletes the unsound position-consume +
// adds an actorChipPile_C::playerGrabbed PRE observer routing through the existing PropDestroy.)
// RE: votv-garage-door-button-sync-RE + votv-sky-stars-celestial-sync-RE-2026-06-08.md.
// v45 (2026-06-08): physical-interactable sweep batch 1 -- ApplianceState=35 (6 on/off
// appliances: faucet/sink/shower/kitchen-oven/serverBox/wallunit-tapes; SYMMETRIC; reuses
// KeyedTogglePayload). The generic keyed-interactable engine (Adapter + Channel) moved out of
// interactable_sync.cpp into coop/interactable_channel.h (RULE 2026-05-25 file-size extraction
// so the new adapter had room); no wire change beyond the new kind. RE: votv-all-interactables-
// sweep-catalog-2026-06-08.md.
// v46 (2026-06-08): physical-interactable sweep batch 2 -- PowerControlState=36 (the base
// power panel's 5 breakers, ApowerControl_C; SYMMETRIC; new 5-bit PowerPanelPayload, its own
// coop/power_sync module since 5 bools/actor doesn't fit the generic 1-bool Channel). RE:
// votv-powerControl-panel-sync-RE-2026-06-08.md.
// v47 (2026-06-08): ATV/quadbike Phase 1 -- AtvState=37 (occupant-authoritative keyed body pose
// stream, AATV_C; new 60-byte AtvStatePayload; coop/atv_sync + ue_wrap/atv). RE: votv-ATV-
// quadbike-RE + votv-ATV-Phase1-pose-stream-blueprint-2026-06-08.md.
// v48 (2026-06-08): delivery DRONE Phase 1 -- DroneState=38 (HOST-authoritative singleton body
// pose mirror, Adrone_C; new 28-byte DroneStatePayload; coop/drone_sync + ue_wrap/drone). Cargo
// rides the existing prop pipeline. RE: votv-delivery-drone-RE-and-coop-sync-design-2026-06-03.md.
// v49 (2026-06-09): delivery-drone ECONOMY -- OrderRequest=39 (reliable CLIENT->HOST shop-order
// forward; the client polls its local saveSlot.orders, chunks each new order to the host, who
// re-commits it via the native Uui_laptop_C::makeAnOrder so the shared drone delivers it). Cargo +
// drone body ride the existing prop + DroneState pipelines. Variable-length (OrderRequestHeader +
// packed items); chunked to fit kMaxReliablePayload. coop/order_sync + ue_wrap/order_economy +
// ue_wrap/ftext_utils. RE: votv-delivery-drone-RE-and-coop-sync-design-2026-06-03.md (ECONOMY BUILD PLAN).
// v50 (2026-06-09): WIND GUST sync -- WeatherStatePayload +windTarget Vec3 (56->68B). The
// leaf-shake is the spring `intensity`, a low-pass of AdirectionalWind_C's windTarget, re-rolled
// per-peer by a changeWindOrigin RNG timer -> host gusty, client calm. Host streams windTarget;
// client writes it + suppresses its changeWindOrigin. RE: votv-wind-event-driver-RE-2026-06-09.md.
// v51 (2026-06-09): PEER-SYMMETRIC ambient FIREFLY mirror -- ReliableKind::FireflySpawn=40 +
// FireflySpawnPayload. Every peer PRE+POST-observes its OWN firefly spawner's ReceiveTick, diffs the
// ParticleSystemComponent set to capture the spawn the EX_CallMath SpawnEmitterAtLocation hid from our
// hook, broadcasts the position (relayed host->other clients); every peer spawns others' eff_fireflies
// -> all peers see the union near every peer. RE: votv-firefly-host-mirror-RE-2026-06-09.md.
// v52 (2026-06-09): trash-clump ball->pile ATOMIC convert + identity grab-destroy -- ReliableKind::
// PropConvert=41 + PropConvertPayload. The clump's morph (ball lands -> spawns a chipPile + self-
// destructs) is BP-internal/unobservable, and the old flow sent a separate landed-pile PropSpawn +
// PropDestroy with two DIFFERENT eids found by a cross-peer-unsound FindNearestChipPile -> the ball
// mirror lingered / the wrong pile was consumed -> the infinite re-grab DUPE. Now the owner's clump
// death-watch emits ONE PropConvert{oldEid=ball, newEid=pile, transform, chipType}; the receiver
// atomically destroys the ball by oldEid + spawns the pile by newEid (a settled landed pile). A mirror-pile
// death-watch (proximity+transition gated, the grime super-sponge precedent) propagates a re-grabbed
// pile's destroy BY IDENTITY (PropDestroy(eid)) -- the robust replacement for the retired InpActEvt_use
// lookAtActor grab-guess. RE: votv-clump-lifecycle-observability-and-robust-design-2026-06-08-pass2.md.
// v54 (2026-06-10): PROP IDENTITY PARITY (white-cube root cause) -- PropSpawnPayload 168->200B:
// +propName (the Aprop_C list_props row FName `Name`@0x0258 that init() resolves the mesh/mass/
// collision from; CDO default row is 'cube' -> Name-less mirrors of the host's save-loaded generic
// prop_C actors, e.g. broken-cubicle cubicleP_* wall panels, rendered as white cubes), senders now
// stream the REAL GetActorScale3D (was hardcoded 1,1,1), physFlags += kStatic/kSleep/kRemoveWOrespawn
// (kAtRest constant deleted, bit reused). Receiver raw-writes Name+flags on the deferred mirror BEFORE
// FinishSpawningActor so init()'s single UCS pass constructs the true prop -- the same field set SP's
// own loadObjects->loadData->init() restores. RE: agent ac912b54285a9e263 session 2026-06-10.
// v55 (2026-06-10): SNAPSHOT ADOPTION root-cause set (same deploy as v54; behavior contracts change
// so the version gates both). (1) Fork A bracket coherence: the host opens a snapshot bracket ONLY
// when its prop registry expresses its CURRENT world (seed-world stamp gate + deferred slots +
// mid-drain abort-without-Complete) -- a mid-transition near-empty bracket destroyed 1300 client
// actors against 0 claims on 2026-06-10. (2) Fork B: keyless actorChipPile_C piles are EXPRESSED in
// the snapshot by eid (chipType stamped; host death-watches each) -- pre-v55 the keyed-only
// enumerate dropped every pile while the class-based sweep destroyed the client's own -> client had
// ZERO piles; the adoption sweep universe widened from the 4 RNG litter classes to the full
// expressible universe (keyed IsClassKeyedInteractable + keyless chipPile lineage; claims protect
// everything wire-expressed in either direction, incl. NEW self-claims at the announce sites + a
// local-held guard); the host->client PropSpawn eid gate accepts EITHER range (a bracket
// re-expresses existing peer-born entities). (3) Fork C: client-side ambient flora/forage spawner
// suppression (mushroomMaster/mushroomSpawner/pineconeSpawner) + the falsified dirthole row deleted.
// RE + verdicts: votv-snapshot-adoption-root-causes-2026-06-10.md.
// v56 (2026-06-10): SAVE-TRANSFER JOIN BOOTSTRAP (user mandate: "pull all objects data at connecting
// time and place/spawn objects naturally" -- no more divergent fresh-save client worlds with office
// walls falling from the air). A menu-mode joining client connects AT THE MENU, requests the host's
// save (SaveTransferRequest), receives it chunked on Lane::Bulk (SaveTransferBegin + SaveTransferChunk,
// ~56KB GNS messages diverted around the 228B inbox), writes it as the EPHEMERAL slot s_coop_dl
// (deleted right after load + at boot/disconnect -- the no-steal lifecycle), loads it via the game's
// own LoadStorySave (every prop key now MATCHES the host -> the connect snapshot becomes a thin
// all-exact-key true-up), THEN signals ClientWorldReady -- which is now the ONLY trigger for the
// host's connect replay (bracket + state broadcasts), for BOTH join flows. Pre-world reliable
// gameplay events are DISCARDED client-side (the world-ready bracket re-derives them by design).
// Host save attempt-on-join respects the game's own save gate (event-blocked saves fall back to the
// on-disk save; staleness is absorbed by the true-up). Design: votv-snapshot-adoption-root-causes-
// 2026-06-10.md SAVE-TRANSFER ARC + the connect-world-bootstrap-design workflow verdict.
//
// v57 (2026-06-10): TrashPileState -- the trashBitsPile_C collect-counter mirror (amountA/B int
// pair, poll + proximity death-watch + adopt connect-snapshot; coop/trash_pile_sync). RE:
// findings HANDS-ON ROUND 2 (4); bytecode-grounded by the trashBitsPile RE agent pass.
// v58 (2026-06-11): InventoryPickup -- the iconic inventory-collect blip (inventory_Cue), audible
// at the collecting peer's position on every OTHER peer. Natively PlaySound2D = 2D collector-only;
// the collector's machine POST-observes UGameplayStatics::PlaySound2D (EX_FinalFunction on the CDO
// = ProcessEvent-visible; gated Sound==inventory_Cue + pitch~1.1 + WorldContextObject==local
// player -- the game-wide cue census has zero other matches) and broadcasts its position; peers
// play the cue spatialized there. coop/inventory_pickup_sync; RE:
// votv-inventory-pickup-seam-RE-2026-06-11.md.
// v59 2026-06-11: KeypadSyncPayload + `event` byte (KeypadEvent Accept/Deny) -- the short-code
// submit mirror; receivers run the keypad's native Open(Active) chain instead of the deleted
// host-side buffer==password accept poll (which fired without the accept press and latched the
// LED green forever -- the 2026-06-11 keypad bug).
// v60 2026-06-11: + ChatMessage=48 (the T-chat line; peer-symmetric, host-relayed).
// v61 2026-06-11: + TurbineState=49 (wind-turbine facing/spin mirror, host-auth ~1 Hz).
// v62 2026-06-11: + LockerDoorState=50 (locker + drone-console hinged doors, symmetric toggle).
// v63 2026-06-12: + DeviceClaim=51 -- enterable-device OCCUPANCY (base computers/terminals
// phase 1, user design: ONE peer "inside" an enterable screen device at a time; a second
// peer's E gets DENIED with the game's own button_keypad_deny sound). Host-arbitrated
// first-wins claim table keyed by the SHARED-WIDGET identity (desk-coords / sat / radar /
// reactor / laptop are one claim each -- their screens are literally one shared widget
// instance; transformer panels + arcade props are per-instance via quantized position).
// Claim = activeInterface rising edge (poll; every enter call is EX_LocalVirtualFunction,
// PE-invisible); deny = InpActEvt_use PRE aim-clear (the door HostAuth precedent); loser
// force-exit = reflected setActiveInterface(null) (the game's own ragdollMode exit path).
// Screen-STATE mirror (desk blob / dish aim / sky-signal host-roller / email deltas) is
// phase 2. RE: votv-base-computers-RE-2026-06-11.md.
// v64 2026-06-12: base-computers PHASE 2 increment 1 (screen-state mirror, RE doc SS4-5):
// SkySignalState=52 (host-authoritative sky-signal SET snapshot, parts of 3 wire rows --
// the coords-minigame targets were pure per-peer RNG; the client's spawnSignal roller
// timer is killed and its widget lifetimes become wire-driven, so the client set is a
// pure mirror) + SkySignalCatch=53 (the desk-claim holder's catch relayed to the host
// roller) + DeskState=54 (the desk's live-visible scalars + the coordLog tail:
// claim-owner cadence + any-peer button edges + adopt snapshot) + DishAimState=55
// (spaceRenderer.coords dish-aim stream while the desk is claimed). Email/saved-signal
// deltas reserved as the NEXT increment. RE: votv-computers-phase2-impl-RE-2026-06-12.md.
// v65 2026-06-12: base-computers PHASE 2 increment 3 (the remaining computers block).
// + EmailDelete=57 -- the player's del-button (ui_laptop.delEmail) mirrors cross-peer.
// CONTENT-KEYED (FNV-1a 64 of the row's serialized EmailAppend blob), NOT index-keyed:
// a producer appends natively BEFORE its row relays, so concurrent appends give peers
// different array ORDERS and a wire index can name a different row per peer. Each peer
// shadows saveSlot.emails (per-row POD instance key read raw off the array bytes --
// zero reflected calls in the steady-state poll -- plus the blob hash captured at
// first sight: prime / local append / wire apply); a poll diffs positionally under the
// append-at-tail invariant and broadcasts removed rows' hashes; receivers resolve
// their local index by hash and run the native ui_laptop.delEmail. A short-TTL
// tombstone set closes the delete-beats-chunked-append race.
// + SavedSignalAppend=58 / SavedSignalDelete=59 -- the same shadow/diff/content-hash
// shape on gamemode.savedSignals_0 (the desk signal library); apply = reflected
// gamemode.saveSignal / deleteSignal (native pane refresh); image PNG deferred to a
// bulk lane. + CompState=60 / CompData=61 -- the refiner decode pane: single-simulator
// doctrine (the native latch holder streams; mirrors render passively -- the decode
// ticker has no occupancy gate and completion fires world triggers incl. the theEvil_C
// spawn, so a latched mirror would double-produce). Client world-up UNLATCH kills the
// save-transfer's setData->comp_start auto-resume on joiners (pre-existing
// double-simulation bug since v56). EmailChunkPayload generalized to BlobChunkPayload
// (coop/blob_chunks owns chunked send/reassembly for kinds 56/58/61, RULE 2).
// + U6 day-cycle suppression (NO new kind): a connected CLIENT writes
// daynightCycle.TimeScale=0 with every clock correction (the midnight task/email/
// points cascade + savedtime.Z roll become structurally unreachable client-side --
// host's rolls mirror via the email/order channels) and latches
// saveSlot.dailyDelivery=true (kills the 6am duplicate auto-order INCL. the join-edge
// instant fire); disconnect restores TimeScale=1 (the game's own restore value).
// RE: the 2026-06-12 comp/savedSignals/daynight agent passes.
// v66 2026-06-12: PROXIMITY VOICE CHAT (the Simple-Voice-Chat port, svc-voice-chat-RE +
// votv-voice-chat-port-design 2026-06-12; MTA transport precedent CVoiceDataPacket --
// voice multiplexes over the main session, NO second socket, so SVC's entire UDP
// security layer (magic/AES secret/auth/keepalive/ping) is dropped, not ported).
// + MsgType::VoiceFrame=64 -- 20 ms / 48 kHz mono opus frames (VOIP + FEC 5%), per-sender
// voice seq, empty stop-marker ends a burst (encoder/decoder reset both ends); receiver
// jitter buffer (threshold 3) + <=5-frame PLC + 100 ms prebuffer; linear distance
// attenuation 1-d/maxDist (4800 cm, whisper 2400) + SVC REDUCED-mode stereo pan, mixed
// in a miniaudio playback callback from per-slot PCM rings; capture = miniaudio device
// -> gain+limiter -> PTT 'X' / threshold activation (-50 dB) -> opus encode (SPSC to the
// game thread). Host relays VoiceFrame to all ready slots (no distance cull at <=4
// peers; receiver attenuation zeroes out-of-range audio). + ReliableKind::VoiceState=62
// {micMuted, voiceDisabled} display-only edges. Vendored: libopus v1.5.2 (BSD) +
// miniaudio 0.11.22 (public domain), third_party/. Modules: coop/voice/*.
// v67 2026-06-12: KERFUR ON/OFF CONVERSION host-authoritative (the user dupe: "client
// turns kerfur-prop on, then off -> host sees 2 kerfurs"). Ground truth (kismet,
// votv-kerfur-convert-RE-2026-06-12): both conversion verbs are BP-INTERNAL
// (EX_CallMath BeginDeferred + by-name K2_DestroyActor) -- INVISIBLE to the
// ProcessEvent detour, so the client's local conversion was never suppressed/tracked
// and the host's own conversion never registered its spawn (zero npc-suppress[client]
// / npc-sync[host] broadcast lines across all real-session logs = the BeginDeferred
// interceptor has never fired for game-BP spawns; mid-session NPC sync rode the
// connect-edge walk alone). Fix: PRE-cancel the PE-VISIBLE menu dispatchers on the
// client (kerfurOmega_C::actionName 'turn_off' / prop_kerfurOmega_C::actionOptionIndex
// action==8) -> ReliableKind::KerfurConvertRequest=63 {elementId, toProp}; the HOST
// executes the real verb (dropKerfurProp / spawnKerfuro via ProcessEvent, the BP's
// kill-guard replicated) and CONVERGES the BP-internal side effects explicitly:
// !IsLive actor -> the factored destroy-sync (EntityDestroy / PropDestroy); new prop
// (+floppy) -> ExpressSpawnedProp walk-ingest; new NPC -> RegisterExistingWorldNpcs,
// which now also broadcasts EntitySpawn for newly-registered NPCs while connected.
// Host menu conversions ride the same converge via a pending queue (interceptor may
// not Post). Module: coop/kerfur_convert.
// v68 2026-06-12: WALL-ATTACHABLE STICK mirror (user: client sticks the camera to a
// wall, host sees it fall). RE (votv-camera-stick-RE agent pass, 2026-06-12): the
// stick is comp_wallAttachable_C's held-proximity poll -- commit (ubergraph entry 45,
// reached ONLY via a latent Delay resume = PE-VISIBLE) sets prop.frozen (or .Static
// when pryingRequired) + init() (physics off) + K2_AttachToComponent KeepWorld +
// eff_OC_freeze VFX + a 0.25 s glide to the surface; unstick = re-grab (frozen=false
// + init()). Every spawn/field-write inside is BP-internal -- our prop pipeline never
// saw it, and the receiver-side release/timeout paths unconditionally re-enabled
// physics (the fall). Fix: POST observer on ExecuteUbergraph_comp_wallAttachable
// (EntryPoint==45) -> next-pump-pass PropStickState{key,eid,flags,commit pose}
// broadcast (ReliableKind 64, relayed like other symmetric prop state; NO settle
// delay -- the stick must beat the sender's hold-break PropRelease on their shared
// lane, and the receiver re-derives the settled pose itself); receiver
// stops any drive, re-poses, re-enables simulate and PE-dispatches the BP's OWN
// forceStick(true) (full SP parity: field+attach+VFX; raw frozen-write+init+re-pose
// fallback when the receiver-side trace diverges -- SP's own save-load degraded
// mode). Unstick mirrors through the existing pose stream: a stuck WALL-ATTACHABLE
// mirror unsticks when a SUSTAINED PropPose stream arrives (>=5 fresh poses --
// 1-2 stale in-flight packets sent between the sender's commit and its hold-break
// must not unstick it), and the release/timeout physics re-enable is gated on
// !frozen/!static. Module: coop/prop_stick_sync.
// v69 2026-06-12: DRONE ROTOR-DUST anchor (user: dust shows on host, never on
// clients). Bytecode RE (research/pak_re/drone_dust_notes.md): eff_droneDust is
// bAbsoluteLocation with NO relative offset -- it sits at WORLD ORIGIN until
// drone_C's per-tick chain K2_SetWorldLocation's it to the ground-trace hit
// (and the PS is EmitterLoops=1/20s -- it completes and needs the per-tick
// IsActive!=want re-arm). The client mirror suppresses that tick -> the old
// one-shot SetActive+param replay ran at origin, frustum-culled = invisible.
// Fix: DroneStatePayload grows the host's dust-anchor world location (28->40 B);
// the client replays the BP's own three calls per packet (SetActive on
// IsActive!=want, K2_SetWorldLocation(anchor), SetFloatParameter('dust',
// 1-dist/2000)). Divergence from the RE's preferred client-local re-trace:
// streaming the host's anchor avoids hand-building the SphereTraceSingleFor-
// Objects frame (two TArrays + FHitResult by-ref) -- 12 B on an existing
// 20 Hz host->client stream. Module: coop/drone_sync + ue_wrap/drone.
// v70 2026-06-12: STOLAS SIGNAL-CATCH CONSUME REPLAY (the round-3 "client caught a
// signal, host's downloader said NO SIGNAL / dishes never slewed / terminal full of
// blank lines" report; RE: votv-stolas-signal-catch-RE-2026-06-12.md). The SP consume
// chain after gatherSignal (coord_signalData write -> row delete -> ALL gamemode.dishs
// startMovingTo -> arrival -> dishesStop -> formDownload arms the downloader) ran
// occupant-local only. Now: SkySignalCatch=53 grows from a 20 B row-delete relay into
// the full 80 B consume replay {wire row from the catcher's coord_signalData; the exact
// startMovingTo slew vector read off a just-started dish; kind 0=catch/1=cleared}.
// Catcher detection (claim holder, 1 Hz): row vanished from `signals` AND a dish
// isMoving RISING edge in the same poll -- the dish edge is the success signature
// (expiry deletes a row with no dish movement; a FAILED ping writes the struct but
// moves no dish; this also catches a success on a re-pinged identity the struct
// already pointed at, which the RE doc's struct-changed AND-detector missed). Host
// validates the desk-claim holder, replays, rebroadcasts to everyone but the catcher.
// Receiver replay: coord_signalData := wire row (raw POD + StringToFName) ->
// RemoveSignalByIdentity -> reset the download machine (signalName None + mesh null +
// initDownloadSignal(0,0,-1) -- the @33832 delete-chain shape; mesh-null also makes
// the native @4602 pulse zero resDetect) -> playPingSound(pingSuccess) -> dish::
// StartMovingAll(slew) -- the NATIVE chain then self-runs per peer (random per-dish
// delays, activeDishes bookkeeping, dishesStop -> formDownload(0,-1)) = downloader,
// letter-board lamps, signalFound trigger all arm with zero extra wire. slewValid=0
// fallback (no moving dish found -- e.g. joiner replay after dishes settled): receivers
// arm formDownload directly. kind=1 (cleared, the 'Signal data deleted' button):
// objectName->None edge detected on ANY peer (unclaimed trust, matches the physical
// button), replay = the @33832-34222 chain minus the log line. Joiner: host sends one
// catch replay when coord_signalData is armed (identity from the desk struct).
// + DeskLogLine=65 REPLACES the DeskState coordLog tail diff (RULE 2: the 96-char
// sliding-window tail protocol was structurally lossy -- the 0.2 s coordLog2 timer
// appends ~5 animated lines/s, the 1 Hz window slid past more text than it carried,
// the receiver's no-overlap fallback re-appended fragments = the blank/garbled mirror
// terminal). Producer-side EXACT diff (one local monotone text), complete CRLF lines
// only, ANIMATED lines filtered by prefix (CDOWN:/AREA SCAN:/APPROXIMATION:/ANALYSIS:/
// CR:[ -- those self-generate on every peer from mirrored scalars); only the 9 one-shot
// event lines ride the wire (host-relayed); receiver appends via writeToCoordLog_2
// (native CRLF + repaint + 1000-cap) and advances its own diff baseline (echo-proof).
// + DeskStatePayload reshaped (152->60 B): coordLog[96]+len GONE (DeskLogLine), canDL
// GONE (DERIVED -- canSaveSignal recomputes it per detector pulse from resDetect +
// decoded>=size; a wire write would oscillate against the local recompute), + dlDecoded
// /dlPolarity ADOPT-ONLY fields (DL_SignalDownloadDLData progress for the joiner
// catch-up, applied via formDownload(decoded,polarity) once its mesh arms; live
// streaming REJECTED -- bytecode @66736-68635 shows decoded accrues per tick on EVERY
// armed peer (no occupancy gate), so an advance-triggered stream would make all peers
// 1 Hz senders forever, the exact v69 RAM-balloon shape; per-peer accrual converges
// natively once the catch replay arms everyone). + DishAimStatePayload carries
// ui_coordinates.Direction (the catch-gate panel toggle) in its pad (40 B unchanged).
// Modules: coop/signal_catch_sync (new) + ue_wrap/dish (new) + coop/console_state_sync.
// v71 2026-06-13: SLEEP GATE (Minecraft-style night skip; RE: votv-sleep-nightmare-RE-
// 2026-06-12.md). SP sleep = ONE engine call (SetGlobalTimeDilation(20) @70543) + ONE
// world flag (mainGamemode.isSleep @0x04EC), both PER-PROCESS -- a lone coop sleeper
// would run its whole local world at 20x while the shared clock (host-authored,
// time_sync) stands still, and awake peers' vitals would drain at 20x if it were
// shared. Gate: a peer entering a bed keeps the native cosmetic half (sleepingPawn +
// sleepCam) but sleep_sync's per-tick enforcement undoes the dilation to 1.0 while
// WAITING (polling the isSleep edge -- deliberately NOT the RE doc's sleep() detour:
// the poll covers every entry path (interaction, probes, a host already asleep when a
// client connects) with no new hook surface, at a <=1-tick undo lag). Each peer's
// isSleep edge reports inBed over ReliableKind::SleepState=66; the HOST tallies
// world-ready peers and broadcasts the "N/M sleeping" counter (chat feed) -- when ALL
// are in bed it broadcasts ACCELERATE (each sleeping peer sets its own dilation 20;
// vitals refill at the native dilated rate per peer; the client clock free-runs at
// TimeScale=1 for the phase -- time_sync::SetSleepAccelerate -- so the sky pans
// smoothly instead of stepping on 2 s corrections). END = any peer's isSleep falling
// edge (natural host wake / manual exit / hunger / event / nightmare -- all funnel
// through gamemode.wakeup): host broadcasts End{natural}; receivers still in bed run
// the native wakeup() reflected, and ONLY a NATURAL end (the host slept to >=100)
// grants every peer saveSlot.sleep=100 -- wakeup FIRST, need-write AFTER, because
// wakeup rolls the 10% gearer gift iff sleep>=99 at call time (write-first would gift
// every mirror every night). CLIENTS clamp their need at 99 during the phase (only
// the HOST ends the night naturally -- one authority, no first-to-fill race) and run
// dreamProbability=0 for the whole session (nightmares HOST-ONLY by design; the
// host's own roll is restored to the -1 sentinel only DURING the accelerate phase --
// a host nightmare wakes the house structurally: createDream wakeup()s before the
// dream, the falling edge IS the early End). Module: coop/sleep_sync + ue_wrap/sleep.
// v105 (2026-07-06): HAND-ITEM display axis (ReliableKind::HandItem = 89). The hotbar
// hand item leaves the WORLD-prop pipeline entirely (RULE 2): updateHold respawns the
// hand actor per quick-slot switch, so treating it as a world prop produced the
// never-expressed host hand (PRE-QUIESCENCE gate never lifts on the host -> the client
// dropped 60 Hz keyed poses, 'no local match' x1809, hands-on 2026-07-06 12:40) plus
// physics-release litter on switch-away. Now: owner announces HandItem{class,name} on
// change (reliable, ~1 msg per switch); peers keep a display-only mirror (physics off,
// collision off, echo-suppressed) attached to the puppet; host stores per-slot states
// + replays them in ConnectReplayForSlot. mainPlayer.holding_actor with an Aprop_C no
// longer feeds the PropSpawn/PropPose path (the trash clump/pile carry -- the
// non-Aprop_C holding_actor case -- stays on its lane untouched).
inline constexpr uint16_t kProtocolVersion = 116; // v116 (2026-07-17 eve, the catch-attribution retire):
                                                  // SkySignalCatch kind gains 2 = connect STATE-SEED
                                                  // (applied like 0, never announced to the activity
                                                  // feed) -- a SEMANTIC wire change, hence the bump.
                                                  // Root (measured live 17:04): the successful ping's
                                                  // own completion released the desk FSM-hold in the
                                                  // same second as the catch edge -> the 1 Hz
                                                  // claim-gated detector lost the race and the
                                                  // baseline roll-forward ate the catch PERMANENTLY
                                                  // (host NO SIGNAL, no dish theater, client
                                                  // self-slewed 24 dishes). Retired (RULE 2): the
                                                  // detector LocalHolds gate, the NoteIncomingSnapshot
                                                  // pre-gate, the host holder==sender validator.
                                                  // Added: host-side IsRecent dup guard, the settled-
                                                  // dish lookAt slew fallback, catch -> activity feed
                                                  // (peer_action_feed; kind=0 only).
                                                  // ---- history ----
                                                  // v115b (2026-07-17 pm, BEHAVIOR-only -- no wire
                                                  // format change, no bump): the coord_isPing RAW APPLY
                                                  // is retired at every receiver (DeskInput field 12 ->
                                                  // bookkeeping only; DeskState adopt never copies it).
                                                  // Measured root: the native ping FSM is a LATENT tick
                                                  // machine gated on coord_isPing (analogd uber @82980 ->
                                                  // @80105 stage engine, ==1.0 latches @79979) -- the raw
                                                  // apply woke a PHANTOM parallel sim on observers
                                                  // (divergent verdicts, phantom host ARM 14:47:38,
                                                  // double coordLog authorship). ONE machine per ping:
                                                  // the presser's. Companions: the desk FSM-hold claim
                                                  // (device_occupancy reconciler) + the arm-poll re-init
                                                  // window (DISARM suppressed while signalData lives).
                                                  // v115 (2026-07-17, desk audio-effect mirror):
                                                  // presser-authored one-shot/loop sound forward at the
                                                  // NATIVE audio seam -- Func-patch on AudioComponent:Play
                                                  // + ActorComponent:SetActive/Activate (every whitelist-
                                                  // comp call site measured EX_VirtualFunction on a native
                                                  // target -> funnels through UFunction->Func), pointer-
                                                  // whitelisted to the desk's 6 unit-1 audio comps; new
                                                  // relayed DeskSndFx=105 (44 B) carries {op,comp,cue};
                                                  // mirrors replay SetSound+Play / SetActive under the
                                                  // wire-apply echo guard. RULE-2: PlayScanEffects' beep +
                                                  // signal_catch's PlayPingSuccess replay RETIRED (the fx
                                                  // lane owns desk one-shot audio). WIRE CHANGE (new kind)
                                                  // -> bump; a mixed 114/115 pair HARD-CLOSEs at the gate.
                                                  // v114 (2026-07-17, L7 tape caddy + daily task):
                                                  // wallunit reel slots (presser-authored ReelSlot=102
                                                  // sentinel edges + host ReelPose=40 corrector; client
                                                  // accrual NOT parked -- written park-doctrine
                                                  // deviation), host-authored TaskNewState=103 mirror
                                                  // of saveSlot.taskNew, and the client-eject prop
                                                  // birth path: ReelEjectIntent=104 (client->host,
                                                  // HostSpawnPlacedProp author) + the savedScalar
                                                  // identity-at-birth channel (PropSpawnPayload _pad2
                                                  // -> savedScalar + flag 0x40; PropDropIntentPayload
                                                  // 168->172 B). WIRE CHANGE (new kinds + two payload
                                                  // layouts) -> bump; a mixed 113/114 pair HARD-CLOSEs.
                                                  // v113 (2026-07-16, L4 dishes): host-auth dish pose
                                                  // mirror (new unreliable DishPose=39; client sim
                                                  // PARKED -- tickers killed + own-ping slews killed-
                                                  // with-cleanup; StartMovingAll client half RETIRED),
                                                  // host-polarity download ARM edge (DishArm=99 +
                                                  // DishSnapshot=100 join seed), symmetric calibration
                                                  // lane (DishCalib=101), and the v70 pending-adopt
                                                  // RETIRED: DeskStatePayload drops the ADOPT-ONLY
                                                  // dlDecoded/dlPolarity fields (60->52 B). WIRE CHANGE
                                                  // (new kinds + DeskState layout) -> bump so a mixed
                                                  // 112/113 pair HARD-CLOSEs at the gate.
                                                  // v112 (2026-07-16, the BUGS-v111 fix): the desk INPUT
                                                  // re-partition -- claim-free field-granular DeskInput=97
                                                  // deltas + DeskScanEvent=98 replace the v64 claimed/
                                                  // unclaimed live DeskState lanes (DeskState=54 is now
                                                  // ADOPT-only); coord_cooldown LEAVES the DeskSimPose
                                                  // stream (8->7 channels, 32->28 B snapshot) -- it is
                                                  // charge-event-driven + natively dt-decayed per peer.
                                                  // WIRE CHANGE (kinds + snapshot layout + lane semantics)
                                                  // -> bump so a mixed 111/112 pair HARD-CLOSEs at the gate.
                                                  // v111: signal-desk download-SIM host-authoritative --
                                                  // new unreliable DeskSimPose=38 (host->all output vector)
                                                  // + the live DeskState apply no longer authors the sim
                                                  // OUTPUT fields (host owns them via DeskSim; one author,
                                                  // gate 1). WIRE CHANGE -> version bump so a mixed 110/111
                                                  // pair HARD-CLOSEs at the gate (a 110 peer would author
                                                  // the outputs via DeskState AND ignore DeskSimPose).
                                                  // v110: clock sync design F -- the world clock moves OFF
                                                  // the reliable periodic TimeSync onto the unreliable
                                                  // ClockPose=37 host->all snapshot (~2Hz, newest-wins).
                                                  // v109: desk cursor pose split -- the coords-panel
                                                  // LIVE cursor (viewCoordinate) moves OFF the reliable
                                                  // DishAimState onto the unreliable DeskCursorPose=36
                                                  // stream (60Hz, mirror-interpolated); DishAimStatePayload
                                                  // becomes committed-coords-only (40->32 B). Mixed 108/109
                                                  // peers HARD-CLOSE at the version gate (session.cpp:352-371).
                                                  // v108: roach infestation sync (RoachState=92,
                                                  // RoachConsumed=93) + the ambient owner-mirror flip
                                                  // (pinecone set peer-symmetric) + the OWNER-ENTITY
                                                  // lane (OwnerEntitySpawn/Pose/Destroy=94-96: eyer)
                                                  // v107: signal-server sim state sync (ServerState=91)
// v104: WorldActorPoseSnapshot +auxTargetEid
                                                  // (44->48; batch cap 31->28) -- the piramid
                                                  // wispTarget IDENTITY streams with the pose, so
                                                  // the mirror's head/searchlight run the native
                                                  // CHASE branch during the walk-to-wisp phase
                                                  // (the 0y residue: the mirror idled on relLook
                                                  // wander the host was ignoring). See the field
                                                  // doc at WorldActorPoseSnapshot.
                                                  // Prior:
                                                  // v103: per-player NICK COLOR (12f) -- a self-
                                                  // describing [u8 has][u8 r][u8 g][u8 b] field
                                                  // appended to Join + PlayerJoined (after the v94
                                                  // prefs flags byte) + NickColorChange=88 for live
                                                  // changes (the NameplateChange trust shape). The
                                                  // color axis has ONE owner (coop/player/nick_color);
                                                  // nameplate/chat/scoreboard read it and fall back
                                                  // to their own defaults when unset (packed 0).
                                                  // Prior:
                                                  // v102: WorldActorPoseSnapshot +auxVec (32->44) --
                                                  // the piramid HEAD/searchlight look target (relLook)
                                                  // streams with the pose; the mirror's 1 Hz RANDOM
                                                  // changeLook wander is suppressed and the native
                                                  // lookat VInterpTo eases toward the host's target
                                                  // (state mirrored, playout native; user live
                                                  // 2026-07-05: heads/фонарь diverged when idle).
                                                  // v101: AlarmState=87 -- the base radar alarm as a
                                                  // shared-world toggle lane (poll-based: runTrigger is
                                                  // EX_VirtualFunction-invisible; docs/events/alarm.md)
                                                  // + the late-join answer (mid-alarm joiner gets state).
                                                  // v100: WorldActorPoseSnapshot +auxYaw (28->32) --
                                                  // the piramid's visible heading streams as part of
                                                  // its pose (delta-derivation live-refuted: native
                                                  // heading turns for up to 10 s after motion stops).
                                                  // v99: EntitySpawnPayload +Scale3D (104->116) -- the
                                                  // game spawns piramid2_C at scale 2 via the deferred
                                                  // spawn transform; a scale-less mirror spawned at 1
                                                  // renders half-size + floats at the scale-2 hover Z
                                                  // (user-verified 2026-07-05: "далеко и маленькая").
                                                  // (v98: EventSnapshot=86 -- join-during-event Phase 1)
                                                  //      (host->joiner in-flight event registry snapshot
                                                  //      at the world-ready edge; replay-safe rows replay
                                                  //      with the active-override; docs/COOP_EVENT_JOIN.md).
                                                  // v97: PyramidGather=85 -- the piramid mirror lane
                                                  //      (host gather-commit relay + 'piramid' verdict
                                                  //      flip to no-replay + piramid2_C pose mirror;
                                                  //      docs/events/piramid.md).
                                                  // v96: TimeSyncPayload carries the NAMED clock triple
                                                  //      (timeZ hour/minute/day) -- the client's clock
                                                  //      display + day number were frozen (TimeScale=0
                                                  //      kept its minute pulse dead and the old 12 B
                                                  //      payload only moved the sun). v95: scheduled-event REPLAY channel -- ReliableKind::
                                                  // EventFire=84 (32 B: [u8 dispatch][name[31]]). The story
                                                  // scheduler (saveSlot::settime -> eventer.runEvent) is
                                                  // BP->BP invisible to every hook; the HOST observes fires
                                                  // by polling saveSlot.passEvents GROWTH (settime Array_Adds
                                                  // each fired row -- bytecode-verified) + the F1 dev menu
                                                  // broadcasts at dispatch (a direct runEvent never appends).
                                                  // CLIENTS: scheduler suppressed structurally (allEvents.Num=0;
                                                  // settime's walk source; boot rebuilds it every load) and
                                                  // replay ONLY policy-allowlisted rows (level/save/cosmetic
                                                  // flips no lane carries -- treehouse/campfire, breaks,
                                                  // obelisk/piramid, forceObjects signals, solar/call0, scare
                                                  // arms, graffiti). Lane-covered rows (prop/npc/atv/sleep/
                                                  // wisp/cue/device) are NOT replayed (double-delivery = the
                                                  // client-local dup class); ariralPrank special never crosses
                                                  // (host-local RNG). Module: coop/world/event_fire_sync.
                                                  // Prior:
                                                  // v94: per-player DISPLAY PREFS -- a [u8 flags] byte appended
                                                  // to Join + PlayerJoined (after each skin field); bit0 =
                                                  // "show my nameplate" (user 2026-07-02: any peer can hide its
                                                  // OWN plate, SYNCED so a late joiner agrees), bits 1..7
                                                  // reserved for future prefs (no proto bump per new bool).
                                                  // Live change = NameplateChange reliable (83). Builtin
                                                  // kerfur-omega skins added to the skin registry (name ->
                                                  // game asset path; same SkinChange wire as pak skins).
                                                  // v93: player SKINS -- SkinChange reliable (82) + the skin
                                                  // name appended to Join (after guid) + PlayerJoined (after
                                                  // nick). Every player carries a persisted body-skin choice
                                                  // (votv-coop.ini player_skin=, default hl_einstein_v1sc);
                                                  // the F1 Cosmetics>Skins browser picks from the converter
                                                  // paks in LogicMods/votv-coop/. v92: kerfur retire eid made CROSS-PEER-STABLE. The v91
                                                  // retireOffEid keyed off a save_identity_bind eid that was
                                                  // bound by a LOAD-ORDER CURSOR -- it floated under join-window
                                                  // churn (15:55 regression: retire killed the WRONG kerfur on
                                                  // both peers). FIX: the sidecar (save_identity_map) now carries
                                                  // the off-kerfur's PORTABLE save key (sidecar v3, variable-len
                                                  // entries) and the client pairs kerfurOff native<->eid BY KEY,
                                                  // not by cursor -> the bound eid is intrinsic + cross-peer-
                                                  // stable -> retireOffEid resolves the SAME kerfur on both peers.
                                                  // chipPiles stay keyless ordinal+position. Wire: sidecar v2->v3.
                                                  // Prior:
                                                  // v91: kerfur reconcile goes DETERMINISTIC (kill the
                                                  // position-fuzzy class). EntitySpawnPayload 108->104:
                                                  // hasMatchPos + matchX/Y/Z (the join-window-turned-ON kerfur's
                                                  // SAVE-TIME position, matched fuzzily within 1cm) REPLACED by
                                                  // retireOffEid (the host eid of the off-prop it replaced -- the
                                                  // joiner retires its off-prop MIRROR by that exact eid, already
                                                  // bound via save_identity_bind); PLUS convertFromEid (the host
                                                  // eid the kerfur converted FROM -- the initiator adopts its
                                                  // parked turn-on ghost by that eid, replacing FindParkedGhostNpcNear's
                                                  // 500cm match). Kills the 5-of-6 / dup + the turn-on respawn-pop.
                                                  // See the runbook RESULT 13:21 + topic memory.
                                                  // Prior:
                                                  // v90 (b3): PropSnapPos=81 + PropSnapPosPayload (28B) --
                                                  // a join-window position correction for save-authoritative
                                                  // chipPiles. A pile moved in the join window broadcasts its
                                                  // PropConvert, which the joiner's not-yet-ready reliable
                                                  // channel DROPS; chipPiles are otherwise save-authoritative
                                                  // (no wire position) so NOTHING else carries the moved pos ->
                                                  // the native sticks at the stale save pos. At the joiner's
                                                  // world-ready the host position-compares each save-time pile
                                                  // vs its current actor + sends PropSnapPos for the diverged;
                                                  // the client snaps the bound native at quiescence. Closes the
                                                  // connect-snapshot's save-authoritative hole (the keyed-prop
                                                  // snapshot + NPC EntitySpawn already carry current pos). See
                                                  // research/findings/coop-b3-window-moved-pile-position-DESIGN.
                                                  // v89: pile GRAB-edge self-seed (docs/piles/09, 4th
                                                  // mirror-identity instance). PropConvertPayload 112->124
                                                  // (+hasMatchPos +matchX/Y/Z): a kToPile LAND carries the
                                                  // pre-grab save-time pos when the host grabbed an UNTRACKED
                                                  // pile in the join window (self-seeded its eid at OnPileGrabPre
                                                  // so NotePendingGrab arms + the pre-grab pos rides the convert).
                                                  // Client OnConvert arms a pending save-time twin -> the
                                                  // bracket-independent SweepReconcileSaveTimeTwins retires
                                                  // native@old. Closes the eid-0-at-grab gap. Prior:
                                                  // v88: scope A v1 kerfur off->active dup RETIRE on the
                                                  // NPC CHANNEL -- EntitySpawnPayload 96->108 (+hasMatchPos
                                                  // +matchX/Y/Z: a join-window-turned-ON kerfur's SAVE-TIME
                                                  // pos, carried on the npc EntitySpawn that DOES reach the
                                                  // joiner; the client retires its stale local off-prop).
                                                  // Replaces v87's convert-driven carry (KerfurConvert
                                                  // SendReliable FAILS mid-join -> retire never fired,
                                                  // hands-on 16:37). KerfurConvert reverted 116->104. Prior:
                                                  // v86: Path 1c join-window pile DUP fix -- PropSpawnPayload
                                                  // 200->212 (+hasMatchPos +matchX/Y/Z: the pile's SAVE-TIME
                                                  // position, stamped on the join snapshot from the host's
                                                  // g_blobPileXforms map; the client twin-destroy matches the
                                                  // save-loaded native against it so a host-moved-in-window
                                                  // pile reconciles instead of duping the two channels). Prior:
                                                  // v85: Increment 2 carry VISIBILITY -- HOST-AUTHORITATIVE
                                                  // trash-clump carry/flight pose stream (new MsgType
                                                  // TrashCarryPose=34 + TrashClumpPoseSnapshot, host-originated
                                                  // per-eid batch). A client-grabbed clump is host-driven; this
                                                  // makes every client (incl. the grabber) render it moving
                                                  // through carry + throw-flight (the relay/slot-0 model can't).
                                                  // eid+ctx gated; client feeds it into a per-eid ActiveDrive
                                                  // (the extracted coop/active_drive.h interp). Prior:
                                                  // v84: Increment 2 (chipPile CLIENT-grab direction,
                                                  // docs/piles/08) -- new CLIENT->HOST GrabIntent (78) +
                                                  // STAGED ThrowIntent (79) / PileResyncRequest (80). The host
                                                  // executes playerGrabbed on puppet-N (probe-proven) + drives
                                                  // the puppet-held clump's hold pose (the puppet tick is dead).
                                                  // Prior:
                                                  // v83: trash PROXY per-form SCALE -- PropConvertPayload gains
                                                  // scaleX/Y/Z (the host's real GetActorScale3D(newActor)); the
                                                  // proxy applies it on every convert+spawn so the mirror is
                                                  // host-sized (it rendered smaller -- scale was never sent on
                                                  // convert nor applied to the AStaticMeshActor proxy). The carry
                                                  // JANK fix (fixed-delay snapshot interpolation) is receiver-only
                                                  // -- no wire change. Prior:
                                                  // v82: HOST-AUTHORITATIVE TRASH CHANNEL (the pile-sync
                                                  // redesign; docs/piles/08). RETIRES the v81 morph
                                                  // (proximity land-watch false-fired in pile CLUSTERS,
                                                  // client direction never wired). Adds a per-eid MTA
                                                  // sync-time-context `ctx` byte to PropConvert (in its
                                                  // existing pad -- no size change), PropPoseSnapshot
                                                  // (60->64), and PropReleasePayload (56->64, also +eid
                                                  // so a keyless clump throw routes by eid not key): host
                                                  // bumps ctx on EVERY trash-entity transition (PILED/
                                                  // HELD/FLYING), stamps it on every convert/carry/throw,
                                                  // and receivers DROP a stale-ctx packet -> a late carry/
                                                  // land packet for eid E can never re-apply to the
                                                  // re-skinned entity. Identity is the host-minted eid
                                                  // end-to-end (position is NEVER identity), so the
                                                  // cluster mis-bind is impossible by construction. The
                                                  // clump<->pile link is the host_spawn_watcher convert-
                                                  // spawn POST (WorldContextObject=source, ReturnValue=
                                                  // new actor -- zero proximity). coop/trash_channel +
                                                  // host_spawn_watcher + remote_prop::OnConvert. (v83 adds
                                                  // the client GrabIntent/ThrowIntent direction.)
                                                  // v81: pile MORPH V2 -- the bind-model grab->carry->
                                                  // throw->land sync (PropConvert=41 re-skins eid E in
                                                  // place: oldEid==newEid==E, +kind byte ToClump/ToPile).
                                                  // RETIRES the v52 fresh-eid death-watch morph. Anchored
                                                  // on the PROVEN held-object channel (local_streams),
                                                  // NOT the un-hookable clump Init-POST / holdPlayer.
                                                  // coop/pile_morph + remote_prop::OnConvert (re-skin).
                                                  // docs/piles/07-MORPH-V2-held-object-channel.md.
                                                  // v80: B3b WorldActor mirror -- WorldActorSpawn=76 +
                                                  // WorldActorDestroy=77 (reliable, reuse EntitySpawn/Destroy
                                                  // payloads) + MsgType::WorldActorPose=33 (unreliable FULL-
                                                  // rotation transform batch). Host-authoritative mirror of the
                                                  // ~14 NON-Character event actors (gray saucers, Rozital
                                                  // mothership, ariral ships, sky UFO, jellyfish, firetank)
                                                  // the Character-only NPC mirror can't replicate. A 2nd
                                                  // BeginDeferred interceptor (disjoint allowlist) + its own
                                                  // coop::element::WorldActor element + world_actor_sync module.
                                                  // See research/findings/votv-b3b-worldactor-mirror-design-
                                                  // 2026-06-17.md. (B3a Character creatures extend kNpcAllowlist.)
                                                  // v79: EventCue=75 -- HOST-AUTH cosmetic emitter-cue mirror
                                                  // (B1, the "sync all events" arc). The host polls for new
                                                  // cosmetic ParticleSystemComponents (eff_shootingStar_rain etc.,
                                                  // spawned via EX_CallMath = PE-invisible) + broadcasts {cueId,pos};
                                                  // clients replay the emitter (reflected SpawnEmitterAtLocation).
                                                  // Fixes the Meteor Shower / Shooting Star ("starfall") not
                                                  // appearing on peers. See coop/event_cue_sync.h.
                                                  // v78: kerfur stable-id redesign -- KerfurConvert=74 host->all
                                                  // form-transition broadcast (the SOLE conversion signal). K-5
                                                  // gates the client mint + streams the held kerfur prop mirror's
                                                  // host-range eid (no new packet -- slot 75/kFlagKerfur scaffolding
                                                  // removed). See coop/kerfur_entity.h + the redesign doc.
                                                  // to occupant-exit too (idle ATV = physics ON everywhere, grabbable);
                                                  // AtvState stateBits gains bit3=authored (connect-snapshot: freeze only
                                                  // an actively-authored ATV, leave idle ones physics-on).
                                                  // (v76: ATV grab-carry sync -- AtvRelease=71 (grab-release/throw
                                                  // edge: re-enable receiver physics + inherit velocity) + ATV authority
                                                  // widened to occupant-OR-grabber. AtvState stateBits gains bit2=grabbed.
                                                  // (v75: EntitySpawn drops WireKey, carries a savePersisted flag instead;
                                                  // NPC adoption is class-match via a deferred poll, NOT key-equality:
                                                  // kerfurOmega::loadData drops the int_save key restore so the key is RANDOM
                                                  // per-peer -- bytecode-proven; only is-a-save-object is portable).
                                                  // v74: EntitySpawn carried the int_save Key (FAILED -- key non-deterministic)
                                                  // + EntityPoseSnapshot carries kerfur State/face (host-authoritative mirror).
                                                  // v73: Join appends [u8 guidlen][guid] -- per-player inventory identity

// Default LAN port (overridable via votv-coop.ini "net.port=").
inline constexpr uint16_t kDefaultPort = 47621;

// The OFFICIAL (built-in) public endpoints -- our VPS. PUBLIC connection
// endpoints, not secrets (every client connects to them); the signaling TOKEN
// / TURN creds are never compiled in. ONE definition here (coop/net) so the
// harness config resolver AND the UI display mask share it: user-visible
// surfaces (the shipped ini, the connect console, browser status) show the
// word "DEFAULT" instead of the raw address when the master equals this --
// no need to advertise the VPS IP in plain sight (user 2026-06-10).
// 2026-07-16: the Cloudzy box (the old VPS hosts no coop services anymore). Becomes
// https://master.votv.mp with the Tier B TLS cutover once DNS lands.
inline constexpr const char* kOfficialMasterUrl    = "172.86.94.3:10001";
inline constexpr const char* kOfficialSignalingUrl = "172.86.94.3:10000";

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
    WorldActorPose = 33, // v80 (2026-06-17, B3b): HOST->all NON-Character event-actor pose BATCH
                       //      (unreliable, ~sendHz). Sibling of EntityPose=32 but FULL rotation
                       //      (ships/UFOs bank + roll) and NO speed/CMC/kerfur -- a WorldActor is a
                       //      plain AActor driven transform-only. Body: EntityPoseBatchHeader (count)
                       //      + N WorldActorPoseSnapshot. Keyed per-entry by WorldActor Element id
                       //      (host range, shared Registry id space); newest-wins. The host reads each
                       //      live allowlisted actor's transform (world_actor_sync::TickPoseStream);
                       //      the client interpolates pos + shortest-arc each angle + drives the parked
                       //      mirror (element::WorldActor). Mirrors the ~14 gray saucers / mothership /
                       //      ariral ships / sky UFO / jellyfish / firetank the Character-only NPC
                       //      mirror cannot replicate (B3 split: B3a Character creatures -> kNpcAllowlist).
    TrashCarryPose = 34, // v85 (2026-06-22, Increment 2): HOST->all carried-trash-CLUMP pose BATCH
                       //      (unreliable, ~sendHz). HOST-AUTHORITATIVE + host-ORIGINATED: a client-
                       //      grabbed pile's clump is driven by the host (on the requester's puppet);
                       //      the relay can't echo a client pose back to its origin (session_relay.cpp:37)
                       //      and a client only drives slot 0 (remote_prop.cpp), so the host originates
                       //      this so EVERY client (incl. the grabber) renders the clump moving. Body:
                       //      EntityPoseBatchHeader (count) + N TrashClumpPoseSnapshot. Keyed per-entry
                       //      by trash eid; ctx-gated newest-wins. Covers carry (kinematic hold) + flight
                       //      (physics after throw); the client feeds it into a per-eid ActiveDrive
                       //      (trash_clump_pose_stream::TickApplyAndDrive). Ends at the ToPile convert
                       //      (PropConvert{ToPile} -> ClearDriveForEid). Drains in TickClientCarryPoses.
    HandPose = 35,     // v109 (2026-07-11): live view-relative transform of the R-HOLD hand
                       //      item (unreliable, ~sendHz WHILE holding). The reliable HandItem
                       //      announce carries the item's IDENTITY (+ initial transform); THIS
                       //      stream carries the MOTION -- the owner re-measures its natively
                       //      welded held actor per pump tick, so a melee swing reads as
                       //      continuous motion on the puppet mirror instead of stepping at the
                       //      old 0.5 s reliable drift refresh (user: "attack animation is 1
                       //      frame per second"). Sender identity rides the PacketHeader
                       //      senderSlot (host relay rewrites -- pose precedent); newest-wins.
    DeskCursorPose = 36, // v109 (2026-07-14): the coords-panel LIVE cursor (ui_coordinates.viewCoordinate)
                       //      as continuous MOTION (unreliable, 60Hz WHILE the desk is claimed + the cursor
                       //      moves). Exact sibling of HandPose=35: the reliable DishAimState carries the
                       //      committed-coord IDENTITY (discrete locks), THIS stream carries the sweep. The
                       //      3Hz reliable snap it replaces read jaggy (user); the mirror runs the proven
                       //      LerpWindow error-window interp (50ms) and writes viewCoordinate via a pure
                       //      memcpy (WriteCursorOnly -- NO updCursorLocations, so no 60Hz pingDishes setRot
                       //      storm; the widget's own Tick repaints). Sender identity rides the PacketHeader
                       //      senderSlot (host relay rewrites); newest-wins by seq. Body: DeskCursorPosePacket.
    ClockPose = 37,    // v109 (2026-07-15): HOST->all world-clock ABSOLUTE snapshot (unreliable, ~2Hz).
                       //      Design F (docs qf_thread_clock): the client daynightCycle is a PURE
                       //      host-auth mirror frozen at TimeScale=0; the desync was the RELIABLE 2 s
                       //      correction refreshing SLOWER than the HH:MM display granularity. This
                       //      is the correct transport for periodic idempotent absolute state (the
                       //      pose-stream pattern) -- newest-wins, a dropped snapshot is corrected by
                       //      the next. The RELIABLE TimeSync(29) is retained ONLY for the connect-edge
                       //      guaranteed initial sync. Body: ClockPosePacket (PacketHeader + TimeSyncPayload).
                       //      NOT relayed (host-originated; clients never forward it).
    DeskSimPose = 38,  // v111 (2026-07-15): HOST->all signal-desk download-SIM output vector
                       //      (unreliable, ~10Hz, newest-wins). OPEN-0 fix (docs qf_thread + signals/):
                       //      the download rate formula rolls two UNSEEDED RNG terms per tick + integrates
                       //      the filter offsets from per-peer frame-dt, so decoded/needle/rate/frData/
                       //      poData/offsets DIVERGE across peers (MEASURED host 0.0064 vs client 0.0262).
                       //      The host owns the sim + streams this vector; the client OVERWRITES (its local
                       //      sim self-accrues garbage the overwrite hides) + interpolates like the cursor.
                       //      The knob INTENTS stay occupant-authored via DeskState (host integrates the
                       //      offset from them) -> this vector is host-down only, one author. Body:
                       //      DeskSimPosePacket. NOT relayed (host-originated).
    DishPose = 39,     // v113 (2026-07-16, L4): HOST->all satellite-dish POSE rows (unreliable,
                       //      newest-wins). The client dish sim is PARKED (tickers killed; own-ping
                       //      slews killed-with-cleanup by the catch detector), so the host streams
                       //      movers-only rows {index, isMoving, axis_Z.rel.Yaw, axis_Y.rel.ROLL}
                       //      at 4 Hz while any dish moves + a SETTLE-TAIL (full-24 sweeps, 1 Hz x3
                       //      after MovingCount hits 0 -- self-heals lost falling edges). ONE
                       //      applier (ApplyDishRow) shared with the DishSnapshot join row: shadow
                       //      -> K2_SetRelativeRotation both axes -> raw isMoving -> activeDishes
                       //      -> cue edges. Design: votv-dish-L4-impl-DESIGN-2026-07-16.md.
                       //      Body: DishPosePacket. NOT relayed (host-originated).
    ReelPose = 40,     // v114 (2026-07-17, L7): HOST->all tape-caddy reel CORRECTOR (unreliable,
                       //      newest-wins seq). The wallunit's 1 Hz accrual keeps running on every
                       //      peer (the park is un-holdable: upd() re-applies SetActorTickEnabled
                       //      at every native verb + wire apply -- the WRITTEN park-doctrine
                       //      deviation, L7 design doc); the host streams {reelBig, reelSmall} at
                       //      1 Hz while ANY slot is occupied (active or not -- heals inactive
                       //      divergence). Client applies per-channel EXACT-SNAP ONLY when
                       //      local != -1 AND wire != -1 (slot-sentinel transitions live
                       //      exclusively on the reliable ReelSlot lane) + an IsRecent window
                       //      after a local insert. Sawtooth <= 1 native increment.
                       //      Body: ReelPosePayload. NOT relayed (host-originated).
                       //      Design: votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md.
    VoiceFrame = 64,   // v66 (2026-06-12): proximity VOICE CHAT -- one 20 ms / 48 kHz mono
                       //      opus frame (SVC pipeline port, MTA transport shape: voice
                       //      multiplexes over the main session, no second socket). A STREAM,
                       //      not a state: the receiver queues every arrival into a per-sender
                       //      voice inbox (FIFO) -> jitter buffer; the per-payload voice seq
                       //      (NOT the header seq) drives PLC/reorder there, so the newest-wins
                       //      header-seq drop the pose kinds use does NOT apply. Host relays to
                       //      every other ready slot (no distance cull at <=4 peers -- the
                       //      RECEIVER's linear attenuation hits zero past the radius anyway;
                       //      deliberate divergence from the port design's host cull, which
                       //      would have needed engine positions on the net thread). Body:
                       //      VoiceFramePayload (8 B head + opusLen bytes).
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
                       //     v59: + event (the short-code submit mirror -- see KeypadEvent).
                       //     SYMMETRIC, MTA input-replication. The keypad verbs are BP-internal
                       //     (bypass our ProcessEvent detour), so the SENDER POLLS each keypad's
                       //     {inPassword, active} each tick and broadcasts on a change, keyed by
                       //     AtriggerBase_C::Key. The RECEIVER REPLAYS inputNumber(digit) for the
                       //     typed-buffer delta (native display+beep; direct buffer clear on a
                       //     shrink -- NEVER the Reset() verb, that's set-new-code mode; upd()
                       //     to repaint) -- which drives the keypad's OWN native validator (the
                       //     BP auto-submits at Len>=5), and runs the native Open(Active) chain
                       //     for a stamped short-code Accept/Deny event. isAcc/isDeny were REMOVED
                       //     v35: the door BP disassembly (2026-06-06) proved they are crosshair-
                       //     HOVER flags, not accept state -- mirroring both was the "PURPLE"; the
                       //     door lock keys on the door's own `Active` (DoorState/CanOpen). Host
                       //     relays client edges + connect-snapshots. Payload: KeypadSyncPayload
                       //     (56 B). Own module coop/keypad_sync.
                       //     RE: research/findings/computers-devices/votv-keypad-door-BP-disassembly-2026-06-06.md.
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
    WindowCleanState = 30, // 2026-06-08 (v41): base-window DIRT scalar sync (AbaseWindow_C::clean
                       //     @0x0260 -- the "main huge window"). SYMMETRIC cooperative-clean (MTA
                       //     monotone min-register): each peer POLLS every AbaseWindow_C's clean each
                       //     tick + broadcasts on a DECREASE (a wipe), keyed by the window's
                       //     Aactor_save_C::Key. The receiver applies MIN(local, wire) so a wire
                       //     update only ever makes a window CLEANER -- concurrent wipes from both
                       //     peers converge to the lowest value, no oscillation/regression (clean is
                       //     monotone-decreasing + inert: nothing re-raises it locally, unlike a
                       //     door's autoclose -> no HostAuth needed). The host RELAYS a client's wipe
                       //     to the other clients (IsClientRelayableReliableKind). On a new client's
                       //     connect edge the host snapshots each window's current clean with adopt=1
                       //     (apply VERBATIM: the joiner adopts the host's world even if its own save
                       //     was cleaner). Sender via POLL (cleanSponge is BP-internal, bypasses our
                       //     ProcessEvent detour, same as doors/keypad). Payload: WindowCleanPayload
                       //     (40 B). Own module coop/window_sync (+ ue_wrap/base_window). RE:
                       //     research/findings/votv-dirt-window-cleaning-RE-and-coop-sync-design-
                       //     2026-06-07a.md.
    GrimeState = 31,   // 2026-06-08 (v42): surface grime dirt scalar (Agrime_C::process @0x0250 --
                       //     walls/ceiling/floor). SYMMETRIC cooperative-clean, MIN-wins, exactly
                       //     like WindowCleanState but keyed by a QUANTIZED WORLD-POSITION string
                       //     instead of an FName: Agrime_C is a STATIC decal with a saved transform,
                       //     so both peers place it at an identical position (same save) -> position
                       //     IS the cross-peer identity (no eid). Each peer polls every grime's
                       //     process + broadcasts on a DECREASE keyed by that position string; the
                       //     receiver applies MIN(local, process) (monotone + inert -> converges,
                       //     no oscillation) + repaints via applyMaterial(). Host relays a client's
                       //     wipe + connect-snapshots each grime's process (adopt=1). Payload:
                       //     KeyedScalarPayload (40 B, shared with WindowCleanState). Own module
                       //     coop/grime_sync (+ ue_wrap/grime). NOTE: propagating a decal's FINAL
                       //     removal (a wipe takes process<0 -> native K2_DestroyActor) is DEFERRED
                       //     (slot 32 reserved): grime lives in streamed sublevels, so a vanished
                       //     decal is not a reliable destroy signal (an IsLiveByIndex death-watch
                       //     flooded false destroys on the connect-teleport stream-out). A wiped
                       //     decal's mirror is driven to process~=0 (invisible) instead; the true
                       //     removal needs a K2_DestroyActor PRE edge that reads process<0.
    // Slot 32 stays RESERVED for the deferred GrimeDestroy (the grime decal final-removal --
    // see GrimeState above); the next two kinds take 33/34 to avoid any future collision.
    GarageDoorState = 33, // 2026-06-08 (v44): base GARAGE door open/close (Agarage_C::Open
                       //     @0x02E8, keyed by AtriggerBase_C::Key @0x0260). SYMMETRIC keyed-
                       //     interactable -- the garage has NO sensor/autoclose (never auto-
                       //     reverts), so a symmetric poll never oscillates and it needs none of
                       //     the door HostAuth machinery (RULE 1). Same generic Channel +
                       //     KeyedTogglePayload (40 B) as LightState/ContainerState. Host relays a
                       //     client edge (IsClientRelayableReliableKind). The wall button just
                       //     toggles Open -> the poll catches it (we never observe the button).
                       //     coop/interactable_sync + ue_wrap/garage. RE: research/findings/
                       //     votv-garage-door-button-sync-RE-2026-06-08.md.
    SkyState = 34,     // 2026-06-08 (v44): HOST-authoritative NIGHT-SKY orientation + moon phase
                       //     (Anewsky_C). The star dome (the `sky` mesh) is given a per-game
                       //     UNSEEDED random yaw RandomFloatInRange(-45,-135) at BeginPlay + a slow
                       //     per-tick spin -> diverges per peer; moonPhase_mirror is read from the
                       //     save (host progressed vs client blank) -> diverges. (TimeSync=29 only
                       //     covers the clock-derived sun/moon ORBIT + brightness.) Host pushes the
                       //     resolved sky WORLD rotation + moonPhase on a ~1Hz throttle + connect
                       //     edge; the client writes them (its ReceiveTick keeps rendering). HOST->
                       //     client only (NOT relayed; trust-gated senderPeerSlot==0). Payload:
                       //     SkyStatePayload (16 B). coop/sky_sync + ue_wrap/skysphere. RE:
                       //     votv-sky-stars-celestial-sync-RE-2026-06-08.md.
    ApplianceState = 35, // 2026-06-08 (v45): simple on/off APPLIANCES sweep batch 1 --
                       //     faucet/sink/shower/kitchen-oven/serverBox/wallunit-tapes (all
                       //     Aactor_save_C descendants, a single bool toggle each, keyed by
                       //     Aactor_save_C::Key @0x0230). SYMMETRIC keyed-interactable like
                       //     GarageDoorState/LightState/ContainerState -- none has a sensor or
                       //     autoclose, so it never auto-reverts and a symmetric poll never
                       //     oscillates (only doors need HostAuth). ONE ue_wrap::appliance Adapter
                       //     dispatches by class to the right bool offset + refresh verb
                       //     (upd/updIsOn/SetActive) so the peer's mesh/FX/audio repaint. Reuses
                       //     KeyedTogglePayload (40 B). Host relays a client edge
                       //     (IsClientRelayableReliableKind). The wall switches/breakers that
                       //     drive these just flip the bool -> the poll catches it (we never
                       //     observe the switch). coop/interactable_sync + ue_wrap/appliance. RE:
                       //     research/findings/props-lifecycle/votv-all-interactables-sweep-catalog-2026-06-08.md.
    PowerControlState = 36, // 2026-06-08 (v46): the base POWER PANEL breakers (ApowerControl_C
                       //     -- 5 latched press bools press_coord/downl/play/calc/light
                       //     @0x0380-0x0384, keyed by AtriggerBase_C::Key @0x0260). SYMMETRIC
                       //     (latched, never auto-reverts). Does NOT fit the generic 1-bool
                       //     Channel (5 bools per actor) -> its own coop/power_sync module + a
                       //     5-bit PowerPanelPayload (modeled on keypad_sync). The base power
                       //     EFFECTS (doors/lights/servers) are synced by their OWN channels;
                       //     this mirrors the PANEL's own breaker/LED visual. Host relays a
                       //     client edge. ue_wrap/power_control. RE: votv-powerControl-panel-
                       //     sync-RE-2026-06-08.md.
    AtvState = 37,     // 2026-06-08 (v47): ATV/quadbike (AATV_C) Phase 1 body POSE + state.
                       //     OCCUPANT-authoritative keyed pose stream -- the peer whose LOCAL
                       //     player is seated (the driver) reads its live ATV root transform +
                       //     throttled-streams it (~20Hz) reliably; the host RELAYS a client
                       //     driver's pose to the other clients (IsClientRelayableReliableKind).
                       //     A receiver applies it KINEMATICALLY (physics+tick disabled on the
                       //     mirror -- the clump/NPC discipline) with a LerpWindow interp, UNLESS
                       //     it is itself the occupant of that ATV (then it ignores -- it is the
                       //     authority). Unoccupied ATVs are not streamed (they hold last pose);
                       //     the host connect-snapshots each ATV's current pose (adopt=1) to a
                       //     joiner. Identity = the save-placed Key@0x0618 (cross-peer stable).
                       //     Phase 2 = fuel/health/brake on-change + broken/explode edge; Phase
                       //     1.5 = occupant->puppet seating (occupantSlot carried now). Payload:
                       //     AtvStatePayload (60 B). coop/atv_sync + ue_wrap/atv. RE: votv-ATV-
                       //     quadbike-RE + votv-ATV-Phase1-pose-stream-blueprint-2026-06-08.md.
    DroneState = 38,   // 2026-06-08 (v48): delivery DRONE (Adrone_C) Phase 1 body pose mirror.
                       //     HOST-AUTHORITATIVE singleton -- the drone is host-simulated (its BP
                       //     ReceiveTick flight integrator); the host streams the resolved actor
                       //     transform (~20Hz while Active) + the Active flag; the client
                       //     SUPPRESSES its own drone ReceiveTick (so it can't fly on its own +
                       //     fight the stream) and mirrors the transform kinematically with a
                       //     LerpWindow interp. Singleton identity (FindObjectByClass(drone_C) --
                       //     no key; both peers load the same placed drone). HOST->client only
                       //     (NOT relayed; trust-gated senderPeerSlot==0, like SkyState). Cargo
                       //     (the delivered box) rides the EXISTING Aprop_C prop pipeline -- no
                       //     drone cargo packet. Phase 2 = flyingType/hasSack FX + the console
                       //     call (client->host) + radar Active; v69 adds the rotor-dust anchor
                       //     (the dust component's ground-pinned world location -- see the v69
                       //     header note). Payload: DroneStatePayload (40 B).
                       //     coop/drone_sync + ue_wrap/drone. RE: votv-delivery-drone-RE-and-
                       //     coop-sync-design-2026-06-03.md + research/pak_re/drone_dust_notes.md.
    OrderRequest = 39, // 2026-06-09 (v49): delivery-drone ECONOMY -- a CLIENT forwards a laptop
                       //     shop order to the HOST so the shared drone delivers it. VOTV has NO UE
                       //     replication, so a client's makeAnOrder is 100% client-LOCAL (Array_Adds
                       //     into the CLIENT's own saveSlot.orders + flies the CLIENT's mirror drone).
                       //     So the client POLLS its saveSlot.orders.Num (the commit verb is BP-
                       //     internal/unobservable), and on an increment serializes the new order's
                       //     items (each item's `object` TSubclassOf as a CLASS NAME -- the load-
                       //     bearing field the host re-resolves via FindClass) + price/size/category +
                       //     time, CHUNKED across reliable messages (kMaxReliablePayload=228 B). The
                       //     HOST assembles the chunks per (senderSlot, orderId) and re-commits via
                       //     the native Uui_laptop_C::makeAnOrder(order, automatic=true) -- the proven
                       //     commit+deliver+native-drain path (automatic=true = free/unpaid: charging
                       //     lives in the laptop Button_order graph, NOT makeAnOrder -- bytecode-
                       //     verified). Cargo box (Aprop_C) then rides the EXISTING prop pipeline + the
                       //     drone body rides DroneState. CLIENT->HOST only (NOT relayed; the host is
                       //     the delivery authority). After forwarding, the client RESETS its mirror
                       //     drone (Active:=false/flyingType:=-1/hasOrder:=false -- the checkOrders
                       //     empty-queue arm) so its locally-run sendShop can't fake a takeoff.
                       //     Variable-length: OrderRequestHeader + packed items. coop/order_sync +
                       //     ue_wrap/order_economy. RE: votv-delivery-drone-RE-and-coop-sync-design-
                       //     2026-06-03.md (ECONOMY BUILD PLAN + the 5-question commit/remove RE).
    FireflySpawn = 40, // 2026-06-09 (v51): PEER-SYMMETRIC ambient FIREFLY mirror. The firefly
                       //     spawner (Aticker_fireflySpawner_C) rolls per-peer RNG every 30 s +
                       //     SpawnEmitterAtLocation's eff_fireflies near the LOCAL camera over grass.
                       //     Fireflies are camera-relative (no shared position), so EVERY peer keeps
                       //     running its own spawner AND shares each spawn -> the union: every peer sees
                       //     fireflies near itself PLUS near every other peer (a host-only design would
                       //     leave a far client barren). The spawn is an EX_CallMath native call (bypasses
                       //     our ProcessEvent detour -- NOT observable at the call site), so each peer
                       //     captures its OWN spawn by PRE+POST-observing the spawner's ReceiveTick
                       //     (ProcessEvent-dispatched) and diffing the live ParticleSystemComponent set
                       //     across that one synchronous tick: the new component is the firefly -> broadcast
                       //     its world location. On receive, a peer spawns eff_fireflies there via a
                       //     reflected SpawnEmitterAtLocation. RELAYED (IsClientRelayableReliableKind): a
                       //     client -> host -> the OTHER clients; no suppression; no echo (the origin never
                       //     receives its own send). Transient -> no connect-snapshot. Payload: FireflySpawn
                       //     Payload (12 B). coop/firefly_sync. RE: votv-firefly-host-mirror-RE-2026-06-09.md.
    PropConvert = 41,  // 2026-06-09 (v52): trash-clump ball->pile ATOMIC convert. The owner's clump
                       //     death-watch (the morph-destroy is BP-internal/unobservable) emits ONE event
                       //     the instant its watched clump dies: {oldEid=the broadcast ball, newEid=mint
                       //     for the new pile, pileClass, resting transform, chipType, landing vel}. The
                       //     receiver ATOMICALLY destroys the ball mirror by oldEid AND spawns the pile by
                       //     newEid (a settled landed pile -- no morph). One
                       //     ordered datagram, two distinct eids -> no lingering ball, no double pile, no
                       //     cross-peer FindNearestChipPile guess. Re-grab destroy then propagates by IDENTITY
                       //     via a mirror-pile death-watch (PropDestroy(eid)). NOT relayed beyond the host's
                       //     own fan-out (a client clump's convert reaches the host like any PropSpawn).
                       //     Payload: PropConvertPayload (100 B). coop/trash_collect_sync + remote_prop::On
                       //     Convert. RE: votv-clump-lifecycle-observability-and-robust-design-2026-06-08-pass2.md.
    SaveTransferRequest = 42, // 2026-06-10 (v56): a MENU-MODE joining client asks the host for its
                       //     world save (the save-transfer join bootstrap -- the client loads the
                       //     HOST's save instead of generating a divergent fresh world; user
                       //     mandate "pull all objects data at connecting time"). Sent once by the
                       //     client right after the connect edge, ONLY when the client armed the
                       //     transfer (browser/menu join). Env/autotest clients that already booted
                       //     a world never send it -- they keep the fresh-world+true-up baseline.
                       //     Payload: none. Host replies SaveTransferBegin + chunks.
    SaveTransferBegin = 43, // 2026-06-10 (v56): HOST->client save-transfer header: total byte size,
                       //     chunk count, CRC32 of the whole blob. totalBytes==0 = "no save
                       //     available" (host has no slot file) -> the client falls back to the
                       //     fresh-world boot. Rides Lane::Bulk strictly ahead of its chunks.
                       //     Payload: SaveTransferBeginPayload (16 B).
    SaveTransferChunk = 44, // 2026-06-10 (v56): one save-blob chunk: u32 chunk index + raw bytes
                       //     (kSaveChunkBytes data max). EXCEEDS kMaxReliablePayload BY DESIGN --
                       //     session.cpp diverts this kind to the registered bulk sink (heap
                       //     assembler in coop/save_transfer) BEFORE the fixed-228B inbox copy;
                       //     it never enters the ReliableMessage ring. GNS fragments/reassembles
                       //     the ~56KB message internally; Lane::Bulk keeps poses/events flowing.
                       //     Host-paced against send-buffer backpressure (retry on send failure).
    ClientWorldReady = 45, // 2026-06-10 (v56): CLIENT->host "my gameplay world is loaded -- send the
                       //     world now". THE single trigger for the host's connect replay (snapshot
                       //     bracket + weather/time/keyed-state broadcasts), replacing the connect-
                       //     edge trigger: a menu-mode client is connected ~30-60 s BEFORE it has a
                       //     world (downloading + loading the save), and the pre-v56 connect-edge
                       //     bracket would stream 3000+ PropSpawns at a worldless menu. Sent once
                       //     per connection by net_pump's client tick when Local() resolves.
                       //     Payload: none.
    TrashPileState = 46, // 2026-06-10 (v57): trashBitsPile_C collect-counter mirror (the "uses
                       //     6/7" dispenser piles). amountA@0x0260 + amountB@0x0264 int pair --
                       //     the displayed count is their SUM, formatted live by lookAt (no
                       //     refresh verb exists or is needed; raw writes are fully consistent).
                       //     SYMMETRIC poll channel (grime shape): three BP-internal writers
                       //     (E-grab playerGrabbed, prop_vacuum2 vacuumed, prop_broom broomed)
                       //     make input observers unsound -- each peer polls its indexed piles
                       //     and broadcasts on a DECREASE, keyed by Aactor_save_C::Key@0x0230;
                       //     receiver applies per-component MIN (monotone-down -> concurrent
                       //     collects converge) or VERBATIM for the host adopt=1 connect-snap
                       //     (also trues up the per-peer BeginPlay RNG re-rolls of rowless
                       //     piles). Depletion self-destroys BP-internally (K2_DestroyActor via
                       //     ProcessInternal -- invisible to our observer): caught by the
                       //     proximity-gated death-watch (grime super-sponge shape) -> the
                       //     EXISTING keyed PropDestroy. The dispensed item itself needs NO new
                       //     wire: pickupObjectDirect routes it through the held-edge broadcast
                       //     + pose stream. Payload: TrashPileStatePayload (40 B). Module:
                       //     coop/trash_pile_sync. RE: the trashBitsPile bytecode pass
                       //     (findings HANDS-ON ROUND 2 (4)).
    InventoryPickup = 47, // 2026-06-11 (v58): the inventory-collect BLIP (inventory_Cue,
                       //     the "Half-Life pickup") made audible to OTHER peers. Natively
                       //     putObjectInventory2 plays it PlaySound2D = 2D collector-only,
                       //     and a remote collect reaches peers only as a bare PropDestroy --
                       //     structurally silent. The collector POST-observes GameplayStatics::
                       //     PlaySound2D (EX_FinalFunction on the CDO = ProcessEvent-visible)
                       //     gated on Sound==inventory_Cue + pitch~1.1 + WCO==local player
                       //     (zero other matches in the game-wide cue census; fires once per
                       //     successful collect across all 26 putObjectInventory2 call sites)
                       //     and broadcasts its world position; every other peer plays the cue
                       //     spatialized there (vol 1.0 / pitch 1.1 / att_default). PEER-
                       //     SYMMETRIC + host-relayed (firefly shape). Payload:
                       //     InventoryPickupPayload (12 B). Module: coop/inventory_pickup_sync
                       //     (+ prop_sound playback). RE: votv-inventory-pickup-seam-RE-
                       //     2026-06-11.md.
    ChatMessage = 48,  // 2026-06-11 (v60): the T-chat line (user req "chat on T, rule 1").
                       //     PEER-SYMMETRIC + host-relayed (firefly relay shape): any peer
                       //     types in the T input bar (ui/chat_input); the line broadcasts
                       //     and every receiver pushes "<nick>: <text>" into chat_feed (the
                       //     nick resolves from senderPeerSlot via player_handshake -- the
                       //     wire carries TEXT only, identity comes from the transport).
                       //     The feed's existing TTL fade gives the requested disappear-
                       //     when-idle; ESC closes the input (the game pause menu opens
                       //     normally). Payload: ChatMessagePayload (<=220 B). Module:
                       //     coop/chat_sync (+ ui/chat_input).
    TurbineState = 49, // 2026-06-11 (v61): GIANT WIND TURBINE facing/spin mirror (user req:
                       //     "their direction diverges between client and host"). HOST-
                       //     authoritative, ~1 Hz per turbine (~13 placed): the turbine BP is
                       //     a per-tick SERVO chasing the directionalWind direction (which v50
                       //     already syncs) -- divergence comes from the NON-persisted servo
                       //     integrator `rot` resetting to 0 each world load + a 1 deg/s chase
                       //     + per-peer numeric noise during calm + a BeginPlay rand(0.9,1.0)
                       //     blade-rate multiplier. The host streams the 6 driver floats; the
                       //     client writes them RAW (no engine calls -- the turbine's own tick
                       //     consumes them; its head spring IS the interpolator). NO
                       //     suppression needed (no live RNG re-targeter; setRandRot is dead
                       //     code). Identity = quantized world position (grime PosKey style;
                       //     one map-baked pair SHARES a save Key, so Key-indexing would
                       //     collapse them). NOT relayed (host-only origin). Payload:
                       //     TurbineStatePayload (56 B). Module: coop/turbine_sync +
                       //     ue_wrap/windturbine. RE: votv-wind-turbines-RE-2026-06-11.md.
    LockerDoorState = 50, // 2026-06-11 (v62): hinged-door storage boxes -- the ~19 base/map
                       //     LOCKERS (locker_C + locker_personal_C/locker_death_C) and the
                       //     DRONE-CALL CONSOLE box (droneConsole_C). SYMMETRIC keyed toggle
                       //     (zero auto-revert writers of `opened`, bytecode-scanned) riding
                       //     the interactable Channel with ONE door_box adapter. Identity =
                       //     the level-export actor FName (neither class carries a save Key;
                       //     names are deterministic for placed actors). Apply: locker = the
                       //     native BP verb Open(bool) (sound+swing+collision+trigger);
                       //     console = opened write + setButtonsCollision() + Timeline
                       //     Play/Reverse (the garage write+refresh precedent); both get the
                       //     door.cpp verify+force-snap for far-frozen swings. The radiotower
                       //     "mast box" doors are prop_swinger_C child actors -- already on
                       //     the container channel. Payload: KeyedTogglePayload (40 B).
                       //     RE: votv-lockers-boxes-door-RE-2026-06-11.md.
    DeviceClaim = 51,  // 2026-06-12 (v63): enterable-device OCCUPANCY claim/release (base
                       //     computers/terminals phase 1). CLIENT->HOST request + HOST->all
                       //     broadcast (one kind, both directions; host re-broadcasts after
                       //     arbitration, so NOT in the client-relay whitelist). First-wins:
                       //     a losing claimant gets a point-to-point busy=1 carrying the
                       //     WINNER's slot and force-exits its own player (reflected
                       //     setActiveInterface(null), the ragdollMode exit path) + plays
                       //     button_keypad_deny. busy=0 releases (activeInterface falling
                       //     edge -- covers ESC, ragdoll, death). Host clears a leaver's
                       //     claims on the per-slot disconnect edge; the connect replay
                       //     sends the live claim table to a joiner. Keyed by the SHARED
                       //     WIDGET identity ("desk"/"sat"/"radar"/"reactor"/"laptop" --
                       //     one shared widget instance each -- + "tfm_<posKey>" /
                       //     "arc_<posKey>" per-instance). Payload: DeviceClaimPayload
                       //     (36 B). Module: coop/device_occupancy + ue_wrap/device_screen.
                       //     RE: votv-base-computers-RE-2026-06-11.md.
    SkySignalState = 52, // 2026-06-12 (v64): HOST-authoritative sky-signal SET snapshot
                       //     (the coords-minigame targets, spaceRenderer.signals). The
                       //     host's native spawnSignal roller is the ONLY roller; a client
                       //     kills its own roller timer (K2_ClearTimer; restored by one
                       //     reflected spawnSignal() on disconnect) and mirrors the wire
                       //     set verbatim (addSignal(coords) + row overwrite + widget
                       //     prop pushes; widget lifetimes are wire-driven via keepalive,
                       //     so ONLY wire snapshots remove rows). Sent on the host's ~1 Hz
                       //     poll detecting ANY set change, in parts of <=3 wire rows
                       //     (gen byte guards cross-snapshot part mixing). Trust-gated to
                       //     slot 0; NOT relayed. Payload: SkySignalStatePayload (188 B).
                       //     Module: coop/console_state_sync + ue_wrap/space_renderer.
    SkySignalCatch = 53, // 2026-06-12 (v64; CONSUME REPLAY since v70): the signal-catch
                       //     world event. kind=0 catch: the desk-claim holder's detector
                       //     (row vanished + dish isMoving rising edge, 1 Hz) ships the
                       //     full consume replay {coord_signalData row content + the
                       //     exact dish slew vector}; the HOST validates the claim
                       //     holder, replays (struct write -> row delete -> download-
                       //     machine reset -> ping sound -> StartMovingAll(slew)) and
                       //     rebroadcasts to everyone but the catcher -- the NATIVE
                       //     dish-arrival chain then arms formDownload per peer. kind=1
                       //     cleared: any peer's 'Signal data deleted' button (objectName
                       //     ->None edge, unclaimed trust), replay = the @33832 reset
                       //     chain. Host also sends one kind=0 to a JOINER while
                       //     coord_signalData is armed (slewValid=0 once dishes settled
                       //     -> the joiner arms formDownload directly). Receivers
                       //     register the identity in a short-TTL recent-catch set that
                       //     filters stale in-flight SkySignalState snapshot rows.
                       //     Payload: SkySignalCatchPayload (80 B). Module:
                       //     coop/signal_catch_sync + ue_wrap/dish + ue_wrap/console_desk.
    DeskState = 54,    // 2026-06-12 (v64; reshaped v70; ADOPT-ONLY since v112): the desk's
                       //     scalar snapshot. v112 (RULE 2): the live claimed stream and
                       //     the unclaimed edge lane are RETIRED -- live input rides the
                       //     claim-free field-granular DeskInput=97 deltas; sim outputs
                       //     ride DeskSimPose. This kind now carries ONLY the host->joiner
                       //     connect ADOPT snapshot (adopt=1; seeds all fields incl.
                       //     cooldown/speeds + the dlDecoded/dlPolarity download catch-up
                       //     and primes the joiner's poll baselines). Payload:
                       //     DeskStatePayload (60 B). Module: coop/console_state_sync.
    DishAimState = 55, // 2026-06-12 (v64): the coords-panel cursor state (the
                       //     ui_coordinates widget: viewCoordinate + Coordinate_0..2 +
                       //     selected + the v70 Direction toggle) streamed ~3 Hz by
                       //     the desk-claim owner while claimed; host-relayed. Receivers
                       //     raw-write + reflected updCursorLocations (which also rotates
                       //     the physical pingDishes). Payload: DishAimStatePayload (40 B).
    EmailAppend = 56,  // 2026-06-12 (v64, increment 2): the meadow-PC email mirror. Every
                       //     producer (daynightCycle task mails, drone sell responses,
                       //     console/desk status mails) funnels through gamemode.addEmail
                       //     -- PE-invisible, so the PRODUCING peer watermark-polls
                       //     saveSlot.emails.Num and ships new rows (host-relayed); the
                       //     receiver rebuilds the Fstruct_email (FTexts minted from
                       //     strings, pfp by leaf name or null, date zeroed) and reflected
                       //     gamemode.addEmail applies persistence + list row + the email
                       //     ding + tab highlight in one call (which also RE-STAMPS the
                       //     date from the host-synced clock). Echo-proof: a wire apply
                       //     advances the local watermark past its own append. Topics/
                       //     texts are paragraphs -> VARIABLE-LENGTH chunked stream (the
                       //     OrderRequest shape): EmailChunkPayload (228 B) parts keyed
                       //     (senderSlot, emailSeq). RE:
                       //     votv-computers-phase2-impl-RE-2026-06-12.md SS3.
    EmailDelete = 57,  // 2026-06-12 (v65): the email DELETE mirror -- ui_laptop.delEmail
                       //     (the row's del button) propagated cross-peer. Content-keyed
                       //     by the FNV-1a 64 of the row's serialized EmailAppend blob;
                       //     wire INDEXES are unsafe because concurrent producer appends
                       //     order differently per peer. The deleting peer's shadow diff
                       //     detects the shrink and ships the removed hash; receivers
                       //     resolve their local index by hash and call the native
                       //     delEmail (list slot + saveSlot row in one). Unknown hashes
                       //     tombstone briefly so a delete that outruns its own row's
                       //     chunked append still lands. Payload: EmailDeletePayload.
    SavedSignalAppend = 58,  // 2026-06-12 (v65): saved-signals list (gamemode.savedSignals_0)
                       //     append mirror -- the email shadow/diff shape on the desk's
                       //     signal library. Rows are Fstruct_signalDataDynamic (0x70,
                       //     pure POD+strings; textures/sounds re-derive natively at play
                       //     time via lib_C.dynamicToSignal <- list_signals templates, so
                       //     the row is fully serializable). Producer's poll diffs the
                       //     array, serializes new rows (WITHOUT the image PNG blob --
                       //     laptop-photo bytes are a deferred bulk-lane increment) and
                       //     ships BlobChunkPayload chunks; receivers re-play the native
                       //     gamemode.saveSignal(row,...,selfQuality=true) = append +
                       //     "Create Signal List" pane rebuild + specials/forceObjects
                       //     bookkeeping in one call. Join converges via the v56 save
                       //     transfer (savedSignals_comp_0 rides the .sav). RE: the
                       //     2026-06-12 savedSignals agent pass (saveSignal walkthrough).
    SavedSignalDelete = 59,  // 2026-06-12 (v65): saved-signals delete mirror, content-keyed
                       //     (ContentHashPayload) exactly like EmailDelete -- the game's
                       //     deleteSignal is index-based and ids are non-unique (copies
                       //     share them), so the wire identity is the serialized-blob
                       //     hash; receivers resolve their index + reflected
                       //     deleteSignal(index) (native pane refresh). Covers both the
                       //     play-pane delete AND the export-to-drive move (list side;
                       //     drive CONTENT sync is a documented separate gap).
    CompState = 60,    // 2026-06-12 (v65): the refiner decode-pane scalar stream. The
                       //     SIMULATING peer (the one whose comp_isDecodeActive latched
                       //     natively -- the start-presser; or the host after a save
                       //     load) broadcasts ~1 Hz while decoding + on edges. Mirrors
                       //     stay PASSIVE: raw-write comp_progress/comp_downloading and
                       //     repaint texts/cues; they NEVER latch comp_isDecodeActive --
                       //     the decode ticker has NO occupancy gate (RE: tick chain
                       //     @76437->@75579->@71488), so a latched mirror would simulate
                       //     independently and double-fire completion (incl. the
                       //     level-3 theEvil_C spawn). Payload: CompStatePayload.
    CompData = 61,     // 2026-06-12 (v65): the refiner's loaded signal (comp_data_0,
                       //     Fstruct_signalDataDynamic) -- BlobChunkPayload chunks on
                       //     change edges (drive upload / eject / completion level-up)
                       //     + host adopt at connect-replay. Receivers write the live
                       //     struct in place (engine-minted FStrings/FNames) + updComp
                       //     repaint. Image blob skipped live (same deferral as 58).
    VoiceState = 62,   // 2026-06-12 (v66): voice-chat presence state {micMuted,
                       //     voiceDisabled} broadcast on edges (player-symmetric,
                       //     host-relayed). DISPLAY-ONLY (nameplate/scoreboard/HUD
                       //     icons) -- routing never depends on it: a muted sender
                       //     simply produces no VoiceFrame datagrams. Talking/whisper
                       //     indicators derive client-side from decoded frames (the
                       //     SVC TalkCache shape), zero extra packets. Payload:
                       //     VoiceStatePayload.
    KerfurConvertRequest = 63,  // 2026-06-12 (v67): CLIENT->host kerfur on/off
                       //     CONVERSION request (the dupe fix -- see the v67 header
                       //     note). The client PRE-cancels its local menu dispatch
                       //     (kerfurOmega_C::actionName 'turn_off' / prop_kerfurOmega_C::
                       //     actionOptionIndex action==8) and sends the target's
                       //     elementId + direction; the HOST validates (live actor,
                       //     right class, BP kill-guard) and executes the real verb --
                       //     the conversion's spawns/destroys then mirror through the
                       //     existing Entity*/Prop* pipelines via the explicit converge
                       //     (coop::kerfur_convert). DoorOpenRequest shape: host-only
                       //     receiver; the authoritative results flow back as
                       //     EntityDestroy + PropSpawn (turn_off) or PropDestroy +
                       //     EntitySpawn (turn on). Payload: KerfurConvertPayload.
    PropStickState = 64,  // 2026-06-12 (v68): wall-attachable STICK mirror (see the
                       //     v68 header note). Sent by the STICKING peer on the pump
                       //     pass after its comp_wallAttachable commit (commit pose;
                       //     ordered BEFORE the hold-break PropRelease in-lane);
                       //     symmetric prop state -- the host relays a client's stick
                       //     to the other clients (IsClientRelayableReliableKind).
                       //     Receiver: stop drive -> re-pose -> simulate(true) ->
                       //     PE forceStick(true) (SP replay; raw frozen-write
                       //     fallback). The unstick direction deliberately has NO
                       //     message: it mirrors through the existing PropPose
                       //     sustained-stream gate + the frozen-gated release paths
                       //     (coop/prop_stick_sync + remote_prop). Payload:
                       //     PropStickStatePayload.
    DeskLogLine = 65,  // 2026-06-12 (v70): one coords-terminal EVENT line, produced by
                       //     the peer whose local action wrote it (exact producer-side
                       //     diff of coord_coordLog2Text; animated bar/status lines
                       //     filtered by prefix -- they self-generate on every peer
                       //     from mirrored scalars). Host-relayed (producer-symmetric).
                       //     Receiver: writeToCoordLog_2 append (native CRLF/repaint/
                       //     1000-cap) + own-baseline advance (echo-proof). Replaces
                       //     the v64 DeskState coordLog tail diff (RULE 2 -- see the
                       //     v70 header note). Payload: DeskLogLinePayload (124 B).
    SleepState = 66,   // 2026-06-13 (v71): the Minecraft sleep gate (see the v71
                       //     header note). BOTH directions on one kind (the
                       //     DeviceClaim shape): op=Report is any peer's isSleep
                       //     edge toward the host (trust = the TRANSPORT sender
                       //     slot); op=Tally/Accelerate/End are host->all phase
                       //     broadcasts (clients trust-gate to slot 0). NOT
                       //     client-relayed (host-mediated). Payload:
                       //     SleepStatePayload (4 B). Module: coop/sleep_sync.
    WispGrab = 67,     // 2026-06-13 (v72): the Killer Wisp catch/kill. HOST->ONE victim
                       //     slot: the host detected its killerwisp_C grabbing this
                       //     client's puppet (Target classification), neutralized its own
                       //     false-grab, and tells the victim to die for real. The victim
                       //     self-verifies victimElementId==own, parks its CMC, and
                       //     schedules ragdollMode(true,false,true) on its OWN player after
                       //     killDelayMs (host-decided, ~the tear duration). HOST-ONLY
                       //     (senderPeerSlot==0 gate); NOT client-relayable. Payload:
                       //     WispGrabPayload (12 B). Module: coop/wisp_attack_sync (send) +
                       //     coop/wisp_tear_mirror (receive). Design:
                       //     research/findings/npc-creatures/votv-killerwisp-coop-design-2026-06-13.md.
    WispTear = 68,     // 2026-06-13 (v72): the Killer Wisp tear MIRROR. HOST->ALL: every
                       //     peer resolves its local wisp NPC mirror by wispElementId and
                       //     plays the fatality tear on it (force-tick the parked mirror
                       //     mesh + Montage_Play 'fatality' + spawn/weld the 4 limb gibs)
                       //     and socket-attaches the victim's PUPPET (Registry::Puppet(
                       //     victimSlot)) to the wisp 'playerGrab' socket. On the victim's
                       //     OWN machine victimSlot==own -> no puppet (its real death is the
                       //     view). HOST-ONLY; NOT relayable. Payload: WispTearPayload (8 B).
    PlayerInventoryBlob = 69,  // 2026-06-14 (v73): per-player inventory stream. CLIENT->HOST:
                       //     a client streams its serialized saveSlot inventory (inventoryData
                       //     + equipment + hold, via coop/inventory_wire) on change (~1 Hz,
                       //     FNV-deduped); the host persists it to coop_players/<guid>.json.
                       //     Reuses BlobChunkPayload (chunked over Lane::Bulk; no new payload
                       //     struct). HOST-TERMINAL -- NOT client-relayable (never fanned out to
                       //     other peers). The HOST->CLIENT apply-on-join (Inc 4) reuses this
                       //     kind in the reverse direction. Module: coop/player_inventory_sync.
                       //     Plan: research/findings/inventory-items/votv-inventory-impl-plan-2026-06-14.md.
    KerfurCommand = 70,  // 2026-06-14 (v74): host-authoritative kerfur radial-menu command relay.
                       //     CLIENT->HOST (+ host's own menu use, executed locally): a player picks
                       //     a kerfur menu verb (follow/idle/patrol/fix_servers/get_reports/
                       //     fix_transformers); the client cancels the local actionName dispatch (the
                       //     verbs hard-pin GetPlayerPawn(0) -> would follow the wrong player) and
                       //     sends KerfurCommandPayload{eid, command}. The host runs the real verb via
                       //     ProcessEvent (State change streams to mirrors via the pose stream), EXCEPT
                       //     Follow: the host sets State=idle + drives a CreateMoveToProxyObject loop
                       //     toward THE REQUESTING PLAYER's body (senderPeerSlot; the BP can't, it has
                       //     no pawn for remote players). turn_off stays in KerfurConvertRequest.
                       //     Module: coop/kerfur_command. RE: votv-kerfurOmega-coop-double-and-camera-
                       //     RE-2026-06-14.md sec 7 + the menu-command RE agent.
    AtvRelease = 71,   // 2026-06-15 (v76): ATV grab-carry RELEASE edge (companion to AtvState=37).
                       //     ATV authority widened from "seated occupant" to "occupant OR grav-hand
                       //     GRABBER" (atv_sync IsLocalAuthority): a peer that E-grabs an ATV streams
                       //     its airborne pose like a driver; receivers PrepareMirror (physics off) +
                       //     interp. On the grab-release/throw edge the grabber sends AtvReleasePayload
                       //     {key, lin/ang velocity}; the receiver ReleaseMirror (physics ON) THEN sets
                       //     velocity (kinematic body ignores velocity writes -- order matters, the
                       //     PropRelease apply pair) so the ATV un-freezes + arcs + lands instead of
                       //     hanging at last pose. Same lane as AtvState (Normal, in-order: last pose
                       //     before release) + relayed to other clients. RE: votv-ATV-grab + the design
                       //     in research/findings (ATV grab/air-move + purchased-ATV brief 2026-06-15).
    AtvSpawn = 72,     // 2026-06-15 (v77): HOST->ALL purchased-ATV announce. A bought ATV is delivered
                       //     by the HOST's order economy ONLY (order_sync is host-authoritative: a
                       //     client's order is forwarded + re-committed on the host; the client resets
                       //     its mirror drone so its local delivery never spawns). So the joining/other
                       //     clients have NO local twin of a purchased ATV -- the host fresh-spawns it
                       //     for them: AtvSpawnPayload{synthKey, className, pose}. The client BeginDeferred
                       //     -spawns the AATV_C (physics ON = a native idle ATV, grabbable by anyone) and
                       //     registers it under the host-assigned SYNTHETIC wire key (a purchased ATV's
                       //     own int_save Key is minted RANDOM per peer -- the kerfur trap -- so it is
                       //     NEVER used cross-peer; the synth key is the stable identity). The existing
                       //     AtvState/AtvRelease key-stream then drives it unchanged. Default save-placed
                       //     ATVs (deterministic key, both peers loaded them) stay on the real-key path.
    AtvDestroy = 73,   // 2026-06-15 (v77): HOST->ALL purchased-ATV teardown. The host's synthetic-keyed
                       //     ATV vanished (sold/removed) -> AtvDestroyPayload{synthKey}; the client
                       //     K2_DestroyActors its fresh-spawned mirror + drops the index entry. Same
                       //     Normal lane as AtvSpawn/AtvState (spawn->pose->destroy in order).
    KerfurConvert = 74,    // 2026-06-16 (v78): HOST->ALL kerfur form-transition broadcast -- the SOLE
                       //     conversion signal (redesign section 10.3). Carries the stable KerfurId + the
                       //     new form + the new-form wire eid + transform + class name (or rejected=1 on a
                       //     sentient/kill refusal). Every client destroys its old-form mirror + adopts its
                       //     own claimed local conversion ghost to the authoritative newEid (initiator) or
                       //     materializes a fresh mirror (others). Payload: KerfurConvertBroadcastPayload.
    EventCue = 75,     // 2026-06-17 (v79): HOST-AUTH cosmetic emitter-cue mirror (B1, the "sync all events"
                       //     arc). The host polls for a NEW cosmetic ParticleSystemComponent whose Template is
                       //     a registered cue (eff_shootingStar_rain @ (0,0,6000) = the Meteor Shower / Shooting
                       //     Star "starfall"; spawned via EX_CallMath SpawnEmitterAtLocation = PE-invisible, no
                       //     mirrorable actor) + broadcasts {cueId, pos}; every client replays the emitter via a
                       //     reflected SpawnEmitterAtLocation. HOST-AUTHORITATIVE (clients run a dormant
                       //     scheduler -- time_sync pins their TimeScale=0 -- so they never fire events
                       //     themselves): NOT relayable, NO suppression. Payload: EventCuePayload. Cue
                       //     registry: coop/event_cue_sync.cpp. (Slot 75 was briefly KerfurHoldRequest
                       //     scaffolding in K-4a, removed in K-5 BEFORE it ever shipped on the wire -- never
                       //     sent, so the id is genuinely free; reused here with the v79 version bump.)
    WorldActorSpawn = 76,  // 2026-06-17 (v80, B3b): HOST->ALL non-Character event-actor SPAWN announce
                       //     (the WorldActor analogue of EntitySpawn=5). Reuses EntitySpawnPayload
                       //     (className + host-range elementId + loc/rotPYR; savePersisted always 0 --
                       //     event actors are never save objects). HOST-AUTHORITATIVE: the host's
                       //     BeginDeferred interceptor (a SECOND interceptor on the same UFunction;
                       //     disjoint allowlist from npc_sync) allocates a WorldActor Element + broadcasts;
                       //     the client suppresses its own (dormant scheduler never fires events anyway)
                       //     + materializes a transform-only mirror (world_actor_sync). NOT client-
                       //     relayable, NOT pre-world-sendable. event_dispatch_entity dispatches it.
    WorldActorDestroy = 77, // 2026-06-17 (v80, B3b): HOST->ALL non-Character event-actor DESTROY (the
                       //     WorldActor analogue of EntityDestroy=6). Reuses EntityDestroyPayload
                       //     (elementId only). Host K2_DestroyActor PRE observer broadcasts; the client
                       //     tears down its mirror. Host-authoritative; shares the Bulk lane with
                       //     WorldActorSpawn (in-order spawn-before-destroy, same as EntitySpawn/Destroy).
    GrabIntent = 78,   // 2026-06-22 (v84): CLIENT->HOST chipPile grab REQUEST (Increment 2, docs/piles/08).
                       //     The client-grab direction, structurally the door OnRequest shape
                       //     (interactable_channel.h:220, Channel::Mode::HostAuth) + MTA AttachElements
                       //     (CStaticFunctionDefinitions.cpp:1602). CLIENT: suppress the native grab (null
                       //     lookAtActor for the dispatch) at OnPileGrabPre + send this. HOST: validate
                       //     PILED && !HELD, execute playerGrabbed on puppet-N (probe-proven, see
                       //     votv-puppet-grab-feasibility-RE-2026-06-22), bump ctx, broadcast
                       //     PropConvert{kToClump} to ALL incl. the requester. CLIENT->HOST only; NOT
                       //     relayable; host gates senderPeerSlot!=0 + role==Host. Payload: GrabIntentPayload.
    ThrowIntent = 79,  // 2026-06-22 (v84, STAGED -- ID reserved, no handler yet): CLIENT->HOST throw of a
                       //     puppet-held clump. The existing flight-stream + ToPile re-pile convert already
                       //     own the throw end-to-end on the host, so this is wired only when the
                       //     client-initiated path (phase 2) needs an explicit release edge. Payload:
                       //     ThrowIntentPayload.
    PileResyncRequest = 80, // 2026-06-22 (v84, STAGED -- ID reserved, no handler yet): CLIENT->HOST drain-
                       //     survive (MTA EntityAdd-on-rescope). On a shadow-drain the client asks the host
                       //     to re-stream PropSpawn per live pile (host eid preserved). No body.
    PropSnapPos = 81,  // 2026-06-26 (v90, b3): HOST->ONE-JOINER position correction for a save-authoritative
                       //     chipPile the host MOVED during that joiner's connect window. The move's PropConvert
                       //     was dropped (the joiner's pre-world reliable gate), and chipPiles carry no position
                       //     in the connect-snapshot (both peers load them from the identical save; the bind maps
                       //     by ordinal), so the native stuck at the stale save pos. At the joiner's world-ready
                       //     the host position-compares each of that joiner's save-time pile positions vs the
                       //     pile's CURRENT actor pos + sends this for the diverged ones; the client arms it and
                       //     SetActorLocation's the bound native at the quiescence sweep (identity preserved,
                       //     position-only, idempotent). HOST->CLIENT only; NOT relayable; NOT pre-world-sendable
                       //     (sent AFTER the gate opens at ClientWorldReady). Payload: PropSnapPosPayload.
                       //     event_dispatch_entity dispatches it -> quiescence_drain::ArmPendingPosCorrection.
    SkinChange = 82,   // 2026-07-02 (v93): a player picked a new body SKIN in the F1 browser (docs/
                       //     COOP_CLIENT_MODEL.md; skins = converter paks in LogicMods/votv-coop/, name =
                       //     pak stem; "dr_kel" = the native body). Field-by-field payload:
                       //       [u8 slot][u8 namelen][name ASCII]
                       //     CLIENT->HOST: slot MUST equal senderPeerSlot (forgery guard). The host stores
                       //     it (player_handshake g_skinBySlot), re-skins that slot's puppet live, and
                       //     REBROADCASTS to every other ready client (originator excluded). HOST->ALL:
                       //     the host's own pick goes out with slot=0. Receivers validate the name
                       //     (IsValidSkinName -- it becomes a LoadObject package path component), store,
                       //     and re-skin the described slot's puppet if spawned. The at-join skin rides
                       //     the Join payload (after the guid field) + PlayerJoined (after the nick), so
                       //     late joiners get every peer's skin without extra wire. PRE-WORLD-SENDABLE
                       //     (IsPreWorldSendableKind): the receiver is engine-free without a puppet, and
                       //     gating it swallowed a change made during a joiner's load window (audit
                       //     2026-07-02). A peer missing the skin's pak falls back to the native kel
                       //     body (graceful; logged).
    NameplateChange = 83,  // 2026-07-02 (v94): a player toggled "show my nameplate" in F1 > Cosmetics.
                       //     Payload: [u8 slot][u8 visible(0/1)]. Same trust shape as SkinChange:
                       //     CLIENT->HOST slot MUST equal senderPeerSlot (forgery guard; a peer only
                       //     hides ITS OWN plate), host stores (coop::nameplate per-slot visibility)
                       //     and REBROADCASTS (originator excluded); HOST->ALL with slot=0 for the
                       //     host's own toggle. The at-join state rides the v94 prefs flags byte in
                       //     Join/PlayerJoined (bit0), so late joiners agree without extra wire (the
                       //     user's ask: no "ghost plate" for peers who joined after the toggle).
                       //     PRE-WORLD-SENDABLE: receiver is a plain flag store, engine-free -- the
                       //     SkinChange load-window lesson applied from birth. Slot resets to VISIBLE
                       //     on disconnect (a slot reuse must not inherit the departed peer's pref).
    EventFire = 84,    // 2026-07-03 (v95): HOST->ALL scheduled/story event fired -- clients replay
                       //     the native verb reflected, PER-ROW POLICY (coop/world/event_fire_sync;
                       //     the dupe matrix from votv-event-system-RE-2026-06-13.md section 10).
                       //     Payload: EventFirePayload (32 B) = [u8 dispatch: 0 runEvent / 1
                       //     runSpecialEvent][char name[31] ASCII NUL-bound]. No special field BY
                       //     DESIGN: the only native special is 'ariralPrank' (a host-local RNG
                       //     prank roll -- replaying it would roll a DIFFERENT prank per peer).
                       //     Host-only origin (senderPeerSlot must be 0); host receiving one drops.
    PyramidGather = 85, // 2026-07-04 (v97): HOST->ALL pyramid gather COMMIT (piramid mirror lane,
                       //     docs/events/piramid.md). The host pyramid's checkIfReached arrived at a
                       //     killerwisp and latched `gathering` (POST-observer edge); the client stages
                       //     wispTarget+isWalking on ITS mirrors and re-dispatches checkIfReached so the
                       //     game's OWN bytecode plays the whole choreography (montage/beams/timelines/
                       //     wisp freeze). Payload: PyramidGatherPayload (8 B) = [u32 pyramidEid (WA
                       //     lane id)][u32 wispEid (npc lane id)]. Host-only origin; host receiving
                       //     one drops (loopback). Not relayable, not pre-world-sendable. v98: ALSO
                       //     re-sent ToSlot at a joiner's world-ready edge when a gather is in flight
                       //     (piramid_sync::QueueConnectBroadcastForSlot -- the lane's late-join
                       //     answer for the ~10 s gather choreography; COOP_EVENT_JOIN.md 3.4).
    EventSnapshot = 86, // 2026-07-05 (v98): HOST->JOINER in-flight event snapshot -- join-during-event
                       //     Phase 1 (docs/COOP_EVENT_JOIN.md 3.2). Sent per ACTIVE entry of the native
                       //     activeEvents_senders registry at the joiner's world-ready edge (subsystems::
                       //     ConnectReplayForSlot -> event_active_sync::SendJoinSnapshotForSlot), one
                       //     message per entry (0-4 concurrent in practice; refcount registry).
                       //     Payload: EventSnapshotPayload (98 B). rowName carries the mapped
                       //     list_events row ('' = class not in the map yet -- receiver logs LOUD +
                       //     skips; the map fills per-event, Phase 2 completes the ~95 census).
                       //     Receiver replays ONLY replay-safe rows (event_fire_sync policy) with the
                       //     ACTIVE-OVERRIDE: an in-flight row bypasses the InClientPassEvents dedupe
                       //     (that dedupe is for COMPLETED history; the joiner's blob already carries
                       //     the row's passEvents entry -- the "killing blow" of COOP_EVENT_JOIN 2).
                       //     Host-only origin (senderPeerSlot must be 0); host receiving one drops.
                       //     Not relayable. Not pre-world-sendable BY CONSTRUCTION (deliberate
                       //     divergence from the 3.2 design sketch): it is only ever sent AT the
                       //     world-ready edge, when the slot's send gate is already open; the
                       //     receiver-side eventer race is absorbed by event_fire_sync's pending
                       //     drain, same as EventFire.
    AlarmState = 87,   // 2026-07-05 (v101): the base radar alarm active state -- a shared-world
                       //     toggle (docs/events/alarm.md). BOTH directions: host->all on any
                       //     observed transition of trigger_alarm_C.active (1 Hz poll -- runTrigger
                       //     is EX_VirtualFunction, PE-INVISIBLE, so the lane polls the state field
                       //     per the L2 device pattern) + host->joiner unconditionally at the
                       //     world-ready edge (the late-join answer); client->host when the CLIENT's
                       //     own radar scan / stop-press toggled its local trigger (host applies
                       //     natively, its poll broadcasts the canonical fanout). Apply = reflected
                       //     runTrigger(nullptr, active) -- the bytecode's own idempotency check
                       //     (IntToBool(index)==active -> no-op) makes redundant applies free and
                       //     breaks every echo loop. Payload: AlarmStatePayload (4 B).
    NickColorChange = 88,  // 2026-07-05 (v103, 12f): a player picked a nickname COLOR in F1 >
                       //     Cosmetics. Payload: [u8 slot][u8 has(0/1)][u8 r][u8 g][u8 b] (5 B;
                       //     has=0 -> reset to the surface defaults). Same trust shape as
                       //     NameplateChange: CLIENT->HOST slot MUST equal senderPeerSlot
                       //     (forgery guard; a peer only colors ITS OWN nick), host stores
                       //     (coop/player/nick_color -- the color axis' ONE owner) and
                       //     REBROADCASTS (originator excluded); HOST->ALL with slot=0 for the
                       //     host's own pick. The at-join state rides a [u8 has][r][g][b] field
                       //     appended to Join/PlayerJoined after the v94 prefs flags byte, so
                       //     late joiners agree without extra wire. Consumers: nameplate nick
                       //     (default white), chat nick prefix (default per-slot palette),
                       //     scoreboard row (default role gold/white -- role stays readable via
                       //     the Link column's "LAN/P2P HOST"). PRE-WORLD-SENDABLE: receiver is
                       //     a plain per-slot atomic store, engine-free pre-puppet (the
                       //     SkinChange load-window lesson). Slot resets to default on
                       //     disconnect (a slot reuse must not inherit the color).
    HandItem = 89,     // 2026-07-06 (v105): the hotbar HAND-ITEM display state -- what a
                       //     player's quick-slot hand currently shows. Payload:
                       //     [u8 slot][u8 has][u8 clsLen][cls ascii][u8 nameLen][name ascii].
                       //     Same trust shape as NickColorChange: CLIENT->HOST slot MUST
                       //     equal senderPeerSlot (forgery guard), host stores
                       //     (coop/player/hand_item) + REBROADCASTS originator-excluded;
                       //     HOST->ALL with slot=0 for the host's own hand. Receivers keep a
                       //     DISPLAY-ONLY mirror (physics/collision off, spawn/destroy echo
                       //     suppressed) attached to the puppet root at the native hold
                       //     offset -- the hand item is PLAYER EXPRESSION (MTA current-weapon
                       //     shape), never a world entity. Connect replay: host re-sends all
                       //     non-empty slots to a world-ready joiner. Empty-hand announces are
                       //     debounced ~250 ms (updateHold's destroy+respawn null flicker).
    PropDropIntent = 90, // 2026-07-09 (v106, F2 Inc-1): CLIENT->HOST -- a client PLACED a keyed
                       //     world prop it had picked up (hold-R place = simulateDrop -> a fresh
                       //     Aprop_C, host-authoritative-skipped at prop_lifecycle:210 -> the host
                       //     never learns of it). The client authors NO keyed spawn; it sends this
                       //     INTENT and the HOST spawns the authoritative Aprop by Key at the
                       //     transform + broadcasts via the existing FinishSpawn author lane (so
                       //     every peer, incl. the placer, adopts by Key -> no dup). Host-gated +
                       //     senderPeerSlot. Payload: PropDropIntentPayload. SAFETY: the client only
                       //     sends this for a key whose pickup-DESTROY it already propagated (so the
                       //     host destroyed its copy first) -> no host dup; the +1-tick send after
                       //     the place makes the in-hand husk-destroy reach the host FIRST (a no-op
                       //     against the already-gone rock) before the intent spawn. The GrabIntent
                       //     eid-lane (78) is the chipPile analog; a keyed prop stays ONE Aprop (no
                       //     morph) so it needs the durable Key + class, not just an eid. See
                       //     research/findings/join-identity/votv-keyed-prop-grabdrop-intent-lane-DESIGN-2026-07-09.md
                       //     + coop/props/prop_drop_intent.h.
    ServerState = 91,  // 2026-07-09 (v107): HOST->clients signal-SERVER simulation state (Inc-1). The
                       //     server break/fix sim (mainGamemode.{servers,brokenServers,serverEfficiency}
                       //     + per-serverBox.IsBroken) is NOT UE-replicated, so a client self-computes
                       //     diverged state + self-authors a FALSE "SERVER X is down" (email/console/
                       //     alarm). HOST-AUTHORITATIVE, ONE-directional: host polls its server state
                       //     (1 Hz, alarm_sync shape) -> broadcasts ServerStatePayload; the client
                       //     DRIVE-REALs it (raw-write serverBox.IsBroken + reflected check() to re-skin
                       //     -- verbs breakServer/fix are EX_LocalVirtualFunction-invisible, so we mirror
                       //     STATE not the verb) + neutralizes its own ticker_serverBreaker (disable
                       //     tick). Client never sends. Connect replay: current state to a world-ready
                       //     joiner. Inc-2 will forward the break EDGE for the true notice. See
                       //     research/findings/world-systems/votv-notifications-suppress-mirror-DESIGN-2026-07-09.md
                       //     + docs/notifications/ + coop/interactables/serverbox_sync.h.
    RoachState = 92,   // 2026-07-10 (v108): HOST->clients roach-infestation snapshot, PAGED (12
                       //     roaches per datagram; maxAmount=128 CDO). Roaches are COMPONENTS on
                       //     the one AcockroachMaster_C (not actors -- no npc/prop lane fits), and
                       //     the sim mutates SHARED props (food drain/destroy), so the host owns
                       //     it (client sim parked in spawn_authority). Client applies by ORDINAL:
                       //     count equal -> drive component loc/scale; count differs -> rebuild
                       //     via the game's own addRoach(bypassCheck)/deleteRoach. Host-sender-
                       //     checked; never client-sent. coop/creatures/roach_sync.
    RoachConsumed = 93, // 2026-07-10 (v108): CLIENT->HOST -- a roach was consumed LOCALLY on the
                       //     client (native eat/stomp event destroyed the component; those events
                       //     are not tick-driven, so they stay live on the parked master). Carries
                       //     the last known component location; the host deletes its nearest live
                       //     roach within the adjudication radius and the next RoachState converges
                       //     every peer. Not relayed.
    OwnerEntitySpawn = 94,   // 2026-07-10 (v108, OWNER-ENTITY tier -- user rule): a peer's OWN
                       //     stalker-entity spawned locally (eyer_C: each peer keeps its native
                       //     roll -- the entity targets ITS player -- but every peer must SEE it).
                       //     Key = (transport senderPeerSlot, seq); classId indexes the module's
                       //     class table. Relayed client->host->others; receivers materialize a
                       //     brain-parked, collision-off display mirror. Re-sent as a ~10 s
                       //     keepalive (receivers treat a known key as a pose refresh) -- that is
                       //     also the late-joiner delivery. coop/creatures/owner_entity_sync.
    OwnerEntityPose = 95,    // owner -> peers position/yaw drive for a live owner entity (sent on
                       //     movement at the module cadence; relayed like Spawn).
    OwnerEntityDestroy = 96, // owner's entity died locally (native despawn/self-destroy caught by
                       //     the owner death-watch) -- receivers destroy the mirror. Relayed.
    DeskInput = 97,    // 2026-07-16 (v112, BUGS-v111 fix): CLAIM-FREE field-granular desk INPUT
                       //     delta -- ONE input-class field (knob speed / filter toggle / polarity
                       //     dir / volume / select / maxLevel / unit power / coordIsPing / a
                       //     cooldown CHARGE) changed on the presser (detected by the 250 ms
                       //     per-field poll; desk verbs are EX_Local* PE-INVISIBLE, the poll is
                       //     the canonical detector). Presser -> host -> relayed to all EXCEPT
                       //     the originator (an echo would revert a newer local value -- the
                       //     eaten-scroll race). Receivers apply (patch + WriteScalars upd chain /
                       //     native setter side effects) + prime that field's poll baseline in
                       //     the same GT task (echo-proof). Replaces the v64 claimed/unclaimed
                       //     live DeskState lanes (RULE 2; DeskState=54 is ADOPT-only now).
                       //     Payload: DeskInputPayload. Relayed.
    DeskScanEvent = 98, // 2026-07-16 (v112): the SHIFT quick-scan happened on a peer (classified
                       //     by the poll: an upward cooldown jump to > maxCooldown/2 -- only the
                       //     scan charges to FULL coord_maxCooldown; dots charge to half; the
                       //     ENTER ping never touches cooldown). Mirrors replay the accepted-
                       //     branch EFFECTS only: reflected spawnDirs() + playPingSound(beepLong1)
                       //     (never useSearch() -- its cooldown gate would refuse on decay
                       //     jitter). The charge itself rides a DeskInput cooldown delta; the log
                       //     line rides DeskLogLine. Payload: DeskScanEventPayload. Relayed.
    DishArm = 99,      // v113 (2026-07-16, L4): the download-ARM state edge, HOST-authored --
                       //     initDownloadSignal(-1) rolls PER-PEER RNG polarity, so the host's roll
                       //     is THE roll (COOP_RNG_AUTHORITY T2-5d). Host detects via a 4 Hz raw
                       //     poll (DownloadMeshValid EDGE | DL signal-FName change | polarity
                       //     change; decoded excluded -- it accrues on the 10 Hz DeskSimPose
                       //     stream). armed=1: client pre-clears mirrored moving state, calls
                       //     reflected checkFordDishes() (the native display tail: camera aim +
                       //     objectRenderer.begin() + signalFound), then ArmDownloadFromSignal
                       //     (decoded, host polarity). armed=0 (disarm): ResetDownloadMachine +
                       //     reflected deleteSignalActor (native un-arm parity). Also the JOINER
                       //     arm delivery (a connect-replay row after the desk rows -- replaced
                       //     the v70 pending-adopt, RULE 2). ONE author for the ARM axis;
                       //     kind=0/1 SkySignalCatch remain the CATCH/DELETE verb replays.
                       //     Payload: DishArmPayload. NOT relayed (host-originated).
    DishSnapshot = 100, // v113 (L4): full-24 dish pose/state seed for a joiner (world-ready
                       //     connect replay, AFTER the desk rows; re-rides the re-seed) --
                       //     {yawZ, rollY, calibration, isMoving} x N + the activeDishes mask
                       //     (orientation is never saved; BeginPlay randrot diverges per peer).
                       //     Applied through the same ApplyDishRow as the DishPose stream (cue
                       //     edges fire for a mid-slew join). Payload: DishSnapshotPayload.
    DishCalib = 101,   // v113 (L4): symmetric per-dish CALIBRATION batch -- ANY peer whose local
                       //     values changed (1 Hz diff-poll, baseline-gated) broadcasts absolute
                       //     {index, value} pairs; host applies + relays (arrival order = the
                       //     total order); receivers apply + prime GT-atomically (echo-proof).
                       //     Covers all four native writers invariantly: host losePrec decay,
                       //     the ui_console calibrate terminal (any peer), tool_setDishCalibration,
                       //     the virusEvent scramble (initiating peer). Payload: DishCalibPayload.
                       //     Relayed.
    ReelSlot = 102,    // v114 (2026-07-17, L7): tape-caddy SLOT sentinel edge, PRESSER-authored --
                       //     both peers poll wallunit {reelBig @0x288, reelSmall @0x28C} at 4 Hz
                       //     for -1.0-sentinel transitions (ONE invariant detector for every native
                       //     writer: playerUsedOn insert, reelbox throw-in overlap, eject).
                       //     -1->P = INSERT{reel, progress}; P->-1 = EJECT{reel}. Apply GT-atomic
                       //     + PRIME (fields + poll baselines in one task; sender primes at send --
                       //     the poll echo is dead by construction). INSERT on an OCCUPIED slot:
                       //     the HOST keeps its own value + WARN (authority tiebreak; the ReelPose
                       //     corrector re-asserts <=1 s), a CLIENT writes-if-differs. The `active`
                       //     toggle carries NO event here -- it already rides the symmetric
                       //     ApplianceState lane (appliance.cpp wallunit row). Payload:
                       //     ReelSlotPayload. Relayed (presser may be a client).
    TaskNewState = 103, // v114 (L7): saveSlot.taskNew mirror, HOST-authored -- every live writer
                       //     is host-only (createNewTask: client daynightCycle frozen at
                       //     TimeScale=0; processTask: reachable only via setTaskNew + sell;
                       //     sell: client drone tick suppressed -- GUID census across all dumped
                       //     assets). ~1 Hz change-hash poll (fires a few times per game-day;
                       //     taskNew.reel_* is the task's best-SENT pair, not the accruing
                       //     wallunit pair). Client apply GT-atomic: scalars raw; the three
                       //     int32 arrays in-place when count==Num else EngineAlloc + copy +
                       //     EngineFree + {ptr,num,max} (ue_wrap/inventory.cpp precedent).
                       //     Payload: TaskNewStatePayload. NOT relayed (host-originated).
    ReelEjectIntent = 104, // v114 (L7): CLIENT->HOST -- "my native eject just birthed this reel
                       //     prop in my hands; author it". A client's fresh Aprop_C spawn never
                       //     broadcasts (prop_lifecycle client-skip), so the ejecting client mints
                       //     a synth key at the F2 FinishSpawn-drain seam, setKey's its local
                       //     reel, and sends PropDropIntentPayload {className, key, progress in
                       //     savedScalar, transform, physFlags|kSleep}; the host authors via
                       //     HostSpawnPlacedProp (dup-guard by key; Progress written from
                       //     savedScalar; born ASLEEP -- the client's held-prop pose stream takes
                       //     over <=100 ms) and its watcher broadcasts the PropSpawn. HOST-INGEST
                       //     ONLY (class-whitelisted to the reel classes -- NOT a general client
                       //     spawn door); NOT relayed. Lane::Bulk so a fast pocket-DESTROY(key)
                       //     cannot overtake the intent in-lane.
    DeskSndFx = 105,   // v115: presser-authored desk AUDIO EFFECT forward. The desk's one-shot
                       //     clicks/beeps and the two loops are played by presser-LOCAL BP paths
                       //     (OnKeyDown/OnKeyUp/screen buttons/verb internals -- all EX_Local*
                       //     or presser-only FSM), so observers heard nothing. The seam is the
                       //     NATIVE audio layer: Func-patch on AudioComponent:Play +
                       //     ActorComponent:SetActive/Activate (every whitelist-comp call site
                       //     measured EX_VirtualFunction on a NATIVE target -> dispatch funnels
                       //     through UFunction->Func regardless of caller opcode). Filter =
                       //     pointer compare against the 6 resolved unit-1 comps (kDeskSndComp*).
                       //     Ops: Play (cue name rides the payload -- SetSound ran before Play,
                       //     so the comp's Sound prop is the truth at hook time), LoopOn
                       //     (SetActive(true,true) -- ALL measured native ON sites use reset
                       //     semantics), LoopOff (SetActive(false,false); engine ignores bReset
                       //     on deactivate). Mirrors replay via reflected SetSound+Play /
                       //     SetActive under the wire-apply echo guard (our own replay dispatch
                       //     also funnels through ->Func -- the guard kills the echo).
                       //     Symmetric + RELAYED (claim-free desk doctrine: whoever really
                       //     pressed authors; host relays, origin excluded by construction).
                       //     Loop state is join-re-asserted by the HOST from component ground
                       //     truth (bIsActive) at the joiner's ready edge; leaver teardown is
                       //     HOST-OWNED (attribution map host-only; host broadcasts the OFF).
                       //     Payload: DeskSndFxPayload (44 B).
    LaptopState = 106, // v116 (laptop_sync): the stationary PC (Alaptop_C) power + floppy axes.
                       //     RE base: votv-laptop-pc-RE-2026-07-17.md; design qf rounds 7-9.
                       //     Presser-authored edges (all entry verbs are EX-invisible -> 1 Hz
                       //     poll; the THROWN-disc insert is authored at the PE-visible
                       //     BndEvt overlap PRE/POST pair, atomic with the native accept).
                       //     Ops: 0=power (isOpened edge; receivers replay the native
                       //     actionOptionIndex b8 under echo guard), 1=insert (slot scalars +
                       //     the thrown world-disc eid; the HOST destroys its authoritative
                       //     disc on apply -> ONE PropDestroy fan-out owns the twin deaths),
                       //     2=eject (receivers clear scalars only -- the presser's native
                       //     spawn crosses on the existing birth channels), 3=connect state,
                       //     4=content chunk (kind 0 = laptop slot content, kind 1 = disc
                       //     prop content keyed by eid -- the HOST is the content authority;
                       //     a client-ejected disc's content arrives client->host after the
                       //     adoption eid-binding, host loadDatas its actor + re-fans).
                       //     Symmetric + RELAYED (host applies + refans except origin).
                       //     Payload: LaptopStatePayload (216 B).
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
    // v82: MTA sync-time-context for the trash-entity carry stream. The holder stamps the host's
    // current per-eid ctx (trash_channel::CtxForEid) on every carry pose; the receiver drops a pose
    // whose ctx is OLDER than the eid's known generation -- so a clump-carry pose still in flight when
    // the entity re-piles (ToPile bumps ctx) can never re-drive the settled pile (the cluster-mis-bind
    // / stale-pose guard the morph lacked). 0 = no enforcement (a non-trash keyed prop -- ctx only
    // applies to eid-identified trash entities, which start at ctx>=1). [grows the snapshot 60->64]
    uint8_t  ctx;
    uint8_t  _pad[3];
};
static_assert(sizeof(PropPoseSnapshot) == 64, "PropPoseSnapshot must be 64 bytes (v82: +ctx)");

struct PropPosePacket {
    PacketHeader     header;  // 20
    PropPoseSnapshot pose;    // 64
};
static_assert(sizeof(PropPosePacket) == 84, "PropPosePacket must be 84 bytes (v82: pose +ctx)");

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
    uint8_t  stateBits;   // 1  -- bit0=inAir, bit1=hasLookAt, bit2=hasBodyYaw, bit3=hasKerfurState, bit4=kerfurSpooky
    uint8_t  kerfState;   // 1  -- v74: kerfur command (enum_kerfurCommand @ "State"); VALID iff bit3. Drives the
                          //      AnimBP state machine on the parked mirror (which can't run its own AI). 0 when bit3 clear.
    uint8_t  kerfFace;    // 1  -- v74: kerfur faceMaterialIndex (@ "faceMaterialIndex"); VALID iff bit3. The mirror's
                          //      face actor (kerfusFace) is otherwise frozen (its timer_face is neutralized at park).
    uint8_t  _pad;        // 1  -- 4-byte alignment
};
static_assert(sizeof(EntityPoseSnapshot) == 44, "EntityPoseSnapshot must be 44 bytes");

// EntityPoseSnapshot.stateBits flags. bit 0 reuses kStateBitInAir (same numeric/meaning as
// PoseSnapshot). bits 1-2 are EntityPose-specific (the PoseSnapshot 0x02 = kStateBitRagdoll does
// not apply to NPCs -- distinct struct, distinct meaning).
inline constexpr uint8_t kEntityPoseBitHasLookAt = 0x02;  // v39: lookAt{X,Y,Z} carries a valid head-look world target
inline constexpr uint8_t kEntityPoseBitHasBodyYaw = 0x04;  // v40: bodyYaw carries a valid visible-body world yaw
inline constexpr uint8_t kEntityPoseBitHasKerfurState = 0x08;  // v74: kerfState + kerfFace carry valid kerfur command/face
inline constexpr uint8_t kEntityPoseBitKerfurSpooky   = 0x10;  // v74: the kerfur is in its spooky/kill state (host-authoritative)

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

// v80 (B3b): ONE non-Character event WorldActor's pose in the WorldActorPose batch. Keyed by the
// WorldActor Element id (host range; same shared Registry id space as EntitySpawn -- a WA and an Npc
// never share an eid). Unlike EntityPoseSnapshot this is transform-ONLY with FULL rotation: a
// WorldActor (UFO/ship/saucer) banks + rolls, and it is a plain AActor (no CMC, no kerfur, no
// locomotion blend) so there is no speed/stateBits/lookAt/bodyYaw. The host reads GetActorLocation +
// GetActorRotation each send tick; the client interpolates pos (vector error) + each angle
// (shortest-arc) and SetActorLocation/SetActorRotation-drives the parked mirror (element::WorldActor).
struct WorldActorPoseSnapshot {
    uint32_t elementId;        // 4  -- WorldActor Element id (host range)
    float    x, y, z;          // 12 -- world cm (actor location = pivot)
    float    pitch, yaw, roll; // 12 -- actor world rotation deg (NormalizeAxis'd, FULL rotation)
    float    auxYaw;           // 4  -- v100: class-specific VISIBLE-heading yaw when it lives outside
                               //      the actor rotation. piramid2_C: the movementVector/Arrow
                               //      ArrowComponents' world yaw (the actor root NEVER yaws -- the
                               //      AnimBP orients the body off the component; deriving this from
                               //      position deltas was live-refuted 2026-07-05: the native
                               //      heading keeps easing toward the walk target for up to 10 s
                               //      AFTER motion stops, which no delta can see). Classes whose
                               //      facing IS the actor rotation stream auxYaw == yaw (client
                               //      consumers are class-specific; the generic drive ignores it).
    float    auxX, auxY, auxZ; // 12 -- v102: class-specific auxiliary TARGET vector. piramid2_C:
                               //      `relLook` -- the head/searchlight's idle look target (RELATIVE
                               //      frame), natively re-randomized at 1 Hz PER INSTANCE (the
                               //      changeLook timer), so each peer's head wandered its own way
                               //      (user live 2026-07-05: "фонарь и голова не на 100%"). The
                               //      mirror's changeLook is suppressed and its native lookat
                               //      VInterpTo eases toward THIS streamed value instead (target
                               //      mirrored, easing native). The relLook branch only STEERS
                               //      the mirror head while the host itself is on it -- during a
                               //      chase auxTargetEid (below) selects the native chase branch.
                               //      Other classes: zeros; the generic drive ignores it.
    uint32_t auxTargetEid;     // 4  -- v104: class-specific TARGET-IDENTITY eid (0 = none). piramid2_C:
                               //      the host's live `wispTarget` as its npc-lane eid, streamed EVERY
                               //      tick. Root of the 0y "голова и свет маленько рассинхронятся"
                               //      residue (user 2026-07-05 late): during the WALK-to-wisp phase the
                               //      host head/searchlight ease toward wispTarget's WORLD location
                               //      (tick chase branch, RE @3660-3937) while the mirror -- whose
                               //      wispTarget is nulled by design until the gather relay -- kept
                               //      following the relLook idle branch, i.e. RANDOM wander the host
                               //      itself was ignoring (changeLook keeps re-rolling during a chase).
                               //      The client resolves the eid via the npc mirror table and writes
                               //      the mirror's wispTarget (piramid_sync::ApplyMirrorWispTarget) so
                               //      the SAME native branch runs both ends; while the mirror is
                               //      `gathering` the field is owned by the gather choreography and
                               //      never touched. 0 clears (host's del nulls wispTarget -> mirrors
                               //      follow). Other classes: 0.
};
static_assert(sizeof(WorldActorPoseSnapshot) == 48, "WorldActorPoseSnapshot must be 48 bytes (v104: +auxTargetEid)");

// Max WorldActors per WorldActorPose datagram, MTU-capped: (1400 - PacketHeader(20) -
// EntityPoseBatchHeader(4)) / 48 = 28 (v104). The realistic event WA count is a handful (a few UFOs
// at once), so 28 keeps the datagram (20 + 4 + 28*48 = 1368) under the 1400 MTU budget.
// The batch reuses EntityPoseBatchHeader (a generic count+pad), NOT a byte-identical twin (RULE 2).
inline constexpr int kMaxWorldActorBatchEntries = 28;
inline constexpr int kWorldActorPoseDatagramMax =
    static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader)) +
    kMaxWorldActorBatchEntries * static_cast<int>(sizeof(WorldActorPoseSnapshot));

// v85 (Increment 2, the chipPile CLIENT-grab carry): ONE host-driven trash CLUMP's pose in the
// TrashCarryPose batch. HOST-AUTHORITATIVE and host-ORIGINATED (never relayed -- the relay skips
// the origin, so the grabbing client could not see its OWN clump via a client pose; constraint:
// remote_prop only drives slot 0 on a client, which the host's own carry already uses). Covers
// BOTH the kinematic hold (the host drives the clump to the puppet's hand) AND the physics flight
// after a throw, until the ToPile land commits. Keyed by the trash entity eid (host-minted, same
// id space as PropConvert). `ctx` is the current carry generation (the same trash_channel ctx byte
// as PropPoseSnapshot): the client drops a pose whose ctx != the eid's currently-adopted generation
// so a late carry packet can't re-drive a re-skinned/re-piled proxy (IsInboundStreamCtxFresh, the
// pile-jump/double-grab-cue guard). Transform == WorldActorPoseSnapshot (28 B) + ctx byte + pad = 32 B.
struct TrashClumpPoseSnapshot {
    uint32_t eid;              // 4  -- trash entity id (host-minted)
    float    x, y, z;          // 12 -- world cm
    float    pitch, yaw, roll; // 12 -- deg (NormalizeAxis'd by the sender)
    uint8_t  ctx;              // 1  -- carry-gen gate (same byte as PropPoseSnapshot.ctx)
    uint8_t  _pad[3];          // 3
};
static_assert(sizeof(TrashClumpPoseSnapshot) == 32, "TrashClumpPoseSnapshot must be 32 bytes");

// Max carried clumps per TrashCarryPose datagram. A player carries at most one trash clump, so the
// realistic simultaneous count is (kMaxPeers - 1) client grabs; 8 is ample headroom. Datagram
// 20 + 4 + 8*32 = 280 B, far under MTU. Reuses EntityPoseBatchHeader (generic count+pad; RULE 2).
inline constexpr int kMaxTrashCarryBatchEntries = 8;
inline constexpr int kTrashCarryPoseDatagramMax =
    static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader)) +
    kMaxTrashCarryBatchEntries * static_cast<int>(sizeof(TrashClumpPoseSnapshot));

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

// v109 (2026-07-11): the R-HOLD hand item's live view-relative transform
// (MsgType::HandPose). Same units/definitions as the reliable HandItem
// announce's trailing floats: relPos = cm along the owner's roll-0 view basis
// {fwd,right,up} from the head anchor (head bone + 33 cm); relRot = the item's
// rotator in that view frame (deg, canonical (-180,180]). Streamed at sendHz
// only WHILE holding; the receiver overwrites its SlotHand rel fields
// (hand_item.cpp) and DriveMirror re-composes per tick. Sender identity rides
// the PacketHeader senderSlot; host relays to the other clients (pose shape).
struct HandPoseSnapshot {
    float relPos[3];  // cm in the owner's view basis (fwd/right/up)
    float relRot[3];  // item rotator {pitch,yaw,roll} in the view frame (deg)
};
static_assert(sizeof(HandPoseSnapshot) == 24, "HandPoseSnapshot must be 24 bytes");

struct HandPosePacket {
    PacketHeader     header;  // 20
    HandPoseSnapshot pose;    // 24
};
static_assert(sizeof(HandPosePacket) == 44, "HandPosePacket must be 44 bytes");

// v109: the coords-panel live cursor as continuous motion (MsgType::DeskCursorPose=36).
// Sibling of HandPoseSnapshot: the reliable DishAimState carries the committed-coord
// identity, THIS unreliable stream carries the sweep (ui_coordinates.viewCoordinate,
// FVector2D screen-space). Streamed at sendHz WHILE the desk is claimed + the cursor
// moves; the mirror interpolates (LerpWindow) and writes it via WriteCursorOnly.
struct DeskCursorPoseSnapshot {
    float viewX, viewY;  // ui_coordinates.viewCoordinate (screen-space)
};
static_assert(sizeof(DeskCursorPoseSnapshot) == 8, "DeskCursorPoseSnapshot must be 8 bytes");

struct DeskCursorPosePacket {
    PacketHeader           header;  // 20
    DeskCursorPoseSnapshot pose;    // 8
};
static_assert(sizeof(DeskCursorPosePacket) == 28, "DeskCursorPosePacket must be 28 bytes");

// v66 voice chat: one 20 ms opus frame (MsgType::VoiceFrame). The DATAGRAM
// carries only 8 + opusLen bytes of this struct (the array is max-sized for
// the receive copy). seq is the per-sender VOICE sequence (monotonic per
// session, increments per frame INCLUDING the stop marker) -- the jitter
// buffer orders/PLCs by it. A stop marker (bit1, opusLen=0) ends a talk
// burst: the receiver flushes its jitter buffer and resets its decoder, the
// sender resets its encoder (SVC's empty-frame contract). Whisper (bit0)
// halves the receiver-side attenuation radius. The SPEAKER's identity rides
// the PacketHeader senderSlot (host relay rewrites it -- the pose precedent),
// not the payload.
inline constexpr int kVoiceMaxOpusBytes = 200;  // encoder hard cap (48 kbps VOIP ~ 120 B typical)
inline constexpr uint8_t kVoiceFlagWhisper = 0x01;
inline constexpr uint8_t kVoiceFlagStop    = 0x02;
struct VoiceFramePayload {
    uint8_t  flags;     // kVoiceFlag*
    uint8_t  _pad;
    uint16_t opusLen;   // 0 for the stop marker
    uint32_t seq;       // per-sender voice seq (NOT the header seq)
    uint8_t  opus[kVoiceMaxOpusBytes];
};
inline constexpr int kVoiceFrameHeadBytes = 8;  // payload bytes before opus[]
struct VoiceFramePacket {
    PacketHeader      header;  // 20
    VoiceFramePayload body;    // 8 + opusLen on the wire
};
static_assert(sizeof(VoiceFramePayload) == kVoiceFrameHeadBytes + kVoiceMaxOpusBytes,
              "VoiceFramePayload layout drifted");
static_assert(sizeof(VoiceFramePacket) == 228,  // 20+8+200; kMaxPacketBytes (256) declared below
              "VoiceFramePacket must fit one datagram");

// v66: voice presence for the icon surfaces (ReliableKind::VoiceState).
struct VoiceStatePayload {
    uint8_t micMuted;       // 1 = the peer muted its mic
    uint8_t voiceDisabled;  // 1 = the peer turned the voice module off entirely
    uint8_t _pad[2];
};
static_assert(sizeof(VoiceStatePayload) == 4, "VoiceStatePayload must be 4 bytes");

// v68: wall-attachable stick state (PropStickState). key = the prop's wire key
// (the canonical Aprop_C resolve); elementId rides along for the eid fallback
// (0 = sender had no Element). flags carries WHICH field the BP set (bit0
// frozen -- the camera family; bit1 static -- pryingRequired attachables); the
// receiver's SP-replay (forceStick) re-derives it natively and the flags only
// steer the raw-write fallback + logging. pose = the sender's COMMIT-time actor
// transform -- where the stick trace succeeded (the receiver pre-poses there so
// the replay's re-trace finds the same surface; its own glide settles it).
struct PropStickStatePayload {
    WireKey  key;
    uint32_t elementId;
    uint8_t  flags;      // bit0 = frozen, bit1 = static
    uint8_t  _pad[3];    // zeroed
    float locX, locY, locZ;
    float rotPitch, rotYaw, rotRoll;
};
static_assert(sizeof(PropStickStatePayload) == 64, "PropStickStatePayload must be 64 bytes");

// v67: client->host kerfur on/off conversion request (KerfurConvertRequest).
// elementId routes by the entity the client's menu targeted: an Npc Element id
// when toProp=1 (turn_off on a live kerfur mirror), a Prop Element id when
// toProp=0 (turn on, on a kerfur-prop mirror). The host resolves the element,
// validates the bound actor (live + kerfurOmega_C- / prop_kerfurOmega_C-derived)
// and runs the BP verb; there is deliberately NO other state in the payload --
// the conversion's outcome (which prop class, floppy, transform) is the HOST
// BP's own business and mirrors through the entity pipelines.
struct KerfurConvertPayload {
    uint32_t elementId;  // the dying form's host-range MIRROR eid (the client is eid-based -- redesign
                         //      11). The host resolves BOTH the actor (MirrorManager::Get) and the
                         //      stable KerfurId (kerfur_entity::GetKerfurIdForEid) from it, runs the
                         //      verb, and the result rides KerfurConvert (the host->all broadcast).
    uint8_t  toProp;     // 1 = NPC -> prop (turn_off); 0 = prop -> NPC (turn on)
    uint8_t  _pad[3];    // zeroed
};
static_assert(sizeof(KerfurConvertPayload) == 8, "KerfurConvertPayload must be 8 bytes");

// v78: HOST->ALL kerfur form-transition broadcast (KerfurConvert=74) -- the SOLE conversion signal
// (redesign 10.3). The host runs the BP verb (its own radial menu OR a client KerfurConvertRequest),
// registers the new-form actor via the normal Npc/Prop pipeline (host-range newEid), then broadcasts
// this. Every client destroys its old-form mirror (resolved kerfurId->oldEid) and ADOPTS its own
// claimed local conversion ghost to the authoritative newEid (initiator) or materializes a fresh
// mirror (others). On a sentient/kill refusal the host sends rejected=1 (no transform/class) so the
// initiator restores its mirror (fixes Failure #7).
struct KerfurConvertBroadcastPayload {
    uint32_t      kerfurId;      // 4  -- the stable host-allocated KerfurId (spans both forms; for logs/correlation)
    uint32_t      oldEid;        // 4  -- the OLD-form wire eid the client destroys (its current mirror)
    uint32_t      newEid;        // 4  -- the new-form's host-range wire eid (the Npc/Prop mirror id to install)
    uint8_t       toForm;        // 1  -- 0 = NPC (prop->NPC turn on), 1 = prop (NPC->prop turn_off)
    uint8_t       rejected;      // 1  -- 1 = host refused (sentient/kill); 0 = success
    uint8_t       _pad[2];       // 2
    float         locX, locY, locZ;            // 12 -- new-form actor world location
    float         rotPitch, rotYaw, rotRoll;   // 12 -- new-form actor rotation
    WireClassName newClassName;  // 64 -- the new-form class (e.g. "prop_kerfurOmega_C"); empty on reject
};
static_assert(sizeof(KerfurConvertBroadcastPayload) == 104, "KerfurConvertBroadcastPayload must be 104 bytes");
static_assert(sizeof(KerfurConvertBroadcastPayload) <= 256 - 20 - 8,
              "KerfurConvertBroadcastPayload must fit in one reliable datagram");

// (KerfurHoldPayload removed in K-5 with ReliableKind slot 75 -- the held-kerfur carry needs no new
// packet; see the ReliableKind enum note. The client streams the kerfur prop mirror's host-range eid
// in its ordinary PropPose.)

// v74: host-authoritative kerfur menu command (KerfurCommand=70). CLIENT->HOST. The HOST
// derives the REQUESTER from senderPeerSlot (so Follow follows the clicking player); the host's
// OWN menu use never hits the wire (executed locally in kerfur_command::Tick with requester=host).
// command = KerfurMenuCommand enum (coop/kerfur_command.h). Deliberately no requesterSlot field --
// the wire layer already attributes the sender, and a self-declared slot would be spoofable.
struct KerfurCommandPayload {
    uint32_t elementId;  // host Npc eid of the target kerfur
    uint8_t  command;    // KerfurMenuCommand (follow/idle/patrol/fix_servers/get_reports/fix_transformers)
    uint8_t  _pad[3];    // zeroed
};
static_assert(sizeof(KerfurCommandPayload) == 8, "KerfurCommandPayload must be 8 bytes");

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
    // v82: the trash-entity eid (0 = a keyed Aprop release, resolved by `key` as before). A keyless
    // clump streams key=None, so the THROW must be routed by eid -- without this the receiver matched
    // the release to whatever clump the sender's drive slot happened to carry (a late E1 throw landing
    // on E2 after a re-grab). The receiver requires the slot's driven eid == this, else resolves the
    // live actor by eid; identity is the eid end-to-end (docs/piles/08). [+4 bytes]
    uint32_t elementId;
    // v82: MTA sync-time-context for the trash-entity THROW edge (HELD -> FLYING). Same semantics as
    // PropPoseSnapshot.ctx: the host stamps its current per-eid ctx; the receiver DROPS a release whose
    // ctx is older than the eid's known generation (a throw delayed past a re-pile/re-grab can't re-apply
    // velocity to the re-skinned entity). 0 = no enforcement (a non-trash keyed prop release). [+4 bytes]
    uint8_t ctx;
    uint8_t _pad[3];
};
static_assert(sizeof(PropReleasePayload) == 64, "PropReleasePayload must be 64 bytes (v82: +elementId +ctx)");
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
    // v54 SP-parity identity (white-cube root cause, RE 2026-06-10): an
    // Aprop_C's rendered mesh/mass/collision are NOT class state -- init()
    // resolves them from the list_props DataTable row named by the FName
    // `Name`@0x0258 (CDO default row = 'cube'). VOTV's own save loader
    // (mainGamemode_C::loadObjects -> Aprop_C::loadData) restores Name +
    // scale + Static/removeWOrespawn/frozen/sleep then re-runs init(); a
    // mirror spawn must carry the same identity or it constructs as the
    // literal cube row (the host's broken-cubicle 'cubicleP_*' wall panels
    // mirrored as white cubes). len=0 for non-Aprop_C classes (chipPile/
    // clump/trashBits have no Name row). Same 32-byte short-string carrier
    // shape as WireKey.
    WireKey       propName;        // 32 -- Aprop_C list_props row FName
    float         locX, locY, locZ;            // 12 -- world cm
    float         rotPitch, rotYaw, rotRoll;   // 12 -- FRotator (matches PropPose shape)
    float         scaleX, scaleY, scaleZ;      // 12 -- sender's real GetActorScale3D (v54;
                                  //     pre-v54 senders hardcoded (1,1,1) -- scale is part
                                  //     of the saved transform SP restores, see propName)
    uint8_t       physFlags;        // 1
    uint8_t       chipType;         // 1 (v28: trash-clump variant selector enum_chipPileType; 0 for non-trash props)
    uint8_t       hasMatchPos;      // 1 (v86 Path 1c: 1 => matchX/Y/Z carry this pile's SAVE-TIME position)
    uint8_t       _pad;             // 1 (v16: senderContext byte removed; v28: chipType; v86: hasMatchPos)
    float         initLinVelX, initLinVelY, initLinVelZ;  // 12 -- throw velocity (cm/s); usually
                                  //     (0,0,0). (v29's kFreshLanded landing-velocity use was
                                  //     retired v52 with turnToPile.)
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
    // v86 Path 1c -- the join-window two-channel pile DUP fix. For a chipPile expressed in the JOIN
    // connect-replay snapshot, the host stamps the pile's position AT SCRATCH-SAVE time (the frozen
    // value BOTH peers loaded from the same transferred save -- save_transfer's g_blobPileXforms map,
    // keyed by the host eid which survives a grab/re-pile convert). The client's join twin-destroy
    // matches its save-loaded NATIVE against THIS (not the current render pose `loc`): a pile the host
    // MOVED in the join-load window renders @new but its save-time key is @old == where the client's
    // native loaded, so the native is reconciled (destroyed, proxy = sole mirror). Pre-1c the match was
    // the proxy's current pose, which goes >1cm-blind on a moved pile = the two distinct objects = DUP.
    // Valid IFF hasMatchPos (only the join snapshot stamps it; a mid-game/incremental spawn leaves it 0,
    // and the receiver falls back to `loc`). See votv-pile-dup-join-window-two-channel-RE-2026-06-23.
    float         matchX, matchY, matchZ;   // 12 -- save-time position (world cm); valid iff hasMatchPos
    // v114 (L7): per-prop SAVE-SCALAR identity-at-birth channel (the reel's Progress @0x364 IS the
    // native save's struct_save.mFloat[0]; PropSpawn already carries the rest of the loadData set --
    // Name/scale/physFlags -- so a scalar-bearing class's mirror must get its scalar AT BIRTH too,
    // or a 3rd peer inserting a progress-less mirror broadcasts a wrong value into everyone's unit).
    // Filled by the ONE shared reader ue_wrap::prop::ReadSavedScalarForClass at BOTH fill sites
    // (host live express + prop_snapshot builder); applied post-Finish at mirror birth (measured:
    // prop_reel's Progress consumers are lookAt + loadData only). Valid iff physFlags bit
    // propspawn_flags::kHasSavedScalar (the formerly-free 0x40). Occupies the old _pad2 slot --
    // size stays 212.
    float         savedScalar;      // 4 -- per-class save scalar (reels: Progress); valid iff kHasSavedScalar
};
static_assert(sizeof(PropSpawnPayload) == 212, "PropSpawnPayload must be 212 bytes (v86: +matchX/Y/Z save-time key)");
static_assert(sizeof(PropSpawnPayload) <= 256 - 20 - 8,
              "PropSpawnPayload must fit in one reliable datagram");

namespace propspawn_flags {
inline constexpr uint8_t kSimulatePhysics = 0x01;
inline constexpr uint8_t kIsHeavy         = 0x02;
inline constexpr uint8_t kFrozen          = 0x04;
// v54 SP-parity flag bits: the remaining Aprop_C bools that SP's loadData
// restores before re-running init() (init computes SetSimulatePhysics =
// !(Static||frozen||sleep) and collision type from Static||heavy). The
// receiver raw-writes these on the deferred-spawned mirror BEFORE
// FinishSpawningActor so init()'s single UCS pass resolves the true
// physics/collision state -- no post-Finish correction window.
// (0x08 history: kFreshLanded retired v52 -- it dispatched turnToPile, the
// pile->clump grab morph, on the landed-pile mirror = the clump DUPE; then
// kAtRest, the v53 teleport-then-PutRigidBodyToSleep experiment, reverted
// 2026-06-09 for the host-authoritative kinematic mirror and never read
// since -- constant deleted per RULE 2, bit reused.)
inline constexpr uint8_t kStatic          = 0x08;  // Aprop_C.Static @0x02D8
inline constexpr uint8_t kSleep           = 0x10;  // Aprop_C.sleep  @0x02DD
inline constexpr uint8_t kRemoveWOrespawn = 0x20;  // Aprop_C.removeWOrespawn @0x02D9
// (0x40 was kFlagKerfur in K-4a; REMOVED in K-5 -- a client identifies a kerfur prop mirror by CLASS
//  (kerfur_entity::IsKerfurClass), not a PropSpawn flag, so no bit is needed.)
inline constexpr uint8_t kHasSavedScalar  = 0x40;  // v114 (L7): PropSpawnPayload.savedScalar /
                                                   // PropDropIntentPayload.savedScalar is valid
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

// v106 (2026-07-09, F2 Inc-1): CLIENT->HOST intent -- "I placed this keyed world prop I had
// picked up". Carries the SP-parity identity the host needs to re-spawn the authoritative Aprop
// (className + Key + list_props `propName` -> avoids the v54 white-cube) + the placement transform
// + scale + the physFlags parity bits (WriteSpParityIdentity). NO elementId (the host allocates its
// own host-range eid when its FinishSpawn watcher expresses the spawn) and NO velocity (a placed
// prop is at rest). Field layout mirrors PropSpawnPayload's identity core so the host feeds the same
// spawn-by-key path. Fits one datagram (168 << 256-20-8). See coop/props/prop_drop_intent.h.
struct PropDropIntentPayload {
    WireClassName className;                    // 64 -- "prop_rock_C" etc. (R::ClassNameOf of the placed actor)
    WireKey       key;                          // 32 -- the persistent cross-peer save Key (loadData-restored)
    WireKey       propName;                     // 32 -- Aprop_C list_props row FName (white-cube parity)
    float         locX, locY, locZ;             // 12 -- placement world cm
    float         rotPitch, rotYaw, rotRoll;    // 12 -- placement FRotator
    float         scaleX, scaleY, scaleZ;       // 12 -- placed actor's GetActorScale3D
    uint8_t       physFlags;                    // 1  -- propspawn_flags (kStatic/kFrozen/kSleep/kRemoveWOrespawn parity)
    uint8_t       _pad[3];                      // 3  -- 4-byte alignment; zero on the wire
    // v114 (L7): the save-scalar birth channel (see PropSpawnPayload.savedScalar). Used by
    // ReliableKind::ReelEjectIntent (the reel's Progress); a PropDropIntent sender leaves it 0 with
    // the kHasSavedScalar flag clear. 168 -> 172 B (proto 114 hard-closes mixed pairs).
    float         savedScalar;                  // 4 -- valid iff physFlags & kHasSavedScalar
};
static_assert(sizeof(PropDropIntentPayload) == 172, "PropDropIntentPayload must be 172 bytes (v114: +savedScalar)");
static_assert(sizeof(PropDropIntentPayload) <= 256 - 20 - 8, "PropDropIntentPayload must fit one datagram");

// --- v56 save-transfer join bootstrap ------------------------------------------
// Chunk DATA bytes per SaveTransferChunk message (+4 B index prefix). Far above
// kMaxReliablePayload by design (session diverts the kind to the bulk sink; GNS
// fragments the message internally; uint16 ReliableHeader.payloadLen caps the
// whole payload at 65535 -> 56K data + 4 index fits with headroom). 17 MB save
// = ~308 messages on Lane::Bulk.
inline constexpr uint32_t kSaveChunkBytes = 56u * 1024u;

// SaveTransferBegin: the blob header. totalBytes==0 == "host has no save file"
// (fresh-hosted world whose slot never wrote, or a persistent read failure) ->
// the client falls back to the fresh-world boot instead of waiting forever.
struct SaveTransferBeginPayload {
    uint32_t totalBytes;   // whole STREAMED blob size = sidecarBytes + .sav size (0 = no save available)
    uint32_t chunkCount;   // ceil(totalBytes / kSaveChunkBytes)
    uint32_t crc32;        // CRC-32 of the whole streamed blob (sidecar + .sav; client verifies pre-write)
    uint8_t  gameMode;     // host's enum_gamemode ordinal (story=0) -- the zcoop_
                           // slot prefix can't prefix-match a mode, so the client
                           // threads this into LoadStorySave(forceGameMode)
    uint8_t  pad[3] = {};  // (was liveCaptured; retired 2026-06-15 -- the client now
                           // runs the divergence/ghost-sweep reconcile for EVERY join
                           // incl. live-capture, gated on NPC-aware load-tail quiescence
                           // + the >50% safety valve, so no per-join skip flag is needed)
    uint32_t sidecarBytes; // Phase 2 (stable-id identity sidecar): the count of LEADING bytes of the
                           // streamed blob that are the framed {objectsData-index -> host-eid} identity
                           // map for the keyless save-loaded natives (prepended to the .sav so the map
                           // travels ATOMICALLY inside this one CRC'd stream -- it cannot desync from the
                           // blob it indexes). 0 = no sidecar (shipping default / stale-fallback join).
                           // The client strips these bytes before writing the .sav, then (dev checkpoint)
                           // parses + logs the map. coop/save_identity_map.h owns the framing.
};
static_assert(sizeof(SaveTransferBeginPayload) == 20, "SaveTransferBeginPayload must be 20 bytes");
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
// research/findings/computers-devices/votv-doors-and-lightswitches-RE-2026-05-25.md.
struct KeyedTogglePayload {
    WireKey  key;        // 32 -- the instance's Key FName (string)
    uint8_t  action;     // 1  -- 0 = closed/off, 1 = open/on (the state AFTER the edge)
    uint8_t  _pad[7];    // 7  -- 8-byte alignment / reserved
};
static_assert(sizeof(KeyedTogglePayload) == 40, "KeyedTogglePayload must be 40 bytes");
static_assert(sizeof(KeyedTogglePayload) <= 256 - 20 - 8,
              "KeyedTogglePayload must fit in one reliable datagram");

// v63: enterable-device occupancy claim/release (ReliableKind::DeviceClaim).
// `key` = the shared-widget claim identity (see the kind doc); `slot` = the
// HOLDING peer slot (on a host broadcast / arbitration reply this is the
// authoritative holder -- the WINNER, not necessarily the sender); `busy` =
// 1 claim / 0 release. A losing claimant recognizes its loss by busy=1 with
// slot != its own while it is still inside that device.
struct DeviceClaimPayload {
    WireKey  key;        // 32 -- the device claim key
    uint8_t  slot;       // 1  -- holding peer slot
    uint8_t  busy;       // 1  -- 1 = claimed, 0 = released
    uint8_t  _pad[2];    // 2  -- alignment / reserved
};
static_assert(sizeof(DeviceClaimPayload) == 36, "DeviceClaimPayload must be 36 bytes");
static_assert(sizeof(DeviceClaimPayload) <= 256 - 20 - 8,
              "DeviceClaimPayload must fit in one reliable datagram");

// v64: one sky signal on the wire (Fstruct_signal_spawn 0x2C with the FName
// objectName replaced by its string -- FName indices are not cross-process
// stable). The POD head is byte-copied from/to the live row; objectName is
// re-resolved to an FName on the receiver. The widget tail carries the paired
// ui_signal's rolled/ticking state: `alpha` is THE expiry countdown (1 -> 0;
// reduceLifetime subtracts dt/lifetime and self-deletes at <= 0 -- LifeTime
// alone does NOT drive expiry), and `direction` is GAMEPLAY-LOAD-BEARING
// (the desk gates catch success on gatherSignal dir == the panel's direction
// toggle; a per-peer re-roll would diverge the minigame). Phase-2 impl RE.
struct WireSkySignal {
    float   x, y, z;          // 12 -- coordinates (FVector; also the cross-peer identity)
    int32_t type;             // 4
    float   strength;         // 4
    float   frequency;        // 4  -- identity tiebreaker (wire-copied exact)
    float   frequencySpread;  // 4
    float   polarity;         // 4
    float   polaritySpread;   // 4
    float   alpha;            // 4 -- widget Alpha: the 1->0 expiry countdown
    float   lifeTime;         // 4 -- widget LifeTime: the countdown divisor
    float   maxLifetime;      // 4 -- widget MaxLifetime (rolled 120-240)
    uint8_t direction;        // 1 -- widget Direction (catch-gate parity)
    uint8_t nameLen;          // 1
    char    objectName[14];   // 14 -- rolled names are short ("sat1"-class); truncation logged
};
static_assert(sizeof(WireSkySignal) == 64, "WireSkySignal must be 64 bytes");

// v64: the host-authoritative sky-signal SET snapshot (ReliableKind::SkySignalState),
// split into parts of <=3 rows. `gen` increments per snapshot so a receiver never
// mixes parts of two different snapshots (drop-and-wait-for-next on mismatch --
// snapshots are state-complete, so losing one only delays convergence one change).
struct SkySignalStatePayload {
    uint8_t gen;       // snapshot generation (wraps; equality-checked only)
    uint8_t part;      // 0-based part index
    uint8_t parts;     // total parts in this snapshot (>=1)
    uint8_t count;     // rows in THIS part (<=3)
    uint8_t totalCount;// rows in the whole snapshot (receiver sanity/log)
    uint8_t _pad[3];
    WireSkySignal rows[3];
};
static_assert(sizeof(SkySignalStatePayload) == 200, "SkySignalStatePayload must be 200 bytes");
static_assert(sizeof(SkySignalStatePayload) <= 256 - 20 - 8,
              "SkySignalStatePayload must fit in one reliable datagram");

// v70: the signal-catch CONSUME REPLAY (ReliableKind::SkySignalCatch; replaces
// the v64 20 B row-delete-only relay -- RULE 2). `row` is filled by the CATCHER
// from its own coord_signalData (the authoritative post-catch struct, @79303) --
// identity stays the exact wire-copied coordinates + frequency; alpha/lifetimes
// ride along zeroed (the row is being deleted; receivers ignore them). `slew` is
// the exact startMovingTo argument, read as (dish.lookAt - dish.ActorLocation)
// off any just-started dish -- the BP computes ONE vector for all dishes, and
// startMovingTo re-adds each receiver dish's own ActorLocation, so one relative
// vector replays exactly everywhere. v116: the sender reads it from a MOVING dish
// preferred, else the first live dish's lookAt (the catch chain wrote ALL dishes
// absolute at the catch moment, so a settled array still holds the fresh target);
// slewValid=0 survives only for a zero-dish machine (dead-guard WARN on apply).
// kind: 0 = catch (v116: UNGATED -- the unprimed local change-edge is the
// authority, the v63 holder validation is retired; host dedups via recent-TTL),
// 1 = cleared (the 'Signal data deleted' button -- unclaimed trust; row + slew
// ignored, receivers replay the @33832 reset chain), 2 = connect STATE-SEED
// (host-authored only; applied exactly like 0 but NEVER announced to the
// activity feed -- the joiner adopts state, it does not witness an action).
struct SkySignalCatchPayload {
    WireSkySignal row;          // 64 -- the caught signal's full row content
    float   slewX, slewY, slewZ;// 12 -- the startMovingTo relative vector
    uint8_t kind;               // 1  -- 0 = catch, 1 = cleared, 2 = connect seed (v116)
    uint8_t slewValid;          // 1  -- 0 = no dish was moving; arm directly
    uint8_t _pad[2];            // 2
};
static_assert(sizeof(SkySignalCatchPayload) == 80, "SkySignalCatchPayload must be 80 bytes");

// v116 (laptop_sync): the stationary PC's power + floppy axes (ReliableKind::
// LaptopState). op: 0=power edge {isOpened}, 1=insert edge {slot scalars +
// thrown-disc eid (0=held)}, 2=eject edge (receivers clear scalars only),
// 3=connect state {isOpened + slot scalars}, 4=content chunk (contentKind
// 0 = laptop slot content, target the laptop; 1 = disc prop content, target
// eid). Content serialization: UTF-8, fields joined by 0x1F; chunks assemble
// per (sender, kind, eid), total cap 4 KB (truncate+WARN beyond -- OPEN-9
// residual). In-lane ordering guarantees scalars-before-chunks.
struct LaptopStatePayload {
    uint8_t  op;          // 0=power, 1=insert, 2=eject, 3=state, 4=content chunk
    uint8_t  isOpened;    // op 0/3
    uint8_t  zip;         // op 1/3
    uint8_t  slot;        // op 1: 0=floppy hitbox, 1=zip hitbox
    int32_t  floppyType;  // op 1/3 (-1 = empty)
    int32_t  readWrites;  // op 1/3
    uint32_t eid;         // op 1: thrown world-disc eid (0=held); op 4 kind 1: target disc eid
    uint16_t chunkSeq;    // op 4: 0..chunkTotal-1
    uint16_t chunkTotal;  // op 4
    uint16_t contentLen;  // op 4: bytes used in content[]
    uint8_t  contentKind; // op 4: 0 = laptop slot content, 1 = disc content
    uint8_t  _pad;
    char     content[192];
};
static_assert(sizeof(LaptopStatePayload) == 216, "LaptopStatePayload must be 216 bytes");
static_assert(sizeof(LaptopStatePayload) <= 228, "LaptopStatePayload must fit the inline reliable buffer");

// v64 (reshaped v70, v113): the desk's live-visible scalars (ReliableKind::DeskState).
// Field set = the RE doc SS4.1 live actor fields; receivers write raw + run the
// desk's own upd* refresh chain. `adopt` = 1 on the host connect snapshot.
// v70 removals (RULE 2): the coordLog[96] tail + len moved to the lossless
// DeskLogLine event-line channel; canDL is DERIVED (canSaveSignal recomputes it
// per detector pulse) and never belonged on the wire.
// v113 removal (RULE 2): the v70 ADOPT-ONLY dlDecoded/dlPolarity fields are gone --
// the pending-adopt mechanism is RETIRED; the joiner's arm (with host polarity)
// rides a DishArm=99 connect-replay row, and decoded/resDetect ride the standing
// 10 Hz DeskSimPose stream (L4 design doc).
struct DeskStatePayload {
    float   dlPoFilterOffset;   // 4
    float   dlFrFilterOffset;   // 4
    float   dlPoFilterSpeed;    // 4
    float   dlFrFilterSpeed;    // 4
    float   dlDownloading;      // 4 -- float in the BP (0 = idle)
    float   dlResDetecPercent;  // 4 -- the live detection-needle percent
    float   coordCooldown;      // 4
    int32_t playVolume;         // 4 -- int32 in the BP (header-verified)
    int32_t dlPolarityDir;      // 4
    int32_t compMaxLevel;       // 4 -- claim-owner edit; the comp DECODE stream is CompState (v65)
    int32_t playSelectIndex;    // 4
    uint8_t dlActiveFrFilter;   // 1
    uint8_t dlActivePoFilter;   // 1
    uint8_t activePlay;         // 1
    uint8_t activeDownload;     // 1
    uint8_t activeCoords;       // 1
    uint8_t activeComp;         // 1
    uint8_t coordIsPing;        // 1 -- v115b: DIAGNOSTIC only; receivers never adopt it
                                //    (the ping FSM's run-flag -- adopting TRUE wakes a
                                //    phantom sim on the joiner; the 2026-07-17 root)
    uint8_t adopt;              // 1
};
static_assert(sizeof(DeskStatePayload) == 52, "DeskStatePayload must be 52 bytes (v113)");
static_assert(sizeof(DeskStatePayload) <= 256 - 20 - 8,
              "DeskStatePayload must fit in one reliable datagram");

// v70: one coords-terminal EVENT line (ReliableKind::DeskLogLine) -- the 9
// one-shot writeToCoordLog_2 lines ('Successful ping...', the Err2-Err6 gate
// errors, '<r>Cooldown error</>', quick-scan, 'Signal data deleted'). Produced
// by the peer whose local action wrote the line (exact producer-side diff of
// coord_coordLog2Text; ANIMATED bar/status lines are filtered by prefix -- they
// self-generate on every peer from mirrored scalars). Host-relayed. Receivers
// append via writeToCoordLog_2 (native CRLF + repaint + scroll + 1000-cap) and
// advance their own producer baseline past the applied text (echo-proof). ASCII
// (the BP lines, including the <r>/<c> rich-text markup, are ASCII).
struct DeskLogLinePayload {
    uint8_t len;        // 1 -- used bytes in line[]
    uint8_t _pad[3];    // 3
    char    line[120];  // 120 -- one event line WITHOUT the trailing CRLF
};
static_assert(sizeof(DeskLogLinePayload) == 124, "DeskLogLinePayload must be 124 bytes");

// v71: the sleep gate (ReliableKind::SleepState). op semantics:
//   0 Report     (peer -> host)  flag = inBed (the sender's isSleep edge)
//   1 Tally      (host -> all)   count/total for the "N/M sleeping" feed line
//   2 Accelerate (host -> all)   everyone is in bed -- start the 20x phase
//   3 End        (host -> all)   flag = natural (1: the host slept to full --
//                                 every peer is granted sleep=100; 0: early
//                                 interrupt -- peers keep their accrued need)
struct SleepStatePayload {
    uint8_t op;     // 1
    uint8_t flag;   // 1 -- Report: inBed; End: natural
    uint8_t count;  // 1 -- Tally: peers in bed
    uint8_t total;  // 1 -- Tally: world-ready peers
};
static_assert(sizeof(SleepStatePayload) == 4, "SleepStatePayload must be 4 bytes");

// v95: HOST->ALL "a scripted/story event fired" (ReliableKind::EventFire). Origin = the host's
// passEvents growth poll (scheduler fires) or the F1 dev menu at dispatch (a direct runEvent
// never appends passEvents -- bytecode-verified). Receivers replay the native verb reflected
// ONLY for policy-allowlisted rows (coop/world/event_fire_sync -- the dupe matrix; lane-covered
// rows would double-deliver). dispatch: 0 = runEvent(name, None), 1 = runSpecialEvent(name).
// NO special field by design (the only native special, 'ariralPrank', is a host-local RNG roll).
struct EventFirePayload {
    uint8_t dispatch;  // 1 -- event_fire_sync::FireKind (0 runEvent / 1 runSpecialEvent)
    char name[31];     // 31 -- the row/case FName, ASCII, NUL-bound (longest live row = 18 chars)
};
static_assert(sizeof(EventFirePayload) == 32, "EventFirePayload must be 32 bytes");

// v98: HOST->JOINER one in-flight event registry entry (ReliableKind::EventSnapshot; join-during-
// event Phase 1, COOP_EVENT_JOIN.md 3.2). Origin = the host's 1 Hz activeEvents_senders mirror
// (event_active_sync) read at the joiner's world-ready edge. className is ClassOf(sender) (the
// event's implementation class, e.g. "obelisk_C"); rowName is the mapped list_events row the
// receiver's replay policy is keyed by ('' = unmapped class -- logged LOUD + skipped). elapsedSec
// is the host-observed in-flight duration (diagnostics now; Phase 3 phase-hint fast-forward later).
struct EventSnapshotPayload {
    char className[48];   // 48 -- ASCII, NUL-bound (longest census class ~30 chars)
    char rowName[48];     // 48 -- ASCII, NUL-bound; '' = class->row map has no entry yet
    uint16_t elapsedSec;  // 2  -- clamped at 65535 (18 h; event phases run seconds-to-minutes)
};
static_assert(sizeof(EventSnapshotPayload) == 98, "EventSnapshotPayload must be 98 bytes");
static_assert(sizeof(EventSnapshotPayload) <= 256 - 20 - 8,
              "EventSnapshotPayload must fit in one reliable datagram");

// v101: the base radar alarm active state (ReliableKind::AlarmState; docs/events/alarm.md).
// Symmetric shared-world toggle: host->all on any observed trigger_alarm_C.active transition
// (+ unconditionally to a joiner at the world-ready edge), client->host when the client's own
// radar scan / stop-press toggled it locally. Receiver applies via a reflected runTrigger
// (natively idempotent) -- see coop/world/alarm_sync.
struct AlarmStatePayload {
    uint8_t active;   // 1 -- 0/1, the desired trigger_alarm_C.active state
    uint8_t pad[3];   // 3 -- zeroed
};
static_assert(sizeof(AlarmStatePayload) == 4, "AlarmStatePayload must be 4 bytes");

// v107: signal-SERVER simulation state (ReliableKind::ServerState). HOST->clients, one-directional
// (host-authoritative; the client never sends). The host polls mainGamemode.servers[] each ~1 Hz and
// on any change broadcasts the whole small snapshot; the client DRIVE-REALs it (raw-write each
// serverBox.IsBroken then reflected check() to re-skin) + mirrors the gamemode aggregates. Servers are
// identified by their save-stable servers[] ARRAY INDEX (both peers load the same host save in order),
// so isBrokenMask bit i == servers[i].IsBroken. Up to 64 servers (a base's server farm ran 54 -- smoke
// 2026-07-09); a larger world logs + caps to the first 64. See coop/interactables/serverbox_sync.h.
struct ServerStatePayload {
    int32_t  brokenServers;   // 4  -- mainGamemode.brokenServers (aggregate mirror)
    float    effCalc;         // 4  -- serverEfficiency_calc
    float    effDownl;        // 4  -- serverEfficiency_downl
    uint8_t  serverCount;     // 1  -- servers[].Num at send (bounds; <=64 carried in the mask)
    uint8_t  _pad[3];         // 3  -- zeroed (isBrokenMask is 8-aligned at offset 16)
    uint64_t isBrokenMask;    // 8  -- bit i = servers[i].IsBroken (up to 64 servers)
};
static_assert(sizeof(ServerStatePayload) == 24, "ServerStatePayload must be 24 bytes");

// v108: one page of the host's roach-infestation snapshot (ReliableKind::RoachState).
// A snapshot is the FULL live-roach set (valid slots of cockroachMaster.roaches in
// array order); pages of one snapshot share `seq` and arrive in-order on the
// reliable lane. The client assembles all `pageCount` pages, then applies by
// ordinal. Positions are absolute world floats (roaches roam the whole base --
// no quantization); scale is uniform (VSize-driven growth, applied via
// SetWorldScale3D / addRoach's Size param).
struct RoachStatePayload {
    uint32_t seq;         // 4 -- snapshot sequence (per-host monotonic)
    uint8_t  page;        // 1 -- 0-based page index
    uint8_t  pageCount;   // 1 -- total pages in this snapshot (>=1)
    uint8_t  entryCount;  // 1 -- entries used in THIS page (<= kRoachEntriesPerPage)
    uint8_t  totalCount;  // 1 -- total live roaches in the snapshot (<= 128 = maxAmount CDO)
    struct Entry {
        float x, y, z;    // component world location
        float scale;      // uniform world scale
    } entries[12];        // 192
};
inline constexpr int kRoachEntriesPerPage = 12;
inline constexpr int kRoachSnapshotCap    = 128;  // cockroachMaster.maxAmount CDO
static_assert(sizeof(RoachStatePayload) == 200, "RoachStatePayload must be 200 bytes");
static_assert(sizeof(RoachStatePayload) <= 256 - 20 - 8,
              "RoachStatePayload must fit in one reliable datagram");

// v108: a client's local roach consumption (ReliableKind::RoachConsumed).
struct RoachConsumedPayload {
    float x, y, z;        // last known world location of the consumed roach's component
};
static_assert(sizeof(RoachConsumedPayload) == 12, "RoachConsumedPayload must be 12 bytes");

// v108 OWNER-ENTITY lane (OwnerEntitySpawn/Pose/Destroy). Identity is
// (transport senderPeerSlot, seq) -- no element eid: the lane is a
// self-contained per-owner display mirror, not part of the host-auth
// element registry (a peer-owned axis; anti-smear: its own owner module).
struct OwnerEntitySpawnPayload {
    uint16_t seq;         // 2 -- owner-local monotonic entity id
    uint8_t  classId;     // 1 -- index into the module's class table (0 = eyer_C)
    uint8_t  _pad;        // 1 -- zeroed
    float    x, y, z;     // 12 -- world location
    float    yaw;         // 4 -- degrees
};
static_assert(sizeof(OwnerEntitySpawnPayload) == 20, "OwnerEntitySpawnPayload must be 20 bytes");

struct OwnerEntityPosePayload {
    uint16_t seq;         // 2
    uint8_t  _pad[2];     // 2 -- zeroed
    float    x, y, z;     // 12
    float    yaw;         // 4
};
static_assert(sizeof(OwnerEntityPosePayload) == 20, "OwnerEntityPosePayload must be 20 bytes");

struct OwnerEntityDestroyPayload {
    uint16_t seq;         // 2 -- 0 = WILDCARD: destroy ALL entities of originSlot (host teardown)
    uint8_t  originSlot;  // 1 -- 0 = "the transport sender" (owner-sent); non-zero only on the
                          //      HOST-sent leaver teardown (audit F-1: a client transport edge for
                          //      another client's slot does not exist, so the host fans the
                          //      leaver's teardown out on its behalf)
    uint8_t  _pad;        // 1 -- zeroed
};
static_assert(sizeof(OwnerEntityDestroyPayload) == 4, "OwnerEntityDestroyPayload must be 4 bytes");

// v64/v65: one chunk of a variable-length serialized blob. Shared by every
// chunked-row kind (EmailAppend v64, SavedSignalAppend + CompData v65);
// assembly key = (transport senderSlot, kind, blobSeq); chunks arrive
// in-order on the reliable lane; coop/blob_chunks owns send + reassembly
// (C-1 restart-on-chunk-0 + TTL semantics). Email blob: { uint8 version(=1);
// uint8 username; uint16 topicChars; uint16 textChars; uint16 pfpChars;
// topic UTF-16LE; text; pfpLeaf } -- caps topic 256 / text 4096 / pfp 96.
// Signal-row blob (SavedSignalAppend / CompData): see coop/signal_wire.
struct BlobChunkPayload {
    uint32_t blobSeq;    // 4 -- per-SENDER monotonically increasing (per kind)
    uint8_t  chunkIdx;   // 1
    uint8_t  chunks;     // 1 -- total (>=1)
    uint16_t chunkLen;   // 2 -- used bytes in data[]
    uint8_t  data[220];  // 220
};
static_assert(sizeof(BlobChunkPayload) == 228, "BlobChunkPayload must be 228 bytes");
static_assert(sizeof(BlobChunkPayload) <= 256 - 20 - 8,
              "BlobChunkPayload must fit in one reliable datagram");

// v65: one content-keyed delete (EmailDelete / SavedSignalDelete). The hash is
// FNV-1a 64 over the row's serialized append blob (which excludes per-peer
// re-stamped fields), so the producer and every receiver derive the SAME key
// for the same row regardless of local array order.
struct ContentHashPayload {
    uint64_t contentHash;
};
static_assert(sizeof(ContentHashPayload) == 8, "ContentHashPayload must be 8 bytes");

// v65: the refiner decode-pane scalar stream (ReliableKind::CompState), sent
// ~1 Hz by the SIMULATING peer while comp_isDecodeActive, plus on edges and
// as the host's adopt snapshot at connect-replay. decodeActive is WIRE-ONLY
// state on mirrors -- never written to the mirror's comp_isDecodeActive (a
// latched mirror would simulate the decode itself; see ReliableKind docs).
struct CompStatePayload {
    uint8_t decodeActive;  // 1
    uint8_t adopt;         // 1 -- host connect snapshot (trust-gated to slot 0)
    uint8_t isFinalLevel;  // 1 -- v66 (audit I-1): stamped by the SIMULATOR at the
                           //      falling edge from its post-increment level vs
                           //      maxLevel; the mirror's Done-vs-prog beep MUST use
                           //      this (its local level is pre-CompData stale --
                           //      CompState outruns the chunked data on the lane)
    uint8_t _pad;          // 1
    float   progress;      // 4 -- comp_progress (0..100)
    float   downloading;   // 4 -- comp_downloading (this tick's increment; the B\s readout)
};
static_assert(sizeof(CompStatePayload) == 12, "CompStatePayload must be 12 bytes");

// v64: the dish-aim stream (ReliableKind::DishAimState) -- the claim owner's
// coords-panel cursor state at ~3 Hz while the desk is claimed. Phase-2 impl
// RE falsified the spaceRenderer.coords/coords_rot fields (dead bytecode);
// the REAL aim lives on the ui_coordinates widget: viewCoordinate + the three
// Coordinate_N cursors + the selected index. Receivers raw-write + reflected
// updCursorLocations() (which also rotates the physical pingDishes).
// v70: + direction (ui_coordinates.Direction, the panel polarity toggle that
// GATES catch success @9189) -- aim state, rides the pad (size unchanged).
// v109: the LIVE cursor (viewCoordinate) is NO LONGER here -- it moved to the
// unreliable DeskCursorPose=36 stream (continuous motion). This reliable payload
// now carries ONLY the discrete committed-coord locks (the triangulation IDENTITY),
// change-gated + connect-snapshotted. See DeskCursorPoseSnapshot below.
struct DishAimStatePayload {
    float   c0X, c0Y;                // 8  -- Coordinate_0
    float   c1X, c1Y;                // 8  -- Coordinate_1
    float   c2X, c2Y;                // 8  -- Coordinate_2
    int32_t selected;                // 4  -- the selected cursor index
    uint8_t direction;               // 1  -- v70: the Direction toggle (catch gate)
    uint8_t _pad[3];                 // 3
};
static_assert(sizeof(DishAimStatePayload) == 32, "DishAimStatePayload must be 32 bytes (v109: -viewCoordinate)");

// KeyedScalarPayload -- the SHARED payload for "keyed monotone-decreasing dirt scalar" sync:
// WindowCleanState (30, v41 -- AbaseWindow_C::clean@0x0260) AND GrimeState/GrimeDestroy (31/32,
// v42 -- Agrime_C::process@0x0250). One payload for the keyed-float kinds, mirroring how
// KeyedTogglePayload serves the 3 bool kinds (RULE 2). `key` is the instance's cross-peer-stable
// string identity (the window's Aactor_save_C::Key FName for windows; a QUANTIZED WORLD-POSITION
// string for the static, unkeyed grime decals). `value` is the dirt scalar (>= 0; wiped DOWN,
// never re-raised). SYMMETRIC: every peer polls + broadcasts on a decrease; the host relays a
// client's wipe. The receiver resolves the instance by `key` and applies MIN(local, value) for a
// LIVE wipe (adopt==0) so a wire update can only clean, never re-dirty -> concurrent wipes
// converge with no oscillation. adopt==1 (host connect-snapshot only -- trust-gated to senderSlot
// 0 at the receiver) overrides MIN and writes VERBATIM so a joiner adopts the host's world.
// GrimeDestroy reads only `key` (value/adopt ignored). The window/grime modules do the engine
// read/write through ue_wrap::base_window / ue_wrap::grime.
struct KeyedScalarPayload {
    WireKey  key;        // 32 -- instance identity string (FName for windows; quantized position for grime)
    float    value;      // 4  -- the dirt scalar (>= 0; 0 = fully clean)
    uint8_t  adopt;      // 1  -- 1 = connect-snapshot, apply VERBATIM (adopt host's world); 0 = live wipe, apply MIN
    uint8_t  _pad[3];    // 3  -- 8-byte alignment / reserved
};
static_assert(sizeof(KeyedScalarPayload) == 40, "KeyedScalarPayload must be 40 bytes");
static_assert(sizeof(KeyedScalarPayload) <= 256 - 20 - 8,
              "KeyedScalarPayload must fit in one reliable datagram");

// TrashPileStatePayload -- the trashBitsPile_C collect-counter mirror (TrashPileState=46, v57).
// amountA/amountB are the pile's two dispense sides (the displayed count is their sum). int16
// is ample (BeginPlay rolls 3-6/2-3; saves carry small ints). adopt=1 = host connect-snapshot,
// apply VERBATIM; adopt=0 = live collect edge, receiver applies per-component MIN(local, wire)
// -- the counters are monotone-down, so concurrent collects on both peers converge without
// oscillation (same-side simultaneous picks lose at most one decrement; acceptable for litter).
struct TrashPileStatePayload {
    WireKey  key;        // 32 -- Aactor_save_C::Key @0x0230 (FName string; save-persisted)
    int16_t  amountA;    // 2  -- AtrashBitsPile_C::amountA @0x0260
    int16_t  amountB;    // 2  -- AtrashBitsPile_C::amountB @0x0264
    uint8_t  adopt;      // 1
    uint8_t  _pad[3];    // 3
};
static_assert(sizeof(TrashPileStatePayload) == 40, "TrashPileStatePayload must be 40 bytes");
static_assert(sizeof(TrashPileStatePayload) <= 256 - 20 - 8,
              "TrashPileStatePayload must fit in one reliable datagram");

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
// RE: research/findings/computers-devices/votv-keypad-door-BP-disassembly-2026-06-06.md.
// v59 (2026-06-11): + `event` -- the SHORT-code submit mirror. The BP auto-submits at
// Len>=5 (uber @2398), so long codes validate natively on EVERY peer from the digit replay
// alone; but a short code's ACCEPT press (open(password==inPassword)) and the explicit
// CANCEL (open(false)) change NO digit -- they were invisible on the wire, and the old
// host-side "accept when buffer==password" poll fired WITHOUT the accept press (2026-06-11
// user bug: door triggers the moment the last digit is typed) and latched the LED green
// forever. Now the TYPING peer detects its own native submit edge (active flip + buffer
// cleared, lastKnown buffer 0<len<5, not reset mode) and stamps Accept/Deny; the receiver
// runs the keypad's OWN native Open(Active) chain (accept/deny sound, LED, buffer clear,
// LOCK-state propagation to pair + gated door -- an accept UNLOCKS the door, never opens
// it; opening is a normal E press on the door channel). len>=5 transitions stamp None
// (native already ran everywhere -- a stamped event would double-run the chain).
enum class KeypadEvent : uint8_t {
    None   = 0,  // plain state mirror (digits / active)
    Accept = 1,  // short-code accept press with correct code -> receiver runs native Open(true)
    Deny   = 2,  // short-code wrong-accept / explicit cancel -> receiver runs native Open(false)
};
struct KeypadSyncPayload {
    WireKey  key;        // 32 -- the keypad's Key FName (string)
    uint8_t  bufLen;     // 1  -- digits in `buf` (0..16; codes are short)
    uint8_t  buf[16];    // 16 -- the typed digits, one per byte (each 0..9)
    uint8_t  active;     // 1  -- v38: keypad `active` @0x0330 (LED selector: 0=red/locked, 1=green/powered; mirrors cancel->red)
    uint8_t  event;      // 1  -- v59: KeypadEvent (the short-code submit mirror)
    uint8_t  _pad[5];    // 5  -- 8-byte alignment / reserved (was isAcc/isDeny + pad; removed v35)
};
static_assert(sizeof(KeypadSyncPayload) == 56, "KeypadSyncPayload must be 56 bytes");
static_assert(sizeof(KeypadSyncPayload) <= 256 - 20 - 8,
              "KeypadSyncPayload must fit in one reliable datagram");

// v46 (2026-06-08): the base POWER PANEL (ApowerControl_C) breaker state. 5 latched press
// bools packed into a bitmask (bit0=coord, 1=downl, 2=play, 3=calc, 4=light -- the FIELD
// order press_coord/downl/play/calc/light @0x0380-0x0384). SYMMETRIC: any peer flips a
// breaker; the receiver mirrors the panel's own visual (lever positions + LED particles)
// through ue_wrap::power_control. Keyed by the panel's AtriggerBase_C::Key. Its own module
// coop/power_sync (the 5-bool state doesn't fit the generic 1-bool toggle Channel). NOTE: the
// powerChanged() setter's ARG order is (calc,downl,coords,play,light) -- different from this
// field/bit order -- so the wrapper maps bit<->arg by NAME, not position. RE: research/
// findings/votv-powerControl-panel-sync-RE-2026-06-08.md.
struct PowerPanelPayload {
    WireKey  key;        // 32 -- the panel's AtriggerBase_C::Key FName (string)
    uint8_t  pressMask;  // 1  -- bit0=coord,1=downl,2=play,3=calc,4=light (press_* @0x0380-0x0384)
    uint8_t  _pad[7];    // 7  -- 8-byte alignment / reserved
};
static_assert(sizeof(PowerPanelPayload) == 40, "PowerPanelPayload must be 40 bytes");

// v47 (2026-06-08): ATV/quadbike (AATV_C) Phase 1 body pose + state. Occupant-authoritative
// keyed pose stream (the seated driver streams; host relays a client driver's pose; receivers
// mirror kinematically with a LerpWindow interp unless they are the occupant). Identity =
// Key@0x0618 (save-placed, cross-peer stable). See the ReliableKind::AtvState comment for the
// full authority/relay/connect-snapshot model. coop/atv_sync + ue_wrap/atv.
struct AtvStatePayload {
    WireKey  key;          // 32 -- the ATV's Key@0x0618 (FName string)
    float    x, y, z;      // 12 -- root body world location (cm; the root Mesh == the actor)
    float    pitch, yaw, roll;  // 12 -- full rotation (the ATV tips/flips, unlike a biped)
    uint8_t  occupantSlot; // 1  -- the driver's peer slot (0xFF = unoccupied) [Phase 1.5 seating]
    uint8_t  stateBits;    // 1  -- bit0=isDriven, bit1=brake, bit2=grabbed (grav-hand carried), bit3=authored
                           //      (v77: SOME peer is actively driving/grabbing this ATV -- set on the
                           //      connect-snapshot so a joiner freezes only authored ATVs; idle ones stay
                           //      physics-on + grabbable. A live stream is always from an authority, so the
                           //      bit is informational there.)
    uint8_t  adopt;        // 1  -- 1 = host connect-snapshot (snap verbatim), 0 = live stream
    uint8_t  _pad;         // 1
};
static_assert(sizeof(AtvStatePayload) == 60, "AtvStatePayload must be 60 bytes");

// v77 (2026-06-15): purchased-ATV host announce (see ReliableKind::AtvSpawn). The host fresh-spawns
// a bought ATV for the clients that lack a local twin (host-only economy). Carries the className so
// the client BeginDeferred-spawns the exact AATV_C skin, and the host-assigned synthetic wire key
// (the purchased ATV's own int_save key is random per peer -> useless cross-peer). coop/atv_sync.
struct AtvSpawnPayload {
    WireKey       synthKey;   // 32 -- host-assigned stable identity ("coopatv#N")
    WireClassName className;   // 64 -- "ATV_C" or a skin subclass
    float         x, y, z;     // 12 -- spawn pose (cm)
    float         pitch, yaw, roll;  // 12
};
static_assert(sizeof(AtvSpawnPayload) == 120, "AtvSpawnPayload must be 120 bytes");
static_assert(sizeof(AtvSpawnPayload) <= 256 - 20 - 8,
              "AtvSpawnPayload must fit in one reliable datagram (kMaxReliablePayload)");

// v77 (2026-06-15): purchased-ATV host teardown (ReliableKind::AtvDestroy). coop/atv_sync.
struct AtvDestroyPayload {
    WireKey synthKey;  // 32
};
static_assert(sizeof(AtvDestroyPayload) == 32, "AtvDestroyPayload must be 32 bytes");

// v76 (2026-06-15): ATV grab-carry RELEASE -- sent ONCE on the grab-release/throw edge by the peer
// that was the grav-hand grabber (atv_sync). The receiver re-enables the mirror's physics
// (ReleaseMirror) and THEN writes the inherited PhysX velocity (lin + ang) so the ATV un-freezes
// and arcs/lands under its own simulation instead of hanging at the last streamed pose. Order
// (simulate-on BEFORE velocity) + the payload shape mirror PropReleasePayload exactly -- a kinematic
// body ignores a velocity write, so the re-enable must precede it. Carries the Key so an out-of-order
// release still finds the right ATV. coop/atv_sync.
struct AtvReleasePayload {
    WireKey key;       // 32 -- the ATV's Key@0x0618
    float   linVelX;   // cm/s   -- GetActorRootPhysicsVelocity linear at release
    float   linVelY;
    float   linVelZ;
    float   angVelX;   // deg/s  -- GetActorRootPhysicsVelocity angular at release
    float   angVelY;
    float   angVelZ;
};
static_assert(sizeof(AtvReleasePayload) == 56, "AtvReleasePayload must be 56 bytes");
static_assert(sizeof(AtvReleasePayload) <= 256 - 20 - 8,
              "AtvReleasePayload must fit in one reliable datagram (kMaxReliablePayload)");

// v48 (2026-06-08): delivery drone (Adrone_C) Phase 1 body pose. HOST-AUTHORITATIVE singleton
// transform mirror (the drone is host-simulated; the client suppresses its own ReceiveTick + drives
// the streamed transform kinematically with a LerpWindow interp). No key (singleton, resolved by
// class). See the ReliableKind::DroneState comment for the full model. coop/drone_sync + ue_wrap/drone.
struct DroneStatePayload {
    float   x, y, z;           // 12 -- root actor world location (cm)
    float   pitch, yaw, roll;  // 12 -- full rotation (the drone leans/pitches in flight)
    uint8_t active;            // 1  -- Adrone_C::Active (dormant<->flying); gates the host stream
    uint8_t stateBits;         // 1  -- FX + interaction mirror (v49 Phase 2): bit0=rotor dust active
                               //        (eff_droneDust), bit1=canTakeOff (arrived: plays the audio_alarm
                               //        cue + signal light AND is THE interaction gate -- written onto
                               //        the mirror so a parked drone isn't "in motion"), bit2=hasSack
                               //        (cargo aboard: the action-option prerequisite, also written onto
                               //        the mirror). Host packs via ue_wrap::drone::ReadFxBits; client
                               //        replays/writes on the bit edges (the suppressed tick can't).
    uint8_t adopt;             // 1  -- 1 = host connect-snapshot (snap verbatim), 0 = live stream
    uint8_t _pad;              // 1
    float   dustX, dustY, dustZ;  // 12 -- v69: eff_droneDust world location (the host BP pins the
                               //        bAbsoluteLocation component to its ground-trace hit per tick;
                               //        the mirror replays K2_SetWorldLocation + the 'dust' param from
                               //        it). Valid only while stateBits bit0 is set; zeros otherwise.
};
static_assert(sizeof(DroneStatePayload) == 40, "DroneStatePayload must be 40 bytes");

// v49 (2026-06-09): delivery-drone ECONOMY -- the CLIENT->HOST OrderRequest (see ReliableKind::
// OrderRequest). VARIABLE-LENGTH: this fixed 16-byte header is followed by `chunkItems` packed
// items, each laid out as:
//     int32  price;       // Fstruct_store.price  @0x00 (box-fidelity, not load-bearing)
//     int32  size;        // Fstruct_store.size   @0x40
//     uint8  category;    // Fstruct_store.category@0x20 (enum_shopCats)
//     uint8  objLen;      // length of the class name that follows (1..kMaxOrderClassName)
//     <objLen bytes>      // object @0x10 leaf CLASS NAME (ASCII; host FindClass -> the spawn class)
// An order with more items than fit in one datagram (kMaxReliablePayload) is split into multiple
// OrderRequest messages sharing one orderId; the host assembles them by (senderSlot, orderId) using
// baseIndex/totalItems, then commits once all totalItems arrive. The reliable channel is ordered, so
// chunks arrive in baseIndex order. (omitted vs Fstruct_store: subcategory FText + achievementUnlock
// + name/asProp -- bytecode-verified never read on the commit/deliver path; the spawn uses `object`
// directly. host reconstruction fills subcategory with a pinned empty FText for laptop-UI safety.)
struct OrderRequestHeader {
    uint32_t orderId;     // 4 -- client-local monotonic order id (unique per sender slot)
    uint16_t totalItems;  // 2 -- total items in the WHOLE order (1..kMaxOrderItems)
    uint16_t baseIndex;   // 2 -- index of this chunk's first item (== items already sent)
    uint16_t chunkItems;  // 2 -- items carried in THIS message
    uint16_t _pad;        // 2
    float    time;        // 4 -- delivery-ETA (Fstruct_storeOrder.time @0x10; same across chunks)
};
static_assert(sizeof(OrderRequestHeader) == 16, "OrderRequestHeader must be 16 bytes");

// Economy wire bounds (host trust boundary -- a client must not make the host allocate unbounded).
inline constexpr int kMaxOrderItems     = 64;   // a cart > 64 line-items is rejected as garbage
inline constexpr int kMaxOrderClassName = 96;   // UE leaf class names are short; cap the per-item string

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
    uint8_t       savePersisted;   // 1 -- v75: 1 = this NPC is a save object (has a non-None int_save
                                   //      "Key") that the joining client ALSO loaded from the transferred
                                   //      save -> the client must ADOPT its own local twin (class-match,
                                   //      coop/npc_adoption) instead of spawning a duplicate. 0 = no local
                                   //      twin (host-spawned transient enemy) -> fresh-spawn a mirror.
                                   //      REPLACES v74's WireKey: the kerfur's int_save Key is minted
                                   //      RANDOM per load (kerfurOmega::loadData overrides the int_save
                                   //      base + drops the key restore -- bytecode-proven), so it differs
                                   //      across peers and key-equality adoption is impossible. Only the
                                   //      PRESENCE of a key (= is-a-save-object) is portable, not its value.
                                   //      See research/findings/votv-kerfurOmega-coop-double-and-camera-
                                   //      RE-2026-06-14.md sec "CORRECTION" + sec 11.
    uint8_t       _pad[3];         // 3 -- align loc to 4
    float         locX, locY, locZ;            // 12 -- world cm at spawn time
    float         rotPitch, rotYaw, rotRoll;   // 12 -- FRotator
    float         scaleX, scaleY, scaleZ;      // 12 -- v99: actor Scale3D at spawn/snapshot time. The
                                               //      spawn TRANSFORM carries scale in-engine (the
                                               //      piramid spawner passes 2.0) and a mirror spawned
                                               //      without it renders wrong-size + hovers at the
                                               //      wrong altitude (Z streams from the host, whose
                                               //      hover = 10000*scaleX above ground). Receivers
                                               //      sanitize via SanitizeWireScaleAxis (a zeroed
                                               //      legacy/degraded payload must not spawn an
                                               //      invisible scale-0 actor).
    uint32_t      retireOffEid;    // 4 -- v91 (kerfur off->active dup retire, DETERMINISTIC): non-zero => this
                                   //      NPC is a kerfur the host turned ON in the join window, and
                                   //      retireOffEid is the HOST EID of the off-prop it replaced
                                   //      (KerfurRecord.originOffEid, captured at the first conversion). The
                                   //      joining client retires the off-prop MIRROR bound at that exact eid
                                   //      (kerfur_reconcile, eid-keyed). REPLACES the v88 position-fuzzy
                                   //      hasMatchPos+matchX/Y/Z: the active form rides THIS npc channel (which
                                   //      reaches the joiner), unlike the KerfurConvert reliable that is
                                   //      pre-world-gated mid-join (v56 B2; hands-on 16:37 + 13:21 root). The
                                   //      client already binds its save-loaded off-prop to the host eid via
                                   //      save_identity_bind, so the retire is deterministic -- no 1cm
                                   //      position match, no fuzzy collision (the recurring 5-of-6 / dup class).
                                   //      0 = not a window turn-on (always-active NPC, or a transient enemy;
                                   //      WorldActorSpawn reuse leaves it 0).
    uint32_t      convertFromEid;  // 4 -- v91 (kerfur turn-on ghost adopt, DETERMINISTIC): non-zero => this
                                   //      kerfur NPC EntitySpawn is a MID-SESSION turn-on, and convertFromEid is
                                   //      the host eid of the form it converted FROM (KerfurConvert oldEid =
                                   //      KerfurRecord.lastConvertFromEid). The INITIATING client parked a local
                                   //      conversion ghost tagged with that exact eid (ClaimConversionGhosts);
                                   //      npc_mirror::OnEntitySpawn adopts THAT ghost by eid (TakeParkedGhostByEid)
                                   //      instead of the old FindParkedGhostNpcNear 500cm position match -- so the
                                   //      EntitySpawn-vs-KerfurConvert race can't fuzzy-miss into a respawn-pop/dup.
                                   //      A non-initiator peer has no ghost at that eid -> null -> fresh-spawns. 0 =
                                   //      not a conversion (a save/connect-snapshot NPC; WorldActorSpawn leaves it 0).
};
static_assert(sizeof(EntitySpawnPayload) == 116, "EntitySpawnPayload must be 116 bytes (v99: +Scale3D; v91: deterministic retireOffEid + convertFromEid, no position-fuzzy fields)");
static_assert(sizeof(EntitySpawnPayload) <= 256 - 20 - 8,
              "EntitySpawnPayload must fit in one reliable datagram");

// v99 receiver-side scale sanitizer (trust boundary, same rank as the NaN/kMaxCoord checks): a
// non-finite / zero / absurd wire scale must not reach FinishSpawning (scale-0 spawns an invisible
// actor; the degraded-sender case is a zeroed payload field). Unit scale is the only safe fallback.
inline float SanitizeWireScaleAxis(float s) {
    if (!(s > 0.01f && s < 100.f)) return 1.f;  // NaN fails both comparisons -> 1
    return s;
}

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
//   research/findings/inventory-items/votv-flashlight-RE-2026-05-25.md
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

// WispGrab (67) -- host->ONE victim slot. The host's killerwisp_C is grabbing this
// client's puppet (its acquired Target classified as a client puppet via GetController);
// the host neutralized its own false-grab and tells the addressed client to ragdoll-die
// for real after a host-decided fixed delay (the kill is per-peer-authoritative: the
// victim runs ragdollMode on its OWN player -- the wisp's BP only ever kills the local
// host). Host-only send; receiver gates senderPeerSlot==0 + victimElementId==own; not
// relayable. See research/findings/npc-creatures/votv-killerwisp-coop-design-2026-06-13.md (B).
struct WispGrabPayload {
    uint32_t victimElementId;  // the addressed peer's Player Element id (self-verify == own)
    uint32_t wispElementId;    // the killerwisp NPC Element id (tear-mirror association)
    uint32_t killDelayMs;      // host-decided delay before the victim ragdolls (~tear length)
};
static_assert(sizeof(WispGrabPayload) == 12, "WispGrabPayload must be exactly 12 bytes (v72)");
static_assert(sizeof(WispGrabPayload) <= 256 - 20 - 8,
              "WispGrabPayload must fit in one reliable datagram");

// WispTear (68) -- host->ALL. Every peer plays the fatality tear on its LOCAL mirror of
// the wisp (resolved by wispElementId via the Npc Registry) and socket-attaches the
// victim's puppet (Registry::Puppet(victimSlot)) to the wisp 'playerGrab' socket. On the
// victim's own machine victimSlot==own -> there is no self-puppet (the real ragdoll death
// from WispGrab is the local view). Host-only send; receiver gates senderPeerSlot==0; not
// relayable. See the design doc (C).
struct WispTearPayload {
    uint32_t wispElementId;    // the killerwisp NPC Element id -> resolve the local mirror
    uint32_t victimSlot;       // cross-peer Registry slot of the victim (whose puppet to hold)
};
static_assert(sizeof(WispTearPayload) == 8, "WispTearPayload must be exactly 8 bytes (v72)");
static_assert(sizeof(WispTearPayload) <= 256 - 20 - 8,
              "WispTearPayload must fit in one reliable datagram");

// PyramidGather (85) -- v97 piramid mirror lane: the host pyramid committed a wisp gather
// (checkIfReached arrived -> gathering latched). Both ids are the carrying lanes' own
// identities: the pyramid is a WorldActor element (world_actor_sync), the wisp an Npc
// element (npc lane). Receiver: coop/creatures/piramid_sync (client replays the native
// branch on its mirrors once their interp converges inside the arrive radius).
struct PyramidGatherPayload {
    uint32_t pyramidEid;  // host-range WorldActor element id of the piramid2_C
    uint32_t wispEid;     // host-range Npc element id of the gathered killerwisp_C
};
static_assert(sizeof(PyramidGatherPayload) == 8, "PyramidGatherPayload must be exactly 8 bytes (v97)");
static_assert(sizeof(PyramidGatherPayload) <= 256 - 20 - 8,
              "PyramidGatherPayload must fit in one reliable datagram");

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
// research/findings/weather-wind/votv-weather-DESIGN-2026-05-26.md for the field-by-field
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
// Wind: the 4 AdirectionalWind_C persistent fields ARE on the wire (v43, below). The
// prior claim that "rainWindSpeed is the only wind field we need + setWindParameters()
// propagates it" was DISPROVEN by RE (votv-wind-basefog-RE-2026-06-08.md):
// setWindParameters() only writes the RAIN pair (windSpeed_rain = rainWindSpeed,
// windStrength_rain = (rainStrength+0.5)*rain -- the latter from the cycle's per-peer
// free-running `rain`), and NEVER touches windSpeed/Strength_background, which default
// 5.0/0 and are only changed by the day-rollover Ease -> they diverge per peer (the
// "strong wind on the joined client"). So the host streams all 4 and the client
// overwrites them every apply; the client's own ReceiveTick + 1 s updateDirWind then
// converge the totals + the engine WindDirectionalSource. This is host-authority of
// ONE state (not a parallel path) -- setWindParameters() is no longer the wind sync.
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
    // v43 (2026-06-08): WIND rain/background fields. Correct for rain-wind + the
    // particle/audio/engine SPEED, but NOT the leaf-shake (that's windTarget/intensity,
    // v50 below): the tick overwrites windStrengthBg = intensity every frame, so syncing
    // it alone can't fix "strong leaves on host, calm on client". The client overwrites
    // these every apply. RE: research/findings/weather-wind/votv-wind-basefog-RE-2026-06-08.md.
    float    windSpeedBg;       // AdirectionalWind_C::windSpeed_background    @0x02EC
    float    windStrengthBg;    // AdirectionalWind_C::windStrength_background @0x02F0
    float    windSpeedRain;     // AdirectionalWind_C::windSpeed_rain          @0x02E4
    float    windStrengthRain;  // AdirectionalWind_C::windStrength_rain       @0x02E8
    // v50 (2026-06-09): the GUST INPUT -- windTarget's RelativeLocation. THE leaf-shake
    // driver: the tick springs `intensity` (= the foliage MPC scalar + engine wind) from
    // it. Re-rolled per-peer by a `changeWindOrigin` RNG timer (random 1-60 s), so host is
    // mid-gust while client is calm. Host reads / client writes this + the client suppresses
    // its changeWindOrigin (so its local roll stops fighting the synced target). Gated by
    // kWindValid (shared with the 4 fields above). RE: votv-wind-event-driver-RE-2026-06-09.md.
    float    windTargetX;       // AdirectionalWind_C::windTarget->RelativeLocation.X
    float    windTargetY;       //                                              .Y
    float    windTargetZ;       //                                              .Z
};
static_assert(sizeof(WeatherStatePayload) == 68, "WeatherStatePayload must be 68 bytes (v50: +windTarget Vec3)");
static_assert(sizeof(WeatherStatePayload) <= 256 - 20 - 8,
              "WeatherStatePayload must fit in one reliable datagram");

// FireflySpawnPayload -- one mirrored firefly emitter (FireflySpawn=40, v51). A peer
// captures its OWN firefly spawn (PRE+POST ReceiveTick diff -- see ReliableKind::FireflySpawn)
// and broadcasts the world location; every other peer spawns eff_fireflies there. World-space
// position only -- the eff_fireflies template, zero rotation, unit scale, autoDestroy are
// fixed (the firefly BP always spawns with those). PEER-SYMMETRIC + host-relayed.
struct FireflySpawnPayload {
    float x;  // world spawn location (the grass hit point near the host's camera)
    float y;
    float z;
};
static_assert(sizeof(FireflySpawnPayload) == 12, "FireflySpawnPayload must be 12 bytes");

// EventCuePayload -- one HOST-AUTHORITATIVE cosmetic emitter cue (EventCue=75, v79; B1). The
// host detects a new cosmetic ParticleSystemComponent whose Template is a registered cue
// (cueId == index into coop/event_cue_sync's static cue registry; starRain=0) and broadcasts
// the spawn position; every client replays that emitter there via reflected
// SpawnEmitterAtLocation. World-space position only (the firefly shape): for a fixed-location
// cue like starRain the host sends the BP-hardcoded (0,0,6000); for a positioned cue it sends
// the captured component location. Host->client only (clients never fire events), so no relay,
// no echo. cueId is on the wire and APPEND-ONLY (never renumber the registry).
struct EventCuePayload {
    uint32_t cueId;  // index into event_cue_sync's cue registry (append-only)
    float x;         // world spawn location of the cue emitter
    float y;
    float z;
};
static_assert(sizeof(EventCuePayload) == 16, "EventCuePayload must be 16 bytes");

// InventoryPickupPayload -- one inventory-collect blip (InventoryPickup=47, v58). The
// collector broadcasts its own world position at the moment the native inventory_Cue
// PlaySound2D fired (see ReliableKind::InventoryPickup); every other peer plays the cue
// spatialized there. Position-on-the-wire (the firefly shape) rather than puppet-resolved
// at the receiver: immune to puppet-not-yet-spawned races and interp lag.
struct InventoryPickupPayload {
    float x;  // the collector's world location at collect time
    float y;
    float z;
};
static_assert(sizeof(InventoryPickupPayload) == 12, "InventoryPickupPayload must be 12 bytes");

// ChatMessagePayload -- one T-chat line (ChatMessage=48, v60). TEXT ONLY: the sender's
// identity comes from the transport's senderPeerSlot (nickname via player_handshake),
// never from the payload -- a peer cannot speak as someone else. UTF-8, len-prefixed,
// NOT NUL-terminated; receiver clamps len and sanitizes non-printable bytes before
// display (chat_feed stores ASCII; multibyte renders as '?').
struct ChatMessagePayload {
    uint8_t len;        // bytes used in text[] (0 < len <= sizeof(text))
    char    text[203];  // the line, UTF-8
};
static_assert(sizeof(ChatMessagePayload) == 204, "ChatMessagePayload must be 204 bytes");
static_assert(sizeof(ChatMessagePayload) <= 256 - 20 - 8,
              "ChatMessagePayload must fit in one reliable datagram");

// TurbineStatePayload -- one wind turbine's driver state (TurbineState=49, v61).
// HOST->client ~1 Hz per turbine. The six floats are the turbine BP's spring/
// integrator INPUTS (RE doc votv-wind-turbines-RE-2026-06-11.md section 1): the
// receiver writes them raw and the turbine's OWN tick does everything else (the
// head spring is the interpolator; rot is the unbounded servo integrator whose
// 0-reset on world load is the main divergence source). Identity = quantized
// world position (one baked pair shares a save Key).
struct TurbineStatePayload {
    WireKey key;            // 32 -- "t_<qx>_<qy>_<qz>" quantized world position
    float   headRotation;   // @0x0300 the facing (world yaw deg; spring output)
    float   targetRot;      // @0x030C spring target
    float   rot;            // @0x0340 servo integrator (unbounded deg, raw)
    float   alphaBlades;    // @0x02F8 blade spin phase (deg accumulator)
    float   bladesMomentum; // @0x0334 blade spring output (spin rate)
    float   mult;           // @0x0328 per-instance BeginPlay rand(0.9,1.0) rate skew
};
static_assert(sizeof(TurbineStatePayload) == 56, "TurbineStatePayload must be 56 bytes");

// propconvert_kind -- the morph edge a PropConvert re-skins (v81 MORPH V2).
namespace propconvert_kind {
inline constexpr uint8_t kToClump = 0;  // pile-A -> clump (grab): spawn a kinematic clump, drive by pose
inline constexpr uint8_t kToPile  = 1;  // clump -> pile-B (land): spawn a settled, grabbable pile
}  // namespace propconvert_kind

// PropConvertPayload -- the bind-model pile MORPH re-skin (PropConvert=41, v81 MORPH V2). REPLACES the
// v52 fresh-eid death-watch model. On the bind model BOTH peers own the same pile bound to the shared
// host-minted eid `E` (RegisterPropMirror / local tracker Element). The morph re-skins E across the
// three UObjects pile-A -> clump -> pile-B: `oldEid == newEid == E` on EVERY edge, so the receiver
// re-points its single rendering of E (spawn-new -> rebind -> echo-destroy-old) instead of creating a
// second cross-peer entity. `kind` selects the edge (ToClump on grab, ToPile on land). The owner emits
// it from the PROVEN held-object channel (grab: new-held edge; land: bound-clump death) -- never from
// the un-hookable clump Init-POST. The host applies a CLIENT's convert against its OWN local element E
// (host-authority via the host-minted eid, no request/relay). docs/piles/07-MORPH-V2-held-object-channel.md.
struct PropConvertPayload {
    uint32_t      oldEid;                 // == E (the bound pile/clump being re-skinned)
    uint32_t      newEid;                 // == E (SAME id on the bind model; identity is preserved)
    WireClassName pileClass;              // ToPile: the chipPile leaf class; ToClump: the clump leaf class
    float locX, locY, locZ;               // resting/grab transform of the new rendering
    float rotPitch, rotYaw, rotRoll;
    float scaleX, scaleY, scaleZ;         // v83: the host's real GetActorScale3D(newActor), PER FORM (a clump
                                          //      and a pile scale differ). The trash PROXY is an AStaticMeshActor
                                          //      WE own; without this it kept the default scale and rendered
                                          //      SMALLER than the host's pile/clump. Applied on every convert
                                          //      (OnConvert ReskinProxy/spawn) + the join PropSpawn proxy.
    uint8_t chipType;                     // the trash variant (carried across both edges)
    uint8_t kind;                         // propconvert_kind:: kToClump (grab) | kToPile (land)  [was _pad[0]]
    uint8_t ctx;                          // v82: MTA sync-time-context -- the host's per-eid generation counter
                                          //      (trash_channel), bumped on EVERY trash-entity transition. The
                                          //      receiver adopts it (host-authoritative) + drops a later PropPose/
                                          //      PropConvert for this eid whose ctx is older -> a stale carry/land
                                          //      packet can never re-apply to the re-skinned entity (the cluster
                                          //      mis-bind guard the morph lacked). [was _pad[0]]
    uint8_t hasMatchPos;                  // v89 (docs/piles/09, the 4th mirror-identity instance): 1 => matchX/Y/Z
                                          //      carry this pile's PRE-GRAB save-time position. ONLY a kToPile LAND
                                          //      stamps it, and ONLY when the host self-seeded the eid at an
                                          //      in-window grab (OnPileGrabPre) + recorded the pre-grab pos into the
                                          //      blob map. The client's OnConvert then arms a pending save-time twin
                                          //      so the bracket-independent quiescence sweep retires its stale
                                          //      native@old (the L1 mechanism, extended to the in-window MOVE). [was _pad[0]]
    float   matchX, matchY, matchZ;       // v89: the pre-grab position (world cm); valid IFF hasMatchPos && kToPile.
};
static_assert(sizeof(PropConvertPayload) == 124, "PropConvertPayload must be 124 bytes (v89: +hasMatchPos +matchX/Y/Z)");

// GrabIntentPayload (GrabIntent=78, v84) -- CLIENT->HOST chipPile grab REQUEST (Increment 2,
// docs/piles/08; the door OnRequest shape). The client sends ONLY the eid of the mirrored pile it
// wants to grab; the host validates (PILED && !HELD), executes playerGrabbed on puppet-N, and
// broadcasts the authoritative PropConvert{kToClump}. No state push from the client -- intent only
// (MTA CStaticFunctionDefinitions::AttachElements :1602). Host gates senderPeerSlot!=0 + role==Host.
struct GrabIntentPayload {
    uint32_t eid;        // the trash entity eid the client requests to grab
    uint8_t  _pad[4];    // 8-byte alignment; bytes-beyond-eid zero
};
static_assert(sizeof(GrabIntentPayload) == 8, "GrabIntentPayload must be 8 bytes");
static_assert(sizeof(GrabIntentPayload) <= 256 - 20 - 8, "GrabIntentPayload must fit one datagram");

// ThrowIntentPayload (ThrowIntent=79, v85; +mode/camFwd 2026-06-26) -- the client-initiated release/throw
// of a puppet-held clump. CLIENT->HOST. TWO native mechanics, distinguished by `mode`:
//   mode=kRelease (E)    -- the native E "drop": the host derives the launch from the puppet's smoothed
//                          hand motion (HandVelocityForEid, capped) -- a still hold drops soft, a flick
//                          flies. dir is UNUSED (zero). This is the original v85 behavior (#3 grab/release).
//   mode=kHardThrow (LMB)-- the native throwHoldingProp: a deliberate camera-DIRECTED launch at the native
//                          mass-scaled speed (RE: cameraFwd * 15000/max(mass,10) + playerVel, NO cap). The
//                          throw flies exactly where the client looks AT THE PRESS, so the client sends its
//                          instantaneous camera-forward unit vector (`dir`); the host applies the native
//                          formula with the REAL clump mass (it holds the clump) + the puppet velocity.
// The host releases the puppet grab + applies physics velocity; the clump self-re-piles via its own
// ground-hit ubergraph either way.
namespace throw_mode { constexpr uint8_t kRelease = 0; constexpr uint8_t kHardThrow = 1; }
struct ThrowIntentPayload {
    uint32_t eid;        // the trash entity eid the client requests to throw (must be the one it holds)
    uint8_t  mode;       // throw_mode::kRelease (E) | kHardThrow (LMB)
    uint8_t  _pad[3];    // align dir to 4; bytes zero
    float    dirX, dirY, dirZ;  // kHardThrow ONLY: client camera-forward unit vector at the press (zero for kRelease)
};
static_assert(sizeof(ThrowIntentPayload) == 20, "ThrowIntentPayload must be 20 bytes (eid+mode+pad+dir)");
static_assert(sizeof(ThrowIntentPayload) <= 256 - 20 - 8, "ThrowIntentPayload must fit one datagram");

// PileResyncRequestPayload (PileResyncRequest=80, v84 STAGED -- ID reserved, no handler yet) --
// CLIENT->HOST drain-survive (MTA EntityAdd-on-rescope): on a shadow-drain the client asks the host
// to re-stream PropSpawn per live pile (host eid preserved). No body; the senderPeerSlot identifies
// whom to re-stream to.
struct PileResyncRequestPayload {
    uint8_t _pad[8];     // no payload body; kept 8 bytes for a uniform minimum datagram
};
static_assert(sizeof(PileResyncRequestPayload) == 8, "PileResyncRequestPayload must be 8 bytes");

// PropSnapPosPayload (PropSnapPos=81, v90, b3) -- HOST->ONE-JOINER position correction for a
// save-authoritative chipPile the host moved during that joiner's connect window (its PropConvert was
// dropped by the joiner's pre-world reliable gate, and chipPiles carry no position in the connect-snapshot).
// The host sends the eid + its CURRENT authoritative transform; the client snaps the bound native to it at
// the quiescence sweep. Identity is preserved (the native stays a native -- this is a position delta on the
// existing save-authoritative identity, NOT a re-spawn). Idempotent (safe for an eid already corrected by a
// delivered convert -> drift 0). research/findings/piles-trash/coop-b3-window-moved-pile-position-DESIGN-2026-06-26.md.
struct PropSnapPosPayload {
    uint32_t eid;                       // the save-authoritative pile eid to reposition
    float    locX, locY, locZ;          // host's CURRENT authoritative world position (cm)
    float    rotPitch, rotYaw, rotRoll; // host's CURRENT authoritative rotation (deg)
};
static_assert(sizeof(PropSnapPosPayload) == 28, "PropSnapPosPayload must be 28 bytes (eid + loc + rot)");
static_assert(sizeof(PropSnapPosPayload) <= 256 - 20 - 8, "PropSnapPosPayload must fit one datagram");

// TimeSyncPayload -- the host-authoritative WORLD CLOCK (TimeSync=29, v36). The cycle's
// totalTime/Day/TimeScale (AdaynightCycle_C) are not otherwise replicated, so a fresh joiner
// free-runs its own clock -> a dark client world. The host broadcasts this periodically + on
// connect; the client direct-writes the three floats (its own ReceiveTick re-derives the sun).
// Writing timeScale lets the client free-run smoothly between pushes. RE: research/findings/
// votv-coop-class-clone-migration-roadmap-2026-06-06.md §2.
struct TimeSyncPayload {
    float totalTime;   // within-day clock [0, MaxTime) -- the sun/moon derive from this every tick
    float day;         // the within-day ACCUMULATOR (midnight cascade threshold), NOT the day number
    float timeScale;   // clock advance rate (so the client advances at the host's rate)
    // v96: the NAMED clock triple (daynightCycle.timeZ) -- the day NUMBER lives in timeZ.Z and the
    // HUD clock reads timeZ. A TimeScale=0 client never runs its own minute pulse, so without these
    // its displayed clock/day stayed frozen at the transferred save's values (user 2026-07-03:
    // set-time "не работает нормально").
    int32_t hour;
    int32_t minute;
    int32_t dayZ;
};
static_assert(sizeof(TimeSyncPayload) == 24, "TimeSyncPayload must be 24 bytes");

// v109 (2026-07-15, design F): the unreliable HOST->all world-clock stream datagram
// (MsgType::ClockPose=37). Wraps the SAME TimeSyncPayload the reliable TimeSync(29) connect-edge
// carries -- the ONLY difference is the transport: this periodic snapshot rides the unreliable
// pose channel (newest-wins by header seq), so the frozen client mirror is refreshed at HH:MM
// display granularity without loading the reliable ARQ. See MsgType::ClockPose.
struct ClockPosePacket {
    PacketHeader    header;  // 20
    TimeSyncPayload clock;   // 24
};
static_assert(sizeof(ClockPosePacket) == 44, "ClockPosePacket must be 44 bytes");

// v111 (2026-07-15): the unreliable HOST->all signal-desk download-SIM output vector
// (MsgType::DeskSimPose=38). The host-authoritative replacement for the per-peer download sim:
// decoded/needle/rate/frData/poData/offsets/cooldown are host-owned + streamed newest-wins; the
// client interpolates + OVERWRITES its own (its local sim self-accrues garbage the overwrite hides).
// See MsgType::DeskSimPose + coop/interactables/desk_sim_sync.
struct DeskSimSnapshot {
    float decoded;    // 4 -- DL_SignalDownloadDLData.decoded (progress)
    float resDetec;   // 4 -- DL_resDetecPercent (needle)
    float rate;       // 4 -- DL_downloading (0 = idle)
    float frData;     // 4 -- DL_frData (freq-match)
    float poData;     // 4 -- DL_poData (polarity-match)
    float frOffset;   // 4 -- DL_FrFilterOffset (knob position)
    float poOffset;   // 4 -- DL_poFilterOffset
    // v112: coord_cooldown REMOVED from the stream (RULE 2) -- the 10 Hz host
    // overwrite erased a client presser's charge (BUGS-v111 bug 1). It is now
    // DeskInput charge-event-driven + natively dt-decayed per peer.
};
static_assert(sizeof(DeskSimSnapshot) == 28, "DeskSimSnapshot must be 28 bytes (v112: cooldown out)");

struct DeskSimPosePacket {
    PacketHeader   header;  // 20
    DeskSimSnapshot sim;    // 28
};
static_assert(sizeof(DeskSimPosePacket) == 48, "DeskSimPosePacket must be 48 bytes (v112)");

// v112 (2026-07-16): the claim-free field-granular desk INPUT delta (DeskInput=97).
// Exactly ONE input-class field per message; the receiver applies it through the
// field's native side-effect path and primes its own poll baseline for that field.
// See coop/interactables/desk_input_sync.
enum class DeskInputField : uint8_t {
    FrFilterSpeed = 0,   // float   DL_FrFilterSpeed
    PoFilterSpeed = 1,   // float   DL_poFilterSpeed
    FrFilterActive = 2,  // bool    DL_activeFrFilter
    PoFilterActive = 3,  // bool    DL_activePoFilter
    PolarityDir = 4,     // int32   DL_PolarityDir
    PlayVolume = 5,      // int32   play_volume (+ live signalSound.SetVolumeMultiplier)
    PlaySelectIndex = 6, // int32   play_selectIndex
    CompMaxLevel = 7,    // int32   comp_maxLevel
    ActivePlay = 8,      // bool    active_play    (+ hum/light side effects)
    ActiveDownload = 9,  // bool    active_download (+ hum/light side effects)
    ActiveCoords = 10,   // bool    active_coords  (+ hum/light side effects)
    ActiveComp = 11,     // bool    active_comp    (+ light/console-glow side effects)
    CoordIsPing = 12,    // bool    coord_isPing edge NOTIFICATION (rising = presser's ENTER;
                         //         falling = its FSM end). v115b: receivers NEVER write it
                         //         into the machine -- it is the ping FSM's run-flag (latent
                         //         tick machine @82980; the raw apply woke a phantom sim on
                         //         every observer). Bookkeeping only: host ping attribution
                         //         + the device_occupancy desk FSM-hold.
    CooldownCharge = 13, // float   coord_cooldown -- UPWARD jumps only (a press charge; decay is
                         //         per-peer local and never rides the wire)
    Count = 14,
};

struct DeskInputPayload {
    uint8_t field;     // 1 -- DeskInputField
    uint8_t boolVal;   // 1 -- for bool fields (0/1)
    uint8_t _pad[2];   // 2
    float   floatVal;  // 4 -- for float fields
    int32_t intVal;    // 4 -- for int fields
};
static_assert(sizeof(DeskInputPayload) == 12, "DeskInputPayload must be 12 bytes");

// v112: the SHIFT quick-scan notification (DeskScanEvent=98). Carries the observed
// charge for the log line only; mirrors replay spawnDirs() (the VISUAL; the beep
// itself rides DeskSndFx since v115 -- RULE 2: one owner for desk one-shot audio).
struct DeskScanEventPayload {
    float observedCooldown;  // 4 -- the presser's post-charge cooldown (diagnostic)
};
static_assert(sizeof(DeskScanEventPayload) == 4, "DeskScanEventPayload must be 4 bytes");

// ---- v115: the desk audio-effect forward (DeskSndFx=105) ----
//
// The component index is a COMPILE-TIME wire contract (never discovery order):
// both peers map index <-> the same ObjectProperty name on AanalogDScreenTest_C
// via the static table in ue_wrap/desk_audio.cpp. Order below is frozen.
enum class DeskSndComp : uint8_t {
    KeyPress    = 0,  // audio_coordKeyPress    -- one-shot, every accepted key down/up
    CoordFail   = 1,  // audio_coordFail        -- one-shot, broken-radar fail
    ButtonSound = 2,  // audio_coordButtonSound -- one-shot channel (playButtonSound: SetSound+Play)
    PingSound   = 3,  // audio_coord_pingSound  -- one-shot channel (playPingSound: SetSound+Play)
    CorrdsLoop  = 4,  // corrds_loop            -- LOOP: cursor movement (spaceRenderer edge-guard)
    PingLoop    = 5,  // audio_coord_pingLoop   -- LOOP: the ping FSM loop
    Count       = 6,
};
inline constexpr int kDeskSndFirstLoop = 4;  // comps >= this are loops (state, join-re-asserted)

enum class DeskSndOp : uint8_t {
    Play    = 0,  // one-shot: mirror replays SetSound(cue)+Play(0) on the comp
    LoopOn  = 1,  // mirror replays SetActive(true, true)  (all native ON sites reset)
    LoopOff = 2,  // mirror replays SetActive(false, false) (bReset ignored on deactivate)
};

inline constexpr int kDeskSndCueCap = 40;  // longest measured cue name = 35 chars
                                           // (newdesk_panelCoord_pingChangeCursor) + NUL + slack

struct DeskSndFxPayload {
    uint8_t op;                  // 1 -- DeskSndOp
    uint8_t comp;                // 1 -- DeskSndComp
    uint8_t cueLen;              // 1 -- strlen(cue); 0 for loop ops
    uint8_t _pad;                // 1
    char    cue[kDeskSndCueCap]; // 40 -- ASCII cue object short name, NUL-padded
};
static_assert(sizeof(DeskSndFxPayload) == 44, "DeskSndFxPayload must be 44 bytes");

// v113 (L4 dishes): the host->all dish POSE stream (DishPose=39, unreliable,
// newest-wins by header seq). Movers-only rows at 4 Hz while any dish slews +
// full-24 settle-tail sweeps (1 Hz x3) after MovingCount hits 0. The pose frame
// is RELATIVE: yawZ = axis_Z.RelativeRotation.Yaw, rollY = axis_Y.RelativeRotation
// .Roll (the native loop's own channels -- impl-RE SS1). One applier with the
// DishSnapshot join row. See coop/interactables/dish_sync.
inline constexpr int32_t kMaxDishes = 24;

// Angles ride as unsigned centidegrees normalized to [0, 36000) -- 0.01 deg
// resolution against the loop's own 1.0 deg arrival tolerance; calibration as
// value*65535 (0..1). Keeps the full-24 packets inside kMaxPacketBytes /
// kMaxReliablePayload.
inline uint16_t QuantDeg(float deg) {
    if (!(deg > -1.0e6f && deg < 1.0e6f)) return 0;  // NaN/inf/absurd -> 0 (UB-safe int cast)
    float n = deg - 360.f * static_cast<float>(static_cast<int>(deg / 360.f));
    if (n < 0.f) n += 360.f;
    if (n >= 360.f) n = 0.f;  // float edge: -1e-5 + 360 rounds to 36000
    return static_cast<uint16_t>(n * 100.f + 0.5f);
}
inline float DequantDeg(uint16_t q) { return static_cast<float>(q) * 0.01f; }

struct DishPoseRow {
    uint8_t  index;      // 1 -- gamemode.dishs index
    uint8_t  isMoving;   // 1
    uint16_t yawCdeg;    // 2 -- axis_Z.RelativeRotation.Yaw, centidegrees
    uint16_t rollCdeg;   // 2 -- axis_Y.RelativeRotation.Roll, centidegrees
};
static_assert(sizeof(DishPoseRow) == 6, "DishPoseRow must be 6 bytes");

struct DishPoseBody {
    uint8_t     count;             // 1 -- used rows
    uint8_t     _pad[3];           // 3
    DishPoseRow rows[kMaxDishes];  // 144
};
static_assert(sizeof(DishPoseBody) == 148, "DishPoseBody must be 148 bytes");

struct DishPosePacket {
    PacketHeader header;  // 20
    DishPoseBody body;    // 148
};
static_assert(sizeof(DishPosePacket) == 168, "DishPosePacket must be 168 bytes");

// v113 (L4): the download-ARM state edge (DishArm=99), host-authored.
// armed=1 -> {decoded, polarity} valid (polarity = THE host roll); armed=0 ->
// disarm (receivers ResetDownloadMachine + deleteSignalActor).
struct DishArmPayload {
    uint8_t armed;      // 1
    uint8_t _pad[3];    // 3
    float   decoded;    // 4 -- arm-time initializer only (the 10 Hz sim stream is
                        //      the standing authority; staleness heals <=100 ms)
    int32_t polarity;   // 4 -- host-rolled (RNG_AUTHORITY T2-5d)
};
static_assert(sizeof(DishArmPayload) == 12, "DishArmPayload must be 12 bytes");

// v113 (L4): the joiner's full-24 dish seed (DishSnapshot=100, connect replay).
struct DishSnapshotRow {
    uint16_t yawCdeg;      // 2
    uint16_t rollCdeg;     // 2
    uint16_t calibQ;       // 2 -- calibration * 65535 (0..1)
    uint8_t  isMoving;     // 1
    uint8_t  activeDish;   // 1 -- gamemode.activeDishes[i]
};
static_assert(sizeof(DishSnapshotRow) == 8, "DishSnapshotRow must be 8 bytes");

struct DishSnapshotPayload {
    uint8_t         count;             // 1 -- used rows (dish i = rows[i])
    uint8_t         _pad[3];           // 3
    DishSnapshotRow rows[kMaxDishes];  // 192
};
static_assert(sizeof(DishSnapshotPayload) == 196, "DishSnapshotPayload must be 196 bytes");
static_assert(sizeof(DishSnapshotPayload) <= 256 - 20 - 8,
              "DishSnapshotPayload must fit in one reliable datagram");

// v113 (L4): the symmetric calibration batch (DishCalib=101) -- absolute values
// from the peer whose LOCAL values changed; host-relayed (total order).
struct DishCalibEntry {
    uint8_t  index;      // 1
    uint8_t  _pad;       // 1
    uint16_t valueQ;     // 2 -- calibration * 65535 (0..1)
};
static_assert(sizeof(DishCalibEntry) == 4, "DishCalibEntry must be 4 bytes");

struct DishCalibPayload {
    uint8_t        count;               // 1
    uint8_t        _pad[3];             // 3
    DishCalibEntry entries[kMaxDishes]; // 96
};
static_assert(sizeof(DishCalibPayload) == 100, "DishCalibPayload must be 100 bytes");
static_assert(sizeof(DishCalibPayload) <= 256 - 20 - 8,
              "DishCalibPayload must fit in one reliable datagram");

// --- v114 (L7): tape caddy + daily task --------------------------------------------------------
// Design of record: research/findings/computers-devices/votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md.

// ReelSlotPayload (ReliableKind::ReelSlot=102) -- ONE wallunit slot sentinel edge, presser-authored.
// reel: 0 = big, 1 = small. op: 0 = INSERT (progress valid), 1 = EJECT (progress = the last value,
// informational only -- the prop carries the truth via savedScalar). Detected by the 4 Hz sentinel
// poll (never the verbs); applied GT-atomically with a poll-baseline prime.
struct ReelSlotPayload {
    float   progress;   // 4 -- INSERT: the value entering the unit (0..100); EJECT: last value
    uint8_t reel;       // 1 -- 0 = reelBig @0x288, 1 = reelSmall @0x28C
    uint8_t op;         // 1 -- 0 = INSERT (-1 -> P), 1 = EJECT (P -> -1)
    uint8_t _pad[2];    // 2
};
static_assert(sizeof(ReelSlotPayload) == 8, "ReelSlotPayload must be 8 bytes");

// ReelPosePayload (MsgType::ReelPose=40, unreliable) -- the HOST's 1 Hz accrual corrector.
// Newest-wins by the PACKET HEADER seq (the DishPose shape). A channel value of -1.0 means "my
// slot is empty" and is NEVER applied (the receiver also skips any channel whose LOCAL slot is
// empty -- sentinel transitions belong to ReelSlot alone).
struct ReelPosePayload {
    float reelBig;   // 4
    float reelSmall; // 4
};
static_assert(sizeof(ReelPosePayload) == 8, "ReelPosePayload must be 8 bytes");

// The ReelPose datagram (header seq = the newest-wins guard).
struct ReelPosePacket {
    PacketHeader    header;  // 20
    ReelPosePayload body;    // 8
};
static_assert(sizeof(ReelPosePacket) == 28, "ReelPosePacket must be 28 bytes");

// TaskNewStatePayload (ReliableKind::TaskNewState=103) -- the HOST's saveSlot.taskNew mirror.
// Serialized field-by-field (the struct holds three engine TArrays -- never byte-copied). Counts
// are clamped AT SEND with a WARN (never silent); the receiver rejects counts over the caps.
// sigRequired/sigCompleted are indexed by process LEVEL (fixed MakeArray + Array_Set(processLvl));
// requiredDishes holds dish INDICES (Shuffle(gamemode.dishs) subset). i16 everywhere: measured
// values are tiny (counts < 100, indices < 24); caps carry headroom over the measured bounds.
inline constexpr uint8_t kTaskSigCap  = 24;  // measured: fixed MakeArray literal (<= enum-ish small)
inline constexpr uint8_t kTaskDishCap = 32;  // measured: <= gamemode.dishs.Num = 24 on the base map
struct TaskNewStatePayload {
    uint8_t active;                       // 1 -- taskNew.active
    uint8_t sigRequiredCount;             // 1
    uint8_t sigCompletedCount;            // 1
    uint8_t requiredDishesCount;          // 1
    int32_t rewardSig;                    // 4
    int32_t rewardSat;                    // 4
    float   reelBig;                      // 4 -- taskNew.reel_big (best-SENT; not the accruing pair)
    float   reelSmall;                    // 4 -- taskNew.reel_small
    int16_t sigRequired[kTaskSigCap];     // 48
    int16_t sigCompleted[kTaskSigCap];    // 48
    int16_t requiredDishes[kTaskDishCap]; // 64
};
static_assert(sizeof(TaskNewStatePayload) == 180, "TaskNewStatePayload must be 180 bytes");
static_assert(sizeof(TaskNewStatePayload) <= 256 - 20 - 8,
              "TaskNewStatePayload must fit in one reliable datagram");

// SkyStatePayload -- the host-authoritative NIGHT-SKY snapshot (SkyState=34, v44). The visible
// star dome (Anewsky_C's `sky` mesh) is given a per-game UNSEEDED random yaw + a slow per-tick
// spin, and moonPhase comes from the save, so both diverge per peer. The host pushes the sky
// mesh's WORLD rotation (carrying the random offset + accumulated spin) + moonPhase on a ~1Hz
// throttle + connect edge; the client writes them (SetComponentWorldRotation + moonPhase_mirror).
// Sun/moon ORBIT + brightness already converge via TimeSync(29). Modeled on TimeSyncPayload.
// RE: research/findings/weather-wind/votv-sky-stars-celestial-sync-RE-2026-06-08.md.
struct SkyStatePayload {
    float skyPitch;    // sky mesh WORLD rotation (FRotator) -- pitch
    float skyYaw;      //   yaw  (the dominant value: random initial offset + accumulated spin)
    float skyRoll;     //   roll
    float moonPhase;   // Anewsky_C::moonPhase_mirror (= UsaveSlot_C::moonPhase)
};
static_assert(sizeof(SkyStatePayload) == 16, "SkyStatePayload must be 16 bytes");

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
inline constexpr uint8_t kWindValid      = 0x08;  // v43: the 4 wind fields were validly read (host directionalWind live). The client applies wind ONLY when set, so an unread host (rare, mid-transition) never zeros the client's wind -- and calm (all-zero) wind still syncs because the bit, not the values, marks validity.
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
