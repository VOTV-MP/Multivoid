# multivoid.ini — seeder, config registry, validation, catalog (ALL FOUR ARCS BUILT)

**Date:** 2026-07-24 (pass 3: 2026-07-25) · **Status:** **THE WHOLE WORKSTREAM IS BUILT 2026-07-25
(arcs 1+2 released in b126-dev; arc 3 = the const Row& ratchet both directions + registry_gate CI,
arc 4 = the T8 catalog — see the arcs table for commits + evidence; per-arc impl designs:
`votv-ini-arc3-impl-DESIGN-2026-07-25.md`, `votv-ini-arc4-T8-catalog-impl-DESIGN-2026-07-25.md`).
NOT hands-on (the SETTINGS CHECK panel + the catalog await the user's take).** · **Passes:** `/qf` pass 1 = 16
rounds (did NOT converge); pass 2 = 15 rounds against this document (did NOT converge; four primary answers
withdrawn); **pass 3 = rounds E1-E15 against the pass-2 rewrite + confirmations E16-E19 against THIS text —
CONVERGED: genuine "that holds" at E19 (§9); the design is certified and arcs 1+2 (one release) are cleared
to build.** · **Scope consent:** user, 2026-07-24: *"мне пофиг, хоть месяц даже
если потратим на это"* — full scope authorized, arcs 1-4.

Design of record for the config layer rework. **Read §2 before touching any part of it.**

---

## 1. What the user actually asked for

Verbatim, in order:

> "А наша пара dll xinput и multivoid — там ini создается автоматом? А что внутри после первого запуска?
> … надо продумать как там что идёт первым. Давай блок мультиплеера net первым и пусть там по дефолту
> никнейм Pelmentor стоит, а в самом конце пусть будут флаг dev features чтоб их особо не было заметно
> тем кто не интересуется."

Then, catching two errors in the sketch from his own memory of the code:

> "Разве net.master не должен быть =DEFAULT"
> "Разве UI у нас тоже не должны быть значения по дефолту я там задавал на разные секции дефолт шрифт разный?"

Then a third requirement, and its clarification:

> "Ну и какой-то robust error handling тоже надо по типу значение говно и несуразица то ставится
> исправление или типа того" → **"Не в смысле чинить, а в смысле дефолт ставить да"**

Then, on the catalog:

> "Файл multivoid.ini.example это генерируемый пример? Давай туда засунем вообще все флаги и тд вообще
> всё что есть и пусть юзер если ему надо там выбирает смотрит что ему в его ini копипастнуть"

**Ruling during pass 2** — on `release/votv-coop.ini` (F24): *"Не надо ничего никому доставлять, это старый
артефакт, наш мод будет делать свой ini правильным образом."* → deleted (RULE 2, commit `4e232b38`).

Settled: `Pelmentor` is a joke and stays; the three surfaced product points are accepted; the catalog is
commented-lines (delegated to the primary, chosen); the orphan file is deleted.

**Product question ANSWERED (user, 2026-07-25):** *"Pelmentor это в свежем ini и оно должно не навязываться,
игрок поменяет это на свой ник."* → **"стоит" = a VISIBLE ACTIVE line**: the fresh-file skeleton carries
`net.nick=Pelmentor` as its one seeded value, deliberately editable — the player replaces it with their own
nick. `net.nick` is the ONE named registry exception (`kind = seeded-active`); the price was stated and
accepted (an active line pins the value past any future default change — ≈0 cost for a nick; the UI
overwrites the line on first edit anyway, `server_browser.cpp:120`). Every other key keeps zero seeded
values (F4).

**Drift, stated up front:** requirement 3 ("dev flags last, out of the way") has almost no observable subject
in a fresh ini (no dev keys exist there); it is satisfied by `[dev]` last in the skeleton and in the catalog.

## 2. Measured fact base

F1-F23 measured in pass 1; F24-F35 in pass 2; **F36-F45 in pass 3.** Nothing is carried from a doc.

| # | Fact | Evidence |
|---|---|---|
| F1 | No seeder exists. The ini appears as a side effect of the first `WriteIniValue`, which appends to a nonexistent file. | `config.cpp:166-172` |
| F2 | On a clean install the file after first launch holds **exactly two lines** — `player_guid` and `player_skin`. | `harness.cpp:117`, `:120` → `config.cpp:419-431`, `:397-403` |
| F3 | **Sections are decorative.** `ParseIniLine` splits on the first `=`; `[net]` has none, so it is skipped. | `config.cpp:77-83` |
| F4 | `ReadIniValue(key, def)` returns `def` **only when the key is ABSENT**. A present-but-empty key returns `""`. A seeded key OVERRIDES the code default. | `config.cpp:96-104` |
| F5 | `WriteIniValue` replaces a key's line in place only if the key exists; otherwise it **appends at EOF**. | `config.cpp:166-172` |
| F5b | ~~Reader and writer both take the FIRST match.~~ **FALSE AS A LAW — see F25.** | corrected by pass 2 |
| F6 | Key/site census, units explicit (pass-3 re-run, authoritative): **106 literal read sites + 6 non-literal sites across 59 files; 99 unique literal keys + 10 composed = 109 KEYS.** 24 write sites / 10 files. | grep census 2026-07-25 |
| F7 | `WriteIniValue` hardened after the **2026-07-02 data loss**: abort if the file exists but is read-locked, `.new` + atomic `MoveFileExW`, every write checked before the swap, one process-wide mutex. | `config.cpp:107-200` |
| F8 | **THREE buffer sizes for one format:** 128 flag / 256 reader / 512 writer. A 380-char line splits; its tail contains `=` → a **phantom key live in 2 of 4 rig inis**. | `config.cpp:461`, `:98`, `:152` |
| F9 | **STUN is not a hole.** The live master returns our own coturn URI. | POST `/v1/host` |
| F10 | Production TLS healthy; the "expired" scare was a stale local CA bundle. | `openssl`, `curl` |
| F11 | A runtime key walk is **structurally incomplete**: `desk_diag_ms` is read only inside a block gated on `IsIniKeyTrue("desk_diag")`. | `desk_diag.cpp:52` vs `:134` |
| F12 | The typed-read pattern exists by hand at one site (`ms` clamp). | `desk_diag.cpp:53-55` |
| F13 | The nickname is **never an identity key** — per-slot display string; ban list keys by IP. | `ban_list.h:9-13` |
| F14 | Config reaches the wire; `nicklen` is `uint8` ⇒ cap `net.nick` at 255 bytes. No protocol bump. | `protocol.h:1240-1246` |
| F15 | Deployed `net.nick`: HOST `Pelmentor` (user-set), CLIENT_3 `DevPeer`, CLIENT_1/2 absent. | the 4 rig inis |
| F16 | Inline comments live inside values; only the flag reader tolerates them. Hazard latent. | the 4 rig inis |
| F17 | **ENV beats ini.** 16 `VOTVCOOP_*` reads in `config.cpp`; ~46 more outside it. | grep census |
| F18 | No legitimate `;` inside any value in the 4 rig inis; all four inline comments are whitespace-preceded. | grep census |
| F19 | The nick literal's real split is **MY NAME vs SOMEONE ELSE'S**: mine = `config.cpp:386`, `player_handshake.cpp:37`, `:219`, `session_manager.cpp:61`, `server_browser.cpp:46`. Someone else's = `scoreboard.cpp:115`, `peer_action_feed.cpp:52`. | grep census |
| F20 | `VOTVCOOP_SCENARIO` is deliberately **env-only**; `scenario.txt` retired 2026-06-06. | `config.cpp:44-58` |
| F21 | **Duplicate env resolvers:** `VOTVCOOP_MASTER_URL` (config.cpp + session_manager.cpp); `VOTVCOOP_NET_ROLE` (config.cpp + 19 `autotest_*.cpp`). | grep census |
| F22 | `net.nick=` (present, empty) yields `""`, sanitized to `"Player"`. Sharpened by F33. | `config.cpp:386` → `player_handshake.cpp:219` |
| F23 | First writes on a clean install are guid + skin. **A seeder must run before `harness.cpp:117`.** | `harness.cpp:117`, `:120` |
| F24 | `release/votv-coop.ini` was an orphan example carrying the requested layout as ACTIVE values; nothing deployed it. User ruled: DELETE. **Deleted + pushed.** | `git log`, commit `4e232b38` |
| F25 | **The two readers have DIFFERENT first-match rules.** `ReadIniValue`: first matching KEY, case-sensitive, no comment strip. `LookupTriState`: first RECOGNIZED VALUE, case-insensitive, strips comments, **takes no mutex** (the `config.cpp:85-89` comment is FALSE). | `config.cpp:96-104` vs `:457-473` |
| F26 | **FOUR truthiness shapes ship:** (a) `LookupTriState` 58 keys; (b) `!= "0"` (anything true) — `harness.cpp:122`, `peer_action_feed.cpp:25`, `voice_chat.cpp:86`; (c) `== "1"` strict — `session_runtime.cpp:205`, `net_stats_panel.cpp:29`; (d) `1\|true\|yes\|on` — `config.cpp:225`. `nameplate=true` works, `ui.netstats=true` silently does not. | grep census |
| F27 | **Composed-key producers are exactly three**; no other non-literal key argument exists in the tree. | `fonts.cpp:72`, `voice_chat.cpp:85`, `voice_panel.cpp:60,67` |
| F28 | `ui.font=fixedsys` sits in the requester's own HOST ini and is read by NOBODY (the code composes `ui.font.<role>`). | `fonts.cpp:70,251` + HOST ini |
| F29 | **SIX default shapes** (inline literal; genuinely-none `""`; `""`-sentinel with the real default downstream; absence=false; absence=TRUE; computed). | grep census |
| F30 | Range census over the literal keys: 10 numeric, 4 enum, 5 string-boolean, rest free strings. ONE site clamps today. | grep census |
| F31 | `WriteIniValue` is **LOSSLESS on long lines**; the real corruption path is `ParseIniLine` running per CHUNK in the writer. | `config.cpp:152-165`, `:182-184` |
| F32 | **A MOVE past a duplicate can flip a flag** (F25 first-recognized). Reformat must collapse first. | derived, `config.cpp:467-471` |
| F33 | **Present-but-empty turns a default-off key ON** (`"" != "0"`). | `voice_chat.cpp:84-86` |
| F34 | Two UNLATCHED flag reads open+scan the ini per call; the `call_once` idiom exists in-tree. | `kerfur_form_assembler.cpp:168`, `kerfur_convert.cpp:570` |
| F35 | `dev_menu` gated on `devkeys` in the very file it would fix; `boot_warning_dialog::Arm` single-slot. | `dev_menu.cpp:539`, `:624`; `boot_warning_dialog.cpp:29-35` |
| **F36** | **Today's writer deletes the inline comment of the rewritten line** (replaces the whole line with `key=value\n`). Pre-existing behavior. | `config.cpp:156-158` |
| **F37** | **ABSENT vs UNREADABLE is discriminable**: live probe — share-locked open fails `EACCES(13)`, missing `ENOENT(2)`; and the F7 writer already discriminates exists-vs-locked in-tree. | live probe 2026-07-25; `config.cpp:145-149` |
| **F38** | `fgets` NULL conflates EOF with stream error; **today's read loops never check `ferror`** — a mid-stream failure reads as ABSENT for every key past it. | `config.cpp:99-102` |
| **F39** | **The two live phantom keys collide with ZERO of the 99 real keys** (they contain spaces), and the real key on those lines (`join_window_pos_trace`) keeps its verdict under unbounded reading — the phantom-death flip moves **zero verdicts** on the rig. | chunk-split simulation 2026-07-25 |
| **F40** | **Zero case-only twins** among the 99 literal keys; **zero wrong-case key lines** in the 4 rig inis; **zero ci-fold collisions** across all 109 keys (99 literal + 10 composed, both sets all-lowercase). | grep census 2026-07-25 |
| **F41** | **Composed keys are exactly 10**: `ui.font.{menu,chat,net,nameplate,toast}` (kRoles, per-role defaults Fixedsys×3 + Roboto×2), `voice.{test_tone,loopback,enabled}` (EnvOrIniBool), `voice.{mic_device,output_device}` (DeviceCombo). | `fonts.cpp:50-56`; `voice_chat.cpp:119,121,171`; `voice_panel.cpp:203,205` |
| **F42** | **The HUD root is ungated** (zero ini reads in `imgui_overlay.cpp` + `hud.cpp`); the overlay installs at harness BOOT (present-hook, first frame — draws at the main menu pre-session). **`multiplayer_menu` IS ini-gated** (`multiplayer_menu_off`). | `harness.cpp:469`; `multiplayer_menu.cpp:310` |
| **F43** | `ReadPlayerSkin` has the **same mint shape** as guid (read → invalid → mint → write). `WriteIniValue` is `void`; the guid log prints **"persisted" unconditionally — false on a locked file**. | `config.cpp:390-434`, `:107`, `:431` |
| **F44** | **Empty env = unset at both product resolvers of the shared vars**: `MASTER_URL` (`m.empty() ? default : m`, `session_manager.cpp:136-137`) and `net.role` (`role.empty() → ini`, `config.cpp:314-315`). The 19 autotest `NET_ROLE` sites are **role discriminators** (`!= "client"`), a deliberate dev bypass, not truthiness reads. | read 2026-07-25 |
| **F45** | The ~46 outside-config.cpp env reads are **~90% harness/probe triggers** (`RUN_*`, `TEST_*`, `AUTOTEST_*`, `RACE_*`, menu/spawn/scoreboard triggers) with no ini twin; the user-facing remainder: voice singles (single resolver each, read beside their ini keys) + the F21 duplicates. `NET_NICK` is read only at `config.cpp:385`. | census 2026-07-25 |
| **F46** | **No other in-product parser of `multivoid.ini` exists** — measured by artifact shape, not call census: every mention outside `config.cpp` is a UI help string (7 hits, all `TextWrapped`/`TextDisabled`); zero raw file-open primitives (`_wfopen`/`fopen`/`CreateFile`/`fstream`) touch an ini path outside `config.cpp`; the loader proxy (`loader/xinput_proxy.cpp`) contains no `.ini` reference at all. | grep by artifact shape 2026-07-25 |

## 3. Why this grew from a layout request

The first hand-written sketch got **4 of ~10 defaults wrong** (`net.port` 7777 vs `kDefaultPort=47621`;
`ui.scale` 1.0 vs `"1.25"`; `net.master` empty vs `DEFAULT`; `ui.font.*` as one key vs five roles with two
defaults). By F4 a seeded key overrides the code default → a seeder carrying transcribed defaults is a
second source of truth that silently wins the drift. That forced the registry; F24 showed the drift bomb was
already assembled in the repo, merely undelivered.

## 4. The design

Everything below survived passes 2-3. Items the passes deleted are in §8.

### The unified occurrence rule (pass 3's core move)

**The authoritative line of key K = the FIRST occurrence of K by CASE-INSENSITIVE key equality.** ONE rule
shared by the reader, the writer, and the T10 report. The two value layers (string resolve / truthiness)
differ ONLY in vocabulary, never in line selection. F25's first-RECOGNIZED-value rule **dies as legacy** (it
silently swallows garbage — the opposite of the third ask). Safety measured: F40 (no ci collisions between
distinct keys, no case twins, rig clean).

