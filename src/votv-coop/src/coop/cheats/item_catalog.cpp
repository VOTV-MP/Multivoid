// coop/cheats/item_catalog.cpp -- see coop/cheats/item_catalog.h.

#include "coop/cheats/item_catalog.h"

#include "ue_wrap/actors/prop_gather.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <unordered_map>

namespace coop::cheats::item_catalog {
namespace {

namespace GT = ue_wrap::game_thread;

std::mutex g_mutex;
std::shared_ptr<const std::vector<std::string>> g_names;   // guarded by g_mutex
std::atomic<bool> g_posted{false};                         // a refresh is queued or running
std::atomic<uint64_t> g_lastAskMs{0};
std::atomic<bool> g_settled{false};   // two reads in a row gave the same list: the game has finished filling it

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

void Refresh() {
    std::vector<std::wstring> raw;
    if (ue_wrap::prop_gather::ListNames(raw) && !raw.empty()) {
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
        // The game fills its list some seconds after the world loads, and a read can land mid-fill: the list is settled
        // once a read gives the same number of names as the one before.
        if (g_byLower.size() == byLower.size()) g_settled.store(true, std::memory_order_release);
        g_byLower = std::move(byLower);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_names = std::move(names);
        }
        UE_LOGI("cheats: the game lists %zu spawnable props (%zu names not plain text were left out)",
                g_byLower.size(), skipped);
    }
    g_posted.store(false, std::memory_order_release);
}

}  // namespace

std::shared_ptr<const std::vector<std::string>> Names() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_names;
}

void RequestRefresh() {
    if (g_settled.load(std::memory_order_acquire)) return;
    const uint64_t now = ::GetTickCount64();
    if (now - g_lastAskMs.load(std::memory_order_relaxed) < 2000) return;   // until settled: once in two seconds
    bool expected = false;
    if (!g_posted.compare_exchange_strong(expected, true)) return;
    g_lastAskMs.store(now, std::memory_order_relaxed);
    GT::Post([] { Refresh(); });
}

bool Resolve(std::string_view typed, std::wstring& canonical) {
    if (!g_settled.load(std::memory_order_acquire)) Refresh();   // a request can arrive before the list settled
    if (typed.empty()) return false;
    const auto it = g_byLower.find(Lower(std::wstring(typed.begin(), typed.end())));
    if (it == g_byLower.end()) return false;
    canonical = it->second;
    return true;
}

}  // namespace coop::cheats::item_catalog
