// coop/items/player_inventory_sync.cpp -- see coop/items/player_inventory_sync.h.

#include "coop/items/player_inventory_sync.h"

#include "coop/net/blob_chunks.h"
#include "coop/config/config.h"
#include "coop/items/inventory_wire.h"
#include "coop/items/player_profile.h"
#include "coop/player/player_profile_store.h"
#include "coop/player/players_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_synth_key.h"  // RandomKeyString, the fresh key of a copied record
#include "coop/session/player_handshake.h"
#include "ue_wrap/actors/begin_equipment.h"  // GiveFromClass, the starter-kit probe
#include "ue_wrap/engine/engine.h"      // SetSaveObjectReadyHook, the pre-materialise apply point
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/puppet.h"  // ReadCharacterIsFalling
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile_names.h"  // the start point
#include "ue_wrap/engine/save_capture.h"  // the moment the world is gathered into the save object
#include "ue_wrap/engine/save_to_slot_hook.h"  // the moment the host's world is saved

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

namespace coop::player_inventory_sync {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// Host: the reassembler of the clients' streams. What it completes goes to
// coop/player/player_profile_store, which holds it by GUID.
coop::blob_chunks::Assembler g_assembler;

// Client: the send-dedup state.
uint64_t          g_lastItemsHash = 0;  // the items half of the last profile sent
Clock::time_point g_lastSend{};
uint64_t          g_oversizeHash = 0;  // the state already reported as too large to send
uint32_t          g_sendSeq = 0;
Clock::time_point g_lastPoll{};
Clock::time_point g_lastSweep{};

constexpr auto kClientPoll  = std::chrono::seconds(1);
// The vitals drain every second and the player walks, so they would re-send the whole profile
// at the poll rate. They ride a change of the items at once, and on their own at this cadence,
// which is the most a crash or a kill of the client can cost them.
constexpr auto kVitalsCadence = std::chrono::seconds(30);
constexpr auto kAsmTtl      = std::chrono::seconds(30);

// The live per-player apply on join. The host pushes each joiner its profile (the one the store
// holds, else coop_players/<slot>/<guid>.json); the client buffers it and the pre-materialise save-object
// hook substitutes it into the freshly loaded save object before the native load builds the
// live world from it, so the joiner gets their own items, not the host's. The loaded save object
// is a capture of the HOST's, so what it carries is never this player's: with no blob the hook
// empties it rather than keep it.

// Client: the host's on-join apply blob, reassembled here (a separate assembler from the
// host's receive path, since host-to-client is a distinct stream direction).
coop::blob_chunks::Assembler g_clientAssembler;
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

// Client: poll the carried, worn and held items at 1 Hz and stream the profile to the host. A
// change of the items goes at once; the vitals and the pose alone go at kVitalsCadence, and are
// not even read on a poll that will not send.
void ClientStreamTick(coop::net::Session* s) {
    const Clock::time_point now = Clock::now();
    if (now - g_lastPoll < kClientPoll) return;
    g_lastPoll = now;
    if (!g_profileApplied) return;  // this world holds no profile of ours to report
    ue_wrap::inventory::PlayerInventory items;
    if (!ue_wrap::inventory::ReadAll(items)) return;  // world not up yet
    SampleStandingPose();
    std::vector<uint8_t> blob = coop::inventory_wire::SerializeItems(items);
    const uint64_t itemsHash = coop::blob_chunks::Fnv64(blob);
    if (itemsHash == g_lastItemsHash && now - g_lastSend < kVitalsCadence) return;

    // The vitals not reading (a field the game renamed) must not stop the ITEMS from being stored:
    // the profile then goes without them and is applied with the game's defaults.
    ue_wrap::vitals::Snapshot vitals;
    const bool haveVitals = ue_wrap::vitals::ReadSnapshot(vitals);
    if (!haveVitals) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGE("player_inventory[client]: the vitals do not read -- the profile is streamed "
                    "WITHOUT them and a rejoin starts from the game's defaults");
        }
    }
    coop::inventory_wire::AppendState(blob, haveVitals ? &vitals : nullptr, g_standingPose);
    if (blob.size() > coop::blob_chunks::MaxBlobBytes()) {
        // The transport cannot carry it, and a profile is not a list whose tail may be dropped.
        // Said once per state of the items: the host keeps the last profile that fitted.
        if (itemsHash != g_oversizeHash) {
            g_oversizeHash = itemsHash;
            UE_LOGE("player_inventory[client]: the profile is %zu bytes (%zu carried), past the "
                    "%zu-byte transport ceiling -- NOT sent; the host keeps the last one that fitted",
                    blob.size(), items.inventory.size(), coop::blob_chunks::MaxBlobBytes());
        }
        return;
    }
    if (coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::PlayerInventoryBlob, ++g_sendSeq, blob)) {
        // Logged for a change of the items only: the vitals cadence is steady state, not an event.
        if (itemsHash != g_lastItemsHash)
            UE_LOGI("player_inventory[client]: streamed profile (%zu bytes, %zu carried) to host",
                    blob.size(), items.inventory.size());
        g_lastItemsHash = itemsHash;
        g_lastSend = now;
    }  // else: refused -> retry next poll under a fresh seq
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
        // join), send it its persisted inventory once, latching on a successful enqueue only, so a
        // channel-busy refusal or a not-yet-arrived GUID retries on the next 1 Hz tick. Reliable
        // delivery carries the single blob to the joiner during its pre-world wait, where the
        // receiver, installed before any world, buffers it for the save-object hook.
        if (!g_applySentToSlot[slot] && s->IsSlotConnected(slot) &&
            !coop::player_handshake::GuidForSlot(slot).empty()) {
            if (SendInventoryToSlot(slot)) g_applySentToSlot[slot] = true;
        }
    }
}

