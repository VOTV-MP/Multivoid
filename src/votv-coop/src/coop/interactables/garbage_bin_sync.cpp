// coop/interactables/garbage_bin_sync.cpp -- see coop/interactables/garbage_bin_sync.h.

#include "coop/interactables/garbage_bin_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/object_scan_hub.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_flags.h"   // CallSetPropProps
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"   // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"      // ClassByName: one hashed lookup, loaded or not
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::garbage_bin_sync {
namespace {

namespace F  = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace sg = ue_wrap::script_gate;
namespace BC = coop::blob_chunks;

// The one body every write to itemsInside ends in: the add (doStuff, from the tick) and the clear (propAwoken) each call
// checkPickup, which also writes the two pickup flags. A script function called inside the Blueprint's own graph, so only
// the script gate sees it, on whatever route it is reached.
UE_FACT_FN(kRowCheckPickup, "garbage_bin_sync", L"prop_openContainer_C", L"checkPickup", Declared);
constexpr int kTagCheckPickup = 0x47424350;   // 'GBCP'

constexpr size_t kMaxKeyChars = sizeof(coop::net::WireKey::data);   // a key is 1..31 chars

std::atomic<coop::net::Session*> g_session{nullptr};
uint32_t g_world = 0;
bool g_hubRegistered = false;
bool g_watchRegistered = false;
bool g_watchSettled = false;
int  g_watchTries = 0;
constexpr int kMaxWatchTries = 600;   // one policy for a watch the gate has not taken yet: asked each tick, a bounded number of times

// HOST: a joiner whose replay waits for the scan index to be current for this world (the index's own pass-complete event
// ends the wait), and whether any slot is owed work (the replay or a send the transport refused) this tick.
coop::roster_ledger::PerSlotState<bool> g_replayOwed;
bool g_joinOwedAny = false;

// ---- the class ----

// The class and its offsets are latched on the class's listing identity: a Blueprint class dies with its world, and one
// listed again (even at the same address) answers a new id, so the latch re-resolves and never holds a dead pointer. A
// class not loaded costs one hashed lookup (never a walk of the object array); offsets that did not resolve are not tried
// again for the same listing.
void*    g_cls = nullptr;
uint64_t g_clsId = 0;
uint64_t g_failedId = 0;
int32_t g_offItems = -1, g_offPropData = -1, g_offCanHold = -1, g_offCanCollect = -1;

bool EnsureResolved() {
    namespace OI = ue_wrap::object_index;
    if (g_cls && OI::ClassListingId(g_cls) == g_clsId) return true;
    g_cls = nullptr;
    void* cls = OI::ClassByName(L"prop_garbageContainer_C");
    if (!cls) return false;
    const uint64_t id = OI::ClassListingId(cls);
    if (id == g_failedId) return false;
    g_offItems = R::FindPropertyOffset(cls, L"itemsInside");
    g_offPropData = R::FindPropertyOffset(cls, L"propData");
    void* st = R::PropertyInnerStruct(cls, L"propData");
    g_offCanHold = st ? R::FindPropertyOffsetByPrefix(st, L"canHold_") : -1;
    g_offCanCollect = st ? R::FindPropertyOffsetByPrefix(st, L"canCollect_") : -1;
    if (g_offItems < 0 || g_offPropData < 0 || g_offCanHold < 0 || g_offCanCollect < 0) {
        g_failedId = id;
        UE_LOGE("garbage_bin: prop_garbageContainer_C's itemsInside or propData's canHold/canCollect did not "
                "resolve (%d, %d, %d, %d) -- not carried", g_offItems, g_offPropData, g_offCanHold, g_offCanCollect);
        return false;
    }
    g_cls = cls;
    g_clsId = id;
    return true;
}

bool IsBin(void* obj) {
    if (!obj || !g_cls) return false;
    void* cls = R::ClassOf(obj);
    void* bases[1] = {g_cls};
    return cls && R::IsDescendantOfAny(cls, bases, 1);
}

// The watch is on prop_openContainer_C, whose descendants include the compost bucket, a sibling of the garbage class. A
// class is walked once; the table goes with the world's generation (a Blueprint class dies with its world).
std::unordered_map<void*, bool> g_classIsBin;
uint32_t                        g_classGen = 0;

bool IsBinObject(void* obj) {
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    if (!cls) return false;
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen != g_classGen) { g_classIsBin.clear(); g_classGen = gen; }
    auto it = g_classIsBin.find(cls);
    if (it == g_classIsBin.end()) it = g_classIsBin.emplace(cls, IsBin(obj)).first;
    return it->second;
}

std::wstring KeyOf(void* a) {
    std::wstring key = a && R::IsLive(a) ? F::GetInteractableKeyString(a) : std::wstring();
    if (key.empty() || key == L"None" || key.size() >= kMaxKeyChars + 1) return {};
    return key;
}

// The container's own list, every live entry: the game puts no length check on the add, so the list is read whole.
std::vector<void*> ReadItems(void* bin) {
    std::vector<void*> out;
    const SR::Arr arr = SR::ReadArr(bin, g_offItems);
    for (int32_t i = 0; arr.data && i < arr.num; ++i) {
        void* a = reinterpret_cast<void* const*>(arr.data)[i];
        if (a && R::IsLive(a)) out.push_back(a);
    }
    return out;
}

// A client's copy never runs the add, and no load path writes the list (glueInside has no caller), so this write is the
// only one: it is load-bearing. The copy's own ReceiveDestroyed calls propAwoken, which walks itemsInside to free the
// props glued inside, and with an empty list a destroyed copy would leave them frozen. The array is the object's own: its
// buffer is reused when the capacity holds the list, else replaced and the old one freed.
void WriteItems(void* bin, const std::vector<void*>& items) {
    if (!SR::ReplaceArray(bin, g_offItems, items.data(), static_cast<int32_t>(items.size()), sizeof(void*)))
        UE_LOGW("garbage_bin: the list of container %p did not write -- its own destroy would free nothing", bin);
}

uint8_t ReadFlags(void* bin) {
    const uint8_t* pd = static_cast<const uint8_t*>(bin) + g_offPropData;
    return (pd[g_offCanHold] ? coop::net::kGarbageBinCanHold : 0) |
           (pd[g_offCanCollect] ? coop::net::kGarbageBinCanCollect : 0);
}

void WriteFlags(void* bin, uint8_t flags) {
    uint8_t* pd = static_cast<uint8_t*>(bin) + g_offPropData;
    pd[g_offCanHold] = (flags & coop::net::kGarbageBinCanHold) ? 1 : 0;
    pd[g_offCanCollect] = (flags & coop::net::kGarbageBinCanCollect) ? 1 : 0;
}

bool CallProp(void* prop, const wchar_t* fn, void (*fill)(ue_wrap::ParamFrame&)) {
    void* f = R::FindDispatchFunctionCached(R::ClassOf(prop), fn);
    ue_wrap::ParamFrame pf(f);
    if (!f || !pf.valid()) return false;
    if (fill) fill(pf);
    return ue_wrap::Call(prop, pf);
}

// The container's own glue (glueInside): the prop frozen by its setPropProps, then attached where it stands. The first few
// are said with what each call answered and the parent the prop reads back, so the rig shows a glue that did not hold.
void Glue(void* bin, void* item) {
    const bool frozen = ue_wrap::prop::CallSetPropProps(item, false, true, false, false);
    const bool attached = ue_wrap::engine::AttachActorToActorKeepWorld(item, bin, true);
    static int s_said = 0;
    if (s_said >= 5) return;
    ++s_said;
    void* parent = nullptr;
    ue_wrap::engine::GetAttachParentActor(item, parent);
    UE_LOGI("garbage_bin: glued %p into %p: setPropProps %s, attach %s, the prop's parent now %p", item, bin,
            frozen ? "ok" : "FAILED", attached ? "ok" : "FAILED", parent);
}

// The container's own release (propAwoken): the prop detached and woken by its awakeUnfreeze.
void Release(void* item) {
    if (!item || !R::IsLive(item)) return;
    ue_wrap::engine::DetachActorFromParent(item);
    CallProp(item, L"awakeUnfreeze", nullptr);
}

// ---- the index: every container of this world, by key, from the shared scan pass ----

// A container of the index is held by slot and serial: a slot a new object of another class takes reads as dead.
struct Ref {
    ue_wrap::CachedObjRef obj;
};
std::unordered_map<std::wstring, Ref> g_index;
std::vector<std::pair<std::wstring, Ref>> g_scanFound;
uint32_t g_indexGen = 0;
size_t g_lastCount = SIZE_MAX;

bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

void HubPassBegin(void*, bool) { g_scanFound.clear(); }

void HubMatch(void*, void* obj) {
    if (!R::IsLive(obj)) return;
    std::wstring key = KeyOf(obj);
    if (key.empty()) return;
    Ref ref;
    ref.obj.Set(obj);
    g_scanFound.emplace_back(std::move(key), ref);
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    if (isFull) g_index.clear();
    for (auto& f : g_scanFound) g_index[f.first] = f.second;
    if (!isFull) {
        for (auto it = g_index.begin(); it != g_index.end();)
            it = it->second.obj.Alive() ? std::next(it) : g_index.erase(it);
    }
    g_scanFound.clear();
    g_indexGen = worldGen;
    for (int slot = 1; slot < g_replayOwed.size(); ++slot)
        if (g_replayOwed[slot]) g_joinOwedAny = true;   // the index is current: a waiting joiner's replay runs at the next tick
    if (g_index.size() != g_lastCount) {
        g_lastCount = g_index.size();
        UE_LOGI("garbage_bin: index -- %zu container(s)", g_lastCount);
    }
    return g_index.size();
}

void* Resolve(const std::wstring& key) {
    if (IndexCurrent()) {
        auto it = g_index.find(key);
        // The slot's serial is the first test; the class is the second, since a bin is read and written as a bin by every caller.
        void* const hit = it != g_index.end() && it->second.obj.Alive() ? it->second.obj.Get() : nullptr;
        if (hit && IsBin(hit)) return hit;
    }
    void* b = coop::prop_key_index::Find(key);
    return IsBin(b) ? b : nullptr;
}

coop::net::Session* Live() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

// ---- the message ----

// A container's state as it travels: its flags and the keys of the props its list holds. One that does not fit a key's
// 31 chars never reaches here (KeyOf), so a key is always 1..31 chars.
struct State {
    uint8_t flags = 0;
    size_t count = 0;                  // every live entry of the list, keyed or not
    std::vector<std::wstring> keys;    // the keyed ones, in the list's order
    std::vector<void*> actors;         // the same, as pointers
};

constexpr size_t kHeadBytes = 2;   // version, key length

// Empty when the state cannot be said: a key past what a body carries, or a count past a u16.
std::vector<uint8_t> Pack(const std::wstring& binKey, const State& st) {
    if (binKey.empty() || binKey.size() > kMaxKeyChars || st.keys.size() > 0xFFFF) return {};
    std::vector<uint8_t> out;
    out.reserve(kHeadBytes + binKey.size() + 3 + st.keys.size() * (1 + kMaxKeyChars));
    out.push_back(coop::net::kGarbageBinVersion);
    out.push_back(static_cast<uint8_t>(binKey.size()));
    for (wchar_t c : binKey) out.push_back(static_cast<uint8_t>(c & 0x7F));
    out.push_back(st.flags);
    out.push_back(static_cast<uint8_t>(st.keys.size() & 0xFF));
    out.push_back(static_cast<uint8_t>((st.keys.size() >> 8) & 0xFF));
    for (const std::wstring& k : st.keys) {
        if (k.empty() || k.size() > kMaxKeyChars) return {};
        out.push_back(static_cast<uint8_t>(k.size()));
        for (wchar_t c : k) out.push_back(static_cast<uint8_t>(c & 0x7F));
    }
    return out;
}

struct Body {
    std::wstring binKey;
    uint8_t flags = 0;
    std::vector<std::wstring> keys;
};

// A key, length-prefixed, ASCII, 1..31 chars; false on a length the body cannot hold.
bool ReadKey(const std::vector<uint8_t>& b, size_t& o, std::wstring& out) {
    if (o >= b.size()) return false;
    const size_t n = b[o++];
    if (n == 0 || n > kMaxKeyChars || o + n > b.size()) return false;
    out.clear();
    for (size_t i = 0; i < n; ++i) {
        if (b[o + i] == 0 || b[o + i] > 0x7F) return false;
        out.push_back(static_cast<wchar_t>(b[o + i]));
    }
    o += n;
    return true;
}

// Strict: every length is read from the body and must add up to it exactly.
bool Unpack(const std::vector<uint8_t>& b, Body& out, const char** why) {
    out = Body{};
    size_t o = 0;
    if (b.empty() || b[o++] != coop::net::kGarbageBinVersion) { *why = "a version this build does not know"; return false; }
    if (!ReadKey(b, o, out.binKey)) { *why = "a container key the body cannot hold"; return false; }
    if (o + 3 > b.size()) { *why = "shorter than its head"; return false; }
    out.flags = b[o++];
    if (out.flags & ~(coop::net::kGarbageBinCanHold | coop::net::kGarbageBinCanCollect)) {
        *why = "a flag outside the two";
        return false;
    }
    const size_t count = static_cast<size_t>(b[o]) | (static_cast<size_t>(b[o + 1]) << 8);
    o += 2;
    if (count > (b.size() - o) / 2) { *why = "a count the body cannot hold"; return false; }   // a key is 2 bytes at least
    out.keys.reserve(count);
    std::unordered_set<std::wstring> seen;
    seen.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        std::wstring k;
        if (!ReadKey(b, o, k)) { *why = "a listed key the body cannot hold"; return false; }
        if (k == out.binKey) { *why = "a container that lists itself"; return false; }
        if (!seen.insert(k).second) { *why = "a prop listed twice"; return false; }
        out.keys.push_back(std::move(k));
    }
    if (o != b.size()) { *why = "a count that does not add up to the body"; return false; }
    return true;
}

