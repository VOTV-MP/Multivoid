// l10n/locale_choice.cpp -- see l10n/locale_choice.h.

#include "locale_choice.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace l10n::detail {
namespace {

bool IsAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
char Upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

}  // namespace

std::string NormaliseTag(std::string_view tag) {
    // A gettext locale may carry an encoding or a modifier: `zh_CN.UTF-8`, `sr_RS@latin`.
    const size_t cut = tag.find_first_of(".@");
    if (cut != std::string_view::npos) tag = tag.substr(0, cut);
    while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) tag.remove_prefix(1);
    while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t')) tag.remove_suffix(1);
    std::vector<std::string_view> parts;
    size_t at = 0;
    while (at <= tag.size()) {
        size_t end = tag.find_first_of("-_", at);
        if (end == std::string_view::npos) end = tag.size();
        parts.push_back(tag.substr(at, end - at));
        at = end + 1;
    }
    if (parts.empty() || parts[0].size() < 2 || parts[0].size() > 3) return {};
    for (char c : parts[0])
        if (!IsAlpha(c)) return {};
    std::string lang;
    for (char c : parts[0]) lang.push_back(Lower(c));
    std::string script, region;
    for (size_t i = 1; i < parts.size(); ++i) {
        const std::string_view p = parts[i];
        if (p.size() == 1) break;   // a BCP-47 extension or private-use singleton: what follows is not a region
        bool alpha = !p.empty(), digits = !p.empty();
        for (char c : p) {
            alpha = alpha && IsAlpha(c);
            digits = digits && IsDigit(c);
        }
        if (script.empty() && region.empty() && p.size() == 4 && alpha) {
            for (char c : p) script.push_back(Lower(c));
        } else if (region.empty() && ((p.size() == 2 && alpha) || (p.size() == 3 && digits))) {
            for (char c : p) region.push_back(Upper(c));
        }
        // Variants and extensions say nothing a catalogue is chosen by.
    }
    if (region.empty() && lang == "zh") {
        if (script == "hans") region = "CN";
        else if (script == "hant") region = "TW";
    }
    return region.empty() ? lang : lang + "_" + region;
}

std::string WindowsDisplayLanguage() {
    ULONG count = 0, chars = 0;
    if (!::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &chars) || chars == 0) return {};
    std::vector<wchar_t> buf(chars);
    if (!::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &chars) || count == 0) return {};
    // A double-NUL-terminated list; the first entry is the language in use.
    std::string out;
    for (const wchar_t* p = buf.data(); *p; ++p) {
        if (*p > 0x7F) return {};   // a language name is ASCII
        out.push_back(static_cast<char>(*p));
    }
    return out;
}

}  // namespace l10n::detail
