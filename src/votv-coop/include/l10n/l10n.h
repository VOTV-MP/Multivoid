// l10n/l10n.h -- the mod's own interface text in the player's language.
//
// The shape is MTA's (CLocalization.cpp, CLocalizationInterface.h): gettext
// catalogues, English as the msgid, the lookups -- T is `_`, Tc `_tc` (with a context), Tn `_tn`
// (plural); MTA's `_tcn` waits for a call site that needs a context and a plural at once -- and a marker that does nothing at run time so the template extractor finds a
// string in a static table, translated where it is drawn (`_td`, here L10N_MARK). The reader, the plural
// evaluator and the formatter are ours: MTA's parser, tinygettext, is GPL and is not vendored.
//
// Keys are narrow UTF-8 literals and every call is qualified `l10n::` (the extractor's rule). A lookup
// returns the catalogue's own immutable storage, or its argument on a miss -- Tn's miss is
// `n == 1 ? singular : plural`, the English rule -- with one hash, no allocation and no lock. The
// catalogue is built once by Init, before the overlay's first frame, and never changes: a change of
// language takes effect at the next start. Before Init every lookup misses. Any thread.
#pragma once

#include "l10n/mark.h"

#include <cstddef>

namespace l10n {

const char* T  (const char* msgid);
const char* Tc (const char* ctx, const char* msgid);
const char* Tn (const char* singular, const char* plural, unsigned long long n);

// "<text>###<id>" for an id-bearing ImGui widget, composed into its own buffer: a temporary that lives
// to the end of the full expression, so `ImGui::Button(l10n::Label(l10n::T("Apply"), "apply"))` is
// safe. The id is a literal and never depends on the language, so a translated label neither changes a
// widget's id nor collides with another translated alike. Cut at a UTF-8 boundary; nothing interned.
class Label {
public:
    Label(const char* text, const char* id);
    operator const char*() const { return buf_; }

private:
    char buf_[256];
};

// Byte offsets of an argument's text inside a formatted line; begin -1 when it is absent.
struct Span {
    int begin = -1;
    int len = 0;
};

// printf with argument numbers (`%2$s %1$s`), over the conversion grammar of l10n/printf_check.h.
// Returns the bytes written, cut at the last whole UTF-8 sequence that fits and always terminated, or
// -1 with an empty buffer when the format is refused. FmtSpan also reports where argument 1 landed.
// The arguments are read by the types the format names, as printf reads them: a call passing fewer
// arguments than its msgid's conversions is a defect at that call site, and nothing here can see it
// (a translation cannot cause it: its conversions are pinned to the msgid's at load).
int Fmt    (char* buf, size_t size, const char* fmt, ...);
int FmtSpan(char* buf, size_t size, Span* arg1, const char* fmt, ...);

// The locale chosen ("zh_CN") when any catalogue of it -- zh_CN or zh, pack or override -- contributed
// an entry, or "" when the interface is in English.
const char* ActiveLocale();

// Once, from the harness, before the overlay's first frame: `language` is the ui.language row's value,
// "auto" for the Windows display language. Builds the one merged catalogue (embedded pack and override
// files) and says what it chose and why.
void Init(const char* language);

// The pure selftest, from the harness before Init: prints "l10n selftest: ALL PASS (N checks)" or each
// failing case. `injectRed` (the dev row l10n_selftest_red) makes one expected plural form wrong, so the
// rig's verdict is seen reading a failure.
bool RunSelftest(bool injectRed);

// Dev, for the drill: whether a lookup of this entry has found its translation (ctx may be null), and
// how many entries have. Both read the flags a found lookup sets; neither marks anything.
bool   WasFound(const char* ctx, const char* msgid);
size_t FoundCount();

}  // namespace l10n
