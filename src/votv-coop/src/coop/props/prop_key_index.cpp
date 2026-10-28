// coop/props/prop_key_index.cpp -- every live keyed prop of this peer by its key. See the header.

#include "coop/props/prop_key_index.h"

#include "coop/config/config.h"            // [dev] key_index_seed_hold_sec, key_index_forget_one
#include "coop/config/config_registry.h"
#include "coop/element/object_scan_hub.h"  // the parity pass rides its slices
#include "coop/player/hand_item.h"         // CollectHandAxisActors: a hand is never a holder
#include "coop/player/players_registry.h"  // kMaxPeers
#include "ue_wrap/actors/prop.h"           // KeyFieldOf, ReadKeyField
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"   // the prop base name
#include "ue_wrap/engine/engine.h"          // IsChildActor
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::prop_key_index {
namespace {

namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace OI = ue_wrap::object_index;
namespace WI = ue_wrap::world_identity;
using ue_wrap::prop::KeyField;
using Clock = std::chrono::steady_clock;

// The four bodies after which a prop's key may have changed (the header says why each).
UE_FACT_FN(kRowInit,     "prop_key_index", nullptr, L"init",     AnyClass);
UE_FACT_FN(kRowSetKey,   "prop_key_index", nullptr, L"setKey",   AnyClass);
UE_FACT_FN(kRowGetKey,   "prop_key_index", nullptr, L"getKey",   AnyClass);
UE_FACT_FN(kRowLoadData, "prop_key_index", nullptr, L"loadData", AnyClass);
constexpr int kTag = 0x504B4958;  // 'PKIX'

bool SameName(const R::FName& a, const R::FName& b) {
    return a.ComparisonIndex == b.ComparisonIndex && a.Number == b.Number;
}

// One holder of a key: the object, its slot, the slot's serial when it was indexed (a successor in the same
// slot at the same address reads as another object) and the key it held then.
struct Holder {
    void*    obj;
    int32_t  idx;
    int32_t  serial;
    R::FName key;
};
// What a slot is indexed under, so a write that leaves the key as it was costs one probe and no string.
struct SlotEntry {
    void*        obj;
    int32_t      serial;
    R::FName     key;
    std::wstring keyStr;
};
struct Parked {
    void*             obj;
    int32_t           idx;
    Clock::time_point since;
};
// A birth still loading after this long stops holding a miss at Pending: loading is seconds, and one
// object stuck in it must not keep every miss from being an answer. It stays parked and is still indexed
// when it finishes; the first one this happens to is named. Source tells its listeners when an entity has
// finished spawning (entitylist.cpp:1176, OnEntitySpawned); the engine's object notifications here carry
// no such event, so a birth still loading is polled until it reads as loaded.
constexpr auto kParkedHoldsPending = std::chrono::seconds(5);

std::unordered_map<std::wstring, std::vector<Holder>> g_byKey;
std::unordered_map<int32_t, SlotEntry>                g_bySlot;
std::vector<Parked>                                   g_parked;  // born while still loading
// Moves on every change of the two tables above: a holder filed, dropped or re-filed, a seed, the end of a hold.
uint64_t                                              g_version = 0;

bool g_installed = false;
bool g_watchRefused = false;
bool g_listening = false;
bool g_wanted = false;  // the session holds the gate: Begin was called and End not yet
bool g_live = false;    // seeded in this hold
bool g_deadSaid = false;
// The install cannot index: a watch or the listener refused, or a watch dead for good. Read off the game thread
// by the session start's refusal.
std::atomic<bool> g_unavailable{false};
bool g_namesSettled = false;  // all four watches live or dead for good
bool g_staleParkSaid = false;
bool g_findPendingSaid = false;  // the first Find that met Pending this session is named
bool g_forgotOne = false;        // [dev] key_index_forget_one: done for this session
const char* g_pendingWhy = "";   // why the last Pending was answered, for the line that names one
uint64_t g_offThreadBase = 0;  // the script gate's off-thread count when the summary window opened
Clock::time_point g_beganAt{};  // the session's Begin, which the dev seed hold counts from
std::atomic<uint64_t> g_offThreadReads{0};  // reads from another thread, which answer Pending

// The 60 s summary.
struct Sum {
    uint64_t births = 0, deaths = 0, postsIn = 0, postsOut = 0, reindexed = 0, reads = 0, hits = 0, misses = 0,
             pending = 0, findPending = 0, catchUps = 0, unobserved = 0, pruned = 0, visited = 0;
    size_t parkedHigh = 0;
};
Sum g_sum;
Clock::time_point g_sumSince{};

// The key field of each class, asked once per class pointer. A class that unloads may leave its pointer to
// a new class, so the memo is dropped when the object index unlists one. A class can only load after its
// super, so a None answered for a subclass is never the result of a base still to come.
std::unordered_map<void*, KeyField> g_fieldOf;
uint64_t g_fieldOfVersion = 0;

// Both classes a key field can come from are loaded: until then a class asked may read as keyless only because
// its base is still loading.
bool KeyBasesLoaded() {
    return OI::ClassByName(ue_wrap::profile::name::PropClass) && OI::ClassByName(L"trashBitsPile_C");
}

KeyField FieldOf(void* cls) {
    if (!cls) return KeyField::None;
    const uint64_t v = OI::ClassUnlistVersion();
    if (v != g_fieldOfVersion) {
        g_fieldOf.clear();
        g_fieldOfVersion = v;
    }
    const auto it = g_fieldOf.find(cls);
    if (it != g_fieldOf.end()) return it->second;
    const KeyField f = ue_wrap::prop::KeyFieldOf(cls);
    // A None is kept only once both bases are loaded: one still loading reads as absent, and a subclass
    // asked in that moment would stay out for good.
    if (f != KeyField::None || KeyBasesLoaded())
        g_fieldOf.emplace(cls, f);
    return f;
}

// The hands (the local player's held item and every peer's display copy), read once per session tick: a hand
// carries the held item's key and is never the world prop a key names.
void* g_hand[1 + coop::players::kMaxPeers];
size_t g_handN = 0;

void RefreshHand() { g_handN = coop::hand_item::CollectHandAxisActors(g_hand, 1 + coop::players::kMaxPeers); }

bool IsHand(void* obj) {
    for (size_t i = 0; i < g_handN; ++i)
        if (g_hand[i] == obj) return true;
    return false;
}

// Remove one holder from one key, and the slot's entry if it still names that holder.
void DropHolder(const std::wstring& key, int32_t idx, void* obj) {
    ++g_version;
    const auto kt = g_byKey.find(key);
    if (kt != g_byKey.end()) {
        auto& v = kt->second;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i].idx == idx && v[i].obj == obj) {
                v[i] = v.back();
                v.pop_back();
                break;
            }
        }
        if (v.empty()) g_byKey.erase(kt);
    }
    const auto st = g_bySlot.find(idx);
    if (st != g_bySlot.end() && st->second.obj == obj && st->second.keyStr == key) g_bySlot.erase(st);
}