BC::Assembler g_asm;
uint32_t      g_blobSeq = 1;
std::chrono::steady_clock::time_point g_nextSweep{};
constexpr std::chrono::seconds kSweepEvery{1};
constexpr std::chrono::seconds kAssemblyTtl{30};

// ---- HOST ----

// What the host last told its clients about one container, with the props it listed (by pointer: the destroy seam asks
// which container held a prop that died, and a pointer is the one thing it has at that moment).
struct Sent {
    uint8_t flags = 0;
    size_t count = 0;
    std::vector<std::wstring> keys;
    std::vector<void*> actors;
    bool valid = false;
};
std::unordered_map<std::wstring, Sent> g_sent;
std::unordered_map<void*, std::wstring> g_binOfItem;   // a listed prop -> its container's key
// The containers whose state may have changed since the last flush. A POST on checkPickup marks one, and so does the death
// of a prop it listed; the flush reads each once and sends it when it differs from what the clients were told.
std::unordered_set<std::wstring> g_dirty;
// A joiner's replay that the transport did not take, per slot: the containers still owed, asked again each tick.
coop::roster_ledger::PerSlotState<std::vector<std::wstring>> g_joinOwed;
uint32_t g_hostSends = 0;

// What the host holds about this world's containers; a world's change drops it. Whoever meets the new generation first (the
// watch's POST, a joiner's replay, the tick) clears it before storing anything of the new world, so nothing fresh is wiped.
void ForgetWorldState() {
    g_sent.clear();
    g_binOfItem.clear();
    g_dirty.clear();
    for (int i = 0; i < g_joinOwed.size(); ++i) {
        g_joinOwed[i].clear();
        g_replayOwed[i] = false;
    }
    g_joinOwedAny = false;
}

