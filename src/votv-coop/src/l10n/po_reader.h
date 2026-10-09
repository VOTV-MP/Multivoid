// l10n/po_reader.h -- a gettext `.po` file, from its bytes to its entries and what was refused.
//
// The only parser of translator input, which is untrusted text: a pure function, no engine and no
// I/O, so the selftest drives it on in-memory files. The token set is gettext's: `msgctxt`, `msgid`,
// `msgid_plural`, `msgstr`, `msgstr[n]`, adjacent string lines concatenated, comment lines ignored, a
// `#, fuzzy` entry skipped (a line a translator has not confirmed -- msgfmt skips it too, where
// tinygettext reads it: dictionary_manager.cpp:45), the header read for its plural rule, charset and
// translators. A defect in one entry refuses that entry; a defect no entry boundary can contain (a
// string that never closes, a charset we do not read, bytes that are not UTF-8, a file over 1 MiB)
// refuses the file. An unknown escape refuses its entry where tinygettext warns and keeps it
// (po_parser.cpp:126-139): what a translator meant by it is not ours to guess.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace l10n::po {

inline constexpr size_t kMaxFileBytes  = 1u << 20;
inline constexpr size_t kMaxEntryBytes = 16u << 10;
inline constexpr size_t kMaxForms      = 8;

struct Entry {
    bool        hasCtx = false;
    std::string ctx;                  // msgctxt
    std::string id;                   // msgid
    bool        plural = false;
    std::string idPlural;             // msgid_plural
    std::vector<std::string> str;     // msgstr, or msgstr[0..n-1]
    int         line = 0;             // the msgid's line
};

struct Diag {
    int         line = 0;
    std::string what;
};

struct File {
    bool ok = false;                  // false: the whole file is refused, `refusal` says why
    Diag refusal;
    std::string pluralForms;          // the header's Plural-Forms value, "" if absent
    std::string language, lastTranslator, languageTeam;
    bool charsetAssumed = false;      // no charset, or the template's CHARSET placeholder: read as UTF-8
    std::vector<Entry> entries;       // translated entries only: no header, fuzzy, empty or refused one
    std::vector<Diag> refused;        // each refused entry, with its line
};

// Reads a whole file. An entry is translated when its msgstr (every msgstr[n] of a plural entry) is
// not empty; its conversions are checked against the msgid's here (l10n/printf_check.h), its count of
// plural forms against a rule by the catalogue, which knows which rule applies.
File Read(std::string_view bytes);

}  // namespace l10n::po
