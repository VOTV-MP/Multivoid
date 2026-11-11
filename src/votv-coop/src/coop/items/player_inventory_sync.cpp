// coop/items/player_inventory_sync.cpp -- see coop/items/player_inventory_sync.h.

#include "coop/items/player_inventory_sync.h"

#include "coop/net/blob_chunks.h"
#include "coop/config/config.h"
#include "coop/items/inventory_wire.h"
#include "coop/items/player_profile.h"
#include "coop/player/player_profile_store.h"
#include "coop/player/players_registry.h"
#include "coop/save/save_scratch.h"  // IsScratch: the join capture is not the hosted world saved
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/player_handshake.h"
#include "ue_wrap/actors/effects.h"  // RemovePlayerEffectRecords, the host's effects out of the joiner's save
#include "ue_wrap/engine/engine.h"      // SetSaveObjectReadyHook, the pre-materialise apply point
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/player_record.h"
#include "ue_wrap/actors/puppet.h"  // ReadCharacterIsFalling
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile_names.h"  // the start point
#include "ue_wrap/engine/save_capture.h"  // the moment the world is gathered into the save object
#include "ue_wrap/engine/save_folder.h"  // SaveNameOf: the save a written folder belongs to
#include "ue_wrap/engine/save_write_hook.h"  // the moment the host's world is saved

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace coop::player_inventory_sync {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// Host: the reassembler of the clients' streams. What it completes goes to
// coop/player/player_profile_store, which holds it by GUID.
coop::blob_chunks::Assembler g_assembler;
coop::inventory_wire::PartJoiner g_hostParts;  // puts a client's profile sent in parts back together

// Client: the send-dedup state.
uint64_t          g_lastItemsHash = 0;  // the inventory component's stream at the last profile sent
Clock::time_point g_lastSend{};
uint64_t          g_oversizeHash = 0;  // the state already reported as too large to send
uint32_t          g_sendSeq = 0;
uint16_t          g_partGroup = 0;  // the group of the next profile sent in parts (coop/items/inventory_wire.h)
coop::player_inventory_sync::SendStats g_lastSendStats{};  // the last profile the transport took
Clock::time_point g_lastPoll{};
Clock::time_point g_lastSweep{};

// Host: the two counts a reader waits on rising (ProfilesReceived, WorldSavesWritten). Game thread.
uint32_t g_profilesReceived  = 0;
uint32_t g_worldSavesWritten = 0;

constexpr auto kClientPoll  = std::chrono::seconds(1);
// The vitals drain every second and the player walks, so they would re-send the whole profile
// at the poll rate. They ride a change of the items at once, and on their own at this cadence,
// which is the most a crash or a kill of the client can cost them.
constexpr auto kVitalsCadence = std::chrono::seconds(30);
constexpr auto kAsmTtl      = std::chrono::seconds(30);

// The live per-player apply on join. The host pushes each joiner its profile (the one the store
// holds, else <mod data dir>/coop_players/<slot>/<guid>.json); the client buffers it and the
// pre-materialise save-object hook substitutes its record for the player's record in the freshly
// loaded save object before the native load builds the live world from it, so the joiner gets
// their own items and vitals, not the host's. The loaded save object is a capture of the HOST's,
// so the player record it carries is never this player's: with no blob the hook takes it out
// rather than keep it.

// Client: the host's on-join apply blob, reassembled here (a separate assembler from the
// host's receive path, since host-to-client is a distinct stream direction).
coop::blob_chunks::Assembler g_clientAssembler;
coop::inventory_wire::PartJoiner g_clientParts;
coop::player_profile::Profile g_pendingApply;
// Client: where the join's world appearance puts this player, and the pose for a ProfilePose; the
// placement reads both once (TakeJoinPlacement).
coop::player_profile::Pose g_joinPose;
JoinPlacement g_joinPlacement = JoinPlacement::StartPoint;

bool JoinStaysAtHost() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::join_at_host);
    return s;
}
std::atomic<bool> g_hasPendingApply{false};
// Client: this session's world was built from a host profile. The stream to the host is gated on
// it, because a world that came up without one carries nothing of the player's, and streaming that
// would overwrite the good profile on the host with an empty one.
bool g_profileApplied = false;
// Client: the next save object to come ready is a JOIN's. Set by the join boot on its own thread
// right before it loads a world, consumed by the hook. Without it the hook cannot tell a join from
// any other load in this process: the session's role outlives a stopped session, so "I am a
// client" stayed true through a later Host-with-save, whose load the hook would then have emptied.
std::atomic<bool> g_joinApplyArmed{false};

