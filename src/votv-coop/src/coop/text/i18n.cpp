// coop/text/i18n.cpp -- see coop/text/i18n.h.

#include "coop/text/i18n.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace coop::i18n {
namespace {

constexpr const char* kEnglish    = "en";
constexpr const char* kAutoToken  = "auto";
// The entry name a pack must use, and the only place the folder name is spelled: the loader and
// the README both point here, so a rename cannot leave the two disagreeing.
constexpr const wchar_t* kPackDirName = L"trans";
constexpr size_t kMaxMismatchLogLines = 8;

struct State {
    std::string tag = kEnglish;
    std::string packPath;                     // empty: the built-in English is in use
    std::vector<std::string> dirs;            // what was searched, for the log line
    std::unordered_map<std::string, std::string> utf8;
    std::unordered_map<std::wstring, std::wstring> wide;
    size_t mismatches = 0;
};

std::once_flag g_once;
State g_state;
// Set by Init, read by every lookup. Deliberately NOT a call_once trigger on the lookup path:
// see the note in the header -- a namespace-scope literal is evaluated while the DLL is loading.
std::atomic<bool> g_ready{false};

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

std::string LowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// The primary subtag, which is the second chance at a file name: a translator who writes
// zh.json covers every Chinese locale, and one who writes zh-CN.json covers only that one.
std::string MainSubtag(const std::string& tag) {
    const size_t dash = tag.find('-');
    return dash == std::string::npos ? std::string() : tag.substr(0, dash);
}

bool ReadWholeFile(const std::filesystem::path& path, std::string* out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

// A UTF-8 BOM is what Notepad is most likely to leave on a hand-edited pack, and nlohmann reads
// it as a parse error -- so the file would simply never apply, with nothing on screen to say why.
void StripUtf8Bom(std::string* text) {
    if (text->size() >= 3 && static_cast<unsigned char>((*text)[0]) == 0xEF &&
        static_cast<unsigned char>((*text)[1]) == 0xBB &&
        static_cast<unsigned char>((*text)[2]) == 0xBF)
        text->erase(0, 3);
}

std::string ConfiguredTag() {
    const std::string v = Trim(::coop::config::ResolveString(::coop::config_registry::rows::ui_language));
    if (v.empty() || LowerAscii(v) == kAutoToken) return std::string();
    return v;
}

// The tags to try, best first. An explicit ui.language overrides Windows entirely, so a player
// can pin a language their system does not name (and a tester can see English on a Chinese box).
std::vector<std::string> CandidateTags(const std::string& configured, const std::string& system) {
    std::vector<std::string> out;
    const std::string first = configured.empty() ? system : configured;
    if (first.empty()) return out;
    out.push_back(first);
    const std::string main = MainSubtag(first);
    if (!main.empty() && LowerAscii(main) != LowerAscii(first)) out.push_back(main);
    return out;
}

bool TryLoad(const std::filesystem::path& path, const std::string& tag, State* st) {
    std::string text;
    if (!ReadWholeFile(path, &text)) return false;
    StripUtf8Bom(&text);

    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        UE_LOGW("i18n: pack '%s' is not a JSON object -- ignored, the UI stays English",
                st->tag.empty() ? "?" : path.string().c_str());
        return false;
    }

    size_t kept = 0, empty = 0, skipped = 0, warned = 0;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_string()) { ++skipped; continue; }
        const std::string key = it.key();
        const std::string val = it.value().get<std::string>();
        if (key.empty()) { ++skipped; continue; }
        if (val.empty()) { ++empty; continue; }   // a blank value is an entry nobody wrote yet
        // A translation that changes the CONVERSION SET is refused, not repaired: more
        // conversions than the call site passes is undefined behaviour at the surface, and the
        // English line is both correct and self-announcing (the player sees which line is
        // untranslated instead of seeing the game misbehave).
        if (ConversionsOf(key) != ConversionsOf(val)) {
            ++st->mismatches;
            if (warned < kMaxMismatchLogLines) {
                ++warned;
                UE_LOGW("i18n: '%s' -> '%s' changes the conversions ('%s' vs '%s') -- that line "
                        "stays English", key.c_str(), val.c_str(), ConversionsOf(key).c_str(),
                        ConversionsOf(val).c_str());
            }
            continue;
        }
        st->utf8[key] = val;
        ++kept;
    }

    st->tag = tag;
    st->packPath = path.string();
    st->wide.clear();
    st->wide.reserve(st->utf8.size());
    for (const auto& kv : st->utf8)
        st->wide.emplace(coop::text::FromUtf8Lossy(kv.first.data(), kv.first.size()),
                         coop::text::FromUtf8Lossy(kv.second.data(), kv.second.size()));

    UE_LOGI("i18n: pack '%s' loaded for '%s' -- %zu entr%s kept, %zu blank, %zu skipped, %zu "
            "conversion mismatch(es)", st->packPath.c_str(), st->tag.c_str(), kept,
            kept == 1 ? "y" : "ies", empty, skipped, st->mismatches);
    return true;
}

