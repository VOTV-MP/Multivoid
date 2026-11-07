// coop/interactables/garbage_bin_sync.cpp -- see coop/interactables/garbage_bin_sync.h.

#include "coop/interactables/garbage_bin_sync.h"

#include "coop/element/object_scan_hub.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "coop/session/poll_timer.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::garbage_bin_sync {
namespace {

namespace F  = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
using Clock = std::chrono::steady_clock;
using Payload = coop::net::GarbageBinPayload;

constexpr auto kStateLife = std::chrono::seconds(10);   // a client's state whose props never resolve is let go
constexpr size_t kMaxItems = 64;                         // an itemsInside past this is not read

std::atomic<coop::net::Session*> g_session{nullptr};
coop::session::PollTimer g_poll{std::chrono::milliseconds(250)};
uint32_t g_world = 0;
bool g_hubRegistered = false;

// ---- the class ----

void* g_cls = nullptr;
int32_t g_offItems = -1, g_offPropData = -1, g_offCanHold = -1, g_offCanCollect = -1;

bool EnsureResolved() {
    if (g_cls) return true;
    void* cls = R::FindClass(L"prop_garbageContainer_C");
    if (!cls) return false;
    g_offItems = R::FindPropertyOffset(cls, L"itemsInside");
    g_offPropData = R::FindPropertyOffset(cls, L"propData");
    void* st = R::PropertyInnerStruct(cls, L"propData");
    g_offCanHold = st ? R::FindPropertyOffsetByPrefix(st, L"canHold_") : -1;
    g_offCanCollect = st ? R::FindPropertyOffsetByPrefix(st, L"canCollect_") : -1;
    if (g_offItems < 0 || g_offPropData < 0 || g_offCanHold < 0 || g_offCanCollect < 0) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGE("garbage_bin: prop_garbageContainer_C's itemsInside or propData's canHold/canCollect did not "
                    "resolve (%d, %d, %d, %d) -- not carried", g_offItems, g_offPropData, g_offCanHold, g_offCanCollect);
        }
        return false;
    }
    g_cls = cls;
    return true;
}

