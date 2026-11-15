// coop/dev/vending_drill.cpp -- see coop/dev/vending_drill.h.

#include "coop/dev/vending_drill.h"

#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"
#include "coop/world/spawn_authority.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::vending_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

uint64_t g_clientSeenMs = 0, g_lastSayHost = 0, g_lastSayClient = 0;
bool g_done[16] = {};
uint64_t g_clientWaitSince = 0;

// This world's live instances of a class (the object index's walk of the class's own instances, then the liveness and world tests).
std::vector<void*> Live(const wchar_t* cls) {
    std::vector<void*> out;
    void* c = ue_wrap::object_index::ClassByName(cls);
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!c || !world) return out;
    std::vector<void*> found;
    ue_wrap::object_index::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
        if (obj) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &found);
    for (void* obj : found)
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__") && ue_wrap::world_identity::WorldOf(obj) == world)
            out.push_back(obj);
    return out;
}

bool Once(int slot, uint64_t since, uint64_t atMs) {
    if (g_done[slot] || since < atMs) return false;
    g_done[slot] = true;
    return true;
}

bool SayDue(uint64_t since, uint64_t& last) {
    const uint64_t bucket = since / 5000 + 1;
    if (bucket <= last) return false;
    last = bucket;
    return true;
}

// ---- the vending machines ----

struct TArr { void* data; int32_t num; int32_t max; };

const TArr* ArrOf(void* actor, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), name);
    return off < 0 ? nullptr : reinterpret_cast<const TArr*>(static_cast<const uint8_t*>(actor) + off);
}

std::vector<int32_t> Ints(void* actor, const wchar_t* name) {
    std::vector<int32_t> v;
    const TArr* a = ArrOf(actor, name);
    for (int i = 0; a && a->data && i < a->num; ++i) v.push_back(static_cast<const int32_t*>(a->data)[i]);
    return v;
}

std::vector<std::wstring> Names(void* actor, const wchar_t* name) {
    std::vector<std::wstring> v;
    const TArr* a = ArrOf(actor, name);
    for (int i = 0; a && a->data && i < a->num; ++i) v.push_back(R::ToString(static_cast<const R::FName*>(a->data)[i]));
    return v;
}

std::string Join(const std::vector<int32_t>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
    return s.empty() ? "-" : s;
}

bool Bool(void* actor, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return R::FindBoolProperty(R::ClassOf(actor), name, off, mask) && off >= 0 && (*(static_cast<const uint8_t*>(actor) + off) & mask) != 0;
}

void SetBool(void* actor, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(actor), name, off, mask) || off < 0) return;
    uint8_t* p = static_cast<uint8_t*>(actor) + off;
    *p = v ? (*p | mask) : (*p & ~mask);
}

int32_t Int(void* actor, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), name);
    return off < 0 ? -1 : *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(actor) + off);
}

void SetInt(void* actor, const wchar_t* name, int32_t v) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), name);
    if (off >= 0) *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(actor) + off) = v;
}

std::wstring KeyOf(void* a) {
    std::wstring k = ue_wrap::prop::GetInteractableKeyString(a);
    return k.empty() ? L"None" : k;
}

// The machines, in key order: both peers' first one is the same machine.
std::vector<void*> Machines() {
    std::vector<void*> m = Live(L"prop_vending_C");
    std::sort(m.begin(), m.end(), [](void* a, void* b) { return KeyOf(a) < KeyOf(b); });
    return m;
}

bool Call0(void* actor, const wchar_t* fnName) {
    void* fn = actor ? R::FindDispatchFunctionCached(R::ClassOf(actor), fnName) : nullptr;
    return fn && R::CallFunction(actor, fn, nullptr);
}

void SayVend(const char* who, uint64_t since) {
    const std::vector<void*> ms = Machines();
    size_t n = 0;
    for (void* m : ms) {
        if (++n > 3) break;
        UE_LOGI("[GAP3] [%s] t=%us: machine %zu/%zu key '%ls' items %zu amounts [%s] prices [%s] balance %d empty %d working %d", who,
                static_cast<unsigned>(since / 1000), n, ms.size(), KeyOf(m).c_str(), Names(m, L"retarget_items").size(),
                Join(Ints(m, L"retarget_amounts")).c_str(), Join(Ints(m, L"retarget_prices")).c_str(), Int(m, L"balance"),
                Bool(m, L"isempty") ? 1 : 0, Bool(m, L"isworking") ? 1 : 0);
    }
    if (ms.empty()) UE_LOGI("[GAP3] [%s] t=%us: no prop_vending_C", who, static_cast<unsigned>(since / 1000));
    UE_LOGI("[GAP3] [%s] t=%us: %zu plain prop_C, %zu prop_food_C", who, static_cast<unsigned>(since / 1000), Live(L"prop_C").size(),
            Live(L"prop_food_C").size());
}

