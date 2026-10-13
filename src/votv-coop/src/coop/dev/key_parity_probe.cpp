// coop/dev/key_parity_probe.cpp -- [dev] the prop key index against the live world. See the header.
#include "coop/dev/key_parity_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/registry.h"  // EidForActor, naming a persistent miss
#include "coop/player/hand_item.h"  // CollectHandAxisActors: the index never holds the hand axis
#include "coop/player/players_registry.h"  // kMaxPeers
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_key_index.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"  // IsChildActor
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::dev::key_parity_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace WI = ue_wrap::world_identity;
namespace PT = coop::prop_element_tracker;
using Clock = std::chrono::steady_clock;

// The four bodies after which a prop's key can have changed: init writes it from getKey (a None key
// becomes a random one), loadData writes the saved key and runs init, the setKey event writes it alone, and
// a getKey follows the one raw cross-object write the game makes (ATV.createContainer).
UE_FACT_FN(kRowInit,     "dev.key_parity_probe", nullptr, L"init",     AnyClass);
UE_FACT_FN(kRowSetKey,   "dev.key_parity_probe", nullptr, L"setKey",   AnyClass);
UE_FACT_FN(kRowGetKey,   "dev.key_parity_probe", nullptr, L"getKey",   AnyClass);
UE_FACT_FN(kRowLoadData, "dev.key_parity_probe", nullptr, L"loadData", AnyClass);
enum Body : int { kInit = 0, kSetKey, kGetKey, kLoadData, kBodies };
constexpr int kTagBase = 0x4B505200;  // 'KPR' + body
const wchar_t* const kBodyName[kBodies] = {L"init", L"setKey", L"getKey", L"loadData"};

// The keyed domain: prop_C's lineage (its own `key`) and trashBitsPile (actor_save_C's `key`). The chip
// pile and the garbage clump have no key field, and their getKey is a stub returning None.
bool InDomain(void* cls) {
    if (!cls) return false;
    if (ue_wrap::prop::IsClassDescendantOfProp(cls)) return true;
    void* bits = ue_wrap::prop::TrashBitsPileClass();
    return bits && ue_wrap::prop::WalksToBase(cls, bits);
}

bool SameName(const R::FName& a, const R::FName& b) {
    return a.ComparisonIndex == b.ComparisonIndex && a.Number == b.Number;
}

// The shadow index, by slot: the tenant it saw (pointer and serial) and the key it read after the body.
struct Shadow {
    void*   obj    = nullptr;
    int32_t serial = 0;
    R::FName key{0, 0};
};
std::unordered_map<int32_t, Shadow> g_shadow;
std::uint64_t g_posts[kBodies] = {};
std::uint64_t g_changed[kBodies] = {};

void Post(const SG::Call& call) {
    void* obj = call.object;
    if (!obj) return;
    if (!InDomain(R::ClassOf(obj))) return;
    const int body = call.tag - kTagBase;
    if (body < 0 || body >= kBodies) return;
    ++g_posts[body];
    const int32_t idx = R::InternalIndexOf(obj);
    if (idx < 0) return;
    const R::FName key = ue_wrap::prop::GetInteractableKey(obj);  // a field read on this domain
    const int32_t serial = R::AllocateSlotSerial(idx);
    Shadow& s = g_shadow[idx];
    if (s.obj != obj || s.serial != serial || !SameName(s.key, key)) ++g_changed[body];
    s.obj = obj;
    s.serial = serial;
    s.key = key;
}

// One live keyed prop, as the walk finds it.
struct Truth {
    void*    obj;
    int32_t  idx;
    R::FName key;
};

struct Counts {
    size_t truth = 0, dupKeys = 0;
    size_t liveMiss = 0, liveStale = 0;
    size_t shadowMiss = 0, shadowStale = 0;
    size_t lookupKeys = 0, lookupMiss = 0, lookupWrong = 0;  // per distinct live key, through the lookup itself
    // The key index: per distinct live key, Resolve's answer; and its entries the world no longer confirms.
    size_t indexMiss = 0, indexWrong = 0, indexPending = 0, indexEntries = 0, indexStale = 0, indexDead = 0;
    bool operator==(const Counts& o) const {
        return truth == o.truth && dupKeys == o.dupKeys && liveMiss == o.liveMiss && liveStale == o.liveStale &&
               shadowMiss == o.shadowMiss && shadowStale == o.shadowStale && lookupKeys == o.lookupKeys &&
               lookupMiss == o.lookupMiss && lookupWrong == o.lookupWrong && indexMiss == o.indexMiss &&
               indexWrong == o.indexWrong && indexPending == o.indexPending && indexEntries == o.indexEntries &&
               indexStale == o.indexStale && indexDead == o.indexDead;
    }
};