**T1 — the fresh ini is a skeleton.** Ordered section headers — `[net]` first, `[dev]` last — and
**zero default values** (F4) with exactly ONE exception, user-ruled 2026-07-25: the skeleton seeds
**`net.nick=Pelmentor` as an active line under `[net]`** (registry `kind = seeded-active`; the joke is meant
to be SEEN and replaced by the player — "оно должно не навязываться, игрок поменяет это на свой ник"). **No banner marker** (deleted in pass 3: its only consumer was the dropped
auto-migration; T3 discriminates by section header per-key; emitting a consumer-less marker is RULE-2-at-birth).
The **section-order table lives in the registry TU, which is BORN in arc 1** carrying exactly two things —
that table and the T7 MY-NAME constant (both single-source config data); skeleton and T3 placement both
consume it — no transcription exists in any commit. (Per-KEY section membership is a T2-enum column, born
in arc 2: in the arc-1 work-state the T3 MOVE is simply inert — every write takes today's placement — and
it activates when T2-enum lands, within the same release.) Existing files are
never re-formatted/re-ordered automatically (single-key writes rewrite the file atomically as they always
have — that is `WriteIniValue`'s job, not a reformat). Seeder runs before `harness.cpp:117` (F23).

**T1b — the owner's opt-in reformat** (button on T10's panel — NOT `dev_menu`, F35/F42). Rules:
- **AS-BUILT AMENDMENT 2026-07-26 (`a05b14e5`), user bug report: layout-only was a DEAD BUTTON.**
  Pressing "Tidy up" moved lines around but left the panel's own complaints (unknown keys, invalid
  values) as LIVE lines, so the post-press re-sweep reproduced identical rows, the panel never closed,
  and it returned every launch — with zero feedback either way. The reformat now RETIRES the fixable
  classes: an unknown key (every occurrence) and a single-occurrence known key whose value fails typed
  validation become tagged comments (`; unknown key (tidy): ...` / `; invalid value (tidy): ...`) —
  data stays readable, the complaint dissolves. Classification uses the sweep's OWN authorities
  (`config_registry::IsKnownKey` + `config::ValueValidForKey`), never a second opinion; differing
  duplicates stay untouched (commenting their first line would silently flip the winner). The panel
  now prints the outcome (counts, or a red locked-file line) and closes itself when the re-sweep comes
  back empty. Covered by config-selftest Drill H (6/6, the user's literal `posinfo=1` case);
  `ReformatLiveIni` split to a path-parameterized core + `SelftestReformat` twin, the house pattern.
- collapses ONLY value-identical duplicates (behavior-preserving under any semantics);
- a key with ci-occurrence count N>1 and differing values is **never repositioned and never adjudicated** —
  reported instead ("resolve by hand", both values shown, winner named), with per-row owner buttons
  **keep-N / keep-M** (owner-triggered deletion via the same primitive — the automatic path never deletes);
- moves only N==1 keys; relative order of an un-collapsed pair is invariant under other keys' moves.

**T2 — one declarative registry.** Row = `{key, section, kind, default | computed-marker | env-only-marker,
lo, hi, tokens, gatedBy, comment, envVar}`. Rows are **KEYS** (denominator: the 109 of F6/F40). The three
F27 producers' key spaces are **included by REFERENCE** (kRoles exported and enumerated compile-time; a 6th
font role yields a 6th row automatically — no transcription). **The registry row IS the typed handle**: after
arc 3 the only read API takes `const Row&` — a future producer cannot mint an unregistered key; the ratchet
is symbol visibility (`ReadIniValue` leaves the public header), enforced by the compiler.