void Unindex(int32_t idx) {
    const auto st = g_bySlot.find(idx);
    if (st == g_bySlot.end()) return;
    const std::wstring key = st->second.keyStr;
    DropHolder(key, idx, st->second.obj);
}

// Index `obj` (live, readable, of a class with a key field) under the key its field holds now.
void Index(void* obj, int32_t idx, KeyField f) {
    const R::FName key = ue_wrap::prop::ReadKeyField(obj, f);
    const auto st = g_bySlot.find(idx);
    if (st != g_bySlot.end() && st->second.obj == obj && st->second.serial == R::SlotSerial(idx) &&
        SameName(st->second.key, key))
        return;  // as it was
    Unindex(idx);
    if (key.ComparisonIndex == 0) return;  // None: no key to be found by
    std::wstring keyStr = R::ToString(key);
    const int32_t serial = R::AllocateSlotSerial(idx);
    ++g_version;
    g_byKey[keyStr].push_back(Holder{obj, idx, serial, key});
    g_bySlot[idx] = SlotEntry{obj, serial, key, std::move(keyStr)};
    ++g_sum.reindexed;
}

// A domain object the index meets: indexed if readable, parked while it still loads. One the slot already
// files is re-read straight away (a write after its birth); otherwise an object marked for death is left
// out, and a class default object, which holds the class's default key, is never an instance.
void Admit(void* obj, int32_t idx, KeyField f) {
    const auto st = g_bySlot.find(idx);
    if (st != g_bySlot.end() && st->second.obj == obj && st->second.serial == R::SlotSerial(idx)) {
        Index(obj, idx, f);
        return;
    }
    if (!R::IsLiveByIndex(obj, idx)) return;
    if (R::SlotFlags(idx) & R::slot_flags::NotYetReadable) {
        for (const Parked& p : g_parked)
            if (p.idx == idx && p.obj == obj) return;  // parked already
        g_parked.push_back(Parked{obj, idx, Clock::now()});
        if (g_parked.size() > g_sum.parkedHigh) g_sum.parkedHigh = g_parked.size();
        return;
    }
    if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
    Index(obj, idx, f);
}

