// coop/world/ambient_fixture_sync.cpp -- see coop/world/ambient_fixture_sync.h.

#include "coop/world/ambient_fixture_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/ambient_births.h"
#include "coop/world/spawn_authority.h"
#include "coop/world/sus_hole_intent.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::ambient_fixture_sync {
namespace {

namespace AB = coop::ambient_births;
namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace PKI = coop::prop_key_index;
namespace R = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
using Payload = coop::net::AmbientFixtureStatePayload;
namespace fam = coop::net::ambient_fixture;

constexpr int kSendsPerTick = 64;  // a hexahive's field arrives whole in one tick

// The branch's own edges. getBee binds the nest's `touched` (BeginPlay of a spawned branch, processKeys of a loaded one),
// loadData reads the saved key, touched forgets the nest when a player takes it, ReceiveDestroyed destroys the nest it holds.
UE_FACT_FN(kRowGetBeeBeehiveBranch, "ambient_fixture_sync", L"beehiveBranch_C", L"getBee", Declared);
UE_FACT_FN(kRowLoadDataBeehiveBranch, "ambient_fixture_sync", L"beehiveBranch_C", L"loadData", Declared);
UE_FACT_FN(kRowTouchedBeehiveBranch, "ambient_fixture_sync", L"beehiveBranch_C", L"touched", Declared);
UE_FACT_FN(kRowReceiveDestroyedBeehiveBranch, "ambient_fixture_sync", L"beehiveBranch_C", L"ReceiveDestroyed", Declared);
constexpr int kTag = 0x41465853;  // 'AFXS'

std::atomic<coop::net::Session*> g_session{nullptr};
std::vector<AB::Event> g_events;  // reused by every drain
Counters g_counters;              // the drill's read

// The field drill's red arms, read once: no_bind leaves a loaded branch unbound by its key (the host's branch then comes as a
// copy beside it), no_census makes the census-end end nothing.
bool RedArm(const char* arm) {
    static const std::string s_arm = coop::config::ResolveEnum(coop::config_registry::rows::ambient_field_red);
    return s_arm == arm;
}

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// HOST: each fixture by its actor: its number and place, the key a branch's nest was born with, and what still has to go out.
struct Known {
    int32_t  index = -1;
    Payload  wire{};
    bool     placed = false;
    bool     keyKnown = false;     // a branch: wire.birthKey holds the key
    uint64_t holdUntil = 0;        // a branch whose key came from loadData waits one tick for the load's own getBee
    bool     sentAny = false;
    uint8_t  sentHasNest = 0;
    bool     pendingAll = true;    // goes to every client
    uint32_t pendingSlots = 0;     // goes to these joiners alone (bit = slot)
};
std::unordered_map<void*, Known> g_known;
uint32_t g_nextId = 1;
uint64_t g_tick = 0;
bool g_work = false;               // something in g_known needs a pass: an edge, a pending send, a census
uint32_t g_censusSlots = 0;        // the joiners whose census-end is owed (bit = slot)
std::vector<Payload> g_ends;       // ends not yet sent
bool g_keylessSaid = false;

// ---- HOST ----

Known& Ensure(void* obj, int32_t index, uint8_t family) {
    auto it = g_known.find(obj);
    if (it != g_known.end() && it->second.index != index) {
        g_known.erase(it);  // the slot was taken by another actor at this address; the old one's death is dropped by its index
        it = g_known.end();
    }
    if (it == g_known.end()) {
        it = g_known.emplace(obj, Known{}).first;
        it->second.index = index;
        it->second.wire.id = g_nextId++;
        it->second.wire.family = family;
        it->second.wire.alive = 1;
    }
    g_work = true;
    return it->second;
}

void RecordKey(Known& k, const std::wstring& key) {
    if (k.keyKnown || key.empty()) return;
    coop::net::WireKeyFromString(key, k.wire.birthKey);
    k.keyKnown = true;
}

// A branch the host's own events have not named a key for: read it where it stands (a seeded branch, a spawned one whose
// begin play has run).
void TryReadKey(void* obj, Known& k) {
    std::wstring key;
    void* hive = nullptr;
    if (AC::ReadBranch(obj, key, hive)) RecordKey(k, key);
}

void OnGetBeePost(const SG::Call& call) {
    if (!IsHost() || !call.object) return;
    std::wstring key;
    void* hive = nullptr;
    if (!AC::ReadBranch(call.object, key, hive) || !hive) return;
    if (key.empty()) {
        if (!g_keylessSaid) {
            g_keylessSaid = true;
            UE_LOGW("ambient_fixture_sync: a branch bound a nest that has no key -- it is not sent (the first this run)");
        }
        return;
    }
    const int32_t index = R::InternalIndexOf(call.object);
    if (index < 0) return;
    Known& k = Ensure(call.object, index, fam::kBranch);
    RecordKey(k, key);
    k.holdUntil = 0;  // the load's own getBee has come
}

void OnLoadDataPost(const SG::Call& call) {
    if (!IsHost() || !call.object) return;
    std::wstring key;
    void* hive = nullptr;
    if (!AC::ReadBranch(call.object, key, hive) || key.empty()) return;
    const int32_t index = R::InternalIndexOf(call.object);
    if (index < 0) return;
    Known& k = Ensure(call.object, index, fam::kBranch);
    if (!k.keyKnown) k.holdUntil = g_tick + 1;
    RecordKey(k, key);
}

// A player took the nest: the branch forgets it, and the key it was born with stays here.
void OnTouchedPost(const SG::Call& call) {
    if (!IsHost() || !call.object) return;
    const auto it = g_known.find(call.object);
    if (it != g_known.end()) g_work = true;
}

// A client's copy ends with the nest still standing: the nest is the prop lane's.
SG::Verdict OnDestroyedPre(const SG::Call& call) {
    if (coop::spawn_authority::IsActiveClientSession() && call.object) {
        std::wstring key;
        void* hive = nullptr;
        if (AC::ReadBranch(call.object, key, hive)) ++(hive ? g_counters.hiveDropped : g_counters.hiveGone);
        AC::DropBranchNest(call.object);
    }
    return SG::Verdict::Run;
}

bool ReadPlace(void* obj, Known& k) {
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(obj, at) || !E::TryGetActorRotation(obj, rot)) return false;
    k.wire.x = at.X;
    k.wire.y = at.Y;
    k.wire.z = at.Z;
    k.wire.pitch = rot.Pitch;
    k.wire.yaw = rot.Yaw;
    k.wire.roll = rot.Roll;
    k.placed = true;
    return true;
}