// Examples logged per world, so a miss is named once and not every sample.
int g_liveExamples = 0, g_shadowExamples = 0;
constexpr int kExamples = 12;
std::map<std::wstring, size_t> g_shadowMissByClass;  // per sample, logged with the examples
// A miss that outlives a load: consecutive samples missing, by slot and tenant, for each index; named
// once at the third sample (about 6 s), up to kExamples per world.
struct Run { void* obj = nullptr; int samples = 0; bool named = false; };
std::unordered_map<int32_t, Run> g_liveRun, g_shadowRun;
int g_livePersistNamed = 0, g_shadowPersistNamed = 0;
constexpr int kPersistSamples = 3;
// The lookup's misses by key, the same persistence rule: consecutive samples, named once at the third.
std::unordered_map<std::wstring, int> g_lookupRun;
int g_lookupPersistNamed = 0;
int g_indexNamed = 0;  // the key index's misses and stale entries named, per world
bool g_anotherChecked = false;  // the AnotherHolder check ran in this world

void NoteMiss(std::unordered_map<int32_t, Run>& runs, std::unordered_map<int32_t, Run>& next, int& named,
              const char* which, void* obj, int32_t idx, const std::wstring& keyStr) {
    Run r{};
    const auto it = runs.find(idx);
    if (it != runs.end() && it->second.obj == obj) r = it->second;
    r.obj = obj;
    ++r.samples;
    if (r.samples >= kPersistSamples && !r.named && named < kExamples) {
        r.named = true;
        ++named;
        const auto anyEid = coop::element::Registry::Get().EidForActor(obj);
        UE_LOGI("key_parity: %s PERSISTENT MISS (%d samples) %ls key='%ls' slot=%d eid=%u local-eid=%u", which,
                r.samples, R::ClassNameOf(obj).c_str(), keyStr.c_str(), idx, static_cast<unsigned>(anyEid),
                static_cast<unsigned>(PT::GetPropElementIdForActor(obj)));
    }
    next[idx] = r;
}

// The shadow's starting point: every live keyed prop as it stands, the seed a real index takes when its
// watches go live. Taken once, when all four watches are live.
void SeedShadow() {
    const int32_t n = R::NumObjects();
    size_t seeded = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (R::SlotFlags(i) & (R::slot_flags::NotYetReadable | R::slot_flags::Unreachable)) continue;
        if (!InDomain(R::ClassOf(obj))) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        Shadow& s = g_shadow[i];
        s.obj = obj;
        s.serial = R::AllocateSlotSerial(i);
        s.key = ue_wrap::prop::GetInteractableKey(obj);
        ++seeded;
    }
    UE_LOGI("key_parity: shadow seeded with %zu live keyed-domain objects (the four watches are live)", seeded);
}

std::wstring LevelOf(void* obj) {
    void* level = R::OuterOf(obj);
    void* world = level ? R::OuterOf(level) : nullptr;
    return world ? R::ToString(R::NameOf(world)) : L"?";
}

