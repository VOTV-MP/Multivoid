// coop/net/protocol.h -- the wire format.
//
// Sits above the transport and below the session: packed little-endian POD structs and their
// (de)serialisation, nothing about sockets and nothing about the engine. Both peers are x86-64
// Windows, so structs go raw; the magic and the protocol version guard a mismatch.
//
// Rules of the file. kProtocolVersion is the build number of the version pair and bumps on
// every change a peer would parse differently and on every release. A retired wire value is
// never reused. Every reliable payload fits one datagram (kMaxReliablePayload) unless the
// session diverts the kind to the bulk sink. The direction, the trust and the late-join answer
// of each kind are stated at the kind; which lane it rides, whether the host relays a client's
// copy and whether it may be sent before the joiner's world exists are the tables in
// session_lanes.h. The subsystem pages under docs/ carry the why.

#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop::net {

// Magic guard: rejects a stray datagram that hits our port. The spelling is "VMTP".
inline constexpr uint32_t kMagic = 0x564D5450u;

// The build number of the version pair (game target + build). Two peers must carry the same
// value to share a lobby; the check is byte equality per lobby, an older cohort keeps playing
// among itself. Bumped on every wire change and on every release.
inline constexpr uint16_t kProtocolVersion = 152;

// Default LAN port (overridable via multivoid.ini "net.port=").
inline constexpr uint16_t kDefaultPort = 47621;

// What the direct-connect box starts with. It must name kDefaultPort: a prefilled address is the
// only port most players ever see, and replacing the address while keeping the port is the natural
// edit, so the static_assert couples the string to the constant.
inline constexpr const char* kDefaultDirectAddr = "127.0.0.1:47621";
static_assert(kDefaultPort == 47621,
              "kDefaultDirectAddr spells kDefaultPort out -- update both, or the "
              "direct-connect box teaches a port nothing listens on");

// The official public endpoints: connection endpoints, not secrets (the signaling token and the
// TURN credentials are never compiled in). One definition, so the config resolver and the display
// mask share it: a user-visible surface shows DEFAULT instead of the address when the master
// equals this. The URL grammar is schemeless = secure: a bare host:port means TLS, and only an
// explicit http:// or tcp:// opts a self-hoster down to cleartext. The signaling constant names
// the plaintext port: it only seeds the master-down fallback, and the master's own answer is what
// a session dials. The root domain is proxied and must never be used here; the proxy does not
// pass custom ports.
inline constexpr const char* kOfficialMasterUrl    = "master.multivoid.dev:10443";
inline constexpr const char* kOfficialSignalingUrl = "master.multivoid.dev:10000";

// Where a person fetches a newer build: one constant for every update-available surface.
inline constexpr const char* kReleasesUrl = "github.com/VOTV-MP/Multivoid/releases";

// Datagram types. The pose kinds are unreliable and newest-wins by header seq; Reliable wraps a
// ReliableKind. The values are stable and a retired value is never reused.
enum class MsgType : uint8_t {
    // A player's pose; unreliable, newest wins. PosePacket.
    PoseSnapshot = 2,

    // An ordered, delivered message wrapping a ReliableKind payload.
    Reliable = 4,

    // A held prop's world transform; unreliable, per frame while held. PropPosePacket.
    PropPose = 8,

    // A ragdolling player's pelvis transform and velocity; unreliable, per frame while
    // ragdolled. RagdollPosePacket.
    RagdollPose = 16,

    // Host to all: a batch of character poses keyed by element id; unreliable, newest wins.
    // EntityPoseBatchHeader plus N EntityPoseSnapshot.
    EntityPose = 32,

    // Host to all: a batch of world-actor transforms with full rotation (ships bank and roll).
    // EntityPoseBatchHeader plus N WorldActorPoseSnapshot.
    WorldActorPose = 33,

    // Host to all, host-originated so the grabbing client sees its own clump move: carried and
    // flying trash clumps keyed by eid, gated by the carry generation. EntityPoseBatchHeader plus
    // N TrashClumpPoseSnapshot.
    TrashCarryPose = 34,

    // The held hand item's view-relative transform; unreliable while holding. HandPosePacket.
    HandPose = 35,

    // The desk's live cursor; unreliable while the desk is claimed and the cursor moves.
    // DeskCursorPosePacket.
    DeskCursorPose = 36,

    // Host to all: the world clock, about twice a second, newest wins. ClockPosePacket.
    ClockPose = 37,

    // Host to all: the desk download simulation's outputs, about 10 Hz. DeskSimPosePacket.
    DeskSimPose = 38,

    // Host to all: moving dish rows at 4 Hz, then a settle tail. DishPosePacket.
    DishPose = 39,

    // Host to all: the wall unit's reel accruals at 1 Hz while a slot is occupied. ReelPosePacket.
    ReelPose = 40,

    // One 20 ms Opus frame: a stream, not a state. The receiver queues every arrival per sender
    // and the payload's own seq orders the jitter buffer, so the newest-wins drop of the pose kinds
    // does not apply. The host relays it to every other ready slot. VoiceFramePayload.
    VoiceFrame = 64,
};

// Payload kinds carried inside a Reliable message. A retired value is never reused: 16, 17, 21,
// 22 and 24 stay unassigned; 32 and 128 are reserved.
enum class ReliableKind : uint8_t {
    // Each peer to the other, once after admission: the sender's Player element id, then the nick,
    // the skin, the display flags, the nick colour and the game target, parsed field by field. The
    // receiver compares the game target with its own first and refuses the connection on a mismatch,
    // before any identity side effect; then it establishes the mirror for the slot and names the puppet.
    Join = 1,

    // The holder released a held prop: the prop by key, the inherited linear and angular velocity,
    // and for a keyless trash entity the eid and its generation. The receiver re-enables physics,
    // writes the velocities and fires the prop's own thrown event above kThrownLinVelThreshold.
    // PropReleasePayload.
    PropRelease = 2,

    // A prop was born: an inventory drop, a spawner, a container extract, or a save-loaded prop in
    // the connect snapshot. The receiver spawns deferred, writes the key and the parity fields before
    // FinishSpawningActor so init() constructs the true prop, then registers the mirror. The host
    // authors every keyed prop; a client authors only the keyless trash families. PropSpawnPayload.
    PropSpawn = 3,

    // A prop died: the key plus the sender's element id (a keyless entity resolves by eid). The
    // receiver destroys its actor and drains the mirror binding; the destroy it runs is echo-suppressed.
    // PropDestroyPayload.
    PropDestroy = 4,

    // Host to all: a character of an allowlisted class exists. A client fresh-spawns a mirror, or
    // adopts its own save-loaded twin by class when the flag says the host's copy came from the save.
    // EntitySpawnPayload.
    EntitySpawn = 5,

    // Host to all: the character with this element id is gone. EntityDestroyPayload.
    EntityDestroy = 6,

    // Host to all, a dev key: every peer maxes out its own food, sleep and health. No payload.
    RestoreVitals = 7,

    // Host to one client: teleport your player to this pose; the join placement and a dev key.
    // TeleportClientPayload.
    TeleportClient = 8,

    // Host to all: a base door's open state, keyed by the door's Key. Doors auto-revert through
    // their own sensors, so the host is the single syncer: it polls, broadcasts changes and sends a
    // full snapshot to a joiner; a client renders the state with autoclose suppressed and sends
    // DoorOpenRequest for its own presses. KeyedTogglePayload.
    DoorState = 9,

    // Any peer, relayed by the host: a light switch's own toggle bit, keyed by the switch. The
    // lamps it drives are gated by their group root and ride LightGroupState. KeyedTogglePayload.
    LightState = 10,

    // Any peer, relayed by the host: a swinger lid (cabinet, fridge, safe) open or closed, keyed
    // by the prop's key. KeyedTogglePayload.
    ContainerState = 11,

    // Host to all: the weather scheduler's state. The client suppresses its own five scheduler
    // functions on the day-night cycle and applies the host's state through the cycle's own mutators;
    // the host sends on change and on a connect edge. WeatherStatePayload.
    WeatherState = 13,

    // Host to all: the red-sky story visual toggled; the client runs the same gamemode calls.
    // RedSkyPayload.
    RedSky = 15,

    // Host to all: a lightning strike at this location; the client spawns the strike actor there.
    // LightningStrikePayload.
    LightningStrike = 14,

    // Any peer: an equipment item's world effect changed, the flashlight first. Names the item
    // class, the sender's element id, the state and the light cone; the receiver applies it to that
    // peer's puppet. ItemActivatePayload.
    ItemActivate = 12,

    // Host to one client, right after the admission exchange: your slot and the host's Player
    // element id, which the client mirrors in slot 0. Its arrival is the admission signal.
    // AssignPeerSlotPayload.
    AssignPeerSlot = 18,

    // Host to all: the current occupant of one slot, as state, re-sent by a repair pulse; it may
    // describe slot 0 and the receiver's own slot. Parsed field by field: the slot, the player number,
    // the element id, the link kind and ping, the nick, the skin, the display flags and the colour. A
    // player number of 0 means the slot is empty (that is how a departure arrives); a changed number
    // is a replacement, and the receiver drains the outgoing occupant first.
    RosterRow = 19,

    // Host to one client: an enemy on the host hit your puppet; apply this damage to your own
    // player, whose own armor mitigates it. Never relayed; the receiver requires slot 0 as sender.
    // PlayerDamagePayload.
    PlayerDamage = 20,

    // Host to all: the shared balance, absolute. The host polls its own points and sends on change
    // and on a connect edge; a client writes the value directly. BalancePayload. Value 24 was the
    // client-to-host delta and stays retired: no client authors the economy.
    BalanceSync = 23,

    // Any peer, relayed by the host: one keypad's typed buffer, LED selector and short-code event,
    // keyed by the keypad's Key. The sender polls; the receiver replays inputNumber per digit and
    // runs the keypad's own Open chain for a stamped event. KeypadSyncPayload.
    KeypadState = 25,

    // Client to host: my player opened or closed this door. The host applies it with its own lock
    // and jam guards, and its poll broadcasts the result as DoorState. KeyedTogglePayload; never
    // relayed.
    DoorOpenRequest = 26,

    // Host to one client: the connect snapshot starts, with the prop count as the progress
    // denominator. Rides the Bulk lane ahead of every PropSpawn it introduces. SnapshotBeginPayload.
    SnapshotBegin = 27,

    // Host to one client: the last PropSpawn of the snapshot has been sent; the client lifts its
    // loading cover. Same lane, so it lands after the props. SnapshotEndPayload.
    SnapshotComplete = 28,

    // Host to one client at the connect edge: the world clock. The periodic clock rides ClockPose.
    // TimeSyncPayload.
    TimeSync = 29,

    // Any peer, relayed by the host: a base window's dirt scalar decreased (a wipe), keyed by the
    // window's Key. The receiver applies the minimum of local and wire; a connect snapshot applies
    // the host's value as is. KeyedScalarPayload.
    WindowCleanState = 30,

    // Any peer, relayed by the host: a grime decal's process scalar decreased, keyed by the decal's
    // quantized world position (a static decal's position is its identity). Minimum wins, like the
    // window. A decal's final removal has no lane yet. KeyedScalarPayload.
    GrimeState = 31,

    // Any peer, relayed by the host: the garage door's open state, keyed by the garage's
    // level-export name (its save key can be None after a reload). KeyedTogglePayload.
    GarageDoorState = 33,

    // Host to all: the star dome's world rotation and the moon phase, about once a second and on a
    // connect edge. SkyStatePayload.
    SkyState = 34,

    // Any peer, relayed by the host: a simple on/off appliance (faucet, sink, shower, oven, server
    // box, wall-unit tapes), keyed by the actor's Key; the adapter in ue_wrap/appliance maps the class
    // to its field and refresh verb. KeyedTogglePayload.
    ApplianceState = 35,

    // Any peer, relayed by the host: the power panel's five breakers as a bitmask, keyed by the
    // panel's Key. PowerPanelPayload.
    PowerControlState = 36,

    // The ATV's pose, velocity and condition from its author: the seated driver or the grabbing
    // hand, or the host at 5 Hz for an idle ATV that moved. A receiver keeps simulating and is
    // corrected toward the wire; presence fields (tires, spare) are taken from host-authored packets
    // only. Keyed by the ATV's Key; relayed; the host snapshots pose and velocity to a joiner.
    // AtvStatePayload.
    AtvState = 37,

    // Host to all: the delivery drone's transform, activity bits and dust anchor, about 20 Hz while
    // flying. The client suppresses its own drone tick and drives the transform. DroneStatePayload.
    DroneState = 38,

    // Client to host: a laptop shop order, as list_store row names only. The host prices it from
    // its own table, checks its own balance and commits it through the native order call. Chunked:
    // OrderRequestHeader plus packed items. Never relayed.
    OrderRequest = 39,

    // Any peer, relayed by the host: my firefly spawner spawned an emitter here; every other peer
    // spawns one too. FireflySpawnPayload.
    FireflySpawn = 40,

    // The trash entity with this eid changed form: pile to clump on a grab, clump to pile on a
    // landing. The receiver re-skins its single rendering of the eid in place. PropConvertPayload.
    PropConvert = 41,

    // Client to host, once, from the menu: send me your world save. No payload.
    SaveTransferRequest = 42,

    // Host to one client: the save transfer header (size, chunk count, CRC, game mode, sidecar
    // size). Bulk lane, ahead of its chunks. SaveTransferBeginPayload.
    SaveTransferBegin = 43,

    // Host to one client: one chunk of the save blob, a u32 index plus up to kSaveChunkBytes.
    // Larger than a datagram by design: the session diverts this kind to the bulk sink and the
    // transport fragments it; it never enters the reliable inbox.
    SaveTransferChunk = 44,

    // Client to host: my world is loaded. The only trigger of the host's connect replay. No payload.
    ClientWorldReady = 45,

    // Any peer, relayed by the host: a dispenser pile's two counters, keyed by the actor's Key. The
    // receiver applies the per-component minimum, or the host's values as is on a connect snapshot.
    // TrashPileStatePayload.
    TrashPileState = 46,