void SyncWorld() {
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world == g_world) return;
    g_world = world;
    ForgetWorldState();
}

State ReadState(void* bin) {
    State st;
    st.flags = ReadFlags(bin);
    const std::vector<void*> items = ReadItems(bin);
    st.count = items.size();
    for (void* a : items) {
        std::wstring k = KeyOf(a);
        if (k.empty()) continue;
        st.keys.push_back(std::move(k));
        st.actors.push_back(a);
    }
    return st;
}

// Sent: the transport took it. Refused: it did not, so the caller asks again. TooBig: the state is past what a blob
// carries, said here and never cut; the clients keep the last state they were told.
enum class Send { Sent, Refused, TooBig };

// `slot` < 0 is every client with a world; else that slot alone.
Send SendState(coop::net::Session& s, int slot, const std::wstring& key, const State& st) {
    const std::vector<uint8_t> blob = Pack(key, st);
    if (blob.empty() || blob.size() > BC::MaxBlobBytes()) {
        UE_LOGW("garbage_bin: '%ls' lists %zu prop(s), a state past what one message carries (%zu B, the ceiling is %zu) -- "
                "not sent, the clients keep the last state they were told", key.c_str(), st.keys.size(), blob.size(),
                BC::MaxBlobBytes());
        return Send::TooBig;
    }
    const bool ok = slot < 0 ? BC::SendBlob(&s, coop::net::ReliableKind::GarbageBinState, g_blobSeq++, blob)
                             : BC::SendBlobToSlot(&s, slot, coop::net::ReliableKind::GarbageBinState, g_blobSeq++, blob);
    return ok ? Send::Sent : Send::Refused;
}

