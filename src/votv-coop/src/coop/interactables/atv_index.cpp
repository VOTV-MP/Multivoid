// coop/interactables/atv_index.cpp -- the ATV lane's index: which actor is which game Key. The hub's shared
// scan pass drives the callbacks below and fills the map atv_sync.cpp owns; the host's announce of that
// index (AtvSpawn on a key gained, AtvDestroy on a key lost) lives here with them. Declared in
// atv_index_internal.h; atv_authority.cpp keeps the seat and syncer table, atv_sync.cpp the stream and the
// connect snapshot.

#include "coop/interactables/atv_index_internal.h"
#include "coop/interactables/atv_authority.h"   // ClearTable / ResetRequests: a successor actor starts from the empty table
#include "coop/interactables/atv_sync_internal.h"

#include "atv_spawn_park.h"                    // spawn_park::kCap
#include "coop/element/object_scan_hub.h"      // the shared sliced scan pass
#include "coop/interactables/atv_sync.h"       // AnnounceableAtvKey, ForEachAnnounceableAtv
#include "coop/interactables/atv_host_sync.h"  // NoteAtvGone / NoteAtvIndexed: the client's open requests and held states
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"           // FVector, FRotator
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::atv_sync {

namespace R = ue_wrap::reflection;
namespace A = ue_wrap::atv;
namespace W = ue_wrap::world_identity;
using ue_wrap::FVector;
using ue_wrap::FRotator;
using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

namespace detail {

namespace {
// A WireClassName from a wide class name (ASCII), truncated at 63.
void FillWireClassName(coop::net::WireClassName& out, const std::wstring& name) {
    out.len = 0;
    for (size_t i = 0; i < name.size() && i < sizeof(out.data); ++i)
        out.data[out.len++] = static_cast<char>(name[i]);
}

constexpr size_t kMaxKeyChars = sizeof(coop::net::WireKey::data);  // 31: what a WireKey carries

std::unordered_set<std::wstring> g_saidKeys;      // "topic|key" pairs already said (once per key and topic)
uint32_t g_saidPastCap = 0;                       // sayings counted but not stored, once the set was full

void Refuse(const std::wstring& key, const char* why, const char* side) {
    if (FirstSaying(L"refuse|", key))
        UE_LOGW("atv: %s refuses the ATV key '%ls' (%s) -- not announced / not taken (said once per key)",
                side, key.c_str(), why);
}

void SendAtvDestroy(const std::wstring& key) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    coop::net::AtvDestroyPayload p{};
    WireKeyFromString(key, p.key);
    s->SendReliable(coop::net::ReliableKind::AtvDestroy, &p, sizeof(p));
}
}  // namespace

// "Live", for the index: live by index, an ATV, and in the world this peer is in now. IsAtv comes
// before any key read, since GetKeyString reads a fixed offset, and the world test is what the old
// "found is authoritative" prune carried through the hub's world filter.
bool LiveInThisWorld(void* actor, int32_t idx) {
    return R::IsLiveByIndex(actor, idx) && A::IsAtv(actor) && W::WorldOf(actor) == W::CurrentWorld();
}

// Past the cap a saying is counted and not stored: a client's keys come off the wire, and an unbounded
// set of them would grow with the host's packets. The cap is hit with one log line.
bool FirstSaying(const wchar_t* topic, const std::wstring& key) {
    std::wstring entry(topic);
    entry += key;
    if (g_saidKeys.count(entry)) return false;
    if (g_saidKeys.size() >= spawn_park::kCap) {
        if (g_saidPastCap++ == 0)
            UE_LOGW("atv: %zu keys already said -- further sayings are counted, not stored (said once)",
                    spawn_park::kCap);
        return false;
    }
    g_saidKeys.insert(std::move(entry));
    return true;
}

bool KeyNamesAnAtv(const std::wstring& key, const char* side) {
    if (AnnounceableAtvKey(key)) return true;
    Refuse(key, key.empty() ? "empty" : key == L"None" ? "None" : "longer than 31 chars", side);
    return false;
}

bool WireKeyNamesAnAtv(const coop::net::WireKey& k, const char* side) {
    if (k.len > kMaxKeyChars) { Refuse(StringFromWireKey(k), "longer than 31 chars", side); return false; }
    return KeyNamesAnAtv(StringFromWireKey(k), side);
}

void ResetAnnounceState() {
    if (g_saidPastCap)
        UE_LOGI("atv: %u saying(s) past the %zu-key cap of the said-once set were counted, not stored or logged",
                g_saidPastCap, coop::atv_sync::spawn_park::kCap);
    g_saidKeys.clear();
    g_saidPastCap = 0;
}

// Announces an ATV of the host's index so receivers hold it under its game Key; slot < 0 broadcasts
// (a key the index gained), slot >= 0 seeds one joiner. Reads the class and the current pose. True
// when the packet was queued.
bool SendAtvSpawn(const std::wstring& key, void* actor, int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !actor) return false;
    void* cls = R::ClassOf(actor);
    if (!cls) return false;
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(actor, loc, rot)) return false;
    coop::net::AtvSpawnPayload p{};
    WireKeyFromString(key, p.key);
    FillWireClassName(p.className, R::ToString(R::NameOf(cls)));
    p.x = loc.X; p.y = loc.Y; p.z = loc.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    if (slot < 0) return s->SendReliable(coop::net::ReliableKind::AtvSpawn, &p, sizeof(p));
    return s->SendReliableToSlot(slot, coop::net::ReliableKind::AtvSpawn, &p, sizeof(p));
}

