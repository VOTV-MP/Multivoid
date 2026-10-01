# Translating Multivoid

Turn the mod's interface into your own language -- **no programming, no rebuild**: copy one file,
translate it, rename it, drop it beside the game, restart.

## Three steps

1. Find the `trans` folder in your game install:

       <game dir>\WindowsNoEditor\VotV\Binaries\Win64\trans\

   It sits next to `multivoid.ini` and `multivoid.log`. If it does not exist, create it.

2. Copy `example.json` from this folder into that one, rename it to your language code
   (see LANGUAGE-CODES.md -- e.g. `zh-CN.json`), and replace the English on the **right** with
   your language. **Do not touch the left-hand side (the key).**

       {
         "Multiplayer": "...",
         "Host": "..."
       }

3. Restart the game. The interface is now in your language.

## How your file is chosen

At start-up the mod looks, in this order, for:

1. `ui.language` in `multivoid.ini` (default `auto`, i.e. "not specified")
2. your **Windows system language** (`zh-CN`, `ja-JP`, `de-DE`, ...)
3. the exact name first (`zh-CN.json`), then the primary subtag (`zh.json`)
4. and if neither exists, the **built-in English** -- the UI is never blank and never shows a key name

To see what it settled on, open `multivoid.log` and search for `i18n:`. That one line prints the
tag, the file it read, how many entries it kept, and which folder it searched.

## What must stay exactly as it is

| Item | Example | Why |
|---|---|---|
| printf placeholders | %s  %d  %zu  %.0f | the code fills these in. The **count must match**; their order may follow your language |
| `##` markers | the `##banlist` in `"Ban List##banlist"` | that is ImGui's internal widget id; changing it breaks the panel |
| positional words | "set a password below" | English's below/above describes English's layout -- **check the real screen** instead of translating it literally |

A wrong translation cannot harm anything: that single line falls back to English and a warning is
written to the log. An empty value means "not translated yet".

## For maintainers

- `example.json` is **generated** -- do not edit it by hand:
  - regenerate: `python src/votv-coop/i18n-tools/extract.py`
  - check template and source agree (CI-friendly): `... extract.py --check`
- draft a pack with a **local** model (no API cost): `... translate.py --lang zh-CN --tag ZH-CN`
  It validates the placeholders and `##` markers of every line, and resumes after an interruption.
- wire new strings to the lookup: `... apply.py` (dry run; add `--write` to edit files). It only
  touches strings that are already in the template, and skips `constexpr` tables -- a constant
  expression cannot call a function, so those are translated where they are drawn.