// Host: the per-slot send sequence for the on-join push (independent of the client stream's
// sequence space; assembler keys are per sender and sequence, so they never collide).
std::array<uint32_t, coop::net::kMaxPeers> g_hostSendSeq{};
std::array<uint16_t, coop::net::kMaxPeers> g_hostPartGroup{};  // the group of the next profile pushed in parts
// Host: the per-slot pushed-this-connection latch. The push must land in the joiner's
// pre-world window (the connect-replay edge fires at world-ready, too late), so it is driven
// from the host tick the instant the slot is connected and its join GUID has arrived, well
// before the client finishes its save transfer and load. Reset on disconnect.
std::array<bool, coop::net::kMaxPeers> g_applySentToSlot{};

// Client: the last place the local player was standing (see player_profile::Pose). Sampled at
// the poll, kept while the player drives, falls or lies ragdolled.
coop::player_profile::Pose g_standingPose;

void SampleStandingPose() {
    void* pawn = coop::players::Registry::Get().Local();
    if (!pawn) return;
    bool ragdoll = false, dead = false;
    void* seat = nullptr;
    if (!ue_wrap::engine::ReadMainPlayerRagdollState(pawn, ragdoll, dead) || ragdoll || dead) return;
    if (!ue_wrap::engine::ReadMainPlayerSittingOn(pawn, seat) || seat) return;
    if (ue_wrap::puppet::ReadCharacterIsFalling(pawn)) return;
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};   // unread: the last standing pose stays
    if (!ue_wrap::engine::TryGetActorLocation(pawn, at) || !ue_wrap::engine::TryGetActorRotation(pawn, rot)) return;
    g_standingPose = {at.X, at.Y, at.Z, rot.Yaw, true};
}

// Client: send the profile -- the player's own save record, the vitals and the pose -- to the host.
// True when the transport took it. The caller owns the poll and the dedup; this owns the record
// capture, the vitals read, the size ceiling, the send and the dedup state a send leaves behind.
static bool SendProfile(coop::net::Session* s, void* pawn, uint64_t itemsHash, Clock::time_point now) {
    coop::player_profile::Profile p;
    if (!ue_wrap::player_record::CaptureLocal(pawn, p.record)) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGE("player_inventory[client]: the player's save record does not read -- nothing is streamed");
        }
        return false;
    }
    // The vitals not reading (a field the game renamed) must not stop the record from being stored:
    // the profile then goes without them, and the join reads the record's own.
    p.hasVitals = ue_wrap::vitals::ReadSnapshot(p.vitals);
    // The gamemode's pawn and the save object come up a moment after the pawn the poll found: until they
    // do the vitals are not readable YET, and the profile waits for the next poll. Past a bound the wait is
    // a fault and the profile goes without them, as for a row that does not resolve.
    static int s_notUp = 0;
    if (p.hasVitals) s_notUp = 0;
    else if (!ue_wrap::vitals::SnapshotSourcesUp() && ++s_notUp < 30) return false;
    if (!p.hasVitals) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGE("player_inventory[client]: the vitals do not read -- the profile is streamed "
                    "WITHOUT them");
        }
    }
    p.pose = g_standingPose;
    const std::vector<uint8_t> blob = coop::inventory_wire::Serialize(p);
    const std::vector<std::vector<uint8_t>> parts =
        coop::inventory_wire::SplitParts(blob, coop::blob_chunks::MaxBlobBytes(), ++g_partGroup);
    if (parts.empty()) {
        // Past even the parts' ceiling (coop::inventory_wire::kMaxParts transport blobs of the deflated profile), and a
        // record is not a list whose tail may be dropped. Said once per state of the items: the host keeps the last
        // profile that fitted.
        if (itemsHash != g_oversizeHash) {
            g_oversizeHash = itemsHash;
            UE_LOGE("player_inventory[client]: the profile is %zu bytes (record %zu), past the %zu-byte ceiling of "
                    "%zu parts -- NOT sent; the host keeps the last one that fitted",
                    blob.size(), p.record.size(),
                    coop::blob_chunks::MaxBlobBytes() * coop::inventory_wire::kMaxParts,
                    coop::inventory_wire::kMaxParts);
        }
        return false;
    }
    bool sentAll = true;
    for (const std::vector<uint8_t>& part : parts)
        if (!coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::PlayerInventoryBlob, ++g_sendSeq, part)) {
            sentAll = false;
            break;
        }
    if (sentAll) {
        // Logged for a change of the items only: the vitals cadence is steady state, not an event.
        if (itemsHash != g_lastItemsHash)
            UE_LOGI("player_inventory[client]: streamed profile (%zu bytes in %zu transport blob(s), record %zu) to host",
                    blob.size(), parts.size(), p.record.size());
        g_lastItemsHash = itemsHash;
        g_lastSend = now;
        g_lastSendStats.count++;
        g_lastSendStats.wireBytes = blob.size();
        g_lastSendStats.recordBytes = p.record.size();
        g_lastSendStats.recordHash = coop::blob_chunks::Fnv64(p.record);
        return true;
    }
    return false;  // refused -> retry next poll under a fresh seq
}