// The buy: the machine's own actionOptionIndex(action 4) with the look-at set to stock item `slot`, as the player's aim sets it.
bool Buy(void* m, size_t slot, uint8_t action) {
    const int32_t io = R::FindPropertyOffset(R::ClassOf(m), L"retarget_items");
    const int32_t lo = R::FindPropertyOffset(R::ClassOf(m), L"lookatitem");
    const TArr* items = ArrOf(m, L"retarget_items");
    if (io < 0 || lo < 0 || !items || !items->data || static_cast<int32_t>(slot) >= items->num) return false;
    std::memcpy(static_cast<uint8_t*>(m) + lo, static_cast<const R::FName*>(items->data) + slot, sizeof(R::FName));
    SetBool(m, L"lookatbuttons", true);
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(m), L"actionOptionIndex");
    ue_wrap::ParamFrame f(fn);
    void* player = coop::players::Registry::Get().Local();
    return fn && f.valid() && f.Set<void*>(L"player", player) && f.Set<uint8_t>(L"action", action) && ue_wrap::Call(m, f);
}

void ClientCoin();

void ClientVend(bool lane, uint64_t since) {
    const std::vector<void*> ms = Machines();
    if (ms.empty()) return;
    void* m = ms.front();
    if (!lane && Once(0, since, 12000)) {
        SetInt(m, L"balance", 1000);
        SetBool(m, L"isworking", false);
        const std::vector<int32_t> before = Ints(m, L"retarget_amounts");
        const bool ran = Buy(m, 0, 4);
        UE_LOGI("[GAP3] [C] BASELINE: balance written 1000 on this peer's machine, buy of slot 0 called: %d; amounts [%s] -> [%s], balance %d", ran ? 1 : 0,
                Join(before).c_str(), Join(Ints(m, L"retarget_amounts")).c_str(), Int(m, L"balance"));
    }
    if (lane) {
        ClientCoin();
        // Waits until the host's balance has reached this copy, then buys once.
        if (!g_done[0] && Int(m, L"balance") > 0) {
            if (!g_clientWaitSince) g_clientWaitSince = coop::net::NowMs();
            if (coop::net::NowMs() - g_clientWaitSince > 3000) {
                g_done[0] = true;
                const std::vector<int32_t> before = Ints(m, L"retarget_amounts");
                const bool ran = Buy(m, 0, 4);
                UE_LOGI("[GAP3] [C] LANE: this copy shows the host's balance %d; buy of slot 0 called: %d (refused here, asked of the host); amounts "
                        "right after [%s] (were [%s])", Int(m, L"balance"), ran ? 1 : 0, Join(Ints(m, L"retarget_amounts")).c_str(), Join(before).c_str());
            }
        }
    }
}

struct NameCtx { int32_t off; R::FName name; };
void WriteName(void* ctx, void* actor) {
    const auto* c = static_cast<const NameCtx*>(ctx);
    if (c->off >= 0) std::memcpy(static_cast<uint8_t*>(actor) + c->off, &c->name, sizeof(c->name));
}

// The game's own coin: a plain prop_C named pmoney_c_5 (vending's spawnCoin), with the name written before its begin play.
void* SpawnCoin(const ue_wrap::FVector& at) {
    void* cls = R::FindClass(L"prop_C");
    if (!cls) return nullptr;
    NameCtx ctx{R::FindPropertyOffset(cls, L"name"), ue_wrap::fname_utils::StringToFName(L"pmoney_c_5")};
    return ctx.off < 0 || ctx.name.ComparisonIndex == 0 ? nullptr : E::SpawnActor(cls, at, false, &WriteName, &ctx);
}

void* FindCoin() {
    for (void* a : Live(L"prop_C"))
        if (R::ClassNameOf(a) == L"prop_C" && ue_wrap::prop::GetPropNameString(a) == L"pmoney_c_5") return a;
    return nullptr;
}