void MarkDirty(const std::wstring& binKey) { g_dirty.insert(binKey); }

// The reverse map follows what a send listed: the props of the state before it go, the props of this one come.
void Remember(const std::wstring& binKey, State&& st) {
    Sent& sent = g_sent[binKey];
    for (void* a : sent.actors) g_binOfItem.erase(a);
    for (void* a : st.actors) g_binOfItem[a] = binKey;
    sent.flags = st.flags;
    sent.count = st.count;
    sent.keys = std::move(st.keys);
    sent.actors = std::move(st.actors);
    sent.valid = true;
}

void Forget(const std::wstring& binKey) {
    auto it = g_sent.find(binKey);
    if (it == g_sent.end()) return;
    for (void* a : it->second.actors) g_binOfItem.erase(a);
    g_sent.erase(it);
}

// The per-tick flush: each marked container is read once and sent when it differs from what the clients were told. A send
// the transport refuses leaves the container marked for the next tick. With no client that has a world there is no one to
// tell, and a joiner is told every container at its replay.
void Flush(coop::net::Session& s) {
    if (g_dirty.empty()) return;
    const bool peers = s.AnyWorldReadyPeer();
    for (auto it = g_dirty.begin(); it != g_dirty.end();) {
        void* bin = Resolve(*it);
        if (!bin) {
            Forget(*it);
            it = g_dirty.erase(it);
            continue;
        }
        State st = ReadState(bin);
        auto known = g_sent.find(*it);
        const bool same = known != g_sent.end() && known->second.valid && known->second.flags == st.flags &&
                          known->second.count == st.count && known->second.keys == st.keys;
        if (!same && peers) {
            const Send sent = SendState(s, -1, *it, st);
            if (sent == Send::Refused) {
                ++it;
                continue;
            }
            if (sent == Send::Sent) {
                ++g_hostSends;
                if (known == g_sent.end() || !known->second.valid || known->second.count != st.count ||
                    known->second.flags != st.flags)
                    UE_LOGI("garbage_bin: '%ls' holds %zu prop(s) (%zu by key), canHold=%d canCollect=%d", it->c_str(),
                            st.count, st.keys.size(), (st.flags & coop::net::kGarbageBinCanHold) ? 1 : 0,
                            (st.flags & coop::net::kGarbageBinCanCollect) ? 1 : 0);
            }
        }
        Remember(*it, std::move(st));
        it = g_dirty.erase(it);
    }
}

