// coop/dev/dial_probe.cpp -- [dev] the signal desk's dials turned by the clients (VOTVCOOP_DIAL_PROBE=1 on every peer).
//
// The desk (analogDScreenTest_C) integrates DL_FrFilterOffset and DL_poFilterOffset by their speeds each tick while the
// dial's filter is on (calculate_download), and each notch of the mouse wheel on a dial adds a step to its speed
// (addFilterParam). Every peer starts its clock once it sees VOTVCOOP_DIAL_PEERS players (default 2). A CLIENT with
// VOTVCOOP_DIAL_SCRIPT=a or b turns dials: it switches both filters on at 10 s, then each turn is notches of
// VOTVCOOP_DIAL_STEP (default 2) on the speed, 80 ms apart: six up, 1.5 s held, six back to 0, then 1.5 s on a
// one-notch nudge undone 150 ms later. Script a turns the frequency dial at 12 s and the polarity dial at 28 s; script b
// the polarity dial at 20 s and the frequency dial at 28 s, the dial a turned first, while a turns the other. EVERY
// PEER, every 20 ms from 10 s to 40 s: the wall clock and the desk's two speeds and offsets; at 42 s the DeskInput
// deltas it sent and received, by field (`[DIAL]`).

#include "coop/dev/dial_probe.h"

#include "coop/config/config.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/desk/console_desk.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::dial_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace CD = ue_wrap::console_desk;

uint64_t Now() { return ::GetTickCount64(); }