// A coin put in: the CLIENT runs the machine's playerUsedOn with the host's coin (the copy of it) in hand; refused there, asked of the host.
void ClientCoin() {
    if (g_done[3]) return;
    void* coin = FindCoin();
    const std::vector<void*> ms = Machines();
    if (!coin || ms.empty()) return;
    g_done[3] = true;
    void* m = ms.front();
    const int32_t no = R::FindPropertyOffset(R::ClassOf(coin), L"name");
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(m), L"playerUsedOn");
    ue_wrap::ParamFrame f(fn);
    bool ok = fn && f.valid() && f.Set<void*>(L"player", coop::players::Registry::Get().Local()) && f.Set<void*>(L"holdobject", coin);
    if (ok && no >= 0) ok = f.SetRaw(L"holdpropname", static_cast<const uint8_t*>(coin) + no, static_cast<int32_t>(sizeof(R::FName)));
    const bool ran = ok && ue_wrap::Call(m, f);
    UE_LOGI("[GAP3] [C] COIN: this peer put the host's coin %p in its copy of the machine: %d (refused here, asked of the host); balance on this copy %d", coin,
            ran ? 1 : 0, Int(m, L"balance"));
}

void HostVend(bool lane, uint64_t since) {
    const std::vector<void*> ms = Machines();
    if (ms.empty()) return;
    void* m = ms.front();
    if (lane && Once(1, since, 6000)) {
        SetInt(m, L"balance", 1000);
        UE_LOGI("[GAP3] [H] balance 1000 written on the host's machine '%ls'", KeyOf(m).c_str());
    }
    if (lane && Once(4, since, 24000)) {
        void* player = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        void* coin = player && E::TryGetActorLocation(player, at) ? SpawnCoin({at.X + 150.f, at.Y + 150.f, at.Z + 50.f}) : nullptr;
        UE_LOGI("[GAP3] [H] a pmoney_c_5 coin stood %p; the machine's balance is %d", coin, Int(m, L"balance"));
    }
    if (lane && Once(5, since, 32000))
        UE_LOGI("[GAP3] [H] COIN: the host's machine balance is now %d", Int(m, L"balance"));
    if (lane && Once(2, since, 40000)) {
        const TArr* a = ArrOf(m, L"retarget_amounts");
        for (int i = 0; a && a->data && i < a->num; ++i) static_cast<int32_t*>(a->data)[i] = 0;
        SetBool(m, L"isempty", false);
        UE_LOGI("[GAP3] [H] the host's machine emptied (every amount written 0); checkStock called: %d", Call0(m, L"checkStock") ? 1 : 0);
    }
}

// ---- the Poolwalker: poolwalker_C ----

void SayWalker(const char* who, uint64_t since) {
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::FVector pp{};
    const bool havePlayer = player && E::TryGetActorLocation(player, pp);
    const std::vector<void*> w = Live(L"poolwalker_C");
    UE_LOGI("[GAP3] [%s] t=%us: %zu poolwalker_C held; class loaded: %d; this peer's player at (%.0f, %.0f, %.0f)", who, static_cast<unsigned>(since / 1000), w.size(),
            ue_wrap::object_index::ClassByName(L"poolwalker_C") ? 1 : 0, havePlayer ? pp.X : 0.f, havePlayer ? pp.Y : 0.f, havePlayer ? pp.Z : 0.f);
    for (void* a : w) {
        ue_wrap::FVector at{};
        E::TryGetActorLocation(a, at);
        void* ctrl = nullptr;
        const int32_t co = R::FindPropertyOffset(R::ClassOf(a), L"Controller");
        if (co >= 0) ctrl = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(a) + co);
        UE_LOGI("[GAP3] [%s] t=%us:   '%ls' at (%.0f, %.0f, %.0f) %s, controller %p, class chain %ls", who, static_cast<unsigned>(since / 1000), R::ToString(R::NameOf(a)).c_str(),
                at.X, at.Y, at.Z, Bool(a, L"bhidden") ? "hidden" : "shown", ctrl, R::ClassNameOf(a).c_str());
    }
}

// ---- the Reactor Restart: trigger_jamDoor_C ----

