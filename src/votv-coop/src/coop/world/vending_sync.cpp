// coop/world/vending_sync.cpp -- see coop/world/vending_sync.h.
//
// The cooked graphs this rests on (0.10.0):
//   prop_vending_C.actionOptionIndex  action 4: with the aimed button `lookatbuttons` and the aimed item `lookatitem` (an FName of
//                    `retarget_items`), amount > 0 and balance >= price: amounts[i]--, balance -= price, then after 1 s mints the
//                    item (a prop_food_C or a prop_C named `sellitem`) at the `spawn` component and flings it; action 7: gives the
//                    change back as coins (spawnCoin) and clears `balance`.
//   prop_vending_C.playerUsedOn       a coin in hand: balance += its value and K2_DestroyActor on the held prop.
//   prop_vending_C.checkStock / timerCheckStock / beginRestock  when every amount is 0, beginRestock waits (a 0.5 s loop) until a
//                    trace from the figure's audio component to player 0's camera is blocked, then restores the three arrays from
//                    their `_default`s, sets `isEmpty` and ticks.

#include "coop/world/vending_sync.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/remote_prop.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::vending_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;
using Payload = coop::net::AmbientFixtureStatePayload;

UE_FACT_FN(kRowUse, "vending_sync", L"prop_vending_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowCoin, "vending_sync", L"prop_vending_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowBeginRestock, "vending_sync", L"prop_vending_C", L"beginRestock", Declared);
UE_FACT_FN(kRowCheckStock, "vending_sync", L"prop_vending_C", L"checkStock", Declared);
UE_FACT_FN(kRowTimerCheckStock, "vending_sync", L"prop_vending_C", L"timerCheckStock", Declared);
constexpr int kTagUse = 0x56444E31, kTagCoin = 0x56444E32, kTagRestock = 0x56444E33;
constexpr long long kPollMs = 1000;
constexpr uint8_t kNoSlot = 0xFF;
constexpr uint8_t kActionBuy = 4, kActionChange = 7;
constexpr int kMaxSlots = 32;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchAsked = false;
Clock::time_point g_lastPoll{};

struct TArr { void* data; int32_t num; int32_t max; };

const TArr* ArrOf(void* actor, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), name);
    return off < 0 ? nullptr : reinterpret_cast<const TArr*>(static_cast<const uint8_t*>(actor) + off);
}

std::vector<int32_t> IntsOf(void* actor, const wchar_t* name) {
    std::vector<int32_t> v;
    const TArr* a = ArrOf(actor, name);
    for (int i = 0; a && a->data && i < a->num && i < kMaxSlots; ++i) v.push_back(static_cast<const int32_t*>(a->data)[i]);
    return v;
}

std::vector<std::string> NamesOf(void* actor, const wchar_t* name) {
    std::vector<std::string> v;
    const TArr* a = ArrOf(actor, name);
    for (int i = 0; a && a->data && i < a->num && i < kMaxSlots; ++i) {
        const std::wstring w = R::ToString(static_cast<const R::FName*>(a->data)[i]);
        std::string s;
        for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
        v.push_back(s);
    }
    return v;
}

bool BoolOf(void* actor, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return R::FindBoolProperty(R::ClassOf(actor), name, off, mask) && off >= 0 && (*(static_cast<const uint8_t*>(actor) + off) & mask) != 0;
}

void SetBoolOf(void* actor, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(actor), name, off, mask) || off < 0) return;
    uint8_t* p = static_cast<uint8_t*>(actor) + off;
    *p = v ? static_cast<uint8_t>(*p | mask) : static_cast<uint8_t>(*p & ~mask);
}

int32_t IntOf(void* actor, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), name);
    return off < 0 ? 0 : *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(actor) + off);
}

std::wstring KeyOf(void* machine) {
    const std::wstring k = ue_wrap::prop::GetInteractableKeyString(machine);
    return (k.empty() || k == L"None") ? std::wstring() : k;
}

uint32_t Hash(const std::wstring& k) {
    uint32_t h = 2166136261u;
    for (wchar_t c : k) { h ^= static_cast<uint32_t>(c); h *= 16777619u; }
    return h ? h : 1u;
}

void LiveMachines(std::vector<void*>& out) {
    out.clear();
    void* cls = ue_wrap::object_index::ClassByName(L"prop_vending_C");
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!cls || !world) return;
    std::vector<void*> found;
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (obj) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &found);
    for (void* obj : found)
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__") && ue_wrap::world_identity::WorldOf(obj) == world &&
            !KeyOf(obj).empty())
            out.push_back(obj);
}