// The parked births whose object has finished loading are indexed; the gone are dropped.
void ReadParked() {
    size_t keep = 0;
    for (size_t i = 0; i < g_parked.size(); ++i) {
        const Parked p = g_parked[i];
        if (R::ObjectAt(p.idx) != p.obj || (R::SlotFlags(p.idx) & R::slot_flags::Unreachable)) continue;
        if (R::SlotFlags(p.idx) & R::slot_flags::NotYetReadable) {
            g_parked[keep++] = p;
            continue;
        }
        if (!R::IsLiveByIndex(p.obj, p.idx)) continue;  // marked for death while it loaded
        const KeyField f = FieldOf(R::ClassOf(p.obj));
        if (f != KeyField::None && !R::NameStartsWith(R::NameOf(p.obj), L"Default__")) Index(p.obj, p.idx, f);
    }
    g_parked.resize(keep);
}

// Whether a parked birth may still hold the key a read did not find: one parked for less than the bound.
bool ParkedHoldsPending() {
    if (g_parked.empty()) return false;
    const auto now = Clock::now();
    bool young = false;
    for (const Parked& p : g_parked) {
        if (now - p.since < kParkedHoldsPending) {
            young = true;
        } else if (!g_staleParkSaid) {
            g_staleParkSaid = true;
            UE_LOGW("prop_key_index: a %ls has been loading for more than 5 s (slot %d) -- it no longer holds a "
                    "miss at Pending; it is indexed when it finishes (the first of this run)",
                    R::ClassNameOf(p.obj).c_str(), p.idx);
        }
    }
    return young;
}

// A filing the feeds missed, found by a read (a key that moved through a body no watch saw) or by the parity
// pass (a prop never filed, or filed under a key it no longer holds): one count for the run, the first named
// with what found it, counted before it is filed again so the repair can never be what makes the count zero.
void NoteUnobserved(void* obj, int32_t idx, const wchar_t* was, const char* foundBy) {
    if (++g_sum.unobserved == 1)
        UE_LOGW("prop_key_index: a filing the feeds missed -- %ls slot=%d filed under '%ls' (found by %s; filed "
                "again; the first of this run)",
                R::ClassNameOf(obj).c_str(), idx, was, foundBy);
}

// Holders whose object is gone, dropped in one pass: a death the object index reports for a slot it
// never listed reaches no listener, so the read's own drop is not the only collector.
void PruneDead() {
    std::vector<std::pair<std::wstring, Holder>> dead;
    for (const auto& [key, holders] : g_byKey)
        for (const Holder& h : holders)
            if (!R::IsLiveByIndex(h.obj, h.idx) || R::SlotSerial(h.idx) != h.serial) dead.push_back({key, h});
    for (const auto& [key, h] : dead) DropHolder(key, h.idx, h.obj);
    g_sum.pruned += dead.size();
}

