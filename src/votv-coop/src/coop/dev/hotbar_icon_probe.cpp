// coop/dev/hotbar_icon_probe.cpp -- see coop/dev/hotbar_icon_probe.h.

#include "coop/dev/hotbar_icon_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/hotbar/icons.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::hotbar_icon_probe {
namespace {

namespace R   = ue_wrap::reflection;
namespace HB  = ue_wrap::hotbar;
namespace INV = ue_wrap::inventory;
namespace sg  = ue_wrap::script_gate;

// 8 Hz: fast enough to place the icon tables' arrival against the bar's last rebuild, cheap
// enough that it is not a per-frame reader. Every read goes through ue_wrap::hotbar.
constexpr int kPollTicks = 15;
constexpr int kWatchTag  = 0x0F121;
constexpr const wchar_t* kRefreshVerb = L"updateSlotInv";

std::chrono::steady_clock::time_point g_bootT0 = std::chrono::steady_clock::now();

uint64_t SinceBootMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_bootT0).count());
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (const wchar_t c : w) s.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
    return s;
}

std::string SlotLine(const HB::State& s) {
    std::string out;
    for (int i = 0; i < 10; ++i) {
        if (i) out += " ";
        const std::wstring t = HB::SlotTexture(s, i);
        out += t.empty() ? "-" : Narrow(t);
    }
    return out;
}

std::string Summary(const HB::State& s) {
    return "slotInv=" + std::to_string(s.slots) + " slotTexts=" + std::to_string(s.slotTexts) +
           " propNames=" + std::to_string(s.iconNames) +
           " texs_GAMEINST=" + std::to_string(s.iconTextures) +
           " fins=" + std::to_string(s.rendererPhases) +
           " carried=" + std::to_string(s.carried);
}

// ---- the refresh verb's own dispatches ---------------------------------------------------------
//
// A world load ends in one long game-thread body, and the pump does not drain while it runs
// (docs/coop-dispatch-visibility.md, the pump rule, which measures its tail at 5.6 to 6.8 s on a
// joining client). So no tick of ours -- world-up-gated or not -- reliably gets a sample while the
// game is loading, and the first cut of this probe proved it by landing its earliest sample well
// after the save was loaded, with the bar already broken. The seam that fires INSIDE that body is
// the VM's own script loop. `updateSlotInv` is called from ten Blueprint sites -- the game mode,
// the player and the inventory screen -- and every one of them is a Blueprint-to-Blueprint call
// that ProcessEvent never sees; the watch is by NAME, so it catches all ten.
int g_calls = 0;

sg::Verdict OnRefreshPre(const sg::Call& c) {
    ++g_calls;
    // Read HERE, not deferred to the next tick: the tick is exactly what a world load blocks, and
    // an instrument must not schedule its own reading against the verb it is reading. For the same
    // reason no task batch has drained the object index inside the load, and the reading finds the
    // gamemode through it: the probe drains it first.
    ue_wrap::object_index::Drain();
    HB::State s;
    const bool have = HB::Read(s);
    UE_LOGI("hotbar_probe: VERB #%d t=%llums %s caller=%ls", g_calls,
            static_cast<unsigned long long>(SinceBootMs()),
            have ? Summary(s).c_str() : "(not resolvable yet)",
            c.callerFunction ? R::ToString(R::NameOf(c.callerFunction)).c_str() : L"<ProcessEvent>");
    if (have && s.slots > 0)
        UE_LOGI("hotbar_probe:   VERB #%d found: %s", g_calls, SlotLine(s).c_str());
    return sg::Verdict::Run;
}

void OnRefreshPost(const sg::Call&) {
    ue_wrap::object_index::Drain();
    HB::State s;
    if (!HB::Read(s) || s.slots <= 0) return;
    UE_LOGI("hotbar_probe:   VERB #%d left: %s", g_calls, SlotLine(s).c_str());
}

// Armed from the FIRST tick, before any world exists, because the dispatch this exists to catch
// happens during the load. A dev probe holding the gate for the process is the cost of seeing
// inside a load; a hold, so no other holder's release turns it off under the probe.
void EnsureWatch() {
    static bool s_armed = false;
    if (!s_armed) {
        if (!sg::IsInstalled()) return;
        static bool s_held = false;  // the watch below may retry; the hold is taken once
        if (!s_held) { sg::Acquire("the hotbar icon probe"); s_held = true; }
        if (!sg::WatchName(kRefreshVerb, kWatchTag, OnRefreshPre, OnRefreshPost)) return;
        s_armed = true;
        UE_LOGI("hotbar_probe: watching %ls at the script loop (registered; name resolve pending)",
                kRefreshVerb);
    }
    static bool s_live = false;
    if (s_live) return;
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(kRefreshVerb, kWatchTag)) {
        s_live = true;
        UE_LOGI("hotbar_probe: %ls watch is LIVE at t=%llums", kRefreshVerb,
                static_cast<unsigned long long>(SinceBootMs()));
    }
}

}  // namespace

void Tick() {
    static const bool s_on = ::coop::config::ResolveFlag(::coop::config_registry::rows::hotbar_icon_probe);
    if (!s_on) return;

    static int s_ticks = 0;
    if (++s_ticks < kPollTicks) return;
    s_ticks = 0;

    // Before anything world-shaped: the watch has to be live while the menu still runs, because
    // the dispatch it catches happens inside the load body.
    EnsureWatch();

    HB::State s;
    if (!HB::Read(s)) return;

    // The clock starts at the first readable sample: what is being measured is a sequence of
    // transitions around the world coming up, so the anchor is the world, not boot.
    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_t0{};
    if (s_t0 == Clock::time_point{}) s_t0 = Clock::now();
    const uint64_t elapsedMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s_t0).count());

    const std::string slotLine = SlotLine(s);
    const std::string now = Summary(s) + " | " + slotLine;
    static std::string s_last;
    if (now == s_last) return;
    const bool first = s_last.empty();
    s_last = now;

    UE_LOGI("hotbar_probe: %s t=%llums %s", first ? "FIRST" : "CHANGE",
            static_cast<unsigned long long>(elapsedMs), Summary(s).c_str());
    UE_LOGI("hotbar_probe:   slots: %s", slotLine.c_str());
}

}  // namespace coop::dev::hotbar_icon_probe
