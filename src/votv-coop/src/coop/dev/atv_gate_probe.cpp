// coop/dev/atv_gate_probe.cpp -- see coop/dev/atv_gate_probe.h.

#include "coop/dev/atv_gate_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/session.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::atv_gate_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;

// ATV_C's own bodies; the witch car inherits them, so a Declared row matches it too.
UE_FACT_FN(kRowGascanFuel, "dev.atv_gate_probe", L"ATV_C", L"gascanFuel", Declared);
UE_FACT_FN(kRowFuelUp, "dev.atv_gate_probe", L"ATV_C", L"fuelUp", Declared);
UE_FACT_FN(kRowToolboxFix, "dev.atv_gate_probe", L"ATV_C", L"toolboxFix", Declared);
UE_FACT_FN(kRowPlayerUsedOn, "dev.atv_gate_probe", L"ATV_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowUpgradeTake, "dev.atv_gate_probe", L"ATV_C", L"upgradeTake", Declared);
UE_FACT_FN(kRowUpdLights, "dev.atv_gate_probe", L"ATV_C", L"Upd Lights", Declared);
UE_FACT_FN(kRowSetBrake, "dev.atv_gate_probe", L"ATV_C", L"setBrake", Declared);
UE_FACT_FN(kRowUnturbo, "dev.atv_gate_probe", L"ATV_C", L"unturbo", Declared);
UE_FACT_FN(kRowLights, "dev.atv_gate_probe", L"ATV_C", L"InpActEvt_atv_lights_K2Node_InputActionEvent_3", Declared);
UE_FACT_FN(kRowBrake, "dev.atv_gate_probe", L"ATV_C", L"InpActEvt_atv_brake_K2Node_InputActionEvent_22", Declared);
UE_FACT_FN(kRowTurboPress, "dev.atv_gate_probe", L"ATV_C", L"InpActEvt_atv_turbo_K2Node_InputActionEvent_12", Declared);
UE_FACT_FN(kRowTurboRelease, "dev.atv_gate_probe", L"ATV_C", L"InpActEvt_atv_turbo_K2Node_InputActionEvent_13", Declared);
UE_FACT_FN(kRowHonkPress, "dev.atv_gate_probe", L"ATV_C", L"InpActEvt_atv_honk_K2Node_InputActionEvent_14", Declared);
UE_FACT_FN(kRowHonkRelease, "dev.atv_gate_probe", L"ATV_C", L"InpActEvt_atv_honk_K2Node_InputActionEvent_15", Declared);

constexpr int      kTagBase = 0x41544700;   // 'ATG', then the function
constexpr uint32_t kMaxLines = 200;         // per function and seam
constexpr auto     kPeRetry = std::chrono::seconds(1);
constexpr int      kPeMaxTries = 600;

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<uint32_t>            g_offThread{0};   // a pe line seen off the game thread, counted and dropped
Counts                           g_counts;         // game thread
uint32_t                         g_lines[2][kFnCount] = {};
std::vector<float>               g_toolboxHealth;  // the ATV's health at each toolboxFix call's entry; game thread
constexpr size_t                 kMaxTrace = 256;

const char* Role() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return "solo";
    return s->role() == coop::net::Role::Host ? "host" : "client";
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 0x20 || c == L'"' ? ' ' : (c < 128 ? static_cast<char>(c) : '?'));
    return s;
}

std::string Caller(const sg::Call& c) {
    if (!c.callerObject || !c.callerFunction || !R::IsLive(c.callerObject) || !R::IsLive(c.callerFunction)) return "none";
    return Narrow(R::ClassNameOf(c.callerObject)) + "." + Narrow(R::ToString(R::NameOf(c.callerFunction)));
}

struct Entry {
    const ue_wrap::facts::Function* row;
    const char*                     label;
    sg::PreFn                       gate;
    GT::ProcessEventObserverFn      pe;
    bool                            gateRegistered;
    bool                            gateSettled;
    bool                            gateLive;
    bool                            peRegistered;
};

const char* const kSeam[2] = {"script_gate", "pe"};
extern Entry g_entries[kFnCount];