// ---- the feeds ----

void OnCreated(void*, void* obj, void* cls, int32_t idx) {
    if (!g_live) return;
    const KeyField f = FieldOf(cls);
    if (f == KeyField::None) return;
    ++g_sum.births;
    Admit(obj, idx, f);
}

// The object may be freed by now: only its pointer is compared.
void OnDeleted(void*, void* obj, void*, int32_t idx) {
    if (!g_live) return;
    const auto st = g_bySlot.find(idx);
    if (st == g_bySlot.end() || st->second.obj != obj) return;
    ++g_sum.deaths;
    Unindex(idx);
}

void OnPost(const SG::Call& call) {
    if (!g_live || !call.object) return;
    const KeyField f = FieldOf(R::ClassOf(call.object));
    if (f == KeyField::None) {
        ++g_sum.postsOut;
        return;
    }
    ++g_sum.postsIn;
    const int32_t idx = R::InternalIndexOf(call.object);
    if (idx >= 0) Admit(call.object, idx, f);
}

// ---- the reads ----

enum class Verdict : uint8_t { Counts, Dead, Moved, Excluded };

// A holder counts when its slot still holds it with its serial and no kill flags, it is readable, its class
// still keeps a key field holding the key it was indexed under, it is neither a child actor nor a hand, and
// it belongs to the world the game is running (the scan hub's rule, object_scan_hub.cpp).
Verdict Judge(const Holder& h, void* world) {
    if (!R::IsLiveByIndex(h.obj, h.idx) || R::SlotSerial(h.idx) != h.serial) return Verdict::Dead;
    if (R::SlotFlags(h.idx) & R::slot_flags::NotYetReadable) return Verdict::Excluded;
    const KeyField f = FieldOf(R::ClassOf(h.obj));
    if (f == KeyField::None) return Verdict::Dead;
    if (!SameName(ue_wrap::prop::ReadKeyField(h.obj, f), h.key)) return Verdict::Moved;
    if (ue_wrap::engine::IsChildActor(h.obj) || IsHand(h.obj)) return Verdict::Excluded;
    if (world && WI::WorldOf(h.obj) != world) return Verdict::Excluded;
    return Verdict::Counts;
}

// The lowest-slot holder of `key` that counts and is not `skip`. A dead holder is dropped; one whose key
// changed through a body no watch saw is re-indexed and named once, since a read can then miss a key it
// should have found.
void* Best(const std::wstring& key, void* world, void* skip) {
    const auto kt = g_byKey.find(key);
    if (kt == g_byKey.end()) return nullptr;
    void* best = nullptr;
    int32_t bestIdx = INT32_MAX;
    std::vector<Holder> dead, moved;  // no allocation unless one is met
    for (const Holder& h : kt->second) {
        switch (Judge(h, world)) {
            case Verdict::Counts:
                if (h.obj != skip && h.idx < bestIdx) {
                    best = h.obj;
                    bestIdx = h.idx;
                }
                break;
            case Verdict::Dead:  dead.push_back(h); break;
            case Verdict::Moved: moved.push_back(h); break;
            case Verdict::Excluded: break;
        }
    }
    for (const Holder& h : dead) DropHolder(key, h.idx, h.obj);
    for (const Holder& h : moved) {
        NoteUnobserved(h.obj, h.idx, key.c_str(), "a read: the key changed");
        Index(h.obj, h.idx, FieldOf(R::ClassOf(h.obj)));
    }
    return best;
}

