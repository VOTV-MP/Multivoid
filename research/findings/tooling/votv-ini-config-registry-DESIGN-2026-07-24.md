# multivoid.ini — seeder, config registry, validation, catalog (DESIGN)

**Date:** 2026-07-24 · **Status:** DESIGN, not built · **Passes:** `/qf` pass 1 = 16 rounds (did NOT
converge); `/qf` pass 2 = 15 rounds against THIS document (did NOT converge at the cap; four of the
primary's own answers were withdrawn mid-pass) · **Scope consent:** user, 2026-07-24, after the cost was
re-opened: *"мне пофиг, хоть месяц даже если потратим на это"* — full scope authorized, arcs 1-4.

Design of record for the config layer rework. **Read §2 before touching any part of it.** Four of the ten
defaults in the first hand-written sketch were WRONG, and that is what turned a layout request into a
source-of-truth problem; pass 2 then found that several of the doc's own claims were wrong too.

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

**Ruling during pass 2** — on `release/votv-coop.ini`, an orphan example ini in the repo that already
carried the exact requested layout (see F24):

> "Не надо ничего никому доставлять, это старый артефакт, наш мод будет делать свой ini правильным образом."

Settled: `Pelmentor` is a joke and stays; the three surfaced product points are accepted; the
commented-vs-active catalog choice was delegated to the primary; the orphan file is DELETED (RULE 2).

**Drift, stated up front:** requirement 3 ("dev flags last, out of the way") ends with almost no
observable subject, because a fresh ini contains no dev keys at all. It is satisfied in form (`[dev]` last
in the skeleton and in the catalog), but its intent was already satisfied by a different means.

## 2. Measured fact base

Rows F1-F23 were measured in pass 1; **F24-F35 in pass 2, against the current tree.** Nothing is carried
from a doc. Rows CORRECTED by pass 2 are marked.

| # | Fact | Evidence |
|---|---|---|
| F1 | No seeder exists. The ini appears as a side effect of the first `WriteIniValue`, which appends to a nonexistent file. | `config.cpp:166-172` |
| F2 | On a clean install the file after first launch holds **exactly two lines** — `player_guid` and `player_skin`. Everything else is UI-driven. | `harness.cpp:117`, `:120` → `config.cpp:419-431`, `:397-403` |
| F3 | **Sections are decorative.** `ParseIniLine` splits on the first `=`; `[net]` has none, so it is skipped. | `config.cpp:77-83` |
| F4 | `ReadIniValue(key, def)` returns `def` **only when the key is ABSENT**. A present-but-empty key returns `""`. So a seeded key OVERRIDES the code default. | `config.cpp:96-104` |
| F5 | `WriteIniValue` replaces a key's line in place only if the key exists; otherwise it **appends at EOF**. | `config.cpp:166-172` |
| F5b | ~~Reader and writer both take the FIRST match.~~ **FALSE AS A LAW — see F25.** | corrected by pass 2 |
| F6 | ~100 ini keys. 109 read sites across 60 files; 24 write sites across 10 files. Some keys are composed at runtime (F27). | grep census |
| F7 | `WriteIniValue` was hardened after the **2026-07-02 data loss**: abort if the file exists but is read-locked, `.new` + atomic `MoveFileExW`, every write checked before the swap, one process-wide mutex. | `config.cpp:107-200` |
| F8 | **THREE buffer sizes for one file format:** `char[128]` in `LookupTriState`, `char[256]` in the reader, `char[512]` in the writer. A 380-char line in CLIENT_1's ini splits, and its tail contains `=`, so a **phantom key exists live in 2 of our 4 inis**. | `config.cpp:461`, `:98`, `:152` |
| F9 | **STUN is not a hole.** The live production master returns a `stun:<our own coturn>:3478` URI. `COOP_STUN_URI` **is** set in production. | POST `/v1/host` against the live master |
| F10 | Production TLS is healthy. The "certificate has expired" error hit first was a **stale local Python CA bundle**, not an outage. | `openssl s_client`, `curl` |
| F11 | A runtime walk of keys is **structurally incomplete**: `desk_diag_ms` is read inside a block gated on `IsIniKeyTrue("desk_diag")`, so a normal launch never reads it. | `desk_diag.cpp:52` vs `:134` |
| F12 | The typed-read pattern already exists by hand at one site: `if (ms<100) ms=100; if (ms>60000) ms=60000;`. | `desk_diag.cpp:53-55` |
| F13 | The nickname is **never an identity key** — a per-slot display string. The ban list keys by IP only. | `ban_list.h:9-13` |
| F14 | **Config reaches the wire.** `nicklen` is `uint8` ⇒ any `net.nick` validator must cap at 255 bytes. No protocol bump is needed (a default value is not a FORMAT change). | `protocol.h:1240-1246` |
| F15 | Deployed `net.nick`: HOST `Pelmentor` (the user set the joke by hand already), CLIENT_3 `DevPeer`, CLIENT_1/2 **ABSENT**. | the 4 real inis |
| F16 | Inline comments live inside values; only the flag reader tolerates them. No key carrying an inline comment is read by strict `ReadIniValue`, so the hazard is **latent**. | the 4 real inis |
| F17 | **ENV is a config layer that BEATS ini.** 16 `VOTVCOOP_*` reads in `config.cpp`; **46 more outside it**. | grep census |
| F18 | No legitimate `;` inside any value in the 4 real inis; all four inline comments are **whitespace-preceded**. | grep census + pass-2 audit |
| F19 | The nick literal's real split is **MY NAME vs SOMEONE ELSE'S**: mine = `config.cpp:386`, `player_handshake.cpp:37`, `:219`, `session_manager.cpp:61`, `server_browser.cpp:46`. Someone else's = `scoreboard.cpp:115` (already splits local vs remote), `peer_action_feed.cpp:52`. | grep census |
| F20 | `VOTVCOOP_SCENARIO` is deliberately **env-only**; `scenario.txt` was retired 2026-06-06 because a file aliases the NEXT native launch into gameplay. | `config.cpp:44-58` |
| F21 | **Duplicate env resolvers:** `VOTVCOOP_MASTER_URL` in `config.cpp` AND `session_manager.cpp`; `VOTVCOOP_NET_ROLE` in `config.cpp` AND 19 `autotest_*.cpp` — and `net.role` IS an ini key, so the autotests bypass that layer. | grep census |
| F22 | `net.nick=` (present, empty) yields `""` and `SetLocalNickname` sanitizes it to `"Player"`. **Sharpened by F33.** | `config.cpp:386` → `player_handshake.cpp:219` |
| F23 | The first writes on a clean install are `ReadPlayerGuid` and `ReadPlayerSkin`. **A seeder must run before `harness.cpp:117`** or the file is born without the banner. | `harness.cpp:117`, `:120` |
| **F24** | **`release/votv-coop.ini` exists in the repo** — a user-facing example carrying `[net]` first, `net.nick=Pelmentor`, `net.master=DEFAULT`, `net.port=47621` as **ACTIVE values**, `[dev]` last: the exact layout the user asked for. Measured: **nothing deploys it**, no `RELEASE.md` line mentions it, last touched 2026-06-23 (pre-rebrand, still named votv-coop). It is also the ONLY place `net.master.custom` was ever documented. User ruled: DELETE. | `release/`, `git log -- release/votv-coop.ini` |
| **F25** | **The two readers have DIFFERENT first-match rules.** `ReadIniValue` breaks on the first matching **KEY** whatever its value; `LookupTriState` breaks on the first **RECOGNIZED VALUE**, skipping a `key=garbage` line above a `key=1`. It is also case-INSENSITIVE on the key (the string reader is case-sensitive), strips inline comments (the string reader does not), and **takes no `g_iniMutex`** — so the comment at `config.cpp:85-89` ("Readers take it too") is FALSE. | `config.cpp:96-104` vs `:457-473` |
| **F26** | **FOUR truthiness shapes ship today** (grep census, not eyeball): (a) `LookupTriState` = `1\|true\|0\|false`, whole normalized line, 58 keys; (b) `!= "0"` accepting **anything** as true — `harness.cpp:122`, `peer_action_feed.cpp:25`, `voice_chat.cpp:86`; (c) `== "1"` strict, where `true` reads FALSE — `session_runtime.cpp:205`, `net_stats_panel.cpp:29`; (d) the 4-token chain `1\|true\|yes\|on` — `config.cpp:225`. Live consequence: **`nameplate=true` works, `ui.netstats=true` silently does not.** | grep census |
| **F27** | **Composed-key producers are exactly three**, all machine-enumerable one indirection deeper: `fonts.cpp:72,252` (`ui.font.<role>`, table-driven), `voice_chat.cpp:85` (`EnvOrIniBool(env,iniKey,def)`), `voice_panel.cpp:60,67` (`DeviceCombo(label,iniKey,…)`). No other non-literal key argument exists in the tree. | grep census |
| **F28** | **`ui.font=fixedsys` sits in the requester's own HOST ini and is read by NOBODY** — the code composes `ui.font.<role>`; no site reads the bare key. A plausible key silently doing nothing, on the requester's machine. | `fonts.cpp:70,251` + the HOST ini |
| **F29** | **SIX default shapes**, not two: inline literal at the read (33 of 50 `ReadIniValue` refs); `""` meaning genuinely-none (~9); `""` as a **sentinel** with the real default downstream (`net.port` → the `coop::net::Config` struct member, `config.cpp:321-332`; `voice.*_key` → `ParseKey(x,0)`); **absence = false** (`IsIniKeyTrue`, 69 refs / 52 files); **absence = TRUE** (`MasterEnabled`); computed (`player_guid`, `player_skin`). | grep census |
| **F30** | Range census over the 38 literal string keys: **10 numeric** (`desk_diag_ms`, `vitals_keepalive_sec`, `ui.scale`, `net.port`, `voice.distance_cm/jitter_threshold/mic_gain_db/prebuffer_frames/threshold_db/volume`), **4 enum** (`voice.mode`, `net.topology`, `net.ice`, `nick_color`), **5 string-booleans**, the rest free strings. Exactly ONE site clamps today. | grep census + `desk_diag.cpp:53-55` |
| **F31** | **`WriteIniValue` is LOSSLESS on long lines**: `fgets` chunks are pushed verbatim and re-emitted with no injected newline, so a split line re-concatenates byte-for-byte. The doc's original T4-first justification ("the annotated format pushes lines toward the writer's limit") was **unmeasured and wrong in mechanism**. The REAL corruption path: `ParseIniLine` runs **per CHUNK** in the writer, so a long line whose TAIL chunk parses as the key under write gets that chunk replaced and a newline spliced into the middle of the user's line. | `config.cpp:152-165`, `:182-184` |
| **F32** | **A MOVE can flip a flag.** With `enabled=1` … `enabled=0`, moving the first occurrence below the second changes which one is "first recognized" (F25) and the verdict inverts. So a reformat must **COLLAPSE first, then move.** | derived from F25, `config.cpp:467-471` |
| **F33** | **Present-but-empty turns a default-off key ON.** `EnvOrIniBool` reads `ReadIniValue(key, def?"1":"0")`, so `key=` yields `""` and `"" != "0"` is TRUE **regardless of the site's default**. | `voice_chat.cpp:84-86` |
| **F34** | **Two UNLATCHED flag reads open + scan the ini on every call** — `kerfur_form_assembler.cpp:168` (`LogVerbose()`, 5 callers on assembler steps) and `kerfur_convert.cpp:570` (destroy edge). The other ~60 are `static const` latched, and the `std::call_once` idiom already exists in-tree (`peer_action_feed.cpp:23`, `net_stats_panel.cpp:28`). | grep census |
| **F35** | **`dev_menu` is gated on `MasterEnabled() && IsIniKeyTrue("devkeys")`** — so anything hosted there is invisible to the requester unless he hand-writes `devkeys=1` into the very file he asked to reorder. And `boot_warning_dialog::Arm` stores into a **single slot by assignment** (one caller today), so a second arm would silently drop the duplicate-DLL warning. | `dev_menu.cpp:539`, `:624`; `boot_warning_dialog.cpp:29-35` |

## 3. Why this grew from a layout request

The first hand-written sketch got **4 of ~10 concrete defaults wrong**: `net.port` 7777 vs the real
`kDefaultPort = 47621`; `ui.scale` 1.0 vs `"1.25"`; `net.master` empty vs the `DEFAULT` sentinel;
`ui.font.*` as one key vs **five roles with two different defaults**. Two of the four the user caught
himself, from memory of `release/votv-coop.ini` (F24).

By F4 a seeded key overrides the code default. So a seeder carrying its own transcribed copy of the
defaults is a **second source of truth that silently wins the drift**. That is what forced the registry —
and F24 shows the drift bomb was already assembled in the repo, merely undelivered.

## 4. The design

**A note on provenance:** every item below survived pass 2. Items the pass **deleted** are in §8.

**T1 — the fresh ini is a skeleton.** Our banner marker line + ordered section headers, `[net]` first,
`[dev]` last, and **zero default values** (F4). Existing files are never rewritten automatically. Per F23
the seeder runs before `harness.cpp:117`.

**T1b — the owner's opt-in reformat.** A user-triggered action reformats an existing ini, because "never
rewrite existing files" would otherwise leave the person who asked for the order (F15) with nothing. It
**COLLAPSES duplicates first, then moves** (F32), collapsing by the rule of whichever reader actually
reads that key. Surface: the config-review notice's own button (T10) — NOT `dev_menu`, which is gated by
the very file being reordered (F35).

**T2 — one declarative registry.** Row = `{key, section, kind, default | computed-marker | env-only-marker,
lo, hi, tokens, gatedBy, comment, envVar}`. `kind` covers the **six** shapes of F29; `gatedBy` covers keys
read only when another key is true (`net.master` behind `net.master.custom`), whose only documentation was
the deleted F24 file. A row carries **either** a default **or** a computed marker, never both. `SCENARIO`
is an env-only row carrying "ini layer FORBIDDEN + reason" (F20). **The registry row IS the typed handle**
— there is no `ReadInt(key,def,lo,hi)`, because that signature puts the default back at the call site.

**T2b — separate commit, same batch.** Retire the F21 duplicate env resolvers behind one accessor.

**T3 — one write algorithm, no branches.** Find the key's section header; if the file has no such header,
append at EOF (today's behaviour for a bannerless file). Insertion is a **MOVE of the single occurrence,
never an ADD** (F5b's replacement: an occurrence anywhere is edited in place). The automatic path never
deletes a line — deletion is T1b's, done knowingly by the owner (F7 is the seam this protects).

**T3b — writers go through the registry too** (section, clamps, occurrence count). Clamp on write as well:
never persist a value the read would reject.

**T4 — one LEXER, two upper layers.** The shared lexer owns buffering (**unbounded line**, retiring
128/256/512), CRLF, and comment cutting. Above it sit **two distinct questions that do NOT merge** (F25):
the **string resolve** (case-sensitive key, first matching KEY, `;`-cut only when whitespace-preceded per
F18) and the **truthiness** read (case-insensitive, first RECOGNIZED value, unconditional `;`-cut as
today). Merging them under one key-compare would silently change flag verdicts on user files.

**T5 — inline-comment stripping lives in the lexer**, with the whitespace-preceded narrowing applied
**only to the string layer**; the flag layer keeps its unconditional cut, or a live `probe=1;note` flips
ON→OFF.

**T6 — validation sits AFTER layer resolution.** `Resolve(key) = env → ini → default`, **then** clamp
(hanging the validator on the ini read would let env garbage sail past, F17). One CHOSEN vocabulary —
`1|true|yes|on` / `0|false|no|off`, case-insensitive — and **everything else, including present-but-empty
(F33), is garbage → the default in memory, logged, never written.** Exception, with an invariant: *write
only what nothing else defines and what must survive a restart* — exactly `player_guid` + `player_skin`.
`net.nick` is clamped to 255 bytes (F14).

*"No behaviour change" is impossible and is not claimed.* Four shapes ship today (F26); no single
vocabulary preserves all of them. The choice is made on merits, and §7 lists what moves.

**T7 — `Pelmentor` is the default for MY NAME** (F19), landed as **ONE shared constant** consumed by all
five sites — not five literals, or the arc would re-create the very root §3 names. Someone-else's-name
sites are untouched, or a nameless remote peer would be labelled Pelmentor. Nothing is seeded into any
file (F4).

**T8 — `multivoid.ini.example`.** Generated **at launch**, atomically, beside the DLL. **Comments are
hard-wrapped (~100 columns)** — that one policy removes the only path by which this arc would arm F31's
splice. All ini keys, each row carrying its env var name and its `gatedBy` condition. Per-launch env
appears as **reference text, never as a copyable line** (F20). **All lines commented**, with an
instruction header. Never read as config; does not ride `WriteIniValue`, so F7 is untouched.
**Two completeness axes:** the KEY LIST is complete immediately (mere enumeration); DEFAULT OWNERSHIP
migrates per key, and an unmigrated key prints an explicit placeholder, never an empty value (F4).
**The catalog is the stopping rule** — migration is done when no placeholder remains.

**T9 — STUN is a separate arc.** No hole (F9). Residue: the dead Google literal at `config.cpp:283`, and
the missing empty-response fallback — `session_manager.cpp:278/:374/:539` overwrite the whole ICE block
unconditionally, so "the master sent empty" is indistinguishable from "the master did not send".

**T10 — ONE ungated, one-time config review.** Exactly the user's third ask and nothing more: **rejected
values** ("`ui.scale=abc` is not a number — using 1.25"; "`voice.x=` has no value — using the default"),
**unknown keys** (F28 — the registry is the complete key list, so this costs almost nothing), and the
T1b reorder button. It **reports, never rewrites**. Hard order: T4 precedes it, or the buffer-split
phantoms (F8) would be reported as unknown keys.

**T11 — latch the two unlatched flag reads** (F34) with the `call_once` idiom that already exists in-tree.
Two lines; no in-memory ini snapshot is minted, so live ini edits keep working where they work today.

**Per-key migration is RULE-2-clean at every point.** The concept is "the default of key X": delete X's
literal when X's row is created and X has exactly one source, regardless of whether Y migrated. **The
ratchet is symbol visibility, not a list:** `ReadIniValue` leaves the public header, so a new raw site does
not compile; the three F27 producers get one explicit dynamic-key API.

## 5. Verification

**One shared corpus fixture for every instrument** — pass 2 measured that a reduced corpus silently
weakens them (the D13 run dropped to real-inis-only and produced a worthless zero-flip result).
Corpus = the 4 real inis **+ injected positives**: a duplicate key with differing values, `=yes` / `=true`
on a flag, a present-but-empty value, a 380-char line, a line whose tail chunk equals a key under write,
CRLF, no trailing newline, `=` inside a comment, the live phantom key.

| Step | Instrument |
|---|---|
| T4 | **BOTH readers' verdicts per key**, before vs after, plus a multiset of all non-key lines. A `{key→value}` map through the STRING reader alone is structurally blind to the change T4 makes. Must-FAIL controls flip an actual FLAG verdict. |
| T2 | `{key → default}` dumped before and after the registry move and compared. |
| T3 | same shape as T4, plus the occurrence-count report and a MOVE-with-duplicates case (F32). |
| T6 | per-key value-space table: for each of the four shapes (F26), the tokens whose verdict moves. |
| T8 | every registry row appears exactly once; no computed-marker row prints a value as a real default; no env-only key in copyable form; every line commented and wrapped. **Round-trip:** uncomment the catalog, parse it with our own parser, and it must equal the registry defaults. |
| T7/T10 | **SMOKE, not an instrument**: launch with `net.nick` ABSENT on a **fresh temporary install** and assert the log and the wire carry `Pelmentor`. |

**The BEFORE half may be simulated offline; the AFTER half must call the real C++ lexer.** A second
transcription of the format certifying the unification of the format's parsers is the bug under test.

## 6. Arcs and order

Pass 2 measured that **nothing in the verbatim ask depends on the registry**: T7 is one constant; T1's
skeleton writes zero values; without T3 the guid/skin writes append at EOF exactly as today and the header
ORDER still holds. So the order follows the ASK, not the root:

| arc | contents | size |
|---|---|---|
| **1** | T7 (one shared MY-NAME constant) + T1 skeleton + T3 section-aware write | ~a day |
| **2** | T4 lexer + T5 + T6 typed reads (~19 keys of F30) + T10 config review + T11 latches | 1-2 days |
| **3** | T2 registry migration (~109 sites / 60 files) + T2b duplicate env resolvers | ~a week |
| **4** | T8 catalog — **requires arc 3**, or it prints possibly-wrong defaults machine-authoritatively | ~a day |

Also in arc 1: **delete `release/votv-coop.ini`** (F24, user ruling, RULE 2).

The original "roughly a week" was justified by the registry being the root of the ask; pass 2 refuted that
dependency, the cost was re-opened, and the user authorized the full scope regardless
(*"хоть месяц даже если потратим на это"*, 2026-07-24).

## 7. Consequences that must be said out loud

- **Every unconfigured peer becomes `Pelmentor`.** CLIENT_1/2 have no `net.nick`, and neither does any
  user who never edited their name. No correctness impact (F13), but the activity feed always renders the
  nickname, never "You", so several `Pelmentor`s are indistinguishable. Same is true of today's `Player`.
- **Verdicts that move**, none of them silent-and-harmful: `ui.netstats=true` false→true; `peer_actions=off`
  true→false; `enabled=no` → dev features off; `nameplate=false` true→false; `voice.x=` (empty) true→false
  (F33). Each either **matches what the user literally wrote**, or is garbage and lands in T10's rejected-value
  report. There is no third case — which is why no old-semantics predicate is compiled beside the new lexer.
- **Our own dev rig loses attribution** unless the four installs get distinct `net.nick` lines.
- **The requester does not benefit by default.** T1 only reaches installs created after it ships; T1b is
  the deliberate answer.
- **Population honesty.** Every corpus claim covers **4 inis on the maintainer's rig**. The repo has been
  public since s29b; no census of user files exists and none is claimed.

## 8. What was dropped, and by which round

Pass 1 reframes: (1) seeding active default values — they override the code default (F4); (2) seeding
commented default values — still a second copy, and no field distinguishes a seeder line from a user's own
note; (3) migrating existing inis automatically — replaced by T1b; (4) STUN as part of this work (F9).
Also dropped: a C++ call-site parser to harvest defaults; typed key handles across all ~100 keys.

**Pass 2 deletions — mechanisms designed and then dissolved, not deferred:**

- **The meaning-change report**, its persisted "already shown" state, its overlay deferral, and the four
  per-shape legacy predicates. Dissolved by the §7 two-case argument: a changed verdict either matches what
  the user wrote, or is garbage already covered by T10. Compiling old semantics beside new would have been
  RULE 2 with a calendar-driven deletion.
- **The in-memory ini snapshot** (F34): the existing `call_once` idiom closes the two unlatched reads for
  two lines, so the snapshot bought only two new invariants (unreadable-at-boot; invalidation after a UI
  write) that then disappeared with it.
- **The ratchet as an external `(file,key)` list**: dissolved into symbol visibility.
- **Re-shaping `boot_warning_dialog`** (F35): out of scope for a config arc, and a config notice does not
  belong in the same modal as "MOD INSTALL PROBLEM". Its single-slot defect is FILED as its own finding and
  gets no second caller — it is latent today precisely because there is one.
- **Four of the primary's own pass-2 answers**: vocabulary narrowing (D7), the "promise already made" union
  (D9), the token-delta table (D12 — it cannot express the `!= "0"` shape, whose true-set is every string),
  and the "zero flips over the rig" claim (D13 — a negative with no known-positive control; the corpus
  contained no instance of any change class and the key filter excluded the one live phantom).

## 9. Open — neither pass converged

Pass 1: 16 rounds, the last still material. Pass 2: 15 rounds against this document, the last round still
producing three substantive changes (the shared MY-NAME constant, dropping the snapshot, rendering
present-but-empty explicitly). Per the `/qf` rule, convergence is the critic holding, not the round cap, so
**this design is NOT certified and no code has been written.**

Residual questions at the point of writing:

- **Does the banner marker still earn its place?** It existed to detect "our" file for an automatic
  migration that was dropped; the reformat is now owner-triggered from T10.
- **The ini UNREADABLE or locked at boot is undefined for READS.** F7 covers writes only.
- Whether the ~46 env reads outside the registry are their own arc (same duplicated-literal shape, but
  per-launch env must not gain an ini layer).
- Whether the catalog can honestly claim "все флаги" while those 46 live outside by design (F17/F20).
  Current answer: it lists them as reference, not as copyable rows, and the header says so.

**Next step:** a third `/qf` pass against this rewritten document, then arc 1.