**T2 splits across arcs (E1-1):** **T2-enum** (the complete key enumeration — cheap axis) is born in ARC 2;
**T2-migrate** (default ownership: deleting the ~106 call-site literals) is ARC 3. T10's unknown-key report
consumes the enum axis, so unmigrated-but-legitimate keys are never reported unknown.

**T2b** — retire the F21 duplicate resolvers (arc 3; `session_manager`'s is measured semantically equivalent
today (F44) — a RULE-2 cleanup, not a semantic fix; the autotest bypass is dev-only and enumerated).

**T3 — one write algorithm.** Occurrence count by ci key equality. **A MOVE exists only in a file that
CARRIES the key's section header** (a fresh skeleton file, or one the owner reformatted via T1b); **in a
file with no such header the PLACEMENT behaviour is today's** (append at EOF for a new key; no relocation
of an existing line) — but the write's TARGETING is the unified ci rule everywhere, headered or not: the
authoritative line is ci-found, edited, and normalized (the `Enabled=1` divergence class closes in every
file, not only headered ones — enumerated in §7). In a headered file: **MOVE only at N==1; at N>1 edit
the authoritative line in place** — after the write it carries canonical vocabulary, so both layers converge
on it; moving past a duplicate would hand victory to the un-written line. **Normalize
the key spelling of the rewritten line to canonical** (safe: F40) — the rewritten line ONLY; all other bytes
verbatim (F31). The rewritten line's inline comment is deleted — today's behavior (F36), kept and enumerated
(§7): the comment described the OLD value. The automatic path never deletes a line; deletion is T1b's.
Closes a live class: today's case-sensitive writer MISSES `Enabled=1` when writing `enabled`, appends a
second occurrence, and the two readers then disagree from one write.