// Client: the engine's pre-materialise hook, registered in Install. Fires on the game thread
// with the freshly loaded save object, before the native load builds the world from it, the
// one window to substitute this client's inventory. Fires for every load in the process, so it
// acts only on the one the join boot armed (BeginJoinApply), once. The join boot waits for the
// blob; if it still is not here, the host's items are emptied out of the save
// object all the same -- they are the host's, not data of this player's that could be lost -- and
// the stream stays shut for the session, so the profile on the host survives for the next join.
void OnSaveObjectReady(void* saveSlotObject) {
    if (!g_joinApplyArmed.exchange(false, std::memory_order_acq_rel)) return;  // not a join's load
    g_profileApplied = false;
    g_joinPose = {};
    g_joinPlacement = JoinStaysAtHost() ? JoinPlacement::AtHost : JoinPlacement::StartPoint;
    if (!g_hasPendingApply.load(std::memory_order_acquire)) {
        const bool emptied = ue_wrap::inventory::ApplyToSaveObject(
            saveSlotObject, ue_wrap::inventory::PlayerInventory{});
        ue_wrap::vitals::Snapshot fresh;
        const bool vitals = ue_wrap::vitals::ReadDefaults(fresh) &&
                            ue_wrap::vitals::ApplySnapshot(saveSlotObject, fresh);
        namespace N = ue_wrap::profile::name;
        if (g_joinPlacement == JoinPlacement::StartPoint)
            ue_wrap::vitals::WritePlayerTransform(saveSlotObject, N::kKPPSpawnX, N::kKPPSpawnY,
                                                  N::kKPPSpawnZ, 0.f);
        UE_LOGE("player_inventory[client]: SaveObjectReady with no profile from the host -- %s, "
                "vitals %s; this session plays empty-handed and reports nothing back, the profile "
                "on the host is untouched",
                emptied ? "emptied the host's items out of the save object"
                        : "the save object could not be written",
                vitals ? "fresh" : "NOT written (the host's)");
        return;
    }
    if (!ue_wrap::inventory::ApplyToSaveObject(saveSlotObject, g_pendingApply.items)) {
        UE_LOGE("player_inventory[client]: ApplyToSaveObject FAILED on %p -- the world comes up "
                "with the host's items and the stream stays shut", saveSlotObject);
        return;
    }
    // A player who left dead, whose numbers are not numbers, or whose profile holds no vitals
    // starts a fresh life at the start point: health 0 applied as stored would kill them again on
    // the first frame.
    ue_wrap::vitals::Snapshot v = g_pendingApply.vitals;
    const bool alive = g_pendingApply.hasVitals && std::isfinite(v.health) &&
                       std::isfinite(v.maxHealth) && std::isfinite(v.food) &&
                       std::isfinite(v.sleep) && v.health > 0.f;
    const bool vitalsOk = (alive || ue_wrap::vitals::ReadDefaults(v)) &&
                          ue_wrap::vitals::ApplySnapshot(saveSlotObject, v);
    if (!vitalsOk)
        UE_LOGE("player_inventory[client]: the vitals were NOT written -- this player starts at "
                "the host's numbers");
    // The pose is a world position off the wire: finite and inside the bound every other world
    // position on the wire is held to (an extreme finite coordinate still asserts in the engine).
    const auto& pose = g_pendingApply.pose;
    auto inWorld = [](float c) { return std::isfinite(c) && std::fabs(c) <= coop::net::kMaxCoord; };
    if (alive && pose.valid && inWorld(pose.x) && inWorld(pose.y) && inWorld(pose.z) &&
        std::isfinite(pose.yaw))
        g_joinPose = pose;
    // Where the game itself believes this player is: its anti-noclip check falls back there, and
    // in a join capture that is where the HOST stood.
    namespace N = ue_wrap::profile::name;
    if (g_joinPlacement != JoinPlacement::AtHost && g_joinPose.valid) g_joinPlacement = JoinPlacement::ProfilePose;
    bool placed = true;
    if (g_joinPlacement == JoinPlacement::ProfilePose)
        placed = ue_wrap::vitals::WritePlayerTransform(saveSlotObject, g_joinPose.x, g_joinPose.y,
                                                       g_joinPose.z, g_joinPose.yaw);
    else if (g_joinPlacement == JoinPlacement::StartPoint)
        placed = ue_wrap::vitals::WritePlayerTransform(saveSlotObject, N::kKPPSpawnX, N::kKPPSpawnY,
                                                       N::kKPPSpawnZ, 0.f);
    g_profileApplied = true;
    UE_LOGI("player_inventory[client]: applied per-player profile to save object %p (carried=%zu "
            "equip=%zu hold=%zu | vitals %s: hp=%.0f/%.0f food=%.0f sleep=%.0f | pose %s%s)",
            saveSlotObject, g_pendingApply.items.inventory.size(),
            g_pendingApply.items.equipment.size(), g_pendingApply.items.hold.size(),
            !vitalsOk ? "NOT WRITTEN" : alive ? "restored" : "fresh", v.health, v.maxHealth, v.food,
            v.sleep,
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

// Host: the engine reported a world save written (ue_wrap/engine/save_to_slot_hook; the thread
// that saved, which is the game thread). Only THE world counts: the live save object, under a slot
// name of the game's own. The save-slot menu writes regenerated and copied save objects, and the
// join capture writes the live one to a zcoop_ scratch slot; neither is a moment the host's world
// on disk changed.
void OnWorldSaveWritten(void* saveObject, const wchar_t* slotName) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    if (std::wcsncmp(slotName, L"zcoop_", 6) == 0) return;
    if (void* live = ue_wrap::inventory::ResolveSaveSlot(); saveObject != live) {
        // The save-slot menu writes save objects of its own; a mismatch that is NOT that would be a
        // stale gamemode behind the resolver, and a cut skipped for it must be visible.
        UE_LOGI("player_inventory: a world save of %p to '%ls' is not the hosted world's save object "
                "(%p) -- no profiles cut", saveObject, slotName, live);
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
            "disk with it in %lld us", slotName, n, static_cast<long long>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count()));
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    ue_wrap::save_to_slot_hook::SetWritten(&OnWorldSaveWritten);
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
    if (++s_ticks < 600) return;  // ~5s settle (world up + saveSlot resolvable)
    coop::player_profile::Profile mine;
    if (!ue_wrap::inventory::ReadAll(mine.items)) return;  // world not up yet -> retry next tick
    s_done = true;
    mine.hasVitals = ue_wrap::vitals::ReadSnapshot(mine.vitals);
    mine.pose = g_standingPose;
    const auto& inv = mine.items;
    UE_LOGI("inventory[selftest]: read local saveSlot -- inventory=%zu equipment=%zu hold=%zu",
            inv.inventory.size(), inv.equipment.size(), inv.hold.size());
    {   // DIAG: name what the LOCAL player actually has post-spawn (find flashlight/glasses/compass).
        // Explicit, because the implicit iterator copy this replaced both warned (C4244) and
        // silently mangled every non-ASCII character into whatever its low byte happened to be.
        // These are engine class and key leaves, ASCII in practice -- so a surprising name must be
        // VISIBLE in the log as '?' rather than quietly become a different name.
        auto narrow = [](const std::wstring& w) {
            std::string s;
            s.reserve(w.size());
            for (const wchar_t c : w) s.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
            return s;
        };
        for (size_t i = 0; i < inv.inventory.size(); ++i)
            UE_LOGI("  selftest inv[%zu]: className='%s' key='%s'", i,
                    narrow(inv.inventory[i].className).c_str(), narrow(inv.inventory[i].key).c_str());
        for (size_t i = 0; i < inv.equipment.size(); ++i)
            UE_LOGI("  selftest eq[%zu]: propName='%s' className='%s'", i,
                    narrow(inv.equipment[i].propName).c_str(), narrow(inv.equipment[i].data.className).c_str());
        for (size_t i = 0; i < inv.hold.size(); ++i)
            UE_LOGI("  selftest hold[%zu]: propName='%s' className='%s'", i,
                    narrow(inv.hold[i].propName).c_str(), narrow(inv.hold[i].data.className).c_str());
    }
    const std::vector<uint8_t> blob1 = coop::inventory_wire::Serialize(mine);
    coop::player_profile::Profile back;
    const bool de = coop::inventory_wire::Deserialize(blob1, back);
    const auto& inv2 = back.items;
    std::vector<uint8_t> blob2;
    if (de) blob2 = coop::inventory_wire::Serialize(back);
    const bool roundtrip = de && blob1 == blob2;
    UE_LOGI("inventory[selftest]: serialize=%zu bytes, deserialize=%d, ROUND-TRIP %s "
            "(inv2: inventory=%zu equip=%zu hold=%zu)",
            blob1.size(), de ? 1 : 0, roundtrip ? "OK" : "MISMATCH",
            inv2.inventory.size(), inv2.equipment.size(), inv2.hold.size());

    // A dev probe (ini starterkit_test=1): equip the three starters through the game's own
    // add-equipment path and re-read, confirming the canonical equip path adds them. One-shot,
    // riding the selftest.
    if (::coop::config::ResolveFlag(::coop::config_registry::rows::starterkit_test)) {
        for (const wchar_t* c : {L"prop_equipment_flashlight_C", L"prop_equipment_glasses_C",
                                 L"prop_equipment_compass_C"})
            ue_wrap::begin_equipment::GiveFromClass(c);
        ue_wrap::inventory::PlayerInventory after;
        if (ue_wrap::inventory::ReadAll(after))
            UE_LOGI("starterkit[probe]: after AddEquipment x3 -- inventory=%zu equip=%zu hold=%zu "
                    "(was %zu/%zu/%zu)", after.inventory.size(), after.equipment.size(),
                    after.hold.size(), inv.inventory.size(), inv.equipment.size(), inv.hold.size());
    }
}

