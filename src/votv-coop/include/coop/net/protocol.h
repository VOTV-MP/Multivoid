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
// v6 (2026-05-26 PM): ItemActivatePayload grew 16 -> 24 bytes -- added float
// intensity + outerConeAngle + innerConeAngle + uint8 mode so the receiver can
// mirror the sender's EXACT cone shape (Phase 5F user feedback: puppet's
// flashlight was too bright AND focused-vs-spread mode wasn't synced).
// v3 (2026-05-23 PM): PoseSnapshot grew from 24 -> 28 bytes (added headYawDelta).
// Both peers must run v6; older packets are rejected at the header check. No
// back-compat layer (RULE 2 -- mod is pre-ship; bump cleanly).
inline constexpr uint16_t kProtocolVersion = 6;

// Default LAN port (overridable via votv-coop.ini "net.port=").
inline constexpr uint16_t kDefaultPort = 47621;

enum class MsgType : uint8_t {
    Hello = 1,         // handshake (either direction); no payload beyond the header
    PoseSnapshot = 2,  // a player pose (unreliable: freely dropped, newest wins)
    Bye = 3,           // graceful disconnect; no payload
    Reliable = 4,      // a reliable, ordered, ack'd message (chat / system events)
    ReliableAck = 5,   // acknowledges a Reliable message by its relSeq
    Ping = 6,          // RTT probe: payload = sender's local steady_clock millis (uint32)
    Pong = 7,          // RTT echo:  payload = the SAME millis we received in Ping
    PropPose = 8,      // v4: held-prop world transform (unreliable; sent per-frame
                       //     while host holds; receiver finds local prop by Key,
                       //     SetSimulatePhysics(false), drives transform per packet)
};

// Payload kinds carried inside a Reliable message. The chat/event-feed groundwork:
// Join announces a player (its nickname) so the peer can post "<nick> joined" and
// label the remote player. (Chat text is a future ReliableKind.)
enum class ReliableKind : uint8_t {
    Join = 1,         // payload: [uint8 len][len bytes UTF-8 nickname]
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
    ItemActivate = 12, // Phase 5F (2026-05-25 NIGHT-3): unified item-activation
                       //     state sync. First instance: flashlight on/off
                       //     (Case b per the RE doc -- the world light cone is
                       //     mainPlayer_C::light_R, NOT a component on the item
                       //     actor). itemClassHash distinguishes flashlight vs
                       //     radio vs torch etc.; peerSessionId identifies the
                       //     puppet whose state changes; actorKeyHash carries
                       //     the Aprop_C::Key hash for Case-a world props
                       //     (radio, torch, lamp) and is 0 for Case-b player-
                       //     equipment items. Reserved IDs 9-11 are queued for
                       //     Phase 5D DoorState/LightState/LockState (RE doc
                       //     landed earlier; impl pending).
                       //     Payload: ItemActivatePayload (24 bytes -- v6).
};

#pragma pack(push, 1)

// Every datagram starts with this. seq is per-sender, monotonically increasing
// (ordering + stale-drop on the receiver; never trust an older seq than the last).
// token is the session nonce: the host mints a random one at session start and
// hands it out in its Hello; thereafter EVERY packet must carry it or it is
// dropped. An off-path spoofer never sees the token, so it cannot inject
// pose/Bye into an established session (anti-hijack / anti-replay). A client's
// FIRST Hello (before it has learned the token) carries 0.
struct PacketHeader {
    uint32_t magic;    // kMagic
    uint16_t version;  // kProtocolVersion
    uint8_t  type;     // MsgType
    uint8_t  _pad;     // reserved
    uint32_t seq;      // per-sender sequence number
    uint64_t token;    // session nonce (0 == "not yet known", client's first Hello)
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
struct PoseSnapshot {
    float x, y, z;
    float yaw;
    float pitch;
    float headYawDelta;
    float speed;
};
static_assert(sizeof(PoseSnapshot) == 28, "PoseSnapshot must be 28 bytes");

struct PosePacket {
    PacketHeader header;
    PoseSnapshot pose;
};
static_assert(sizeof(PosePacket) == 48, "PosePacket must be 48 bytes");

// Ping/Pong: header + a single uint32_t payload (the sender's local steady-clock
// milliseconds, truncated to 32 bits -- ~49 days of monotonic range, far more
// than any session). Pong echoes the EXACT bytes from the Ping it answered;
// the original sender then computes RTT = (now32 - echoedMs).
struct PingPacket {
    PacketHeader header;
    uint32_t senderMs;
    uint32_t _pad;  // keep 8-byte alignment; reserved for future use
};
static_assert(sizeof(PingPacket) == 28, "PingPacket must be 28 bytes");

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
};
static_assert(sizeof(PropPoseSnapshot) == 56, "PropPoseSnapshot must be 56 bytes");

struct PropPosePacket {
    PacketHeader     header;  // 20
    PropPoseSnapshot pose;    // 56
};
static_assert(sizeof(PropPosePacket) == 76, "PropPosePacket must be 76 bytes");

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
    uint8_t       _pad[3];          // 4
    float         initLinVelX, initLinVelY, initLinVelZ;  // 12 -- usually (0,0,0)
    float         initAngVelX, initAngVelY, initAngVelZ;  // 12
};
static_assert(sizeof(PropSpawnPayload) == 160, "PropSpawnPayload must be 160 bytes");
static_assert(sizeof(PropSpawnPayload) <= 256 - 20 - 8,
              "PropSpawnPayload must fit in one reliable datagram");