// Client: poll the inventory component at 1 Hz and stream the profile to the host. A change of
// the items goes at once; the vitals and the pose alone go at kVitalsCadence, and the record is not
// even captured on a poll that will not send.
void ClientStreamTick(coop::net::Session* s) {
    const Clock::time_point now = Clock::now();
    if (now - g_lastPoll < kClientPoll) return;
    g_lastPoll = now;
    if (!g_profileApplied) return;  // this world holds no profile of ours to report
    void* const pawn = coop::players::Registry::Get().Local();
    std::vector<uint8_t> items;
    if (!pawn || !ue_wrap::player_record::InventoryBytes(pawn, items)) return;  // world not up yet
    SampleStandingPose();
    const uint64_t itemsHash = coop::blob_chunks::Fnv64(items);
    if (itemsHash == g_lastItemsHash && now - g_lastSend < kVitalsCadence) return;
    SendProfile(s, pawn, itemsHash, now);
}

// Host: sweep stale half-assemblies and push each newly connected joiner its per-player apply
// blob once (the pre-world connect edge). Nothing is written to disk from here: the store is cut
// when the host's world is saved, and only then.
void HostPersistTick(coop::net::Session* s) {
    const Clock::time_point now = Clock::now();
    if (now - g_lastSweep < std::chrono::seconds(1)) return;
    g_lastSweep = now;
    g_assembler.Sweep(now, kAsmTtl);
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        // The connect-edge push: once a slot is connected and its GUID has arrived (carried in the
        // join), send it its persisted profile once, latching on a successful enqueue only, so a
        // channel-busy refusal or a not-yet-arrived GUID retries on the next 1 Hz tick. Reliable
        // delivery carries the single blob to the joiner during its pre-world wait, where the
        // receiver, installed before any world, buffers it for the save-object hook.
        if (!g_applySentToSlot[slot] && s->IsSlotConnected(slot) &&
            !coop::player_handshake::GuidForSlot(slot).empty()) {
            if (SendInventoryToSlot(slot)) g_applySentToSlot[slot] = true;
        }
    }
}

// Client: the save object's player takes the class's own values: its record leaves the save
// object, so loadObjects leaves the level's pawn as its class made it (the class's vitals, an
// empty inventory), and the save object's own per-player fields take their defaults. The defaults
// are read first: the record is where a class that is not loaded yet is loaded from.
bool StartAfresh(void* saveSlotObject) {
    ue_wrap::vitals::Snapshot fresh;
    const bool haveDefaults = ue_wrap::vitals::ReadDefaults(fresh, saveSlotObject);
    const int removed = ue_wrap::player_record::RemoveFromSave(saveSlotObject);
    const bool savePart = haveDefaults && ue_wrap::vitals::ApplySnapshot(saveSlotObject, fresh);
    UE_LOGI("player_inventory[client]: %d player record(s) left the save object; its per-player fields %s",
            removed, savePart ? "take the defaults" : "were NOT written (the host's)");
    return removed >= 0;
}

