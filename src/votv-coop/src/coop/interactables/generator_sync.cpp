// coop/interactables/generator_sync.cpp -- see coop/interactables/generator_sync.h.

#include "coop/interactables/generator_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/props/remote_prop.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::generator_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
using Payload = N::GeneratorPayload;

constexpr const wchar_t* kClass = L"prop_generatorP_C";
constexpr const wchar_t* kCanClass = L"prop_gascan_C";
constexpr float kMaxFuel = 100.f;
constexpr float kReachUU = 400.f;
constexpr uint8_t kStartAction = 8;      // actionOptionIndex's action: tryActivate
constexpr uint64_t kReadMs = 500;        // the host reads its generators twice a second
constexpr uint64_t kRunningMs = 1000;    // and sends a running one once a second
constexpr int kMaxIntentsPerSecond = 8;  // a sender's intents past this in one second are dropped

UE_FACT_FN(kRowTick, "generator_sync", L"prop_generatorP_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowFuelIn, "generator_sync", L"prop_generatorP_C",
           L"BndEvt__prop_generatorP_fuelIn_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPour, "generator_sync", L"prop_generatorP_C", L"gascanFuel", Declared);
UE_FACT_FN(kRowPress, "generator_sync", L"prop_generatorP_C", L"actionOptionIndex", Declared);

enum Row { kTick, kFuelIn, kPour, kPress, kRowCount };
const ue_wrap::facts::Function* const kRows[kRowCount] = {&kRowTick, &kRowFuelIn, &kRowPour, &kRowPress};
constexpr int kTagBase = 0x47454E00;   // 'GEN', then the row

std::atomic<N::Session*> g_session{nullptr};
uint32_t g_registered = 0;   // bit i: row i's watch registered
int g_applying = 0;          // inside our own write of a state: no watch reads it as a player's act

uint64_t Now() { return ::GetTickCount64(); }

N::Session* Live() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() ? s : nullptr;
}

// ---- the generator's fields ----

struct Fields {
    int32_t fuel = -1, active = -1;
    uint8_t activeMask = 0;
};
const Fields& F(void* cls) {
    static Fields f;
    static bool done = false;
    if (!done && cls) {
        done = true;
        f.fuel = R::FindPropertyOffset(cls, L"fuel");
        R::FindBoolProperty(cls, L"isActive", f.active, f.activeMask);
        if (f.fuel < 0 || f.active < 0) UE_LOGW("generator_sync: prop_generatorP_C's fuel or isActive does not resolve");
    }
    return f;
}

bool IsGenerator(void* o) { return o && R::IsLive(o) && R::ClassNameOf(o) == kClass; }

bool Read(void* g, float& fuel, bool& active) {
    const Fields& f = F(R::ClassOf(g));
    if (f.fuel < 0 || f.active < 0) return false;
    const uint8_t* b = static_cast<const uint8_t*>(g);
    fuel = *reinterpret_cast<const float*>(b + f.fuel);
    active = (b[f.active] & f.activeMask) != 0;
    return true;
}

bool Write(void* g, float fuel, bool active) {
    const Fields& f = F(R::ClassOf(g));
    if (f.fuel < 0 || f.active < 0) return false;
    uint8_t* b = static_cast<uint8_t*>(g);
    *reinterpret_cast<float*>(b + f.fuel) = fuel;
    b[f.active] = active ? static_cast<uint8_t>(b[f.active] | f.activeMask) : static_cast<uint8_t>(b[f.active] & ~f.activeMask);
    return true;
}

bool CallParamless(void* g, const wchar_t* name) {
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(g), name);
    return fn && R::CallFunction(g, fn, nullptr);
}

