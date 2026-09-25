// coop/dev/init_seam_probe.cpp -- [dev] every keyed Init body the gate sees. See the header.
#include "coop/dev/init_seam_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace coop::dev::init_seam_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace WI = ue_wrap::world_identity;
using Clock = std::chrono::steady_clock;

const wchar_t* const kInitName = L"Init";
constexpr int kTag = 0x49534550;  // 'ISEP'

// One kind of call: the body's owner, its caller, nested inside another Init of the object, key unset.
using Kind = std::tuple<std::wstring, std::wstring, bool, bool>;
std::map<Kind, std::uint64_t> g_counts;
std::uint64_t g_total = 0;
// The Init bodies of other classes that ProcessEvent dispatched, by owning class: each one a watch that
// keeps the ProcessEvent set passes to a keyed-lineage check, which pays class lookups while a keyed
// base class is unresolved.
std::map<std::wstring, std::uint64_t> g_otherViaPe;
int g_samplesLeft = 40;   // the first calls in full

// The objects whose Init bodies are running on the game thread, innermost last. The gate pairs a
// body's pre and post unless a consumer cancels it or a fault is absorbed; this probe cancels nothing,
// so a stray entry can only come from a fault, and the next world change clears the list.
std::vector<void*> g_running;

bool Keyed(void* obj) { return obj && ue_wrap::prop::IsClassKeyedInteractable(R::ClassOf(obj)); }

std::wstring NameOrNull(void* obj) { return obj ? R::ToString(R::NameOf(obj)) : L"<ProcessEvent>"; }

SG::Verdict Pre(const SG::Call& call) {
    if (Keyed(call.object)) g_running.push_back(call.object);
    return SG::Verdict::Run;
}

void Post(const SG::Call& call) {
    if (!Keyed(call.object)) {
        if (!call.callerFunction) ++g_otherViaPe[NameOrNull(R::OuterOf(call.function))];
        return;
    }
    if (!g_running.empty() && g_running.back() == call.object) g_running.pop_back();
    const bool nested = std::find(g_running.begin(), g_running.end(), call.object) != g_running.end();
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(call.object);
    const bool keyUnset = key.empty() || key == L"None";
    const std::wstring owner = NameOrNull(R::OuterOf(call.function));
    const std::wstring caller = NameOrNull(call.callerFunction);
    ++g_counts[Kind{owner, caller, nested, keyUnset}];
    ++g_total;
    if (g_samplesLeft > 0) {
        --g_samplesLeft;
        UE_LOGI("init_seam: %ls on %p (%ls) from %ls, depth %d, %s, key '%ls'%s", owner.c_str(),
                call.object, R::ClassNameOf(call.object).c_str(), caller.c_str(), call.depth,
                nested ? "NESTED in another Init of the object" : "outermost", key.c_str(),
                call.fromOurCode ? ", our dispatch" : "");
    }
}

void Dump(const char* why) {
    std::vector<std::pair<std::uint64_t, Kind>> rows;
    for (const auto& [k, n] : g_counts) rows.push_back({n, k});
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    UE_LOGI("init_seam: %s -- %llu keyed Init bodies, %zu kinds", why,
            static_cast<unsigned long long>(g_total), rows.size());
    // The first 40 kinds, and every kind dispatched through ProcessEvent however rare: those are the
    // calls a per-class ProcessEvent observer sees, and their sum is what such an observer counts.
    int shown = 0;
    std::uint64_t viaProcessEvent = 0;
    for (const auto& [n, k] : rows) {
        const bool pe = std::get<1>(k) == L"<ProcessEvent>";
        if (pe) viaProcessEvent += n;
        if (++shown > 40 && !pe) continue;
        UE_LOGI("init_seam:   %6llu  owner=%ls caller=%ls %s%s", static_cast<unsigned long long>(n),
                std::get<0>(k).c_str(), std::get<1>(k).c_str(), std::get<2>(k) ? "NESTED " : "",
                std::get<3>(k) ? "key-unset" : "keyed");
    }
    // The gate sees the game thread only; its tripwire counts a watched body anywhere else, so a zero
    // here says no Init body ran off it either.
    UE_LOGI("init_seam: %s -- %llu of the %llu dispatched through ProcessEvent; watched bodies off the game "
            "thread: %llu", why, static_cast<unsigned long long>(viaProcessEvent),
            static_cast<unsigned long long>(g_total), SG::GetStats().offGameThread);
    std::uint64_t other = 0;
    for (const auto& [owner, n] : g_otherViaPe) other += n;
    UE_LOGI("init_seam: %s -- %llu Init bodies of classes outside the keyed lineage came through "
            "ProcessEvent, of %zu owning classes", why, static_cast<unsigned long long>(other),
            g_otherViaPe.size());
    for (const auto& [owner, n] : g_otherViaPe)
        UE_LOGI("init_seam:   %6llu  other owner=%ls", static_cast<unsigned long long>(n), owner.c_str());
}

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::init_seam_probe);
    if (!s_on) return;
    static bool s_watched = false;
    if (!s_watched && SG::IsInstalled()) {
        s_watched = true;
        SG::Acquire("init_seam_probe");
        SG::WatchName(kInitName, kTag, &Pre, &Post);
    }
    static std::uint32_t s_gen = 0;
    static Clock::time_point s_last = Clock::now();
    const std::uint32_t gen = WI::Generation();
    const auto now = Clock::now();
    if (gen != s_gen) {
        if (s_gen != 0) Dump("world change");
        s_gen = gen;
        g_running.clear();
        s_last = now;
    } else if (now - s_last >= std::chrono::seconds(30)) {
        s_last = now;
        Dump("every 30 s");
    }
}

}  // namespace coop::dev::init_seam_probe
