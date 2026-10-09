// l10n/locale_choice.h -- which language the interface is in (private to src/l10n/).
//
// `auto` is the Windows display language -- what the player reads Windows in, not the regional format
// a player with an English Windows and Russian number formats has -- through
// GetUserPreferredUILanguages, the SDK's current call (GetUserDefaultUILanguage and LCIDToLocaleName are
// marked deprecated in winnls.h). MTA stores a `locale` setting its installer chose, default en_US
// (CLocalization.cpp); the mod has no installer, so the OS is the only signal at first start.
#pragma once

#include <string>
#include <string_view>

namespace l10n::detail {

// A BCP-47 or gettext tag in gettext form: `zh-CN` and `zh_CN.UTF-8` are `zh_CN`; a script subtag with a
// region is dropped (`zh-Hans-CN` is `zh_CN`), one without picks its region for Chinese (`zh-Hans` is
// `zh_CN`, `zh-Hant` is `zh_TW`) and is dropped otherwise; "" when the text is not a language tag.
std::string NormaliseTag(std::string_view tag);

// The first of the user's preferred display languages, as Windows names it (`zh-CN`), or "".
std::string WindowsDisplayLanguage();

}  // namespace l10n::detail
