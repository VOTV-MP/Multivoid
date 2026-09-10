// coop/interactables/atv_sync.cpp -- see coop/interactables/atv_sync.h. The ATV (AATV_C) rig sync.
// The mirror simulates: a peer that does not author an ATV runs the rig natively, physics and tick
// on, and is corrected toward the authority through velocity, never frozen or teleported. AATV_C is
// a five-body constraint rig whose visible output is suspension travel, and SetActorLocation moves
// the root only, so a kinematic apply dragged four constrained bodies behind one teleported root.
// The one thing a mirror may not do is author collision damage (the hit guard). MTA's shape (a hard
// velocity write per packet, a warp past a speed-scaled threshold, an elected syncer for an
// unoccupied vehicle that sends only on change) with one divergence: correction through velocity,
// since their vehicle is one rigid body. Two predicates: IsPoseAuthor (I drive or carry it) and
// OwnsTick (that, or I am the host and nobody authors it); the seat (occupantSlot, which the
// mount deny reads) and the author (authorSlot, who streams) stay separate, since a grabbing
// peer must not deny the seat. Keyed by the save key. The seat is self-elected, and two peers
// mounting inside one round-trip both elect themselves: the lower slot wins in OnReliable, a
// tie-break, not authority.

#include "coop/interactables/atv_sync.h"
#include "coop/interactables/atv_corrector.h"
#include "coop/dev/atv_eject_drill.h"
#include "coop/interactables/atv_condition_sync.h"
#include "coop/interactables/atv_hit_guard.h"
#include "coop/interactables/atv_sync_internal.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/player/players_registry.h"   // Registry::Local / LocalPeerId / kMaxPeers
#include "coop/player/roster_ledger.h"      // SubscribeSlotReplaced: a departed author must not hold an ATV

#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"          // ReadMainPlayerGrabState (grabber authority) + Get/SetActorRootPhysicsVelocity
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"     // the generation-stamped index
#include "coop/element/object_scan_hub.h"      // the shared sliced scan pass
#include "ue_wrap/core/types.h"           // FVector, FRotator, NormalizeAxis

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::atv_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace A = ue_wrap::atv;
using ue_wrap::FVector;
using ue_wrap::FRotator;
using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

constexpr uint64_t kDriveSendMs = 50;   // about 20 Hz while a peer authors it
constexpr uint64_t kIdleSendMs  = 200;  // the idle syncer's ceiling, 5 Hz, and only when it changed
// Never nothing: the change gate asks whether the sender's copy moved and is blind to the
// receiver, whose mirror can roll while the host's copy is parked, and a dropped connect snapshot
// would otherwise be permanent for an idle ATV. The gate lowers the rate; this floor keeps it
// non-zero.
constexpr uint64_t kIdleKeepaliveMs = 2000;

// The idle syncer's change gate (MTA's WriteVehicleInformation in our units): a parked ATV sends
// nothing.
constexpr float kIdleMovedCm   = 1.0f;
constexpr float kIdleTurnedDeg = 0.5f;
constexpr float kIdleMovingCmS = 1.0f;


// Per-ATV state; a mirror is a simulating body whose velocity is biased at packet arrival, with
// no interpolation state. Game thread only.

std::atomic<coop::net::Session*> g_session{nullptr};

// g_atvs is game-thread only: Install, Tick, OnReliable, the connect snapshot and OnDisconnect run
// serially inside the pump. Tick mutates entry fields only; inserts and erases happen in the hub
// pass (scan_hub::Tick runs before this Tick) and in OnReliable (the drain, before the ticks).
std::unordered_map<std::wstring, AtvEntry> g_atvs;
size_t   g_lastLogCount = SIZE_MAX;
uint64_t g_lastLogHash  = 0;
bool     g_installed    = false;  // latches the one-time index; Install is the per-tick ensure path

// Runtime-spawned ATV identity. Nothing sells an ATV, but list_props row 'atv' spawns an ATV_C
// from the spawn menu; such an ATV has no save twin on the other peers and its own save key is
// minted randomly per peer, so the host gives it a synthetic wire key ("coopatv#N") and announces
// it, and clients fresh-spawn a native AATV_C under that key. A save-placed ATV (a deterministic
// key both peers loaded) keeps its real key.
std::unordered_set<std::wstring>        g_savePlacedKeys;    // host: real keys seen before any client connected, so save-placed
std::unordered_set<void*>               g_savePlacedActors;  // host: the actors present before any client, so a save ATV that mints its key late is not misread as a runtime spawn
std::unordered_map<void*, std::wstring> g_synthForActor;     // actor -> synthetic wire key (host runtime-spawned + client mirror)
uint32_t                                g_synthCounter = 0;  // host: the monotonic synthetic-key id