// A fixture ended. Never sent means no client has it, and a join sends only the living. A branch's end is the host's and
// crosses; a hole ends by each viewer's own distance and view (susDirtHole's 600 s check), so the host's end of one is its
// own and no end of a hole is sent. MTA decides a per-player element's presence on the server and removes it for that player
// alone (reference/mtasa-blue/Server/mods/deathmatch/logic/CPerPlayerEntity.cpp:186-239); here the game's own camera test
// decides, on each peer (principle 6).
void Retire(const Known& k) {
    const bool branch = k.wire.family == fam::kBranch;
    if (k.sentAny && branch) {
        Payload end = k.wire;
        end.alive = 0;
        end.hasNest = 0;
        g_ends.push_back(end);
        g_work = true;
    }
    UE_LOGI("ambient_fixture_sync: fixture %u family=%u ended%s", k.wire.id, k.wire.family,
            branch ? "" : " here alone (a hole's end is each viewer's)");
}

void DrainHost() {
    for (const AB::Event& e : g_events) {
        if (e.cls == AB::Class::Nest || e.cls == AB::Class::Tree) continue;  // the prop lane's and the creature lane's
        const uint8_t family = e.cls == AB::Class::Hole ? fam::kHole : fam::kBranch;
        if (e.kind == AB::Kind::Death) {
            if (family == fam::kHole) coop::sus_hole_intent::OnHoleEnded(e.obj, e.index);  // a client's hole stops counting for it
            const auto it = g_known.find(e.obj);
            if (it == g_known.end() || it->second.index != e.index) continue;
            Retire(it->second);
            g_known.erase(it);
            continue;
        }
        if (!R::IsLiveByIndex(e.obj, e.index)) continue;
        Known& k = Ensure(e.obj, e.index, family);
        if (family == fam::kBranch && !k.keyKnown) TryReadKey(e.obj, k);
    }
}

