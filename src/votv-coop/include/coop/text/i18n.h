// coop/text/i18n.h -- the language the mod's own text is drawn in.
//
// Every visible string in the source is written in English. A translator supplies a JSON pack
// named after a BCP-47 tag (ZH-CN.json, DE.json, ...) and drops it in the install's trans
// folder -- <game exe dir>\trans -- the anchor ue_wrap/core/paths gives every runtime artifact,
// NOT the DLL's own directory (UE4SS virtualizes Mods\ and a module-relative write was measured
// landing in the r2modman profile):
//
//     { "Multiplayer": "...", "Host": "..." }   -- the VALUES are in the target language
//
// English is the KEY, which is what makes the fallback total: a string the pack does not name
// renders as the English the source already carries -- never blank, never a key, so a
// half-finished pack is usable the whole way through. The language is the Windows system
// language unless a player pins one (ui.language).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace coop::i18n {

// Reads ui.language and the pack once, and is the ONLY thing that does. Called from the boot
// thread; before it has run, Tr() and TrW() answer with the English they were handed.
//
// That gate is load-bearing, not a nicety: a string literal wrapped at NAMESPACE SCOPE is
// evaluated during DLL load, long before UE4SS hands us a thread -- and a loader that runs there
// (config reads, file I/O) takes the whole DLL down with it, which reads as "the mod vanished".
void Init();

// The tag the surfaces are running in: the loaded pack's tag, or "en" when the built-in English
// is what they show.
const std::string& Language();

// The pack file actually loaded; empty when none was. THIS is the whole diagnosis of "my
// translation did not apply", so the boot line prints it beside every directory searched.
const std::string& PackPath();

// The directories the loader looks in, in order. One entry today; kept a list because the
// first-run story ("I put the file where the tutorial said") is exactly what this reports.
std::vector<std::string> SearchDirs();

// How many entries the pack carried, and how many disagreed with their key's printf
// conversions. Boot prints both; the mismatch count is the one class of translator mistake a
// machine finds for free.
size_t EntryCount();
size_t ConversionMismatches();

// The pack's translation of an English source string, or the argument itself when the pack has no
// entry. Never null, so a caller may pass the result straight to a formatting call.
const char* Tr(const char* english);

// The wide form, for the native Slate screens. The pack is converted to UTF-16 once at load, so
// this is a lookup and not a conversion; the English argument comes back when the pack has no entry.
const wchar_t* TrW(const wchar_t* english);

// Whether the settled language needs a script our four embedded faces do not carry (Han,
// kana, hangul). ui/fonts.cpp reads this to merge a system face and to stop excluding those
// windows from the atlas -- without it a Chinese pack draws tofu, which looks like a broken
// build rather than a missing font.
bool LanguageUsesCjk();

// The tag Windows would choose ("zh-CN"), or "en" when it cannot be read. Split out from the
// loader's own answer so the boot line can show both.
std::string SystemTag();

// Every printf-style conversion a string carries, as a sorted SET (so a translation may
// reorder them), with %% skipped. Compare a translation's result against its English key's: a dropped %s is a crash or a garbled line at
// the surface. Exposed for the audit script and the pack selftest.
std::string ConversionsOf(const std::string& text);

}  // namespace coop::i18n