// The doors the jam trigger names (the level's `door_3` and `door2_0`, keys basedoor_entrance and basedoor_balcony): by level name, on either peer;
// and how many doors of the world are jammed.
void SayJam(const char* who, uint64_t since) {
    size_t n = 0, total = 0, jammed = 0;
    for (void* d : Live(L"door_C")) {
        ++total;
        const bool j = Bool(d, L"jammed");
        if (j) ++jammed;
        const std::wstring nm = R::ToString(R::NameOf(d));
        if (nm != L"door_3" && nm != L"door2_0" && !j) continue;
        UE_LOGI("[GAP3] [%s] t=%us: door '%ls' key '%ls' isOpened %d isMoving %d jammed %d", who, static_cast<unsigned>(since / 1000), nm.c_str(), KeyOf(d).c_str(),
                Bool(d, L"isopened") ? 1 : 0, Bool(d, L"ismoving") ? 1 : 0, j ? 1 : 0);
        ++n;
    }
    UE_LOGI("[GAP3] [%s] t=%us: %zu jam trigger(s) held, %zu of %zu door(s) named above, %zu jammed; jamDoor.runTrigger refused %llu time(s)", who,
            static_cast<unsigned>(since / 1000), Live(L"trigger_jamDoor_C").size(), n, total, jammed,
            static_cast<unsigned long long>(coop::spawn_authority::RefusalCount("jamDoor.runTrigger")));
}

void ClientRunJam() {
    const std::vector<void*> trig = Live(L"trigger_jamDoor_C");
    void* fn = trig.empty() ? nullptr : R::FindDispatchFunctionCached(R::ClassOf(trig.front()), L"runTrigger");
    ue_wrap::ParamFrame f(fn);
    const bool ran = fn && f.valid() && f.Set<void*>(L"owner", nullptr) && f.Set<int32_t>(L"index", 0) && ue_wrap::Call(trig.front(), f);
    UE_LOGI("[GAP3] [C] this peer ran its own jam trigger: %d (a client's run is refused)", ran ? 1 : 0);
}

void RunJam() {
    const std::vector<void*> trig = Live(L"trigger_jamDoor_C");
    void* fn = trig.empty() ? nullptr : R::FindDispatchFunctionCached(R::ClassOf(trig.front()), L"runTrigger");
    ue_wrap::ParamFrame f(fn);
    const bool ran = fn && f.valid() && f.Set<void*>(L"owner", nullptr) && f.Set<int32_t>(L"index", 0) && ue_wrap::Call(trig.front(), f);
    UE_LOGI("[GAP3] [H] the level holds %zu jam trigger(s); runTrigger called: %d", trig.size(), ran ? 1 : 0);
}

}  // namespace

bool IsDrill(const std::string& n) { return n == "vendBase" || n == "vendLane" || n == "jamDoor" || n == "walker"; }

void ClientStep(const std::string& name) {
    const uint64_t now = coop::net::NowMs();
    if (!g_clientSeenMs) g_clientSeenMs = now;
    const uint64_t since = now - g_clientSeenMs;
    if (SayDue(since, g_lastSayClient)) {
        if (name == "vendBase" || name == "vendLane") SayVend("C", since);
        if (name == "jamDoor") SayJam("C", since);
        if (name == "walker") SayWalker("C", since);
    }
    // Object walks at most once a second, never per frame.
    static uint64_t s_lastWork = 0;
    if (now - s_lastWork < 1000) return;
    s_lastWork = now;
    if (name == "vendBase" || name == "vendLane") ClientVend(name == "vendLane", since);
    if (name == "jamDoor" && Once(7, since, 30000)) ClientRunJam();
}

void HostStep(const std::string& name, uint64_t since) {
    if (SayDue(since, g_lastSayHost)) {
        if (name == "vendBase" || name == "vendLane") SayVend("H", since);
        if (name == "jamDoor") SayJam("H", since);
        if (name == "walker") SayWalker("H", since);
    }
    if (name == "jamDoor" && Once(6, since, 10000)) RunJam();
    static uint64_t s_lastWork = 0;
    if (since < s_lastWork || since - s_lastWork < 1000) return;
    s_lastWork = since;
    if (name == "vendBase" || name == "vendLane") HostVend(name == "vendLane", since);
}

void OnDisconnect() {
    g_clientSeenMs = g_lastSayHost = g_lastSayClient = g_clientWaitSince = 0;
    for (bool& b : g_done) b = false;
}

}  // namespace coop::dev::vending_drill