// Client: the engine's pre-materialise hook, registered in Install. Fires on the game thread
// with the freshly loaded save object, before the native load builds the world from it, the
// one window to substitute this client's record, and to reset the host's status effects out
// of it. Fires for every load in the process, so it acts only on the one the join boot armed
// (BeginJoinApply), once. The join boot waits for the blob; if it still is not here, the host's
// record leaves the save object all the same -- it is the host's, not data of this player's that
// could be lost -- and the stream stays shut for the session, so the profile on the host survives
// for the next join.
void OnSaveObjectReady(void* saveSlotObject) {
    if (!g_joinApplyArmed.exchange(false, std::memory_order_acq_rel)) return;  // not a join's load
    // The host's status effects are records among its world's in this save object, and a player
    // joins without them: they are taken out before the world is built, ahead of both branches
    // below.
    // Divergence from MTA, which resets a joiner's own state at the join (Event_OnIngame ->
    // ResetStats, reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:3452-3515) and
    // composes what a joiner receives per recipient on its server
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:1436-1483). Here the records
    // are removed at the receiver: the join streams the canonical on-disk slot when the live
    // capture fails (src/votv-coop/src/coop/save/save_transfer.cpp:370,423) and the capture
    // serialises the host's own live save object
    // (src/votv-coop/src/ue_wrap/engine/save_capture.cpp:178-181), which the host must keep.
    const int effectsRemoved = ue_wrap::effects::RemovePlayerEffectRecords(saveSlotObject);
    if (effectsRemoved > 0)
        UE_LOGI("player_inventory[client]: left the host's status effects out of this player's world "
                "(%d record(s) removed)", effectsRemoved);
    else if (effectsRemoved < 0)
        UE_LOGW("player_inventory[client]: the host's effect records were not removed -- this player may "
                "start with the host's effects");
    g_profileApplied = false;
    g_joinPose = {};
    g_joinPlacement = JoinStaysAtHost() ? JoinPlacement::AtHost : JoinPlacement::StartPoint;
    namespace N = ue_wrap::profile::name;
    if (!g_hasPendingApply.load(std::memory_order_acquire)) {
        const bool emptied = StartAfresh(saveSlotObject);
        if (g_joinPlacement == JoinPlacement::StartPoint)
            ue_wrap::vitals::WritePlayerTransform(saveSlotObject, N::kKPPSpawnX, N::kKPPSpawnY,
                                                  N::kKPPSpawnZ, 0.f);
        UE_LOGE("player_inventory[client]: SaveObjectReady with no profile from the host -- %s; this "
                "session plays empty-handed and reports nothing back, the profile on the host is untouched",
                emptied ? "the host's record left the save object" : "the save object could not be written");
        return;
    }
    const coop::player_profile::Profile& prof = g_pendingApply;
    // A player who left dead, whose numbers are not numbers, or whose profile holds no vitals
    // starts a fresh life at the start point: health 0 applied as stored would kill them again on
    // the first frame.
    const ue_wrap::vitals::Snapshot& pv = prof.vitals;
    const bool alive = prof.hasVitals && std::isfinite(pv.health) && std::isfinite(pv.maxHealth) &&
                       std::isfinite(pv.food) && std::isfinite(pv.sleep) && pv.health > 0.f;
    bool vitalsOk = false;
    if (prof.record.empty()) {
        // A first join: the player starts as a new game's does, with the game's own starting
        // equipment, which the inventory's first construction hands out (addStartupEquipment). The
        // copy that arms it needs the host's record still in the save object.
        const bool kit = ue_wrap::player_record::CopyStartupEquipment(saveSlotObject);
        vitalsOk = StartAfresh(saveSlotObject);
        UE_LOGI("player_inventory[client]: first join -- %s", kit ? "the game's starting equipment is armed"
                                                                 : "the starting equipment did NOT arm (empty-handed)");
    } else {
        const int written = ue_wrap::player_record::WriteToSave(saveSlotObject, prof.record);
        if (written <= 0) {
            UE_LOGE("player_inventory[client]: the save object %p has no player record to take this player's "
                    "(%d) -- the world comes up with the host's, and the stream stays shut", saveSlotObject, written);
            return;
        }
        ue_wrap::vitals::Snapshot v = pv;
        vitalsOk = (alive || ue_wrap::vitals::ReadDefaults(v, saveSlotObject)) &&
                   ue_wrap::vitals::ApplySnapshot(saveSlotObject, v);
        if (!vitalsOk)
            UE_LOGE("player_inventory[client]: the vitals were NOT written -- this player starts at the "
                    "record's own%s", alive ? "" : ", which left dead");
    }
    // The pose is a world position off the wire: finite and inside the bound every other world
    // position on the wire is held to (an extreme finite coordinate still asserts in the engine).
    const auto& pose = prof.pose;
    auto inWorld = [](float c) { return std::isfinite(c) && std::fabs(c) <= coop::net::kMaxCoord; };
    if (alive && pose.valid && inWorld(pose.x) && inWorld(pose.y) && inWorld(pose.z) &&
        std::isfinite(pose.yaw))
        g_joinPose = pose;
    // Where the game itself believes this player is: its anti-noclip check falls back there, and
    // in a join capture that is where the HOST stood.
    if (g_joinPlacement != JoinPlacement::AtHost && g_joinPose.valid) g_joinPlacement = JoinPlacement::ProfilePose;
    bool placed = true;
    if (g_joinPlacement == JoinPlacement::ProfilePose)
        placed = ue_wrap::vitals::WritePlayerTransform(saveSlotObject, g_joinPose.x, g_joinPose.y,
                                                       g_joinPose.z, g_joinPose.yaw);
    // The start point's own spot (the base bed, shelter_spot) needs the world to find, and the world
    // does not exist yet: the save gets the KPP, and the appearance moves the player to the bed's spot.
    else if (g_joinPlacement == JoinPlacement::StartPoint)
        placed = ue_wrap::vitals::WritePlayerTransform(saveSlotObject, N::kKPPSpawnX, N::kKPPSpawnY,
                                                       N::kKPPSpawnZ, 0.f);
    g_profileApplied = true;
    UE_LOGI("player_inventory[client]: applied per-player profile to save object %p (record %zu bytes | "
            "vitals %s: hp=%.0f/%.0f food=%.0f sleep=%.0f | pose %s%s)",
            saveSlotObject, prof.record.size(),
            !vitalsOk ? "NOT WRITTEN" : alive ? "restored" : "fresh", pv.health, pv.maxHealth, pv.food,
            pv.sleep,
            g_joinPlacement == JoinPlacement::AtHost        ? "the host's (join_at_host)"
            : g_joinPlacement == JoinPlacement::ProfilePose ? "restored"
                                                            : "start point",
            placed ? "" : ", playerTransform NOT written");
}