**T3b — writers go through the registry** (section, clamps, occurrence count). Never persist a value the
read would reject.

**T4 — one line-primitive, one lexer, two value layers.** The **unbounded line-primitive is ARC 1's first
commit** (one function, three consumers — retiring 128/256/512 and the F8/F31 phantom class before T3 leans
on occurrence counts; measured safe: F39, zero verdict moves). The primitive returns a **TRI-STATE**:
value / ABSENT / UNREADABLE — where ABSENT is authoritative **only on a clean end of stream** (`feof` and
not `ferror`; open failure discriminated by errno, F37; any `ferror` → whole-file UNREADABLE, F38's fix).
Above the lexer: the string resolve and the truthiness read share occurrence selection (the unified rule)
and differ only in vocabulary. Parse semantics flip **atomically** for all sites (everything funnels through
`config.cpp`'s two functions; no other in-product parser exists).

**T5 — comment stripping lives in the lexer**; the whitespace-preceded narrowing applies to the string layer
only (F18); the flag layer keeps the unconditional cut.

**T6 — validation sits AFTER layer resolution.** `Resolve(key) = env → ini → default`, then clamp. One
vocabulary — `1|true|yes|on` / `0|false|no|off`, case-insensitive; **everything else, including
present-but-empty ini values (F33), is garbage → default in memory + report, never written.**
**Env garbage SHADOWS a valid ini value** (chosen: the operator set env to override the ini; falling through
would honor exactly what they overrode). **Empty env = unset** (falls through — matches today, F44).
**Write exception invariant (scope: SELF-INITIATED writes):** of its own initiative the mod persists only
what nothing else defines and what must survive a restart — exactly `player_guid` + `player_skin`. Every
other write site (22 of F6's 24 — the guid+skin mints are themselves 2 of the census: nick, fonts, devices,
UI toggles) persists an explicit user action — user state, always legitimate, and it goes through the
registry (T3b). **Mint gate (both computed keys, F43):** mint-and-persist ONLY on
authoritative ABSENT; on UNREADABLE the identity is session-only and `WriteIniValue` is never called
(closes the lock-release overwrite race). `WriteIniValue` returns `bool`; the F43 false "persisted" log is
fixed (SESSION-ONLY logged truthfully). `net.nick` clamps at 255 bytes (F14).

**T7 — `Pelmentor` is the default for MY NAME** (F19): ONE shared constant consumed by all five my-name
sites; someone-else's-name sites untouched.

**T8 — `multivoid.ini.example`** (arc 4). Generated at launch, atomically, beside the DLL; all lines
commented; comments hard-wrapped ~100 cols; env var named per row; per-launch env as reference text only
(F20); never read as config; does not ride `WriteIniValue`. Key list complete immediately; default ownership
migrates per key (unmigrated = explicit placeholder). **The catalog's DONE criterion (no placeholder) requires
arc 3**; printing with placeholders earlier is possible but not DONE.

**T9 — STUN is a separate arc** (no hole, F9). Residue unchanged: the dead Google literal
(`config.cpp:283`), the missing empty-ICE fallback in `session_manager`.

**T10 — the config review.** A **persistent-until-dismissed panel on the HUD root** (F42: measured ungated,
boot-init, draws at the main menu; NOT `multiplayer_menu` — measured gated; NOT `dev_menu` — F35 circular).
**Re-arms every launch while any row lives; dismissal is in-memory, session-local** (no persisted state —
the "one-time" wording was residue of the deleted migration notice). It reports, never rewrites. Rows:
- **rejected values**, each carrying its ORIGIN LAYER ("`VOTVCOOP_X` (env) = 'abc' — not a number; using 1.25");
- **unknown keys** (vs T2-enum; the F28 dead `ui.font=fixedsys` is the canonical catch);
- **duplicate-dormant** (differing values; winner = authoritative line, named; both values shown;
  owner keep-N/keep-M buttons; for identity keys the text adds "if this identity is not the one you expect,
  keep line M");
- **identity not durable** — covers file-present-but-unreadable AND mint-write-failed: temp guid this
  session, a host-side `coop_players/<guid>.json` orphan per such launch, full recovery on the first durable
  persist (jsons are never deleted). **UNREADABLE at ANY point (open failure or mid-stream `ferror`) yields
  the same outcome for every resolve: the ini layer drops out, the launch runs on env+defaults, and this row
  shows** — no branch produces an all-defaults session with a clean panel.
The report's data comes from a **boot-time file-vs-schema sweep** over T2-enum (values validated statically
off the row's kind — even for never-read gated keys, F11); the panel reflects BOOT state; mid-session hand
edits are reviewed next launch (§7). The in-memory default substitution still happens at each read site.

**T11 — latch the two unlatched flag reads** (F34) with the in-tree `call_once` idiom. No ini snapshot.

**Arc-2 completeness rule (kills any precedence window):** ALL product typed twin-layer keys convert to
`Resolve` in arc 2 (the F30 typed set + the six F26 b/c/d call sites — `harness.cpp:122`,
`peer_action_feed.cpp:25`, `voice_chat.cpp:86`, `session_runtime.cpp:205`, `net_stats_panel.cpp:29`,
`config.cpp:225`). Free-string keys can wait for arc 3: shadow-vs-fallthrough is unobservable where no value
is rejectable. **INVARIANT: T10 and the read semantics it reports land in the same arc/release** — no
reported-clean/behaves-wrong window exists in any shipped state.

## 5. Verification

**One shared corpus fixture for every instrument** — the 4 rig inis + injected positives, one per enumerated
class: duplicate with differing values; value-identical duplicate; case-variant duplicate
(`Player_Guid=` + `player_guid=`); identity-key case-variant dup (stale-above-live, expected outcome: stale
wins + T10 row shows both); `=yes`/`=true`/`=on` flags; present-but-empty; 380-char line; a line whose tail
chunk equals a key under write; CRLF; no trailing newline; `=` inside a comment; the live phantom key; the
dead `ui.font=fixedsys` (must-REPORT unknown); garbage-above-valid duplicate (default + two rows); a single
wrong-case key line (occurrence flip made visible). A run that reports zero must first prove it can report
non-zero.

| Step | Instrument |
|---|---|
| T4 | **BOTH layers' verdicts per key**, before vs after, + a multiset of all non-key lines. Must-FAIL controls flip a real flag verdict. The 380-char/tail-chunk cases run against the **arc-1 writer**, not only the lexer. |
| T3 | same + occurrence-count report + MOVE-with-duplicates (defined behavior: must-NOT-move, edit-first) + **WRITE-THEN-READ same-line assertion** (write X → resolve X → value equality; "no bytes lost" is blind to write-line-A/read-line-B). |
| T2 | `{key → default}` dumped before/after the registry move and compared. **Enum-completeness instrument**: tree-parse of all read-API call sites; literal args extracted as a KEY SET; a non-literal arg MUST match one of the three F27 producers, else FAIL-closed; diff vs the registry dump (which already carries the producer tables by reference). Two must-FAIL injections: an omitted key; an unknown producer. Retires when arc 3's ratchet makes it structurally redundant. |
| T4-tri-state | **unit fault-injection** for the mid-stream `ferror` branch (the primitive reads via a callback seam; test injects failure after N lines; must-FAIL: the branch returns UNREADABLE, not ABSENT). The open-time errno control exercises a different guard and does not count for this one. |
| T6 | per-key value-space table per F26 shape: the tokens whose verdict moves. |
| T8 | every row exactly once; no computed row prints a value; no env-only key in copyable form; all lines commented+wrapped; round-trip (uncomment → parse with our own parser → equals registry defaults). |
| T7/T10 | **SMOKE**: fresh temp install, `net.nick` absent → log + wire carry `Pelmentor`. |

The BEFORE half may be simulated offline; the AFTER half must call the real C++ lexer.

## 6. Arcs and order

**Arcs 1+2 are WORK order but ONE RELEASE** (E8-2): the ci-writer/cs-reader window and every intermediate
state exist only in the dev tree, never in shipped bytes.

| arc | contents | size |
|---|---|---|
| **1** | **BUILT 2026-07-25** (commits `16c8a448` primitive+selftest TU, `96f68f5a` registry TU+T7, `938bbac9` T1 seeder, `41712605` T3 writer+bool, `9b6982a6` T3 write drills). Instrument PASS via the REAL in-game lexer over 4 rig inis + inject corpus: exactly 4 verdict moves, all the enumerated F8 buffer-split class (longline/longcarrier/join_window_pos_trace×2 whole now); only-in-before 0; T3 drills 11/11; ferror fault-injection Unreadable-not-Absent proven; `config-selftest: DONE fail=0`. **AS-BUILT DEVIATION from F19/T7 (user guard, mid-build): `player_handshake.cpp` SanitizeNickname's empty fallback KEEPS `"Player"`** — measured symmetric (it sanitizes inbound REMOTE nicks too); a garbage remote nick must not render as the my-name default. 4 of the 5 F19 sites consume the constant; the browser literal was DEAD (always loaded on open) and became `{}`. Writer gained a NEW guard beyond the design text: mid-stream read error ABORTS the rebuild (the 2026-07-02 loss class one layer deeper). | done |
| **2** | **BUILT 2026-07-25** (commits `06e9910d` T2-enum registry TU (103 rows) + enum-completeness instrument `tools/config/enum_check.ps1` (PASS 103==103, both must-FAIL controls proven; first catch: `interactable_log` missing from the hand census); `0c0a1ad8` T4 layer flip; `8676a85a` Resolve{Flag,Int,Float,Enum} + all F26/F30 conversions + T3b writer validation/placement + T11 latches + the 255-byte nick cap; `a349886a` mint gate; `1e884135` T10 sweep + settings-check panel + T1b reformat + keep-line dedup; `dbd9d549` the C6 soft-cap extraction (config.cpp 1171->762 + config_ini_write.cpp 431 via the TU-private config_internal.h seam); `90b43f82` T3b/T4 drills). **Evidence:** corpus gate AFTER == NEWSIM (an independent PS reimplementation of the unified rule) on ALL 248 (file,key) verdicts over 4 rig inis + inject corpus; BEFORE(arc-1)->NEW delta list = 56, every one in an enumerated class (identity stale-wins x1, VOCAB x3, GAV x1, T5-STRIP x5, CASE = the rest — comment-text pseudo-keys + a space-carrying phantom key, no product key in either shape, F39/F40); **zero real-key flag verdicts moved on the rig inis**. Drills: T3 11/11 + arc2 15/15 (placement/MOVE/refusal/vocabulary) + fault-injection x2, `config-selftest: DONE fail=0`. Sweep armed live on both peers (HOST row = the dead `ui.font=fixedsys`, the F28 canonical catch). Smoke PASS x2 (one with selftest, one clean). **AS-BUILT DEVIATIONS:** (a) numeric out-of-range = GARBAGE -> DEFAULT + report, NOT clamped — T6's "then clamp" lost to the user's verbatim ruling "не чинить, а дефолт ставить"; the sweep row names the range; (b) registry rows carry NO defaults yet (sites still pass them; T2-migrate = arc 3), so the panel says "the built-in default is used" without naming the value — the design's "using 1.25" wording arrives with arc 3; the gatedBy/comment columns are NOT born (consumer = the arc-4 catalog; the banner-marker RULE-2-at-birth argument); (c) numeric/enum RANGES are row-owned NOW (ui.scale's [0.75,1.75] consumed by both the scale clamp and the F1 slider — their literals deleted); (d) the voice.* five became LITERAL rows (only the fonts family remains composed-by-reference; fonts consumes the registry's kFontRoleKeys/kFontFamilyTokens with static_assert lockstep); (e) the panel is a centered modal (boot_warning shape) drawn under the boot-warning dialog. **AUDITS FOLDED (`7f1765ea`):** perf agent PASS 0 CRITICAL (all 26 Resolve sites latched/boot; T11 = net hot-path gain; FindRow O(103) per frame in the F1 slider = sub-µs); correctness agent found 2 CRITICAL + 1 IMPORTANT, all fixed at the root -- CRIT-1 line-splice on section insert after a newline-less final line (ONE post-scan normalization, branch-local copy retired), CRIT-2 keep-line by stale line NUMBER could delete both copies of a duplicate identity key (now correlated by VALUE, refuses on a vanished value, re-sweeps on both outcomes), IMP-3 composed ui.font.* keys skipped section placement (ONE SectionForKey shared by writer + reformat) -- each with its own drill (E/F/G), re-proven on final bytes `B66A8CB084BC5FBC` x4: smoke PASS, drills 24/24 arc2 + 11/11 T3, DONE fail=0, corpus gate re-run 248/248. Perf residue noted (not this arc): a handful of pre-existing unlatched IsIniKeyTrue sites elsewhere (event_feed:182, save_transfer:441/479, ...) = the same F34 shape T11 fixed -- a follow-up latch sweep candidate. | done |
| **3** | **BUILT WHOLE + VERIFY GREEN 2026-07-25 night** per the impl design `votv-ini-arc3-impl-DESIGN-2026-07-25.md` (/qf 16r "that holds"; READ its Verify section for the full evidence). Commits: C1 `f88a78cf` typed row defaults aliasing the owning constants + .inc X-macro handles + constexpr ValidateRows; C2 `faa0289d` handle-only Resolve + ResolveString + fonts ENUM rows; C3a `3b9aba38` 62-site flag sweep, IsIniKeyTrue DELETED; C3b `1a7c70fb` handle-keyed WRITE door (15 sites; IdentityRow write-only handle); C4 `beb73208` T2b both F21 duplicates retired (g_configured branch + 24 autotest role-env reads → one latched IsClientRole(); bespoke-chain env literals ride row->envVar); C5 `ad15ae7c` registry_gate.ps1 standing CI gate (build-core.yml, both lanes) + **enum_check.ps1 RETIRED** behind the verified inheritor map + `ce035619` typed-resolver selftest twins + drills; `fed851cb` config_selftest.cpp cut (config.cpp 770). Ratchet closed BOTH directions, compile-proof C2664/C2665 recorded. Evidence: AFTER-compare 100/100 defaults equal (+ fonts [3,3,1,1,3]); corpus gate 248/248; drills 15/15 fail=0; gate drills RED×5 real-identifier + green; smoke ×2 PASS (`2ba9014653033cd5` ×4); perf audit 0 CRIT/0 WARN. NOT hands-on. | done (not hands-on) |
| **4** | **BUILT 2026-07-25 night** per `votv-ini-arc4-T8-catalog-impl-DESIGN-2026-07-25.md` (/qf 12r "that holds"): `fd3481cd` the generated multivoid.ini.example (registry rows gain desc ×108 + structured gatedBy ×2, ValidateRows compile gates incl. the defS catalog-safety guard; generator TU config_example.cpp owns the `;;`/`;`/bare-header grammar for emit AND verify; deterministic C-locale %.9g bytes, tri-state compare-first, per-boot ExampleGenStatus, fail-soft field lines; SelftestExampleVerify = 6 detectors + FOUND+typed-equal round-trip on the product cores) + `0bbc0525` F34 latch sweep + `42fabf77` audit fix (the shared atomic writer goes BINARY — text-mode CRLF made compare-first permanently false). DONE criterion met: zero placeholders (every default row-owned since arc 3). Drill green on live boot bytes (9 controls fire + locale canary), smoke-verdict machine assert (RELEASE.md step 0), CI RED drill on the real lane (run 30168118925 red exactly at the gate step). NOT hands-on. | done (not hands-on) |

Arc 1 reaches only fresh installs; **the requester's own existing HOST ini gets its `[net]`-first order via
T1b's button in arc 2** — stated honestly, that is where the verbatim ask is delivered to its author.
(As of the arc-2 build the button exists on the settings-check panel; the HOST ini's live sweep row — the
dead `ui.font=fixedsys` — arms the panel, so the reformat is one click away on the next HOST launch.
NOT hands-on yet: the panel/reformat UX rides the next take.)

## 7. Consequences said out loud (choose-and-enumerate; no old-semantics predicate is compiled anywhere)

- **Vocabulary flips:** `ui.netstats=true` false→true; `peer_actions=off` true→false; `enabled=no` → dev off;
  `nameplate=false` true→false; `voice.x=` (present-empty) true→false. Each either matches what the user
  literally wrote, or is garbage → T10.
- **Occurrence flips (new in pass 3):** a wrong-case line wakes (dead today for the string layer) — no T10
  row, by the matches-what-user-wrote criterion; the sharpest member: a wrong-case duplicate on an identity
  key (`Player_Guid=stale` above `player_guid=live`) flips WHICH value wins consistently — T10's duplicate
  row fires the same launch but POST-FACTUM (the guid keys the session at boot, the panel renders later);
  downstream: the `<guid>.json` inventory is orphaned, with FULL recovery once resolved (jsons are never
  deleted; skin lives in the ini itself, unaffected). Rig measured clean (F40); doubly rare (needs case
  variant AND duplicate); no per-key semantics split (that would resurrect F25's mess).
- **Garbage-above-valid duplicate:** today silently reads the valid line (first-recognized); tomorrow the
  authoritative (garbage) line → default + TWO T10 rows (rejected + duplicate-dormant).
- **The rewritten line's inline comment is deleted** on any value write — pre-existing (F36), kept: the
  comment described the old value.
- **In a headered file, a UI write relocates a pasted-at-EOF line under its section header** (the MOVE);
  files without our headers never see relocation.
- **A write now ci-targets a case-variant line and normalizes its spelling** (instead of appending a
  duplicate beside it) — in ALL files, headered or not; this is the closure of the `Enabled=1` one-write
  reader-divergence class.
- **Env garbage shadows a valid ini value** → default + a T10 row naming the env origin.
- **Empty env = unset** (falls through; matches today, F44).
- **The panel reflects boot state**; mid-session hand edits are reviewed next launch.
- **The panel re-arms every launch while any row lives**; dismissal is per-launch. A real unresolved conflict
  is shown until resolved — the in-panel keep-N/keep-M buttons are the exit.
- **Identity not durable** (unreadable ini OR failed mint write): a fresh guid per such launch, host-side
  json orphans, full recovery on the first durable persist.
- **Every unconfigured peer becomes `Pelmentor`** (F15); the activity feed renders nicknames, never "You" —
  several Pelmentors are indistinguishable, same as today's `Player`. Fresh installs additionally SEE the
  line (`net.nick=Pelmentor` seeded active, user-ruled); the seeded value pins past a future default change —
  accepted, ≈0 cost for a nick.
- **Our dev rig loses attribution** unless the four installs get distinct `net.nick` lines.
- **Population honesty:** every corpus claim covers the 4 rig inis; the repo is public; no census of user
  files exists and none is claimed.

## 8. What was dropped, and by which round

Pass 1: seeding active values (F4); seeding commented values (second copy, indistinguishable from user
notes); automatic migration of existing inis (→ T1b); STUN (F9); a C++ call-site parser; typed handles
across all keys as a separate table.

Pass 2: the meaning-change report + its persisted state + deferral + four legacy predicates (the §7 two-case
argument); the in-memory ini snapshot (T11's `call_once` closes F34 for two lines); the ratchet as an
external list (→ symbol visibility); re-shaping `boot_warning_dialog` (filed as its own finding); four of the
primary's own answers (vocabulary narrowing D7; the "promise already made" union D9; the token-delta table
D12; the blind-corpus "zero flips" D13).

**Pass 3:** the banner marker (E1-2: no surviving consumer — RULE-2-at-birth); F25's first-RECOGNIZED rule
(E4-1: replaced by the unified occurrence rule); T10's "one-time" persisted latch (E5-3: once-per-launch
in-memory); the arc-1-alone release (E8-2: fused with arc 2); the absent/unreadable conflation in the reader
(E9-3/E11-1: tri-state with clean-EOF authority); the assumption that composed rows could be transcribed
(E4-2: included by reference); §9's four residuals (a: closed by deletion; b: closed by measurement F37/F38 +
the mint gate; c: dissolved by the F45 classification + F44 equivalence; d: catalog = all INI keys, harness
env as reference text, header says so).

## 9. Convergence state — CONVERGED ("that holds", 2026-07-25)

Pass 3 ran E1-E15 against the pass-2 rewrite, then this rewrite, then confirmation rounds E16-E18 (each
producing only doc-coherence/labeling items, closed by measurement and folded in) and **E19, which returned
a genuine "that holds"**: every §1 ask piece maps to a delivered mechanism, §7 matches T3/T6/T10
case-for-case with a §5 fixture per class, no contradiction or unmeasured load-bearing claim found.
**The design is certified; the build (arcs 1+2 as one release) is unblocked.** The 'стоит' product question
was ANSWERED by the user 2026-07-25 (§1): the fresh ini seeds `net.nick=Pelmentor` as a visible, editable
active line — the one `seeded-active` registry exception. No open questions remain.

Status header note: the header's "formal that-holds pending" is superseded by this section.