const wchar_t* const kSynthPrefix = L"coopatv#";  // distinguishes synth keys from real ATV keys ("atv"/base64)

bool IsSynthKey(const std::wstring& k) {
    return k.compare(0, 8, kSynthPrefix) == 0;  // "coopatv#" is 8 chars
}

// A WireClassName from a wide class name (ASCII), truncated at 63.
void FillWireClassName(coop::net::WireClassName& out, const std::wstring& name) {
    out.len = 0;
    for (size_t i = 0; i < name.size() && i < sizeof(out.data); ++i)
        out.data[out.len++] = static_cast<char>(name[i]);
}
std::wstring WireClassNameToString(const coop::net::WireClassName& in) {
    std::wstring s;
    const uint8_t n = in.len <= sizeof(in.data) ? in.len : static_cast<uint8_t>(sizeof(in.data));
    s.reserve(n);
    for (uint8_t i = 0; i < n; ++i) s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(in.data[i])));
    return s;
}

// Host: announces a runtime ATV so clients fresh-spawn a native mirror; slot < 0 broadcasts (a
// new ATV mid-session), slot >= 0 seeds one joiner. Reads the class and the current pose.
void SendAtvSpawn(const std::wstring& synthKey, void* actor, int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !actor) return;
    void* cls = R::ClassOf(actor);
    if (!cls) return;
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(actor, loc, rot)) return;
    coop::net::AtvSpawnPayload p{};
    WireKeyFromString(synthKey, p.synthKey);
    FillWireClassName(p.className, R::ToString(R::NameOf(cls)));
    p.x = loc.X; p.y = loc.Y; p.z = loc.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    if (slot < 0) s->SendReliable(coop::net::ReliableKind::AtvSpawn, &p, sizeof(p));
    else          s->SendReliableToSlot(slot, coop::net::ReliableKind::AtvSpawn, &p, sizeof(p));
}

void SendAtvDestroy(const std::wstring& synthKey) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    coop::net::AtvDestroyPayload p{};
    WireKeyFromString(synthKey, p.synthKey);
    s->SendReliable(coop::net::ReliableKind::AtvDestroy, &p, sizeof(p));
}


// True if this peer's player is seated in `actor`, per the local engine.
bool IsLocalOccupant(void* actor, void* localPlayer) {
    return localPlayer && A::IsDriven(actor) && A::GetOccupantPlayer(actor) == localPlayer;
}

// True if this peer can claim or holds driver authority; a seat another peer holds cannot be
// claimed (the double-mount race).
bool CanClaimOrIsDriver(void* actor, void* localPlayer, uint8_t occupantSlot, uint8_t localSlot) {
    if (!IsLocalOccupant(actor, localPlayer)) return false;
    return occupantSlot == 0xFF || occupantSlot == localSlot;
}

// True if this peer's player is grabbing `actor` with the grav hand (carried, not seated). The
// ATV has no grabbed flag of its own (isDriven and Player are written only on the seating path),
// so the grabber identity is the player's grabbing_actor or holding_actor, both accepted.
bool IsLocalGrabber(void* actor, void* localPlayer) {
    if (!localPlayer || !actor) return false;
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(localPlayer, gs)) return false;
    return gs.grabbingActor == actor || gs.holdingActor == actor;
}

// Pose authority: this peer streams `actor`, driving or carrying it. Not tick ownership.
bool IsPoseAuthor(void* actor, void* localPlayer, uint8_t occupantSlot, uint8_t localSlot) {
    return CanClaimOrIsDriver(actor, localPlayer, occupantSlot, localSlot) ||
           IsLocalGrabber(actor, localPlayer);
}

// Tick ownership: the peer that authors the ATV, else the host (MTA's unoccupied-vehicle
// election). A different set from pose authority: on the host with nobody driving, IsPoseAuthor
// is false and this is true. Its two jobs are electing the idle syncer and feeding the collision
// guard's owned set; there is no brain-off (parking the tick was measured useless, since
// SetCenterOfMass runs unconditionally per tick, and the wheel torque and battery terms are
// already single-peer by the game's own isDriven gates).
bool OwnsTickFor(bool isPoseAuthor, bool isHost, uint8_t authorSlot) {
    return isPoseAuthor || (isHost && authorSlot == 0xFF);
}

