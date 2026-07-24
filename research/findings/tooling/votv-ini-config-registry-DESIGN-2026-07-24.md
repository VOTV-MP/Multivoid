# multivoid.ini — seeder, config registry, validation, catalog (DESIGN)

**Date:** 2026-07-24 · **Status:** DESIGN, not built · **Pass:** `/qf` 16 rounds, **did NOT converge**
(every round including the last returned material questions; see §9) · **Green-light:** user, after the
cost was stated.

Design of record for the config layer rework. Read §2 (measured facts) before touching any part of it —
four of the ten defaults in the first hand-written sketch were WRONG, and that is what turned a layout
request into a source-of-truth problem.

---

## 1. What the user actually asked for

Verbatim, in order:

> "А наша пара dll xinput и multivoid — там ini создается автоматом? А что внутри после первого запуска?
> … надо продумать как там что идёт первым. Давай блок мультиплеера net первым и пусть там по дефолту
> никнейм Pelmentor стоит, а в самом конце пусть будут флаг dev features чтоб их особо не было заметно
> тем кто не интересуется."

Then, catching two errors in my sketch from his own memory of the code:

> "Разве net.master не должен быть =DEFAULT"
> "Разве UI у нас тоже не должны быть значения по дефолту я там задавал на разные секции дефолт шрифт разный?"

Then a third requirement, and its clarification:

> "Ну и какой-то robust error handling тоже надо по типу значение говно и несуразица то ставится
> исправление или типа того" → **"Не в смысле чинить, а в смысле дефолт ставить да"**

Then, on the catalog:

> "Файл multivoid.ini.example это генерируемый пример? Давай туда засунем вообще все флаги и тд вообще
> всё что есть и пусть юзер если ему надо там выбирает смотрит что ему в его ini копипастнуть"

Settled by the user during the pass: `Pelmentor` is a joke and stays; the three surfaced product points
are accepted; the commented-vs-active catalog choice was delegated to me.