// HOST: every container's state to one joiner, at its replay. The set is the scan index's and the containers the clients
// were already told of: a bin born since the last scan pass and changed since is in the second, and one that was neither
// changed nor seen by the scan never ran its own checkPickup, so its copy on the joiner already shows what it would be
// told. Each is resolved as a flush resolves one. What the transport does not take is asked again each tick.
void ReplayTo(coop::net::Session& s, int slot) {
    std::unordered_set<std::wstring> keys;
    for (const auto& entry : g_index) keys.insert(entry.first);
    for (const auto& entry : g_sent) keys.insert(entry.first);
    size_t sent = 0;
    g_joinOwed[slot].clear();
    for (const std::wstring& key : keys) {
        void* bin = Resolve(key);
        if (!bin) continue;
        switch (SendState(s, slot, key, ReadState(bin))) {
        case Send::Sent:    ++sent; break;
        case Send::TooBig:  break;
        case Send::Refused: g_joinOwed[slot].push_back(key); g_joinOwedAny = true; break;
        }
    }
    UE_LOGI("garbage_bin: slot %d world-ready -- %zu container(s) sent to it, %zu to be asked again", slot, sent,
            g_joinOwed[slot].size());
}

// A joiner's replay waits for the scan index to be current for this world (the index's own pass-complete event re-arms
// this, HubPassComplete), then runs; the slot's containers that a replay could not send are asked again each tick until
// the transport takes them.
void RetryJoin(coop::net::Session& s) {
    if (!g_joinOwedAny) return;
    g_joinOwedAny = false;
    for (int slot = 1; slot < g_joinOwed.size(); ++slot) {
        if (g_replayOwed[slot]) {
            if (!IndexCurrent()) continue;
            g_replayOwed[slot] = false;
            ReplayTo(s, slot);
        }
        std::vector<std::wstring>& owed = g_joinOwed[slot];
        if (owed.empty()) continue;
        std::vector<std::wstring> still;
        for (const std::wstring& key : owed) {
            void* bin = Resolve(key);
            if (!bin) continue;
            if (SendState(s, slot, key, ReadState(bin)) == Send::Refused) still.push_back(key);
        }
        owed.swap(still);
        if (!owed.empty()) g_joinOwedAny = true;
    }
}

