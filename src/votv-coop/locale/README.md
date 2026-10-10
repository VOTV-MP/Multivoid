# Translating Multivoid

Turn the mod's interface into your own language -- **no programming, no rebuild**: copy one file, translate it,
name it after your language, drop it beside the game, restart.

The files are gettext catalogues (`.po`), the format MTA and most open-source projects use, so any `.po` editor works:
[Poedit](https://poedit.net) is free and shows each English line beside the box for yours.

## Three steps

1. Take the template, `multivoid.pot`, from this folder of the mod's repository (`src/votv-coop/locale/`).

2. Open it in Poedit (*Create new translation*, pick your language) or copy it to a file named after your
   language's code -- see the table below, e.g. `ru.po`, `pt_BR.po` -- and fill in each `msgstr` with your text.
   **Never change a `msgid`**: it is the English the mod looks your line up by.

3. Put the file in a folder named `multivoid_locale` next to the game's executable:

       <game dir>\WindowsNoEditor\VotV\Binaries\Win64\multivoid_locale\ru.po

   It sits next to `multivoid.ini` and `multivoid.log`; create the folder if it does not exist. Restart the game.

## How your file is chosen

At start-up the mod takes the language from `ui.language` in `multivoid.ini` -- `auto` (the default) is the language
Windows itself is displayed in -- and looks, from most to least specific, for `multivoid_locale/<ll_CC>.po`, the pack
the mod ships for `<ll_CC>`, `multivoid_locale/<ll>.po`, the pack for `<ll>`, and English. Your file overrides the
shipped pack line by line: a line you leave empty shows the pack's (or the English). To see what it settled on, open
`multivoid.log` and search for `l10n:` -- one line names the language, why it was chosen, and every file it read.

## What must stay exactly as it is

| Item | Example | Why |
|---|---|---|
| the conversions | `%s`  `%d`  `%llu`  `%.1f` | the mod fills these in. Keep every one, with its type; their order may follow your language if you number them: `%2$s ... %1$s` |
| `%1$s` in a player's action | `%1$s deleted an email: %2$s` | `%1$s` is the player's name, coloured in the chat; put it wherever your sentence needs it |
| the context | `msgctxt "server_browser"` | the same English can need two translations; Poedit shows it, and it must not be edited |
| a literal percent sign | `50%%` | in a line that has conversions, `%%` prints one `%`; a line with no conversions may not contain `%` at all |
| positional words | "set a password below" | English's below/above describes English's layout -- **check the real screen** |

A wrong line cannot break anything: the mod refuses it, shows the English instead, and writes the reason and the line
number to `multivoid.log`. A whole file that cannot be read (not UTF-8, a string that never closes) is refused the same
way and said once.

## Plural forms

A line about a count has a singular and a plural English form and one box per form your language has. Poedit fills the
header's `Plural-Forms` for your language when you create the translation; keep it, since the mod evaluates it to pick
the form for each number.

## Language codes

**The file name is the code**, the form gettext uses: the language in lower case, an underscore, the region in upper
case.

| File | Language | | File | Language |
|---|---|---|---|---|
| `zh_CN.po` | Simplified Chinese (ships with the mod) | | `zh_TW.po` | Traditional Chinese |
| `ja.po` | Japanese | | `ko.po` | Korean |
| `de.po` | German | | `fr.po` | French |
| `es.po` | Spanish | | `it.po` | Italian |
| `pt_BR.po` | Brazilian Portuguese | | `pt.po` | Portuguese |
| `ru.po` | Russian | | `uk.po` | Ukrainian |
| `pl.po` | Polish | | `cs.po` | Czech |
| `nl.po` | Dutch | | `sv.po` | Swedish |
| `tr.po` | Turkish | | `hu.po` | Hungarian |

Any language Windows can report works: the mod tries `<ll_CC>` first, then `<ll>`, then English.

## Notes

- The file must be **UTF-8**; a byte-order mark is tolerated.
- Write a space as an ordinary space: the ideographic space (U+3000) is never drawn, so a name cannot hide behind it.
- The interface draws the scripts its fonts carry -- Latin, Cyrillic, Greek and more -- and Simplified Chinese through a
  font Windows already has. Traditional Chinese, Japanese and Korean draw as boxes until a font for them is added to the
  mod (one table row); a right-to-left or complex script (Arabic, Hebrew, Thai, Devanagari) loads but does not shape
  correctly.
- `ui.language` takes effect at the next start.