std::vector<void*> All() {
    std::vector<void*> out;
    if (void* const c = OI::ClassByName(kClass))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

std::wstring KeyOf(void* g) {
    const std::wstring k = ue_wrap::prop::GetInteractableKeyString(g);
    return k.empty() || k == L"None" || k.size() > sizeof(N::WireKey::data) ? std::wstring() : k;
}

void Fill(Payload& p, const std::wstring& key, float fuel, bool active, uint8_t op) {
    p = Payload{};
    N::WireKeyFromString(key, p.key);
    p.fuel = fuel;
    p.active = active ? 1 : 0;
    p.op = op;
}

// ---- HOST: the reads and the sends ----

struct Sent {
    float fuel = -1.f;
    bool active = false;
    uint64_t at = 0;
};
std::unordered_map<std::wstring, Sent> g_sent;
uint64_t g_nextRead = 0;

void SendState(N::Session& s, void* g, const std::wstring& key, int slot) {
    float fuel = 0.f;
    bool active = false;
    if (!Read(g, fuel, active)) return;
    Payload p;
    Fill(p, key, fuel, active, N::kGeneratorOpState);
    if (slot >= 0) {
        s.SendReliableToSlot(slot, N::ReliableKind::GeneratorState, &p, sizeof(p));
        return;
    }
    s.SendReliable(N::ReliableKind::GeneratorState, &p, sizeof(p));
    g_sent[key] = Sent{fuel, active, Now()};
}

void HostRead(N::Session& s) {
    if (Now() < g_nextRead) return;
    g_nextRead = Now() + kReadMs;
    for (void* g : All()) {
        float fuel = 0.f;
        bool active = false;
        const std::wstring key = KeyOf(g);
        if (key.empty() || !Read(g, fuel, active)) continue;
        const auto it = g_sent.find(key);
        const bool changed = it == g_sent.end() ? (active || fuel > 0.f)
                                                : (it->second.active != active || std::fabs(it->second.fuel - fuel) >= 0.5f);
        const bool due = active && (it == g_sent.end() || Now() - it->second.at >= kRunningMs);
        if (changed || due) SendState(s, g, key, -1);
    }
}

// ---- HOST: a client's intents ----

struct Rate {
    uint64_t second = 0;
    int n = 0;
};
Rate g_rate[N::kMaxPeers];

void Refused(N::Session& s, uint8_t slot, void* g, const std::wstring& key, const char* why) {
    UE_LOGI("generator_sync: slot %u's intent on '%ls' refused: %s", static_cast<unsigned>(slot), key.c_str(), why);
    if (g) SendState(s, g, key, slot);
}

void Judge(N::Session& s, uint8_t slot, const Payload& p) {
    const std::wstring key = N::StringFromWireKey(p.key);
    if (!s.IsSlotWorldReady(slot)) return;
    void* const g = key.empty() ? nullptr : coop::prop_key_index::Find(key);
    if (!IsGenerator(g)) {
        UE_LOGI("generator_sync: slot %u's intent names no generator ('%ls')", static_cast<unsigned>(slot), key.c_str());
        return;
    }
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) return Refused(s, slot, g, key, "dead");
    const coop::element::IntentSubject subject = coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(g);
    if (!subject) return Refused(s, slot, g, key, coop::element::OutcomeName(subject.outcome));
    float fuel = 0.f;
    bool active = false;
    if (!Read(g, fuel, active)) return;
    if (p.op == N::kGeneratorOpPour) {
        if (!(coop::hand_item::HeldClassIsA(slot, kCanClass) || coop::remote_prop::CarriesClassA(slot, kCanClass))) return Refused(s, slot, g, key, "no gas can in the hand");
        if (!(p.fuel > 0.f) || p.fuel > kMaxFuel) return Refused(s, slot, g, key, "a malformed gain");
        if (!(fuel < kMaxFuel)) return Refused(s, slot, g, key, "the tank is full");
        ++g_applying;
        Write(g, std::fmin(fuel + p.fuel, kMaxFuel), active);
        --g_applying;
        UE_LOGI("generator_sync: slot %u poured %.1f into '%ls'", static_cast<unsigned>(slot), p.fuel, key.c_str());
    } else {
        const bool wanted = p.active != 0;
        if (wanted == active) return Refused(s, slot, g, key, "already in that state");
        if (wanted && !(fuel > 0.f)) return Refused(s, slot, g, key, "no fuel");
        ++g_applying;
        CallParamless(g, L"tryActivate");   // the game's own: on with fuel, else off, and the outlets
        --g_applying;
        UE_LOGI("generator_sync: slot %u switched '%ls' %s", static_cast<unsigned>(slot), key.c_str(), wanted ? "on" : "off");
    }
    SendState(s, g, key, -1);
}

// ---- CLIENT: the watches ----

struct Pend {
    void* g = nullptr;
    float fuel = 0.f;
    bool active = false;
    bool valid = false;
};
Pend g_pend[kRowCount];

bool LocalPlayerParam(const sg::Call& call) {
    static R::ParamOffset sPlayer{L"player"};
    const int32_t off = call.locals ? sPlayer.Of(call.function) : -1;
    return off >= 0 && coop::players::Registry::Get().IsLocal(*reinterpret_cast<void* const*>(call.locals + off));
}