// Host: the game is gathering the world into its save object (ue_wrap/engine/save_capture: its own
// save, or a join capture). The profiles as they stand now are the ones consistent with that
// world.
bool g_gatherSeen = false;  // since the last world save
void OnWorldGather() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    g_gatherSeen = true;
    coop::player_profile_store::MarkWorldGathered();
}

// Host: the engine reported the world's container written (ue_wrap/engine/save_write_hook; the thread
// that saved, which is the game thread). Only THE world counts: the live save object, in a save of the
// game's own. The save-slot menu writes regenerated and copied save objects, and the join capture writes
// the live one to a scratch save; neither is a moment the host's world on disk changed. The profiles are
// cut under the name of the save written to, the folder the container went into.
void OnWorldSaveWritten(const ue_wrap::save_write_hook::Write& write) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    if (coop::save_scratch::IsScratch(write.folder)) return;
    const std::wstring slotName = ue_wrap::save_folder::SaveNameOf(write.folder);
    if (void* live = ue_wrap::inventory::ResolveSaveSlot(); write.object != live) {
        // The save-slot menu writes save objects of its own; a mismatch that is NOT that would be a
        // stale gamemode behind the resolver, and a cut skipped for it must be visible.
        UE_LOGI("player_inventory: a world save of %p to '%ls' is not the hosted world's save object "
                "(%p) -- no profiles cut", write.object, slotName.c_str(), live);
        return;
    }
    // No gather seen since the last save: either the game's event gate skipped it, and then the
    // file holds the last gather's world and what was set aside THEN is what belongs beside it --
    // or the gather watch is not standing, and then a save with no event on did gather. Only a
    // positive "no event" is that second case; "cannot ask" writes nothing newer.
    if (!g_gatherSeen && coop::player_profile_store::AnythingPending() &&
        ue_wrap::save_capture::GameEventState() == ue_wrap::save_capture::EventState::Off) {
        UE_LOGW("player_inventory: the host's world was saved with no gather seen and no event on "
                "-- the gather watch is not standing; taking the profiles as of now");
        coop::player_profile_store::MarkWorldGathered();
    }
    g_gatherSeen = false;
    const Clock::time_point t0 = Clock::now();
    const size_t n = coop::player_profile_store::CutToDisk(slotName);
    UE_LOGI("player_inventory: the host's world was saved to '%ls' -- %zu changed profile(s) cut to "
            "disk with it in %lld us", slotName.c_str(), n, static_cast<long long>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count()));
    ++g_worldSavesWritten;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    ue_wrap::save_write_hook::AddWritten(&OnWorldSaveWritten);
    ue_wrap::save_capture::SetWorldGatherHook(&OnWorldGather);
    // Arm the engine's pre-materialise apply point (a no-op until a client join has a pending
    // blob). Re-arming across stop and start is harmless.
    ue_wrap::engine::SetSaveObjectReadyHook(&OnSaveObjectReady);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    // Host: the game's own saves have to be seen from the first one, with or without a client
    // connected. 1 Hz until it stands; the classes it stands on load with the world, and the
    // wrapper makes a failure that cannot heal final.
    if (s && s->running() && s->role() == coop::net::Role::Host) {
        static bool s_gatherWatch = false;
        static Clock::time_point s_nextTry{};
        if (!s_gatherWatch) {
            const Clock::time_point now = Clock::now();
            if (now >= s_nextTry) {
                s_nextTry = now + std::chrono::seconds(1);
                s_gatherWatch = ue_wrap::save_capture::InstallGatherWatch();
            }
        }
    }
    if (s && s->connected()) {
        if (s->role() == coop::net::Role::Client) ClientStreamTick(s);
        else                                      HostPersistTick(s);
    }

    // The read-verify selftest (ini inventory_selftest=1). One-shot; a dev diagnostic.
    static const bool s_selftest = ::coop::config::ResolveFlag(::coop::config_registry::rows::inventory_selftest);
    if (!s_selftest) return;
    static bool s_done = false;
    if (s_done) return;
    static int s_ticks = 0;
    if (++s_ticks < 600) return;  // ~5s settle (world up + pawn resolvable)
    ue_wrap::inventory::PlayerInventory inv;
    if (!ue_wrap::inventory::ReadAll(inv)) return;  // world not up yet -> retry next tick
    s_done = true;
    coop::player_profile::Profile mine;
    void* const pawn = coop::players::Registry::Get().Local();
    const bool haveRecord = pawn && ue_wrap::player_record::CaptureLocal(pawn, mine.record);
    mine.hasVitals = ue_wrap::vitals::ReadSnapshot(mine.vitals);
    mine.pose = g_standingPose;
    UE_LOGI("inventory[selftest]: read the local player -- carried=%zu worn=%zu record=%s (%zu bytes)",
            inv.inventory.size(), inv.equipment.size(), haveRecord ? "read" : "NOT read", mine.record.size());
    {   // DIAG: name what the LOCAL player actually has post-spawn. Explicit, because an implicit
        // narrowing copy mangles every non-ASCII character into whatever its low byte happened to
        // be. These are engine class and key leaves, ASCII in practice -- so a surprising name must
        // be VISIBLE in the log as '?' rather than quietly become a different name.
        auto narrow = [](const std::wstring& w) {
            std::string s;
            s.reserve(w.size());
            for (const wchar_t c : w) s.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
            return s;
        };
        for (size_t i = 0; i < inv.inventory.size(); ++i)
            UE_LOGI("  selftest carried[%zu]: className='%s' key='%s'", i,
                    narrow(inv.inventory[i].className).c_str(), narrow(inv.inventory[i].key).c_str());
        for (size_t i = 0; i < inv.equipment.size(); ++i)
            UE_LOGI("  selftest worn[%zu]: className='%s' key='%s'", i,
                    narrow(inv.equipment[i].className).c_str(), narrow(inv.equipment[i].key).c_str());
    }
    const std::vector<uint8_t> blob1 = coop::inventory_wire::Serialize(mine);
    coop::player_profile::Profile back;
    const bool de = coop::inventory_wire::Deserialize(blob1, back);
    std::vector<uint8_t> blob2;
    if (de) blob2 = coop::inventory_wire::Serialize(back);
    const bool roundtrip = de && blob1 == blob2;
    UE_LOGI("inventory[selftest]: serialize=%zu bytes, deserialize=%d, ROUND-TRIP %s (record %zu bytes)",
            blob1.size(), de ? 1 : 0, roundtrip ? "OK" : "MISMATCH", back.record.size());
}