void OnReliable(const coop::net::BlobChunkPayload& p, uint8_t senderPeerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;

    // The client direction: the host pushed this client its per-player apply blob.
    if (s->role() == coop::net::Role::Client) {
        if (senderPeerSlot != 0) return;  // only the host pushes the apply blob
        std::vector<uint8_t> blob;
        if (!g_clientAssembler.OnChunk(p, senderPeerSlot, blob)) return;  // not complete yet
        coop::player_profile::Profile inv;
        if (!coop::inventory_wire::Deserialize(blob, inv)) {
            UE_LOGW("player_inventory[client]: host apply blob (%zu bytes) failed to deserialize "
                    "-- ignoring (will keep waiting / fall back)", blob.size());
            return;
        }
        g_pendingApply = std::move(inv);
        g_hasPendingApply.store(true, std::memory_order_release);
        UE_LOGI("player_inventory[client]: received per-player apply blob from host (%zu bytes, "
                "inventory=%zu equip=%zu hold=%zu)", blob.size(),
                g_pendingApply.items.inventory.size(), g_pendingApply.items.equipment.size(),
                g_pendingApply.items.hold.size());
        return;
    }

    // The host direction: a client slot streamed its live inventory to persist.
    if (s->role() != coop::net::Role::Host) return;
    if (senderPeerSlot < 1 || senderPeerSlot >= coop::net::kMaxPeers) return;  // a CLIENT slot
    std::vector<uint8_t> blob;
    if (!g_assembler.OnChunk(p, senderPeerSlot, blob)) return;  // not complete yet
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
}