void InitOnce() {
    const std::wstring exeDir = ue_wrap::paths::ExeDir();
    std::filesystem::path dir;
    if (!exeDir.empty()) {
        dir = std::filesystem::path(exeDir) / kPackDirName;
        g_state.dirs.push_back(coop::text::ToUtf8(dir.wstring()));
    }

    const std::string system = SystemTag();
    const std::string configured = ConfiguredTag();
    const std::vector<std::string> tags = CandidateTags(configured, system);

    std::error_code ec;
    for (const std::string& tag : tags) {
        if (dir.empty()) break;
        std::filesystem::path p = dir / (coop::text::FromUtf8Lossy(tag.data(), tag.size()) + L".json");
        if (!std::filesystem::is_regular_file(p, ec) || ec) continue;
        if (TryLoad(p, tag, &g_state)) break;
    }

    if (g_state.packPath.empty()) {
        g_state.tag = kEnglish;
        if (tags.empty() || LowerAscii(MainSubtag(system)) == kEnglish) {
            // The English install: nothing to report beyond where we looked.
            UE_LOGI("i18n: language=en (system='%s', ui.language='%s') -- built-in English, "
                    "dirs searched: %s", system.c_str(),
                    configured.empty() ? kAutoToken : configured.c_str(),
                    g_state.dirs.empty() ? "(none)" : g_state.dirs[0].c_str());
        } else {
            UE_LOGW("i18n: no pack for '%s' (system='%s', ui.language='%s') in %s -- the UI "
                    "stays English. A translator's file is '<LANG>.json' in that folder; see "
                    "i18n/README.md in the mod's source tree.", tags[0].c_str(), system.c_str(),
                    configured.empty() ? kAutoToken : configured.c_str(),
                    g_state.dirs.empty() ? "(no exe dir)" : g_state.dirs[0].c_str());
        }
    }
}

}  // namespace

void Init() {
    std::call_once(g_once, InitOnce);
    g_ready.store(true, std::memory_order_release);
}

const std::string& Language() { Init(); return g_state.tag; }

const std::string& PackPath() { Init(); return g_state.packPath; }

std::vector<std::string> SearchDirs() { Init(); return g_state.dirs; }

size_t EntryCount() { Init(); return g_state.utf8.size(); }

size_t ConversionMismatches() { Init(); return g_state.mismatches; }

const char* Tr(const char* english) {
    if (!english || !*english) return english ? english : "";
    if (!g_ready.load(std::memory_order_acquire)) return english;
    if (g_state.utf8.empty()) return english;
    const auto it = g_state.utf8.find(english);
    return it == g_state.utf8.end() ? english : it->second.c_str();
}

const wchar_t* TrW(const wchar_t* english) {
    if (!english || !*english) return english ? english : L"";
    if (!g_ready.load(std::memory_order_acquire)) return english;
    if (g_state.wide.empty()) return english;
    const auto it = g_state.wide.find(std::wstring(english));
    return it == g_state.wide.end() ? english : it->second.c_str();
}

bool LanguageUsesCjk() {
    Init();
    const std::string t = LowerAscii(g_state.tag);
    return t.rfind("zh", 0) == 0 || t.rfind("ja", 0) == 0 || t.rfind("ko", 0) == 0;
}

std::string SystemTag() {
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {};
    const int n = ::GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH);
    if (n <= 1) return kEnglish;
    return coop::text::ToUtf8(std::wstring(buf, static_cast<size_t>(n - 1)));
}

// The conversion SET, sorted so a translator may reorder them (the call site passes positional
// arguments, and the two English conversions in one line are not always in an order that reads
// naturally in another language). '%%' is a literal percent and carries no argument.
std::string ConversionsOf(const std::string& text) {
    std::vector<std::string> out;
    const char* kFlags = "-+ #0123456789.*hlLjztI";
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') continue;
        size_t j = i + 1;
        if (j < text.size() && text[j] == '%') { ++i; continue; }
        std::string spec = "%";
        while (j < text.size() && std::strchr(kFlags, text[j]) != nullptr) spec += text[j++];
        if (j < text.size()) spec += text[j];
        out.push_back(spec);
        i = j;
    }
    std::sort(out.begin(), out.end());
    std::string joined;
    for (const std::string& s : out) {
        if (!joined.empty()) joined += ' ';
        joined += s;
    }
    return joined;
}

}  // namespace coop::i18n