bool SendProfileNow(coop::net::Session* s) {
    if (!s || !s->connected() || !g_profileApplied) return false;
    void* const pawn = coop::players::Registry::Get().Local();
    std::vector<uint8_t> items;
    if (!pawn || !ue_wrap::player_record::InventoryBytes(pawn, items)) return false;
    SampleStandingPose();
    // No hash or cadence check: the caller has just changed the vitals the profile carries.
    return SendProfile(s, pawn, coop::blob_chunks::Fnv64(items), Clock::now());
}

void OnReliable(const coop::net::BlobChunkPayload& p, uint8_t senderPeerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;

    // The client direction: the host pushed this client its per-player apply blob.
    if (s->role() == coop::net::Role::Client) {
        if (senderPeerSlot != 0) return;  // only the host pushes the apply blob
        std::vector<uint8_t> part, blob;
        if (!g_clientAssembler.OnChunk(p, senderPeerSlot, part)) return;  // not complete yet
        if (!g_clientParts.Feed(senderPeerSlot, part, blob)) return;      // a part of a profile sent in parts
        coop::player_profile::Profile inv;
        if (!coop::inventory_wire::Deserialize(blob, inv)) {
            UE_LOGW("player_inventory[client]: host apply blob (%zu bytes) failed to deserialize "
                    "-- ignoring (will keep waiting / fall back)", blob.size());
            return;
        }
        g_pendingApply = std::move(inv);
        g_hasPendingApply.store(true, std::memory_order_release);
        UE_LOGI("player_inventory[client]: received per-player apply blob from host (%zu bytes, "
                "record %zu bytes%s)", blob.size(), g_pendingApply.record.size(),
                g_pendingApply.record.empty() ? ": a first join" : "");
        return;
    }

    // The host direction: a client slot streamed its profile to persist.
    if (s->role() != coop::net::Role::Host) return;
    if (senderPeerSlot < 1 || senderPeerSlot >= coop::net::kMaxPeers) return;  // a CLIENT slot
    std::vector<uint8_t> part, blob;
    if (!g_assembler.OnChunk(p, senderPeerSlot, part)) return;  // not complete yet
    if (!g_hostParts.Feed(senderPeerSlot, part, blob)) return;  // a part of a profile sent in parts
    const std::string& guid = coop::player_handshake::GuidForSlot(senderPeerSlot);
    if (guid.empty()) {
        UE_LOGW("player_inventory: inventory blob from slot %u but no GUID -- dropping",
                senderPeerSlot);
        return;
    }
    // Held in memory, by GUID; the disk copy is cut with the host's world save (the store's header
    // says why no other moment will do).
    coop::player_profile_store::Put(guid, std::move(blob),
                                    coop::player_profile_store::NickForJson(
                                        coop::player_handshake::NicknameForSlot(senderPeerSlot)));
    ++g_profilesReceived;
}