    // Any peer, relayed by the host: I collected an item here; other peers play the pickup cue at
    // that position. InventoryPickupPayload.
    InventoryPickup = 47,

    // Client to host only: a typed chat line. The host records it and answers with ChatSpeaker and
    // ChatLine. ChatMessagePayload.
    ChatMessage = 48,

    // Host to all, about once a second per turbine: the six driver floats of a wind turbine, keyed
    // by quantized position; the receiver writes them raw and the turbine's own tick does the rest.
    // TurbineStatePayload.
    TurbineState = 49,

    // Any peer, relayed by the host: a locker or drone-console hinged door, keyed by the actor's
    // level-export name. KeyedTogglePayload.
    LockerDoorState = 50,

    // Client to host as a claim or release; host to all with the arbitration result. One peer may
    // be inside an enterable screen device at a time; a losing claimant is told the winner's slot and
    // exits. Keyed by the shared-widget identity. DeviceClaimPayload.
    DeviceClaim = 51,

    // Host to all: the set of sky signals, in parts of up to three rows with a generation byte. A
    // client kills its own roller and mirrors the set. SkySignalStatePayload.
    SkySignalState = 52,

    // The signal-catch consume replay: kind 0 a catch (from the catcher, validated and rebroadcast
    // by the host), 1 a clear, 2 a connect seed. Carries the caught row and the dish slew vector;
    // receivers run the native consume chain. SkySignalCatchPayload.
    SkySignalCatch = 53,

    // Host to one client at the connect edge only: the desk's scalar snapshot with adopt set. Live
    // input rides DeskInput and the simulation rides DeskSimPose. DeskStatePayload.
    DeskState = 54,

    // From the desk occupant, relayed: the committed coordinate locks and the direction toggle,
    // change-gated and snapshotted on connect. The live cursor rides DeskCursorPose.
    // DishAimStatePayload.
    DishAimState = 55,

    // Any peer, relayed by the host: one new email as a chunked blob (BlobChunkPayload); the
    // receiver replays the gamemode's addEmail.
    EmailAppend = 56,

    // Any peer, relayed: an email deleted, named by the content hash of its append blob (wire
    // indexes differ per peer). ContentHashPayload.
    EmailDelete = 57,

    // Any peer, relayed: one saved-signal row appended, as a chunked blob without its image; the
    // receiver replays the gamemode's saveSignal.
    SavedSignalAppend = 58,

    // Any peer, relayed: a saved signal deleted by content hash. ContentHashPayload.
    SavedSignalDelete = 59,

    // From the peer whose refiner decode is running, about once a second, and from the host as a
    // connect snapshot: the decode pane's scalars. Mirrors render and never latch the decode
    // themselves. CompStatePayload.
    CompState = 60,

    // The refiner's loaded signal as a chunked blob, on change edges and at connect. BlobChunkPayload.
    CompData = 61,

    // Any peer, relayed: mic muted and voice disabled, display only. VoiceStatePayload.
    VoiceState = 62,

    // Client to host: turn this kerfur on or off. The client cancels its own menu dispatch; the
    // host runs the real verb and the result rides KerfurConvert. KerfurConvertPayload.
    KerfurConvertRequest = 63,

    // Any peer, relayed: a wall-attachable prop committed its stick at this pose; the receiver
    // re-poses and replays the prop's own forceStick. Unsticking rides the pose stream.
    // PropStickStatePayload.
    PropStickState = 64,

    // Any peer, relayed: one event line of the coordinates terminal, from the peer whose action
    // wrote it; the receiver appends it natively. DeskLogLinePayload.
    DeskLogLine = 65,

    // Both directions on one kind: op Report is a peer's in-bed edge toward the host; Tally,
    // Accelerate and End are host to all. SleepStatePayload.
    SleepState = 66,

    // Host to one client: the killer wisp grabbed your puppet; die for real after killDelayMs.
    // WispGrabPayload.
    WispGrab = 67,

    // Host to all: play the wisp's fatality tear on its mirror and attach the victim's puppet to it.
    // WispTearPayload.
    WispTear = 68,

    // Client to host: the peer's serialized inventory, on change; the host persists it under the
    // peer's guid. Host to client on join with the stored inventory. BlobChunkPayload on the Bulk
    // lane; never relayed.
    PlayerInventoryBlob = 69,

    // Client to host: a kerfur radial-menu verb for this kerfur. The host runs it; Follow follows
    // the requesting player's puppet, which the Blueprint cannot do for a remote player.
    // KerfurCommandPayload.
    KerfurCommand = 70,

    // From the ATV's author, relayed: I am no longer this ATV's author (a dismount or an ungrab,
    // not a yield). The receiver clears the author slot; the host becomes the idle syncer.
    // AtvReleasePayload.
    AtvRelease = 71,

    // Host to all: a runtime-spawned ATV exists under this synthetic key (its own save key is
    // random per peer); clients spawn a native idle ATV for it. AtvSpawnPayload.
    AtvSpawn = 72,

    // Host to all: the runtime ATV with this synthetic key is gone. AtvDestroyPayload.
    AtvDestroy = 73,

    // Host to all: a kerfur changed form. Carries the kerfur id, the old and new element ids, the
    // new form's class and pose, or a rejection. The initiator adopts its parked conversion ghost;
    // others spawn a fresh mirror. KerfurConvertBroadcastPayload.
    KerfurConvert = 74,

    // Host to all: a cosmetic emitter cue (a starfall) at this position; clients spawn the emitter.
    // EventCuePayload.
    EventCue = 75,

    // Host to all: a non-character event actor exists (a saucer, a ship, a coin, the pyramid).
    // WorldActorSpawnPayload, with the class-interpreted birth blob.
    WorldActorSpawn = 76,

    // Host to all: the world actor with this element id is gone. EntityDestroyPayload.
    WorldActorDestroy = 77,

    // Client to host: I want to grab this pile (by eid). The host validates, grabs it on my puppet
    // and broadcasts the PropConvert. GrabIntentPayload.
    GrabIntent = 78,

    // Client to host: release or hard-throw the clump my puppet holds; a hard throw carries my
    // camera direction and the host applies the native launch with the real mass.
    // ThrowIntentPayload.
    ThrowIntent = 79,

    // Client to host: reserved for a drain-survive resync. The id is taken; no handler exists yet.
    PileResyncRequest = 80,

    // Host to one joiner: a save-authoritative pile the host moved during your join window is here
    // now; snap your bound native to it at the quiescence sweep. PropSnapPosPayload.
    PropSnapPos = 81,

    // A player picked a body skin: [u8 slot][u8 nameLen][name]. Client to host with its own slot
    // (anything else is refused), then host to everyone else; host to all with slot 0 for its own.
    // The at-join skin rides Join and RosterRow. Pre-world sendable.
    SkinChange = 82,

    // A player toggled its own nameplate: [u8 slot][u8 visible]. The trust shape of SkinChange;
    // pre-world sendable.
    NameplateChange = 83,

    // Host to all: a scheduled or story event fired. Clients replay the native verb only for the
    // rows the per-row policy allows; rows another lane carries are never replayed. EventFirePayload.
    EventFire = 84,

    // Host to all: the pyramid committed a wisp gather; clients stage the target on their mirrors
    // and re-dispatch the game's own choreography. Re-sent to a joiner while a gather is in flight.
    // PyramidGatherPayload.
    PyramidGather = 85,

    // Host to one joiner at its world-ready edge: one in-flight event registry entry; the receiver
    // replays replay-safe rows with the active override. EventSnapshotPayload.
    EventSnapshot = 86,

    // Both directions: host to all on any observed alarm transition and to a joiner unconditionally;
    // client to host when its own scan toggled the alarm. Applied through the trigger's idempotent
    // runTrigger. AlarmStatePayload.
    AlarmState = 87,

    // A player picked a nickname colour: [u8 slot][u8 has][r][g][b]. The trust shape of SkinChange;
    // pre-world sendable.
    NickColorChange = 88,

    // What a player's hand shows: [u8 slot][u8 has][u8 clsLen][cls][u8 nameLen][name]. The trust
    // shape of SkinChange. Receivers keep a display-only mirror on the puppet; the host replays every
    // non-empty hand to a joiner.
    HandItem = 89,

    // Client to host: I placed this keyed prop I had picked up; spawn it by key at this transform
    // and broadcast it. Sent only after the pickup's destroy went out, so the host never holds two.
    // PropDropIntentPayload.
    PropDropIntent = 90,

    // Host to all, on change at about 1 Hz and to a joiner: the signal-server simulation (broken
    // mask and aggregates); the client writes the state and re-skins. ServerStatePayload.
    ServerState = 91,

    // Host to all: the roach infestation as a paged snapshot; the client applies by ordinal.
    // RoachStatePayload.
    RoachState = 92,

    // Client to host: a roach was eaten or stomped locally at this position; the host deletes its
    // nearest roach. RoachConsumedPayload.
    RoachConsumed = 93,

    // From the owning peer, relayed: my own stalker entity exists at this pose, keyed by (sender
    // slot, seq); re-sent as a keepalive, which also reaches a late joiner. OwnerEntitySpawnPayload.
    OwnerEntitySpawn = 94,

    // From the owner, relayed: the entity moved. OwnerEntityPosePayload.
    OwnerEntityPose = 95,

    // From the owner, relayed, or from the host on a leaver's behalf: the entity is gone (seq 0 =
    // all of that slot's). OwnerEntityDestroyPayload.
    OwnerEntityDestroy = 96,

    // From the presser, relayed to everyone but the origin: one desk input field changed.
    // DeskInputPayload.
    DeskInput = 97,

    // From the presser, relayed: the quick scan fired; mirrors replay its visual. DeskScanEventPayload.
    DeskScanEvent = 98,

    // Host to all: the download arm edge with the host-rolled polarity; also a joiner's arm delivery.
    // DishArmPayload.
    DishArm = 99,

    // Host to one joiner: every dish's pose, calibration and the active mask. DishSnapshotPayload.
    DishSnapshot = 100,

    // Any peer, relayed: absolute calibration values for the dishes whose local values changed.
    // DishCalibPayload.
    DishCalib = 101,

    // From the presser, relayed: a wall-unit reel slot insert or eject. ReelSlotPayload.
    ReelSlot = 102,

    // Host to all: the daily task mirror. TaskNewStatePayload.
    TaskNewState = 103,

    // Client to host: my eject birthed this reel in my hands; author it. PropDropIntentPayload with
    // the progress in savedScalar; class-whitelisted to the reels; Bulk lane, so a pocket destroy
    // cannot overtake it.
    ReelEjectIntent = 104,

    // From the presser, relayed: play a desk one-shot cue, or switch a desk loop on or off, on this
    // component. DeskSndFxPayload.
    DeskSndFx = 105,

    // From the presser, relayed: the laptop's power and floppy edges, its connect state and the
    // portable PC's lid. LaptopStatePayload; content rides LaptopBlob.
    LaptopState = 106,

    // From the presser, relayed: deck playback play or stop, generation-guarded. PlayDeckEventPayload.
    PlayDeckEvent = 107,

    // Peer to host: plug or unplug a desk module by value; host to all: the canonical array; host
    // to one peer: a denial. PhysModsStatePayload.
    PhysModsState = 108,

    // Any peer, relayed: a drive slot's occupancy line, idempotent; the host re-announces
    // canonically on conflict. DriveSlotStatePayload.
    DriveSlotState = 109,

    // Any peer, relayed: one drive's data row as a chunked blob.
    DrivePayload = 110,

    // Peer to host: set or take a rack row; host to all: the canonical rack; host to one peer: a
    // denial. A blob headed by RackStateHead.
    RackState = 111,

    // Any peer, relayed: one laptop database row appended, as a chunked blob.
    MeadowAppend = 112,

    // Any peer, relayed: one database row deleted by content hash. ContentHashPayload.
    MeadowDelete = 113,

    // Client to host with its order after a move; host to all with the canonical order. A blob of
    // content hashes in array order; clients apply host-authored lines only.
    MeadowOrder = 114,

    // Laptop slot or disc content as a chunked blob; the host refans a client's chunks as they are.
    // Same lane as LaptopState.
    LaptopBlob = 115,

    // Client to host: an edit-script batch over the laptop's file buffers; host to all: the
    // canonical quad, which is also the acknowledgement. Chunked blob.
    LaptopQuad = 116,

    // Client to host: push or pop on a disc crate; host to all: the canonical arrays; host to one
    // peer: a denial. Chunked blob, eid-addressed.
    FloppyBoxState = 117,

    // A world container's contents as state, eid-addressed. The peer whose take or add ran authors
    // the slice with the base hash it last applied; the host accepts it only against its current
    // truth, then relays it to everyone but the author. Chunked blob; personal inventories are
    // skipped.
    ContainerContents = 118,

    // Host to all, the origin included: one chat line with the host-assigned sequence that orders
    // the conversation; flag bit 0 marks a join-seed row, which lands retained and never live. Always
    // preceded by ChatSpeaker. ChatLinePayload.
    ChatLine = 119,

    // Host to all: who the following ChatLine is from (nick, colour, slot), sent before every live
    // line and once per speaker in a seed burst. ChatSpeakerPayload.
    ChatSpeaker = 120,

    // Host to one client: the order you forwarded was not performed, and why. The host also sends
    // that client its balance, and the client rebuilds its cart. OrderRefusedPayload.
    OrderRefused = 121,

    // Client to host: I shot this prop with the coin gun, named by key with the eid as the keyless
    // fallback, sent just before the client's own PropDestroy on the same lane. The host prices it
    // from its own copy, mints the coins and destroys the prop itself. CoinGunSellPayload.
    CoinGunSell = 122,

    // Client to host: I collected this coin (a host-minted world actor, named by eid); the host
    // runs the coin's own verb. CoinCollectPayload.
    CoinCollect = 124,

    // Host to one client: what your sale did: sold with the host's price, or a named refusal.
    // CoinGunResultPayload.
    CoinGunResult = 123,