// `catchUp`: a miss first catches the object index up and asks again, so it is final; without it a miss is
// "none indexed now", which a uniqueness check at enrolment asks for, run inside a script body.
Read Ask(const std::wstring& key, void* skip, void** out, bool catchUp) {
    if (!ue_wrap::game_thread::IsGameThread()) {
        g_offThreadReads.fetch_add(1, std::memory_order_relaxed);
        return Read::Pending;  // no reason kept: g_pendingWhy is the game thread's
    }
    ++g_sum.reads;
    // No current world is "not yet" across a travel; on a build whose world lookup could not resolve it is
    // for good, and the world term fails open there, as world_identity asks of a term that guards a read.
    void* const world = WI::CurrentWorld();
    if (!g_live || (!world && !WI::Degraded())) {
        g_pendingWhy = !g_live ? "the index is not seeded" : "no current world";
        ++g_sum.pending;
        return Read::Pending;
    }
    if (key.empty() || key == L"None") {
        ++g_sum.misses;
        return Read::Miss;
    }
    RefreshHand();  // a hand made earlier in this tick must not answer for the world prop
    if (void* a = Best(key, world, skip)) {
        if (out) *out = a;
        ++g_sum.hits;
        return Read::Hit;
    }
    if (!catchUp) {
        if (ParkedHoldsPending()) {
            g_pendingWhy = "a prop still loading may hold it";
            ++g_sum.pending;
            return Read::Pending;
        }
        ++g_sum.misses;
        return Read::Miss;
    }
    // Not held as the index stands: catch the object index up to this instant, read the births that have
    // finished loading, and ask again. A miss after that is the truth now.
    ++g_sum.catchUps;
    if (OI::DrainAll() == SIZE_MAX) {
        g_pendingWhy = "asked inside an object-index drain";
        ++g_sum.pending;
        return Read::Pending;
    }
    if (!g_parked.empty()) ReadParked();
    if (void* a = Best(key, world, skip)) {
        if (out) *out = a;
        ++g_sum.hits;
        return Read::Hit;
    }
    if (ParkedHoldsPending()) {
        g_pendingWhy = "a prop still loading may hold it";
        ++g_sum.pending;
        return Read::Pending;
    }
    ++g_sum.misses;
    return Read::Miss;
}

// ---- the parity pass ----
//
// A scan-hub consumer over every class with a key field: an object the hub hands out (live, readable, in the
// current world, its slot still holding it) whose key the store does not file in that slot, with that serial,
// under that key, is a filing the feeds missed. MTA asserts the parity of two containers it mutates itself, at
// each mutation (reference/mtasa-blue/Server/mods/deathmatch/logic/CPlayerManager.cpp:395,403), and its assert
// stays on in a release and stops the process (reference/mtasa-blue/Shared/sdk/SharedUtil.Defines.h:155-165).
// Ours checks a store against objects the engine owns, where a miss is a write no feed saw rather than our own
// bug, so it is counted, named and filed again instead.
bool ParityReady() { return g_live; }
bool ParityIsInstance(void* obj) { return FieldOf(R::ClassOf(obj)) != KeyField::None; }  // class-pure
// FieldOf answers None for a class asked while a base is still loading, without keeping it; the hub keeps its
// verdicts, so it takes them again when both bases have loaded.
uint64_t ParityBasesLoaded() { return KeyBasesLoaded() ? 1 : 0; }
void ParityBegin(void*, bool) {}

void ParityMatch(void*, void* obj) {
    if (!g_live) return;
    ++g_sum.visited;
    const KeyField f = FieldOf(R::ClassOf(obj));
    const int32_t idx = R::InternalIndexOf(obj);
    if (f == KeyField::None || idx < 0 || !R::IsLiveByIndex(obj, idx)) return;
    const R::FName key = ue_wrap::prop::ReadKeyField(obj, f);
    const auto st = g_bySlot.find(idx);
    const bool slotIsIt = st != g_bySlot.end() && st->second.obj == obj && st->second.serial == R::SlotSerial(idx);
    // A prop without a key is filed nowhere; one with a key is filed under it.
    if (key.ComparisonIndex == 0 ? !slotIsIt : (slotIsIt && SameName(st->second.key, key))) return;
    if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;  // never filed: it holds the class's default
    for (const Parked& p : g_parked) {
        if (p.idx == idx && p.obj == obj) {  // finished loading since ReadParked ran: filed now, not a miss
            Index(obj, idx, f);
            return;
        }
    }
    NoteUnobserved(obj, idx, slotIsIt ? st->second.keyStr.c_str() : L"(nothing)", "the parity pass");
    Index(obj, idx, f);
}

