// l10n/po_reader.cpp -- see l10n/po_reader.h.

#include "po_reader.h"

#include "printf_check.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

namespace l10n::po {
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// UTF-8 is validated by the platform's own strict decoder with no output buffer: no validator of
// ours, and no dependency on coop/text, which l10n may not reach.
bool ValidUtf8(const std::string& s) {
    if (s.empty()) return true;
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                                 nullptr, 0) > 0;
}

enum class StrResult { Ok, BadEscape, Unterminated };

// One quoted string starting at s[i] == '"'; appends its unescaped bytes to `out` and leaves `i` past
// the closing quote, even past a bad escape, so the line's syntax is still read and only the entry is
// refused. The escapes are gettext's: \n \t \r \" \\ \a \b \f \v, octal of one to three digits, hex of
// one or two digits; a longer hex, any other escape, or an escape whose value is 0 (it would cut the C
// string a lookup returns) is a bad escape.
StrResult ReadString(std::string_view s, size_t& i, std::string& out, const char** why) {
    ++i;   // the opening quote
    bool bad = false;
    auto badEscape = [&](const char* w) {
        if (!bad) *why = w;
        bad = true;
    };
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') return bad ? StrResult::BadEscape : StrResult::Ok;
        // A raw NUL would cut the C string a lookup returns, as the escape would.
        if (c == '\0') { badEscape("a NUL byte in a string"); continue; }
        if (c != '\\') { out.push_back(c); continue; }
        if (i >= s.size()) return StrResult::Unterminated;
        const char e = s[i++];
        switch (e) {
        case 'n': out.push_back('\n'); continue;
        case 't': out.push_back('\t'); continue;
        case 'r': out.push_back('\r'); continue;
        case '"': out.push_back('"'); continue;
        case '\\': out.push_back('\\'); continue;
        case 'a': out.push_back('\a'); continue;
        case 'b': out.push_back('\b'); continue;
        case 'f': out.push_back('\f'); continue;
        case 'v': out.push_back('\v'); continue;
        default: break;
        }
        unsigned v = 0;
        if (e >= '0' && e <= '7') {
            v = static_cast<unsigned>(e - '0');
            for (int d = 1; d < 3 && i < s.size() && s[i] >= '0' && s[i] <= '7'; ++d)
                v = v * 8 + static_cast<unsigned>(s[i++] - '0');
            if (v > 0xFF) { badEscape("an octal escape above \\377"); continue; }
        } else if (e == 'x') {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            int digits = 0;
            while (i < s.size() && hex(s[i]) >= 0) {
                ++digits;
                v = v * 16 + static_cast<unsigned>(hex(s[i++]));
            }
            if (digits > 2) { badEscape("a hex escape of more than two digits"); continue; }
            if (digits == 0) { badEscape("a \\x escape with no digit"); continue; }
        } else {
            badEscape("an escape outside gettext's set");
            continue;
        }
        if (v == 0) { badEscape("an escape whose value is 0"); continue; }
        out.push_back(static_cast<char>(v));
    }
    return StrResult::Unterminated;
}

// The value of a header field ("Plural-Forms: ..."), trimmed; the header is "Key: value\n" lines.
std::string HeaderField(const std::string& header, const char* key) {
    const size_t klen = std::strlen(key);
    size_t at = 0;
    while (at < header.size()) {
        size_t end = header.find('\n', at);
        if (end == std::string::npos) end = header.size();
        if (end - at > klen && header.compare(at, klen, key) == 0 && header[at + klen] == ':') {
            size_t b = at + klen + 1, e = end;
            while (b < e && IsSpace(header[b])) ++b;
            while (e > b && IsSpace(header[e - 1])) --e;
            return header.substr(b, e - b);
        }
        at = end + 1;
    }
    return {};
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// The entry being read. `field` is where a continuation string line goes.
struct Building {
    Entry e;
    bool  fuzzy = false;
    bool  any = false;        // a keyword has been read for it
    bool  sawId = false;      // its msgid has been read
    bool  sawStr = false;     // a msgstr has been read: the next msgctxt/msgid starts a new entry
    int   plainStr = 0;       // msgstr lines without an index
    int   indexedStr = 0;     // msgstr[n] lines
    bool  bad = false;        // refused: an escape, a size, an index
    const char* why = nullptr;
    int   line = 0;
    std::string* field = nullptr;
};

}  // namespace