// An AtvStatePayload from a live read; false if the transform read fails. `grabbed` marks the
// authority as the grav-hand grabber (bit 2); `authorSlot` names who holds it (0xFF = nobody,
// which elects the host as its idle syncer).
bool ReadPayload(void* actor, const std::wstring& key, uint8_t occupantSlot, uint8_t authorSlot,
                 bool adopt, coop::net::AtvStatePayload& p, bool grabbed = false) {
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(actor, loc, rot)) return false;
    FVector lin{}, ang{};
    // Best effort: a failed read leaves zeros, the honest value and also a body at rest.
    ue_wrap::engine::GetActorRootPhysicsVelocity(actor, lin, ang);
    std::memset(&p, 0, sizeof(p));
    WireKeyFromString(key, p.key);
    p.x = loc.X; p.y = loc.Y; p.z = loc.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    p.linVelX = lin.X; p.linVelY = lin.Y; p.linVelZ = lin.Z;
    p.angVelX = ang.X; p.angVelY = ang.Y; p.angVelZ = ang.Z;
    p.occupantSlot = occupantSlot;
    p.authorSlot   = authorSlot;
    uint8_t sb = 0;
    if (A::IsDriven(actor)) sb |= 0x1;
    if (A::GetBrake(actor)) sb |= 0x2;
    if (grabbed)            sb |= 0x4;
    p.stateBits = sb;
    p.adopt = adopt ? 1 : 0;
    // The condition block (tires, spare, dirt, fuel, health), filled at this one site so the
    // authority, the idle syncer and the adopt seed cannot fork; a failed read leaves it zeroed
    // with tiresValid 0, and receivers touch nothing.
    coop::atv_condition_sync::FillPayload(actor, p);
    return true;
}




// The idle syncer's change gate: true if the ATV moved, turned or is moving; the baseline updates
// on a yes, so a slow drift accumulates into a send instead of being rounded away.
bool IdleWorthSending(AtvEntry& e) {
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(e.actor, loc, rot)) return false;
    FVector lin{}, ang{};
    ue_wrap::engine::GetActorRootPhysicsVelocity(e.actor, lin, ang);
    bool send = !e.haveLastSync || Len(lin) > kIdleMovingCmS;
    if (!send) {
        const FVector d{ loc.X - e.lastSyncPos.X, loc.Y - e.lastSyncPos.Y, loc.Z - e.lastSyncPos.Z };
        send = Len(d) > kIdleMovedCm ||
               std::fabs(ue_wrap::NormalizeAxis(rot.Pitch - e.lastSyncRot.Pitch)) > kIdleTurnedDeg ||
               std::fabs(ue_wrap::NormalizeAxis(rot.Yaw   - e.lastSyncRot.Yaw))   > kIdleTurnedDeg ||
               std::fabs(ue_wrap::NormalizeAxis(rot.Roll  - e.lastSyncRot.Roll))  > kIdleTurnedDeg;
    }
    if (send) { e.lastSyncPos = loc; e.lastSyncRot = rot; e.haveLastSync = true; }
    return send;
}


// The scan-hub consumer: the hub's shared sliced pass drives these callbacks, preserving the
// sender state for keys that persist (only the actor and index update) and classifying identity
// as above (a save-placed ATV keeps its real key; a host-side runtime ATV gets a synthetic key
// and an announce). A spawn landing inside the pass window at a join is announced on the next
// pass, when the joiner is connected.
uint32_t g_indexGen = 0;  // world gen of the last completed pass (stale-gen index = EMPTY)
bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }
struct ScanFound { std::wstring wireKey; void* obj; int32_t idx; std::wstring realKey; };
std::vector<ScanFound> g_scanFound;   // pass scratch (GT-only)
bool g_scanIsHost = false;            // pass context, captured at pass begin
bool g_scanCapturing = false;

void HubPassBegin(void*, bool) {
    g_scanFound.clear();
    auto* s = g_session.load(std::memory_order_acquire);
    g_scanIsHost = s && s->role() == coop::net::Role::Host;
    const bool isHost = g_scanIsHost;
    // The baseline window: before any client is connected, every keyed ATV the host has is
    // save-placed; after that a newly appearing key is a runtime spawn. Accumulated, not a
    // single-frame latch, so a save ATV slow to mint its key still lands in the set.
    g_scanCapturing = isHost && (!s || !s->connected());
}