void* MachineByKey(const std::wstring& key) {
    std::vector<void*> live;
    LiveMachines(live);
    for (void* m : live)
        if (KeyOf(m) == key) return m;
    return nullptr;
}

void* MachineByHash(uint32_t h) {
    std::vector<void*> live;
    LiveMachines(live);
    for (void* m : live)
        if (Hash(KeyOf(m)) == h) return m;
    return nullptr;
}

void Refresh(void* machine) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(machine), L"gennameboxes");
    ue_wrap::ParamFrame f(fn);
    if (fn && f.valid()) ue_wrap::Call(machine, f);
}

// ---- HOST ----

struct HostState {
    void*                  actor;
    std::vector<int32_t>   amounts, prices;
    std::vector<std::string> items;
    int32_t                balance;
    bool                   empty;
    bool                   said;
};
std::unordered_map<uint32_t, HostState> g_host;   // by key hash
bool g_resendAll = false;

bool SendMachine(coop::net::Session* s, uint32_t hash, const HostState& h) {
    const int n = static_cast<int>(h.amounts.size());
    for (int i = 0; i < n; ++i) {
        Payload p{};
        p.id = hash;
        p.x = static_cast<float>(h.amounts[i]);
        p.y = i < static_cast<int>(h.prices.size()) ? static_cast<float>(h.prices[i]) : 0.f;
        p.z = static_cast<float>(h.balance);
        p.pitch = static_cast<float>(n);
        p.yaw = h.empty ? 1.f : 0.f;
        p.family = coop::net::ambient_fixture::kVendStock;
        p.alive = 1;
        p._pad[0] = static_cast<uint8_t>(i);
        const std::string& nm = i < static_cast<int>(h.items.size()) ? h.items[i] : std::string();
        const size_t len = nm.size() < sizeof(p.birthKey.data) ? nm.size() : sizeof(p.birthKey.data);
        p.birthKey.len = static_cast<uint8_t>(len);
        std::memcpy(p.birthKey.data, nm.data(), len);
        if (!s->SendReliable(coop::net::ReliableKind::AmbientFixtureState, &p, sizeof(p))) return false;
    }
    return true;
}

void HostPoll(coop::net::Session* s) {
    std::vector<void*> live;
    LiveMachines(live);
    const bool connected = s->connected();
    for (void* m : live) {
        const uint32_t hash = Hash(KeyOf(m));
        HostState now{m, IntsOf(m, L"retarget_amounts"), IntsOf(m, L"retarget_prices"), NamesOf(m, L"retarget_items"), IntOf(m, L"balance"),
                      BoolOf(m, L"isempty"), false};
        auto it = g_host.find(hash);
        const bool fresh = it == g_host.end() || it->second.actor != m;
        const bool changed = fresh || it->second.amounts != now.amounts || it->second.prices != now.prices ||
                             it->second.balance != now.balance || it->second.empty != now.empty || it->second.items != now.items;
        if (fresh) {
            UE_LOGI("vending: the host's machine '%ls' stands with %zu slot(s)", KeyOf(m).c_str(), now.amounts.size());
            it = g_host.insert_or_assign(hash, now).first;
        } else if (changed) {
            UE_LOGI("vending: the host's machine '%ls' changed: balance %d, empty %d, amounts[0] %d", KeyOf(m).c_str(), now.balance, now.empty ? 1 : 0,
                    now.amounts.empty() ? -1 : now.amounts[0]);
            now.said = it->second.said;
            it->second = now;
        }
        if (!connected) { it->second.said = false; continue; }
        if (changed || !it->second.said || g_resendAll)
            if (SendMachine(s, hash, it->second)) it->second.said = true;
    }
    if (connected) g_resendAll = false;
}

// ---- CLIENT ----

struct Stage {
    int                     n = 0;
    std::vector<int32_t>    amounts, prices;
    std::vector<std::string> items;
    int32_t                 balance = 0;
    bool                    empty = false;
    unsigned                got = 0;       // bit per slot received since the last complete round
    bool                    pending = false;   // a complete round not yet written into the copy
    bool                    applied = false;
    bool                    warned = false;
    bool                    wasAllZero = false;
};
std::unordered_map<uint32_t, Stage> g_stage;