void OnCheckPickupPost(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || !IsBinObject(c.object)) return;
    SyncWorld();
    const std::wstring key = KeyOf(c.object);
    if (!key.empty()) MarkDirty(key);
}

void SettleWatch() {
    if (g_watchSettled) return;
    if (!g_watchRegistered) {
        g_watchRegistered = sg::WatchName(kRowCheckPickup, kTagCheckPickup, nullptr, &OnCheckPickupPost);
        if (!g_watchRegistered) {
            if (++g_watchTries < kMaxWatchTries) return;
            g_watchSettled = true;
            UE_LOGE("garbage_bin: the script-body gate took no watch on %ls::%ls in %d tries -- the host's containers are not "
                    "announced for the rest of this process", kRowCheckPickup.Class(), kRowCheckPickup.Name(), kMaxWatchTries);
            return;
        }
    }
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowCheckPickup, kTagCheckPickup)) return;
    g_watchSettled = true;
    if (sg::NameWatchLive(kRowCheckPickup, kTagCheckPickup))
        UE_LOGI("garbage_bin: the containers' checkPickup is watched");
    else
        UE_LOGE("garbage_bin: the watch on %ls settled dead -- the host's containers are not announced for the rest of "
                "this process", kRowCheckPickup.Name());
}

// ---- CLIENT ----

// The host's state of each container, kept until it is applied whole: a listed prop this copy does not hold yet (the
// joiner's snapshot births props after the replay) keeps the state here, and it is tried again when the key index's table
// moves, which a birth does. No clock ends the wait. A newer state of the container replaces it; the world's change, the
// container's own destroy and the session's end drop it. Diverges from MTA, whose add packet carries an element's state so
// no packet names an element the client lacks (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CEntityAddPacket.cpp
// :178-181): here the joiner's snapshot births the props after the replay, so the state waits for them.
struct Wanted {
    uint8_t flags = 0;
    std::vector<std::wstring> keys;
    bool waitSaid = false;
};
std::unordered_map<std::wstring, Wanted> g_wanted;
// One walk applies at most this many containers: each costs two engine calls per prop glued or released, and a joiner's
// world can hold dozens of containers of dozens of props. The rest wait in the map and the next tick walks again.
constexpr int      kBinsPerTick = 4;
constexpr uint64_t kWalkNext = ~0ull;   // a value no key index has: the next tick walks
uint64_t           g_walked = 0;
uint32_t           g_wantedGen = 0;
uint32_t           g_clientApplies = 0, g_received = 0, g_dropped = 0, g_resolved = 0;