void HubMatch(void*, void* obj) {
    const bool isHost = g_scanIsHost;
    const bool capturing = g_scanCapturing;
    auto& found = g_scanFound;
    {
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) return;  // skip CDO
        if (!R::IsLive(obj)) return;
        if (capturing) g_savePlacedActors.insert(obj);  // capture the ACTOR (even before it mints its key)
        std::wstring realKey = A::GetKeyString(obj);
        if (realKey.empty() || realKey == L"None") return;  // not yet keyed -- the next pass picks it up
        std::wstring wireKey;
        auto sf = g_synthForActor.find(obj);
        if (sf != g_synthForActor.end()) {
            wireKey = sf->second;                              // already synth (host runtime-spawn / client mirror)
        } else if (capturing) {
            g_savePlacedKeys.insert(realKey);                 // baseline: a save-placed ATV
            wireKey = realKey;
        } else if (isHost && g_savePlacedKeys.find(realKey) == g_savePlacedKeys.end() &&
                   g_savePlacedActors.find(obj) == g_savePlacedActors.end()) {
            // Host: a mid-session ATV not in the save set is a runtime spawn: a synthetic wire key
            // and an announce, so the clients fresh-spawn a mirror.
            wireKey = std::wstring(kSynthPrefix) + std::to_wstring(++g_synthCounter);
            g_synthForActor[obj] = wireKey;
            SendAtvSpawn(wireKey, obj, /*slot*/ -1);          // broadcast to all connected clients
            UE_LOGI("atv: runtime-spawned ATV detected -- synthKey='%ls' class='%ls' (host-announced AtvSpawn)",
                    wireKey.c_str(), R::ToString(R::NameOf(R::ClassOf(obj))).c_str());
        } else {
            wireKey = realKey;                                 // save-placed default ATV (both peers have it)
        }
        found.push_back({ std::move(wireKey), obj, R::InternalIndexOf(obj), std::move(realKey) });
    }
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    const bool isHost = g_scanIsHost;
    auto& found = g_scanFound;
    // Entries whose ATV vanished are dropped; a gone host-synthetic ATV sends AtvDestroy so the
    // clients tear down their mirror. On a full pass `found` is authoritative; on a tail pass it is
    // only the new tail, so liveness by index prunes instead.
    for (auto it = g_atvs.begin(); it != g_atvs.end();) {
        bool keep;
        if (isFull) {
            keep = false;
            for (auto& f : found) if (f.wireKey == it->first) { keep = true; break; }
        } else {
            keep = R::IsLiveByIndex(it->second.actor, it->second.idx);
        }
        if (keep) { ++it; continue; }
        if (IsSynthKey(it->first)) {
            if (isHost) SendAtvDestroy(it->first);
            if (it->second.actor) g_synthForActor.erase(it->second.actor);
        }
        it = g_atvs.erase(it);
    }
    // Update or add. A wire key can outlive its actor (a save-placed key is deterministic and a
    // client join runs two level loads), and the successor must not inherit the dead actor's
    // authority, seat, correction clock and change-gate baseline: the entry stays, everything that
    // described the actor resets.
    for (auto& f : found) {
        AtvEntry& e = g_atvs[f.wireKey];
        if (e.actor && e.actor != f.obj) {
            e.occupantSlot = 0xFF; e.authorSlot = 0xFF;
            e.wasPoseAuthor = false;
            e.haveLastSync = false; e.lastIdleSendMs = 0; e.lastPktMs = 0;
            e.lastErrCm = -1.f; e.stallPackets = 0; e.restReplaces = 0; e.lastRestPlaceMs = 0;
        }
        e.actor = f.obj;
        e.idx   = f.idx;
    }
    g_indexGen = worldGen;
    // The keys hash over the whole index, since on a tail pass `found` is only the new arrivals.
    uint64_t keysHash = 0;
    for (auto& kv : g_atvs) keysHash ^= FnvKey(kv.first);
    if (g_atvs.size() != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = g_atvs.size();
        g_lastLogHash  = keysHash;
        UE_LOGI("atv: index rebuilt -- %zu live ATV(s), keysHash=0x%016llX (%s pass, +%zu new) "
                "(compare host vs client for cross-peer Key stability)",
                g_atvs.size(), static_cast<unsigned long long>(keysHash),
                isFull ? "full" : "tail", found.size());
    }
    g_scanFound.clear();
    return g_atvs.size();
}

// A peer whose seat or authorship we hold has left or been replaced (slots recycle lowest-free,
// so a slot can change occupant with no absence between, which is why this hangs off the
// ledger's row transition). Every ATV it held is released: an authorSlot stuck on a departed peer
// means nobody ever syncs that ATV again.
void OnSlotReplaced(int slot, const coop::roster_ledger::Row& /*outgoing*/,
                    const coop::roster_ledger::Row& /*incoming*/) {
    if (slot < 0 || slot > 0xFE) return;
    const uint8_t s8 = static_cast<uint8_t>(slot);
    int freed = 0;
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (e.occupantSlot == s8) { e.occupantSlot = 0xFF; ++freed; }
        if (e.authorSlot   == s8) { e.authorSlot   = 0xFF; ++freed; }
        // The change gate re-arms: the other peers still hold the departed claim, and healing them
        // depends on the host sending something.
        e.haveLastSync = false;
    }
    if (freed > 0)
        UE_LOGI("atv: slot %d departed -- freed %d ATV seat/author reservation(s)", slot, freed);
}