void PlayFigure(void* machine) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(machine), L"audio_figura");
    void* comp = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(machine) + off) : nullptr;
    void* fn = comp && R::IsLive(comp) ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"Activate") : nullptr;
    ue_wrap::ParamFrame f(fn);
    const bool ran = fn && f.valid() && f.Set<bool>(L"bReset", true) && ue_wrap::Call(comp, f);
    UE_LOGI("vending: the host's refill reached the copy -- the machine's figure sound plays here: %d", ran ? 1 : 0);
}

// Writes a complete staged round into the copy. False while the copy is not there.
bool Apply(uint32_t hash, Stage& st) {
    void* m = MachineByHash(hash);
    if (!m) return false;
    const TArr* am = ArrOf(m, L"retarget_amounts");
    const TArr* pr = ArrOf(m, L"retarget_prices");
    if (!am || !pr || !am->data || !pr->data || am->num != st.n || pr->num != st.n) {
        if (!st.warned) UE_LOGW("vending: the host's machine has %d slot(s), the copy %d -- not written", st.n, am ? am->num : -1);
        st.warned = true;
        return true;
    }
    const std::vector<std::string> mine = NamesOf(m, L"retarget_items");
    if (mine != st.items && !st.warned) {
        UE_LOGW("vending: the copy's item names differ from the host's -- the amounts are written by slot all the same");
        st.warned = true;
    }
    const bool wasEmpty = BoolOf(m, L"isempty");
    for (int i = 0; i < st.n; ++i) {
        static_cast<int32_t*>(am->data)[i] = st.amounts[i];
        static_cast<int32_t*>(pr->data)[i] = st.prices[i];
    }
    const int32_t bo = R::FindPropertyOffset(R::ClassOf(m), L"balance");
    if (bo >= 0) *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(m) + bo) = st.balance;
    SetBoolOf(m, L"isempty", st.empty);
    Refresh(m);
    if (!st.applied)
        UE_LOGI("vending: the copy of '%ls' takes the host's stock: amounts[0] %d, balance %d, empty %d", KeyOf(m).c_str(), st.amounts.empty() ? -1 : st.amounts[0],
                st.balance, st.empty ? 1 : 0);
    else
        UE_LOGI("vending: the copy of '%ls' follows the host: amounts[0] %d, balance %d, empty %d", KeyOf(m).c_str(), st.amounts.empty() ? -1 : st.amounts[0], st.balance,
                st.empty ? 1 : 0);
    // The refill is the figure's moment: the host's `isEmpty` rises with it (the game's own restock sets it), so that edge is the cue.
    if (!wasEmpty && st.empty && st.applied) PlayFigure(m);
    st.applied = true;
    return true;
}

void ClientPoll() {
    for (auto& kv : g_stage)
        if (kv.second.pending && Apply(kv.first, kv.second)) kv.second.pending = false;
}

// ---- the gate ----

bool ActiveClient() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}

uint8_t ByteParam(const sg::Call& c, const wchar_t* name, uint8_t dflt) {
    const int32_t off = R::FindParamOffset(c.function, name);
    return off >= 0 && c.locals ? c.locals[off] : dflt;
}

void* ObjParam(const sg::Call& c, const wchar_t* name) {
    const int32_t off = R::FindParamOffset(c.function, name);
    return off >= 0 && c.locals ? *reinterpret_cast<void* const*>(c.locals + off) : nullptr;
}

// A client's buy or taking of the change: asked of the host.
sg::Verdict OnUsePre(const sg::Call& c) {
    if (!ActiveClient() || !c.object || !R::IsLive(c.object)) return sg::Verdict::Run;
    const uint8_t action = ByteParam(c, L"action", 0xFF);
    if (action != kActionBuy && action != kActionChange) return sg::Verdict::Run;
    const std::wstring key = KeyOf(c.object);
    if (key.empty()) return sg::Verdict::Run;
    uint8_t slot = kNoSlot;
    if (action == kActionBuy) {
        const int32_t lo = R::FindPropertyOffset(R::ClassOf(c.object), L"lookatitem");
        const TArr* items = ArrOf(c.object, L"retarget_items");
        if (lo >= 0 && items && items->data)
            for (int i = 0; i < items->num && i < kMaxSlots; ++i)
                if (std::memcmp(static_cast<const uint8_t*>(c.object) + lo, static_cast<const R::FName*>(items->data) + i, sizeof(R::FName)) == 0) slot = static_cast<uint8_t>(i);
        if (slot == kNoSlot) return sg::Verdict::Run;   // no aimed item: the game's own body says "out of stock" or does nothing
    }
    coop::net::PropIgniteIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.verb = kVerbVendUse;
    p.pad[0] = action;
    p.pad[1] = slot;
    const bool sent = g_session.load()->SendReliable(coop::net::ReliableKind::PropIgniteIntent, &p, sizeof(p));
    UE_LOGI("vending: CLIENT's %s on '%ls' (slot %d) is refused here -- the host is asked to run it: %d", action == kActionBuy ? "buy" : "take-the-change",
            key.c_str(), slot == kNoSlot ? -1 : slot, sent ? 1 : 0);
    return sg::Verdict::Cancel;
}

