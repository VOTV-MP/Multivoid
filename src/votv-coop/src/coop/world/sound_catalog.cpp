// coop/world/sound_catalog.cpp -- see coop/world/sound_catalog.h.

#include "coop/world/sound_catalog.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"  // the sound drill's red arm of the index check

#include "ue_wrap/core/log.h"
#include "ue_wrap/world/physsound_table.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::sound_catalog {
namespace {

// The paths of the two sounds that are not in the physics table, and the attenuations the callers pass, as the three
// Blueprint calls name them (mainPlayer_C::attack, kocker_C::knock, church_C's ubergraph).
constexpr const wchar_t* kKnockPath = L"/Game/audio/effects/metalDoorKnock_Cue.metalDoorKnock_Cue";
constexpr const wchar_t* kBellPath = L"/Game/audio/effects/churchbell.churchbell";
constexpr const wchar_t* kAttDefault = L"/Game/audio/misc/att_default.att_default";
constexpr const wchar_t* kAttDefaultNoAo = L"/Game/audio/misc/att_default_noAO.att_default_noAO";

constexpr uint64_t kRetryMs = 3000;

std::vector<Entry> g_entries;
std::unordered_map<std::wstring, uint16_t> g_index;
uint32_t g_digest = 0;
bool g_built = false;
bool g_mismatch = false;   // the table's layout or data is not what the reader expects: not tried again
uint64_t g_lastTryMs = 0;
std::string g_lastWhy;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// FNV-1a over the sorted paths, ASCII, joined by newlines.
uint32_t DigestOf(const std::vector<Entry>& entries) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) h = (h ^ static_cast<uint8_t>('\n')) * 16777619u;
        for (const wchar_t c : entries[i].path) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    return h;
}

bool IndexCheckOff() {
    static const bool s = coop::config::ResolveFlag(coop::config_registry::rows::sound_index_check_off);
    return s;
}

}  // namespace

bool Ready() {
    if (g_built || g_mismatch) return g_built;
    const uint64_t now = NowMs();
    if (g_lastTryMs && now - g_lastTryMs < kRetryMs) return false;
    g_lastTryMs = now;

    std::vector<std::wstring> cues;
    std::string why;
    const ue_wrap::physsound_table::Read read = ue_wrap::physsound_table::ReadImpactSounds(cues, why);
    if (read == ue_wrap::physsound_table::Read::Mismatch || (read == ue_wrap::physsound_table::Read::Ok && cues.empty())) {
        // A verdict about the table's layout or data: no retry changes it, so this peer sends and plays nothing, said once.
        g_mismatch = true;
        UE_LOGE("sound_catalog: the game's table does not read as expected -- %s; no sound is relayed by this peer",
                why.empty() ? "it names no impact sound" : why.c_str());
        return false;
    }
    if (read == ue_wrap::physsound_table::Read::NotYet) {
        if (why != g_lastWhy) {
            g_lastWhy = why;
            UE_LOGW("sound_catalog: not built yet -- %s (tried again when a sound needs it)", why.c_str());
        }
        return false;
    }
    std::vector<Entry> entries;
    entries.reserve(cues.size() + 2);
    for (const std::wstring& c : cues) entries.push_back({c, kAttack, kAttDefault, 1.f, 1.f, 1.f, 1.f});
    entries.push_back({kKnockPath, kKnock, kAttDefaultNoAo, 1.f, 1.f, 0.5f, 0.5f});
    entries.push_back({kBellPath, kBell, nullptr, 1.f, 1.f, 0.8f, 1.f});
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    // A path named twice would give one sound two indexes: not a table to speak by.
    if (std::adjacent_find(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.path == b.path; }) !=
        entries.end()) {
        g_mismatch = true;
        UE_LOGE("sound_catalog: a sound is named twice in the game's table; no sound is relayed by this peer");
        return false;
    }
    g_entries = std::move(entries);
    g_index.clear();
    for (size_t i = 0; i < g_entries.size(); ++i) g_index.emplace(g_entries[i].path, static_cast<uint16_t>(i + 1));
    g_digest = DigestOf(g_entries);
    g_built = true;
    UE_LOGI("sound_catalog: n=%zu digest=0x%08X (%zu impact sounds, the knock and the bell)", g_entries.size(), g_digest,
            cues.size());
    return true;
}

uint32_t Digest() { return g_built ? g_digest : 0; }
size_t Size() { return g_built ? g_entries.size() : 0; }

uint16_t IndexOf(const std::wstring& path) {
    if (!g_built) return 0;
    const auto it = g_index.find(path);
    return it == g_index.end() ? 0 : it->second;
}

const Entry* Find(uint16_t index) {
    if (!g_built || index == 0 || index > g_entries.size()) return nullptr;
    return &g_entries[index - 1];
}

uint16_t FirstFor(uint8_t caller) {
    if (!g_built) return 0;
    for (size_t i = 0; i < g_entries.size(); ++i)
        if (g_entries[i].callers & caller) return static_cast<uint16_t>(i + 1);
    return 0;
}

Resolve Lookup(uint32_t senderDigest, uint16_t index, const Entry*& entry) {
    entry = nullptr;
    if (!Ready()) return Resolve::NotReady;
    if (senderDigest != g_digest) return Resolve::Digest;
    entry = Find(index);
    // The drill's red arm: an index past the table is taken modulo its size, so the entry played is a wrong one.
    if (!entry && index > 0 && IndexCheckOff())
        entry = &g_entries[static_cast<size_t>(index - 1) % g_entries.size()];
    return entry ? Resolve::Ok : Resolve::Index;
}

void Clamp(const Entry& e, float& volume, float& pitch) {
    if (!(volume >= e.volumeMin)) volume = e.volumeMin;
    if (volume > e.volumeMax) volume = e.volumeMax;
    if (!(pitch >= e.pitchMin)) pitch = e.pitchMin;
    if (pitch > e.pitchMax) pitch = e.pitchMax;
}

}  // namespace coop::sound_catalog