namespace propspawn_flags {
inline constexpr uint8_t kSimulatePhysics = 0x01;
inline constexpr uint8_t kIsHeavy         = 0x02;
inline constexpr uint8_t kFrozen          = 0x04;
}  // namespace propspawn_flags

// v5 Phase 5S0 Inc2: prop-destroy reliable payload. WireKey identifies the
// prop on the receiver via prop_wrap::FindByKeyString -> K2_DestroyActor.
// Tiny (32 bytes) -- no transform/state needed; destruction is just "this
// Key's prop is gone". Sender's K2_DestroyActor PRE observer captures the
// Key just before the engine destroys the actor; the receiver's
// K2_DestroyActor call on its local actor is echo-suppressed via the
// incoming-destroy-set so it doesn't bounce back.
struct PropDestroyPayload {
    WireKey key;
};
static_assert(sizeof(PropDestroyPayload) == 32, "PropDestroyPayload must be 32 bytes");
static_assert(sizeof(PropDestroyPayload) <= 256 - 20 - 8,
              "PropDestroyPayload must fit in one reliable datagram");

// Phase 5N1 Inc2 (2026-05-25): NPC spawn reliable payload. Host detects an
// NPC instantiation via the host-side PRE observer on
// BeginDeferredActorSpawnFromClass (sibling of the existing client-side
// interceptor from Inc1; same UFunction, different role gate). Payload
// carries the class name + a host-assigned monotonic sessionId + the
// spawn world transform. Receiver (Inc3 -- pipeline scaffolded but not
// yet wired):
//   MarkIncomingNpcSpawn(cls)  // bypass the suppressor for this one call
//   actor = BeginDeferredActorSpawnFromClass(cls, transform)
//   FinishSpawningActor(actor, transform)  // BeginPlay runs naturally
//   g_npcMirrorBySessionId[sessionId] = actor
//
// sessionId is NOT a save-UUID (NPCs are runtime-spawned with no
// persistent identity). It's host-assigned monotonic counter, reset on
// session start. Receiver references the same ID for subsequent
// EntityPose (Inc3) and EntityDestroy.
struct EntitySpawnPayload {
    WireClassName className;       // 64 -- "npc_zombie_C", "kerfurOmega_mannequin_C", etc.
    uint32_t      sessionId;       // 4 -- host-assigned monotonic; 0 reserved for "invalid"
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
    uint32_t sessionId;
    uint32_t _pad;     // 8-byte alignment
};
static_assert(sizeof(EntityDestroyPayload) == 8, "EntityDestroyPayload must be 8 bytes");
// Audit-fix Issue 1 (2026-05-25): tripwire for future growth. At 8 bytes
// this trivially fits, but every reliable payload in this file has the
// guard; absence breaks the established consistency.
static_assert(sizeof(EntityDestroyPayload) <= 256 - 20 - 8,
              "EntityDestroyPayload must fit in one reliable datagram (kMaxReliablePayload)");

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
    uint8_t  peerSessionId;   // sender peer id (host=0, joiners 1..)
    uint8_t  state;           // 0 = off / inactive, 1 = on / active
    uint8_t  flags;           // bit0: has_actor_key (1 = use actorKeyHash)
    uint8_t  mode;            // v6: mp.flashlightMode (0=default spread, 1=focused, ...)
    uint32_t actorKeyHash;    // CRC32(Aprop_C::Key string) when flags.has_actor_key=1; 0 otherwise
    float    intensity;       // v6: light_R.Intensity AFTER BP ran (Unitless scale ~0..10)
    float    outerConeAngle;  // v6: light_R.OuterConeAngle (degrees; ~40 default, ~12 focused)
    float    innerConeAngle;  // v6: light_R.InnerConeAngle (degrees; ~0 default, varies)
};
static_assert(sizeof(ItemActivatePayload) == 24,
              "ItemActivatePayload must be exactly 24 bytes (v6 wire-format)");
static_assert(sizeof(ItemActivatePayload) <= 256 - 20 - 8,
              "ItemActivatePayload must fit in one reliable datagram");

// flags bits for ItemActivatePayload.flags
inline constexpr uint8_t kItemActivateFlag_HasActorKey = 0x01;

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

// Fill a header in-place.
inline void WriteHeader(PacketHeader& h, MsgType type, uint32_t seq, uint64_t token) {
    h.magic = kMagic;
    h.version = kProtocolVersion;
    h.type = static_cast<uint8_t>(type);
    h._pad = 0;
    h.seq = seq;
    h.token = token;
}

// Validate a received buffer as one of ours: enough bytes + magic + version.
// Returns the parsed header fields and true if the header is well-formed.
inline bool ParseHeader(const void* data, int len, MsgType& outType, uint32_t& outSeq,
                        uint64_t& outToken) {
    if (len < static_cast<int>(sizeof(PacketHeader))) return false;
    PacketHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic || h.version != kProtocolVersion) return false;
    outType = static_cast<MsgType>(h.type);
    outSeq = h.seq;
    outToken = h.token;
    return true;
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