// The wall clock in milliseconds: the rig's peers share the machine's, so their lines lay side by side.
uint64_t WallMs() {
    FILETIME ft{};
    ::GetSystemTimePreciseAsFileTime(&ft);
    return (((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000ull) % 10000000ull;
}

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_DIAL_PROBE") == "1";
    return on;
}

const std::string& Script() {
    static const std::string s = coop::config::ReadEnv("VOTVCOOP_DIAL_SCRIPT");
    return s;
}

int EnvInt(const char* name, int def) {
    const std::string v = coop::config::ReadEnv(name);
    return v.empty() ? def : std::atoi(v.c_str());
}

float* FloatField(const wchar_t* name) {
    void* const desk = CD::Instance();
    const int32_t off = desk ? R::FindPropertyOffset(R::ClassOf(desk), name) : -1;
    return off >= 0 ? reinterpret_cast<float*>(static_cast<char*>(desk) + off) : nullptr;
}

bool SetBool(const wchar_t* name, bool v) {
    void* const desk = CD::Instance();
    int32_t off = -1;
    uint8_t mask = 0;
    if (!desk || !R::FindBoolProperty(R::ClassOf(desk), name, off, mask)) return false;
    uint8_t& b = static_cast<uint8_t*>(desk)[off];
    b = v ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
    return true;
}

// One notch on a dial: its speed moved by `by`, as addFilterParam adds a step.
void Notch(int dial, float by, const char* what) {
    float* const speed = FloatField(dial == 0 ? L"DL_FrFilterSpeed" : L"DL_poFilterSpeed");
    if (!speed) return;
    *speed += by;
    UE_LOGI("[DIAL] script %s: %s dial=%s speed=%.1f wall=%llu", Script().c_str(), what, dial == 0 ? "fr" : "po", *speed,
            static_cast<unsigned long long>(WallMs()));
}

struct Step {
    uint64_t atMs;
    int dial;
    float by;
    const char* what;
};

// A turn of one dial: six notches up 80 ms apart, held 1.5 s, six back down, then a one-notch nudge undone 150 ms on.
void AddTurn(std::vector<Step>& out, uint64_t at, int dial, float step) {
    for (int i = 0; i < 6; ++i) out.push_back({at + 80u * i, dial, step, i == 0 ? "turn up" : "notch up"});
    const uint64_t down = at + 400u + 1500u;
    for (int i = 0; i < 6; ++i) out.push_back({down + 80u * i, dial, -step, i == 5 ? "turn stop" : "notch down"});
    const uint64_t nudge = down + 400u + 1500u;
    out.push_back({nudge, dial, step, "nudge up"});
    out.push_back({nudge + 150u, dial, -step, "nudge stop"});
}

std::vector<Step> g_steps;
size_t g_next = 0;
uint64_t g_seenAt = 0, g_reportAt = 0;
bool g_filtersOn = false, g_counted = false;

int PlayersSeen() {
    auto& reg = coop::players::Registry::Get();
    int n = 1;
    for (uint8_t sl = 0; sl < coop::net::kMaxPeers; ++sl) {
        if (sl == reg.LocalPeerId()) continue;
        if (coop::RemotePlayer* rp = reg.Puppet(sl); rp && rp->GetActor()) ++n;
    }
    return n;
}

void LogCounts(bool host) {
    using F = coop::net::DeskInputField;
    uint32_t sent[static_cast<int>(F::Count)] = {};
    uint32_t recv[static_cast<int>(F::Count)] = {};
    coop::desk_input_sync::dev::Counts(sent, recv);
    auto at = [](const uint32_t* c, F f) { return c[static_cast<int>(f)]; };
    uint32_t sentAll = 0, recvAll = 0;
    for (int i = 0; i < static_cast<int>(F::Count); ++i) {
        sentAll += sent[i];
        recvAll += recv[i];
    }
    UE_LOGI("[DIAL] counts %s: sent fr_speed=%u po_speed=%u fr_offset=%u po_offset=%u all=%u; received fr_speed=%u "
            "po_speed=%u fr_offset=%u po_offset=%u all=%u",
            host ? "host" : Script().empty() ? "client" : Script().c_str(), at(sent, F::FrFilterSpeed),
            at(sent, F::PoFilterSpeed), at(sent, F::FrFilterOffset), at(sent, F::PoFilterOffset), sentAll,
            at(recv, F::FrFilterSpeed), at(recv, F::PoFilterSpeed), at(recv, F::FrFilterOffset),
            at(recv, F::PoFilterOffset), recvAll);
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_seenAt) {
        if (PlayersSeen() < EnvInt("VOTVCOOP_DIAL_PEERS", 2)) return;
        g_seenAt = Now();
        const float step = static_cast<float>(EnvInt("VOTVCOOP_DIAL_STEP", 2));
        if (Script() == "a") {
            AddTurn(g_steps, 12000u, 0, step);
            AddTurn(g_steps, 28000u, 1, step);
        } else if (Script() == "b") {
            AddTurn(g_steps, 20000u, 1, step);
            AddTurn(g_steps, 28000u, 0, step);
        }
        std::sort(g_steps.begin(), g_steps.end(), [](const Step& x, const Step& y) { return x.atMs < y.atMs; });
        UE_LOGI("[DIAL] probe armed role=%s script=%s fast_off=%d at wall=%llu", host ? "host" : "client",
                Script().empty() ? "-" : Script().c_str(),
                coop::config::ReadEnv("VOTVCOOP_DESK_DIAL_FAST_OFF") == "1" ? 1 : 0,
                static_cast<unsigned long long>(WallMs()));
    }
    const uint64_t t = Now() - g_seenAt;
    if (!Script().empty() && !g_filtersOn && t >= 10000u) {
        g_filtersOn = true;
        const bool fr = SetBool(L"DL_activeFrFilter", true), po = SetBool(L"DL_activePoFilter", true);
        UE_LOGI("[DIAL] script %s: both filters switched on (ok=%d)", Script().c_str(), fr && po ? 1 : 0);
    }
    while (g_next < g_steps.size() && t >= g_steps[g_next].atMs) {
        const Step& s = g_steps[g_next++];
        Notch(s.dial, s.by, s.what);
    }
    if (t >= 42000u && !g_counted) {
        g_counted = true;
        LogCounts(host);
    }
    if (t < 10000u || t > 40000u || Now() < g_reportAt) return;
    g_reportAt = Now() + 20;
    const float* const frS = FloatField(L"DL_FrFilterSpeed");
    const float* const poS = FloatField(L"DL_poFilterSpeed");
    const float* const frO = FloatField(L"DL_FrFilterOffset");
    const float* const poO = FloatField(L"DL_poFilterOffset");
    UE_LOGI("[DIAL] wall=%llu fr_speed=%.1f fr=%.3f po_speed=%.1f po=%.3f", static_cast<unsigned long long>(WallMs()),
            frS ? *frS : -1.f, frO ? *frO : -1.f, poS ? *poS : -1.f, poO ? *poO : -1.f);
}

}  // namespace coop::dev::dial_probe