SendStats LastSend() { return g_lastSendStats; }

uint32_t ProfilesReceived() { return g_profilesReceived; }

uint32_t WorldSavesWritten() { return g_worldSavesWritten; }

bool SendInventoryToSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return false;  // host pushes; clients receive
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return false;
    const std::string& guid = coop::player_handshake::GuidForSlot(peerSlot);
    if (guid.empty()) {
        UE_LOGI("player_inventory: slot %d has no GUID yet -- not sending an apply blob this edge",
                peerSlot);
        return false;  // not sent -> caller does not latch; retries when the GUID lands
    }
    std::vector<uint8_t> blob;
    namespace PS = coop::player_profile_store;
    const PS::Found found = PS::Get(guid, blob);
    // A profile that is there and does not read (a corrupt or locked file, a blob of a build this
    // one cannot parse) is a RETURNING player: they start afresh for this session, and nothing they
    // stream back may be written over what is on disk.
    coop::player_profile::Profile parsed;
    const bool usable = found == PS::Found::Yes && coop::inventory_wire::Deserialize(blob, parsed);
    if (!usable && found != PS::Found::Absent) PS::Quarantine(guid);
    if (!usable) {
        // First join (nothing held and no stored file), or the quarantined case above: a profile
        // with no record, which the joiner applies as a new game's player (the game's own starting
        // equipment, the class's vitals), never as the host's items or another player's. The
        // vitals are the game's own defaults, never the host's numbers, and there is no pose: a
        // first join appears at the start point.
        coop::player_profile::Profile first;
        first.hasVitals = ue_wrap::vitals::ReadDefaults(first.vitals);  // else the joiner reads its own
        blob = coop::inventory_wire::Serialize(first);
        UE_LOGI("player_inventory: slot %d guid=%s -- first join, a new game's player", peerSlot, guid.c_str());
    }
    const std::vector<std::vector<uint8_t>> parts =
        coop::inventory_wire::SplitParts(blob, coop::blob_chunks::MaxBlobBytes(), ++g_hostPartGroup[peerSlot]);
    if (parts.empty()) {
        UE_LOGE("player_inventory: slot %d guid=%s -- the stored profile (%zu bytes) is past the %zu parts the transport "
                "carries; the player starts afresh", peerSlot, guid.c_str(), blob.size(),
                coop::inventory_wire::kMaxParts);
        return false;
    }
    bool sentAll = true;
    for (const std::vector<uint8_t>& part : parts)
        if (!coop::blob_chunks::SendBlobToSlot(s, peerSlot, coop::net::ReliableKind::PlayerInventoryBlob,
                                               ++g_hostSendSeq[peerSlot], part)) {
            sentAll = false;
            break;
        }
    if (sentAll) {
        UE_LOGI("player_inventory: sent slot %d guid=%s its per-player profile (%zu-byte blob, %zu transport blob(s))",
                peerSlot, guid.c_str(), blob.size(), parts.size());
        return true;
    }
    UE_LOGW("player_inventory: send to slot %d refused (channel busy) -- will RETRY next tick "
            "(NOT latched as sent)", peerSlot);
    return false;
}