// A client's coin put in: the host's own coin (the same prop, by its element id) is put in on the host's machine.
sg::Verdict OnCoinPre(const sg::Call& c) {
    if (!ActiveClient() || !c.object || !R::IsLive(c.object)) return sg::Verdict::Run;
    void* coin = ObjParam(c, L"holdobject");
    const std::wstring key = KeyOf(c.object);
    if (key.empty() || !coin || !R::IsLive(coin)) {
        UE_LOGI("vending: CLIENT's coin on '%ls' runs here: no held prop read from the call (key %d, coin %p)", key.c_str(), key.empty() ? 0 : 1, coin);
        return sg::Verdict::Run;
    }
    coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(coin);
    if (eid == coop::element::kInvalidId) eid = coop::remote_prop::ResolveMirrorEidByActor(coin);   // a host-made coin is a mirror here
    if (eid == coop::element::kInvalidId) {
        UE_LOGI("vending: CLIENT's coin %p on '%ls' runs here: it is no prop of the shared set (no element id)", coin, key.c_str());
        return sg::Verdict::Run;
    }
    coop::net::PropIgniteIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.elementId = static_cast<uint32_t>(eid);
    p.verb = kVerbVendCoin;
    const bool sent = g_session.load()->SendReliable(coop::net::ReliableKind::PropIgniteIntent, &p, sizeof(p));
    UE_LOGI("vending: CLIENT's coin (eid %u) on '%ls' is refused here -- the host is asked to put it in: %d", p.elementId, key.c_str(), sent ? 1 : 0);
    return sg::Verdict::Cancel;
}

std::uint64_t g_restockRefused = 0;

sg::Verdict OnRestockPre(const sg::Call& c) {
    if (!ActiveClient()) return sg::Verdict::Run;
    const std::uint64_t n = ++g_restockRefused;
    if ((n & (n - 1)) == 0) UE_LOGI("vending: client-refuse the refill of %p (call #%llu)", c.object, static_cast<unsigned long long>(n));
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watchAsked) return;
    g_watchAsked = true;
    const bool a = sg::WatchName(kRowUse, kTagUse, &OnUsePre, nullptr);
    const bool b = sg::WatchName(kRowCoin, kTagCoin, &OnCoinPre, nullptr);
    const bool c = sg::WatchName(kRowBeginRestock, kTagRestock, &OnRestockPre, nullptr);
    const bool d = sg::WatchName(kRowCheckStock, kTagRestock + 1, &OnRestockPre, nullptr);
    const bool e = sg::WatchName(kRowTimerCheckStock, kTagRestock + 2, &OnRestockPre, nullptr);
    if (!(a && b && c && d && e)) UE_LOGE("vending: a watch did not register (use %d coin %d begin %d check %d timer %d)", a, b, c, d, e);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (!host && g_stage.empty()) return;
    const auto now = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastPoll).count() < kPollMs) return;
    g_lastPoll = now;
    if (host) HostPoll(s);
    else ClientPoll();
}

void OnState(const Payload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;
    // The floats become ints: a value past an int's range (or NaN) would make the cast undefined.
    constexpr float kIntMax = 2.0e9f;
    const int n = (p.pitch >= 1.f && p.pitch <= static_cast<float>(kMaxSlots)) ? static_cast<int>(p.pitch) : 0;
    const int slot = p._pad[0];
    if (n == 0 || slot >= n || !std::isfinite(p.x) ||
        !std::isfinite(p.y) || !std::isfinite(p.z) || std::fabs(p.x) > kIntMax || std::fabs(p.y) > kIntMax ||
        std::fabs(p.z) > kIntMax || p.birthKey.len > sizeof(p.birthKey.data)) {
        UE_LOGW("vending: a malformed slot (n %d, slot %d) -- dropped", n, slot);
        return;
    }
    Stage& st = g_stage[p.id];
    if (st.n != n) {
        const bool applied = st.applied;
        st = Stage{};
        st.applied = applied;
        st.n = n;
        st.amounts.assign(n, 0);
        st.prices.assign(n, 0);
        st.items.assign(n, std::string());
    }
    if (slot == 0) st.got = 0;   // a new round
    st.amounts[slot] = static_cast<int32_t>(p.x);
    st.prices[slot] = static_cast<int32_t>(p.y);
    st.items[slot] = std::string(p.birthKey.data, p.birthKey.len);
    st.balance = static_cast<int32_t>(p.z);
    st.empty = p.yaw > 0.5f;
    st.got |= 1u << slot;
    if (st.got == (n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1u))) st.pending = !Apply(p.id, st);
}