    // Client to host, the first message on a connection: the client's nonce. AuthHelloPayload.
    // The three admission kinds are the only ones an unadmitted connection may carry; they ride no
    // lane and no world gate, since the peer holds no seat yet. Neither side's public key is on the
    // wire: each end reads the other's identity off its own connection, and the signature is what a
    // peer presenting a stolen identity cannot produce.
    AuthHello = 125,

    // Host to client: the host's nonce, its signature over both identities and the client's nonce,
    // and whether a password is wanted. The client verifies against the identity bytes its connection
    // handed it before sending anything further. AuthChallengePayload.
    AuthChallenge = 126,

    // Client to host: the client's signature over both identities and the host's nonce, plus the
    // optional password tag. On a good verify the host seats the peer; the AssignPeerSlot that follows
    // is the admission signal. AuthProofPayload.
    AuthProof = 127,

    // Host to all: a light group's active state, keyed by its root's Key. The switch's own bit rides
    // LightState; the lamps are gated by the root, which the host owns because most things that move
    // a group are host-owned world systems. A client's press still reaches the host as a LightState
    // edge. KeyedTogglePayload.
    LightGroupState = 129,
};

#pragma pack(push, 1)

// Every datagram starts with this. seq is per sender and monotonic; the receiver drops anything
// older than the last it saw. senderEpoch is the sender's per-process epoch, minted non-zero at
// session start: the receiver latches the first epoch it sees from a slot and rejects a mismatch,
// so a packet from a previous incarnation of a reconnected peer is never honoured. senderSlot is
// the logical origin: the host rewrites it (and the epoch) when it relays a client's datagram to
// other clients; a receiving client routes by it, the host ignores it and trusts the connection.
// stateTimeMs24 is the origin's clock for the state carried; see the field.
struct PacketHeader {
    uint32_t magic;        // kMagic
    uint16_t version;      // kProtocolVersion
    uint8_t  type;         // MsgType
    uint8_t  _pad;         // reserved
    uint32_t seq;          // per-sender sequence number
    uint32_t senderEpoch;  // the sender's per-process epoch (non-zero; 0 = not yet latched at the receiver)
    uint8_t  senderSlot;   // the logical origin slot (the host rewrites it on relay)
    // The origin's monotonic time, in milliseconds, for the STATE this datagram carries, not for
    // the moment it was sent: stamping at send would let a game-thread hitch put an old position
    // under a new stamp. 24 bits, wrapping every 16 777 216 ms; read with ReadStateTimeMs24 and
    // wrap-safe arithmetic, and re-anchor after any gap long enough to make a wrap ambiguous.
    // 0 means not stamped. The header cannot grow: BlobChunkPayload and VoiceFramePacket are both
    // exactly the maximum datagram. The relay scrubs the field, since only the host reads it.
    uint8_t  stateTimeMs24[3];
};
static_assert(sizeof(PacketHeader) == 20, "PacketHeader must be 20 bytes");

// The origin's state time, or 0 for "not stamped". Little-endian 24-bit; see PacketHeader.
inline void WriteStateTimeMs24(PacketHeader& h, uint32_t ms) {
    const uint32_t v = ms & 0x00FFFFFFu;
    h.stateTimeMs24[0] = static_cast<uint8_t>(v & 0xFFu);
    h.stateTimeMs24[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    h.stateTimeMs24[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
}
inline uint32_t ReadStateTimeMs24(const PacketHeader& h) {
    return static_cast<uint32_t>(h.stateTimeMs24[0]) |
           (static_cast<uint32_t>(h.stateTimeMs24[1]) << 8) |
           (static_cast<uint32_t>(h.stateTimeMs24[2]) << 16);
}
// Wraparound-safe elapsed between two 24-bit stamps. Only meaningful when the true interval is
// shorter than the 16 777 216 ms period -- the caller owns that precondition by re-anchoring.
inline uint32_t ElapsedMs24(uint32_t earlier, uint32_t later) {
    return (later - earlier) & 0x00FFFFFFu;
}
inline constexpr uint32_t kStateTimeMs24Period = 0x01000000u;

// This process's monotonic time, folded into the 24-bit field. NEVER returns 0, because 0 is the
// "not stamped" sentinel -- a legitimate sample landing exactly on the wrap would otherwise announce
// itself as unstamped and hold its own peer untrusted. The 1 ms substituted once every ~4.7 hours is
// far below every bound that reads this field.
inline uint32_t NowStateTimeMs24() {
    static const std::chrono::steady_clock::time_point kEpoch = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - kEpoch).count();
    const uint32_t v = static_cast<uint32_t>(ms) & 0x00FFFFFFu;
    return v == 0u ? 1u : v;
}

// A player's pose. Floats are UE4 centimetres and degrees.
//   yaw          -- the body's horizontal facing (actor yaw).
//   pitch        -- the view pitch; the actor never tilts, so this drives the puppet's head bone.
//   headYawDelta -- controller yaw minus actor yaw, in (-180, 180]: the head's lead over the body.
//   speed        -- horizontal velocity magnitude (cm/s), the locomotion blend input.
//   stateBits    -- bit 0 in air (the source's movement mode is falling; clears the puppet's foot
//                   IK), bit 1 ragdolled and not dead (every ragdoll cause; the receiver toggles
//                   ragdollMode and forceGetUp on the edges), bits 2..7 reserved.
struct PoseSnapshot {
    float   x, y, z;
    float   yaw;
    float   pitch;
    float   headYawDelta;
    float   speed;
    uint8_t stateBits;
    // Vitals, display only: each peer packs its own, the receiver draws the nameplate bar and
    // never writes them back to a save. Continuous and lossy; they never trigger a state change.
    uint8_t healthFrac;  // health / maxHealth, quantized 0..255 (maxHealth is per-peer)
    uint8_t foodFrac;    // food  / kVitalScalarMax, quantized 0..255
    uint8_t sleepFrac;   // sleep / kVitalScalarMax, quantized 0..255
};
static_assert(sizeof(PoseSnapshot) == 32, "PoseSnapshot must be 32 bytes");

// PoseSnapshot.stateBits flags. Single-byte field; flags assigned bit-by-bit.
inline constexpr uint8_t kStateBitInAir   = 0x01;
inline constexpr uint8_t kStateBitRagdoll = 0x02;  // the source is ragdolled (faint, manual, knock-out), not dead

// The game's vital scalars (food, sleep, the default max health) top out at 100. health is
// normalised by the peer's own max health before quantisation; food and sleep by this.
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

// A reliable message: the standard header (type Reliable) + ReliableHeader + the payload.
struct ReliableHeader {
    uint8_t  kind;     // ReliableKind
    uint8_t  _pad[3];
    uint16_t payloadLen;
    uint16_t _pad2;
};
static_assert(sizeof(ReliableHeader) == 8, "ReliableHeader must be 8 bytes");

// A short string carrier: a prop key, a portable identity, a quantized position. len 0 means not
// set; bytes beyond len are zero on the wire so equality compares work.
struct WireKey {
    uint8_t len;       // 0..31 (chars in `data`)
    char    data[31];  // UTF-8 (Aprop_C Keys are ASCII)
};
static_assert(sizeof(WireKey) == 32, "WireKey must be 32 bytes");

// A Blueprint class leaf name ("Aprop_equipment_flashlight_C"). Bytes beyond len are zero.
struct WireClassName {
    uint8_t len;       // 0..63 chars in `data`
    char    data[63];  // ASCII (VOTV class names are ASCII)
};
static_assert(sizeof(WireClassName) == 64, "WireClassName must be 64 bytes");

// A held prop's world transform, sent unreliably while the sender holds it. The receiver resolves
// the prop by key (by eid for a keyless trash entity), disables its physics and drives the
// transform; a stream that stops is an implicit release.
struct PropPoseSnapshot {
    WireKey key;        // 32 -- which prop (cross-peer stable string)
    float   x, y, z;    // world cm
    float   pitch;
    float   yaw;
    float   roll;
    // The sender's element id for the prop; a keyless trash clump is resolved by it. 0 = none.
    uint32_t elementId;
    // The trash entity's generation (trash_channel ctx): a pose whose ctx is older than the eid's
    // known generation is dropped, so a carry pose in flight when the entity re-piles cannot re-drive
    // the settled pile. 0 = no enforcement (a keyed prop).
    uint8_t  ctx;
    uint8_t  _pad[3];
};
static_assert(sizeof(PropPoseSnapshot) == 64, "PropPoseSnapshot must be 64 bytes");

struct PropPosePacket {
    PacketHeader     header;  // 20
    PropPoseSnapshot pose;    // 64
};
static_assert(sizeof(PropPosePacket) == 84, "PropPosePacket must be 84 bytes");

// One character's pose in the EntityPose batch, keyed by its element id. The host reads the live
// actor each send tick; the client interpolates and drives the mirror's movement component.
struct EntityPoseSnapshot {
    uint32_t elementId;   // 4  -- Npc Element id (host range)
    float    x, y, z;     // 12 -- world cm (actor location = ACharacter capsule centre = pivot)
    float    yaw;         // 4  -- actor yaw deg (NormalizeAxis'd)
    float    speed;       // 4  -- horizontal velocity magnitude cm/s (drives the locomotion blend)
    float    lookAtX, lookAtY, lookAtZ;  // 12 -- the kerfur's head look target in world space; valid iff kEntityPoseBitHasLookAt
                          //      FAnimNode_LookAt aim point). VALID iff stateBits has kEntityPoseBitHasLookAt
                          //      (only kerfur-family NPCs carry it; non-kerfur NPCs leave it zero + the bit clear).
    float    bodyYaw;     // 4  -- the kerfur's visible body yaw, decoupled from the actor root; valid iff kEntityPoseBitHasBodyYaw
    uint8_t  stateBits;   // 1  -- bit0=inAir, bit1=hasLookAt, bit2=hasBodyYaw, bit3=hasKerfurState, bit4=kerfurSpooky
    uint8_t  kerfState;   // 1  -- the kerfur's command state; valid iff bit 3. Drives the parked mirror's state machine.
    uint8_t  kerfFace;    // 1  -- the kerfur's face material index; valid iff bit 3.
    uint8_t  _pad;        // 1  -- 4-byte alignment
};
static_assert(sizeof(EntityPoseSnapshot) == 44, "EntityPoseSnapshot must be 44 bytes");

// EntityPoseSnapshot.stateBits: bit 0 reuses kStateBitInAir; the rest are entity-specific.
inline constexpr uint8_t kEntityPoseBitHasLookAt = 0x02;  // lookAt{X,Y,Z} carries a valid head-look world target
inline constexpr uint8_t kEntityPoseBitHasBodyYaw = 0x04;  // bodyYaw carries a valid visible-body world yaw
inline constexpr uint8_t kEntityPoseBitHasKerfurState = 0x08;  // kerfState + kerfFace carry valid kerfur command/face
inline constexpr uint8_t kEntityPoseBitKerfurSpooky   = 0x10;  // the kerfur is in its spooky/kill state

// The header of a pose batch datagram; N entries follow.
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

// One world actor's transform in the WorldActorPose batch, keyed by its element id. Full rotation
// and no speed: a plain actor that banks and rolls. The client interpolates and drives the parked
// mirror.
struct WorldActorPoseSnapshot {
    uint32_t elementId;        // 4  -- WorldActor Element id (host range)
    float    x, y, z;          // 12 -- world cm (actor location = pivot)
    float    pitch, yaw, roll; // 12 -- actor world rotation deg (NormalizeAxis'd, FULL rotation)
    float    auxYaw;           // 4  -- a class-specific visible heading when it lives outside the actor rotation (the
                               //      pyramid's arrow components; its root never yaws). Otherwise equal to yaw.
    float    auxX, auxY, auxZ;  // 12 -- a class-specific target vector: the pyramid's idle look target, so the mirror's
                               //      native easing aims where the host's does. Zero for other classes.
    uint32_t auxTargetEid;     // 4  -- a class-specific target identity: the pyramid's wisp target as its element id, so
                               //      the mirror runs the same native chase branch. 0 = none.
};
static_assert(sizeof(WorldActorPoseSnapshot) == 48, "WorldActorPoseSnapshot must be 48 bytes");

// Max WorldActors per WorldActorPose datagram, MTU-capped: (1400 - PacketHeader(20) -
// EntityPoseBatchHeader(4)) / 48 = 28. The realistic event WA count is a handful (a few UFOs
// at once), so 28 keeps the datagram (20 + 4 + 28*48 = 1368) under the 1400 MTU budget.
// The batch reuses EntityPoseBatchHeader (a generic count+pad), NOT a byte-identical twin (RULE 2).
inline constexpr int kMaxWorldActorBatchEntries = 28;
inline constexpr int kWorldActorPoseDatagramMax =
    static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader)) +
    kMaxWorldActorBatchEntries * static_cast<int>(sizeof(WorldActorPoseSnapshot));

// One carried or flying trash clump's pose in the TrashCarryPose batch, host-originated so every
// client, the grabber included, sees it move. Keyed by the trash eid; ctx is the carry generation,
// and a pose whose ctx is not the currently adopted one is dropped.
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

// A ragdolling player's pelvis: world location and rotation, linear and angular velocity, read off
// the sender's own ragdoll actor and sent unreliably while ragdolled. The receiver writes the
// velocities onto its mirror body's pelvis each packet and drives the puppet's rotation from it.
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

// The held hand item's view-relative transform (MsgType::HandPose): relPos in cm along the owner's
// roll-free view basis from the head anchor, relRot the item's rotator in that frame. Streamed
// while holding; the receiver overwrites its hand mirror's relative fields.
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

// The desk's live cursor (MsgType::DeskCursorPose): ui_coordinates.viewCoordinate in screen space,
// streamed while the desk is claimed and the cursor moves; the mirror interpolates it.
struct DeskCursorPoseSnapshot {
    float viewX, viewY;  // ui_coordinates.viewCoordinate (screen-space)
};
static_assert(sizeof(DeskCursorPoseSnapshot) == 8, "DeskCursorPoseSnapshot must be 8 bytes");

struct DeskCursorPosePacket {
    PacketHeader           header;  // 20
    DeskCursorPoseSnapshot pose;    // 8
};
static_assert(sizeof(DeskCursorPosePacket) == 28, "DeskCursorPosePacket must be 28 bytes");