**Requirement drift, stated up front (§9 has the rest):** requirement 3 ("dev flags last, out of the
way") ended the pass with almost no observable subject, because a fresh ini contains no dev keys at all.
It is satisfied in form (`[dev]` last in the skeleton and in the catalog), but its intent was already
satisfied by a different means.

## 2. Measured fact base

Every row here was measured this session against the current tree. Nothing is carried from a doc.

| # | Fact | Evidence |
|---|---|---|
| F1 | No seeder exists. The ini appears as a side effect of the first `WriteIniValue`, which appends to a nonexistent file. | `config.cpp:166-172` |
| F2 | On a clean install the file after first launch holds **exactly two lines** — `player_guid` and `player_skin`. Everything else is UI-driven. | `harness.cpp:117`, `:120` → `config.cpp:419-431`, `:397-403` |
| F3 | **Sections are decorative.** `ParseIniLine` splits on the first `=`; `[net]` has none, so it is skipped. | `config.cpp:77-83`, `config.h:31` |
| F4 | `ReadIniValue(key, def)` returns `def` **only when the key is ABSENT**. A present-but-empty key returns `""`. So a seeded key OVERRIDES the code default. | `config.cpp:96-104` |
| F5 | `WriteIniValue` replaces a key's line in place only if the key exists; otherwise it **appends at EOF**. | `config.cpp:166-172` |
| F5b | Reader and writer both take the **FIRST** match. A second occurrence lower down is silently dead — until the first is deleted, when it revives with a stale value. | `config.cpp:101`, `:156` |
| F6 | ~100 ini keys. 109 read sites across 60 files (31 of them `coop/dev` probes); 24 write sites across 10 files. `voice_panel` passes its key as a **runtime parameter**, not a literal. | grep census |
| F7 | `WriteIniValue` was hardened after the **2026-07-02 data loss**: abort if the file exists but is read-locked, `.new` + atomic `MoveFileExW`, every write checked before the swap, one process-wide mutex. | `config.cpp:107-200`, comment block `:122-130` |
| F8 | **THREE buffer sizes for one file format:** `char[128]` in `LookupTriState`, `char[256]` in the reader, `char[512]` in the writer. A 380-char line in CLIENT_1's ini is split by the reader and its tail contains `=`, so a **phantom key exists live in 2 of our 4 inis**. Measured clean: no line >512 anywhere, no leftover `.ini.new`, no duplicate keys — so no physical corruption has happened yet. | `config.cpp:461`, `:98`, `:152` |
| F9 | **STUN is not a hole.** The live production master returns a `stun:<our-own-coturn>:3478` URI — not Google's, not empty. `COOP_STUN_URI` **is** set in production, so NAT piercing works and sessions are not silently forced onto TURN relay. (Endpoint deliberately not spelled out here — public repo.) | POST `/v1/host` against the live master; probe lobby verified reaped after ~90 s |
| F10 | Production TLS is healthy (chain leaf → LE YE1 → ISRG Root YE, valid to 2026-10-18). The "certificate has expired" error hit first was a **stale local Python CA bundle**, not an outage. | `openssl s_client`, `curl` verify OK |
| F11 | A runtime walk of keys is **structurally incomplete**: `desk_diag_ms` is read inside a block gated on `IsIniKeyTrue("desk_diag")`, so a normal launch never reads it. | `desk_diag.cpp:52` vs `:134` |
| F12 | The typed-read pattern already exists by hand at a site: `if (ms<100) ms=100; if (ms>60000) ms=60000;`. | `desk_diag.cpp:53-55` |
| F13 | The nickname is **never an identity key** — a per-slot display string. The ban list keys by IP only; the nick is stored "for the admin's reference". | `ban_list.h:9-13`; no map or lookup by nick anywhere |
| F14 | **Config reaches the wire.** Join carries `[u8 nicklen][nick][u8 guidlen][guid][u8 skinlen][skin][u8 flags][u8 has][r][g][b]`. `nicklen` is `uint8` ⇒ any `net.nick` validator must cap at 255 bytes. No protocol bump is needed (the rule triggers on FORMAT change; a default value is not a format). | `protocol.h:1240-1246` |
| F15 | Deployed `net.nick`: HOST `Pelmentor` (**the user set the joke by hand already**), CLIENT_3 `DevPeer`, CLIENT_1/2 **ABSENT**. | the 4 real inis |
| F16 | Inline comments live inside values; `ParseIniLine` returns them as part of the value. Only `IsIniKeyTrue` tolerates them (`NormalizeFlagLine`). No key carrying an inline comment is currently read by strict `ReadIniValue`, so the hazard is **latent** — typed reads would wake it. | the 4 real inis + `config.h:80-83` |
| F17 | **ENV is a config layer that BEATS ini.** 16 `VOTVCOOP_*` reads in `config.cpp`; **46 more outside it**. | grep census |
| F18 | No legitimate `;` inside any value in the 4 real inis; no default literal contains one; master-supplied `turn`/`stun` **bypass the ini** entirely (`session_manager` writes `cfg` fields directly). | grep census |
| F19 | The nick literal sits on two axes, and the real split is **MY NAME vs SOMEONE ELSE'S**: mine = `config.cpp:386`, `player_handshake.cpp:37`, `player_handshake.cpp:219` (`SanitizeNickname`'s empty fallback, which applies to my own nick), `session_manager.cpp:61`, `server_browser.cpp:46`. Someone else's = `scoreboard.cpp:115` (already splits local vs remote → `"Remote player"`), `peer_action_feed.cpp:52`. | grep census |
| F20 | `VOTVCOOP_SCENARIO` is deliberately **env-only**; `scenario.txt` was retired 2026-06-06 because a file aliases the NEXT native launch into gameplay. | `config.cpp:55-58` |
| F21 | **Duplicate env resolvers:** `VOTVCOOP_MASTER_URL` is read in `config.cpp` AND `session_manager.cpp`; `VOTVCOOP_NET_ROLE` in `config.cpp` AND 19 `autotest_*.cpp` files — and `net.role` IS an ini key (`config.cpp:315`), so the autotests bypass that layer. | grep census |
| F22 | `net.nick=` (present, empty) yields `""` today and `SetLocalNickname` sanitizes it to `"Player"`. If `Resolve` treats present-but-empty as absent, the empty branch stays reachable via all-dash/all-space nicks — **nothing becomes unreachable**, so RULE 2 does not fire here. | `config.cpp:386` → `player_handshake.cpp:219` |
| F23 | The first writes on a clean install are `ReadPlayerGuid` and `ReadPlayerSkin`. **The seeder must run before `harness.cpp:117`** or the file is born without the banner and stays on the append-at-EOF path forever. | `harness.cpp:117`, `:120` |

## 3. Why this grew from a layout request

I hand-wrote a sketch layout and **4 of ~10 concrete defaults were wrong**: `net.port` 7777 vs the real
`kDefaultPort = 47621` (`protocol.h:1090`); `ui.scale` 1.0 vs `"1.25"` (`scale.cpp:73`); `net.master`
empty vs the existing `DEFAULT` sentinel; `ui.font.*` as one key vs **five roles with two different
defaults** (`fonts.cpp:50-56`). Two of the four the user caught himself.