uint32_t ConnectedSlots(const coop::net::Session& s) {
    uint32_t mask = 0;
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot)
        if (s.IsSlotConnected(slot)) mask |= 1u << slot;
    return mask;
}

// The ends queued so far, sent; what the channel refused stays in g_ends for the next pass.
void SendEnds(coop::net::Session& s) {
    for (size_t i = 0; i < g_ends.size();) {
        if (s.SendReliable(coop::net::ReliableKind::AmbientFixtureState, &g_ends[i], sizeof(Payload))) {
            ++g_counters.sent;
            g_ends.erase(g_ends.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void HostPass(coop::net::Session& s) {
    const uint32_t connected = ConnectedSlots(s);
    g_censusSlots &= connected;
    bool again = false;
    int sends = 0;

    for (auto it = g_known.begin(); it != g_known.end();) {
        void* obj = it->first;
        Known& k = it->second;
        k.pendingSlots &= connected;
        ++it;
        if (!k.placed && !ReadPlace(obj, k)) {
            again = true;
            continue;
        }
        uint8_t hasNest = 0;
        if (k.wire.family == fam::kBranch) {
            // A keyless branch is not sent (the save would destroy it); an edge names its key when it has one.
            if (!k.keyKnown) TryReadKey(obj, k);
            if (!k.keyKnown) continue;
            if (g_tick < k.holdUntil) {
                again = true;
                continue;
            }
            std::wstring key;
            void* hive = nullptr;
            if (AC::ReadBranch(obj, key, hive) && hive) hasNest = 1;
            if (k.sentAny && k.sentHasNest != hasNest) k.pendingAll = true;
        }
        k.wire.hasNest = hasNest;
        if (k.pendingAll) {
            if (sends >= kSendsPerTick) {
                again = true;
                continue;
            }
            if (s.SendReliable(coop::net::ReliableKind::AmbientFixtureState, &k.wire, sizeof(Payload))) {
                k.pendingAll = false;
                k.pendingSlots = 0;
                k.sentAny = true;
                k.sentHasNest = hasNest;
                ++sends;
                ++g_counters.sent;
            } else {
                again = true;
            }
        } else if (k.pendingSlots) {
            for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
                if (!(k.pendingSlots & (1u << slot))) continue;
                if (s.SendReliableToSlot(slot, coop::net::ReliableKind::AmbientFixtureState, &k.wire, sizeof(Payload))) {
                    k.pendingSlots &= ~(1u << slot);
                    ++g_counters.sent;
                } else
                    again = true;
            }
        }
    }

    // After the loop: an end queued while the pass ran goes out in it, whatever raised it.
    SendEnds(s);

    // A joiner's census ends once every fixture is sent to it, so the loaded branches nothing named are the host's lack.
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        if (!(g_censusSlots & (1u << slot))) continue;
        bool owed = false;
        for (const auto& kv : g_known)
            if (kv.second.pendingSlots & (1u << slot)) owed = true;
        if (owed) {
            again = true;
            continue;
        }
        Payload end{};
        end.family = fam::kCensusEnd;
        end.alive = 1;
        if (s.SendReliableToSlot(slot, coop::net::ReliableKind::AmbientFixtureState, &end, sizeof(end))) {
            g_censusSlots &= ~(1u << slot);
            ++g_counters.censusSent;
            UE_LOGI("ambient_fixture_sync: census to slot %d complete, %zu fixtures stand", slot, g_known.size());
        } else {
            again = true;
        }
    }
    if (sends) UE_LOGI("ambient_fixture_sync: sent %d fixtures, %zu stand", sends, g_known.size());
    g_work = again || !g_ends.empty();
}

void HostTick(coop::net::Session& s) {
    ++g_tick;
    DrainHost();
    // A send reaches only a world-ready peer; a joiner still loading is answered whole by the connect replay at its edge, so
    // the pass waits, with its work kept, until one is ready.
    if (g_work && s.AnyWorldReadyPeer()) HostPass(s);
}

// ---- CLIENT ----

struct Ref {
    void*   obj = nullptr;
    int32_t index = -1;
};
bool Live(const Ref& r) { return r.obj && R::IsLiveByIndex(r.obj, r.index); }

// A copy of a host fixture, or a loaded branch bound to it, and the nest a branch still waits for.
struct Copy {
    Ref          actor;
    std::wstring nestKey;
    bool         wantNest = false;
};

std::vector<Payload> g_inbox;
std::unordered_map<uint32_t, Copy> g_copies;     // by the host's number; bound branches too
std::unordered_map<void*, int32_t> g_spawned;    // the copies this lane made, with their slots
std::vector<Ref> g_loadCandidates;               // the branches born before the load edge
std::unordered_map<std::wstring, Ref> g_loaded;  // the loaded branches by the key their save gave them, until bound
bool g_loadedRead = false;
bool g_retryNest = false;                        // a nest was born: ask for the ones still wanted
uint64_t g_keyVersionSeen = 0;
bool g_anyWant = false;

// A copy, or a loaded branch bound to one, that ended: IsLiveByIndex compares a slot's pointer and flags and no serial, so a
// slot taken again at the same address would pass for it. No reference to it outlives its end.
void ForgetEnded(void* obj, int32_t index) {
    const auto sp = g_spawned.find(obj);
    if (sp != g_spawned.end() && sp->second == index) g_spawned.erase(sp);
    for (auto it = g_copies.begin(); it != g_copies.end();) {
        if (it->second.actor.obj == obj && it->second.actor.index == index) it = g_copies.erase(it);
        else ++it;
    }
    for (auto it = g_loaded.begin(); it != g_loaded.end();) {
        if (it->second.obj == obj && it->second.index == index) it = g_loaded.erase(it);
        else ++it;
    }
}

void DrainClient() {
    for (const AB::Event& e : g_events) {
        if (e.cls == AB::Class::Nest) {
            if (e.kind == AB::Kind::Birth) g_retryNest = true;
            continue;
        }
        if (e.cls != AB::Class::Hole && e.cls != AB::Class::Branch) continue;
        if (e.kind == AB::Kind::Death) {
            ForgetEnded(e.obj, e.index);
            for (size_t i = 0; i < g_loadCandidates.size(); ++i) {
                if (g_loadCandidates[i].obj != e.obj || g_loadCandidates[i].index != e.index) continue;
                g_loadCandidates.erase(g_loadCandidates.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
            continue;
        }
        // The save's branches are met as births until the load edge reads them; a hole copy is spawned, never taken.
        if (e.cls == AB::Class::Branch && !g_loadedRead) g_loadCandidates.push_back(Ref{e.obj, e.index});
    }
}

// The save's branches come with its objects, late in the load; each holds the key its save gave it, which names the host's
// branch it is. Read once at the load edge, as the host's arrive after, onto what the load made.
void ReadLoaded() {
    g_loadedRead = true;
    for (const Ref& r : g_loadCandidates) {
        std::wstring key;
        void* hive = nullptr;
        if (Live(r) && AC::ReadBranch(r.obj, key, hive) && !key.empty()) g_loaded[key] = r;
    }
    g_counters.loaded = g_loaded.size();
    UE_LOGI("ambient_fixture_sync: the load brought %zu keyed branches", g_loaded.size());
    g_loadCandidates.clear();
}

bool BindNest(Copy& c) {
    if (c.nestKey.empty()) return false;
    void* nest = PKI::Find(c.nestKey);
    if (!nest) return false;
    c.wantNest = false;
    if (AC::BindBranchNest(c.actor.obj, nest)) ++g_counters.nestsBound;
    else UE_LOGW("ambient_fixture_sync: a branch could not bind its nest %p", nest);
    return true;
}

// A branch holds its nest as the host's does: the game's own binder half, once the nest's mirror has its key.
void ApplyNest(Copy& c, uint8_t hasNest) {
    if (!Live(c.actor)) return;
    std::wstring key;
    void* hive = nullptr;
    if (!AC::ReadBranch(c.actor.obj, key, hive)) return;
    if (!hasNest) {
        c.wantNest = false;
        if (hive) AC::ReleaseBranchNest(c.actor.obj);
        return;
    }
    if (hive) {
        c.wantNest = false;  // the load's own binder found it
        return;
    }
    c.wantNest = true;
    if (!BindNest(c)) g_anyWant = true;
}

// The nests a branch still waits for are asked again when a key is filed in the index (a nest's key registers after its
// birth) or a nest is born: a retry on a signalled change, with nothing to count.
void TryNests() {
    if (!g_anyWant) return;
    const uint64_t version = PKI::Version();
    if (version == g_keyVersionSeen && !g_retryNest) return;
    g_keyVersionSeen = version;
    g_retryNest = false;
    bool still = false;
    for (auto& kv : g_copies) {
        Copy& c = kv.second;
        if (!c.wantNest) continue;
        if (!Live(c.actor)) {
            c.wantNest = false;
            continue;
        }
        if (!BindNest(c)) still = true;
    }
    g_anyWant = still;
}

void EndBranch(const Ref& r) {
    if (Live(r)) AC::Destroy(r.obj);  // its ReceiveDestroyed row leaves the nest to the prop lane
    g_spawned.erase(r.obj);
}

void ApplyCensus() {
    ++g_counters.censusEnds;
    if (RedArm("no_census")) {
        g_loaded.clear();
        return;
    }
    size_t ended = 0;
    for (auto& kv : g_loaded) {
        if (!Live(kv.second)) continue;
        AC::Destroy(kv.second.obj);
        ++ended;
    }
    g_counters.censusEnded += ended;
    UE_LOGI("ambient_fixture_sync: census end -- %zu loaded branches the host lacks ended", ended);
    g_loaded.clear();
}

void ApplyInbox() {
    std::vector<Payload> inbox;
    inbox.swap(g_inbox);
    for (const Payload& p : inbox) {
        if (p.family == fam::kCensusEnd) {
            ApplyCensus();
            continue;
        }
        const bool branch = p.family == fam::kBranch;
        auto it = g_copies.find(p.id);
        if (!p.alive) {
            if (it == g_copies.end()) continue;
            EndBranch(it->second.actor);
            g_copies.erase(it);
            UE_LOGI("ambient_fixture_sync: fixture %u ended here", p.id);
            continue;
        }
        if (it != g_copies.end()) {
            if (branch) ApplyNest(it->second, p.hasNest);
            continue;
        }
        Copy c;
        c.nestKey = coop::net::StringFromWireKey(p.birthKey);
        if (branch && !c.nestKey.empty() && !RedArm("no_bind")) {
            const auto lt = g_loaded.find(c.nestKey);
            if (lt != g_loaded.end()) {
                if (Live(lt->second)) {
                    c.actor = lt->second;
                    ++g_counters.bound;
                }
                g_loaded.erase(lt);
            }
        }
        if (!c.actor.obj) {
            void* copy = AC::SpawnFixture(branch, {p.x, p.y, p.z}, {p.pitch, p.yaw, p.roll});
            if (!copy) {
                UE_LOGW("ambient_fixture_sync: fixture %u family=%u did not spawn", p.id, p.family);
                continue;
            }
            c.actor = Ref{copy, R::InternalIndexOf(copy)};
            g_spawned[copy] = c.actor.index;
            ++(branch ? g_counters.branchCopies : g_counters.holeCopies);
        }
        Copy& stored = g_copies.emplace(p.id, c).first->second;
        if (branch) ApplyNest(stored, p.hasNest);
    }
}

void ClientTick() {
    DrainClient();
    if (!coop::net_pump::HasAnnouncedWorldReady() || coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle) return;
    if (!g_loadedRead) ReadLoaded();
    ApplyInbox();
    TryNests();
}

void ResolveWatches() {
    static bool sTaken = false;
    static bool sSettled = false;
    if (!sTaken) {
        sTaken = SG::WatchName(kRowGetBeeBeehiveBranch, kTag, nullptr, &OnGetBeePost) &&
                 SG::WatchName(kRowLoadDataBeehiveBranch, kTag, nullptr, &OnLoadDataPost) &&
                 SG::WatchName(kRowTouchedBeehiveBranch, kTag, nullptr, &OnTouchedPost) &&
                 SG::WatchName(kRowReceiveDestroyedBeehiveBranch, kTag, &OnDestroyedPre, nullptr);
        static bool sSaid = false;
        if (!sTaken && !sSaid) UE_LOGE("ambient_fixture_sync: a watch on the beehive branch was refused -- retried each tick");
        sSaid = !sTaken;
        return;
    }
    if (sSettled) return;
    SG::ResolvePendingNames();
    sSettled = SG::NameWatchSettled(kRowGetBeeBeehiveBranch, kTag) && SG::NameWatchSettled(kRowLoadDataBeehiveBranch, kTag) &&
               SG::NameWatchSettled(kRowTouchedBeehiveBranch, kTag) &&
               SG::NameWatchSettled(kRowReceiveDestroyedBeehiveBranch, kTag);
    if (sSettled && !(SG::NameWatchLive(kRowGetBeeBeehiveBranch, kTag) && SG::NameWatchLive(kRowLoadDataBeehiveBranch, kTag) &&
                      SG::NameWatchLive(kRowTouchedBeehiveBranch, kTag) &&
                      SG::NameWatchLive(kRowReceiveDestroyedBeehiveBranch, kTag)))
        UE_LOGE("ambient_fixture_sync: a watch on the beehive branch is dead -- branches and their nests may not agree");
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session); }

void Tick() {
    coop::net::Session* s = g_session.load();
    if (!s) return;
    ResolveWatches();
    g_events.clear();
    AB::Drain(static_cast<AB::Mask>(AB::Bit(AB::Class::Hole) | AB::Bit(AB::Class::Branch) | AB::Bit(AB::Class::Nest)), g_events);
    if (s->role() == coop::net::Role::Host) HostTick(*s);
    else if (s->role() == coop::net::Role::Client) ClientTick();
}

void OnState(const coop::net::AmbientFixtureStatePayload& p) { g_inbox.push_back(p); }

Counters Read() { return g_counters; }

// A joiner is sent every fixture the host holds, to it alone, and then the census-end; a branch whose key is not known yet
// is sent to everyone when it is. MTA sends a joiner the whole element tree in one add packet
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CMapManager.cpp:195-301), but a joiner's world here is a save, not an
// empty world, so the snapshot needs an end marker that one atomic packet does not.
void QueueConnectBroadcastForSlot(int slot) {
    if (slot <= 0 || slot >= coop::players::kMaxPeers) return;
    const uint32_t bit = 1u << slot;
    for (auto& kv : g_known) {
        Known& k = kv.second;
        if (k.wire.family == fam::kBranch && !k.keyKnown) continue;
        k.pendingSlots |= bit;
        k.holdUntil = 0;
    }
    g_censusSlots |= bit;
    g_work = true;
}

void OnDisconnect() {
    g_counters = Counters{};
    if (g_session.load() && g_session.load()->role() == coop::net::Role::Client) {
        for (const auto& kv : g_spawned)
            if (R::IsLiveByIndex(kv.first, kv.second)) AC::Destroy(kv.first);
    }
    g_known.clear();
    g_nextId = 1;
    g_tick = 0;
    g_work = false;
    g_censusSlots = 0;
    g_ends.clear();
    g_keylessSaid = false;
    g_events.clear();
    g_inbox.clear();
    g_copies.clear();
    g_spawned.clear();
    g_loadCandidates.clear();
    g_loaded.clear();
    g_loadedRead = false;
    g_retryNest = false;
    g_keyVersionSeen = 0;
    g_anyWant = false;
    g_session.store(nullptr);
}

}  // namespace coop::ambient_fixture_sync