// The single-player starter items: the game's begin-equipment logic gives exactly these three
// at New Game, and the host save retains them across play.
int StarterIndex(const std::wstring& className) {
    if (className == L"prop_equipment_flashlight_C") return 0;
    if (className == L"prop_equipment_glasses_C")    return 1;
    if (className == L"prop_equipment_compass_C")    return 2;
    return -1;
}

// Build the first-join starter kit (flashlight, glasses, compass) by reading the host's own
// live inventory and keeping only those three, one per class: valid, complete save records
// with no hardcoded struct layout and none of the host's other items. The worn-equipment
// array shape is preserved (non-starter and duplicate slots cleared) so the kit keeps the
// New Game layout. False if no starter item is present (the host dropped them, or the save
// slot is unresolvable), and the caller sends empty. Game thread. Every copied record gets a
// fresh key: the game's key index holds one object per key (lib::assignKey overwrites), so a copy
// under its source's key aliases the host's item the moment either one is dropped into the world.
bool BuildFirstJoinStarterKit(ue_wrap::inventory::PlayerInventory& out) {
    ue_wrap::inventory::PlayerInventory host;
    if (!ue_wrap::inventory::ReadAll(host)) return false;
    bool got[3] = {false, false, false};
    for (const auto& r : host.inventory) {
        const int k = StarterIndex(r.className);
        if (k >= 0 && !got[k]) {
            got[k] = true;
            out.inventory.push_back(r);
            out.inventory.back().key = coop::prop_synth_key::RandomKeyString();
        }
    }
    auto rekey = [](ue_wrap::inventory::EquipRecord& e) {
        e.propKey = coop::prop_synth_key::RandomKeyString();
        e.data.key = e.propKey;
    };
    out.equipment = host.equipment;  // keep the worn-slot array shape
    for (auto& e : out.equipment) {
        const int k = StarterIndex(e.data.className);
        if (k < 0 || got[k]) e = ue_wrap::inventory::EquipRecord{};  // clear non-starter / dup slot
        else { got[k] = true; rekey(e); }
    }
    out.hold = host.hold;
    for (auto& e : out.hold) {
        const int k = StarterIndex(e.data.className);
        if (k < 0 || got[k]) e = ue_wrap::inventory::EquipRecord{};
        else { got[k] = true; rekey(e); }
    }
    return got[0] || got[1] || got[2];
}