bool HasPendingApply() { return g_hasPendingApply.load(std::memory_order_acquire); }

void BeginJoinApply() { g_joinApplyArmed.store(true, std::memory_order_release); }

JoinPlacement TakeJoinPlacement(float& x, float& y, float& z, float& yaw) {
    const JoinPlacement at = g_joinPlacement;
    if (at == JoinPlacement::ProfilePose) {
        x = g_joinPose.x; y = g_joinPose.y; z = g_joinPose.z; yaw = g_joinPose.yaw;
    }
    // The join's appearance only: a later body in this session is a respawn.
    g_joinPlacement = JoinPlacement::StartPoint;
    g_joinPose = {};
    return at;
}

void OnDisconnectForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    // The leaver's profile stays held by GUID: a rejoin gets it back, and the next save of the
    // host's world cuts it to disk with everyone else's.
    if (peerSlot >= 0 && peerSlot < coop::net::kMaxPeers)
        { g_applySentToSlot[peerSlot] = false; g_hostParts.ClearSlot(static_cast<uint8_t>(peerSlot)); }  // a rejoin re-pushes the apply blob; a half-joined group is gone
}

void OnDisconnect() {
    // Host: nothing to do. This edge is "the last client left", not "the hosted world ended": what
    // is held stays for a rejoin and for the next save (the store drops it when another world is
    // loaded to host).
    // Client: reset the send dedup so a reconnect re-streams, and drop any pending apply blob and
    // its assembler, so a rejoin waits for a fresh host push and never applies a stale inventory
    // to a new world.
    g_lastItemsHash = 0;
    g_lastSend = Clock::time_point{};
    g_joinPose = {};
    g_joinPlacement = JoinPlacement::StartPoint;
    g_standingPose = {};
    g_oversizeHash = 0;
    g_lastPoll = Clock::time_point{};
    g_hasPendingApply.store(false, std::memory_order_release);
    g_joinApplyArmed.store(false, std::memory_order_release);
    g_profileApplied = false;
    g_pendingApply = coop::player_profile::Profile{};
    g_clientAssembler.Clear();
    g_clientParts.Clear();
}

}  // namespace coop::player_inventory_sync
