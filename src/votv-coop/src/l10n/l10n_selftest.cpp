// l10n/l10n_selftest.cpp -- l10n::RunSelftest: the reader, the plural evaluator, the conversion grammar,
// the formatter, Label, the language tag and the catalogue's merge, all on in-memory input.
//
// Pure: no engine, no file, no catalogue published. The plural forms it expects come from
// plural_vectors.inc, which an independent evaluator wrote, so a defect of ours is not mirrored in the
// expectation. Every pack embedded in this build is read too, and must load with nothing refused.

#include "l10n/l10n.h"

#include "catalog.h"
#include "locale_choice.h"
#include "plural_expr.h"
#include "po_reader.h"
#include "printf_check.h"
#include "quiet.h"

#include "ue_wrap/core/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace l10n {

namespace detail {
std::atomic<bool> g_selftestQuiet{false};
}  // namespace detail

namespace {

#include "plural_vectors.inc"

struct Checks {
    int pass = 0, total = 0;
    void Ok(bool cond, const char* what) {
        ++total;
        if (cond) ++pass;
        else UE_LOGE("l10n selftest: FAIL -- %s", what);
    }
};

void Plurals(Checks& c, bool injectRed) {
    constexpr size_t nRules = sizeof(kRules) / sizeof(kRules[0]);
    plural::Rule rules[nRules];
    bool compiled = true;
    for (size_t i = 0; i < nRules; ++i) compiled = rules[i].Compile(kRules[i], nullptr) && compiled;
    c.Ok(compiled, "every shipped plural rule compiles");
    bool all = true;
    for (size_t k = 0; k < sizeof(kVectors) / sizeof(kVectors[0]); ++k) {
        const Vector& v = kVectors[k];
        unsigned want = v.form;
        // The red arm: one expected form made wrong, so the verdict hook is seen failing once.
        if (injectRed && k == 0) want = want + 1;
        if (rules[v.rule].Select(v.n) != want) {
            all = false;
            UE_LOGE("l10n selftest: FAIL -- rule %u at n=%llu took form %u, not %u", v.rule, v.n,
                    rules[v.rule].Select(v.n), want);
        }
    }
    c.Ok(all, "every plural vector selects the form the independent evaluator gave");
    bool refused = true;
    for (const char* r : kRefusedRules) {
        plural::Rule x;
        if (x.Compile(r, nullptr)) {
            refused = false;
            UE_LOGE("l10n selftest: FAIL -- a header that must be refused compiled: '%.60s'", r);
        }
    }
    c.Ok(refused, "every malformed or hostile plural header is refused");
    plural::Rule z;
    c.Ok(z.Compile("nplurals=2; plural=n/0;", nullptr) && z.Select(5) == 0, "a division by zero is form 0");
    c.Ok(z.Compile("nplurals=2; plural=n;", nullptr) && z.Select(7) == 0, "a form at or above nplurals is form 0");
}

bool Accepts(const char* f) {
    printf_check::Parsed p;
    return printf_check::Parse(f, &p, nullptr);
}

bool Match(const char* msgid, const char* tr) {
    printf_check::Parsed a, b;
    return printf_check::Parse(msgid, &a, nullptr) && printf_check::Parse(tr, &b, nullptr) &&
           printf_check::Matches(a, b, nullptr);
}

void Conversions(Checks& c) {
    c.Ok(Accepts("%d of %s, %.1f%% at %zu, %llu, %08x, %-5s"), "the accepted conversions parse");
    c.Ok(!Accepts("%n"), "%n is refused");
    c.Ok(!Accepts("%*d") && !Accepts("%.*f"), "a '*' width or precision is refused");
    c.Ok(!Accepts("%s and %1$s"), "mixed numbering is refused");
    c.Ok(!Accepts("%2$s only"), "a numbering gap is refused");
    c.Ok(!Accepts("%1$s %1$s"), "an argument read twice is refused");
    c.Ok(!Accepts("%ls") && !Accepts("%c") && !Accepts("%hd") && !Accepts("%lf"), "%ls, %c, %hd and %lf are refused");
    c.Ok(!Accepts("%.3s") && !Accepts("%05s") && !Accepts("%+u") && !Accepts("%#d"), "flags C leaves undefined are refused");
    c.Ok(!Accepts("%123d") && !Accepts("100% sure"), "a three-digit width and a bare '%' are refused");
    c.Ok(!Accepts("%--5d") && !Accepts("%00x") && Accepts("%-08.2f"), "a flag repeated is refused, distinct flags are not");
    c.Ok(Match("%1$s took %2$d", "%2$d by %1$s"), "a reordered translation matches");
    c.Ok(Match("%d items", "%i items") && Match("%5.2f", "%.1f"), "d and i are one type; width and precision may differ");
    c.Ok(!Match("%s took %d", "%s took %s") && !Match("%d", "%ld"), "a type or length change is refused");
    c.Ok(!Match("%s and %s", "%s"), "a missing conversion is refused");
    c.Ok(!Match("volume %.2fx", "volume %1$.2fx") && Match("%1$s took %2$d", "%s took %d"),
         "a numbered translation of an unnumbered msgid is refused; the reverse is accepted");
}

void Formatter(Checks& c) {
    char buf[64];
    c.Ok(Fmt(buf, sizeof(buf), "%2$s, %1$s", "a", "b") == 4 && std::strcmp(buf, "b, a") == 0, "Fmt reorders");
    c.Ok(Fmt(buf, sizeof(buf), "%d%% of %llu", 50, 7ull) > 0 && std::strcmp(buf, "50% of 7") == 0, "Fmt formats");
    Span sp;
    c.Ok(FmtSpan(buf, sizeof(buf), &sp, "%2$s greets %1$s.", "Nick", "Bob") > 0 && sp.begin == 11 && sp.len == 4,
         "FmtSpan reports where argument 1 landed");
    // A cut never splits a sequence: U+4E2D is three bytes, and only two of them fit after "ab".
    char small[5];
    const int n = Fmt(small, sizeof(small), "ab%s", "\xE4\xB8\xAD");
    c.Ok(n == 2 && std::strcmp(small, "ab") == 0, "a cut keeps whole UTF-8 sequences");
    c.Ok(FmtSpan(small, sizeof(small), &sp, "abc%s", "Nick") == 4 && sp.begin == 3 && sp.len == 1,
         "a span the cut reaches is clipped");
    c.Ok(FmtSpan(small, sizeof(small), &sp, "abcde%s", "Nick") == 4 && sp.begin == -1,
         "a span the cut removes whole is no span");
    c.Ok(Fmt(buf, sizeof(buf), "%s and %1$s", "x") == -1 && buf[0] == '\0', "a refused format writes nothing");
    const Label l("Apply", "apply");
    c.Ok(std::strcmp(l, "Apply###apply") == 0, "Label composes text and id");
    std::string longText(300, 'x');
    const Label cut(longText.c_str(), "keep_me");
    const size_t len = std::strlen(cut);
    c.Ok(len == 255 && std::strcmp(static_cast<const char*>(cut) + len - 10, "###keep_me") == 0,
         "a long label is cut in its text and keeps its id");
    // 84 three-byte characters are 252 bytes; with "###id" (5) the text has 250 bytes of room, which
    // ends inside the 84th, so the text keeps 83 of them.
    std::string han;
    for (int i = 0; i < 84; ++i) han += "\xE4\xB8\xAD";
    const Label hanCut(han.c_str(), "id");
    c.Ok(std::strlen(hanCut) == 83 * 3 + 5 && std::strcmp(static_cast<const char*>(hanCut) + 83 * 3, "###id") == 0,
         "a label's cut never splits a character");
    const std::string longId(100, 'i');
    const Label idLong("x", longId.c_str());
    c.Ok(std::strlen(idLong) == 1 + 3 + 100, "an id longer than 64 bytes is still kept whole");
}

void Tags(Checks& c) {
    using detail::NormaliseTag;
    c.Ok(NormaliseTag("zh-CN") == "zh_CN" && NormaliseTag("zh_CN.UTF-8") == "zh_CN", "zh-CN is zh_CN");
    c.Ok(NormaliseTag("zh-Hans-CN") == "zh_CN" && NormaliseTag("zh-Hans") == "zh_CN" &&
         NormaliseTag("zh-Hant-TW") == "zh_TW" && NormaliseTag("zh-Hant") == "zh_TW", "Chinese scripts pick a region");
    c.Ok(NormaliseTag("pt-BR") == "pt_BR" && NormaliseTag("en-US") == "en_US" && NormaliseTag("ru") == "ru" &&
         NormaliseTag("sr-Latn") == "sr" && NormaliseTag("es-419") == "es_419", "regions and scripts elsewhere");
    c.Ok(NormaliseTag("").empty() && NormaliseTag("x").empty() && NormaliseTag("12-AB").empty() &&
         NormaliseTag("auto!").empty(), "garbage is no tag");
    c.Ok(NormaliseTag("en-u-ca-gregory") == "en" && NormaliseTag(" zh_CN ") == "zh_CN",
         "an extension is no region, and blanks are trimmed");
}

void Reader(Checks& c) {
    const char* kGood =
        "\xEF\xBB\xBF# a translator comment\n"
        "msgid \"\"\nmsgstr \"\"\n\"Language: ru\\n\"\n\"Plural-Forms: nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : "
        "n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);\\n\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\n"
        "#: ui/a.cpp\nmsgid \"Apply\"\nmsgstr \"\\u041f\"\n\n"                  // an escape outside the set: refused
        "msgctxt \"f1\"\nmsgid \"Rules\"\nmsgstr \"Rul\" \"es\\t\\a\\101\\x42\"\n\n"   // adjacent strings, escapes
        "#, fuzzy\nmsgid \"Fuzzy\"\nmsgstr \"F\"\n\n"                           // skipped
        "msgid \"Empty\"\nmsgstr \"\"\n\n"                                    // untranslated
        "msgid \"%llu peer\"\nmsgid_plural \"%llu peers\"\nmsgstr[0] \"%llu a\"\nmsgstr[1] \"%llu b\"\n"
        "msgstr[2] \"%llu c\"\n\n"
        "msgid \"%llu x\"\nmsgid_plural \"%llu xs\"\nmsgstr[0] \"%llu\"\nmsgstr[1] \"\"\nmsgstr[2] \"%llu\"\n\n"   // half
        "msgid \"%1$s took %2$d\"\nmsgstr \"%2$d by %1$s\"\n\n"
        "msgid \"%s took\"\nmsgstr \"%d\"\n\n"                                // a type change: refused
        "msgid \"Plain\"\nmsgstr \"100%\"\n\n"                                // a '%' in a plain line: refused
        "msgid \"Nul\"\nmsgstr \"a\\0b\"\n\n"                                 // a NUL escape: refused
        "#~ msgid \"Old\"\n#~ msgstr \"Alt\"\n";
    const po::File f = po::Read(kGood);
    if (!f.ok) UE_LOGE("l10n selftest: the well-formed file was refused at line %d: %s", f.refusal.line, f.refusal.what.c_str());
    c.Ok(f.ok, "a well-formed file loads");
    c.Ok(f.language == "ru" && !f.pluralForms.empty() && !f.charsetAssumed, "the header is read");
    auto find = [&](const char* id, bool ctx) -> const po::Entry* {
        for (const po::Entry& e : f.entries)
            if (e.id == id && e.hasCtx == ctx) return &e;
        return nullptr;
    };
    const po::Entry* rules = find("Rules", true);
    c.Ok(rules && rules->ctx == "f1" && rules->str[0] == "Rules\t\aAB", "adjacent strings join and escapes decode");
    c.Ok(!find("Apply", false) && !find("Fuzzy", false) && !find("Empty", false) && !find("Old", false),
         "an unknown escape, a fuzzy, an empty and an obsolete entry are not translations");
    const po::Entry* pl = find("%llu peer", false);
    c.Ok(pl && pl->plural && pl->str.size() == 3, "a plural entry keeps its forms");
    c.Ok(!find("%llu x", false), "a half-filled plural is refused");
    c.Ok(find("%1$s took %2$d", false) && !find("%s took", false), "conversions are checked against the msgid");
    c.Ok(!find("Plain", false) && !find("Nul", false), "a '%' in a plain line and a NUL escape are refused");
    c.Ok(f.refused.size() == 5, "each refused entry is reported once");

    c.Ok(!po::Read("msgid \"a\nmsgstr \"b\"\n").ok, "a string that never closes refuses the file");
    c.Ok(!po::Read("msgid \"\"\nmsgstr \"Content-Type: text/plain; charset=ISO-8859-1\\n\"\n").ok,
         "a charset other than UTF-8 refuses the file");
    c.Ok(!po::Read("msgid \"a\"\nmsgstr \"\xC3\x28\"\n").ok, "bytes that are not UTF-8 refuse the file");
    c.Ok(!po::Read("garbage\n").ok && !po::Read("msgid \"a\" x\nmsgstr \"b\"\n").ok,
         "a line that is no keyword, and text after a string, refuse the file");
    const po::File tmpl = po::Read("msgid \"\"\nmsgstr \"Content-Type: text/plain; charset=CHARSET\\n\"\n");
    c.Ok(tmpl.ok && tmpl.charsetAssumed, "a template's CHARSET is read as UTF-8");
    std::string big(po::kMaxFileBytes + 1, '#');
    c.Ok(!po::Read(big).ok, "a file over 1 MiB is refused");

    // The entry-level rows of D1 that kGood does not reach, one file each.
    auto entryRefused = [](const std::string& body) {
        const po::File g = po::Read(body);
        return g.ok && g.entries.empty() && g.refused.size() == 1;
    };
    c.Ok(entryRefused("msgid \"%llu a\"\nmsgid_plural \"%llu as\"\nmsgstr \"%llu b\"\n"),
         "a msgid_plural with a plain msgstr is refused");
    c.Ok(entryRefused("msgid \"a\"\nmsgstr[0] \"b\"\n"), "msgstr[n] without msgid_plural is refused");
    c.Ok(entryRefused("msgid \"%llu a\"\nmsgid_plural \"%llu as\"\nmsgstr[1] \"%llu b\"\nmsgstr[0] \"%llu c\"\n"),
         "plural forms out of order are refused");
    c.Ok(entryRefused("msgid \"a\"\nmsgstr \"\\x414\"\n"), "a hex escape of three digits is refused");
    c.Ok(entryRefused(std::string("msgid \"a\"\nmsgstr \"") + std::string(17000, 'b') + "\"\n"),
         "an entry over 16 KiB is refused");
    std::string nine = "msgid \"%llu a\"\nmsgid_plural \"%llu as\"\n";
    for (int k = 0; k < 9; ++k) nine += "msgstr[" + std::to_string(k) + "] \"%llu x\"\n";
    c.Ok(entryRefused(nine), "more than 8 plural forms are refused");
    c.Ok(entryRefused("msgid \"%s and %d\"\nmsgid_plural \"%s and %d\"\nmsgstr[0] \"%s\"\nmsgstr[1] \"%s and %d\"\n"),
         "a plural form missing a conversion is refused");
    c.Ok(entryRefused(std::string("msgid \"a\"\nmsgstr \"b") + '\0' + "c\"\n"), "a raw NUL byte in a string is refused");
    const po::File allEmpty = po::Read("msgid \"%llu a\"\nmsgid_plural \"%llu as\"\nmsgstr[0] \"\"\nmsgstr[1] \"\"\n");
    c.Ok(allEmpty.ok && allEmpty.entries.empty() && allEmpty.refused.empty(), "a plural with every form empty is untranslated");
    const po::File twoIds = po::Read("msgid \"\"\nmsgid \"A\"\nmsgstr \"a\"\n");
    c.Ok(twoIds.ok && twoIds.entries.size() == 1 && twoIds.entries[0].id == "A", "a second msgid starts a new entry");
    c.Ok(!po::Read("\"stray\"\n").ok, "a string line with no keyword refuses the file");
    const po::File crlf = po::Read("msgid \"\"\r\nmsgstr \"Language: de\\n\"\r\n\r\nmsgid \"a\"\r\nmsgstr \"b\"\r\n");
    c.Ok(crlf.ok && crlf.language == "de" && crlf.entries.size() == 1, "a CRLF file loads");
    const po::File late = po::Read("msgid \"a\"\nmsgstr \"b\"\n\nmsgid \"\"\nmsgstr \"Language: fr\\n\"\n");
    c.Ok(late.ok && late.language == "fr" && late.entries.size() == 1, "a header after the entries is read");
    const po::File badLine = po::Read("msgid \"a\"\nmsgstr \"b\"\n\nmsgid \"c\"\nmsgstr \"\xC3\x28\"\n");
    c.Ok(!badLine.ok && badLine.refusal.line == 4, "a file-level refusal names its entry's line");
}

void Merge(Checks& c) {
    // Four sources in rising priority, as Init inserts them: ll pack, ll override, ll_CC pack, ll_CC override.
    const char* hdr = "msgid \"\"\nmsgstr \"Plural-Forms: nplurals=2; plural=(n != 1);\\n\"\n\n";
    const std::string llPack = std::string(hdr) + "msgid \"A\"\nmsgstr \"a-ll-pack\"\n\nmsgid \"B\"\nmsgstr \"b-ll-pack\"\n\n"
                               "msgid \"%llu n\"\nmsgid_plural \"%llu ns\"\nmsgstr[0] \"%llu one\"\nmsgstr[1] \"%llu many\"\n";
    const std::string llOver = "msgid \"B\"\nmsgstr \"b-ll-over\"\n\nmsgid \"C\"\nmsgstr \"c-ll-over\"\n\n"
                               "msgid \"%llu n\"\nmsgid_plural \"%llu ns\"\nmsgstr[0] \"%llu uno\"\nmsgstr[1] \"%llu muchos\"\n";
    const std::string ccPack = std::string(hdr) + "msgid \"C\"\nmsgstr \"c-cc-pack\"\n\nmsgid \"D\"\nmsgstr \"d-cc-pack\"\n";
    const std::string ccOver = "msgid \"D\"\nmsgstr \"\"\n\nmsgid \"E\"\nmsgstr \"%s\"\n";   // empty, and refused
    detail::Catalog cat;
    detail::SourceReport r1, r2, r3, r4;
    const int llRule = cat.Insert(po::Read(llPack), -1, r1);
    cat.Insert(po::Read(llOver), llRule, r2);
    const int ccRule = cat.Insert(po::Read(ccPack), -1, r3);
    cat.Insert(po::Read(ccOver), ccRule, r4);
    auto text = [&](const char* id) -> std::string {
        const detail::Entry* e = cat.Peek(nullptr, id);
        return e ? e->forms[0] : std::string();
    };
    c.Ok(text("A") == "a-ll-pack" && text("B") == "b-ll-over" && text("C") == "c-cc-pack" && text("D") == "d-cc-pack",
         "ll_CC override > ll_CC pack > ll override > ll pack");
    c.Ok(cat.Peek(nullptr, "E") == nullptr && r4.refused.size() == 1, "a refused override entry inserts nothing");
    const detail::Entry* pl = cat.Peek(nullptr, "%llu n");
    c.Ok(pl && pl->plural && pl->forms[cat.SelectForm(*pl, 1)] == "%llu uno" &&
         pl->forms[cat.SelectForm(*pl, 5)] == "%llu muchos", "an override's plural entry takes the pack's rule");
    c.Ok(!cat.Peek(nullptr, "A")->hit.load() && cat.Find(nullptr, "A") && cat.Peek(nullptr, "A")->hit.load() &&
         cat.FoundCount() == 1, "a found lookup marks its entry; a peek does not");

    // A count of forms other than the rule's, and a Plural-Forms that does not compile, with and
    // without a pack rule to fall back on.
    detail::Catalog c2;
    detail::SourceReport s1, s2, s3;
    const std::string three = std::string(hdr) +
        "msgid \"%llu n\"\nmsgid_plural \"%llu ns\"\nmsgstr[0] \"a\"\nmsgstr[1] \"b\"\nmsgstr[2] \"c\"\n";
    c2.Insert(po::Read(three), -1, s1);
    c.Ok(c2.Peek(nullptr, "%llu n") == nullptr && s1.refused.size() == 1, "a count of forms other than nplurals is refused");
    const std::string broken = "msgid \"\"\nmsgstr \"Plural-Forms: nplurals=2; plural=n ^ 1;\\n\"\n\n"
        "msgid \"%llu n\"\nmsgid_plural \"%llu ns\"\nmsgstr[0] \"%llu a\"\nmsgstr[1] \"%llu b\"\n";
    detail::Catalog c3;
    c3.Insert(po::Read(broken), -1, s2);
    c.Ok(c3.Peek(nullptr, "%llu n") == nullptr && !s2.pluralNote.empty(), "a pack whose Plural-Forms does not compile refuses its plurals");
    const int pr = c3.Insert(po::Read(std::string(hdr)), -1, s3);
    detail::SourceReport s4;
    c3.Insert(po::Read(broken), pr, s4);
    c.Ok(c3.Peek(nullptr, "%llu n") != nullptr && !s4.pluralNote.empty(), "an override's uncompilable rule falls back to the pack's");
}

// Every pack this build embeds: named L10N_<LL_CC>, and each must load with nothing refused.
void Embedded(Checks& c) {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&Embedded), &self);
    std::vector<std::wstring> names;
    ::EnumResourceNamesW(self, reinterpret_cast<LPCWSTR>(RT_RCDATA),
                         [](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR ctx) -> BOOL {
                             if (!IS_INTRESOURCE(name) && std::wcsncmp(name, L"L10N_", 5) == 0)
                                 reinterpret_cast<std::vector<std::wstring>*>(ctx)->push_back(name);
                             return TRUE;
                         },
                         reinterpret_cast<LONG_PTR>(&names));
    // The packs this build must carry: a resource step that dropped one would otherwise pass in silence.
    auto has = [&](const wchar_t* n) {
        for (const std::wstring& x : names)
            if (_wcsicmp(x.c_str(), n) == 0) return true;
        return false;
    };
    c.Ok(has(L"L10N_ZH_CN") && has(L"L10N_EN_XA"), "the build embeds the zh_CN pack and the en_XA pseudo-locale");
    for (const std::wstring& name : names) {
        HRSRC res = ::FindResourceW(self, name.c_str(), reinterpret_cast<LPCWSTR>(RT_RCDATA));
        HGLOBAL glob = res ? ::LoadResource(self, res) : nullptr;
        const char* p = glob ? static_cast<const char*>(::LockResource(glob)) : nullptr;
        const po::File f = po::Read(p ? std::string_view(p, ::SizeofResource(self, res)) : std::string_view());
        detail::Catalog cat;
        detail::SourceReport rep;
        if (f.ok) cat.Insert(f, -1, rep);
        const bool clean = f.ok && rep.refused.empty();
        if (!clean)
            UE_LOGE("l10n selftest: FAIL -- the embedded pack %ls %s", name.c_str(),
                    f.ok ? "has refused entries" : f.refusal.what.c_str());
        c.Ok(clean, "an embedded pack loads with nothing refused");
    }
}

}  // namespace

bool RunSelftest(bool injectRed) {
    detail::g_selftestQuiet.store(true, std::memory_order_relaxed);
    Checks c;
    Plurals(c, injectRed);
    Conversions(c);
    Formatter(c);
    Tags(c);
    Reader(c);
    Merge(c);
    Embedded(c);
    detail::g_selftestQuiet.store(false, std::memory_order_relaxed);
    if (c.pass == c.total) {
        UE_LOGI("l10n selftest: ALL PASS (%d checks)", c.total);
        return true;
    }
    UE_LOGE("l10n selftest: %d/%d checks passed", c.pass, c.total);
    return false;
}

}  // namespace l10n