// A message this peer does not act on, at the first twenty and each fiftieth after: no honest host sends one.
void DropLogged(const char* why, int slot) {
    ++g_dropped;
    if (g_dropped > 20 && g_dropped % 50 != 0) return;
    UE_LOGW("garbage_bin: a message from slot %d dropped (%s); %u so far", slot, why, g_dropped);
}

// One whole state: whole, or not at all. False while the container or a listed prop is not here.
bool TryApply(const std::wstring& binKey, Wanted& w) {
    void* bin = Resolve(binKey);
    std::vector<void*> want;
    want.reserve(w.keys.size());
    const std::wstring* missing = bin ? nullptr : &binKey;
    for (const std::wstring& k : w.keys) {
        if (!bin) break;
        void* a = coop::prop_key_index::Find(k);
        if (a && R::IsLive(a)) want.push_back(a);
        else if (!missing) missing = &k;
    }
    if (missing) {
        if (!w.waitSaid) {
            w.waitSaid = true;
            UE_LOGI("garbage_bin: the host's state of '%ls' waits for '%ls', not here yet (%zu key(s) listed)",
                    binKey.c_str(), missing->c_str(), w.keys.size());
        }
        return false;
    }
    // The container gaps drill's red: the state is resolved, every listed prop here, and not applied.
    static const bool s_off = coop::config::ResolveFlag(coop::config_registry::rows::garbage_state_off);
    if (s_off) {
        ++g_resolved;
        UE_LOGI("garbage_bin: the host's state of '%ls' is resolved and not applied (garbage_state_off)", binKey.c_str());
        return true;
    }
    WriteFlags(bin, w.flags);
    // Against what the copy's own list holds now, not a ledger of ours: let go of what it holds that the host does not
    // list, glue what the host lists that it lacks, then make the list the host's.
    const std::vector<void*> actual = ReadItems(bin);
    const std::unordered_set<void*> wantSet(want.begin(), want.end());
    const std::unordered_set<void*> actualSet(actual.begin(), actual.end());
    for (void* a : actual)
        if (!wantSet.count(a)) Release(a);
    for (void* a : want)
        if (!actualSet.count(a)) Glue(bin, a);
    WriteItems(bin, want);
    ++g_clientApplies;
    ++g_resolved;
    UE_LOGI("garbage_bin: '%ls' shows the host's %zu prop(s), canHold=%d canCollect=%d", binKey.c_str(), want.size(),
            (w.flags & coop::net::kGarbageBinCanHold) ? 1 : 0, (w.flags & coop::net::kGarbageBinCanCollect) ? 1 : 0);
    return true;
}

void ClientApply() {
    if (g_wanted.empty() || !coop::net_pump::HasAnnouncedWorldReady()) return;
    if (ue_wrap::world_identity::Generation() != g_wantedGen) {
        UE_LOGI("garbage_bin: the world changed -- %zu waiting state(s) dropped with it", g_wanted.size());
        g_wanted.clear();
        return;
    }
    // Nothing but the key index's own table moving can change an answer, so the walk runs when its Version has.
    const uint64_t version = coop::prop_key_index::Version();
    if (version == g_walked) return;
    g_walked = version;
    int applied = 0;
    for (auto it = g_wanted.begin(); it != g_wanted.end();) {
        if (applied >= kBinsPerTick) {
            g_walked = kWalkNext;
            break;
        }
        if (TryApply(it->first, it->second)) {
            it = g_wanted.erase(it);
            ++applied;
        } else {
            ++it;
        }
    }
}

// One whole message on a client: the host's state of a container.
void OnBody(const std::vector<uint8_t>& blob, int senderPeerSlot) {
    Body b;
    const char* why = "";
    if (!Unpack(blob, b, &why)) { DropLogged(why, senderPeerSlot); return; }
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen != g_wantedGen) { g_wanted.clear(); g_wantedGen = gen; }
    ++g_received;
    Wanted& w = g_wanted[b.binKey];
    w = Wanted{b.flags, std::move(b.keys), false};
    g_walked = kWalkNext;
}