// One 20 ms Opus frame (MsgType::VoiceFrame). The datagram carries 8 + opusLen bytes. seq is the
// per-sender voice sequence the jitter buffer orders by. A stop marker (flag bit 1, opusLen 0)
// ends a burst: the receiver flushes and resets its decoder. Whisper (bit 0) halves the
// attenuation radius. The speaker is the header's senderSlot.
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

// Voice presence for the icon surfaces (ReliableKind::VoiceState).
struct VoiceStatePayload {
    uint8_t micMuted;       // 1 = the peer muted its mic
    uint8_t voiceDisabled;  // 1 = the peer turned the voice module off entirely
    uint8_t _pad[2];
};
static_assert(sizeof(VoiceStatePayload) == 4, "VoiceStatePayload must be 4 bytes");

// A wall-attachable's stick (PropStickState): the prop by key (eid as fallback), which field the
// Blueprint set (bit 0 frozen, bit 1 static) and the commit-time transform the receiver pre-poses
// to before replaying the prop's own forceStick.
struct PropStickStatePayload {
    WireKey  key;
    uint32_t elementId;
    uint8_t  flags;      // bit0 = frozen, bit1 = static
    uint8_t  _pad[3];    // zeroed
    float locX, locY, locZ;
    float rotPitch, rotYaw, rotRoll;
};
static_assert(sizeof(PropStickStatePayload) == 64, "PropStickStatePayload must be 64 bytes");

// A kerfur conversion request (KerfurConvertRequest): the element id of the form the client's menu
// targeted (a character when toProp is 1, a prop when 0). The host resolves it, validates the actor
// and runs the Blueprint verb; the outcome rides KerfurConvert.
struct KerfurConvertPayload {
    uint32_t elementId;  // the dying form's host-range mirror eid; the host resolves the actor and the kerfur id
    uint8_t  toProp;     // 1 = NPC -> prop (turn_off); 0 = prop -> NPC (turn on)
    uint8_t  _pad[3];    // zeroed
};
static_assert(sizeof(KerfurConvertPayload) == 8, "KerfurConvertPayload must be 8 bytes");

// A kerfur form transition (KerfurConvert), host to all: the stable kerfur id, the old form's eid
// the client destroys, the new form's eid, class and pose, or rejected = 1 when the host refused.
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

// A kerfur menu command (KerfurCommand), client to host. The host takes the requester from the
// sender slot; a self-declared slot would be spoofable.
struct KerfurCommandPayload {
    uint32_t elementId;  // host Npc eid of the target kerfur
    uint8_t  command;    // KerfurMenuCommand (follow/idle/patrol/fix_servers/get_reports/fix_transformers)
    uint8_t  _pad[3];    // zeroed
};
static_assert(sizeof(KerfurCommandPayload) == 8, "KerfurCommandPayload must be 8 bytes");

// A release (PropRelease): the prop by key, its inherited linear and angular velocity at the
// release edge, and for a keyless trash entity the eid and its generation. Sent once.
struct PropReleasePayload {
    WireKey key;
    float   linVelX;   // cm/s -- GetPhysicsLinearVelocity at release
    float   linVelY;
    float   linVelZ;
    float   angVelX;   // deg/s -- GetPhysicsAngularVelocityInDegrees at release
    float   angVelY;
    float   angVelZ;
    // The trash entity's eid, so a keyless clump's throw routes by identity; 0 = a keyed prop.
    uint32_t elementId;
    // The trash entity's generation; a release older than the eid's known generation is dropped.
    uint8_t ctx;
    uint8_t _pad[3];
};
static_assert(sizeof(PropReleasePayload) == 64, "PropReleasePayload must be 64 bytes");
// Every reliable payload carries this guard: a payload past one datagram's budget would be
// refused at send time, so catch it at compile time.
static_assert(sizeof(PropReleasePayload) <= 256 - 20 - 8,
              "PropReleasePayload must fit in one reliable datagram (kMaxReliablePayload)");

// Velocity magnitude (cm/s) above which a release counts as a throw on the receiver and fires the
// prop's thrown event. 2 m/s is well above a walking drop's residual and well below a flick.
inline constexpr float kThrownLinVelThreshold = 200.f;

// A prop birth (PropSpawn): the class, the persistent key, the list_props row name that init()
// resolves the mesh, mass and collision from, the transform and scale, the physics flags, an
// initial velocity, the sender's element id, the save-time position for a join snapshot and the
// per-class save scalar. Inventory contents never cross; only the world identity does.
//
// physFlags: propspawn_flags.
struct PropSpawnPayload {
    WireClassName className;       // 64 -- "Aprop_equipment_flashlight_C" etc.
    WireKey       key;             // 32 -- the persistent cross-peer Key
    // The list_props row name: an Aprop_C's mesh, mass and collision are resolved by init() from the
    // row named here (the class default is the cube row), so a mirror must carry it or it renders as
    // a white cube. len 0 for classes without a row (the trash families).
    WireKey       propName;        // 32 -- Aprop_C list_props row FName
    float         locX, locY, locZ;            // 12 -- world cm
    float         rotPitch, rotYaw, rotRoll;   // 12 -- FRotator (matches PropPose shape)
    float         scaleX, scaleY, scaleZ;      // 12 -- the sender's actor scale (part of the saved transform)
    uint8_t       physFlags;        // 1
    uint8_t       chipType;         // 1  -- the trash variant (enum_chipPileType); 0 for other props
    uint8_t       hasMatchPos;      // 1  -- matchX/Y/Z carry this pile's save-time position
    uint8_t       _pad;             // 1
    float         initLinVelX, initLinVelY, initLinVelZ;  // 12 -- initial velocity (cm/s), usually zero
    float         initAngVelX, initAngVelY, initAngVelZ;  // 12
    // The prop's element id in the sender's range (host range from the host, peer range from a
    // client's keyless trash entity). 0 = the sender had no element.
    uint32_t      elementId;        // 4
    // For a pile in a join snapshot: the pile's position at scratch-save time, which both peers loaded
    // from the same transferred save. The client's twin-destroy matches its save-loaded native against
    // this rather than the current pose, so a pile the host moved during the join window is
    // reconciled instead of duplicated. Valid iff hasMatchPos.
    float         matchX, matchY, matchZ;   // 12 -- save-time position (world cm); valid iff hasMatchPos
    // A per-class save scalar (a reel's progress) applied at mirror birth, filled by the one shared
    // reader ue_wrap::prop::ReadSavedScalarForClass. Valid iff kHasSavedScalar.
    float         savedScalar;      // 4 -- per-class save scalar (reels: Progress); valid iff kHasSavedScalar
};
static_assert(sizeof(PropSpawnPayload) == 212, "PropSpawnPayload must be 212 bytes");
static_assert(sizeof(PropSpawnPayload) <= 256 - 20 - 8,
              "PropSpawnPayload must fit in one reliable datagram");

namespace propspawn_flags {
inline constexpr uint8_t kSimulatePhysics = 0x01;
inline constexpr uint8_t kIsHeavy         = 0x02;
inline constexpr uint8_t kFrozen          = 0x04;
// The remaining Aprop_C bools the game's own loader restores before re-running init(), which
// derives physics and collision from them; the receiver raw-writes them before FinishSpawningActor.
inline constexpr uint8_t kStatic          = 0x08;  // Aprop_C.Static @0x02D8
inline constexpr uint8_t kSleep           = 0x10;  // Aprop_C.sleep  @0x02DD
inline constexpr uint8_t kRemoveWOrespawn = 0x20;  // Aprop_C.removeWOrespawn @0x02D9
inline constexpr uint8_t kHasSavedScalar  = 0x40;  // PropSpawnPayload.savedScalar / PropDropIntentPayload.savedScalar is valid
}  // namespace propspawn_flags

// A prop death (PropDestroy): the key, and the sender's element id for the mirror binding. The
// sender's destroy observer captures the key before the engine destroys the actor.
struct PropDestroyPayload {
    WireKey  key;
    // The prop's element id in the sender's range; 0 = none. The receiver drains the mirror binding.
    uint32_t elementId;       // 4
    uint32_t _pad;            // 4 -- alignment
};
static_assert(sizeof(PropDestroyPayload) == 40, "PropDestroyPayload must be 40 bytes");

// A placement intent (PropDropIntent), client to host: the identity the host needs to spawn the
// authoritative prop by key (class, key, row name), the placement transform and scale, and the
// parity flags. No element id (the host allocates its own) and no velocity (a placed prop rests).
struct PropDropIntentPayload {
    WireClassName className;                    // 64 -- "prop_rock_C" etc. (R::ClassNameOf of the placed actor)
    WireKey       key;                          // 32 -- the persistent cross-peer save Key (loadData-restored)
    WireKey       propName;                     // 32 -- Aprop_C list_props row FName (white-cube parity)
    float         locX, locY, locZ;             // 12 -- placement world cm
    float         rotPitch, rotYaw, rotRoll;    // 12 -- placement FRotator
    float         scaleX, scaleY, scaleZ;       // 12 -- placed actor's GetActorScale3D
    uint8_t       physFlags;                    // 1  -- propspawn_flags (kStatic/kFrozen/kSleep/kRemoveWOrespawn parity)
    uint8_t       _pad[3];                      // 3  -- 4-byte alignment; zero on the wire
    // The per-class save scalar (ReelEjectIntent carries a reel's progress); valid iff kHasSavedScalar.
    float         savedScalar;                  // 4 -- valid iff physFlags & kHasSavedScalar
};
static_assert(sizeof(PropDropIntentPayload) == 172, "PropDropIntentPayload must be 172 bytes");
static_assert(sizeof(PropDropIntentPayload) <= 256 - 20 - 8, "PropDropIntentPayload must fit one datagram");

// --- The save transfer ---
// Data bytes per SaveTransferChunk message (plus a 4-byte index prefix). Far above
// kMaxReliablePayload by design: the session diverts the kind to the bulk sink and the transport
// fragments the message; the 16-bit payload length caps the whole payload at 65535, so 56K plus
// the index fits with headroom. A 17 MB save is about 308 messages on the Bulk lane.
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
    uint8_t  pad[3] = {};  // zero
    uint32_t sidecarBytes;  // the leading bytes of the blob that are the identity sidecar (the map from save object
                           // index to host eid for the keyless natives), carried inside the same CRC'd stream. 0 = no
                           // sidecar.
};
static_assert(sizeof(SaveTransferBeginPayload) == 20, "SaveTransferBeginPayload must be 20 bytes");
static_assert(sizeof(PropDestroyPayload) <= 256 - 20 - 8,
              "PropDestroyPayload must fit in one reliable datagram");

// The shared payload of every keyed on/off kind: the instance's key and the state after the edge.
// One generic channel in coop/interactables drives them.
struct KeyedTogglePayload {
    WireKey  key;        // 32 -- the instance's Key FName (string)
    uint8_t  action;     // 1  -- 0 = closed/off, 1 = open/on (the state AFTER the edge)
    uint8_t  _pad[7];    // 7  -- 8-byte alignment / reserved
};
static_assert(sizeof(KeyedTogglePayload) == 40, "KeyedTogglePayload must be 40 bytes");
static_assert(sizeof(KeyedTogglePayload) <= 256 - 20 - 8,
              "KeyedTogglePayload must fit in one reliable datagram");

// A device claim or release (DeviceClaim): the claim key, the holding slot (on a host reply the
// winner) and busy. A losing claimant sees busy = 1 with another slot while still inside.
struct DeviceClaimPayload {
    WireKey  key;        // 32 -- the device claim key
    uint8_t  slot;       // 1  -- holding peer slot
    uint8_t  busy;       // 1  -- 1 = claimed, 0 = released
    uint8_t  _pad[2];    // 2  -- alignment / reserved
};
static_assert(sizeof(DeviceClaimPayload) == 36, "DeviceClaimPayload must be 36 bytes");
static_assert(sizeof(DeviceClaimPayload) <= 256 - 20 - 8,
              "DeviceClaimPayload must fit in one reliable datagram");

// One sky signal on the wire: the game's row with the object name as a string (name indices are
// not cross-process stable). alpha is the expiry countdown; direction is gameplay-load-bearing,
// since the catch gate compares it to the panel's toggle.
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

// The sky signal set (SkySignalState) in parts of up to three rows; gen guards against mixing parts
// of two snapshots (a mismatch drops and waits for the next).
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

// The signal-catch replay (SkySignalCatch): the caught row from the catcher's own desk struct, the
// exact dish slew vector (relative, so every receiver's dishes replay it) and kind: 0 a catch, 1
// cleared (row and slew ignored), 2 a connect seed (applied like 0, never announced).
struct SkySignalCatchPayload {
    WireSkySignal row;          // 64 -- the caught signal's full row content
    float   slewX, slewY, slewZ;// 12 -- the startMovingTo relative vector
    uint8_t kind;               // 1  -- 0 = catch, 1 = cleared, 2 = connect seed
    uint8_t slewValid;          // 1  -- 0 = no dish was moving; arm directly
    uint8_t _pad[2];            // 2
};
static_assert(sizeof(SkySignalCatchPayload) == 80, "SkySignalCatchPayload must be 80 bytes");

// The laptop's edges (LaptopState). op: 0 power, 1 insert (slot scalars plus the thrown disc's eid,
// 0 when held), 2 eject, 3 connect state, 6 the portable PC's lid. Content rides LaptopBlob.
struct LaptopStatePayload {
    uint8_t  op;          // 0=power, 1=insert, 2=eject, 3=state, 6=portable-PC lid
    uint8_t  isOpened;    // op 0/3: laptop power; op 6: lid opened
    uint8_t  zip;         // op 1/3
    uint8_t  slot;        // op 1: 0=floppy hitbox, 1=zip hitbox
    int32_t  floppyType;  // op 1/3 (-1 = empty)
    int32_t  readWrites;  // op 1/3
    uint32_t eid;         // op 1: thrown world-disc eid (0=held); op 6: portable PC eid
};
static_assert(sizeof(LaptopStatePayload) == 16, "LaptopStatePayload must be 16 bytes");
static_assert(sizeof(LaptopStatePayload) <= 228, "LaptopStatePayload must fit the inline reliable buffer");

