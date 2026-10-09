// l10n/l10n.cpp -- the lookups, Label, and Init: the language chosen and its catalogue built once.

#include "l10n/l10n.h"

#include "catalog.h"
#include "locale_choice.h"
#include "l10n/po_reader.h"

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
    if (!c || !msgid) return nullptr;
    const Entry* e = c->Find(ctx, msgid);
    return (e && !e->forms.empty()) ? e : nullptr;
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

// A file of at most one byte past the reader's cap, so an oversize file is refused by the reader.
bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.resize(po::kMaxFileBytes + 1);
    DWORD got = 0;
    const BOOL ok = ::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // ASCII tags

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
        summary += "; " + name + " refused";
        return fallbackRule;
    }
    if (f.charsetAssumed) UE_LOGI("l10n: %s names no charset -- read as UTF-8", name.c_str());
    if (!f.language.empty() && detail::NormaliseTag(f.language) != level)
        UE_LOGW("l10n: %s says Language '%s' but is loaded as %s", name.c_str(), f.language.c_str(), level.c_str());
    detail::SourceReport rep;
    rep.name = name;
    const int rule = cat.Insert(f, fallbackRule, rep);
    if (!rep.pluralNote.empty() && rule >= 0 && rule == fallbackRule)
        UE_LOGI("l10n: %s has no usable Plural-Forms (%s) -- its plural entries take the pack's rule",
                name.c_str(), rep.pluralNote.c_str());
    for (const po::Diag& d : rep.refused)
        UE_LOGW("l10n: %s line %d refused: %s", name.c_str(), d.line, d.what.c_str());
    refusedTotal += rep.refused.size();
    summary += "; " + name + " " + std::to_string(rep.inserted) + " entries";
    if (!rep.translator.empty()) summary += " (translator " + rep.translator + ")";
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

const char* Tcn(const char* ctx, const char* singular, const char* plural, unsigned long long n) {
    return Plural(ctx, singular, plural, n);
}

Label::Label(const char* text, const char* id) {
    if (!text) text = "";
    if (!id) id = "";
    const size_t idLen = std::strlen(id);
    if (idLen > 64) {
        // An id is a literal of ours; one this long is a defect at its call site, said once.
        static std::atomic<bool> s_said{false};
        if (!s_said.exchange(true)) UE_LOGE("l10n: a widget id of %zu bytes ('%.32s') -- keep ids short", idLen, id);
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
    const std::string requested = (language && *language) ? language : "auto";
    std::string why, tag;
    if (requested == "auto") {
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
    const std::wstring dir = ue_wrap::paths::ExeDir() + L"\\multivoid_locale\\";
    for (int i = 0; i < nLevels; ++i) {
        const std::string& level = levels[i];
        int packRule = -1;
        const std::string_view pack = Resource(Widen(ResourceName(level)));
        if (!pack.empty())
            packRule = Load(*cat, "embedded pack " + level, level, pack, -1, refused, summary);
        std::string bytes;
        if (ReadFileBytes(dir + Widen(level) + L".po", bytes))
            Load(*cat, "override " + level + ".po", level, bytes, packRule, refused, summary);
    }
    if (cat->Size() == 0) {
        UE_LOGI("l10n: language English (%s) -- no catalogue for %s%s", why.c_str(), tag.c_str(),
                summary.empty() ? ", none embedded or in multivoid_locale" : summary.c_str());
        delete cat;
        return;
    }
    g_locale = tag;
    g_cat.store(cat, std::memory_order_release);
    UE_LOGI("l10n: language %s (%s)%s, %zu refused", tag.c_str(), why.c_str(), summary.c_str(), refused);
}

}  // namespace l10n
