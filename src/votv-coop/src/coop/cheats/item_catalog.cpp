// coop/cheats/item_catalog.cpp -- see coop/cheats/item_catalog.h.

#include "coop/cheats/item_catalog.h"

#include "ue_wrap/actors/prop_gather.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <unordered_map>

namespace coop::cheats::item_catalog {
namespace {

namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;
namespace WI = ue_wrap::world_identity;

// The game's prop processor calls this on every actor that takes the int_coms interface, the gamemode among them,
// once the prop list is filled (it runs setPropNames, then the loop over those actors). So the list is read when the
// gamemode's own call arrives, the engine's signal that it is complete, and not by a guess at how long a fill takes.
UE_FACT_FN(kRowFilled, "item_catalog", L"mainGamemode_C", L"intComs_propRenderer_finishProps", Declared);
constexpr int kTag = 0x49434154;  // 'ICAT'

std::mutex g_mutex;
std::shared_ptr<const std::vector<std::string>> g_names;   // guarded by g_mutex
// The world generation the list was read in; 0 = none. A list of another world is not served: the game fills its
// list again in every world it loads.
std::atomic<uint32_t> g_builtGen{0};
std::atomic<uint32_t> g_askedGen{0};   // the generation RequestRefresh last posted a read for
bool g_armed = false;                  // game thread: the watch is registered and the gate held

// Game thread only: the lower-cased name to the list's own spelling.
std::unordered_map<std::wstring, std::wstring> g_byLower;

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool Ascii(const std::wstring& w) {
    if (w.empty()) return false;
    for (const wchar_t c : w)
        if (c < 0x21 || c > 0x7E) return false;
    return true;
}

// Read the game's list as it stands. False when it is not filled yet; the watch on the fill reads it then.
bool Refresh() {
    std::vector<std::wstring> raw;
    if (!ue_wrap::prop_gather::ListNames(raw) || raw.empty()) return false;
    std::unordered_map<std::wstring, std::wstring> byLower;
    size_t skipped = 0;
    for (const std::wstring& w : raw) {
        if (!Ascii(w)) { ++skipped; continue; }
        byLower.emplace(Lower(w), w);
    }
    auto names = std::make_shared<std::vector<std::string>>();
    names->reserve(byLower.size());
    for (const auto& kv : byLower) names->emplace_back(kv.second.begin(), kv.second.end());
    std::sort(names->begin(), names->end(), [](const std::string& a, const std::string& b) {
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });
    g_byLower = std::move(byLower);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_names = std::move(names);
    }
    g_builtGen.store(WI::Generation(), std::memory_order_release);
    UE_LOGI("cheats: the game lists %zu spawnable props (%zu names not plain text were left out)", g_byLower.size(),
            skipped);
    return true;
}

// The watch's post callback runs inside a Blueprint frame: the read waits for the game thread's next task.
void OnFilled(const sg::Call&) { GT::Post([] { Refresh(); }); }

// Hold the gate and watch the fill, once. A coop session holds the gate for its life; a solo game does not, so the
// catalog takes its own hold the first time it is asked. Game thread.
void Arm() {
    if (g_armed) return;
    g_armed = true;
    sg::Acquire("the item catalog");
    if (!sg::WatchName(kRowFilled, kTag, nullptr, &OnFilled))
        UE_LOGE("cheats: the watch on %ls::%ls was refused -- the list is read only when it is asked for",
                kRowFilled.Class(), kRowFilled.Name());
    sg::ResolvePendingNames();
}

bool Fresh() { return g_builtGen.load(std::memory_order_acquire) == WI::Generation(); }

}  // namespace

std::shared_ptr<const std::vector<std::string>> Names() {
    if (!Fresh()) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_names;
}

void RequestRefresh() {
    const uint32_t gen = WI::Generation();
    if (g_builtGen.load(std::memory_order_acquire) == gen) return;   // read in this world
    uint32_t asked = g_askedGen.load(std::memory_order_acquire);
    // One read for this world: if the game has not filled its list yet, the watch reads it at the fill.
    if (asked == gen || !g_askedGen.compare_exchange_strong(asked, gen)) return;
    GT::Post([] {
        Arm();
        Refresh();
    });
}

bool Resolve(std::string_view typed, std::wstring& canonical) {
    if (!Fresh()) {   // a request can arrive before anyone looked at the list in this world
        Arm();
        if (!Refresh()) return false;
    }
    if (typed.empty()) return false;
    const auto it = g_byLower.find(Lower(std::wstring(typed.begin(), typed.end())));
    if (it == g_byLower.end()) return false;
    canonical = it->second;
    return true;
}

}  // namespace coop::cheats::item_catalog