// The desk's scalar snapshot (DeskState), host to a joiner with adopt set; receivers write raw and
// run the desk's own refresh chain.
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
    int32_t compMaxLevel;       // 4 -- claim-owner edit; the decode stream is CompState
    int32_t playSelectIndex;    // 4
    uint8_t dlActiveFrFilter;   // 1
    uint8_t dlActivePoFilter;   // 1
    uint8_t activePlay;         // 1
    uint8_t activeDownload;     // 1
    uint8_t activeCoords;       // 1
    uint8_t activeComp;         // 1
    uint8_t coordIsPing;        // 1 -- diagnostic only; receivers never adopt it (it is the ping machine's run flag)
    uint8_t adopt;              // 1
};
static_assert(sizeof(DeskStatePayload) == 52, "DeskStatePayload must be 52 bytes");
static_assert(sizeof(DeskStatePayload) <= 256 - 20 - 8,
              "DeskStatePayload must fit in one reliable datagram");

// One coordinates-terminal event line (DeskLogLine), ASCII, without its CRLF.
struct DeskLogLinePayload {
    uint8_t len;        // 1 -- used bytes in line[]
    uint8_t _pad[3];    // 3
    char    line[120];  // 120 -- one event line WITHOUT the trailing CRLF
};
static_assert(sizeof(DeskLogLinePayload) == 124, "DeskLogLinePayload must be 124 bytes");

// The sleep gate (SleepState). op:
//   0 Report     (peer -> host)  flag = inBed (the sender's isSleep edge)
//   1 Tally      (host -> all)   count/total for the "N/M sleeping" feed line
//   2 Accelerate (host -> all)   everyone is in bed: start the 20x phase
//   3 End        (host -> all)   flag = natural (1: the host slept to full and every peer is
//                                 granted sleep=100; 0: an early interrupt, peers keep their need)
struct SleepStatePayload {
    uint8_t op;     // 1
    uint8_t flag;   // 1 -- Report: inBed; End: natural
    uint8_t count;  // 1 -- Tally: peers in bed
    uint8_t total;  // 1 -- Tally: world-ready peers
};
static_assert(sizeof(SleepStatePayload) == 4, "SleepStatePayload must be 4 bytes");

// An event fire (EventFire), host to all: dispatch 0 runEvent, 1 runSpecialEvent, and the row name.
// Receivers replay only policy-allowlisted rows. No special-event field: the only special is a
// host-local random prank.
struct EventFirePayload {
    uint8_t dispatch;  // 1 -- event_fire_sync::FireKind (0 runEvent / 1 runSpecialEvent)
    char name[31];     // 31 -- the row/case FName, ASCII, NUL-bound (longest live row = 18 chars)
};
static_assert(sizeof(EventFirePayload) == 32, "EventFirePayload must be 32 bytes");

// One in-flight event (EventSnapshot), host to a joiner: the event's class, the mapped list_events
// row ('' when unmapped: logged and skipped) and its elapsed seconds.
struct EventSnapshotPayload {
    char className[48];   // 48 -- ASCII, NUL-bound (longest census class ~30 chars)
    char rowName[48];     // 48 -- ASCII, NUL-bound; '' = class->row map has no entry yet
    uint16_t elapsedSec;  // 2  -- clamped at 65535 (18 h; event phases run seconds-to-minutes)
};
static_assert(sizeof(EventSnapshotPayload) == 98, "EventSnapshotPayload must be 98 bytes");
static_assert(sizeof(EventSnapshotPayload) <= 256 - 20 - 8,
              "EventSnapshotPayload must fit in one reliable datagram");

// The radar alarm state (AlarmState); applied through the trigger's idempotent runTrigger.
struct AlarmStatePayload {
    uint8_t active;   // 1 -- 0/1, the desired trigger_alarm_C.active state
    uint8_t pad[3];   // 3 -- zeroed
};
static_assert(sizeof(AlarmStatePayload) == 4, "AlarmStatePayload must be 4 bytes");

// The signal-server simulation (ServerState), host to all: the aggregates and a mask of broken
// servers by their save-stable array index (up to 64; a larger farm logs and caps).
struct ServerStatePayload {
    int32_t  brokenServers;   // 4  -- mainGamemode.brokenServers (aggregate mirror)
    float    effCalc;         // 4  -- serverEfficiency_calc
    float    effDownl;        // 4  -- serverEfficiency_downl
    uint8_t  serverCount;     // 1  -- servers[].Num at send (bounds; <=64 carried in the mask)
    uint8_t  _pad[3];         // 3  -- zeroed (isBrokenMask is 8-aligned at offset 16)
    uint64_t isBrokenMask;    // 8  -- bit i = servers[i].IsBroken (up to 64 servers)
};
static_assert(sizeof(ServerStatePayload) == 24, "ServerStatePayload must be 24 bytes");

// One page of the roach snapshot (RoachState): the full live set in array order, paged; pages of
// one snapshot share seq. The client assembles the pages and applies by ordinal.
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

// A local roach consumption (RoachConsumed): the component's last known location.
struct RoachConsumedPayload {
    float x, y, z;        // last known world location of the consumed roach's component
};
static_assert(sizeof(RoachConsumedPayload) == 12, "RoachConsumedPayload must be 12 bytes");

// The owner-entity lane (OwnerEntitySpawn/Pose/Destroy): identity is (sender slot, seq); the lane
// is a self-contained per-owner display mirror outside the element registry.
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
    uint8_t  originSlot;  // 1 -- 0 = the transport sender; non-zero only on the host's teardown for a leaver's slot
    uint8_t  _pad;        // 1 -- zeroed
};
static_assert(sizeof(OwnerEntityDestroyPayload) == 4, "OwnerEntityDestroyPayload must be 4 bytes");

// One chunk of a serialized blob, shared by every chunked kind; the assembly key is (sender slot,
// kind, blobSeq); chunks arrive in order; coop/blob_chunks owns send and reassembly. The email
// blob is { u8 version; u8 username; u16 topicChars; u16 textChars; u16 pfpChars; topic UTF-16LE;
// text; pfpLeaf }, capped at 256 / 4096 / 96 chars; signal rows are coop/signal_wire.
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

// A content-keyed delete: FNV-1a 64 over the row's serialized append blob, the same on every peer
// whatever its local array order.
struct ContentHashPayload {
    uint64_t contentHash;
};
static_assert(sizeof(ContentHashPayload) == 8, "ContentHashPayload must be 8 bytes");

// The refiner decode pane (CompState): decodeActive is wire-only state on a mirror and is never
// written to its own latch, since a latched mirror would simulate the decode itself.
struct CompStatePayload {
    uint8_t decodeActive;  // 1
    uint8_t adopt;         // 1 -- host connect snapshot (trust-gated to slot 0)
    uint8_t isFinalLevel;  // 1 -- stamped by the simulator at the falling edge; the mirror's done-versus-progress
                           //      beep uses this, since its own level lags the chunked data
    uint8_t _pad;          // 1
    float   progress;      // 4 -- comp_progress (0..100)
    float   downloading;   // 4 -- comp_downloading (this tick's increment; the B\s readout)
};
static_assert(sizeof(CompStatePayload) == 12, "CompStatePayload must be 12 bytes");

// The committed coordinate locks (DishAimState): the three cursors, the selected index and the
// direction toggle that gates a catch. The live cursor is DeskCursorPose.
struct DishAimStatePayload {
    float   c0X, c0Y;                // 8  -- Coordinate_0
    float   c1X, c1Y;                // 8  -- Coordinate_1
    float   c2X, c2Y;                // 8  -- Coordinate_2
    int32_t selected;                // 4  -- the selected cursor index
    uint8_t direction;               // 1  -- the Direction toggle (the catch gate)
    uint8_t _pad[3];                 // 3
};
static_assert(sizeof(DishAimStatePayload) == 32, "DishAimStatePayload must be 32 bytes");

// The shared payload of the keyed monotone-decreasing dirt scalars (WindowCleanState, GrimeState):
// the instance's identity string (a Key for a window, a quantized position for a grime decal), the
// value, and adopt: 0 applies the minimum of local and wire, 1 (a host snapshot, trusted from slot
// 0 only) writes the value as is.
struct KeyedScalarPayload {
    WireKey  key;        // 32 -- instance identity string (FName for windows; quantized position for grime)
    float    value;      // 4  -- the dirt scalar (>= 0; 0 = fully clean)
    uint8_t  adopt;      // 1  -- 1 = connect-snapshot, apply VERBATIM (adopt host's world); 0 = live wipe, apply MIN
    uint8_t  _pad[3];    // 3  -- 8-byte alignment / reserved
};
static_assert(sizeof(KeyedScalarPayload) == 40, "KeyedScalarPayload must be 40 bytes");
static_assert(sizeof(KeyedScalarPayload) <= 256 - 20 - 8,
              "KeyedScalarPayload must fit in one reliable datagram");

// A dispenser pile's counters (TrashPileState): the displayed count is their sum. adopt 1 writes as
// is; 0 applies the per-component minimum, so concurrent collects converge.
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

// A keypad's input mirror (KeypadState): the typed buffer, the LED selector and a short-code
// event. The buffer replays through inputNumber so every peer's keypad validates natively; a short
// code's accept or cancel changes no digit, so the typing peer stamps it and the receiver runs the
// keypad's own Open chain (an accept unlocks the door; opening is a door edge). A long code
// validates itself at five digits and stamps None.
enum class KeypadEvent : uint8_t {
    None   = 0,  // plain state mirror (digits / active)
    Accept = 1,  // short-code accept press with correct code -> receiver runs native Open(true)
    Deny   = 2,  // short-code wrong-accept / explicit cancel -> receiver runs native Open(false)
};
struct KeypadSyncPayload {
    WireKey  key;        // 32 -- the keypad's Key FName (string)
    uint8_t  bufLen;     // 1  -- digits in `buf` (0..16; codes are short)
    uint8_t  buf[16];    // 16 -- the typed digits, one per byte (each 0..9)
    uint8_t  active;     // 1  -- the keypad's active (LED selector: 0 red and locked, 1 green and powered)
    uint8_t  event;      // 1  -- KeypadEvent
    uint8_t  _pad[5];    // 5  -- reserved
};
static_assert(sizeof(KeypadSyncPayload) == 56, "KeypadSyncPayload must be 56 bytes");
static_assert(sizeof(KeypadSyncPayload) <= 256 - 20 - 8,
              "KeypadSyncPayload must fit in one reliable datagram");

// The power panel's breakers (PowerControlState) as a bitmask in field order (coord, downl, play,
// calc, light). The setter's argument order differs, so the wrapper maps bit to argument by name.
struct PowerPanelPayload {
    WireKey  key;        // 32 -- the panel's AtriggerBase_C::Key FName (string)
    uint8_t  pressMask;  // 1  -- bit0=coord,1=downl,2=play,3=calc,4=light (press_* @0x0380-0x0384)
    uint8_t  _pad[7];    // 7  -- 8-byte alignment / reserved
};
static_assert(sizeof(PowerPanelPayload) == 40, "PowerPanelPayload must be 40 bytes");

// The ATV's rig pose, velocity and condition (AtvState), keyed by its Key. A receiver keeps its own
// physics running and is corrected: the velocity is written from the wire every packet, the
// position error is closed by a bounded corrective velocity, and past a speed-scaled threshold the
// game's own teleportVehicle re-places the whole rig.
struct AtvStatePayload {
    WireKey  key;          // 32 -- the ATV's Key@0x0618 (FName string)
    float    x, y, z;      // 12 -- root body world location (cm; the root Mesh == the actor)
    float    pitch, yaw, roll;  // 12 -- full rotation (the ATV tips/flips, unlike a biped)
    float    linVelX, linVelY, linVelZ;  // 12 -- root body linear velocity (cm/s) at sample time
    float    angVelX, angVelY, angVelZ;  // 12 -- root body angular velocity (deg/s)
    uint8_t  occupantSlot; // 1  -- the SEATED driver's peer slot (0xFF = seat free). This is the
                           //      SEAT, not the author: device_occupancy's E-press deny reads it,
                           //      so a peer merely GRABBING the ATV must not appear here.
    uint8_t  authorSlot;   // 1  -- who is streaming this ATV (the driver or the grabber); 0xFF = nobody, which
                           //      elects the host as its syncer
    uint8_t  stateBits;    // 1  -- bit0=isDriven, bit1=brake, bit2=grabbed (produced, not yet read)
    uint8_t  adopt;        // 1  -- 1 = host connect-snapshot (warp as is), 0 = live stream
    // The condition block: the author's accumulators travel; a mirror's own accrual is held at zero,
    // so overwriting never races an irreversible act. Presence (tiresMask, hasSpare) is consumed
    // from host-authored packets only: a client's eject ships a mask bit whose wheel-prop birth
    // cannot travel, and applying it would turn a divergence into persisted item loss.
    float    tiresDurability[4];  // 16 -- 0..100 per wheel (order: the game's tires[] index order)
    float    tiresDirt[4];        // 16 -- 0..1 per wheel
    float    bodyDirt;            // 4  -- ATV_C `dirt` (body scalar; updDirt writes it to the mesh)
    float    spareDurability;     // 4  -- spareTire_durability (no visual consumer; value truth)
    float    spareDirt;           // 4  -- spareTire_dirt
    float    fuel;                // 4  -- 0..100
    float    health;              // 4  -- 0..100; author-real via its own ALLOWED hits, mirror-stale
    uint8_t  tiresMask;           // 1  -- bit i = tires[i] (PRESENCE -- host-authored packets only)
    uint8_t  tiresValid;          // 1  -- 0 = the producer could not read the arrays; the receiver touches nothing (mask 0
                                  //      is a legal state, all four ejected, so absence needs its own bit)
    uint8_t  hasSpare;            // 1  -- hasSpareTire (PRESENCE -- host-authored packets only)
    int8_t   spareFixes;          // 1  -- spareTire_fixes; signed: ejectWheel writes fixes-1 uncapped, so -1 is reachable,
                                  //      and getTireDamage's input is fixes (a uint8 wrap would render the wrong material)
    int8_t   tiresFixes[4];       // 4  -- per-wheel repair countdown (int32 in-game, int8 on wire)
    uint8_t  tiresTypes[4];       // 4  -- setWheelsType input (zero runtime writers measured; kept
                                  //      because it IS reducer input and 4 B closes the class)
};
static_assert(sizeof(AtvStatePayload) == 148, "AtvStatePayload must be 148 bytes");