size_t ParityComplete(void*, bool, uint32_t) { return g_bySlot.size(); }

// [dev] key_index_forget_one: once a session, one filed prop of the current world is dropped from the store, the
// miss the parity pass exists to find; the pass must name it and file it again within a full pass.
void ForgetOneIfAsked() {
    static const bool sOn = coop::config::ResolveFlag(coop::config_registry::rows::key_index_forget_one);
    if (!sOn || g_forgotOne) return;
    void* const world = WI::CurrentWorld();
    if (!world) return;
    for (const auto& [idx, e] : g_bySlot) {
        if (!R::IsLiveByIndex(e.obj, idx) || WI::WorldOf(e.obj) != world) continue;
        UE_LOGI("prop_key_index: [dev] key_index_forget_one dropped %ls slot=%d key '%ls' from the store",
                R::ClassNameOf(e.obj).c_str(), idx, e.keyStr.c_str());
        g_forgotOne = true;
        Unindex(idx);
        return;
    }
}

// ---- the session ----

bool WatchLive(const ue_wrap::facts::Function& row) { return SG::NameWatchLive(row, kTag); }
// Dead for good: settled and not live once no name is still resolving (a placeholder can read settled
// while its live entry is still resolving).
bool WatchDead(const ue_wrap::facts::Function& row) {
    return SG::PendingNameCount() == 0 && SG::NameWatchSettled(row, kTag) && !SG::NameWatchLive(row, kTag);
}

// [dev] key_index_seed_hold_sec: the session's seed waits that long from Begin, so the index is not Live and a
// joiner's world-ready gate names its wait on it and then clears, which a healthy run never shows. It delays only
// this machine's own seed, as a slow name resolve would, and opens nothing: not a switch on a receiving gate.
bool SeedHeldForDrill() {
    static const long sHoldSec = coop::config::ResolveInt(coop::config_registry::rows::key_index_seed_hold_sec);
    return sHoldSec > 0 && Clock::now() < g_beganAt + std::chrono::seconds(sHoldSec);
}

void TrySeed() {
    // The install is harness::Start's first game-thread task; asked again here, it drives the watches'
    // names on, and waits while the script gate itself is not armed.
    Install();
    if (!g_installed || g_deadSaid) return;
    if (g_watchRefused || !g_listening || WatchDead(kRowInit) || WatchDead(kRowSetKey) || WatchDead(kRowGetKey) ||
        WatchDead(kRowLoadData)) {
        g_unavailable.store(true, std::memory_order_release);
        if (!g_deadSaid) {
            g_deadSaid = true;
            UE_LOGE("prop_key_index: a key watch or the object-index listener is not available -- the index "
                    "stays unseeded and every read answers Pending this session");
        }
        return;
    }
    if (!WatchLive(kRowInit) || !WatchLive(kRowSetKey) || !WatchLive(kRowGetKey) || !WatchLive(kRowLoadData))
        return;  // a name still resolving: the next tick
    if (SeedHeldForDrill()) return;
    // Caught up first: the seed is the moment the index promises to hold every live keyed prop, and the
    // host's uniqueness check reads it as it stands in the very ticks the census enrols the world. Seeding
    // from a half-applied object index left it empty there while a load's births were still queued.
    const auto t0 = Clock::now();
    if (OI::DrainAll() == SIZE_MAX) return;
    g_byKey.clear();
    g_bySlot.clear();
    g_parked.clear();
    ++g_version;
    std::vector<void*> classes;
    OI::ForEachClass(
        [](void* ctx, void* cls, void*) {
            if (FieldOf(cls) != KeyField::None) static_cast<std::vector<void*>*>(ctx)->push_back(cls);
        },
        &classes);
    for (void* cls : classes) {
        KeyField f = FieldOf(cls);
        OI::ForEachInstance(
            cls, [](void* ctx, void* obj, int32_t idx) { Admit(obj, idx, *static_cast<KeyField*>(ctx)); }, &f);
    }
    g_live = true;
    ++g_version;
    UE_LOGI("prop_key_index: seeded %zu keyed props under %zu keys from %zu classes, %zu still loading, in %lld us",
            g_bySlot.size(), g_byKey.size(), classes.size(), g_parked.size(),
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count()));
}