File Read(std::string_view bytes) {
    File f;
    auto refuse = [&](int line, const char* what) {
        f.ok = false;
        f.refusal = Diag{line, what};
        f.entries.clear();
        f.refused.clear();
        return f;
    };
    if (bytes.size() > kMaxFileBytes) return refuse(0, "the file is over 1 MiB");
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.remove_prefix(3);   // a UTF-8 byte order mark

    std::string header;
    bool haveHeader = false;
    int headerLine = 0;
    int badLine = 0;   // the line a file-level refusal from finish() names
    bool pendingFuzzy = false;
    Building b;

    auto finish = [&]() -> const char* {
        if (!b.any) { b = Building{}; return nullptr; }
        Entry& e = b.e;
        size_t total = e.ctx.size() + e.id.size() + e.idPlural.size();
        for (const std::string& s : e.str) total += s.size();
        if (!b.bad && total > kMaxEntryBytes) { b.bad = true; b.why = "an entry over 16 KiB"; }
        for (const std::string* s : {&e.ctx, &e.id, &e.idPlural})
            if (!ValidUtf8(*s)) { badLine = b.line; return "bytes that are not UTF-8"; }
        for (const std::string& s : e.str)
            if (!ValidUtf8(s)) { badLine = b.line; return "bytes that are not UTF-8"; }
        if (!b.bad && e.str.empty()) { b.bad = true; b.why = "an entry with no msgstr"; }
        // A plural entry has a msgid_plural and only msgstr[n]; a singular one exactly one msgstr.
        const bool pluralShape = !e.idPlural.empty() && b.plainStr == 0 && b.indexedStr > 0;
        const bool singularShape = e.idPlural.empty() && b.indexedStr == 0 && b.plainStr == 1;
        if (!b.bad && !(e.plural ? pluralShape : singularShape)) {
            b.bad = true;
            b.why = "plural and singular forms mixed in one entry";
        }
        const bool isHeader = !e.hasCtx && e.id.empty() && !e.plural;
        if (isHeader) {
            if (!haveHeader && !b.bad) { header = e.str[0]; haveHeader = true; headerLine = b.line; }
        } else if (b.bad) {
            f.refused.push_back(Diag{b.line, b.why ? b.why : "a malformed entry"});
        } else if (!b.fuzzy) {
            bool allEmpty = true, anyEmpty = false;
            for (const std::string& s : e.str) {
                if (s.empty()) anyEmpty = true;
                else allEmpty = false;
            }
            if (allEmpty) {
                // no translation: the English is shown, and an override's empty entry masks nothing
            } else if (anyEmpty) {
                f.refused.push_back(Diag{b.line, "a plural entry with an empty form"});
            } else {
                // A msgid with a '%' is a format, used through l10n::Fmt: each translation carries its
                // conversions (printf_check). One without is plain text drawn as it is, and its
                // translation may hold no '%', so no translation can make a format of a plain line.
                namespace pc = printf_check;
                const bool format = e.id.find('%') != std::string::npos ||
                                    e.idPlural.find('%') != std::string::npos;
                const char* why = nullptr;
                bool good = true;
                if (format) {
                    pc::Parsed mid, other;
                    good = pc::Parse(e.id, &mid, &why);
                    if (good && e.plural)
                        good = pc::Parse(e.idPlural, &other, &why) && pc::Matches(mid, other, &why);
                    for (size_t k = 0; good && k < e.str.size(); ++k)
                        good = pc::Parse(e.str[k], &other, &why) && pc::Matches(mid, other, &why);
                } else {
                    for (const std::string& t : e.str)
                        if (t.find('%') != std::string::npos) {
                            good = false;
                            why = "a '%' in the translation of a line that has none";
                        }
                }
                if (good) f.entries.push_back(std::move(e));
                else f.refused.push_back(Diag{b.line, why ? why : "a conversion mismatch"});
            }
        }
        b = Building{};
        return nullptr;
    };

    int line = 0;
    size_t at = 0;
    while (at <= bytes.size()) {
        size_t end = bytes.find('\n', at);
        if (end == std::string_view::npos) end = bytes.size();
        std::string_view ln = bytes.substr(at, end - at);
        ++line;
        const size_t next = end + 1;
        size_t i = 0;
        while (i < ln.size() && IsSpace(ln[i])) ++i;
        if (i == ln.size()) { at = next; if (end == bytes.size()) break; continue; }

        if (ln[i] == '#') {
            // `#, flags`: the next entry is fuzzy when "fuzzy" is among them. Every other comment
            // kind -- references, extracted and translator comments, `#|` previous, `#~` obsolete --
            // is ignored.
            if (i + 1 < ln.size() && ln[i + 1] == ',') {
                if (b.sawStr) { if (const char* w = finish()) return refuse(badLine, w); }
                const std::string_view flags = ln.substr(i + 2);
                for (size_t p = 0; (p = flags.find("fuzzy", p)) != std::string_view::npos; p += 5) {
                    const bool l = p == 0 || flags[p - 1] == ' ' || flags[p - 1] == ',' || flags[p - 1] == '\t';
                    const bool r = p + 5 >= flags.size() || flags[p + 5] == ' ' || flags[p + 5] == ',' ||
                                   flags[p + 5] == '\r';
                    if (l && r) pendingFuzzy = true;
                }
            }
            at = next;
            if (end == bytes.size()) break;
            continue;
        }

        if (ln[i] == '"') {
            if (!b.field) return refuse(line, "a string line with no keyword before it");
            const char* why = nullptr;
            const StrResult r = ReadString(ln, i, *b.field, &why);
            if (r == StrResult::Unterminated) return refuse(line, "a string that never closes");
            if (r == StrResult::BadEscape && !b.bad) { b.bad = true; b.why = why; }
        } else {
            size_t k = i;
            while (k < ln.size() && !IsSpace(ln[k]) && ln[k] != '"') ++k;
            const std::string_view kw = ln.substr(i, k - i);
            std::string* target = nullptr;
            if (kw == "msgctxt" || kw == "msgid") {
                // A new entry: after a msgstr, or a second msgctxt, or a second msgid (an empty one
                // included) -- the last two leave an entry with no msgstr, which finish() refuses.
                if (b.sawStr || (kw == "msgctxt" && b.any) || (kw == "msgid" && b.sawId)) {
                    if (const char* w = finish()) return refuse(badLine, w);
                }
                if (!b.any) { b.fuzzy = pendingFuzzy; pendingFuzzy = false; b.line = line; }
                b.any = true;
                if (kw == "msgctxt") { b.e.hasCtx = true; target = &b.e.ctx; }
                else { target = &b.e.id; b.line = line; b.sawId = true; }
            } else if (kw == "msgid_plural") {
                if (!b.any) return refuse(line, "a msgid_plural with no msgid");
                b.e.plural = true;
                target = &b.e.idPlural;
            } else if (kw == "msgstr") {
                if (!b.any) return refuse(line, "a msgstr with no msgid");
                b.sawStr = true;
                ++b.plainStr;
                b.e.str.emplace_back();
                target = &b.e.str.back();
            } else if (kw.size() > 8 && kw.substr(0, 7) == "msgstr[" && kw.back() == ']') {
                if (!b.any) return refuse(line, "a msgstr with no msgid");
                b.sawStr = true;
                ++b.indexedStr;
                size_t idx = 0;
                bool num = true;
                for (char c : kw.substr(7, kw.size() - 8)) {
                    if (c < '0' || c > '9') { num = false; break; }
                    idx = idx * 10 + static_cast<size_t>(c - '0');
                    if (idx > kMaxForms) break;
                }
                if (!num || idx != b.e.str.size()) {
                    if (!b.bad) { b.bad = true; b.why = "plural forms out of order"; }
                } else if (idx >= kMaxForms) {
                    if (!b.bad) { b.bad = true; b.why = "more than 8 plural forms"; }
                }
                b.e.str.emplace_back();
                target = &b.e.str.back();
            } else {
                return refuse(line, "a line that is neither a keyword, a string nor a comment");
            }
            i = k;
            while (i < ln.size() && IsSpace(ln[i])) ++i;
            if (i >= ln.size() || ln[i] != '"') return refuse(line, "a keyword without its string");
            b.field = target;
            const char* why = nullptr;
            const StrResult r = ReadString(ln, i, *target, &why);
            if (r == StrResult::Unterminated) return refuse(line, "a string that never closes");
            if (r == StrResult::BadEscape && !b.bad) { b.bad = true; b.why = why; }
        }
        // gettext's lexer reads strings as tokens, so a second string on the same line joins the first.
        for (;;) {
            while (i < ln.size() && IsSpace(ln[i])) ++i;
            if (i >= ln.size() || ln[i] != '"') break;
            const char* why = nullptr;
            const StrResult r = ReadString(ln, i, *b.field, &why);
            if (r == StrResult::Unterminated) return refuse(line, "a string that never closes");
            if (r == StrResult::BadEscape && !b.bad) { b.bad = true; b.why = why; }
        }
        // Nothing but blanks may follow a string on its line.
        while (i < ln.size() && IsSpace(ln[i])) ++i;
        if (i != ln.size()) return refuse(line, "text after a string on its line");
        at = next;
        if (end == bytes.size()) break;
    }
    if (const char* w = finish()) return refuse(badLine, w);

    if (haveHeader) {
        f.pluralForms = HeaderField(header, "Plural-Forms");
        f.language = HeaderField(header, "Language");
        f.lastTranslator = HeaderField(header, "Last-Translator");
        f.languageTeam = HeaderField(header, "Language-Team");
        const std::string ct = HeaderField(header, "Content-Type");
        const size_t cs = ct.find("charset=");
        if (cs == std::string::npos) {
            f.charsetAssumed = true;
        } else {
            std::string_view v = std::string_view(ct).substr(cs + 8);
            const size_t semi = v.find(';');
            if (semi != std::string_view::npos) v = v.substr(0, semi);
            while (!v.empty() && IsSpace(v.back())) v.remove_suffix(1);
            if (v == "CHARSET") f.charsetAssumed = true;
            else if (!EqualsNoCase(v, "UTF-8") && !EqualsNoCase(v, "UTF8"))
                return refuse(headerLine, "a charset other than UTF-8");
        }
    } else {
        f.charsetAssumed = true;
    }
    f.ok = true;
    return f;
}

}  // namespace l10n::po