// A runtime ATV (AtvSpawn): the host-assigned synthetic key (its own save key is random per peer)
// and the class, so the client spawns the exact skin.
struct AtvSpawnPayload {
    WireKey       synthKey;   // 32 -- host-assigned stable identity ("coopatv#N")
    WireClassName className;   // 64 -- "ATV_C" or a skin subclass
    float         x, y, z;     // 12 -- spawn pose (cm)
    float         pitch, yaw, roll;  // 12
};
static_assert(sizeof(AtvSpawnPayload) == 120, "AtvSpawnPayload must be 120 bytes");
static_assert(sizeof(AtvSpawnPayload) <= 256 - 20 - 8,
              "AtvSpawnPayload must fit in one reliable datagram (kMaxReliablePayload)");

// A runtime ATV's teardown (AtvDestroy).
struct AtvDestroyPayload {
    WireKey synthKey;  // 32
};
static_assert(sizeof(AtvDestroyPayload) == 32, "AtvDestroyPayload must be 32 bytes");

// The authority-lost edge (AtvRelease): a dismount or an ungrab, not a yield. The receiver clears
// the author slot and nothing else; the stream continues from the host as the idle syncer.
struct AtvReleasePayload {
    WireKey key;       // 32 -- the ATV's Key@0x0618
};
static_assert(sizeof(AtvReleasePayload) == 32, "AtvReleasePayload must be 32 bytes");

// The drone's state (DroneState): transform, activity, effect bits and the dust anchor.
struct DroneStatePayload {
    float   x, y, z;           // 12 -- root actor world location (cm)
    float   pitch, yaw, roll;  // 12 -- full rotation (the drone leans/pitches in flight)
    uint8_t active;            // 1  -- Adrone_C::Active (dormant<->flying); gates the host stream
    uint8_t stateBits;         // 1  -- bit 0 rotor dust active, bit 1 can take off (arrived: the alarm cue and the
                               //        interaction gate), bit 2 has sack (cargo aboard)
    uint8_t adopt;             // 1  -- 1 = host connect-snapshot (snap as is), 0 = live stream
    uint8_t _pad;              // 1
    float   dustX, dustY, dustZ;  // 12 -- the dust emitter's world location, which the host's tick pins to its ground
                                  //        trace; the mirror replays the same calls. Valid while bit 0 is set.
                               //        bAbsoluteLocation component to its ground-trace hit per tick;
                               //        the mirror replays K2_SetWorldLocation + the 'dust' param from
                               //        it). Valid only while stateBits bit0 is set; zeros otherwise.
};
static_assert(sizeof(DroneStatePayload) == 40, "DroneStatePayload must be 40 bytes");

// A shop order (OrderRequest), client to host: this header, then chunkItems packed items, each
//     uint8  nameLen;     // length of the row name that follows (1..kMaxOrderRowName)
//     <nameLen bytes>     // a list_store row name (ASCII)
// An item is a row name and nothing else: the host prices it from its own table (a client may name
// what, never what it costs) and rolls its own delivery time. An order that does not fit one
// datagram is split into messages sharing orderId; the host assembles by (sender slot, orderId)
// and commits once all totalItems arrived.
struct OrderRequestHeader {
    uint32_t orderId;     // 4 -- client-local monotonic order id (unique per sender slot)
    uint16_t totalItems;  // 2 -- total items in the WHOLE order (1..kMaxOrderItems)
    uint16_t baseIndex;   // 2 -- index of this chunk's first item (== items already sent)
    uint16_t chunkItems;  // 2 -- items carried in THIS message
    uint16_t _pad;        // 2
};
static_assert(sizeof(OrderRequestHeader) == 12, "OrderRequestHeader must be 12 bytes");

// Economy wire bounds (host trust boundary -- a client must not make the host allocate unbounded).
inline constexpr int kMaxOrderItems   = 64;  // a cart > 64 line-items is rejected as garbage
inline constexpr int kMaxOrderRowName = 96;  // `list_store` keys are short identifiers; cap the string

// Why the host refused a shop order (OrderRefused). Refusal only: a committed order moves the
// balance, which BalanceSync already corrects.
enum class OrderRefusedReason : uint8_t {
    UnknownItem  = 1,  // a row name that is not in the host's own list_store
    Unaffordable = 2,  // the host's OWN balance is short (the client's BP gate tested a mirror that
                       // was stale, or two clients ordered in the same drain pass, or it was bypassed)
    NoCatalog    = 3,  // ue_wrap::store_catalog is INVALID on the host -- fail closed, never guess
    CommitFailed = 4,  // the native makeAnOrder never produced its saveSlot.orders row
};

struct OrderRefusedPayload {
    uint32_t orderId;  // 4 -- echoes OrderRequestHeader.orderId so the client can find its cart items
    uint8_t  reason;   // 1 -- OrderRefusedReason
    uint8_t  _pad[3];  // 3
};
static_assert(sizeof(OrderRefusedPayload) == 8, "OrderRefusedPayload must be 8 bytes");

// A coin-gun sale (CoinGunSell): the sold prop by key, with the eid as the keyless fallback, the
// way PropDestroy names it. Nothing else is trusted: the host re-derives the value from its own
// copy through sellObject, mints the coins itself and positions them from the sold prop, so no
// price, count or gun id belongs here. A client that cannot name the prop sends nothing.
struct CoinGunSellPayload {
    WireKey  key;        // 32 -- the SOLD prop's save Key. len=0 -> keyless, resolve by eid.
    uint32_t elementId;  // 4  -- the SOLD prop's Element id in the SENDER's band. 0 = none.
    uint32_t _pad;       // 4  -- 8-byte alignment (mirrors PropDestroyPayload exactly)
};
static_assert(sizeof(CoinGunSellPayload) == 40, "CoinGunSellPayload must be 40 bytes");

// The host's answer to a sale (CoinGunResult): Sold carries the host's price, which can differ from
// the seller's local toast since price multipliers are per instance; every other code is a refusal
// that must be said, because the seller's prop is already gone from its screen.
enum class CoinGunResultCode : uint8_t {
    Sold          = 1,  // minted; `points` is the price the host derived from ITS copy
    NoSuchProp    = 2,  // neither the key nor the eid resolves to a live prop in the host's world
    AlreadySold   = 3,  // this exact artifact was already minted for and has not died yet
    NoGun         = 4,  // no live, world-placed prop_coingun_C exists to execute the mint
    NotSellable   = 5,  // the host's own sellObject said sold=0 for this prop's name
    HostInternal  = 6,  // a reflection resolve / dispatch on the host failed -- our bug, not theirs
    TooFarAway    = 7,  // the named prop is not within the sender's reach: the gun traces 10 m from the sender's
                        // own camera, so anything farther is an enumeration, not a sale. Also the answer when the
                        // sender has no live puppet to measure against (fail closed)
};

struct CoinGunResultPayload {
    uint8_t  code;      // 1 -- CoinGunResultCode
    uint8_t  _pad[3];   // 3
    int32_t  points;    // 4 -- the price the host minted (Sold only; 0 on every refusal)
};
static_assert(sizeof(CoinGunResultPayload) == 8, "CoinGunResultPayload must be 8 bytes");

// A coin collect (CoinCollect): the coin's host-band eid is its whole identity, since a coin is a
// host-minted world actor with no save key; the client echoes the id the host issued.
struct CoinCollectPayload {
    uint32_t elementId;  // 4 -- the coin's WorldActor eid, in the HOST's band
    uint32_t _pad;       // 4 -- 8-byte alignment
};
static_assert(sizeof(CoinCollectPayload) == 8, "CoinCollectPayload must be 8 bytes");

// --- Admission ---
// See the AuthHello, AuthChallenge and AuthProof kinds for the exchange and for why no public key
// appears in these payloads. Sizes are the primitives': 32 = an Ed25519 public key = a SHA-256
// digest = our nonce; 64 = an Ed25519 signature. The largest is 100 bytes, well inside
// kMaxReliablePayload.
inline constexpr int kAuthNonceBytes = 32;
inline constexpr int kAuthSigBytes   = 64;

struct AuthHelloPayload {
    uint8_t nonce[kAuthNonceBytes];  // 32 -- the CLIENT's freshness, which the host signs
};
static_assert(sizeof(AuthHelloPayload) == 32, "AuthHelloPayload must be 32 bytes");

// LOBBY-PASSWORD FLAGS on the challenge. The host TELLS the joiner whether a
// password is wanted, rather than the joiner inferring it from the browser row:
// a DIRECT or LAN connect has no row at all, and a client that guessed wrong
// would either withhold a required proof or emit one to a host that never asked
// (which is exactly the emission `lobby_password.h`'s rule forbids).
inline constexpr uint8_t kAuthFlagPasswordRequired = 0x01;

struct AuthChallengePayload {
    uint8_t nonce[kAuthNonceBytes];  // 32 -- the HOST's freshness, which the client signs
    uint8_t sig[kAuthSigBytes];      // 64 -- host's signature over the client's nonce blob
    uint8_t flags;                   // kAuthFlag* -- what the host requires of us
    uint8_t _pad[3];                 // explicit: the struct is memcpy'd whole off the wire
};
static_assert(sizeof(AuthChallengePayload) == 100, "AuthChallengePayload must be 100 bytes");

struct AuthProofPayload {
    uint8_t sig[kAuthSigBytes];      // 64 -- client's signature over the host's nonce blob
    // THE PASSWORD TAG IS A SEPARATE FIELD AND NOT PART OF THE SIGNED BLOB, and
    // that separation is the whole security argument -- see `lobby_password.h`.
    // Mixing a KDF of the password into a signature the verifier can recompute is
    // an OFFLINE ORACLE; kept apart, a rogue host learns nothing it can grind,
    // and a client that has not BOUND the host to its advertised identity sends
    // hasPw = 0 rather than a tag.
    uint8_t hasPw;                   // 1 = tag is present and meaningful
    uint8_t _pad[3];
    uint8_t pwTag[32];               // HMAC-SHA256(K, blob) -- zero when hasPw = 0
};
static_assert(sizeof(AuthProofPayload) == 100, "AuthProofPayload must be 100 bytes");

// A character birth (EntitySpawn): the class, the host-allocated element id ([1, 32768); 0 is
// invalid), the transform and scale, whether the host's copy came from the save (the client then
// adopts its own twin by class instead of spawning), and the two kerfur reconcile eids.
struct EntitySpawnPayload {
    WireClassName className;       // 64 -- "npc_zombie_C", "kerfurOmega_mannequin_C", etc.
    uint32_t      elementId;       // 4 -- host-allocated, [1, 32768); 0 = invalid
    uint8_t       savePersisted;   // 1 -- 1 = a save object the joining client also loaded; adopt the local twin by class (the
                                   //      kerfur's save key is random per peer, so only the presence of a key is portable)
    uint8_t       _pad[3];         // 3 -- align loc to 4
    float         locX, locY, locZ;            // 12 -- world cm at spawn time
    float         rotPitch, rotYaw, rotRoll;   // 12 -- FRotator
    float         scaleX, scaleY, scaleZ;      // 12 -- actor scale at spawn; receivers sanitize via SanitizeWireScaleAxis
    uint32_t      retireOffEid;    // 4 -- for a kerfur the host turned on in the join window: the host eid of the off-prop it
                                   //      replaced; the joiner retires that mirror by eid. 0 = not a window turn-on.
    uint32_t      convertFromEid;  // 4 -- for a mid-session kerfur turn-on: the eid of the form it converted from; the
                                   //      initiating client adopts its parked ghost by that eid. 0 = not a conversion.
};
static_assert(sizeof(EntitySpawnPayload) == 116, "EntitySpawnPayload must be 116 bytes");
static_assert(sizeof(EntitySpawnPayload) <= 256 - 20 - 8,
              "EntitySpawnPayload must fit in one reliable datagram");

// A world actor birth (WorldActorSpawn): the class, the host-allocated element id, the transform
// and scale, and an opaque birth blob the receiving class interprets.
struct WorldActorSpawnPayload {
    WireClassName className;                   // 64 -- "piramid2_C", "baocoin_C", ...
    uint32_t      elementId;                   // 4  -- host-allocated, [1, 32768); 0 = invalid
    float         locX, locY, locZ;            // 12 -- world cm at spawn/snapshot time
    float         rotPitch, rotYaw, rotRoll;    // 12 -- FRotator
    float         scaleX, scaleY, scaleZ;      // 12 -- actor Scale3D; receivers run SanitizeWireScaleAxis
    // The birth blob. Opaque to this lane: the receiving class decodes its own bytes and no type
    // comes off the wire. birthLen 0 means the producer carried nothing and the receiver leaves the
    // class default alone; a producer that cannot read logs loudly instead of sending 0. Three
    // allowlisted classes write a property inside their deferred spawn window in three different
    // types (an int, an object reference, two strings), which is why a blob and not a typed field.
    // A member, never bytes appended past sizeof: both ends copy sizeof(p).
    uint8_t       birthLen;                    // 1  -- bytes valid in `birth`; 0 = none carried
    uint8_t       birth[64];                   // 64 -- class-interpreted; see the class's own decoder
    uint8_t       _pad[3];                     // 3  -- explicit; the struct is 4-aligned for the floats
    // Headroom: 172 of the 228 usable bytes. A class whose birth content does not fit
    // length-prefixed in 64 bytes needs the blob sized first.
};
static_assert(sizeof(WorldActorSpawnPayload) == 172,
              "WorldActorSpawnPayload must be 172 bytes");
static_assert(sizeof(WorldActorSpawnPayload) <= 256 - 20 - 8,
              "WorldActorSpawnPayload must fit in one reliable datagram");