sg::Verdict OnPre(const sg::Call& call) {
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount || g_applying || !coop::spawn_authority::IsActiveClientSession())
        return sg::Verdict::Run;
    // The burn and a can lying in the fuel box are the host copy's.
    if (row == kTick || row == kFuelIn) return sg::Verdict::Cancel;
    Pend& pd = g_pend[row];
    pd.valid = false;
    if (!LocalPlayerParam(call)) return sg::Verdict::Run;
    if (row == kPress) {
        static R::ParamOffset sAction{L"action"};
        const int32_t off = sAction.Of(call.function);
        if (off < 0 || call.locals[off] != kStartAction) return sg::Verdict::Run;
    }
    pd.g = call.object;
    pd.valid = Read(call.object, pd.fuel, pd.active);
    return sg::Verdict::Run;
}

void Ask(uint8_t op, void* g, float fuel, bool active) {
    N::Session* s = Live();
    const std::wstring key = KeyOf(g);
    if (!s || s->role() != N::Role::Client || key.empty()) return;
    Payload p;
    Fill(p, key, fuel, active, op);
    s->SendReliable(N::ReliableKind::GeneratorIntent, &p, sizeof(p));
}

void OnPost(const sg::Call& call) {
    const int row = call.tag - kTagBase;
    if (row != kPour && row != kPress) return;
    Pend& pd = g_pend[row];
    if (!pd.valid || pd.g != call.object) return;
    pd.valid = false;
    float fuel = 0.f;
    bool active = false;
    if (!Read(call.object, fuel, active)) return;
    if (row == kPour && fuel > pd.fuel) {
        Ask(N::kGeneratorOpPour, call.object, fuel - pd.fuel, active);
        UE_LOGI("generator_sync: this player poured %.1f into '%ls' -- the host adds it", fuel - pd.fuel,
                KeyOf(call.object).c_str());
    } else if (row == kPress && active != pd.active) {
        Ask(N::kGeneratorOpPress, call.object, fuel, active);
        UE_LOGI("generator_sync: this player switched '%ls' %s -- the host switches it", KeyOf(call.object).c_str(),
                active ? "on" : "off");
    }
}

void Register() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_registered & (1u << i)) continue;
        if (sg::WatchName(*kRows[i], kTagBase + i, &OnPre, (i == kPour || i == kPress) ? &OnPost : nullptr))
            g_registered |= 1u << i;
    }
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* s = Live();
    if (!s) return;
    if (g_registered != (1u << kRowCount) - 1) {
        static uint64_t nextTry = 0;
        if (Now() >= nextTry) {
            nextTry = Now() + 1000;
            Register();
            if (g_registered == (1u << kRowCount) - 1) UE_LOGI("generator_sync: the portable generator's watches are live");
        }
    }
    if (s->role() == N::Role::Host) HostRead(*s);
}

void OnState(const Payload& p) {
    const std::wstring key = N::StringFromWireKey(p.key);
    void* const g = key.empty() ? nullptr : coop::prop_key_index::Find(key);
    if (!IsGenerator(g) || !std::isfinite(p.fuel)) return;
    float fuel = 0.f;
    bool active = false;
    if (!Read(g, fuel, active)) return;
    const bool wantActive = p.active != 0;
    ++g_applying;
    Write(g, std::fmin(std::fmax(p.fuel, 0.f), kMaxFuel), wantActive);
    if (wantActive != active) CallParamless(g, L"activateOutlets");
    --g_applying;
}

void OnIntent(int senderSlot, const Payload& p) {
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Host || senderSlot <= 0 || senderSlot >= N::kMaxPeers) return;
    if (p.op != N::kGeneratorOpPour && p.op != N::kGeneratorOpPress) return;
    Rate& r = g_rate[senderSlot];
    const uint64_t sec = Now() / 1000;
    if (r.second != sec) r = Rate{sec, 0};
    if (++r.n > kMaxIntentsPerSecond) return;
    Judge(*s, static_cast<uint8_t>(senderSlot), p);
}

void SendJoin(int slot) {
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Host) return;
    for (void* g : All()) {
        const std::wstring key = KeyOf(g);
        if (!key.empty()) SendState(*s, g, key, slot);
    }
}

void OnDisconnect() {
    g_sent.clear();
    for (Rate& r : g_rate) r = Rate{};
    for (Pend& p : g_pend) p = Pend{};
}

}  // namespace coop::generator_sync
