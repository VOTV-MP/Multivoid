// coop/moderation/ban_list.cpp -- see coop/moderation/ban_list.h.

#include "coop/moderation/ban_list.h"

#include "coop/atomic_file/atomic_file.h"
#include "coop/mod_data/legacy_note.h"
#include "ue_wrap/core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>

namespace fs = std::filesystem;

namespace coop::ban_list {
namespace {

// The WRITE mutex (outer): the disk write, the folder and the read-only mark. The SET mutex
// (inner): the set and its address index. Anything taking both takes the write mutex first.
std::mutex g_writeMutex;
std::wstring g_serverDir;              // guarded by g_writeMutex
std::atomic<bool> g_readOnly{false};   // written under g_writeMutex

std::mutex g_setMutex;
std::vector<Entry> g_set;              // guarded by g_setMutex
AddressIndex g_index;                  // guarded by g_setMutex

// 32 hex digits, either case.
bool IsHex32(std::string_view s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

bool IsLowerHex32(std::string_view s) {
    if (!IsHex32(s)) return false;
    for (char c : s)
        if (c >= 'A' && c <= 'F') return false;
    return true;
}

// Copy `s` into a fixed field, cut on a UTF-8 boundary below the field's capacity.
template <size_t N>
void SetField(char (&dst)[N], const std::string& s) {
    const std::string cut = coop::text::CapUtf8Bytes(s, N - 1);
    std::memcpy(dst, cut.data(), cut.size());
    dst[cut.size()] = '\0';
}

Entry* FindById(std::vector<Entry>& set, std::string_view id) {
    for (auto& e : set)
        if (id == e.id) return &e;
    return nullptr;
}

// The first eight characters of an id for a log line (the id may not be NUL-terminated).
struct ShortId {
    char text[9] = {};
    explicit ShortId(std::string_view id) {
        std::memcpy(text, id.data(), std::min<size_t>(id.size(), 8));
    }
};

// The pieces of ParseBans, one per field kind. Each returns false when the field is present and
// of the wrong type; an absent field leaves `out` as it was.
bool ReadString(const nlohmann::json& rec, const char* key, std::string* out) {
    const auto it = rec.find(key);
    if (it == rec.end()) return true;
    if (!it->is_string()) return false;
    *out = it->get<std::string>();
    return true;
}

// Write the whole set to bans.json. Caller holds g_writeMutex and NOT g_setMutex: the set was
// copied under it and released, so the copy order is the write order and admission never waits on
// the disk. Memory-only (no folder) and read-only stores write nothing.
void WriteFile(const std::vector<Entry>& copy) {
    if (g_readOnly.load(std::memory_order_acquire)) {
        UE_LOGW("ban_list: the file is read-only this session -- this change holds until the host stops");
        return;
    }
    if (g_serverDir.empty()) return;
    const fs::path path = fs::path(g_serverDir) / L"bans.json";
    const std::string text = SerializeBans(copy);
    const atomic_file::Result r =
        atomic_file::Write(path, text, atomic_file::Mode::Replace, atomic_file::Sync::ToDisk);
    if (!r.ok())
        UE_LOGE("ban_list: could not write %ls (%s) -- the ban holds for this session only",
                path.c_str(), atomic_file::Describe(r).c_str());
}

}  // namespace

bool IsBannableAddress(std::string_view a) {
    if (a.empty() || a == "::" || a == "0.0.0.0") return false;
    if (a == "::1" || a.substr(0, 4) == "127.") return false;
    return true;
}

AddressIndex BuildAddressIndex(const std::vector<Entry>& set) {
    AddressIndex index;
    for (const auto& e : set)
        if (IsBannableAddress(e.address)) index.emplace(e.address, e.id);
    return index;
}

const std::string* LookupAddress(const AddressIndex& index, std::string_view address) {
    if (!IsBannableAddress(address)) return nullptr;
    const auto it = index.find(std::string(address));
    return it == index.end() ? nullptr : &it->second;
}

bool ParseBans(std::string_view json, std::vector<Entry>* out, std::vector<std::string>* problems,
               int* skipped) {
    using Json = nlohmann::json;
    out->clear();
    problems->clear();
    *skipped = 0;
    const Json root = Json::parse(json.begin(), json.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) return false;
    const auto bans = root.find("bans");
    if (bans == root.end() || !bans->is_array()) return false;

    int n = -1;
    for (const Json& rec : *bans) {
        ++n;
        auto skip = [&](const char* field) {
            ++*skipped;
            problems->push_back("record " + std::to_string(n) + " skipped (" + field + ")");
        };
        if (!rec.is_object()) { skip("record"); continue; }

        std::string id, name, address, reason;
        long long since = 0;
        {
            const auto it = rec.find("id");
            if (it == rec.end() || !it->is_string() || !IsHex32(it->get_ref<const std::string&>())) {
                skip("id");
                continue;
            }
            id = it->get<std::string>();
            for (char& c : id)
                if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        }
        if (!ReadString(rec, "name", &name)) { skip("name"); continue; }
        if (!ReadString(rec, "address", &address)) { skip("address"); continue; }
        if (!ReadString(rec, "reason", &reason)) { skip("reason"); continue; }
        {
            const auto it = rec.find("since");
            if (it != rec.end()) {
                if (!it->is_number_integer()) { skip("since"); continue; }
                since = it->get<long long>();
            }
        }

        Entry e;
        SetField(e.id, id);
        SetField(e.nick, name);
        SetField(e.address, address);
        SetField(e.reason, reason);
        e.bannedUnix = since;
        if (Entry* prior = FindById(*out, id)) {
            problems->push_back("record " + std::to_string(n) + " repeats an id");
            *prior = e;
        } else {
            out->push_back(e);
        }
    }
    return true;
}

std::string SerializeBans(const std::vector<Entry>& in) {
    using Json = nlohmann::ordered_json;
    Json arr = Json::array();
    for (const auto& e : in) {
        Json rec = Json::object();
        rec["id"] = std::string(e.id);
        rec["name"] = std::string(e.nick);
        rec["address"] = std::string(e.address);
        rec["reason"] = std::string(e.reason);
        rec["since"] = e.bannedUnix;
        arr.push_back(std::move(rec));
    }
    Json root = Json::object();
    root["bans"] = std::move(arr);
    return root.dump(2, ' ', false, Json::error_handler_t::replace);
}

void Load(const std::wstring& serverDir) {
    std::lock_guard<std::mutex> w(g_writeMutex);
    g_serverDir = serverDir;
    g_readOnly.store(false, std::memory_order_release);

    coop::legacy_note::Once(coop::legacy_note::LegacyFile::BanList, L"multivoid-banlist.txt");

    std::vector<Entry> loaded;
    if (!serverDir.empty()) {
        const fs::path path = fs::path(serverDir) / L"bans.json";
        std::error_code ec;
        const bool exists = fs::exists(path, ec);
        if (!ec && exists) {
            std::string text;
            bool readable = false;
            {
                std::ifstream f(path, std::ios::binary);
                if (f) {
                    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                    readable = !f.bad();
                }
            }
            std::vector<std::string> problems;
            int skipped = 0;
            if (!readable || !ParseBans(text, &loaded, &problems, &skipped)) {
                loaded.clear();
                g_readOnly.store(true, std::memory_order_release);
                UE_LOGE("ban_list: %ls is unreadable -- no bans are enforced, and the file is not "
                        "rewritten this session", path.c_str());
            } else {
                for (const auto& p : problems) UE_LOGW("ban_list: %ls: %s", path.c_str(), p.c_str());
                if (skipped > 0) {
                    g_readOnly.store(true, std::memory_order_release);
                    UE_LOGE("ban_list: %ls has records this build cannot read -- the bans it could "
                            "read are enforced, and the file is not rewritten this session",
                            path.c_str());
                }
                UE_LOGI("ban_list: loaded %zu ban(s) from %ls", loaded.size(), path.c_str());
            }
        } else if (ec) {
            g_readOnly.store(true, std::memory_order_release);
            UE_LOGE("ban_list: %ls is unreadable -- no bans are enforced, and the file is not "
                    "rewritten this session", path.c_str());
        }
    }

    std::lock_guard<std::mutex> s(g_setMutex);
    g_set = std::move(loaded);
    g_index = BuildAddressIndex(g_set);
}

bool IsReadOnly() { return g_readOnly.load(std::memory_order_acquire); }

bool IsBanned(std::string_view playerId, std::string_view address, char* reasonOut, int reasonLen) {
    if (reasonOut && reasonLen > 0) reasonOut[0] = '\0';
    const char* key = nullptr;
    {
        std::lock_guard<std::mutex> s(g_setMutex);
        const Entry* hit = FindById(g_set, playerId);
        if (hit) {
            key = "id";
        } else if (const std::string* id = LookupAddress(g_index, address)) {
            hit = FindById(g_set, *id);
            if (hit) key = "address";
        }
        if (!hit) return false;
        if (reasonOut && reasonLen > 0)
            std::snprintf(reasonOut, static_cast<size_t>(reasonLen), "%s", hit->reason);
    }
    UE_LOGI("ban_list: refused %s... by %s", ShortId(playerId).text, key);
    return true;
}

bool Add(const char* playerId, const char* nick, const char* address, const char* reason) {
    if (!playerId || !IsLowerHex32(playerId)) {
        UE_LOGW("ban_list: refused a ban for a malformed id");
        return false;
    }
    Entry e;
    SetField(e.id, playerId);
    SetField(e.nick, nick ? nick : "");
    if (address && IsBannableAddress(address)) SetField(e.address, address);
    SetField(e.reason, reason ? reason : "");
    e.bannedUnix = static_cast<long long>(::time(nullptr));

    std::lock_guard<std::mutex> w(g_writeMutex);
    std::vector<Entry> copy;
    {
        std::lock_guard<std::mutex> s(g_setMutex);
        if (Entry* prior = FindById(g_set, playerId)) *prior = e;
        else g_set.push_back(e);
        g_index = BuildAddressIndex(g_set);
        copy = g_set;
    }
    WriteFile(copy);
    return true;
}

bool Remove(const char* playerId) {
    if (!playerId || !playerId[0]) return false;
    std::lock_guard<std::mutex> w(g_writeMutex);
    std::vector<Entry> copy;
    bool removed = false;
    size_t remaining = 0;
    {
        std::lock_guard<std::mutex> s(g_setMutex);
        const auto it = std::find_if(g_set.begin(), g_set.end(),
                                     [&](const Entry& e) { return std::string_view(playerId) == e.id; });
        if (it != g_set.end()) {
            g_set.erase(it);
            g_index = BuildAddressIndex(g_set);
            copy = g_set;
            removed = true;
            remaining = g_set.size();
        }
    }
    if (!removed) return false;
    UE_LOGI("ban_list: unbanned %.8s... -- %zu remain", playerId, remaining);
    WriteFile(copy);
    return true;
}

std::vector<std::string> IdsWithPrefix(std::string_view hexPrefix) {
    std::string prefix(hexPrefix);
    for (char& c : prefix)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    std::vector<std::string> out;
    std::lock_guard<std::mutex> s(g_setMutex);
    for (const Entry& e : g_set)
        if (std::string_view(e.id).substr(0, prefix.size()) == prefix) out.emplace_back(e.id);
    return out;
}

void GetSnapshot(std::vector<Entry>& out) {
    {
        std::lock_guard<std::mutex> s(g_setMutex);
        out = g_set;
    }
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return a.bannedUnix > b.bannedUnix;  // most recent ban first
    });
}

}  // namespace coop::ban_list