// The receiver-side scale sanitizer, a trust-boundary check like the coordinate bounds: a
// non-finite, zero or absurd scale must not reach FinishSpawning. Unit scale is the fallback.
inline float SanitizeWireScaleAxis(float s) {
    if (!(s > 0.01f && s < 100.f)) return 1.f;  // NaN fails both comparisons -> 1
    return s;
}

// A character death (EntityDestroy): the element id of the mirror to tear down.
struct EntityDestroyPayload {
    uint32_t elementId;  // host-allocated, [1, 32768); 0 = invalid
    uint32_t _pad;       // 8-byte alignment
};
static_assert(sizeof(EntityDestroyPayload) == 8, "EntityDestroyPayload must be 8 bytes");
static_assert(sizeof(EntityDestroyPayload) <= 256 - 20 - 8,
              "EntityDestroyPayload must fit in one reliable datagram (kMaxReliablePayload)");

// A teleport (TeleportClient): the pose to apply with K2_TeleportTo. NaN and Inf are rejected
// before the engine call; a host receiving one ignores it.
struct TeleportClientPayload {
    float locX, locY, locZ;        // 12 -- world cm
    float rotPitch, rotYaw, rotRoll; // 12 -- degrees
};
static_assert(sizeof(TeleportClientPayload) == 24, "TeleportClientPayload must be 24 bytes");
static_assert(sizeof(TeleportClientPayload) <= 256 - 20 - 8,
              "TeleportClientPayload must fit in one reliable datagram");

// RestoreVitals carries no payload: the receiver maxes out food, sleep and health.

// An item activation (ItemActivate): an equipment item whose world effect lives on the player (the
// flashlight's cone is on the player actor, so the puppet carries it), or a world prop with its own
// light or audio named by its key hash. itemClassHash is a CRC32 of the class name; the cone fields
// snapshot the sender's light after the Blueprint ran, so the puppet mirrors brightness and focus
// without a mode table.
struct ItemActivatePayload {
    uint32_t itemClassHash;   // CRC32 of item UClass FName string (cross-peer stable)
    // The sender's Player element id (host range from the host, peer range from a client); the
    // receiver routes by its slot and uses it as the self-echo guard. 0 = not yet minted; the
    // receiver then routes by the sender slot.
    uint32_t senderElementId;
    uint8_t  state;           // 0 = off / inactive, 1 = on / active
    uint8_t  flags;           // bit0: has_actor_key (1 = use actorKeyHash)
    uint8_t  mode;            // mp.flashlightMode (0 spread, 1 focused); carried, not written
    uint8_t  _pad;            // 1
    uint32_t actorKeyHash;    // CRC32(Aprop_C::Key string) when flags.has_actor_key=1; 0 otherwise
    float    intensity;       // light_R.Intensity after the Blueprint ran (Unitless scale ~0..10)
    float    outerConeAngle;  // light_R.OuterConeAngle (degrees; ~40 default, ~12 focused)
    float    innerConeAngle;  // light_R.InnerConeAngle (degrees; ~0 default, varies)
};
static_assert(sizeof(ItemActivatePayload) == 28,
              "ItemActivatePayload must be exactly 28 bytes");
static_assert(sizeof(ItemActivatePayload) <= 256 - 20 - 8,
              "ItemActivatePayload must fit in one reliable datagram");

// flags bits for ItemActivatePayload.flags
inline constexpr uint8_t kItemActivateFlag_HasActorKey = 0x01;

// A damage relay (PlayerDamage): the owner peer's Player element id, so the receiver verifies it is
// the addressed peer, and the raw hit amount its own armor mitigates.
struct PlayerDamagePayload {
    uint32_t targetElementId;  // the OWNER peer's Player Element id (host-stamped)
    float    damage;           // raw hit amount; owner BP mitigates per its inventory
};
static_assert(sizeof(PlayerDamagePayload) == 8,
              "PlayerDamagePayload must be exactly 8 bytes");
static_assert(sizeof(PlayerDamagePayload) <= 256 - 20 - 8,
              "PlayerDamagePayload must fit in one reliable datagram");

// WispGrab, host to one victim: the host's wisp is grabbing this client's puppet; the host
// neutralized its own false grab and tells the victim to ragdoll-die after a fixed delay. The
// receiver requires slot 0 as sender and its own element id as victim.
struct WispGrabPayload {
    uint32_t victimElementId;  // the addressed peer's Player Element id (self-verify == own)
    uint32_t wispElementId;    // the killerwisp NPC Element id (tear-mirror association)
    uint32_t killDelayMs;      // host-decided delay before the victim ragdolls (~tear length)
};
static_assert(sizeof(WispGrabPayload) == 12, "WispGrabPayload must be exactly 12 bytes");
static_assert(sizeof(WispGrabPayload) <= 256 - 20 - 8,
              "WispGrabPayload must fit in one reliable datagram");

// WispTear, host to all: play the tear on the local wisp mirror and attach the victim's puppet to
// its grab socket; on the victim's own machine there is no self-puppet.
struct WispTearPayload {
    uint32_t wispElementId;    // the killerwisp NPC Element id -> resolve the local mirror
    uint32_t victimSlot;       // cross-peer Registry slot of the victim (whose puppet to hold)
};
static_assert(sizeof(WispTearPayload) == 8, "WispTearPayload must be exactly 8 bytes");
static_assert(sizeof(WispTearPayload) <= 256 - 20 - 8,
              "WispTearPayload must fit in one reliable datagram");

// PyramidGather: the pyramid (a world actor element) and the wisp (a character element) of a
// committed gather; the client replays the native branch on its mirrors.
struct PyramidGatherPayload {
    uint32_t pyramidEid;  // host-range WorldActor element id of the piramid2_C
    uint32_t wispEid;     // host-range Npc element id of the gathered killerwisp_C
};
static_assert(sizeof(PyramidGatherPayload) == 8, "PyramidGatherPayload must be exactly 8 bytes");
static_assert(sizeof(PyramidGatherPayload) <= 256 - 20 - 8,
              "PyramidGatherPayload must fit in one reliable datagram");

// The shared balance (BalanceSync): the absolute total, host to client.
struct BalancePayload {
    int32_t value;
};
static_assert(sizeof(BalancePayload) == 4, "BalancePayload must be exactly 4 bytes");

// The slot assignment (AssignPeerSlot), host to one client: the slot and the host's own Player
// element id, which the client mirrors in slot 0 so the host's packets resolve to a Player.
struct AssignPeerSlotPayload {
    uint8_t  slot;            // 1..kMaxPeers-1
    uint8_t  _pad[3];         // zero
    uint32_t hostElementId;   // the host's local Player element id
};
static_assert(sizeof(AssignPeerSlotPayload) == 8,
              "AssignPeerSlotPayload must be exactly 8 bytes");

// The weather state (WeatherState). The host reads the fields off its live day-night cycle after
// its own scheduler ran; the client writes the config bits and dispatches the apply functions so
// the Blueprint listeners fan out (causeRain for rain, intComs_triggerSnow for snow).
//   flags: bit 0 isRaining, 1 isSnow, 2 enable_rain, 3 enable_fog, 4 enable_superfog,
//   5 enableSunlight, 6 enableMoonlight, 7 permanentRain.
// Wind: all four directional-wind fields travel and the client overwrites them every apply; the
// game's own setWindParameters writes only the rain pair, and the background pair diverged.
struct WeatherStatePayload {
    // The sender's Player element id; the receiver requires it to resolve to slot 0.
    uint32_t senderElementId;
    uint8_t  flags;              // see weather_flags bit layout above
    uint8_t  flags2;             // fog_flags2
    uint8_t  _pad[2];            // align the float block
    float    rainStrength;        // AdaynightCycle_C::rainStrength @0x0404
    float    rainLightningChance; // AdaynightCycle_C::rainLightningChance @0x0408
    float    rainDeactivateChance;// AdaynightCycle_C::rainDeactivateChance @0x040C
    float    rainWindSpeed;       // AdaynightCycle_C::rainWindSpeed @0x041C
    // The host's current interpolated levels, so a joiner snaps to them instead of ramping over
    // minutes. Not in the dedup signature, which hashes only the flags and the four rain scalars.
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
    // The wind fields. Correct for rain wind and the particle, audio and engine speed; the leaf
    // shake is windTarget below.
    float    windSpeedBg;       // AdirectionalWind_C::windSpeed_background    @0x02EC
    float    windStrengthBg;    // AdirectionalWind_C::windStrength_background @0x02F0
    float    windSpeedRain;     // AdirectionalWind_C::windSpeed_rain          @0x02E4
    float    windStrengthRain;  // AdirectionalWind_C::windStrength_rain       @0x02E8
    // The gust input: windTarget's relative location, the leaf-shake driver the tick springs
    // intensity from. Re-rolled per peer by a random timer, so the client suppresses its own roll and
    // writes the host's. Gated by kWindValid.
    float    windTargetX;       // AdirectionalWind_C::windTarget->RelativeLocation.X
    float    windTargetY;       //                                              .Y
    float    windTargetZ;       //                                              .Z
};
static_assert(sizeof(WeatherStatePayload) == 68, "WeatherStatePayload must be 68 bytes");
static_assert(sizeof(WeatherStatePayload) <= 256 - 20 - 8,
              "WeatherStatePayload must fit in one reliable datagram");

// One firefly emitter (FireflySpawn): the world spawn location; template, rotation and scale are
// the Blueprint's fixed values.
struct FireflySpawnPayload {
    float x;  // world spawn location (the grass hit point near the host's camera)
    float y;
    float z;
};
static_assert(sizeof(FireflySpawnPayload) == 12, "FireflySpawnPayload must be 12 bytes");

// One cosmetic emitter cue (EventCue): the registry index and the world position. cueId is on the
// wire and the registry is append-only.
struct EventCuePayload {
    uint32_t cueId;  // index into event_cue_sync's cue registry (append-only)
    float x;         // world spawn location of the cue emitter
    float y;
    float z;
};
static_assert(sizeof(EventCuePayload) == 16, "EventCuePayload must be 16 bytes");

// One pickup blip (InventoryPickup): the collector's world position at collect time, so the cue
// plays there regardless of puppet state.
struct InventoryPickupPayload {
    float x;  // the collector's world location at collect time
    float y;
    float z;
};
static_assert(sizeof(InventoryPickupPayload) == 12, "InventoryPickupPayload must be 12 bytes");

// A typed chat line (ChatMessage), text only: the speaker is the transport's sender slot, so a
// peer cannot speak as someone else. UTF-8, length-prefixed, not NUL-terminated.
struct ChatMessagePayload {
    uint8_t len;        // bytes used in text[] (0 < len <= sizeof(text))
    char    text[203];  // the line, UTF-8
};
static_assert(sizeof(ChatMessagePayload) == 204, "ChatMessagePayload must be 204 bytes");
static_assert(sizeof(ChatMessagePayload) <= 256 - 20 - 8,
              "ChatMessagePayload must fit in one reliable datagram");

// ChatSpeakerPayload -- WHO the ChatLine that immediately follows is from
// (ChatSpeaker). Host to client only.
//
// The nick is carried rather than looked up, because a lookup answers a DIFFERENT
// question: NicknameForSlot(slot) is who is in that slot NOW, and history is about
// who said it THEN. Slots recycle, so after one departure a resident rendering a
// seeded row from its own roster and a joiner rendering the host's frozen copy would
// hold permanently different names for the same message.
//
// nickArgb is the speaker's CUSTOM colour or 0 for none -- the RECEIVER resolves the
// fallback. Sending a resolved colour instead would freeze a render-side palette onto
// the wire, and sending nothing would lose the pick.
struct ChatSpeakerPayload {
    uint16_t speakerId;   // per-burst index; the following ChatLine names it
    uint8_t  slot;        // world-entity handle -- drives the overhead bubble ONLY
    uint8_t  nickLen;     // bytes used in nick[]
    uint32_t nickArgb;    // the speaker's CUSTOM colour, 0 = none (receiver resolves)
    char     nick[80];    // UTF-8, NOT NUL-terminated (coop::text::kNickMaxBytes)
};
static_assert(sizeof(ChatSpeakerPayload) == 88, "ChatSpeakerPayload must be 88 bytes");
static_assert(sizeof(ChatSpeakerPayload) <= 256 - 20 - 8,
              "ChatSpeakerPayload must fit in one reliable datagram");

// One host-authored chat line (ChatLine); see the kind for why chat is host-authored.
struct ChatLinePayload {
    uint32_t lineSeq;     // host-monotone; THE total order (0 is never a real line)
    uint16_t speakerId;   // names the ChatSpeaker that preceded this row
    uint8_t  flags;       // bit0 = part of a JOIN SEED -> lands retained, never live
    uint8_t  len;         // bytes used in text[]
    char     text[203];   // the message, UTF-8
};
static_assert(sizeof(ChatLinePayload) == 211, "ChatLinePayload must be 211 bytes");
static_assert(sizeof(ChatLinePayload) <= 256 - 20 - 8,
              "ChatLinePayload must fit in one reliable datagram");

inline constexpr uint8_t kChatLineFlagSeed = 0x01;

// One turbine's driver state (TurbineState): the six inputs of the turbine's own spring and
// integrator; the receiver writes them raw and the turbine's tick does the rest.
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

// The edge a PropConvert re-skins.
namespace propconvert_kind {
inline constexpr uint8_t kToClump = 0;  // pile-A -> clump (grab): spawn a kinematic clump, drive by pose
inline constexpr uint8_t kToPile  = 1;  // clump -> pile-B (land): spawn a settled, grabbable pile
}  // namespace propconvert_kind