void SubscribeDepartures() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::roster_ledger::SubscribeSlotReplaced(&OnSlotReplaced);
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "atv", nullptr, &A::EnsureResolved, &A::IsAtv,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 15});
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Install is the per-tick ensure path (net_pump re-calls it until the class loads), latched so
    // the one-time registration happens once.
    if (!g_installed && A::EnsureResolved()) {
        RegisterWithScanHub();  // the hub builds the index on its own cadence
        // The condition layout is warmed here, with ATV_C resident, so its one-shot resolve (four
        // uncached function walks) lands at session setup rather than in the first apply frame.
        ue_wrap::atv_condition::Resolve();
        coop::atv_hit_guard::InstallHitGuard();      // the seven ComponentHit interceptors -- Tick refuses to run without them
        SubscribeDepartures();  // a departed author must not hold an ATV hostage
        g_installed = true;
    }
}

// A peer may name itself as an ATV's holder, never anyone else: authorSlot elects the idle syncer
// and every other peer's mount deny reads it, so an unattributed one is an authority assertion,
// and one packet a second from any client would pin an empty ATV as occupied for everyone. Client
// scoped: the host legitimately speaks for other peers in its snapshot and its relay, and the
// relay preserves the origin slot.
bool SenderMaySpeakFor(uint8_t senderSlot, uint8_t claimedSlot) {
    if (senderSlot == 0 || senderSlot == 0xFF) return true;  // host, or unattributed local path
    return claimedSlot == 0xFF || claimedSlot == senderSlot;
}

void OnReliable(const coop::net::AtvStatePayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("atv: OnReliable empty key -- dropping"); return; }
    if (!A::EnsureResolved()) return;
    if (!IndexCurrent()) return;  // a stale-generation index holds another world's ATVs; the stream re-sends
    // A non-finite guard before the engine writes.
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z) ||
        !std::isfinite(payload.pitch) || !std::isfinite(payload.yaw) || !std::isfinite(payload.roll)) {
        UE_LOGW("atv: OnReliable non-finite pose -- dropping key='%ls'", key.c_str());
        return;
    }
    auto it = g_atvs.find(key);
    if (it == g_atvs.end()) return;  // not indexed yet -- the throttled rebuild will pick it up
    AtvEntry& e = it->second;
    if (!R::IsLiveByIndex(e.actor, e.idx)) return;

    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();

    // The simultaneous-mount tie-break, before the authority early-return that reads the seat: two
    // peers mounting inside one round-trip both elect themselves and each treats the other's stream
    // as an echo, a permanent double drive. The lower slot wins, a total order both hold; only a
    // genuine claim from a peer we outrank takes the seat, so an echo can never demote us.
    if (payload.occupantSlot != 0xFF && payload.occupantSlot < localSlot &&
        IsLocalOccupant(e.actor, localPlayer) &&
        (e.occupantSlot == 0xFF || e.occupantSlot == localSlot)) {
        UE_LOGI("atv: seat contention on '%ls' -- slot %u outranks local slot %u; yielding pose authority",
                key.c_str(), static_cast<unsigned>(payload.occupantSlot),
                static_cast<unsigned>(localSlot));
        e.occupantSlot = payload.occupantSlot;   // a mirror now; Tick's release edge is suppressed for a yield
    }

    // Our own authority ignores the incoming pose, so a relayed copy cannot fight the live drive.
    if (IsPoseAuthor(e.actor, localPlayer, e.occupantSlot, localSlot)) return;

    // The seat and author, if the sender may name them.
    if (!SenderMaySpeakFor(senderPeerSlot, payload.authorSlot) ||
        !SenderMaySpeakFor(senderPeerSlot, payload.occupantSlot)) {
        UE_LOGW("atv: slot %u named holder author=%u occ=%u on '%ls' -- refusing (a peer speaks "
                "only for itself)", static_cast<unsigned>(senderPeerSlot),
                static_cast<unsigned>(payload.authorSlot),
                static_cast<unsigned>(payload.occupantSlot), key.c_str());
        return;
    }
    e.occupantSlot = payload.occupantSlot;
    e.authorSlot   = payload.authorSlot;

    // A connect snapshot warps (the joiner has not seen the world); a live packet corrects. The rig
    // keeps simulating either way; the state machine between "adopt an idle ATV" and "mirror an
    // authored one" is where the release defect lived.
    coop::atv_corrector::ApplyCorrection(e, payload, /*snap*/ payload.adopt != 0);
    // The condition block applies after the pose, behind the same gates; presence consumption is
    // further gated inside on the host as sender (atv_condition_sync.h).
    coop::atv_condition_sync::ApplyPayload(e, payload, senderPeerSlot);
}