Counts Sample() {
    Counts c;
    void* hand[1 + coop::players::kMaxPeers];
    const size_t handN = coop::hand_item::CollectHandAxisActors(hand, 1 + coop::players::kMaxPeers);
    std::vector<Truth> truth;
    truth.reserve(4096);
    // The world the game is running: a prop of another (the menu's backdrop, a dying world) is never a
    // key's answer, by the key index's own rule.
    void* const world = WI::CurrentWorld();
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (R::SlotFlags(i) & (R::slot_flags::NotYetReadable | R::slot_flags::Unreachable)) continue;
        if (!InDomain(R::ClassOf(obj))) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        if (ue_wrap::engine::IsChildActor(obj)) continue;
        if (world && WI::WorldOf(obj) != world) continue;
        bool isHand = false;
        for (size_t h = 0; h < handN; ++h) isHand = isHand || hand[h] == obj;
        if (isHand) continue;
        const R::FName key = ue_wrap::prop::GetInteractableKey(obj);
        if (key.ComparisonIndex == 0) continue;  // None: never indexed
        truth.push_back(Truth{obj, i, key});
    }
    c.truth = truth.size();

    // The tracker's tracked keys, copied.
    std::vector<PT::TrackedKeyEntry> live;
    PT::CollectTrackedKeyEntries(live);
    std::unordered_map<void*, std::wstring> liveByActor;
    liveByActor.reserve(live.size());
    for (const auto& e : live)
        if (R::IsLiveByIndex(e.actor, e.internalIdx)) liveByActor[e.actor] = e.key;

    std::unordered_map<std::wstring, int> holders;
    holders.reserve(truth.size());
    g_shadowMissByClass.clear();
    std::unordered_map<int32_t, Run> liveNext, shadowNext;
    for (const Truth& t : truth) {
        const std::wstring keyStr = R::ToString(t.key);
        if (++holders[keyStr] == 2) ++c.dupKeys;
        const auto li = liveByActor.find(t.obj);
        if (li == liveByActor.end() || li->second != keyStr)
            NoteMiss(g_liveRun, liveNext, g_livePersistNamed, "live", t.obj, t.idx, keyStr);
        if (li == liveByActor.end()) {
            ++c.liveMiss;
            if (g_liveExamples < kExamples) {
                ++g_liveExamples;
                UE_LOGI("key_parity: live MISS #%d %ls key='%ls' slot=%d world=%ls", g_liveExamples,
                        R::ClassNameOf(t.obj).c_str(), keyStr.c_str(), t.idx, LevelOf(t.obj).c_str());
            }
        } else if (li->second != keyStr) {
            ++c.liveStale;
            if (g_liveExamples < kExamples) {
                ++g_liveExamples;
                UE_LOGI("key_parity: live STALE #%d %ls key='%ls' indexed as '%ls' slot=%d", g_liveExamples,
                        R::ClassNameOf(t.obj).c_str(), keyStr.c_str(), li->second.c_str(), t.idx);
            }
        }
        const auto si = g_shadow.find(t.idx);
        const bool seen = si != g_shadow.end() && si->second.obj == t.obj &&
                          si->second.serial == R::SlotSerial(t.idx);
        if (!seen || !SameName(si->second.key, t.key))
            NoteMiss(g_shadowRun, shadowNext, g_shadowPersistNamed, "shadow", t.obj, t.idx, keyStr);
        if (!seen) {
            ++c.shadowMiss;
            ++g_shadowMissByClass[R::ClassNameOf(t.obj)];
            if (g_shadowExamples < kExamples) {
                ++g_shadowExamples;
                UE_LOGI("key_parity: shadow MISS #%d %ls key='%ls' slot=%d world=%ls", g_shadowExamples,
                        R::ClassNameOf(t.obj).c_str(), keyStr.c_str(), t.idx, LevelOf(t.obj).c_str());
            }
        } else if (!SameName(si->second.key, t.key)) {
            ++c.shadowStale;
            if (g_shadowExamples < kExamples) {
                ++g_shadowExamples;
                UE_LOGI("key_parity: shadow STALE #%d %ls key='%ls' shadow='%ls' slot=%d", g_shadowExamples,
                        R::ClassNameOf(t.obj).c_str(), keyStr.c_str(), R::ToString(si->second.key).c_str(),
                        t.idx);
            }
        }
    }
    g_liveRun.swap(liveNext);
    g_shadowRun.swap(shadowNext);

    // The lookup a wire key goes through, asked once per distinct live key: it must answer with an actor
    // that carries that key. Asked last: a lookup that misses catches the object index up.
    std::unordered_map<std::wstring, int> lookupNext;
    for (const auto& [keyStr, holderCount] : holders) {
        ++c.lookupKeys;
        void* found = coop::prop_key_index::Find(keyStr);
        bool miss = false;
        if (!found) {
            ++c.lookupMiss;
            miss = true;
        } else if (R::ToString(ue_wrap::prop::GetInteractableKey(found)) != keyStr) {
            ++c.lookupWrong;
            miss = true;
        }
        if (!miss) continue;
        const auto it = g_lookupRun.find(keyStr);
        const int runs = (it == g_lookupRun.end() ? 0 : it->second) + 1;
        lookupNext[keyStr] = runs;
        if (runs == kPersistSamples && g_lookupPersistNamed < kExamples) {
            ++g_lookupPersistNamed;
            UE_LOGI("key_parity: lookup PERSISTENT %s (%d samples) key='%ls' holders=%d", found ? "WRONG" : "MISS",
                    runs, keyStr.c_str(), holderCount);
        }
    }
    g_lookupRun.swap(lookupNext);

    // The key index asked the same question, and its entries checked the other way: each must be a live
    // object still holding the key it is filed under. An entry whose death has not drained yet counts here.
    for (const auto& [keyStr, holderCount] : holders) {
        void* found = nullptr;
        const auto r = coop::prop_key_index::Resolve(keyStr, &found);
        if (r == coop::prop_key_index::Read::Pending) {
            ++c.indexPending;
        } else if (r == coop::prop_key_index::Read::Miss) {
            ++c.indexMiss;
            if (g_indexNamed < kExamples) {
                ++g_indexNamed;
                UE_LOGI("key_parity: index MISS key='%ls' holders=%d", keyStr.c_str(), holderCount);
            }
        } else if (R::ToString(ue_wrap::prop::GetInteractableKey(found)) != keyStr) {
            ++c.indexWrong;
        }
    }
    // The uniqueness read, once per world, on the first live key with a single holder: asked for that
    // holder it must find no other (Miss), asked for nobody it must find the holder (Hit).
    if (!g_anotherChecked) {
        for (const Truth& t : truth) {
            const std::wstring keyStr = R::ToString(t.key);
            const auto h = holders.find(keyStr);
            if (h == holders.end() || h->second != 1) continue;
            void* other = nullptr;
            const auto self = coop::prop_key_index::AnotherHolder(keyStr, t.obj);
            const auto anyone = coop::prop_key_index::AnotherHolder(keyStr, nullptr, &other);
            if (self == coop::prop_key_index::Read::Pending || anyone == coop::prop_key_index::Read::Pending) break;
            g_anotherChecked = true;
            const bool pass = self == coop::prop_key_index::Read::Miss &&
                              anyone == coop::prop_key_index::Read::Hit && other == t.obj;
            UE_LOGI("key_parity: AnotherHolder check %s key='%ls' (self -> %s, anyone -> %s)", pass ? "PASS" : "FAIL",
                    keyStr.c_str(), self == coop::prop_key_index::Read::Miss ? "Miss" : "Hit",
                    anyone == coop::prop_key_index::Read::Hit ? "Hit" : "Miss");
            break;
        }
    }

    std::vector<coop::prop_key_index::DebugEntry> entries;
    coop::prop_key_index::DebugCollect(entries);
    c.indexEntries = entries.size();
    for (const auto& e : entries) {
        if (!R::IsLiveByIndex(e.actor, e.idx)) {
            ++c.indexDead;  // its death not drained yet: a read drops it, and so does the summary's pass
            continue;
        }
        if (R::ToString(ue_wrap::prop::GetInteractableKey(e.actor)) != e.key) {
            ++c.indexStale;
            if (g_indexNamed < kExamples) {
                ++g_indexNamed;
                UE_LOGI("key_parity: index STALE entry key='%ls' slot=%d live=%d", e.key.c_str(), e.idx,
                        R::IsLiveByIndex(e.actor, e.idx) ? 1 : 0);
            }
        }
    }
    return c;
}