By F4, a seeded key overrides the code default. So a seeder carrying its own transcribed copy of the
defaults is a **second source of truth that silently wins the drift** — the file would faithfully
reprint a wrong value forever while looking authoritative. That is what forced the registry.

## 4. The design

**T1 — the fresh ini is a skeleton.** Our banner marker line + ordered section headers, `[net]` first,
`[dev]` last, and **zero default values**. Existing files are never rewritten automatically. Per F23 the
seeder runs before `harness.cpp:117`.

**T1b — the owner's opt-in reformat.** Because "never rewrite existing files" means the person who asked
for the order (F15: his ini exists and is bannerless) would never get it, a **user-triggered** action
reformats an existing ini. Same risk as the automatic migration we rejected, taken knowingly by the one
who wants it, never imposed.

**T2 — one declarative registry.** `{key, section, default | computed-marker | env-only-marker, lo, hi,
comment, envVar}`. It **replaces** the five MY-NAME literals (F19) and the 16 `config.cpp` env reads
(RULE 2, same commit). A row carries **either** a default **or** a computed marker, never both — so every
key has exactly one source; `player_guid`/`player_skin` are computed rows (the random roll + immediate
write stays at the call site). `SCENARIO` is an env-only row carrying "ini layer FORBIDDEN + reason"
(F20). Typed handles for literal sites, string lookup for `voice_panel`'s runtime key.

**T2b — separate commit, same batch.** Retire the F21 duplicate env resolvers behind one accessor. Not
cargo inside T2 — one axis per arc.

**T3 — one write algorithm, no branches.** Find the key's section header; if the file has no such header,
append at EOF — which is exactly today's behaviour for a bannerless file. There is no parallel path and
nothing to retire. Insertion is a **MOVE of the single occurrence, never an ADD**: an occurrence anywhere
in the file is edited in place (F5b). Duplicates are **observed and reported, never deleted** — the write
edits the FIRST occurrence (what the reader already honors) and logs that lower dead copies exist.
Deleting a line from a user's ini is the F7 seam and contradicts "existing files are not rewritten".

**T3b — writers go through the registry too** (section, clamps, occurrence count). Clamp on write as
well: never persist a value the read would reject.

**T4 — one parse primitive, three callers** (retiring 128/256/512), unbounded line read. An over-long
line is read and logged, never rejected or dropped: a local file the user owns is not hostile wire input,
so discarding a line would destroy their data. **This is the FIRST commit** — the annotated format pushes
lines toward the writer's limit, so building on the unfixed parser would manufacture the exact
2026-07-02 loss class.

**T5 — inline-comment stripping lives in that primitive, BEFORE validation**, and only for a
**whitespace-preceded `;`** (the measured convention, F18), so a bare `;` inside a value survives.
`NormalizeFlagLine`'s private tolerance then retires (RULE 2).

**T6 — validation sits AFTER layer resolution.** `Resolve(key) = env → ini → default`, **then** clamp.
Hanging the validator on the ini read would let env garbage sail past (F17). Typed reads:
`ReadInt(key,def,lo,hi)` / `ReadEnum(key,def,tokens)` / `ReadFloat(...)` — validity is expressed by the
signature, so "did a site miss its validator" is answered by the compiler, not by a checklist. Raw string
reads survive only where the value genuinely is a free string. Behaviour per the user's ruling: **log +
return the default in memory, never write.** Exception, with an invariant: *write only what nothing else
defines and what must survive a restart* — exactly `player_guid` + `player_skin`. `net.nick` is clamped
to 255 bytes (F14).

**T7 — `Pelmentor` is the registry default for MY NAME** at all five sites (F19); someone-else's-name
sites are untouched, or a nameless remote peer would be labelled Pelmentor. **Nothing is seeded into any
file** — F4 already returns the default for an absent key, so a seeded line would be a second definition.

**T8 — `multivoid.ini.example`.** Generated **at launch**, atomically, beside the DLL (stronger than a
build artifact, which drifts if a different DLL build is dropped in). All ini keys, each row carrying its
env var name where an ini layer exists. Per-launch env appears as **reference text, never as a copyable
line** (F20). **All lines commented**, with an instruction header — the user delegated this choice; a
wholesale copy-paste is then inert and cannot freeze today's defaults on someone's disk. Never read as
config (the reader opens only `multivoid.ini`); does not ride `WriteIniValue`, so F7 is untouched.

