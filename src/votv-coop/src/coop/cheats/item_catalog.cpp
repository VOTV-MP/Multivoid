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
bool g_watched = false;                // game thread: the watch on the fill is registered
bool g_watchLive = false;              // game thread: it was accepted, so a fill can end a wait for the list
bool g_held = false;                   // game thread: this catalog holds the script gate, for the world it waits on

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
    if (g_held) {   // the list of this world is read: the gate is needed again only at the next world's
        g_held = false;
        sg::Release("the item catalog");
    }
    UE_LOGI("cheats: the game lists %zu spawnable props (%zu names not plain text were left out)", g_byLower.size(),
            skipped);
    return true;
}

// The watch's post callback runs inside a Blueprint frame: the read waits for the game thread's next task.
void OnFilled(const sg::Call&) { GT::Post([] { Refresh(); }); }

// A world change ends the wait for the list of the world that went: the gate is given back, and the next ask of the new
// world takes it again if that world's list is not filled either.
void OnWorldChange() noexcept {
    if (!g_held) return;
    g_held = false;
    sg::Release("the item catalog");
}

// Watch the fill (registered once) and hold the gate until this world's list is read. A coop session holds the gate
// for its life; a solo game does not, so the catalog holds it from the first ask of each world until the read, which
// gives it back (Refresh), or until the world changes. With the watch refused no fill can end the wait, so no hold is
// taken. Game thread.
void Arm() {
    if (!g_watched) {
        g_watched = true;
        g_watchLive = sg::WatchName(kRowFilled, kTag, nullptr, &OnFilled);
        if (!g_watchLive)
            UE_LOGE("cheats: the watch on %ls::%ls was refused -- the list is read only when it is asked for",
                    kRowFilled.Class(), kRowFilled.Name());
        sg::ResolvePendingNames();
        if (!WI::AddWorldChangeListener(&OnWorldChange))
            UE_LOGE("cheats: the world-change list is full -- the catalog's gate hold ends only at its next read");
    }
    if (g_watchLive && !g_held) {
        g_held = true;
        sg::Acquire("the item catalog");
    }
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
        if (!Refresh()) Arm();   // a list already filled needs no gate; one not filled is read at the fill
    });
}

Lookup Resolve(std::string_view typed, std::wstring& canonical) {
    if (!Fresh()) {   // a request can arrive before anyone looked at the list in this world
        if (!Refresh()) {
            Arm();
            return Lookup::Unfilled;
        }
    }
    if (typed.empty()) return Lookup::Unknown;
    const auto it = g_byLower.find(Lower(std::wstring(typed.begin(), typed.end())));
    if (it == g_byLower.end()) return Lookup::Unknown;
    canonical = it->second;
    return Lookup::Found;
}

}  // namespace coop::cheats::item_catalog