// A stored profile from before the lane moved onto the carried store. Its equipment and hold are
// the player's own; its inventory third is the projection, so it goes, and the vitals it never
// held start from the game's defaults. What stays is re-keyed: those files' starter items were
// copied under the HOST's keys. False if the blob does not parse.
bool LiftProjectionProfile(std::vector<uint8_t>& blob) {
    coop::player_profile::Profile p;
    uint8_t ver = 0;
    if (!coop::inventory_wire::Deserialize(blob, p, &ver)) return false;
    if (ver == coop::inventory_wire::kVersion) return true;
    auto& inv = p.items;
    inv.inventory.clear();
    p.hasVitals = ue_wrap::vitals::ReadDefaults(p.vitals);  // those files held none, and no pose
    for (auto* arr : {&inv.equipment, &inv.hold})
        for (auto& e : *arr)
            if (!e.data.className.empty()) {
                e.propKey = coop::prop_synth_key::RandomKeyString();
                e.data.key = e.propKey;
            }
    blob = coop::inventory_wire::Serialize(p);
    return true;
}

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
    // one cannot parse) is a RETURNING player: they get the kit for this session, and nothing they
    // stream back may be written over what is on disk.
    const bool usable = found == PS::Found::Yes && LiftProjectionProfile(blob);
    if (!usable && found != PS::Found::Absent) PS::Quarantine(guid);
    if (!usable) {
        // First join (nothing held and no stored file), or the quarantined case above: seed the starter kit so the
        // player is not dropped into the host's world empty-handed (a loaded coop save runs no
        // begin-equipment, so they would be). Read off the host's own live inventory, filtered to
        // the three starter classes; if the kit is absent, degrade to empty, never to the host's
        // other items or another player's. The vitals are the game's own defaults, never the
        // host's numbers, and there is no pose: a first join appears at the start point.
        coop::player_profile::Profile first;
        const bool kit = BuildFirstJoinStarterKit(first.items);
        if (!kit) first.items = {};
        first.hasVitals = ue_wrap::vitals::ReadDefaults(first.vitals);  // else the joiner reads its own
        blob = coop::inventory_wire::Serialize(first);
        UE_LOGI("player_inventory: slot %d guid=%s -- first join, %s (inventory=%zu equip-slots=%zu)",
                peerSlot, guid.c_str(), kit ? "seeding the starter kit" : "starter kit unavailable, EMPTY",
                first.items.inventory.size(), first.items.equipment.size());
    }
    if (coop::blob_chunks::SendBlobToSlot(s, peerSlot, coop::net::ReliableKind::PlayerInventoryBlob,
                                          ++g_hostSendSeq[peerSlot], blob)) {
        UE_LOGI("player_inventory: sent slot %d guid=%s its per-player inventory (%zu-byte blob)",
                peerSlot, guid.c_str(), blob.size());
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
        g_applySentToSlot[peerSlot] = false;  // a rejoin re-pushes the apply blob
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
}

}  // namespace coop::player_inventory_sync