bool TakeIntent(int senderSlot, const uint8_t* bytes, uint16_t len) {
    coop::net::PropIgniteIntentPayload p{};
    if (!bytes || len < sizeof(p)) return false;
    std::memcpy(&p, bytes, sizeof(p));
    if (p.verb != kVerbVendUse && p.verb != kVerbVendCoin) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || p.key.len > sizeof(p.key.data)) return true;
    void* m = MachineByKey(coop::net::StringFromWireKey(p.key));
    void* player = coop::players::Registry::Get().Local();
    if (!m || !player) {
        UE_LOGW("vending: slot %d asked for a machine this host does not hold ('%ls') -- dropped", senderSlot, coop::net::StringFromWireKey(p.key).c_str());
        return true;
    }
    if (p.verb == kVerbVendUse) {
        const uint8_t action = p.pad[0], slot = p.pad[1];
        if (action != kActionBuy && action != kActionChange) return true;
        const int32_t before = IntOf(m, L"balance");
        if (action == kActionBuy) {
            const int32_t lo = R::FindPropertyOffset(R::ClassOf(m), L"lookatitem");
            const TArr* items = ArrOf(m, L"retarget_items");
            if (lo < 0 || !items || !items->data || slot >= items->num) return true;
            std::memcpy(static_cast<uint8_t*>(m) + lo, static_cast<const R::FName*>(items->data) + slot, sizeof(R::FName));
            SetBoolOf(m, L"lookatbuttons", true);
        } else {
            SetBoolOf(m, L"lookatbuttons", false);
        }
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(m), L"actionOptionIndex");
        ue_wrap::ParamFrame f(fn);
        const bool ran = fn && f.valid() && f.Set<void*>(L"player", player) && f.Set<uint8_t>(L"action", action) && ue_wrap::Call(m, f);
        UE_LOGI("vending: HOST ran slot %d's %s on '%ls' (slot %d): %d; balance %d -> %d", senderSlot, action == kActionBuy ? "buy" : "take-the-change",
                coop::net::StringFromWireKey(p.key).c_str(), slot == kNoSlot ? -1 : slot, ran ? 1 : 0, before, IntOf(m, L"balance"));
        return true;
    }
    void* coin = coop::element::LivePropActor(static_cast<coop::element::ElementId>(p.elementId));
    if (!coin || !R::IsLive(coin)) {
        UE_LOGW("vending: slot %d put in a coin (eid %u) this host does not hold -- dropped", senderSlot, p.elementId);
        return true;
    }
    const int32_t no = R::FindPropertyOffset(R::ClassOf(coin), L"name");
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(m), L"playerUsedOn");
    ue_wrap::ParamFrame f(fn);
    bool ok = fn && f.valid() && f.Set<void*>(L"player", player) && f.Set<void*>(L"holdobject", coin);
    if (ok && no >= 0) ok = f.SetRaw(L"holdpropname", static_cast<const uint8_t*>(coin) + no, static_cast<int32_t>(sizeof(R::FName)));
    const int32_t before = IntOf(m, L"balance");
    const bool ran = ok && ue_wrap::Call(m, f);
    UE_LOGI("vending: HOST put slot %d's coin (eid %u) into '%ls': %d; balance %d -> %d", senderSlot, p.elementId,
            coop::net::StringFromWireKey(p.key).c_str(), ran ? 1 : 0, before, IntOf(m, L"balance"));
    return true;
}

void QueueConnectBroadcastForSlot(int /*slot*/) { g_resendAll = true; }

void OnDisconnect() {
    g_stage.clear();
    g_host.clear();
    g_resendAll = false;
}

}  // namespace coop::vending_sync