void Fire(int i, int seam, void* self, const sg::Call* c) {
    if (!GT::IsGameThread()) {
        g_offThread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint32_t& n = seam == 0 ? g_counts.gate[i] : g_counts.pe[i];
    ++n;
    if (i == kFuelUp && seam == 0 && c) {
        const bool viaPour = c->callerFunction && R::IsLive(c->callerFunction) &&
                             R::ToString(R::NameOf(c->callerFunction)) == L"gascanFuel";
        ++(viaPour ? g_counts.fuelUpFromGascanFuel : g_counts.fuelUpOtherCaller);
    }
    if (i == kToolboxFix && seam == 0 && g_toolboxHealth.size() < kMaxTrace && self && R::IsLive(self))
        g_toolboxHealth.push_back(ue_wrap::atv::GetHealth(self));
    uint32_t& lines = g_lines[seam][i];
    if (lines >= kMaxLines) {
        if (lines == kMaxLines) {
            ++lines;
            UE_LOGI("[ATVG] %s %s via=%s past %u lines -- further calls counted only", Role(), g_entries[i].label,
                    kSeam[seam], kMaxLines);
        }
        return;
    }
    ++lines;
    const bool live = self && R::IsLive(self);
    UE_LOGI("[ATVG] %s %s via=%s self=%s mirror=%d caller=%s", Role(), g_entries[i].label, kSeam[seam],
            live ? Narrow(ue_wrap::atv::GetKeyString(self)).c_str() : "?",
            live && coop::atv_sync::NonOwnerCopy(self) ? 1 : 0, c ? Caller(*c).c_str() : "none");
}

template <int I> sg::Verdict GatePre(const sg::Call& c) {
    Fire(I, 0, c.object, &c);
    return sg::Verdict::Run;
}
template <int I> void PePre(void* self, void*, void*) { Fire(I, 1, self, nullptr); }

Entry g_entries[kFnCount] = {
    {&kRowGascanFuel, "gascanFuel", &GatePre<kGascanFuel>, &PePre<kGascanFuel>, false, false, false, false},
    {&kRowFuelUp, "fuelUp", &GatePre<kFuelUp>, &PePre<kFuelUp>, false, false, false, false},
    {&kRowToolboxFix, "toolboxFix", &GatePre<kToolboxFix>, &PePre<kToolboxFix>, false, false, false, false},
    {&kRowPlayerUsedOn, "playerUsedOn", &GatePre<kPlayerUsedOn>, &PePre<kPlayerUsedOn>, false, false, false, false},
    {&kRowUpgradeTake, "upgradeTake", &GatePre<kUpgradeTake>, &PePre<kUpgradeTake>, false, false, false, false},
    {&kRowUpdLights, "Upd Lights", &GatePre<kUpdLights>, &PePre<kUpdLights>, false, false, false, false},
    {&kRowSetBrake, "setBrake", &GatePre<kSetBrake>, &PePre<kSetBrake>, false, false, false, false},
    {&kRowUnturbo, "unturbo", &GatePre<kUnturbo>, &PePre<kUnturbo>, false, false, false, false},
    {&kRowLights, "InpActEvt_atv_lights", &GatePre<kLights>, &PePre<kLights>, false, false, false, false},
    {&kRowBrake, "InpActEvt_atv_brake", &GatePre<kBrake>, &PePre<kBrake>, false, false, false, false},
    {&kRowTurboPress, "InpActEvt_atv_turbo_press", &GatePre<kTurboPress>, &PePre<kTurboPress>, false, false, false, false},
    {&kRowTurboRelease, "InpActEvt_atv_turbo_release", &GatePre<kTurboRelease>, &PePre<kTurboRelease>, false, false, false,
     false},
    {&kRowHonkPress, "InpActEvt_atv_honk_press", &GatePre<kHonkPress>, &PePre<kHonkPress>, false, false, false, false},
    {&kRowHonkRelease, "InpActEvt_atv_honk_release", &GatePre<kHonkRelease>, &PePre<kHonkRelease>, false, false, false,
     false},
};

// The shape of kerfur_seat_probe's InstallWatch: a refused registration is asked again while the gate is not armed; a
// watch whose name resolved dead is said once, with the reason.
void InstallGate(int i) {
    Entry& e = g_entries[i];
    if (e.gateSettled) return;
    if (!e.gateRegistered) e.gateRegistered = sg::WatchName(*e.row, kTagBase + i, e.gate, nullptr);
    if (!e.gateRegistered) {
        if (sg::IsInstalled()) {
            e.gateSettled = true;
            UE_LOGE("[ATVG] watch '%s' NOT live: the gate refused the registration", e.label);
        }
        return;
    }
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(*e.row, kTagBase + i)) {
        e.gateSettled = e.gateLive = true;
        UE_LOGI("[ATVG] watch '%s' live", e.label);
    } else if (sg::NameWatchSettled(*e.row, kTagBase + i)) {
        e.gateSettled = true;
        UE_LOGE("[ATVG] watch '%s' NOT live: its name resolved dead (the class declares no such function, a native "
                "one, or the table is full)", e.label);
    }
}

// The observer needs the function resolved, which needs ATV_C loaded: asked once a second, as the class loads with the
// world and a miss walks the object array.
void InstallObservers() {
    static std::chrono::steady_clock::time_point next{};
    static int tries = 0;
    if (tries >= kPeMaxTries) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    next = now + kPeRetry;
    bool pending = false;
    for (int i = 0; i < kFnCount; ++i) {
        Entry& e = g_entries[i];
        if (e.peRegistered) continue;
        const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(*e.row);
        if (fn && GT::RegisterPreObserver(fn, e.pe)) {
            e.peRegistered = true;
            UE_LOGI("[ATVG] observer '%s' registered", e.label);
        } else {
            pending = true;
        }
    }
    if (pending && ++tries >= kPeMaxTries) {
        for (const Entry& e : g_entries)
            if (!e.peRegistered) UE_LOGE("[ATVG] observer '%s' NOT registered: the function never resolved", e.label);
    }
}

}  // namespace

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::atv_gate_probe);
    return s;
}

const char* NameOf(Fn fn) { return fn >= 0 && fn < kFnCount ? g_entries[fn].label : "?"; }

bool AllLive() {
    for (const Entry& e : g_entries)
        if (!e.gateLive || !e.peRegistered) return false;
    return true;
}

Counts Snapshot() { return g_counts; }

void ClearToolboxTrace() { g_toolboxHealth.clear(); }

std::vector<float> ToolboxTrace() { return g_toolboxHealth; }

void Install(coop::net::Session* session) {
    if (!Enabled()) return;
    g_session.store(session, std::memory_order_release);
    for (int i = 0; i < kFnCount; ++i) InstallGate(i);
    InstallObservers();
}

void OnDisconnect() {
    if (!Enabled()) return;
    uint32_t gate = 0, pe = 0;
    for (int i = 0; i < kFnCount; ++i) {
        gate += g_counts.gate[i];
        pe += g_counts.pe[i];
    }
    UE_LOGI("[ATVG] session end -- script_gate calls=%u pe calls=%u (pe lines dropped off the game thread=%u)", gate, pe,
            g_offThread.load(std::memory_order_relaxed));
    g_counts = Counts{};
    for (auto& row : g_lines)
        for (uint32_t& n : row) n = 0;
}

}  // namespace coop::dev::atv_gate_probe