void SummaryIfDue() {
    const auto now = Clock::now();
    if (g_sumSince.time_since_epoch().count() == 0) g_sumSince = now;
    if (now - g_sumSince < std::chrono::seconds(60)) return;
    g_sumSince = now;
    PruneDead();
    const uint64_t gateOffThread = SG::GetStats().offGameThread;
    UE_LOGI("prop_key_index: summary (60s): keys=%zu props=%zu parked=%zu (high %zu) births=%llu deaths=%llu "
            "posts=%llu in-domain/%llu other reindexed=%llu reads=%llu hit=%llu miss=%llu pending=%llu "
            "find-pending=%llu catch-ups=%llu off-thread=%llu unobserved=%llu pruned=%llu parity-visited=%llu "
            "gate-off-thread=%llu",
            g_byKey.size(), g_bySlot.size(), g_parked.size(), g_sum.parkedHigh,
            static_cast<unsigned long long>(g_sum.births), static_cast<unsigned long long>(g_sum.deaths),
            static_cast<unsigned long long>(g_sum.postsIn), static_cast<unsigned long long>(g_sum.postsOut),
            static_cast<unsigned long long>(g_sum.reindexed), static_cast<unsigned long long>(g_sum.reads),
            static_cast<unsigned long long>(g_sum.hits), static_cast<unsigned long long>(g_sum.misses),
            static_cast<unsigned long long>(g_sum.pending), static_cast<unsigned long long>(g_sum.findPending),
            static_cast<unsigned long long>(g_sum.catchUps),
            static_cast<unsigned long long>(g_offThreadReads.exchange(0, std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_sum.unobserved),
            static_cast<unsigned long long>(g_sum.pruned), static_cast<unsigned long long>(g_sum.visited),
            static_cast<unsigned long long>(gateOffThread - g_offThreadBase));
    g_offThreadBase = gateOffThread;
    const uint64_t unobserved = g_sum.unobserved;
    g_sum = Sum{};
    g_sum.unobserved = unobserved;  // a count of the run: its first one is named once
}

}  // namespace

void Install() {
    if (g_installed) {
        // A name watch resolves on the game thread when someone drives it; until all four have settled
        // (live, or dead for good), this tick does.
        if (!g_namesSettled) {
            SG::ResolvePendingNames();
            g_namesSettled = SG::NameWatchSettled(kRowInit, kTag) && SG::NameWatchSettled(kRowSetKey, kTag) &&
                             SG::NameWatchSettled(kRowGetKey, kTag) && SG::NameWatchSettled(kRowLoadData, kTag);
        }
        return;
    }
    if (!SG::IsInstalled()) return;
    g_installed = true;
    const bool ok = SG::WatchName(kRowInit, kTag, nullptr, &OnPost) &&
                    SG::WatchName(kRowSetKey, kTag, nullptr, &OnPost) &&
                    SG::WatchName(kRowGetKey, kTag, nullptr, &OnPost) &&
                    SG::WatchName(kRowLoadData, kTag, nullptr, &OnPost);
    g_watchRefused = !ok;
    g_listening = OI::AddListener(OI::Listener{nullptr, &OnCreated, &OnDeleted});
    if (g_watchRefused || !g_listening) g_unavailable.store(true, std::memory_order_release);
    // [dev] key_index_refuse_install: the install reads as unable to index, the one way to see a session refused
    // (MV-J32) on a build that hooks fine. Nothing is refused in the gate or the watches themselves.
    if (coop::config::ResolveFlag(coop::config_registry::rows::key_index_refuse_install)) {
        UE_LOGW("prop_key_index: [dev] key_index_refuse_install -- this install reads as unable to index");
        g_unavailable.store(true, std::memory_order_release);
    }
    if (g_watchRefused || !g_listening)
        UE_LOGE("prop_key_index: install incomplete (watches %s, listener %s)", ok ? "taken" : "REFUSED",
                g_listening ? "taken" : "REFUSED");
}