void OnAtvRelease(const coop::net::AtvReleasePayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("atv: OnAtvRelease empty key -- dropping"); return; }
    if (!A::EnsureResolved()) return;
    if (!IndexCurrent()) return;  // a stale-generation index holds another world's ATVs
    auto it = g_atvs.find(key);
    if (it == g_atvs.end()) return;  // not indexed yet -- nothing whose author we could clear
    AtvEntry& e = it->second;
    if (!R::IsLiveByIndex(e.actor, e.idx)) return;

    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();

    // Our own authority ignores a stale or echoed release.
    if (IsPoseAuthor(e.actor, localPlayer, e.occupantSlot, localSlot)) return;

    // Only the recorded author may clear the author (the host excepted), or any client could hand
    // itself the idle election or un-seat a driver.
    if (senderPeerSlot != 0 && senderPeerSlot != 0xFF &&
        e.authorSlot != 0xFF && e.authorSlot != senderPeerSlot) {
        UE_LOGW("atv: slot %u released '%ls' held by slot %u -- refusing",
                static_cast<unsigned>(senderPeerSlot), key.c_str(),
                static_cast<unsigned>(e.authorSlot));
        return;
    }

    // The whole handler: the seat and the author free. No physics write and no velocity, since
    // nothing was frozen and every AtvState carried the velocity; on the host this election makes
    // it the idle syncer on the next tick.
    e.occupantSlot = 0xFF;
    e.authorSlot   = 0xFF;
    UE_LOGI("atv: OnAtvRelease key='%ls' -- author cleared (the rig kept simulating throughout)",
            key.c_str());
}

void OnAtvSpawn(const coop::net::AtvSpawnPayload& payload, uint8_t /*senderPeerSlot*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only (the host owns the real ATV)
    if (!A::EnsureResolved()) return;
    std::wstring synthKey = StringFromWireKey(payload.synthKey);
    if (synthKey.empty()) { UE_LOGW("atv: OnAtvSpawn empty synthKey -- dropping"); return; }
    if (g_atvs.find(synthKey) != g_atvs.end()) return;  // already spawned (re-announce / connect dup) -- idempotent
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z) ||
        !std::isfinite(payload.pitch) || !std::isfinite(payload.yaw) || !std::isfinite(payload.roll)) {
        UE_LOGW("atv: OnAtvSpawn non-finite pose synthKey='%ls' -- dropping", synthKey.c_str());
        return;
    }
    std::wstring className = WireClassNameToString(payload.className);
    if (className.empty()) { UE_LOGW("atv: OnAtvSpawn empty className synthKey='%ls' -- dropping", synthKey.c_str()); return; }
    const FVector loc{ payload.x, payload.y, payload.z };
    const FRotator rot{ payload.pitch, payload.yaw, payload.roll };
    void* spawned = A::SpawnMirror(className, loc, rot);  // physics LEFT ON -- a native idle grabbable ATV
    if (!spawned) {
        UE_LOGW("atv: OnAtvSpawn SpawnMirror failed synthKey='%ls' class='%ls'", synthKey.c_str(), className.c_str());
        return;
    }
    AtvEntry e{};
    e.actor = spawned;
    e.idx = R::InternalIndexOf(spawned);
    e.isClientSpawnedMirror = true;
    g_atvs[synthKey] = std::move(e);
    g_synthForActor[spawned] = synthKey;
    UE_LOGI("atv: spawned runtime-ATV mirror synthKey='%ls' class='%ls' actor=%p loc=(%.0f, %.0f, %.0f)",
            synthKey.c_str(), className.c_str(), spawned, loc.X, loc.Y, loc.Z);
}

void OnAtvDestroy(const coop::net::AtvDestroyPayload& payload, uint8_t /*senderPeerSlot*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only
    std::wstring synthKey = StringFromWireKey(payload.synthKey);
    if (synthKey.empty()) return;
    if (!IndexCurrent()) return;  // the pass prunes a dead-world entry itself
    auto it = g_atvs.find(synthKey);
    if (it == g_atvs.end()) return;
    void* actor = it->second.actor;
    if (it->second.isClientSpawnedMirror) A::DestroyMirror(actor);  // K2_DestroyActor our fresh spawn
    if (actor) g_synthForActor.erase(actor);
    g_atvs.erase(it);
    UE_LOGI("atv: destroyed runtime-ATV mirror synthKey='%ls'", synthKey.c_str());
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // The hub keeps the index a pass fresh; a spawn inside that window is announced on the next
    // pass.
    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    int sent = 0, spawns = 0;
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (!R::IsLiveByIndex(e.actor, e.idx)) continue;
        // A synthetic-keyed ATV is announced first on the same lane, so the joiner fresh-spawns it
        // before the pose lands.
        if (IsSynthKey(kv.first)) { SendAtvSpawn(kv.first, e.actor, peerSlot); ++spawns; }
        // The change gate re-arms for the joiner: the snapshot is fire-once and a fresh joiner is
        // the peer most likely to drop it, and a satisfied gate would then send nothing until the
        // host's copy moved.
        e.haveLastSync = false;
        // The joiner gets pose and velocity and warps to it. It needs who holds the ATV: the host
        // itself if it authors, else the recorded author (or nobody, which makes the host its
        // syncer). An ATV in the air at a join arrives moving and lands.
        const bool hostAuthors = IsPoseAuthor(e.actor, localPlayer, e.occupantSlot, localSlot);
        const uint8_t authorSlot = hostAuthors ? localSlot : e.authorSlot;
        coop::net::AtvStatePayload p{};
        if (!ReadPayload(e.actor, kv.first, e.occupantSlot, authorSlot, /*adopt*/true, p)) continue;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::AtvState, &p, sizeof(p));
        ++sent;
    }
    UE_LOGI("atv: connect-snapshot -- sent %d ATV pose(s) (%d runtime-ATV announce(s)) to slot %d (of %zu indexed)",
            sent, spawns, peerSlot, g_atvs.size());
}