void SweepAssemblies() {
    if (g_asm.Idle()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextSweep) return;
    g_nextSweep = now + kSweepEvery;
    g_asm.Sweep(now, kAssemblyTtl);
}

// The session's end: the host's table of this world, and the client's waiting states.
void ForgetAll() {
    ForgetWorldState();
    g_wanted.clear();
    g_walked = 0;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_hubRegistered) {
        g_hubRegistered = true;
        coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
            "garbage_bin", nullptr, &EnsureResolved, &IsBin, &HubPassBegin, &HubMatch, &HubPassComplete,
            // Demand-exempt (rides the tail passes and the backstop): a world may hold no container at all, and a count of
            // zero never settles, which would keep the whole hub in full passes.
            /*settleScans*/ 0});
    }
}

void Tick() {
    SweepAssemblies();
    if (!EnsureResolved()) return;
    SettleWatch();
    coop::net::Session* s = Live();
    if (!s) return;
    SyncWorld();
    if (s->role() == coop::net::Role::Host) {
        Flush(*s);
        RetryJoin(*s);
    } else {
        ClientApply();
    }
}

void NoteDestroyed(void* actor) {
    // CLIENT: a container that died is no state to wait for (the host's state of it may arrive after its destroy).
    if (!g_wanted.empty() && IsBinObject(actor)) {
        const std::wstring key = KeyOf(actor);
        if (!key.empty()) g_wanted.erase(key);
    }
    if (g_binOfItem.empty()) return;
    auto it = g_binOfItem.find(actor);
    if (it == g_binOfItem.end()) return;
    MarkDirty(it->second);
    g_binOfItem.erase(it);
}

void OnChunk(coop::net::Session& session, const void* payload, size_t payloadLen, int senderPeerSlot) {
    UE_ASSERT_GAME_THREAD("garbage_bin_sync::OnChunk");
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s != &session || !s->connected()) return;
    // The host's word reaches a client from the host alone.
    if (s->role() == coop::net::Role::Host || senderPeerSlot != 0) {
        DropLogged("a state from the wrong side of the link", senderPeerSlot);
        return;
    }
    if (!payload || payloadLen < sizeof(coop::net::BlobChunkPayload)) {
        DropLogged("a chunk shorter than a chunk", senderPeerSlot);
        return;
    }
    coop::net::BlobChunkPayload chunk{};
    std::memcpy(&chunk, payload, sizeof(chunk));
    std::vector<uint8_t> blob;
    // The assembler refuses these quietly (it returns false for a part way too): counted here, as every other refusal.
    if (chunk.chunks == 0 || chunk.chunkIdx >= chunk.chunks || chunk.chunkLen > sizeof(chunk.data)) {
        DropLogged("a chunk whose part fields do not add up", senderPeerSlot);
        return;
    }
    if (!g_asm.OnChunk(chunk, static_cast<uint8_t>(senderPeerSlot), blob)) return;
    OnBody(blob, senderPeerSlot);
}

void QueueConnectBroadcastForSlot(int slot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || slot < 1 || slot >= g_joinOwed.size()) return;
    // Never a silent nothing: the replay is owed, and runs at the next tick once the scan index is current for this world
    // (at once when it is already).
    SyncWorld();
    g_replayOwed[slot] = true;
    g_joinOwedAny = true;
}

uint32_t ResolvedCount() { return g_resolved; }

uint32_t AppliedCount() { return g_clientApplies; }

void OnPeerGone(uint8_t slot) {
    g_asm.ClearSlot(slot);
    if (slot < g_joinOwed.size()) {
        g_joinOwed[slot].clear();
        g_replayOwed[slot] = false;
    }
}

void OnDisconnect() {
    if (g_hostSends || g_clientApplies || g_received || g_dropped)
        UE_LOGI("garbage_bin: session end -- host sends=%u, client received=%u applied=%u dropped=%u", g_hostSends,
                g_received, g_clientApplies, g_dropped);
    g_hostSends = g_clientApplies = g_received = g_dropped = g_resolved = 0;
    ForgetAll();
    g_asm.Clear();
    g_blobSeq = 1;
    g_world = 0;
    g_wantedGen = 0;
    g_session.store(nullptr);
}

}  // namespace coop::garbage_bin_sync
