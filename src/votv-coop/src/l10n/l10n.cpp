// l10n/l10n.cpp -- the lookups, Label, and Init: the language chosen and its catalogue built once.

#include "l10n/l10n.h"

#include "catalog.h"
#include "locale_choice.h"
#include "po_reader.h"
#include "quiet.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

namespace l10n {
namespace {

using detail::Catalog;
using detail::Entry;

// Published once by Init and never freed: every pointer a lookup returned stays valid for the process.
std::atomic<const Catalog*> g_cat{nullptr};
std::string g_locale;   // written before g_cat's release store, read after its acquire load

const Entry* Lookup(const char* ctx, const char* msgid) {
    const Catalog* c = g_cat.load(std::memory_order_acquire);
    return (c && msgid) ? c->Find(ctx, msgid) : nullptr;   // an entry always holds a form
}

const char* Plural(const char* ctx, const char* singular, const char* plural, unsigned long long n) {
    const Catalog* c = g_cat.load(std::memory_order_acquire);
    if (c && singular) {
        const Entry* e = c->Find(ctx, singular);
        // A plural msgid answered by a singular entry is no answer: the English rule applies.
        if (e && e->plural) return e->forms[c->SelectForm(*e, n)].c_str();
    }
    return n == 1 ? singular : plural;
}

// An RCDATA resource of our own module, by name; the bytes live in the mapped image for the process.
std::string_view Resource(const std::wstring& name) {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&Resource), &self);
    if (!self) return {};
    HRSRC res = ::FindResourceW(self, name.c_str(), reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!res) return {};
    HGLOBAL glob = ::LoadResource(self, res);
    const DWORD size = ::SizeofResource(self, res);
    const void* p = glob ? ::LockResource(glob) : nullptr;
    if (!p || size == 0) return {};
    return std::string_view(static_cast<const char*>(p), size);
}

// A file of at most one byte past the reader's cap, so an oversize file is refused by the reader. Shared
// for writing and deleting too: a translator's editor holds the file open while the game starts. A file
// that exists but cannot be read is said; one that is not there is the usual case and says nothing.
bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
            UE_LOGW("l10n: '%ls' exists but could not be opened (error %lu) -- it is not used", path.c_str(), err);
        return false;
    }
    out.resize(po::kMaxFileBytes + 1);
    DWORD got = 0;
    const BOOL ok = ::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // ASCII tags

// Header text from a translator's file, for a log line: control bytes become spaces.
std::string Printable(const std::string& s) {
    std::string out = s.substr(0, 120);
    for (char& c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
    return out;
}

std::string ResourceName(const std::string& level) {
    std::string n = "L10N_";
    for (char c : level) n.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c);
    return n;
}

// One source into the catalogue, its outcome logged. Returns the rule its plural entries use.
int Load(Catalog& cat, const std::string& name, const std::string& level, std::string_view bytes,
         int fallbackRule, size_t& refusedTotal, std::string& summary) {
    const po::File f = po::Read(bytes);
    if (!f.ok) {
        UE_LOGW("l10n: %s refused whole (line %d: %s) -- nothing of it is used", name.c_str(), f.refusal.line,
                f.refusal.what.c_str());
        summary += ", " + name + " refused";
        return fallbackRule;
    }
    if (f.charsetAssumed) UE_LOGI("l10n: %s names no charset -- read as UTF-8", name.c_str());
    if (!f.language.empty() && detail::NormaliseTag(f.language) != level)
        UE_LOGW("l10n: %s says Language '%s' but is loaded as %s", name.c_str(), Printable(f.language).c_str(),
                level.c_str());
    detail::SourceReport rep;
    rep.name = name;
    const int rule = cat.Insert(f, fallbackRule, rep);
    if (!rep.pluralNote.empty())
        UE_LOGW("l10n: %s has a Plural-Forms that does not compile (%s) -- %s", name.c_str(),
                rep.pluralNote.c_str(), rule >= 0 ? "its plural entries take the pack's rule"
                                                  : "its plural entries are refused");
    // One line per refused entry up to a cap, then the count: a broken file must not flood the log.
    constexpr size_t kSaidMax = 20;
    for (size_t i = 0; i < rep.refused.size() && i < kSaidMax; ++i)
        UE_LOGW("l10n: %s line %d refused: %s", name.c_str(), rep.refused[i].line, rep.refused[i].what.c_str());
    if (rep.refused.size() > kSaidMax)
        UE_LOGW("l10n: %s -- %zu more refused entries not listed", name.c_str(), rep.refused.size() - kSaidMax);
    refusedTotal += rep.refused.size();
    summary += ", " + name + " " + std::to_string(rep.inserted) + " entries";
    if (!rep.translator.empty()) summary += " (translator " + Printable(rep.translator) + ")";
    if (!rep.team.empty()) summary += " (team " + Printable(rep.team) + ")";
    return rule;
}

}  // namespace