// The scan-hub consumer: the hub's shared sliced pass drives these callbacks, preserving the sender
// state for keys that persist (only the actor and index update). Every keyed ATV is filed under its
// own game Key. The host announces a key it gains (AtvSpawn) and a key it loses (AtvDestroy): its
// actor died, or still stands under another key; a joiner is seeded from every ATV the host's world holds
// by the connect snapshot, which walks the world and not this index (atv_sync.cpp).
uint32_t g_indexGen = 0;  // world gen of the last completed pass (stale-gen index = EMPTY)
namespace {
size_t   g_lastLogCount = SIZE_MAX;
uint64_t g_lastLogHash  = 0;
struct ScanFound { std::wstring key; void* obj; int32_t idx; };
std::vector<ScanFound> g_scanFound;   // pass scratch (GT-only)

void HubPassBegin(void*, bool) {
    g_scanFound.clear();
}

void HubMatch(void*, void* obj) {
    const std::wstring nm = R::ToString(R::NameOf(obj));
    if (nm.rfind(L"Default__", 0) == 0) return;  // skip CDO
    if (!R::IsLive(obj)) return;
    std::wstring key = A::GetKeyString(obj);
    if (key.empty() || key == L"None") return;   // not yet keyed -- the next pass picks it up
    g_scanFound.push_back({ std::move(key), obj, R::InternalIndexOf(obj) });
}

// Host: a key the index just gained goes to every connected client. The liveness is read again first
// (the pass is sliced; the actor can have died inside it).
void AnnounceGained(const std::wstring& key, const AtvEntry& e) {
    if (!LiveInThisWorld(e.actor, e.idx)) return;
    if (!SendAtvSpawn(key, e.actor, /*slot*/ -1)) return;
    UE_LOGI("atv: announced ATV key='%ls' class='%ls' (AtvSpawn)", key.c_str(),
            R::ToString(R::NameOf(R::ClassOf(e.actor))).c_str());
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    auto* sess = g_session.load(std::memory_order_acquire);
    const bool isHost = sess && sess->role() == coop::net::Role::Host;
    auto& found = g_scanFound;
    void* const thisWorld = W::CurrentWorld();
    // One prune rule on a full pass: a row goes iff its actor is not live or its current key is no
    // longer the row's. A mirror spawned while the sliced pass was in flight survives, so does a
    // transient scan miss; a recycled slot's foreign tenant and a re-key do not. A tail pass only has
    // the new arrivals in `found`, so it prunes by liveness alone. The host sends AtvDestroy for the
    // key of a row pruned in the world it is in now, whether its actor died or still stands under
    // another key (a re-key, a reused slot's new tenant: the old key names no ATV of the host's any
    // more, and the actor's new key is announced by the insert below). A row whose world has been left
    // prunes silently: the clients run that teardown on their own travel.
    // Divergence from MTA, which removes an element at its destroy call
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:369-376):
    // the SP game destroys an ATV without telling this mod, so the remove follows from the scan,
    // within a pass.
    for (auto it = g_atvs.begin(); it != g_atvs.end();) {
        const AtvEntry& row = it->second;
        const bool live = LiveInThisWorld(row.actor, row.idx);
        const bool keep = isFull ? live && A::GetKeyString(row.actor) == it->first
                                 : R::IsLiveByIndex(row.actor, row.idx);
        if (keep) { ++it; continue; }
        // Residual: in the <= 100 ms before CurrentWorld()'s memo moves at a host world swap, the old
        // world's rows still compare equal, so a destroy can reach a client still in its own world. The
        // host's re-spawn of a recurring key (a save-placed one) heals it: a flicker, not a ghost.
        if (isHost && thisWorld && row.world == thisWorld) SendAtvDestroy(it->first);
        coop::atv_host_sync::NoteAtvGone(it->first);   // its open requests can never be answered
        it = g_atvs.erase(it);
    }
    // Update or add. A wire key can outlive its actor (a save-placed key is deterministic and a
    // world's replay re-creates its actors), and the successor must not inherit the dead actor's
    // seat, syncer, correction clock and change-gate baseline: the entry stays, everything that
    // described the actor resets.
    for (auto& f : found) {
        auto row = g_atvs.find(f.key);
        const bool gained = row == g_atvs.end();
        // A key the wire cannot carry is not indexed on the host: a lane addressing it would put an
        // unaddressable key on the wire, and an ATV with no key is skipped the same way (HubMatch).
        if (gained && isHost && !KeyNamesAnAtv(f.key, "host")) continue;
        AtvEntry& e = gained ? g_atvs[f.key] : row->second;
        if (e.actor && e.actor != f.obj) {
            coop::atv_authority::ClearTable(e);
            coop::atv_authority::ResetRequests(e);
            e.haveLastSync = false; e.lastIdleSendMs = 0; e.lastPktMs = 0;
            e.lastErrCm = -1.f; e.stallPackets = 0; e.restReplaces = 0; e.lastRestPlaceMs = 0;
        }
        e.actor = f.obj;
        e.idx   = f.idx;
        e.world = W::WorldOf(f.obj);
        if (gained && isHost) AnnounceGained(f.key, e);
        if (gained && !isHost) coop::atv_host_sync::NoteAtvIndexed(f.key, f.obj);   // a state held for it is applied
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
}  // namespace

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "atv", nullptr, &A::EnsureResolved, &A::IsAtv,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 15});
}