// A trash re-skin (PropConvert): both peers own the same entity bound to the shared host-minted
// eid, and the morph re-skins it across pile, clump and pile; oldEid == newEid on every edge, so
// the receiver re-points its single rendering instead of creating a second entity. The owner
// emits it from the held-object channel; the host applies a client's convert against its own
// element.
struct PropConvertPayload {
    uint32_t      oldEid;                 // == E (the bound pile/clump being re-skinned)
    uint32_t      newEid;                 // == E (SAME id on the bind model; identity is preserved)
    WireClassName pileClass;              // ToPile: the chipPile leaf class; ToClump: the clump leaf class
    float locX, locY, locZ;               // resting/grab transform of the new rendering
    float rotPitch, rotYaw, rotRoll;
    float scaleX, scaleY, scaleZ;         // the host's real scale of the new form (a clump and a pile differ), applied on
                                          // every convert
    uint8_t chipType;                     // the trash variant (carried across both edges)
    uint8_t kind;                         // propconvert_kind: kToClump (grab) or kToPile (land)
    uint8_t ctx;                          // the host's per-eid generation, bumped on every trash transition; a later pose or
                                          // convert with an older ctx is dropped
    uint8_t hasMatchPos;                  // 1 => matchX/Y/Z carry the pile's pre-grab save-time position (a landing after an
                                          // in-window grab), so the client retires its stale native at the quiescence sweep
    float   matchX, matchY, matchZ;       // the pre-grab position (world cm); valid iff hasMatchPos and kind is kToPile
};
static_assert(sizeof(PropConvertPayload) == 124, "PropConvertPayload must be 124 bytes");

// A grab intent (GrabIntent): the eid of the mirrored pile the client wants; intent only, no state.
struct GrabIntentPayload {
    uint32_t eid;        // the trash entity eid the client requests to grab
    uint8_t  _pad[4];    // 8-byte alignment; bytes-beyond-eid zero
};
static_assert(sizeof(GrabIntentPayload) == 8, "GrabIntentPayload must be 8 bytes");
static_assert(sizeof(GrabIntentPayload) <= 256 - 20 - 8, "GrabIntentPayload must fit one datagram");

// A throw intent (ThrowIntent). mode kRelease: the native drop; the host derives the launch from
// the puppet's smoothed hand motion. mode kHardThrow: the native camera-directed throw; the client
// sends its camera-forward unit vector and the host applies the game's formula with the real mass
// and the puppet's velocity. The clump re-piles itself on landing either way.
namespace throw_mode { constexpr uint8_t kRelease = 0; constexpr uint8_t kHardThrow = 1; }
struct ThrowIntentPayload {
    uint32_t eid;        // the trash entity eid the client requests to throw (must be the one it holds)
    uint8_t  mode;       // throw_mode::kRelease (E) | kHardThrow (LMB)
    uint8_t  _pad[3];    // align dir to 4; bytes zero
    float    dirX, dirY, dirZ;  // kHardThrow ONLY: client camera-forward unit vector at the press (zero for kRelease)
};
static_assert(sizeof(ThrowIntentPayload) == 20, "ThrowIntentPayload must be 20 bytes (eid+mode+pad+dir)");
static_assert(sizeof(ThrowIntentPayload) <= 256 - 20 - 8, "ThrowIntentPayload must fit one datagram");

// PileResyncRequest has no body; the sender slot says whom to re-stream to. No handler exists yet.
struct PileResyncRequestPayload {
    uint8_t _pad[8];     // no payload body; kept 8 bytes for a uniform minimum datagram
};
static_assert(sizeof(PileResyncRequestPayload) == 8, "PileResyncRequestPayload must be 8 bytes");

// A position correction (PropSnapPos): the eid and the host's current transform; the client snaps
// its bound native at the quiescence sweep. Identity is preserved; idempotent.
struct PropSnapPosPayload {
    uint32_t eid;                       // the save-authoritative pile eid to reposition
    float    locX, locY, locZ;          // host's CURRENT authoritative world position (cm)
    float    rotPitch, rotYaw, rotRoll; // host's CURRENT authoritative rotation (deg)
};
static_assert(sizeof(PropSnapPosPayload) == 28, "PropSnapPosPayload must be 28 bytes (eid + loc + rot)");
static_assert(sizeof(PropSnapPosPayload) <= 256 - 20 - 8, "PropSnapPosPayload must fit one datagram");

// The world clock (TimeSync and ClockPose): the cycle's within-day clock, its accumulator and rate,
// plus the named hour, minute and day the HUD reads (a client at TimeScale 0 never runs its own
// minute pulse).
struct TimeSyncPayload {
    float totalTime;   // within-day clock [0, MaxTime) -- the sun/moon derive from this every tick
    float day;         // the within-day ACCUMULATOR (midnight cascade threshold), NOT the day number
    float timeScale;   // clock advance rate (so the client advances at the host's rate)
    int32_t hour;
    int32_t minute;
    int32_t dayZ;
};
static_assert(sizeof(TimeSyncPayload) == 24, "TimeSyncPayload must be 24 bytes");

// The clock stream datagram (MsgType::ClockPose): the same payload as the reliable connect-edge
// TimeSync, on the unreliable channel, newest wins.
struct ClockPosePacket {
    PacketHeader    header;  // 20
    TimeSyncPayload clock;   // 24
};
static_assert(sizeof(ClockPosePacket) == 44, "ClockPosePacket must be 44 bytes");

// The desk simulation's outputs (MsgType::DeskSimPose), host-owned and streamed newest-wins; the
// client overwrites its own.
struct DeskSimSnapshot {
    float decoded;    // 4 -- DL_SignalDownloadDLData.decoded (progress)
    float resDetec;   // 4 -- DL_resDetecPercent (needle)
    float rate;       // 4 -- DL_downloading (0 = idle)
    float frData;     // 4 -- DL_frData (freq-match)
    float poData;     // 4 -- DL_poData (polarity-match)
    float frOffset;   // 4 -- DL_FrFilterOffset (knob position)
    float poOffset;   // 4 -- DL_poFilterOffset
};
static_assert(sizeof(DeskSimSnapshot) == 28, "DeskSimSnapshot must be 28 bytes");

struct DeskSimPosePacket {
    PacketHeader   header;  // 20
    DeskSimSnapshot sim;    // 28
};
static_assert(sizeof(DeskSimPosePacket) == 48, "DeskSimPosePacket must be 48 bytes");

// One desk input delta (DeskInput): exactly one field per message; the receiver applies it through
// the field's native side-effect path and primes its own poll baseline.
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
    CoordIsPing = 12,    // bool    coord_isPing edge notification (rising = the presser's ENTER); receivers
                         //         never write it, it is the ping machine's run flag; bookkeeping only
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

// The quick-scan notification (DeskScanEvent): the observed charge, for the log line; mirrors
// replay the visual, the beep rides DeskSndFx.
struct DeskScanEventPayload {
    float observedCooldown;  // 4 -- the presser's post-charge cooldown (diagnostic)
};
static_assert(sizeof(DeskScanEventPayload) == 4, "DeskScanEventPayload must be 4 bytes");

// ---- The desk audio-effect forward (DeskSndFx) ----
//
// The component index is a compile-time wire contract, never discovery order: both peers map the
// index to the same property name on the desk screen class through the static table in
// ue_wrap/desk_audio.cpp. The order below is frozen.
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

// One deck playback edge (PlayDeckEvent).
struct PlayDeckEventPayload {
    uint8_t  op;           // 1 -- 0=play 1=stop
    uint8_t  _pad[3];      // 3
    int32_t  selectIndex;  // 4 -- play: the presser's validated play_selectIndex; stop: -1
    uint32_t gen;          // 4 -- play: the minted playback generation; stop: the gen it ends
};
static_assert(sizeof(PlayDeckEventPayload) == 12, "PlayDeckEventPayload must be 12 bytes");

// The desk modules lane (PhysModsState).
struct PhysModsStatePayload {
    uint8_t op;         // 1 -- 0=plug 1=unplug (peer->host) 2=canonical 3=deny
    uint8_t byte;       // 1 -- ops 0/1: the module byte; op 3: the ORIGINAL op
    uint8_t byte2;      // 1 -- op 3: the denied module byte; else 0
    uint8_t _pad;       // 1
    uint8_t bytes[12];  // 12 -- op 2: the canonical array; else zero
};
static_assert(sizeof(PhysModsStatePayload) == 16, "PhysModsStatePayload must be 16 bytes");

// One drive-slot state line (DriveSlotState). role: 0 the desk play slot, 1 the desk comp slot,
// 2 the eraser slot. occupied 1 names the slotted drive; 0 means empty, with driveEid the last
// occupant for the latch completion.
struct DriveSlotStatePayload {
    uint8_t  role;       // 1 -- ue_wrap::drive_chain::kRole*
    uint8_t  occupied;   // 1
    uint16_t censusIdx;  // 2 -- eraser census index (0 today)
    uint32_t driveEid;   // 4
};
static_assert(sizeof(DriveSlotStatePayload) == 8, "DriveSlotStatePayload must be 8 bytes");

// The RackState blob head, followed by the row payload.
struct RackStateHead {
    uint32_t rackEid;  // 4
    uint8_t  op;       // 1 -- 0=set{idx,row} 1=take{idx} 2=deny{idx, orig op in _pad0}
                       //      3=canonical{16 x (u8 has + row)}
    uint8_t  idx;      // 1
    uint8_t  _pad0;    // 1 -- op 2: the denied ORIGINAL op
    uint8_t  _pad1;    // 1
};
static_assert(sizeof(RackStateHead) == 8, "RackStateHead must be 8 bytes");

// The dish pose stream (MsgType::DishPose): movers-only rows at 4 Hz while any dish slews, then
// full sweeps as a settle tail. Angles are relative: the yaw of the Z axis and the roll of the Y
// axis, the native loop's own channels. One applier with the DishSnapshot join row.
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

// The download arm edge (DishArm), host-authored: armed 1 carries the decoded value and the
// host-rolled polarity; 0 disarms.
struct DishArmPayload {
    uint8_t armed;      // 1
    uint8_t _pad[3];    // 3
    float   decoded;    // 4 -- arm-time initializer only (the 10 Hz sim stream is
                        //      the standing authority; staleness heals <=100 ms)
    int32_t polarity;   // 4 -- host-rolled
};
static_assert(sizeof(DishArmPayload) == 12, "DishArmPayload must be 12 bytes");

// The joiner's dish seed (DishSnapshot).
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

// The calibration batch (DishCalib): absolute values from the peer whose local values changed.
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

// --- The tape caddy and the daily task ---

// One reel slot edge (ReelSlot): reel 0 big, 1 small; op 0 insert (progress valid), 1 eject.
struct ReelSlotPayload {
    float   progress;   // 4 -- INSERT: the value entering the unit (0..100); EJECT: last value
    uint8_t reel;       // 1 -- 0 = reelBig @0x288, 1 = reelSmall @0x28C
    uint8_t op;         // 1 -- 0 = INSERT (-1 -> P), 1 = EJECT (P -> -1)
    uint8_t _pad[2];    // 2
};
static_assert(sizeof(ReelSlotPayload) == 8, "ReelSlotPayload must be 8 bytes");

// The reel corrector (MsgType::ReelPose): newest wins by header seq. A channel value of -1 means an
// empty slot and is never applied; slot transitions belong to ReelSlot alone.
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

// The night sky (SkyState): the star dome's world rotation (its random initial yaw plus spin) and
// the moon phase; the client writes both.
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

// Fog active-state bits (WeatherStatePayload::flags2), distinct from the config bits in flags:
// fog is rendered by event actors, so the host stamps their presence and the client asserts it.
namespace fog_flags2 {
inline constexpr uint8_t kFogActive      = 0x01;  // host has a live rolling-fog actor (AweatherFogController_C @ cycle->fogEventObject)
inline constexpr uint8_t kSuperFogActive = 0x02;  // host has a live AsuperFog_C
inline constexpr uint8_t kPermanentFog   = 0x04;  // host's permanentFog gamerule (re-arms the scheduler)
inline constexpr uint8_t kWindValid      = 0x08;  // the wind fields were read from a live host wind actor; the client applies wind only when set
}  // namespace fog_flags2

// The red sky (RedSky): the receiver runs the same gamemode chain (spawnRedSky first, then set).
struct RedSkyPayload {
    // The sender's Player element id; the receiver requires slot 0.
    uint32_t senderElementId;
    uint8_t  state;          // 0 = revert color curves, 1 = red
    uint8_t  _pad[3];        // zero
};
static_assert(sizeof(RedSkyPayload) == 8, "RedSkyPayload must be 8 bytes");
static_assert(sizeof(RedSkyPayload) <= 256 - 20 - 8,
              "RedSkyPayload must fit in one reliable datagram");

// A lightning strike (LightningStrike): the strike's world location; the receiver spawns the
// strike actor there, and it destroys itself.
struct LightningStrikePayload {
    // The sender's Player element id; the receiver requires slot 0.
    uint32_t senderElementId;
    float    locX, locY, locZ; // world cm
};
static_assert(sizeof(LightningStrikePayload) == 16, "LightningStrikePayload must be 16 bytes");
static_assert(sizeof(LightningStrikePayload) <= 256 - 20 - 8,
              "LightningStrikePayload must fit in one reliable datagram");

// The loading-screen brackets: SnapshotBegin carries the candidate count (the denominator),
// SnapshotComplete the count actually sent.
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

// Fill a header in place. senderEpoch is the sender's per-process epoch; senderSlot the logical
// origin (direct sends pass their own slot, the relay passes the true origin).
inline void WriteHeader(PacketHeader& h, MsgType type, uint32_t seq,
                        uint32_t senderEpoch, uint8_t senderSlot = 0) {
    h.magic = kMagic;
    h.version = kProtocolVersion;
    h.type = static_cast<uint8_t>(type);
    h._pad = 0;
    h.seq = seq;
    h.senderEpoch = senderEpoch;
    h.senderSlot = senderSlot;
    // 0 = not stamped. Only the pose stream stamps a state time, right after this call.
    WriteStateTimeMs24(h, 0);
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
    // Angles are canonical FRotator axes in (-180, 180]; senders normalise at the wire boundary.
    // The engine's control rotation is unnormalised (looking down reads 350), so the range is the
    // full axis, not a small pitch band.
    if (p.yaw          < -180.f || p.yaw          > 180.f) return false;
    if (p.pitch        < -180.f || p.pitch        > 180.f) return false;
    if (p.headYawDelta < -180.f || p.headYawDelta > 180.f) return false;
    return true;
}

}  // namespace coop::net