void Begin() {
    // The parity pass joins the scan hub at the first session, as the hub's other consumers do: the hub is
    // ticked only in a session, so a consumer registered at boot would set its observer collecting births for
    // a single-player game that never drains them.
    static bool sParityRegistered = false;
    if (!sParityRegistered) {
        sParityRegistered = true;
        coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
            "prop_key_parity", nullptr, &ParityReady, &ParityIsInstance, &ParityBegin, &ParityMatch,
            &ParityComplete, /*settleScans*/ 0,  // demand-exempt: rides the tail passes and the full backstop
            &ParityBasesLoaded});
    }
    g_wanted = true;
    g_beganAt = Clock::now();
    g_findPendingSaid = false;
    g_forgotOne = false;
    if (!g_live) TrySeed();
}

void End() {
    g_wanted = false;
    g_live = false;
    g_byKey.clear();
    g_bySlot.clear();
    g_parked.clear();
    ++g_version;
}

void Tick() {
    if (!g_wanted) return;
    if (!g_live) TrySeed();
    if (!g_live) return;
    RefreshHand();
    if (!g_parked.empty()) ReadParked();
    ForgetOneIfAsked();
    SummaryIfDue();
}

Read Resolve(const std::wstring& key, void** out) { return Ask(key, nullptr, out, /*catchUp=*/true); }

void* Find(const std::wstring& key) {
    void* actor = nullptr;
    const Read r = Ask(key, nullptr, &actor, /*catchUp=*/true);
    // A Pending here is answered as no holder, the one place "not yet" becomes "not here": counted apart, and the
    // first of a session named, since a run that shows none is the evidence this costs nothing.
    if (r == Read::Pending && ue_wrap::game_thread::IsGameThread() && g_wanted) {
        ++g_sum.findPending;
        if (!g_findPendingSaid) {
            g_findPendingSaid = true;
            UE_LOGW("prop_key_index: a read by key could not be answered -- '%ls' (%s); its caller takes it as no "
                    "holder (the first this session)",
                    key.c_str(), g_pendingWhy);
        }
    }
    return r == Read::Hit ? actor : nullptr;
}

Read AnotherHolder(const std::wstring& key, void* self, void** other) {
    return Ask(key, self, other, /*catchUp=*/false);
}

bool CanAnswer() {
    if (!g_wanted) return true;  // no session index to wait for
    // Live comes on the first session tick after the four names resolve, and an any-class name watch never
    // dies once resolved; an install that cannot index never starts a session, and a join that meets one ends
    // (Unavailable). Unlike the barrier's other terms this one has no deadline and no degraded announce: an
    // index that does not hold the world would duplicate it. The per-batch drain reaches a zero
    // backlog on its own: a gate asked every tick never drains (DrainAll answers a miss, not a tick). A birth
    // still loading does not hold the gate, since one after another could hold it without end; a read that
    // meets one answers Pending, and the summary counts it.
    return g_live && OI::Backlog() == 0;
}

bool Unavailable() { return g_unavailable.load(std::memory_order_acquire); }

size_t Size() { return g_bySlot.size(); }

uint64_t Version() { return g_version; }

void DebugCollect(std::vector<DebugEntry>& out) {
    for (const auto& [key, holders] : g_byKey)
        for (const Holder& h : holders) out.push_back(DebugEntry{h.obj, h.idx, key});
}

}  // namespace coop::prop_key_index
