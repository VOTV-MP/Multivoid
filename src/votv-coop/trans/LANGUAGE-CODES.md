# Language codes

**The file name IS the tag.** Copy `example.json` to the name from the tables below and translate it.

The tag is a BCP-47 language code, and it is what Windows reports for your system language --
which is what the mod compares against, so an exact match is the common case.

## Chinese

| File | Language |
|---|---|
| `zh-CN.json` | Simplified Chinese (what a Chinese Windows returns) |
| `zh.json` | covers every Chinese locale as a fallback when the exact name is absent |
| `zh-TW.json` | Traditional Chinese (Taiwan) |
| `zh-HK.json` | Traditional Chinese (Hong Kong) |

## Other common languages

| File | Language | | File | Language |
|---|---|---|---|---|
| `en.json` | English (built in) | | `tr.json` | Turkish |
| `ja.json` | Japanese | | `hu.json` | Hungarian |
| `ko.json` | Korean | | `ro.json` | Romanian |
| `de.json` | German | | `vi.json` | Vietnamese |
| `fr.json` | French | | `th.json` | Thai |
| `es.json` | Spanish | | `id.json` | Indonesian |
| `pt-BR.json` | Brazilian Portuguese | | `ms.json` | Malay |
| `pt.json` | Portuguese | | `hi.json` | Hindi |
| `it.json` | Italian | | `ar.json` | Arabic |
| `ru.json` | Russian | | `fa.json` | Persian |
| `uk.json` | Ukrainian | | `he.json` | Hebrew |
| `pl.json` | Polish | | `cs.json` | Czech |
| `nl.json` | Dutch | | `sv.json` | Swedish |
| `da.json` | Danish | | `fi.json` | Finnish |
| `no.json` | Norwegian | | `el.json` | Greek |

Any tag Windows can report works: the loader tries the exact name first, then the part before the
dash, then English.

## Notes

- The file must be **UTF-8**. A byte-order mark is tolerated and stripped, but plain UTF-8 is better.
- The mod only draws scripts its font layer can reach. CJK (`zh`, `ja`, `ko`) is wired up and
  merges a system face; for other scripts, check the log -- a missing glyph is reported.