void Tick() {
    // Every early return means this module is not deciding who owns what, and the collision guard
    // must then be inert: it suppresses damage by default, and armed against a stale set it would
    // make the local ATV invulnerable. Disarmed first; the path that earns it re-arms at the end.
    struct DisarmUnlessArmed {
        bool armed = false;
        ~DisarmUnlessArmed() {
            if (armed) return;
            coop::atv_hit_guard::SetActive(false);
            // The set clears too: OwnsTick is a public read, and a stale set would label a mirror
            // sample as an owner sample.
            coop::atv_hit_guard::PublishOwned(nullptr, 0);
        }
    } scope;

    if (!A::EnsureResolved()) return;
    RegisterWithScanHub();  // safety net for any order where Tick precedes Install
    if (!IndexCurrent()) return;  // index belongs to a dead world -- wait for the hub's next pass
    auto* s = g_session.load(std::memory_order_acquire);

    if (!s || !s->connected()) return;
    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    const uint64_t nowMs = NowMs();

    // Fail closed: without the hit interceptors a non-owner would author damage on a simulating
    // rig, so nothing is mirrored and the ERROR at Install says why.
    if (!coop::atv_hit_guard::Armed()) return;

    const bool isHost = s->role() == coop::net::Role::Host;
    void* owned[coop::atv_hit_guard::kMaxOwned];
    int   ownedN = 0;

    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (!R::IsLiveByIndex(e.actor, e.idx)) continue;

        const bool isDriver  = CanClaimOrIsDriver(e.actor, localPlayer, e.occupantSlot, localSlot);
        const bool isGrabber = !isDriver && IsLocalGrabber(e.actor, localPlayer);  // mutually exclusive
        const bool authority = isDriver || isGrabber;

        // The authority-lost edge, with two causes handled oppositely: a dismount or ungrab frees
        // the ATV (the seat and author clear, and a release goes out); a yield (still seated, but
        // outranked by a lower slot) means someone else holds it, and clearing would erase the
        // winner's claim, re-claim next tick and flap.
        const bool yielded = IsLocalOccupant(e.actor, localPlayer) &&
                             e.occupantSlot != 0xFF && e.occupantSlot != localSlot;
        if (e.wasPoseAuthor && !authority && !yielded) {
            e.occupantSlot = 0xFF;
            e.authorSlot   = 0xFF;
            coop::net::AtvReleasePayload rp{};
            WireKeyFromString(kv.first, rp.key);
            s->SendReliable(coop::net::ReliableKind::AtvRelease, &rp, sizeof(rp));
            UE_LOGI("atv: authority released key='%ls' -- author cleared; the host now syncs it idle",
                    kv.first.c_str());
        }
        e.wasPoseAuthor = authority;

        if (authority) {
            if (isDriver) e.occupantSlot = localSlot;   // claim the seat locally
            e.authorSlot = localSlot;
        }

        // Tick ownership, a different question from pose authority.
        const bool ownsTick = OwnsTickFor(authority, isHost, e.authorSlot);
        // The eject drill, env-gated, once per process.
        coop::atv_eject_drill::MaybeFire(e.actor, kv.first.c_str(), nowMs, isHost, authority, ownsTick);
        if (ownsTick) {
            if (ownedN < coop::atv_hit_guard::kMaxOwned) {
                owned[ownedN++] = e.actor;
            } else {
                static bool sSaturated = false;
                if (!sSaturated) { sSaturated = true;
                    UE_LOGE("atv: owned-set saturated at %d -- ATV '%ls' and any beyond it will "
                            "have their OWN collisions suppressed on the peer that owns them",
                            coop::atv_hit_guard::kMaxOwned, kv.first.c_str()); }
            }
        }

        if (authority) {
            if (nowMs - e.lastSentMs >= kDriveSendMs) {
                e.lastSentMs = nowMs;
                coop::net::AtvStatePayload p{};
                const uint8_t occSlot = isDriver ? localSlot : uint8_t{0xFF};  // grabber: no seated driver
                if (ReadPayload(e.actor, kv.first, occSlot, localSlot, /*adopt*/false, p, /*grabbed*/isGrabber)) {
                    s->SendReliable(coop::net::ReliableKind::AtvState, &p, sizeof(p));
                    coop::atv_condition_sync::NoteSent(e, p);  // keep the idle gate's baseline fresh across a drive-to-idle handoff
                }
            }
        } else if (ownsTick) {
            // The idle syncer (the host, nobody driving): a slower cadence and a change gate, so a
            // parked ATV costs nothing while one rolling down a hill still converges.
            if (nowMs - e.lastSentMs >= kIdleSendMs) {
                // The clock bumps once the window elapses, as MTA's does, not only on a send:
                // bumped only on a send it stayed stale for the parked ATV, and the gate then ran
                // its dispatches at the pump rate.
                e.lastSentMs = nowMs;
                // The payload is built before the gate so the condition block can vote: a parked
                // host eject (a mask flip, no motion) would otherwise wait out the keepalive.
                coop::net::AtvStatePayload p{};
                const bool readable  = ReadPayload(e.actor, kv.first, e.occupantSlot, /*authorSlot*/0xFF, /*adopt*/false, p);
                const bool changed   = IdleWorthSending(e);
                const bool condMoved = readable && coop::atv_condition_sync::CondChangedSinceLastSend(e, p);
                const bool keepalive = nowMs - e.lastIdleSendMs >= kIdleKeepaliveMs;
                if (readable && (changed || condMoved || keepalive)) {
                    e.lastIdleSendMs = nowMs;
                    s->SendReliable(coop::net::ReliableKind::AtvState, &p, sizeof(p));
                    coop::atv_condition_sync::NoteSent(e, p);
                }
            }
        }
        // A mirror does nothing here; it is corrected at packet arrival.
    }

    // Published before arming, so the guard never runs against an unrefreshed set.
    coop::atv_hit_guard::PublishOwned(owned, ownedN);
    coop::atv_hit_guard::SetActive(true);
    scope.armed = true;
}