bool IsBin(void* obj) {
    if (!obj || !g_cls) return false;
    void* cls = R::ClassOf(obj);
    void* bases[1] = {g_cls};
    return cls && R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring KeyOf(void* a) {
    std::wstring key = a && R::IsLive(a) ? F::GetInteractableKeyString(a) : std::wstring();
    if (key.empty() || key == L"None" || key.size() >= sizeof(coop::net::WireKey::data)) return {};
    return key;
}

std::vector<void*> ReadItems(void* bin) {
    std::vector<void*> out;
    const SR::Arr arr = SR::ReadArr(bin, g_offItems);
    for (int32_t i = 0; arr.data && i < arr.num && i < static_cast<int32_t>(kMaxItems); ++i) {
        void* a = reinterpret_cast<void* const*>(arr.data)[i];
        if (a && R::IsLive(a)) out.push_back(a);
    }
    return out;
}

void WriteItems(void* bin, const std::vector<void*>& items) {
    void* buf = items.empty() ? nullptr : SR::AllocZeroed(items.size(), sizeof(void*));
    if (!items.empty() && !buf) return;
    if (buf) std::memcpy(buf, items.data(), items.size() * sizeof(void*));
    SR::WriteArrHeader(bin, g_offItems, buf, static_cast<int32_t>(items.size()));
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

// The container's own glue (glueInside): the prop frozen by its setPropProps, then attached where it stands.
void Glue(void* bin, void* item) {
    CallProp(item, L"setPropProps", [](ue_wrap::ParamFrame& f) {
        f.Set<bool>(L"static", false);
        f.Set<bool>(L"frozen", true);
        f.Set<bool>(L"active", false);
        f.Set<bool>(L"sleeping", false);
    });
    ue_wrap::engine::AttachActorToActorKeepWorld(item, bin, true);
}

// The container's own release (propAwoken): the prop detached and woken by its awakeUnfreeze.
void Release(void* item) {
    if (!item || !R::IsLive(item)) return;
    ue_wrap::engine::DetachActorFromParent(item);
    CallProp(item, L"awakeUnfreeze", nullptr);
}

// ---- the index: every container of this world, by key, from the shared scan pass ----

struct Ref {
    void* actor;
    int32_t idx;
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
    if (!key.empty()) g_scanFound.emplace_back(std::move(key), Ref{obj, R::InternalIndexOf(obj)});
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    if (isFull) g_index.clear();
    for (auto& f : g_scanFound) g_index[f.first] = f.second;
    if (!isFull) {
        for (auto it = g_index.begin(); it != g_index.end();)
            it = R::IsLiveByIndex(it->second.actor, it->second.idx) ? std::next(it) : g_index.erase(it);
    }
    g_scanFound.clear();
    g_indexGen = worldGen;
    if (g_index.size() != g_lastCount) {
        g_lastCount = g_index.size();
        UE_LOGI("garbage_bin: index -- %zu container(s)", g_lastCount);
    }
    return g_index.size();
}

void* Resolve(const std::wstring& key) {
    if (IndexCurrent()) {
        auto it = g_index.find(key);
        if (it != g_index.end() && R::IsLiveByIndex(it->second.actor, it->second.idx)) return it->second.actor;
    }
    void* b = coop::prop_key_index::Find(key);
    return IsBin(b) ? b : nullptr;
}

coop::net::Session* Live() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

// ---- HOST ----

struct Sent {
    uint8_t flags = 0;
    uint8_t count = 0;
    std::vector<std::wstring> keys;
    bool valid = false, pending = false;
};
std::unordered_map<std::wstring, Sent> g_sent;
uint32_t g_hostSends = 0;

void HostPoll(coop::net::Session& s) {
    if (!IndexCurrent()) return;
    for (const auto& [key, ref] : g_index) {
        if (!R::IsLiveByIndex(ref.actor, ref.idx)) continue;
        const uint8_t flags = ReadFlags(ref.actor);
        const std::vector<void*> items = ReadItems(ref.actor);
        std::vector<std::wstring> keys;
        for (void* a : items) {
            std::wstring k = KeyOf(a);
            if (!k.empty()) keys.push_back(std::move(k));
        }
        const uint8_t count = static_cast<uint8_t>(std::min<size_t>(items.size(), 255));
        Sent& sent = g_sent[key];
        if (sent.valid && !sent.pending && sent.flags == flags && sent.count == count && sent.keys == keys) continue;
        Payload p{};
        coop::net::WireKeyFromString(key, p.key);
        p.flags = flags;
        p.count = count;
        p.keys = static_cast<uint8_t>(std::min<size_t>(keys.size(), coop::net::kGarbageBinKeys));
        for (uint8_t i = 0; i < p.keys; ++i) coop::net::WireKeyFromString(keys[i], p.items[i]);
        if (!s.SendReliable(coop::net::ReliableKind::GarbageBinState, &p, sizeof(p))) continue;
        ++g_hostSends;
        if (!sent.valid || sent.count != count || sent.flags != flags)
            UE_LOGI("garbage_bin: '%ls' holds %u prop(s) (%u by key), canHold=%d canCollect=%d", key.c_str(), count,
                    p.keys, (flags & coop::net::kGarbageBinCanHold) ? 1 : 0,
                    (flags & coop::net::kGarbageBinCanCollect) ? 1 : 0);
        sent.flags = flags;
        sent.count = count;
        sent.keys = std::move(keys);
        sent.valid = true;
        sent.pending = false;
    }
    for (auto it = g_sent.begin(); it != g_sent.end();) it = g_index.count(it->first) ? std::next(it) : g_sent.erase(it);
}

// ---- CLIENT ----

struct Shown {
    Payload p{};
    Clock::time_point at{};
    bool done = false;
};
std::unordered_map<std::wstring, Shown> g_shown;
struct Glued {
    void* actor;
    int32_t idx;
};
std::unordered_map<std::wstring, std::vector<Glued>> g_glued;   // each container's props this copy glued
uint32_t g_clientApplies = 0;

void ClientApply(Clock::time_point now) {
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;
    for (auto it = g_shown.begin(); it != g_shown.end();) {
        Shown& sh = it->second;
        if (sh.done) {
            ++it;
            continue;
        }
        void* bin = Resolve(it->first);
        if (!bin) {
            it = now - sh.at > kStateLife ? g_shown.erase(it) : std::next(it);
            continue;
        }
        std::vector<void*> want;
        bool missing = false;
        for (uint8_t i = 0; i < sh.p.keys && i < coop::net::kGarbageBinKeys; ++i) {
            void* a = coop::prop_key_index::Find(coop::net::StringFromWireKey(sh.p.items[i]));
            if (a && R::IsLive(a)) want.push_back(a);
            else missing = true;
        }
        WriteFlags(bin, sh.p.flags);
        std::vector<Glued>& glued = g_glued[it->first];
        // Let go of what the host's copy no longer holds, then glue what it holds that this copy has not glued.
        for (auto g = glued.begin(); g != glued.end();) {
            const bool alive = R::IsLiveByIndex(g->actor, g->idx);
            if (alive && std::find(want.begin(), want.end(), g->actor) != want.end()) {
                ++g;
                continue;
            }
            if (alive) Release(g->actor);
            g = glued.erase(g);
        }
        for (void* a : want) {
            const bool have = std::any_of(glued.begin(), glued.end(), [a](const Glued& g) { return g.actor == a; });
            if (have) continue;
            Glue(bin, a);
            glued.push_back({a, R::InternalIndexOf(a)});
        }
        WriteItems(bin, want);
        ++g_clientApplies;
        UE_LOGI("garbage_bin: '%ls' shows the host's %zu prop(s), canHold=%d canCollect=%d%s", it->first.c_str(),
                want.size(), (sh.p.flags & coop::net::kGarbageBinCanHold) ? 1 : 0,
                (sh.p.flags & coop::net::kGarbageBinCanCollect) ? 1 : 0, missing ? " (some props not here yet)" : "");
        // Applied whole once every listed prop is here; one still missing is tried again until the state's life ends.
        if (!missing || now - sh.at > kStateLife) sh.done = true;
        ++it;
    }
}

void Forget() {
    g_sent.clear();
    g_shown.clear();
    g_glued.clear();
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_hubRegistered) {
        g_hubRegistered = true;
        coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
            "garbage_bin", nullptr, &EnsureResolved, &IsBin, &HubPassBegin, &HubMatch, &HubPassComplete,
            /*settleScans*/ 15});
    }
}

void Tick() {
    coop::net::Session* s = Live();
    if (!s || !EnsureResolved()) return;
    const Clock::time_point now = Clock::now();
    if (!g_poll.Due(now)) return;
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_world) {
        g_world = world;
        Forget();
    }
    if (s->role() == coop::net::Role::Host) HostPoll(*s);
    else ClientApply(now);
}

void OnState(const coop::net::GarbageBinPayload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (key.empty()) return;
    Shown& sh = g_shown[key];
    sh.p = p;
    sh.at = Clock::now();
    sh.done = false;
}

void QueueConnectBroadcastForSlot(int /*slot*/) {
    for (auto& entry : g_sent) entry.second.pending = true;
}

void OnDisconnect() {
    if (g_hostSends || g_clientApplies)
        UE_LOGI("garbage_bin: session end -- host sends=%u, client applies=%u", g_hostSends, g_clientApplies);
    g_hostSends = g_clientApplies = 0;
    Forget();
    g_session.store(nullptr);
}

}  // namespace coop::garbage_bin_sync