void Report(const Counts& c, const char* why) {
    const char* role = PT::SessionIsHost() ? "host" : (PT::SessionIsClient() ? "client" : "none");
    UE_LOGI("key_parity: %s role=%s gen=%u truth=%zu dupkeys=%zu | live miss=%zu stale=%zu | shadow miss=%zu "
            "stale=%zu | lookup keys=%zu miss=%zu wrong=%zu | index miss=%zu wrong=%zu pending=%zu entries=%zu "
            "stale=%zu dead=%zu verdict=%s | posts init=%llu setKey=%llu getKey=%llu loadData=%llu changed=%llu/%llu/%llu/%llu | "
            "offgt=%llu backlog=%zu shadow=%zu",
            why, role, WI::Generation(), c.truth, c.dupKeys, c.liveMiss, c.liveStale, c.shadowMiss, c.shadowStale,
            c.lookupKeys, c.lookupMiss, c.lookupWrong, c.indexMiss, c.indexWrong, c.indexPending, c.indexEntries,
            c.indexStale, c.indexDead,
            c.indexEntries == 0 ? "UNSEEDED"
                                : (c.indexMiss + c.indexWrong + c.indexPending + c.indexStale == 0 ? "PASS" : "FAIL"),
            static_cast<unsigned long long>(g_posts[kInit]), static_cast<unsigned long long>(g_posts[kSetKey]),
            static_cast<unsigned long long>(g_posts[kGetKey]), static_cast<unsigned long long>(g_posts[kLoadData]),
            static_cast<unsigned long long>(g_changed[kInit]), static_cast<unsigned long long>(g_changed[kSetKey]),
            static_cast<unsigned long long>(g_changed[kGetKey]),
            static_cast<unsigned long long>(g_changed[kLoadData]),
            static_cast<unsigned long long>(SG::GetStats().offGameThread), ue_wrap::object_index::Backlog(),
            g_shadow.size());
    if (c.shadowMiss > 0) {
        int shown = 0;
        for (const auto& [cls, k] : g_shadowMissByClass) {
            if (++shown > 20) break;
            UE_LOGI("key_parity:   shadow miss %6zu  %ls", k, cls.c_str());
        }
    }
}

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::key_parity_probe);
    if (!s_on) return;
    static bool s_watched = false;
    if (!s_watched && SG::IsInstalled()) {
        s_watched = true;
        SG::Acquire("key_parity_probe");
        SG::WatchName(kRowInit, kTagBase + kInit, nullptr, &Post);
        SG::WatchName(kRowSetKey, kTagBase + kSetKey, nullptr, &Post);
        SG::WatchName(kRowGetKey, kTagBase + kGetKey, nullptr, &Post);
        SG::WatchName(kRowLoadData, kTagBase + kLoadData, nullptr, &Post);
        UE_LOGI("key_parity: armed -- four POST watches (init, setKey, getKey, loadData), a walk every 2 s");
    }
    static bool s_seeded = false;
    if (s_watched && !s_seeded && SG::NameWatchLive(kRowInit, kTagBase + kInit) &&
        SG::NameWatchLive(kRowSetKey, kTagBase + kSetKey) && SG::NameWatchLive(kRowGetKey, kTagBase + kGetKey) &&
        SG::NameWatchLive(kRowLoadData, kTagBase + kLoadData)) {
        s_seeded = true;
        SeedShadow();
    }
    static std::uint32_t s_gen = 0;
    static Clock::time_point s_last = Clock::now(), s_lastPrint = Clock::now();
    static Counts s_prev{};
    const auto now = Clock::now();
    const std::uint32_t gen = WI::Generation();
    // A world change is sampled on the tick that sees it, so the first moments of a new world are read on
    // an edge rather than by the luck of the cadence.
    bool flip = false;
    if (gen != s_gen) {
        s_gen = gen;
        flip = true;
        g_liveExamples = 0;
        g_shadowExamples = 0;
        g_livePersistNamed = 0;
        g_shadowPersistNamed = 0;
        g_lookupPersistNamed = 0;
        g_indexNamed = 0;
        g_anotherChecked = false;
        UE_LOGI("key_parity: world generation %u", gen);
    }
    if (!flip && now - s_last < std::chrono::seconds(2)) return;
    s_last = now;
    const Counts c = Sample();
    // A line when a count moved or the world changed, else one every 30 s.
    if (flip || !(c == s_prev) || now - s_lastPrint >= std::chrono::seconds(30)) {
        Report(c, flip ? "world-flip" : (c == s_prev ? "steady" : "moved"));
        s_prev = c;
        s_lastPrint = now;
    }
}

}  // namespace coop::dev::key_parity_probe