// The receiver files the world's own copy of an announced ATV the way the hub files it, with one difference
// the hub never makes: the row is not a mirror this lane spawned, so a disconnect leaves the actor standing.
void AdoptNative(const std::wstring& key, void* actor) {
    AtvEntry e{};
    e.actor = actor;
    e.idx   = R::InternalIndexOf(actor);
    e.world = W::WorldOf(actor);
    g_atvs[key] = std::move(e);
    coop::atv_host_sync::NoteAtvIndexed(key, actor);   // a state held for it is applied
}

}  // namespace detail

bool AnnounceableAtvKey(const std::wstring& key) {
    return !key.empty() && key != L"None" && key.size() <= detail::kMaxKeyChars;
}

size_t ForEachAnnounceableAtv(void (*fn)(void* ctx, void* actor, const std::wstring& key), void* ctx) {
    struct Walk { void (*fn)(void*, void*, const std::wstring&); void* ctx; } walk{fn, ctx};
    return A::ForEachAtv([](void* p, void* actor, int32_t idx) {
        const Walk* w = static_cast<const Walk*>(p);
        if (!detail::LiveInThisWorld(actor, idx)) return;
        const std::wstring key = A::GetKeyString(actor);
        if (AnnounceableAtvKey(key)) w->fn(w->ctx, actor, key);
    }, &walk);
}

}  // namespace coop::atv_sync