void OnDisconnect() {
    // Disarmed first: nothing publishes an owned set from here, and a live hit must reach the game.
    coop::atv_hit_guard::SetActive(false);
    coop::atv_hit_guard::PublishOwned(nullptr, 0);
    for (auto& kv : g_atvs) {
        const bool live = R::IsLiveByIndex(kv.second.actor, kv.second.idx);
        if (kv.second.isClientSpawnedMirror) {
            if (live) A::DestroyMirror(kv.second.actor);   // a fresh-spawned runtime mirror is a coop artifact
        }
        // Nothing to restore: the lane disables neither tick nor physics, so a session leaves every
        // save ATV as it found it.
    }
    const size_t n = g_atvs.size();
    g_atvs.clear();
    g_synthForActor.clear();
    g_savePlacedKeys.clear();
    g_savePlacedActors.clear();
    g_synthCounter = 0;
    g_installed = false;  // a new session re-indexes via the next Install (latched again)
    const auto c  = coop::atv_corrector::ReadCounters();
    const auto hg = coop::atv_hit_guard::ReadCounters();
    if (n > 0)
        UE_LOGI("atv: OnDisconnect -- cleared %zu ATV(s) (brains restored; runtime mirrors destroyed); "
                "hit guard: %s, %llu neutered / %llu allowed / %llu UNRESOLVED; "
                "corrector: %llu nudged / %llu warped "
                "/ %llu cut-on-stall / %llu parked-replace",
                n, hg.armed ? "armed" : "NEVER ARMED",
                static_cast<unsigned long long>(hg.neutered),
                static_cast<unsigned long long>(hg.allowed),
                static_cast<unsigned long long>(hg.unresolved),
                static_cast<unsigned long long>(c.corrections),
                static_cast<unsigned long long>(c.warps),
                static_cast<unsigned long long>(c.stallWarps),
                static_cast<unsigned long long>(c.restPlaces));
}

bool OwnsTick(void* actor) {
    // The set, the latch and the answer live with the collision guard (atv_hit_guard::Owns).
    return coop::atv_hit_guard::Owns(actor);
}

// Whether a remote peer occupies this ATV; the mount deny reads it.
bool IsOccupiedByOther(void* actor, uint8_t* outOccupantSlot) {
    if (!actor) return false;
    if (!IndexCurrent()) return false;   // a stale-generation index holds another world's ATVs
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    for (const auto& kv : g_atvs) {
        if (kv.second.actor == actor) {
            const uint8_t occ = kv.second.occupantSlot;
            if (occ != 0xFF && occ != localSlot) {
                if (outOccupantSlot) *outOccupantSlot = occ;
                return true;
            }
            return false;
        }
    }
    return false;
}

}  // namespace coop::atv_sync