**T9 — STUN is a separate arc.** No hole (F9). Residue: the dead Google literal at `config.cpp:283` (dead
on the normal path, but the one external address we impose on the env/ini and master-down paths), and the
missing empty-response fallback — `session_manager.cpp:278/:374/:539` overwrite the whole ICE block
unconditionally, so "the master sent empty" is indistinguishable from "the master did not send".

## 5. Verification

| Step | Instrument |
|---|---|
| T4 | before/after `{key → value}` map **as read**, plus a multiset of all non-key lines; must-FAIL controls: drop a line, change a value, duplicate a key. Corpus: the 4 real inis + synthetic nasties (380-char line, CRLF, no trailing newline, a dupe, `=` inside a comment). |
| T2 | `{key → default}` dumped before and after the registry move and compared. |
| T3 | same shape as T4, plus the occurrence-count report. |
| T8 | every registry row appears exactly once; no computed-marker row prints a value as a real default; no env-only key emitted in copyable form; every line commented. **Round-trip:** uncomment the whole catalog, parse it with our own parser, and it must equal the registry defaults. |
| T2/T6 | **SMOKE, not an instrument**: launch with `net.nick` ABSENT and assert the log and the wire carry `Pelmentor`, not an empty string. Must run on a **fresh temporary install**, not on the dev rig (see §7). |

## 6. Commit order

`T4` (parse primitive) → `T2` (registry + RULE-2 retirements) → `T2b` (duplicate env resolvers) → `T6`
(typed reads) → `T3`/`T3b` (banner + section-aware move) → `T1`/`T1b` (seeder + opt-in reformat) → `T8`
(catalog).

## 7. Consequences that must be said out loud

- **Every unconfigured peer becomes `Pelmentor`.** Measured: CLIENT_1/2 have no `net.nick`, and neither
  does any user who never edited their name. No correctness impact (F13), but the activity feed always
  renders the nickname, never "You", so several `Pelmentor`s are indistinguishable in the feed. Same is
  true of today's `Player`; the difference is that a joke default gets kept.
- **Our own dev rig loses attribution** unless the four installs get distinct `net.nick` lines — HOST is
  already `Pelmentor`, CLIENT_1/2 would join it. One line each; rig hygiene, not a design change.
- **The requester does not benefit by default.** T1 only reaches installs created after it ships; T1b is
  the deliberate answer.
- **Cost:** the registry touches ~109 read sites in 60 files and 24 write sites in 10, plus a separate
  commit for 20 duplicate resolver sites. Roughly a week. The visible result for a new player is an
  ordered four-header ini and a reference catalog beside it. Justified under RULE 1 because the real roots
  — duplicated defaults and three buffer sizes for one format — are genuine, but the price was stated
  before starting, not after.

## 8. What was dropped along the way (four reframes)

1. **Seeding active default values** → they override the code default (F4) and are a second source.
2. **Seeding commented default values** → still a second copy; re-rendering them makes it worse (a wrong
   value reprinted authoritatively forever), and no field distinguishes a seeder line from a user's own
   hand-commented note, so re-rendering would destroy user input in the file hardened after a data loss.
3. **Migrating existing inis** → real comment blocks document several keys, one of them elsewhere in the
   file, so "carry the adjacent comment" is undefinable; the only benefit was cosmetics in files only the
   maintainer sees. Replaced by T1b (opt-in).
4. **STUN as part of this work** → measured to not be a hole at all (F9).

Also dropped: a C++ call-site parser to harvest defaults (a new framework for a problem the registry does
not have); typed key handles across all ~100 keys (scope guard).

## 9. Open — the pass did NOT converge

16 `/qf` rounds; the last round still returned material questions, and they were not cosmetic (round 16
reversed the duplicate-deletion decision and surfaced that the requester gets nothing by default). Per the
`/qf` rule, convergence is the critic holding, not the round cap, so this design is **not certified**.

Residual questions at the point of writing:

- Whether the catalog can honestly claim "все флаги" when 46 env reads live outside the registry by
  design (F17/F20). Current answer: it lists them as reference, not as copyable rows, and the header says
  so.
- Whether the ~46 outside env reads' inline defaults are their own arc (they are the same
  duplicated-literal shape, but per-launch env must not gain an ini layer).
- T1b's exact surface (menu action vs console command) is unspecified.

**Next step:** run the next `/qf` pass against **this document**, not against a summary of it — so the
critic reads what the user reads.