const char* T(const char* msgid) {
    const Entry* e = Lookup(nullptr, msgid);
    return e ? e->forms[0].c_str() : msgid;
}

const char* Tc(const char* ctx, const char* msgid) {
    const Entry* e = Lookup(ctx, msgid);
    return e ? e->forms[0].c_str() : msgid;
}

const char* Tn(const char* singular, const char* plural, unsigned long long n) {
    return Plural(nullptr, singular, plural, n);
}

Label::Label(const char* text, const char* id) {
    if (!text) text = "";
    if (!id) id = "";
    const size_t idLen = std::strlen(id);
    if (idLen > 64) {
        // An id is a literal of ours; one this long is a defect at its call site, said once.
        static std::atomic<bool> s_said{false};
        if (!detail::g_selftestQuiet.load(std::memory_order_relaxed) && !s_said.exchange(true))
            UE_LOGE("l10n: a widget id of %zu bytes ('%.32s') -- keep ids short", idLen, id);
    }
    // The id is kept whole and the text is cut to fit, at a UTF-8 boundary: a cut id would collide.
    const size_t tail = 3 + idLen;
    size_t room = tail < sizeof(buf_) - 1 ? sizeof(buf_) - 1 - tail : 0;
    size_t n = std::strlen(text);
    if (n > room) {
        n = room;
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;   // never split a sequence
    }
    std::memcpy(buf_, text, n);
    size_t pos = n;
    const size_t idKept = idLen < sizeof(buf_) - 1 - pos - 3 ? idLen : sizeof(buf_) - 1 - pos - 3;
    std::memcpy(buf_ + pos, "###", 3);
    pos += 3;
    std::memcpy(buf_ + pos, id, idKept);
    pos += idKept;
    buf_[pos] = '\0';
}

const char* ActiveLocale() {
    return g_cat.load(std::memory_order_acquire) ? g_locale.c_str() : "";
}

bool WasFound(const char* ctx, const char* msgid) {
    const Catalog* c = g_cat.load(std::memory_order_acquire);
    const Entry* e = (c && msgid) ? c->Peek(ctx, msgid) : nullptr;
    return e && e->hit.load(std::memory_order_relaxed);
}

size_t FoundCount() {
    const Catalog* c = g_cat.load(std::memory_order_acquire);
    return c ? c->FoundCount() : 0;
}

void Init(const char* language) {
    static std::atomic<bool> s_done{false};
    if (s_done.exchange(true)) return;
    std::string requested = (language && *language) ? language : "auto";
    std::string lower;
    for (char ch : requested) lower.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
    std::string why, tag;
    if (lower == "auto") {
        const std::string win = detail::WindowsDisplayLanguage();
        tag = detail::NormaliseTag(win);
        why = "auto: Windows display language " + (win.empty() ? std::string("unknown") : win);
    } else {
        tag = detail::NormaliseTag(requested);
        why = "ui.language=" + requested;
    }
    if (tag.empty()) {
        UE_LOGI("l10n: language English (%s) -- not a language tag, no catalogue", why.c_str());
        return;
    }
    // The levels, in rising priority: the language alone, then with its region.
    const size_t us = tag.find('_');
    std::string levels[2];
    int nLevels = 0;
    levels[nLevels++] = tag.substr(0, us);
    if (us != std::string::npos) levels[nLevels++] = tag;

    auto* cat = new Catalog();   // immortal: see g_cat
    size_t refused = 0;
    std::string summary;
    // An exe dir that could not be read would put the folder at the drive's root: no overrides then.
    const std::wstring exeDir = ue_wrap::paths::ExeDir();
    const std::wstring dir = exeDir.empty() ? std::wstring() : exeDir + L"\\multivoid_locale\\";
    for (int i = 0; i < nLevels; ++i) {
        const std::string& level = levels[i];
        int packRule = -1;
        const std::string_view pack = Resource(Widen(ResourceName(level)));
        if (!pack.empty()) packRule = Load(*cat, "embedded pack " + level, level, pack, -1, refused, summary);
        else summary += ", embedded pack " + level + " none";
        std::string bytes;
        if (!dir.empty() && ReadFileBytes(dir + Widen(level) + L".po", bytes))
            Load(*cat, "override " + level + ".po", level, bytes, packRule, refused, summary);
        else
            summary += ", override " + level + ".po none";
    }
    // D8's line: what was chosen, why, and what every source contributed.
    const char* sources = summary.empty() ? "" : summary.c_str() + 2;
    if (cat->Size() == 0) {
        UE_LOGI("l10n: language English (%s) -- no catalogue for %s: %s", why.c_str(), tag.c_str(), sources);
        delete cat;
        return;
    }
    g_locale = tag;
    g_cat.store(cat, std::memory_order_release);
    UE_LOGI("l10n: language %s (%s) -- %s, %zu refused", tag.c_str(), why.c_str(), sources, refused);
}

}  // namespace l10n
