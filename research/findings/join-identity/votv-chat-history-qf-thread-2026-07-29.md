# BRIEF — chat HISTORY on T-activation (+ the two chat defects found today)

## WHAT THE USER ACTUALLY ASKED FOR — VERBATIM

«Надо бы еще потом заняться чатом, потому что сейчас нету истории у него. А она должна быть когда
игрок активирует чат на T. А пока он не активировал чат, никакой истории и тд нет, чат как обычно
работает - сообщения появляются плавно и счезают плавно, а когда чат активирован то мы плавно
проявляем всю или какую-то часть истории чата.»

Translation, keeping the user's own nouns:
- «сейчас нету истории у него» — chat has NO history right now.
- «она должна быть когда игрок активирует чат на T» — history must appear when the player ACTIVATES
  chat with T.
- «пока он не активировал ... чат как обычно работает - сообщения появляются плавно и счезают плавно»
  — while NOT activated, chat works AS IT DOES TODAY: messages appear smoothly and disappear smoothly.
  **This is an explicit no-regression clause on the passive path.**
- «когда чат активирован то мы плавно проявляем всю или какую-то часть истории чата» — when activated
  we smoothly REVEAL all, or SOME PART OF, the chat history.

**AMBIGUITY I AM NOT RESOLVING SILENTLY:** «всю или какую-то часть» can mean (a) *we* pick how much to
reveal (a design latitude grant), or (b) the player can reach further back (scroll). I read it as (a)
— a latitude grant, because the sentence is about what WE do («мы плавно проявляем»). Flagged for the
critic to challenge.

**ARC CONTEXT.** This arrived mid-round-1 of a /qf on a glyph-fallback design. That /qf then CONVERGED
BY MEASUREMENT and killed its own design: `mp.py smoke_i18n` proved the chat lane already carries
Cyrillic + astral emoji + hanzi + kana end-to-end, byte-intact, to every peer. So chat's TEXT lane is
fine and the glyph work is dead. What is NOT fine is (i) no history — this ask — and (ii) two real
defects found on the way, below.

## INVESTIGATION

Give chat a history that is revealed on T-activation and hidden otherwise, without regressing the
passive fade behaviour, and without letting retention become a peer-controlled amplifier.

## CURRENT CLAIMS — all `measured-artifact`, opened this session

**The store (`coop/comms/chat_feed.cpp`) DESTROYS history; it does not hide it.**
- `g_lines` is a `std::deque<Entry>`, game-thread only, capped at `kMaxLines = 6` by
  `TrimOverflow():88-95` which **pops the front**.
- `Tick():235-241` pops the front while `now - bornMs >= kTtlMs` (`kTtlMs = 11000`).
- Both paths call `NoteExpired()` into an 8-entry ring `g_expired[8]` whose `text` is `char[64]` —
  that is a RESURRECTION PROBE, not a history: truncated to 64 bytes, no slot, no nickLen, no bornMs
  ordering guarantee. **Not reusable as the history store.**
- So a line older than 11 s, or pushed out by a 7th line, is GONE from the process.

**The snapshot is a FIXED POD sized to the live cap.**
- `Snapshot { Line lines[kMaxLines]; int count; }`; `Line::text` is `char[256]`.
- `Republish():124-162` rebuilds it each push/tick under `g_mu`; `GetSnapshot()` copies it out.
- **A history view cannot ride this struct** without either growing it or adding a second channel.

**Alpha is owned by the STORE, derived from TTL — this is the crux.**
- `FadeAlpha(ageMs):112-120`: 220 ms arrival ramp, full, then a 1,500 ms fade-out tail.
- Every history line is BY DEFINITION past `kTtlMs`, i.e. alpha 0 by this function.
- **The activation reveal is a function of WHEN THE USER PRESSED T**, which the game-thread store does
  not know. So reveal alpha must be a RENDER-side property, and the store's alpha must not be reused
  for it. Today there is exactly one alpha and one owner.
- There is a live ALPHA-JUMP probe (`Republish():146-158`) that fires on an impossible alpha RISE for
  the same `bornMs` in its fade tail. A second alpha source risks either false-firing it or being
  invisible to it.

**The renderer (`ui/hud.cpp:300-370` `DrawChat`).**
- Draws to `ImGui::GetBackgroundDrawList()` — deliberately NOT a window (`:310-318`: the old
  AlwaysAutoResize window flickered the expiring top row; the fix was removing the window entirely).
- Bottom-anchored at `kBottomFrac = 0.5` of screen height, grows UPWARD, newest at the bottom.
- `forEachRow` word-wraps at `wrapW = min(0.42*W, S(640))`; pass 1 sums height, "bounded --
  kMaxLines=6 lines"; pass 2 draws. **Both passes assume a small bounded set.**
- `:303` `if (cs.count <= 0) return;` — an empty feed draws nothing at all.

**The input bar (`ui/chat_input.cpp`).**
- `Open()` clears `g_buf[204]`, sets `g_focusPending`. `IsOpen()` is an atomic read.
- Positioned at `io.DisplaySize.y * 0.5f + S(6.f)` — i.e. **pinned directly under the feed anchor**.
  If history expands the feed upward, the bar does not move (correct); if history is drawn DOWNWARD
  it would collide.
- **NAME COLLISION HAZARD:** `chat_input.cpp:28-31` already has `g_history` / `kHistoryMax = 16` /
  `g_histPos` — that is the Up/Down recall of **what YOU typed**, a different concept from the feed
  history the user is asking for. Two things called "history" in one subsystem.

**Two defects found today in the same subsystem (fold in, don't spin off):**
- **No decode boundary.** `chat_sync.cpp:114-124 OnReliable` takes `payload.text`, clamps `n`, calls a
  byte-stripping `SanitizeUtf8`, and pushes to `chat_feed` AND `chat_bubbles`. No `FromUtf8Strict`.
  Arc D1's «the receive boundary is STRICT and refuses ill-formed fields whole» is FALSE here, on the
  one attacker-controlled surface.
- **Three re-implementations of `coop::text`** inside `chat_sync.cpp`, which already includes the
  header: its own `SanitizeUtf8:32` (byte-identical to `utf8_codec.cpp:81`) and `TrimAndCap:45`
  (= `CapUtf8Bytes`). RULE 2.

**The amplifier, inherited from the glyph /qf's round-2 A4 (`measured` reasoning, not yet exploited):**
Chat text is peer-controlled and today self-limits because entries are POPPED. A history buffer
REMOVES that limit by design. Whatever the retention rule is, it is the cap on how much memory a
remote peer can make me hold. **This is a `docs/security/` concern, not a perf note.**

## PROPOSED SHAPE (the thing to interrogate — NOT built)

1. `chat_feed` keeps a second bounded deque of RETIRED entries (full `Entry`, not the 64-byte probe
   ring), fed from the two existing pop sites, capped by BOTH a line count and an age.
2. A second snapshot accessor for the history view, so the live `Snapshot` POD keeps its shape and the
   passive path is byte-for-byte unchanged.
3. Reveal alpha owned by the RENDER side, keyed on the `ui::chat_input::IsOpen()` edge, never by
   `FadeAlpha`.
4. `DrawChat` gains a mode: passive = today exactly; activated = the retained set + the live set,
   revealed with the activation ramp.
5. Rename one of the two "history" concepts before adding the third.

## PRIOR QF ROUNDS

None on chat history. The immediately preceding /qf (2 rounds, glyph fallback) converged by
measurement and is closed; its A4 (the amplifier) is carried forward INTO this brief as a live
constraint, not as settled.

---

# ROUND 1 — asked and answered

**A1 MY PREMISE WAS FALSE (measured).** `TrimOverflow():88-95` pops the front whenever
`size > kMaxLines` with NO age test — 7 messages in 1 s retires a ~200 ms-old line at alpha ~1.0. The
two retirement paths (overflow / TTL) produce entries in OPPOSITE alpha states. On multiply-vs-replace
the critic's framing exposes a third answer: reference implementations (SA-MP, Minecraft) show the
backlog SOLID on open, so activated mode should SUSPEND store alpha rather than multiply it — but that
is now a claim I must defend, and the inversion proves one-alpha-one-owner cannot express it.

**A2 INFERRED AND MIS-FILED (conceded).** A hard line cap bounds memory by construction (N x ~280 B);
there is no unbounded-memory story. The real cost is CPU and I have NO NUMBER: `forEachRow` calls
`CalcWordWrapPositionA` TWICE PER FRAME (height pass + draw pass) over the whole set, on the render
thread, under a comment that says "bounded -- kMaxLines=6". 200 revealed lines = ~400 wrap
computations/frame. I filed a render cost as a security row by pattern-matching the previous /qf's
amplifier finding.

**A3 BOTH EARLY RETURNS ARE ON THE FEATURE'S MAIN PATH (measured).** `chat_feed.cpp:234`
`if (g_lines.empty()) return;` fires BEFORE any trimming, and an empty live set is exactly what a
history buffer idles in. `hud.cpp:303` `if (cs.count <= 0) return;` fires BEFORE any drawing. So
"quiet lobby -> press T -> see history" — the headline case in the user's own sentence — is refused
TWICE by current code before reaching anything new.

**A4 RIGHT ON BOTH, AND BOTH ARE ACTIONS.** The decode boundary lands FIRST, own commit, own
`docs/security/TRACKER.md` row: retention converts "ill-formed for 11 s" into "ill-formed
indefinitely", making it a PRECONDITION of the feature, not a passenger. And «всю или какую-то часть»
goes back to the USER in plain text; resolving it as latitude was wrong.

# CORRECTED FACT BASE FOR ROUND 2

- Two retirement paths with opposite alpha states; activated mode needs a stated alpha POLICY, not an
  owner swap.
- The bound to defend is RENDER-THREAD CPU (2x CalcWordWrapPositionA per frame per revealed line),
  NOT memory. Unmeasured.
- Two early returns (`chat_feed.cpp:234`, `hud.cpp:303`) sit on the main path and must move or be
  bypassed — a new store that only trims when the LIVE set is non-empty is a latent leak.
- Open product question, going to the user, NOT resolved here: how much history on reveal.

---

# ROUND 2 — asked and answered

**A1 WRONG BY COUNT (measured).** `Reset():254-255` does `g_lines.clear()` with NO `NoteExpired` — a
THIRD removal site, in the file I had just read. "Feed from the two pop sites" was a SITE LIST and it
was already stale when written. INVARIANT: a single `Retire(Entry&&)` that is the ONLY way an entry
leaves `g_lines`; `TrimOverflow`, `Tick`'s TTL loop and `Reset` all route through it. Disconnect
`Reset` CLEARS the retained set (another session's conversation) — stated as a product call, not
derived.

**A2 "BYTE-FOR-BYTE UNCHANGED" RETRACTED.** A3 proves two passive-path lines must move, so the CODE
changes; what must not change is the OBSERVABLE PASSIVE BEHAVIOUR. Alpha triple resolved: item 3
SURVIVES, A1's "suspend" is RETRACTED. Final alpha is render-side
`passive ? store : max(store, revealRamp)` — live lines never dim below their TTL alpha, history rises
to the ramp, the store never learns `T` exists. AND: `TrimOverflow` pops at alpha 1.0, so a 7th
message makes the oldest line VANISH INSTANTLY — already contradicting «сообщения ... исчезают
плавно» in the user's own sentence. A pre-existing passive-path defect, found inside a history brief.

**A3 THE max() FORM DISSOLVES THE THREADING QUESTION.** Reveal is render-side only -> the game thread
never reads `ui::` -> no `coop/ -> ui/` inversion (principle 7). Per-frame lock cost NOT paid: publish
a GENERATION COUNTER with the history snapshot, render re-copies only on change. TTL-crossing: while
chat is open a live line ages and pops to retained mid-reveal; under `max(store, revealRamp)` with the
ramp at 1.0 its contribution is 1.0 on BOTH sides, so no blink. **This is an ARGUMENT, not a
measurement — first thing a drill must try to falsify.**

**A4 YES, AND THE CRITIC'S SECOND HALF IS THE BETTER ANSWER.** Text is immutable per `Entry`; `wrapW`
moves only on resize. Recomputing the wrap 2x/frame is waste at N=6 as much as at N=200. CACHE the row
split on the `Entry`, invalidate on `wrapW` change — that is the root fix and it makes N a PRODUCT
decision, not a CPU budget. Still take the number ("cheap" is an inference until measured), but do not
let an unmeasured budget pick N.

# CORRECTED SHAPE AFTER ROUND 2

1. ONE `Retire(Entry&&)` chokepoint — the invariant, not a site list. All three removal paths route
   through it. `Reset` clears retained too.
2. Retained deque of full `Entry`, bounded by line count (+ age?), living where the trim runs
   REGARDLESS of `g_lines.empty()` (the `Tick():234` early return must move).
3. Render-side reveal ramp keyed on `ui::chat_input::IsOpen()`; final alpha
   `passive ? store : max(store, revealRamp)`. Store unchanged, no `coop/ -> ui/`.
4. History snapshot + GENERATION COUNTER; render re-copies only on change, never per frame under
   `g_mu`.
5. Cached per-`Entry` row split, invalidated on `wrapW` change — kills the 2x/frame wrap for the
   passive path too.
6. `hud.cpp:303` early return must not refuse the quiet-lobby-press-T case.
7. Rename `chat_input`'s `g_history` (SEND recall) before adding feed history.
8. OPEN, going to the USER: how much history to reveal.
9. PRE-EXISTING DEFECT surfaced: `TrimOverflow`'s instant pop contradicts «исчезают плавно».

---

# ROUND 3 — asked and answered

**A1 WRONG AXIS, AND THE FILE SAYS SO.** `hud.cpp:310-318` records the previous chat blink was proven
NOT alpha (ALPHA-JUMP never fired) and WAS layout. I defended the TTL crossing on alpha. Since
`y = anchorBottomY - totalH`, one duplicated/missing row shifts EVERY line incl. the newest. TWO
independently-generationed snapshots make exactly that possible (copy live at gen N, retained at gen
M, entry moved between -> double-count or gap). **FIX: ONE snapshot carrying BOTH sets under ONE
generation** — "a bornMs is in exactly one set" becomes true BY CONSTRUCTION. Corrected-shape item 4
RETRACTED. Settling read: one frame of `(gen, bornMs multiset, totalH)`.

**A2 TODAY'S OWN LESSON, QUOTED BACK CORRECTLY.** I wrote
`lesson_an_agent_authored_rule_becomes_a_user_rule` this session ("a prose rule that nothing enforces
is not a rule") and then proposed a chokepoint enforced by PROSE. A bare `std::deque` keeps
`clear`/`erase`/`pop_back`/assignment/swap compiling. What fails MECHANICALLY: a TYPE THAT OWNS the
container and exposes no other drain (the `PerSlotState<T>` precedent — registers its own clear in its
ctor). And `g_pending` is a FOURTH path already: `Tick():227` erase + `Reset` clear, neither retires a
never-promoted line.

**A3 UNDESIGNED; PART OF IT IS NOT MINE TO DECIDE.** Ramp has an open and no close. ESC, rapid toggle,
resize-mid-reveal (invalidates every cached row split at once) all unspecified. Sharpest: `Close()`
fires on SUBMIT (`chat_input.cpp:122`), so sending a message COLLAPSES THE BACKLOG you were reading.
Product question -> USER, with item 8.

**A4 BEST QUESTION OF THE PASS — IT REFRAMES THE CLAUSE.** The retained deque DISSOLVES the instant-pop
defect rather than deferring it: the overflow victim survives, so the render can FADE it. So "passive =
today exactly" would deliberately preserve the one behaviour the user's sentence says does NOT happen.
**The no-regression clause protects the USER'S DESCRIPTION, not today's code** — «сообщения появляются
плавно и счезают плавно» is what they believe chat does, and where the code disagrees, THE CODE IS
WRONG. I had it backwards.

# SHAPE AFTER ROUND 3 (fact base stable; 2 product questions open to the USER)

1. A TYPE that owns the line container, exposing `Retire()` as the ONLY drain — mechanical, not prose.
   Covers `g_lines` AND `g_pending` (never-promoted lines are a 4th path today).
2. ONE snapshot carrying live + retained under ONE generation counter. (Two snapshots = a layout race
   on `totalH`, which is the axis the previous blink was actually on.)
3. Render-side reveal ramp; final alpha `passive ? store : max(store, revealRamp)`. Store never learns
   `T` exists (no `coop/ -> ui/`).
4. Cached per-`Entry` row split, invalidated on `wrapW` change. Kills 2x/frame wrap on BOTH paths.
5. `Tick():234` and `hud.cpp:303` early returns must move (both sit on the feature's main path).
6. Overflow victims FADE instead of vanishing — dissolving the pre-existing defect, per A4.
7. Rename `chat_input`'s `g_history` (SEND recall) before adding feed history.
8. Drill, not eyes: one frame of `(gen, bornMs multiset, totalH)` at a TTL crossing while open.

## OPEN — USER ONLY
- Q1: how much history on reveal (all / last N / scrollable)?
- Q2: does sending a message close chat and collapse the backlog (today `Close()` fires on submit)?

---

# USER ANSWERED THE TWO PRODUCT QUESTIONS (2026-07-29)

> 1. The last N lines.
> 2. Close like minecraft.

**Q1 -> "the last N lines".** So the reveal is a BOUNDED TAIL, not "all" and not scrollable. N is a
constant we choose. Consequences: the retained deque's line cap and the reveal count can be the SAME
number (retaining more than we can ever show is dead memory); no scroll state, no scrollbar, no
mouse-wheel handling, no `NoInputs` change on the feed's draw-list rendering.

**Q2 -> "close like minecraft".** Minecraft: Enter sends AND closes; the backlog collapses back to the
passive fade. So `chat_input.cpp:122`'s existing `Close()`-on-submit is CORRECT and stays. What was
listed in round 3 as a hazard ("sending a message collapses the backlog you were reading") is now the
INTENDED behaviour by user decision. ESC also closes without sending (already true, `:78`).

**This closes both open items. Nothing product-shaped is left open.**

# CONCRETE PROPOSAL FOR ROUND 4 (numbers now, not shapes)

- `kRevealLines` = the reveal tail. PROPOSED 12 (Minecraft shows ~10 when open; passive is 6, so 12 is
  one screenful of backlog beyond what you already see). Retention cap = the SAME number: retaining
  more than can ever be shown is dead memory.
- Retention holds full `Entry` (~280 B) -> 12 x 280 B = ~3.4 KB. Not a memory story at any N we would
  pick, per round-1 A2.
- Reveal ramp: open ramp only (a close is instantaneous per Minecraft parity, and the passive fade
  already owns what happens after).
- One snapshot, one generation, both sets (round-3 A1).
- Container-owning type with `Retire()` as the only drain (round-3 A2), covering `g_lines` AND
  `g_pending`.
- Cached per-`Entry` row split invalidated on `wrapW` change (round-2 A4).
- `Tick():234` + `hud.cpp:303` early returns move (round-1 A3).
- Overflow victims fade instead of vanishing (round-3 A4) -- this is a passive-path FIX, and it is
  now the ONLY intentional passive-path behaviour change.
- Rename `chat_input`'s `g_history` (send recall) -> `g_sendRecall` before adding feed history.

---

# ROUND 4 — asked and answered

**A1 REAL GAP (measured).** `FadeAlpha(now - bornMs)`: an overflow victim popped at 200 ms sits at
1.0 for ~9.3 s more. "Fades" is meaningless on the BIRTH clock. Entry needs `retiredMs` + a REGIME
SWITCH, not a second coexisting alpha: live -> birth-keyed TTL fade; retiring -> `retiredMs`-keyed
short fade to 0; retired -> 0. One alpha per entry, one owner, two regimes (compatible with round 2
only because it is now stated explicitly). **HEIGHT BOUND MOVES:** during a burst passive draws the 6
live lines PLUS whatever is still in its retirement fade -> bound goes `kMaxLines` -> `kMaxLines +
in-flight`. Item 6's "only one passive change" was WRONG; there are two.

**A2 UNION; RETENTION IS COUNT-BOUNDED ONLY, DELIBERATELY.** "Last N" is over the union; an entry is
in EXACTLY ONE of {live, retiring, retired} at any instant, so nothing draws twice (the one-snapshot
invariant applied to MEMBERSHIP). The dropped `(+ age?)` is KILLED on purpose and I should have said
so: **age-bounding retention defeats the feature** — a line from ten minutes ago is exactly what
"history" means. A settled retained line has store alpha 0 forever -> `max(store, reveal)` = reveal,
correct for it; A1's fading victim is in the RETIRING regime, not the retired one. No conflict.

**A3 SAME SITE-LIST SHAPE — CONCEDED.** `Reset():256-258` writes `g_pub` DIRECTLY; `Tick():234`
returns before `Republish`. Under one generation those are two paths mutating published state without
bumping it. Fix is NOT "remember to bump in three places": the container-owning type owns PUBLICATION
as well as retirement, so `g_pub` has exactly ONE writer and the bump is unconditional inside it.
Round-3's invariant extended one step further.

**A4 TWO THINGS, AND THE SECOND DISSOLVES THE FIRST.** A ramp at 0.4 collapsing to 0 in one frame is a
`totalH` jump on precisely the axis `hud.cpp:310-318` proves the last blink was on -> "open ramp only"
was WRONG; the ramp needs a SYMMETRIC CLOSE, and Minecraft agrees (closing leaves recent lines fading,
it does not blank them). On the edge: `IsOpen()` is polled once per frame, so close+open inside one
frame produces NO EDGE. Answer: stop using an edge. The ramp has a TARGET (`IsOpen() ? 1 : 0`) and
moves toward it each frame -> rapid toggling resumes from wherever the ramp is, and "what value
survives to the next open" is answered by construction: the current one.

# SHAPE AFTER ROUND 4

1. Container-owning type owns RETIREMENT **and** PUBLICATION. One drain (`Retire()`), one writer of
   `g_pub`, unconditional `gen` bump inside it. `Reset` and the `Tick` early-return route through it.
2. Entry alpha = ONE value, THREE regimes: live (bornMs TTL) / retiring (retiredMs short fade) /
   retired (0).
3. Reveal = a render-side RAMP WITH A TARGET (`IsOpen() ? 1 : 0`), symmetric open and close, no edge
   detection. Final alpha `max(storeAlpha, revealRamp)` over the union's last N.
4. Retention: COUNT-bounded only (12), NEVER age-bounded. ~3.4 KB.
5. TWO intentional passive changes, both owned: overflow victims fade instead of vanishing, AND the
   passive height bound becomes `kMaxLines + in-flight-retiring`.
6. Cached per-`Entry` row split, invalidated on `wrapW` change.
7. Rename `chat_input`'s `g_history` -> `g_sendRecall`.
8. Drill: one frame of `(gen, bornMs multiset, totalH)` at a TTL crossing while open.

---

# ROUND 5 — asked and answered

**A1 PERMANENT LAYOUT BUG; MY DRILL IS BLIND TO IT.** An asymptoting ramp settling at 1e-6 keeps the
retained rows in `totalH` FOREVER — height is charged per DRAWN row and an INVISIBLE ROW STILL
OCCUPIES HEIGHT. The passive block would sit permanently 12 rows too high, on the axis round 3 named.
Snapping to exact 0/1 within an epsilon is necessary but NOT sufficient: **membership in the layout
must be a PREDICATE, not a consequence of alpha** — a row below the visibility threshold is excluded
from BOTH passes. Today's code never had this because expired lines are POPPED; retention introduces
it. Drill must add a frame AFTER a close settles.

**A2 dt-SCALED; THE CITED LESSON ARGUES THE OTHER WAY HERE.** `FadeAlpha` is wall-clock ms, so a
frame-count ramp composes two clocks inside one `max()`. `scale.cpp:52-57` chose FRAMES deliberately
but for a DEBOUNCE ("a wall-clock hold would tie the answer to the frame rate it is trying to
protect"). A reveal ANIMATION is the inverse: same 150 ms at 15 fps and 240 fps. At the 4.2 FPS peer
(`LESSONS:498`) a dt ramp completes in ONE frame (instant, acceptable); a 12-frame ramp would take
**2.9 s**. NOT measured on any peer — the smoke host ran ~114 fps, no low-fps sample.

**A3 ONE CONCEPT, AND THE QUESTION FOUND ITS NAME.** Invariant, one sentence, no "and": **no mutation
of the store is observable except through a republished snapshot.** The drain is a mutation,
publication is the observation, `gen` is the observation's identity — `Retire()` and
single-writer-`g_pub` are two FACES OF ONE RULE, not two fixes welded together. The three alpha
regimes are NOT part of it; they belong to `Entry`. Separation is clearer for being challenged.

**A4 SCOPE CREEP AGAINST THE USER'S OWN WORDS — ITEM 5 DROPPED.** «пока он не активировал чат,
НИКАКОЙ ИСТОРИИ И ТД НЕТ» is explicit. A taller passive stack during a burst is history leaking into
the passive path; a player who never pressed T would SEE the feature. So passive stays EXACTLY as
today, INCLUDING the instant overflow pop, and retained lines appear ONLY behind T. The
instant-pop-vs-«исчезают плавно» tension is real but is a SEPARATE PRE-EXISTING question — filed, not
bundled. Round-3's A4 reframe was right about the defect and wrong to fold it in.
Closing test lands: a player who never presses T notices NOTHING from items 1/4/6/7/8 — those are
ENGINEERING HYGIENE, justified on their own terms, but they are not the feature.

# SHAPE AFTER ROUND 5

THE FEATURE (what the user would notice):
- F1. Retention: count-bounded (12), never age-bounded, full `Entry`, ~3.4 KB.
- F2. Reveal = render-side ramp with a TARGET (`IsOpen() ? 1 : 0`), **dt-scaled**, symmetric open and
  close, snapped to exact 0/1 within an epsilon, no edge detection.
- F3. Draw the union's last N, final alpha `max(storeAlpha, revealRamp)`. **Layout membership is a
  PREDICATE** (alpha >= threshold), never a consequence of alpha.
- F4. PASSIVE PATH UNCHANGED — no behaviour change at all, including the instant overflow pop.

ENGINEERING HYGIENE (invisible to the user; justified separately, NOT the feature):
- H1. Container-owning type. ONE invariant: *no mutation of the store is observable except through a
  republished snapshot.* Covers the drain, the single `g_pub` writer, the unconditional `gen` bump,
  `Reset`, and the `Tick` early return.
- H2. Cached per-`Entry` row split, invalidated on `wrapW` change.
- H3. Rename `chat_input`'s `g_history` -> `g_sendRecall`.

DRILLS:
- D1. TTL crossing while open: `(gen, bornMs multiset, totalH)`.
- D2. **A frame AFTER a close settles** — assert retained rows contribute ZERO to `totalH`.
- D3. Low-fps reveal duration (no sample exists; the smoke host runs ~114 fps).

FILED, NOT BUNDLED:
- The instant overflow pop vs «сообщения исчезают плавно».

---

# ROUND 6 — asked and answered

**A1 NO SUCH ARTIFACT EXISTS YET; THE RECIPE SAYS IT MUST EXIST FIRST.** `LESSONS:1904-1910` requires
an instrument the refactor commit PHYSICALLY CANNOT TOUCH + a baseline x2 on unsplit code. The
observable is render-side -> the instrument is a dev-only probe logging
`(sampleIdx, count, quantized alpha multiset, totalH)` on a SCRIPTED PASSIVE scenario (M messages at
fixed intervals, T never pressed); digest = a hash over that sample sequence. **It must land and be
baselined x2 BEFORE H1/H2, in its own commit.** Ordering H1 first would make "unchanged" an assertion
again.

**A2 DROPPED AS A CONSEQUENCE, NEVER STATED — exactly the risk named.** F4 restores the instant pop,
so the RETIRING regime loses its reason to exist: regimes reduce to **TWO** — live (`bornMs` TTL) and
retained (0) — with the reveal ramp supplying all retained visibility. A 7th-line pop while chat is
OPEN changes no alpha (already at `max(0, ramp)` = ramp) and no ORDER: the union is ordered by
`bornMs` throughout, and the overflow victim is by construction the OLDEST, so it lands at the top of
the history where it belongs.

**A3 MEASURED — A HOST DRILL WOULD CERTIFY NOTHING.** `player_handshake.cpp:520` gates `PushDelayed`
on `role == net::Role::Client && slot == 0` — CLIENT-ONLY, explicitly. `net_pump.cpp:196-198` says in
its own comment that this funnel is NOT the host path ("DisconnectAll is the WRONG home -- it also
runs on the HOST when a client leaves, and the host must KEEP its UI"). Only `event_feed.cpp:121` runs
on both. **Two of three drains are client-side** -> D1-D3 must run on a JOINING CLIENT, including a
join -> leave -> rejoin cycle.

**A4 THE CACHE CANNOT LIVE ON THE `Entry`.** The split is a function of `font`/`px`/`wrapW` (all
`ui/`); `Entry` is game-thread `coop/`. Putting it there is the inversion round 2 dissolved. It lives
**RENDER-SIDE, keyed by `bornMs`** — already this file's entry identity (ALPHA-JUMP keys on it at
`:149`), monotone, and SURVIVES the live->retained migration unchanged, so a `gen` bump that only
moves an entry between sets re-splits nothing. Invalidation is wholesale on `wrapW`/`font`/`px`.

# SHAPE AFTER ROUND 6 (build order is now part of the design)

ORDER OF COMMITS (the instrument first, or "unchanged" is an assertion):
  0. **D0 the frozen passive digest probe** + baseline x2 on unsplit code. Dev-only, RULE-2 exempt.
     Scripted: M messages at fixed intervals, T NEVER pressed. Digest over
     `(sampleIdx, count, quantized alphas, totalH)`.
  1. H1 container-owning type. ONE invariant: *no mutation of the store is observable except through
     a republished snapshot.* Digest must be IDENTICAL to the D0 baseline.
  2. H2 render-side row-split cache keyed by `bornMs`, invalidated wholesale on `wrapW`/`font`/`px`.
     Digest must be IDENTICAL. (H2 is a passive-path perf fix in its own right.)
  3. F1-F4 the feature.
  4. H3 rename `g_history` -> `g_sendRecall`.

THE FEATURE:
- F1. Retention: count-bounded (12), never age-bounded, full `Entry`, ~3.4 KB.
- F2. Reveal ramp with a TARGET (`IsOpen() ? 1 : 0`), dt-scaled, symmetric, snapped to exact 0/1.
- F3. Union's last N ordered by `bornMs`; final alpha `max(storeAlpha, revealRamp)`; **layout
  membership is a PREDICATE (alpha >= threshold)**, never a consequence of alpha.
- F4. PASSIVE PATH UNCHANGED, including the instant overflow pop.
- Alpha regimes: **TWO** (live = bornMs TTL, retained = 0). The retiring regime is GONE with F4.

DRILLS — **ALL ON A JOINING CLIENT** (two of three drains are client-only):
- D1. TTL crossing while open: `(gen, bornMs multiset, totalH)`.
- D2. A frame AFTER a close settles: retained rows contribute ZERO to `totalH`.
- D3. Low-fps reveal duration (no sample exists; smoke host runs ~114 fps).
- D4. join -> leave -> rejoin (exercises `Reset` + `PushDelayed`, both client-only).

FILED, NOT BUNDLED: the instant overflow pop vs «сообщения исчезают плавно».

---

# ROUND 7 — asked and answered

**A1 SENSITIVE IN PRINCIPLE IS NOT SHOWN-FAILING.** The digest DOES observe retirement (count and
`totalH` both drop on expiry/pop), so it is not blind — but that phrase is exactly `LESSONS:682-689`.
D0 is worthless until shown RED. Spec must carry injected must-FAIL mutations proven to change the
digest: `kMaxLines` 6->5, `kTtlMs` 11000->10000, skip-one-trim.

**A2 ASSUMED, NOT SPECIFIED — AND IT SPLITS D0 IN TWO.** `FadeAlpha` is wall-clock, `totalH` is
render-thread, so a FRAME-INDEXED digest will not reproduce byte-equal across two runs of the SAME
bytes. H1 and H2 are different KINDS of change and need different proofs:
- **H1** store-side, deterministic given a scripted send schedule -> digest over **STORE EVENTS**
  (push/retire), sampled on the EVENT, not the frame.
- **H2** a pure function of `(text, font, px, wrapW)` -> NOT a digest: a direct equality assertion,
  cached split == uncached split, every entry every frame, dev build.

**A3 THE SPLIT IS NOT HONEST — SAYING SO PLAINLY.** With F4 in force **H1 fixes NO observable passive
defect**; it is a PRECONDITION of retention, i.e. **the first commit OF the feature**. Presenting it
as independent hygiene was dressing a feature diff to look modular. RELABELLED. **H2 is worse: nobody
measured that 2x wrap costs anything at `kMaxLines=6`** — justified by a cost I INFERRED and then
carried for five rounds. **H2 LEAVES THE BUILD ORDER** until the feature makes N large enough to
matter, or someone takes the number.

**A4 NOT COMPUTED, AND THE ARITHMETIC BREAKS IT.** Available height = `0.5*H`. At 1080p with the
smoke's own `chat 22 px, scale 1.25`, `rowH ~24.5` -> ~**22 rows** fit. Twelve ENTRIES != twelve ROWS:
at `wrapW = 640*S` a full 203-byte message wraps to ~3 rows, so twelve worst-case entries is **~36
rows ~880 px against 540 available** — off the top of the screen. **`kRevealLines` cannot be a count.**
The reveal walks the union NEWEST-FIRST accumulating wrapped height and STOPS AT THE HEIGHT BUDGET,
with 12 as an UPPER BOUND, not a target. Only form that survives a resolution / UI-scale change.

# SHAPE AFTER ROUND 7

BUILD ORDER (honest labels now):
  0. **D0 the store-event digest probe** (dev-only, RULE-2 exempt) + baseline x2 + **three injected
     must-FAIL mutations proven RED** (`kMaxLines` 6->5, `kTtlMs` 11000->10000, skip-one-trim).
  1. **Feature commit 1** — container-owning type. Invariant: *no mutation of the store is observable
     except through a republished snapshot.* Digest identical to the D0 baseline. (NOT "hygiene": it
     is a precondition of retention and would not ship alone.)
  2. **Feature commit 2** — F1-F4.
  3. Rename `g_history` -> `g_sendRecall`.
  DROPPED FROM THE ORDER: the row-split cache (unmeasured at `kMaxLines=6`; revisit if N grows).

THE FEATURE:
- F1. Retention: count-bounded (12 = an UPPER bound), never age-bounded, full `Entry`, ~3.4 KB.
- F2. Reveal ramp with a TARGET (`IsOpen() ? 1 : 0`), dt-scaled, symmetric, snapped to exact 0/1.
- F3. Reveal walks the union NEWEST-FIRST by `bornMs`, accumulating WRAPPED height, stopping at the
  height budget (`0.5*H`) or 12 entries, whichever comes first. Final alpha
  `max(storeAlpha, revealRamp)`; **layout membership is a PREDICATE**, never a consequence of alpha.
- F4. PASSIVE PATH UNCHANGED, including the instant overflow pop.
- Alpha regimes: TWO (live = bornMs TTL, retained = 0).

DRILLS — ALL ON A JOINING CLIENT (two of three drains are client-only):
- D1 TTL crossing while open · D2 a frame AFTER a close settles (retained rows contribute ZERO to
  `totalH`) · D3 low-fps reveal duration · D4 join -> leave -> rejoin.

FILED, NOT BUNDLED: the instant overflow pop vs «сообщения исчезают плавно».

---

# ROUND 8 — asked and answered

**A1 CARRIED FRAMING, AND IT IS FALSE (measured).** `bornMs` has exactly TWO assignments: `:177`
`NowMs()` and `:224` `e.bornMs = now` — and `now` is hoisted OUTSIDE the promotion loop, so every line
promoted in one pass gets a BYTE-IDENTICAL stamp BY CONSTRUCTION. Two independent pushes inside one
millisecond collide too. **`bornMs` cannot carry identity**, and I hung THREE things on it: "in
exactly one set", the union ORDERING, and the render cache KEY. Worse: `:149`'s existing ALPHA-JUMP
probe ALREADY asserts uniqueness in its own comment — a PRE-EXISTING wrong claim I inherited and
amplified across four rounds. FIX: a monotone `seq` counter incremented on every push; `bornMs` stays
the FADE CLOCK and stops being an IDENTITY.

**A2 12 IS THE UNION TAIL — and the arithmetic survives for a reason I had not stated.** "Last N
lines" is what you SEE -> N bounds the UNION tail. Retention still needs 12 because `live` can be
ZERO (everything expired in a quiet lobby — the headline case in the user's own sentence), and then
all 12 shown are retained. No dead memory, but it depends on the EMPTY-LIVE case, not on round 4's
cap-equals-reveal reasoning.

**A3 THE SAME INFERENCE IN THE OTHER DIRECTION — CAUGHT.** I dropped a cache as unmeasured and added a
walk in the same breath. But the walk and the height pass are THE SAME TRAVERSAL: selecting
newest-first BY ACCUMULATED HEIGHT *is* the height computation. Implemented as ONE walk that both
selects and measures, it stays at two passes over the SELECTED set, not three over 18. A correctness
constraint on the implementation, now written down.

**A4 NO TOP MARGIN; FLUSH-TO-EDGE IS NOT INTENDED.** The bar never collides (it sits BELOW the
anchor), but a budget-filling block puts its top row at `y = 0`, hard against the screen edge, which
no other overlay in this file does — `DrawChat` already carries `pad = S(14.f)`. Budget = `0.5*H - pad`.

# SHAPE AFTER ROUND 8

IDENTITY: a monotone `seq` (uint64, ++ on every push) is the entry identity. `bornMs` is the FADE
CLOCK ONLY. Fixes the union ordering, the "exactly one set" invariant, and any render-side cache key.
**ALSO A PRE-EXISTING BUG:** `chat_feed.cpp:149`'s ALPHA-JUMP probe pair-matches on `bornMs` and its
comment asserts uniqueness that does not hold -> it can pair the WRONG two entries. File it.

BUILD ORDER:
  0. D0 store-event digest probe + baseline x2 + three injected must-FAIL mutations proven RED.
  1. Feature commit 1 — container-owning type. Invariant: *no mutation of the store is observable
     except through a republished snapshot.* Digest identical. Adds `seq`.
  2. Feature commit 2 — F1-F4.
  3. Rename `g_history` -> `g_sendRecall`.
  NOT IN THE ORDER: the row-split cache (unmeasured at `kMaxLines=6`).

THE FEATURE:
- F1. Retention count-bounded at 12 (needed in full because `live` can be 0), never age-bounded.
- F2. Reveal ramp with a TARGET (`IsOpen() ? 1 : 0`), dt-scaled, symmetric, snapped to exact 0/1.
- F3. ONE walk, newest-first by `seq`, that BOTH selects and measures: accumulate wrapped height,
  stop at `0.5*H - pad` or 12 entries, whichever first. Final alpha `max(storeAlpha, revealRamp)`;
  layout membership is a PREDICATE.
- F4. PASSIVE PATH UNCHANGED, including the instant overflow pop.
- Alpha regimes: TWO (live = bornMs TTL, retained = 0).

DRILLS — ALL ON A JOINING CLIENT: D1 TTL crossing while open · D2 a frame after a close settles ·
D3 low-fps reveal duration · D4 join -> leave -> rejoin.

FILED, NOT BUNDLED: the instant overflow pop vs «исчезают плавно»; the ALPHA-JUMP `bornMs` pair-match.

---

# ROUND 9 — asked and answered

**A1 MEASURED — THE WORST FINDING OF THE PASS.** `imgui_overlay.cpp:458` gates whether an ImGui frame
is BUILT AT ALL on `AnyOpen() || ui::hud::IsActive() || join_curtain::IsActive()`, and `hud.cpp:416`
puts `chat_feed::HasAny()` in that disjunction. `HasAny()` reads the PUBLISHED count. If retained
lines ride the published snapshot and retention is never age-bounded, **ONE chat message turns the
overlay frame ON PERMANENTLY** — NewFrame, DrawChat, the whole pass, every Present, forever, in a
lobby showing nothing. A large passive-path REGRESSION hiding inside a feature labelled "passive path
unchanged". FIX: the ACTIVATION PREDICATE — `HasAny()` must mean LIVE count > 0, and the reveal keeps
the frame alive only while the ramp is non-zero. Round 5's "membership is a predicate, not a
consequence" applied ONE LAYER UP, where I had not thought to apply it.

**A2 RETRACTED, AND THE FALSE CLAIM IS IN TWO PLACES.** `chat_feed.h:32` says
`uint64_t bornMs = 0;   // entry identity (the resurrection probe keys on it)` — the identical
falsehood filed only against `chat_feed.cpp:149`. BOTH need correcting. And yes: `seq` must ride the
published `Line` and the array must hold the union -> **"the POD keeps its shape" is DEAD**. Copy
cost: `Line` ~272 B -> 6 = ~1.6 KB, 18 = ~4.9 KB per `GetSnapshot`. That is what the GENERATION
COUNTER is for — the render side re-copies ONLY on a `gen` change, so the per-frame cost is a `gen`
compare, not a memcpy. Already in the design; Q1 makes it LOAD-BEARING rather than nice-to-have.

**A3 NO SUCH CAPABILITY EXISTS AND IT IS NOT IN THE BUILD ORDER.** `mp.py:1397+` `_type_chat` presses
T, types, presses Enter — it can NEVER leave chat open, so **D1, D2, D3 have no way to run**. D0 is
fine (the client under test never presses T; the OTHER peers send and it receives). The build order
owes a HARNESS COMMIT before the drills: a hold-open path on the client under test. A fourth thing
that would have been discovered at the keyboard.

# SHAPE AFTER ROUND 9

ACTIVATION (new, and the biggest correction): `chat_feed::HasAny()` means **LIVE count > 0**. Retained
lines NEVER activate the HUD. The reveal holds the frame alive only while the ramp is non-zero. Without
this, one message pins the overlay frame ON forever (`imgui_overlay.cpp:458` -> `hud.cpp:416`).

IDENTITY: monotone `seq` (uint64, ++ per push) rides the published `Line`. `bornMs` is the FADE CLOCK
only. **Two pre-existing false comments to correct: `chat_feed.h:32` AND `chat_feed.cpp:149`.**

SNAPSHOT: the POD holds the UNION (not 6). ~4.9 KB. Copied ONLY on a `gen` change — the generation
counter is now load-bearing, not an optimisation.

BUILD ORDER:
  0. D0 store-event digest probe + baseline x2 + three injected must-FAIL mutations proven RED.
  0b. **HARNESS: a hold-chat-open capability on a chosen peer** (mp.py `_type_chat` cannot do it).
  1. Feature commit 1 — container-owning type; invariant *no mutation of the store is observable
     except through a republished snapshot*; adds `seq`; fixes both false comments. Digest identical.
  2. Feature commit 2 — F1-F4 + the activation predicate.
  3. Rename `g_history` -> `g_sendRecall`.
  NOT IN THE ORDER: the row-split cache (unmeasured at `kMaxLines=6`).

THE FEATURE: F1 retention 12, count-bounded, never age-bounded · F2 dt-scaled symmetric ramp with a
TARGET, snapped to exact 0/1 · F3 ONE walk newest-first by `seq` that BOTH selects and measures,
stopping at `0.5*H - pad` or 12 · F4 passive path unchanged INCLUDING activation · TWO alpha regimes.

DRILLS — ALL ON A JOINING CLIENT, and D1-D3 BLOCKED until 0b exists.

FILED, NOT BUNDLED: the instant overflow pop vs «исчезают плавно»; the ALPHA-JUMP `bornMs` pair-match.

---

# ROUND 10 — asked and answered

**A1 IT DISSOLVES OUTRIGHT (measured).** `imgui_overlay.cpp:191-194` handles `T` in the **WndProc
hook** and calls `chat_input::Open()` — an atomic store OUTSIDE any frame. `:126` already has
`ChatOpen()` inside `AnyOpen()`, and `:458` gates on `AnyOpen() || hud::IsActive() || ...`. So in a
quiet lobby the gate is off, T flips the atomic from the MESSAGE PUMP, and the very next Present
admits the frame. **The headline case needs NOTHING NEW** — my proposed "third gate term" was solving
a problem the existing structure had already solved.

**A2 THE ROUND'S REAL FINDING.** `io.DeltaTime` comes from `ImGui_ImplWin32_NewFrame()` at `:346`,
INSIDE the gate. After 60 s of a quiet lobby the first post-T frame carries `DeltaTime ~ 60`, so a
dt-scaled ramp saturates to 1.0 in ONE FRAME — the reveal would be instant on exactly the case the
user described, and would look like the animation was never built. FIX: neither a clamp nor a
first-frame arm — **the ramp owns its own WALL CLOCK**, the same `steady_clock` `FadeAlpha` uses, so
its value is a pure function of `(now, transitionStart, startValue, target)`. Transition start is
recorded when the OBSERVED TARGET DIFFERS FROM THE STORED ONE — a stored-state comparison, not a
hardware edge, so close+open inside one frame still reads target 1 and the ramp continues (round 4's
requirement).

**A3 DISSOLVED BY A2's FIX.** With the ramp a pure function of wall-clock time there is no "advance"
step to place, so the circularity (the term gates the frame that advances the value the term reads)
disappears. The gate can evaluate the ramp WITHOUT a frame having run, and it provably reaches exact 0
at `transitionStart + duration` instead of asymptoting to a stuck 0.4 — which also retires round 5's
snapping concern BY CONSTRUCTION.

# SHAPE AFTER ROUND 10 (believed converged)

ACTIVATION: `chat_feed::HasAny()` means LIVE count > 0; retained lines never activate the HUD. Press-T
in a quiet lobby works through the EXISTING `AnyOpen() -> ChatOpen()` path (WndProc atomic, outside the
frame). **No new gate term.** The frame stays alive while the ramp is non-zero via the same
`ChatOpen()`-adjacent evaluation, which is now a pure wall-clock function.

RAMP: owns its own `steady_clock`; value = f(now, transitionStart, startValue, target); transitionStart
set when the OBSERVED target differs from the STORED one. Immune to gate gaps, to frame rate, and to
close+open-in-one-frame. Reaches exact 0/1 by construction (no snapping, no asymptote).

IDENTITY: monotone `seq` rides the published `Line`; `bornMs` is the FADE CLOCK only. Correct BOTH
false comments (`chat_feed.h:32`, `chat_feed.cpp:149`).

SNAPSHOT: POD holds the UNION (~4.9 KB), copied ONLY on a `gen` change.

BUILD ORDER: 0 D0 digest probe + baseline x2 + 3 must-FAIL mutations RED · 0b harness hold-chat-open ·
1 container-owning type (+`seq`, +both comment fixes; digest identical) · 2 F1-F4 + activation
predicate · 3 rename `g_history` -> `g_sendRecall`. NOT IN ORDER: row-split cache (unmeasured).

FEATURE: F1 retention 12 count-bounded never age-bounded · F2 wall-clock symmetric ramp with a stored
target · F3 ONE walk newest-first by `seq` that BOTH selects and measures, stopping at `0.5*H - pad`
or 12 · F4 passive path unchanged INCLUDING activation · TWO alpha regimes.

DRILLS on a JOINING CLIENT: D1 TTL crossing while open · D2 a frame after close settles · D3 low-fps
reveal duration · D4 join->leave->rejoin. D1-D3 blocked until 0b.

FILED, NOT BUNDLED: instant overflow pop vs «исчезают плавно»; ALPHA-JUMP `bornMs` pair-match.

---

# ROUND 11 — asked and answered

**A1 NO TERM KEEPS IT ALIVE — "no new gate term" WAS WRONG.** In the quiet lobby at close, `IsOpen()`
-> false so `AnyOpen()` -> false; `HasAny()` is live-only so `hud::IsActive()` -> false; curtain false.
**No frame is built, so the symmetric CLOSE ramp never draws** -> the instant collapse round 4
established as a layout jump. Round 10's A1 correctly found the OPEN path needs nothing new; I
over-generalised it to the CLOSE path, which is not symmetric with it.

**A2 WIDENING `ChatOpen()` WOULD BE THE WRONG FIX — THE FILE SAYS SO.** `ChatOpen()` feeds
`CaptureActive():135`, so widening it holds the cursor + typed-key swallow for the whole close
duration after every Enter — a keystroke eaten on every send. The file's own curtain comment names the
answer: *"Added to the RENDER gate only (NOT AnyOpen, which also gates input/cursor)."* So the ramp
gets **its OWN term in the RENDER gate at `:458` only**, beside `join_curtain::IsActive()` — never in
`AnyOpen()`, never in `CaptureActive()`. The precedent was already in the file, one line above the
gate I was editing.

**A3 IT WOULD HAVE PASSED BY CONSTRUCTION — third time this pass that check has caught something.**
With no frame after close, D2 samples nothing and reports green. With the render-gate term it samples
real frames, but "a frame after settle" is a ONE-FRAME window and therefore fragile. SHARPENED: **D2
samples the WHOLE CLOSE RAMP and asserts `totalH` decreases MONOTONICALLY to the passive baseline,
with the final sampled frame EQUAL to that baseline** — in a quiet lobby, zero.

# SHAPE AFTER ROUND 11

GATE: a NEW term `ui::chat_input::RevealActive()` (ramp != 0) in the **RENDER gate only**
(`imgui_overlay.cpp:458`), beside `join_curtain::IsActive()`. **NEVER in `AnyOpen()` and NEVER in
`CaptureActive()`** — widening `ChatOpen()` would eat a keystroke after every send. The file's curtain
comment is the cited precedent.

ACTIVATION: `chat_feed::HasAny()` = LIVE count > 0. Open needs nothing new (WndProc atomic ->
`AnyOpen()`); CLOSE needs the render-gate term above.

RAMP: owns its own `steady_clock`; value = f(now, transitionStart, startValue, target); transitionStart
set when the OBSERVED target differs from the STORED one. Exact 0/1 by construction.

IDENTITY: monotone `seq` rides the published `Line`; `bornMs` = fade clock only; fix BOTH false
comments (`chat_feed.h:32`, `chat_feed.cpp:149`).

SNAPSHOT: POD holds the UNION (~4.9 KB), copied ONLY on a `gen` change.

BUILD ORDER: 0 D0 digest probe + baseline x2 + 3 must-FAIL mutations RED · 0b harness hold-chat-open ·
1 container-owning type (+`seq`, +both comment fixes; digest identical) · 2 F1-F4 + activation
predicate + the render-gate term · 3 rename `g_history` -> `g_sendRecall`.
NOT IN ORDER: row-split cache (unmeasured at `kMaxLines=6`).

FEATURE: F1 retention 12 count-bounded never age-bounded · F2 wall-clock symmetric ramp, stored target
· F3 ONE walk newest-first by `seq` that BOTH selects and measures, stopping at `0.5*H - pad` or 12 ·
F4 passive path unchanged INCLUDING activation · TWO alpha regimes.

DRILLS on a JOINING CLIENT: D1 TTL crossing while open · **D2 the WHOLE close ramp: `totalH` monotone
down to the passive baseline, final frame == baseline** · D3 low-fps reveal duration · D4
join->leave->rejoin. D1-D3 blocked until 0b.

FILED, NOT BUNDLED: instant overflow pop vs «исчезают плавно»; ALPHA-JUMP `bornMs` pair-match.

---

# ROUND 12 — asked and answered

**A1 THE HEADLINE CASE IS DRAWN ZERO TIMES (measured).** `imgui_overlay.cpp:359` is
`if (ui::hud::IsActive() && !PauseMenuOpen()) ui::hud::Render();` — a **SECOND gate**, and
`hud::Render()` is the ONLY caller of `DrawChat`. With `HasAny()` narrowed to live-only, a quiet lobby
has `IsActive()` false, so pressing T admits the frame at `:458` and then **NEVER DRAWS THE CHAT**. My
round-9 activation fix broke the exact case it was meant to serve, and round 10's "it dissolves
outright" was wrong for a SECOND reason: I checked `:458` and never looked at `:359`.

**A2 A PREDICATE WITH TWO CONSUMERS — I described it as one term.** `RevealActive()` must appear in
the RENDER gate (`:458`, so a frame exists) **AND** in `hud::IsActive()` (`hud.cpp:416`, so the chat is
DRAWN). D2 samples `totalH`, computed inside `DrawChat`, so D2 was always aiming at the `:359`
consumer — the one I had not gated.

**A3 THE WORST OF THE THREE.** `config_registry_rows.inc:113` declares `voice_enabled` default
**`true`**, and the smoke logs confirm voice starts on every peer
(`voice_chat: devices (capture=ok playback=ok loopback=0 tone=0)`). So `voice_chat::Enabled()` is TRUE
in every drill environment -> `hud::IsActive()` is UNCONDITIONALLY TRUE there -> **every drill I
specified would have PASSED while both defects shipped**, visible only to a user who turns voice off.
A masking condition in the ENVIRONMENT, not in the code. **D0-D4 now all carry the precondition
`voice.enabled=false`.** Fourth instance of the blind-instrument shape in this pass.

# SHAPE AFTER ROUND 12

`RevealActive()` (ramp != 0) is a PREDICATE WITH TWO CONSUMERS:
  - `imgui_overlay.cpp:458` RENDER gate (a frame exists), beside `join_curtain::IsActive()`.
  - `hud.cpp:416` `hud::IsActive()` (the chat is DRAWN via `:359`).
  **NEVER in `AnyOpen()`, NEVER in `CaptureActive()`** (would eat a keystroke after every send).

ACTIVATION: `chat_feed::HasAny()` = LIVE count > 0. Open admits the frame via the WndProc atomic ->
`AnyOpen()`; DRAWING in a quiet lobby requires `RevealActive()` in `hud::IsActive()`; CLOSE requires it
in both.

DRILL PRECONDITION (new, mandatory): **`voice.enabled=false`** on every drill peer. Otherwise
`hud::IsActive()` is incidentally true and the instrument is blind to exactly what it tests.

RAMP: own `steady_clock`; f(now, transitionStart, startValue, target); transitionStart set when the
OBSERVED target differs from the STORED one. Exact 0/1 by construction.

IDENTITY: monotone `seq` on the published `Line`; `bornMs` = fade clock only; fix both false comments.

SNAPSHOT: POD holds the UNION (~4.9 KB), copied ONLY on a `gen` change.

BUILD ORDER: 0 D0 digest probe + baseline x2 + 3 must-FAIL mutations RED (voice off) · 0b harness
hold-chat-open · 1 container-owning type (+`seq`, +both comment fixes; digest identical) · 2 F1-F4 +
activation predicate + `RevealActive()` in BOTH consumers · 3 rename `g_history` -> `g_sendRecall`.
NOT IN ORDER: row-split cache (unmeasured).

FEATURE: F1 retention 12 count-bounded never age-bounded · F2 wall-clock symmetric ramp, stored target
· F3 ONE walk newest-first by `seq` that BOTH selects and measures, stopping at `0.5*H - pad` or 12 ·
F4 passive path unchanged INCLUDING activation · TWO alpha regimes.

DRILLS on a JOINING CLIENT, ALL with voice OFF: D1 TTL crossing while open · D2 the whole close ramp
(`totalH` monotone down to the passive baseline, final frame == baseline) · D3 low-fps reveal duration
· D4 join->leave->rejoin. D1-D3 blocked until 0b.

FILED, NOT BUNDLED: instant overflow pop vs «исчезают плавно»; ALPHA-JUMP `bornMs` pair-match.

---

# ROUND 13 — asked and answered

**A1 THE SAME DEFECT ONE LAYER UP, AND WORSE (measured).** `hud.cpp:384` and `:388` BOTH resolve
colour from `l.slot` **at DRAW time** — `PackedForSlot` reads the live table, `kSlotCols[l.slot % 8]`
is a live index — while the nick TEXT was baked at push. Arc A established slots RECYCLE lowest-free
with **no absence in between**. So a ten-minute-old retained line reading `Пельмень: привет` renders in
the colour of whoever holds slot 2 NOW — **the message is visually attributed to a person who never
sent it.** Today nearly unreachable (11 s lines); retention makes it routine. Exactly the class just
fixed on `bornMs`: **a field that is a REFERENCE, not an IDENTITY, resolved LATE.**

**A2 `slot` SHOULD LEAVE THE PUBLISHED `Line` ENTIRELY (measured).** Every consumer of `Line::slot` in
`hud.cpp` is exactly two hits, `:384` and `:388`, both the colour. Baking the RESOLVED PACKED COLOUR
onto the `Entry` at push turns retention into a **sender-identity SNAPSHOT** and leaves `slot` with no
consumer at all. Keeping it would leave a second late-bound reader alive for someone to reach for
later — which is how this defect got here.

**A3 NO SPECIFIED DRILL WOULD SEE IT.** D4 exercises the client-under-test's OWN `Reset`/`PushDelayed`;
a REMOTE peer departing and being replaced never resets the OBSERVER's feed. **NEW DRILL D5:** observer
holds retained lines from slot 2 -> slot-2 peer leaves -> a new peer joins and takes slot 2 -> assert
the retained lines' colour is UNCHANGED.

# SHAPE AFTER ROUND 13

IDENTITY, now TWO fields demoted from late-bound references to push-time snapshots:
  - `seq` (monotone) replaces `bornMs` as entry identity; `bornMs` = fade clock only.
  - **The resolved packed nick COLOUR is baked onto the `Entry` at push**; `slot` LEAVES the published
    `Line` (its only two consumers, `hud.cpp:384/388`, are the colour).
  General rule this pass keeps rediscovering: **anything a retained line renders must be captured at
  push, because retention outlives every live table it could point at.**

`RevealActive()` = a predicate with TWO consumers: `imgui_overlay.cpp:458` (a frame exists) and
`hud.cpp:416` `hud::IsActive()` (the chat is drawn via `:359`). NEVER `AnyOpen()`/`CaptureActive()`.

ACTIVATION: `chat_feed::HasAny()` = LIVE count > 0.
RAMP: own `steady_clock`; f(now, transitionStart, startValue, target); stored-target comparison.
SNAPSHOT: POD holds the UNION (~4.9 KB), copied ONLY on a `gen` change.

BUILD ORDER: 0 D0 digest probe + baseline x2 + 3 must-FAIL mutations RED (voice off) · 0b harness
hold-chat-open · 1 container-owning type (+`seq`, +baked colour, -`slot`, +both comment fixes; digest
identical) · 2 F1-F4 + activation predicate + `RevealActive()` in both consumers · 3 rename
`g_history` -> `g_sendRecall`. NOT IN ORDER: row-split cache (unmeasured).

FEATURE: F1 retention 12 count-bounded never age-bounded · F2 wall-clock symmetric ramp · F3 ONE walk
newest-first by `seq` that BOTH selects and measures, stopping at `0.5*H - pad` or 12 · F4 passive path
unchanged INCLUDING activation · TWO alpha regimes.

DRILLS on a JOINING CLIENT, ALL with voice OFF: D1 TTL crossing while open · D2 the whole close ramp ·
D3 low-fps reveal · D4 join->leave->rejoin · **D5 slot recycle under retained lines (colour unchanged)**.
D1-D3 blocked until 0b.

FILED, NOT BUNDLED: instant overflow pop vs «исчезают плавно»; ALPHA-JUMP `bornMs` pair-match.

---

# ROUND 14 — asked and answered

**A1 BAKING AT PUSH IS A REGRESSION AND AN OWNERSHIP VIOLATION.** `nick_color.h:3` declares a SINGLE
owner for the colour axis, and the F1 picker makes a peer's colour MUTABLE mid-session, so today a
change repaints that peer's LIVE lines next frame. Baking at PUSH would freeze them and make
`chat_feed` a SECOND owner. Round 13's fix was right about the defect and **wrong about the moment**.

**A2 CAPTURE AT RETIREMENT — ONE RULE, ONE SENTENCE.** *A line late-binds while its sender is still the
live authority for it; retirement is the moment that authority ends, so retirement is the capture.*
One rule, one transition — **the regime IS the transition**, so there is no per-regime source and no
second owner: the bake only exists AFTER the entry has left the live set, i.e. after the colour
module's authority over that line has already ended. Live lines keep late-binding exactly as today.
STATED, NOT HIDDEN: a slot that recycles while a line is still LIVE still repaints for up to 11 s.
That is today's behaviour, bounded, and retention does not worsen it — **filed as pre-existing, not
bundled**, same as the instant overflow pop.

**A3 D5 WOULD READ GREEN ON A BUILD THAT ALSO FROZE LIVE LINES; D0 IS BLIND TO COLOUR. FIFTH
INSTANCE.** D5 gains a COMPANION assertion — **change a peer's colour while its lines are LIVE and
assert they REPAINT** — and **D0's digest must include the resolved colour**, or the whole colour axis
passes by construction.

# SHAPE AFTER ROUND 14 (the design of record)

THE ONE IDENTITY RULE (this pass rediscovered it three times, at three layers):
  **A line late-binds while its sender is still the live authority for it; RETIREMENT is the moment
  that authority ends, so retirement is the capture.**
  - `seq` (monotone, ++ per push) is entry identity; `bornMs` is the FADE CLOCK only.
  - The resolved packed nick COLOUR is captured **at RETIREMENT** (not at push); live lines keep
    late-binding, so the F1 colour picker still repaints them and `nick_color` stays the one owner.
  - `slot` leaves the published `Line` (its only consumers, `hud.cpp:384/388`, are the colour).

`RevealActive()` = a predicate with TWO consumers: `imgui_overlay.cpp:458` (a frame exists) and
`hud.cpp:416` `hud::IsActive()` (the chat is drawn via `:359`). NEVER `AnyOpen()`/`CaptureActive()`.
ACTIVATION: `chat_feed::HasAny()` = LIVE count > 0.
RAMP: own `steady_clock`; f(now, transitionStart, startValue, target); stored-target comparison; exact
0/1 by construction.
SNAPSHOT: POD holds the UNION (~4.9 KB), copied ONLY on a `gen` change.

BUILD ORDER: 0 D0 digest probe (**incl. resolved colour**) + baseline x2 + 3 must-FAIL mutations RED
(voice off) · 0b harness hold-chat-open · 1 container-owning type (+`seq`, +retirement colour capture,
-`slot`, +both comment fixes; digest identical) · 2 F1-F4 + activation predicate + `RevealActive()` in
both consumers · 3 rename `g_history` -> `g_sendRecall`. NOT IN ORDER: row-split cache (unmeasured).

FEATURE: F1 retention 12 count-bounded never age-bounded · F2 wall-clock symmetric ramp · F3 ONE walk
newest-first by `seq` that BOTH selects and measures, stopping at `0.5*H - pad` or 12 · F4 passive path
unchanged INCLUDING activation · TWO alpha regimes.

DRILLS on a JOINING CLIENT, ALL with voice OFF: D1 TTL crossing while open · D2 the whole close ramp ·
D3 low-fps reveal · D4 join->leave->rejoin · D5 slot recycle under retained lines (colour unchanged)
**+ its companion: a colour change while lines are LIVE must REPAINT them**.

FILED, NOT BUNDLED: instant overflow pop vs «исчезают плавно» · ALPHA-JUMP `bornMs` pair-match ·
live-line repaint inside the 11 s window on a slot recycle.

---

# ROUND 15 — asked and answered (the cap was hit WITHOUT convergence; thread continued by the user)

**A1 DISCONNECT, NOT RETIREMENT (measured).** `player_handshake.cpp:275` calls
`nick_color::OnSlotDisconnected(slot)`; `nick_color.cpp:130-134` zeroes `g_bySlot[slot]`, under a
teardown arc A established ALSO fires on a REPLACEMENT. Authority ends at DISCONNECT; retirement can
trail it by up to 8 s. **Round 14's rule was WRONG**, and wrong the same way round 13's was: I named a
moment that FELT like the boundary instead of measuring where the authority actually dies.

**A2 "RETENTION DOES NOT WORSEN IT" IS FALSE — WITHDRAWN.** Capture-at-retirement reads a CLEARED or
REPLACEMENT slot and freezes that wrong colour PERMANENTLY, and the line visibly changes colour at the
live->retired crossing, possibly mid-reveal. **ROOT FIX INVERTS THE DIRECTION: `nick_color` PUSHES
into the store on change**, instead of the draw PULLING from a live table. Live lines repaint because
the owner pushed; at disconnect the stored value is already the last valid one; retirement becomes
IRRELEVANT to colour; the crossing has no colour change at all. Single owner preserved — it now WRITES
rather than being READ.

**A3 NONE. NEW DRILL D6:** a LIVE line whose sender departs inside the 11 s window; assert the colour
holds across BOTH the departure and the retirement.

**A4 A REAL INVARIANT BREAK (measured).** `Retire()` cannot be the route for `Tick():227`'s promotion
erase: promotion is a MOVE INTO the live set, not out of it, so routing it through `Retire()` would
put the entry in BOTH sets and break "in exactly one set". The owning type needs **`Promote()`
(pending->live) as a separate transition**, with **`Retire()` defined as the only path OUT OF THE LIVE
SET** — not "the only mutation". Never-promoted entries dropped at `Reset` are correctly neither.

# SHAPE AFTER ROUND 15

TRANSITIONS (the owning type; three, not one):
  - `Promote()` pending -> live.
  - `Retire()` live -> retained. **THE ONLY PATH OUT OF THE LIVE SET** (not "the only mutation").
  - `Reset()` drops everything; never-promoted pending entries are neither promoted nor retired.
  Invariant unchanged: *no mutation of the store is observable except through a republished snapshot.*

COLOUR — DIRECTION INVERTED: **`nick_color` PUSHES into the store on change; the draw never pulls.**
Live lines repaint because the owner pushed. At disconnect the stored value is already the last valid
one. Retirement is irrelevant to colour. `slot` still leaves the published `Line`.

IDENTITY: `seq` (monotone, ++ per push); `bornMs` = fade clock only; fix both false comments
(`chat_feed.h:32`, `chat_feed.cpp:149`).

`RevealActive()` = predicate with TWO consumers: `imgui_overlay.cpp:458` + `hud.cpp:416`. NEVER
`AnyOpen()`/`CaptureActive()`. ACTIVATION: `HasAny()` = LIVE count > 0.
RAMP: own `steady_clock`; f(now, transitionStart, startValue, target); stored-target comparison.
SNAPSHOT: POD holds the UNION (~4.9 KB), copied ONLY on a `gen` change.

BUILD ORDER: 0 D0 digest probe (incl. resolved colour) + baseline x2 + 3 must-FAIL mutations RED (voice
off) · 0b harness hold-chat-open · 1 owning type (+3 transitions, +`seq`, -`slot`, +colour push seam,
+both comment fixes; digest identical) · 2 F1-F4 + activation + `RevealActive()` in both consumers ·
3 rename `g_history` -> `g_sendRecall`. NOT IN ORDER: row-split cache (unmeasured).

FEATURE: F1 retention 12 count-bounded never age-bounded · F2 wall-clock symmetric ramp · F3 ONE walk
newest-first by `seq` that BOTH selects and measures, stopping at `0.5*H - pad` or 12 · F4 passive path
unchanged INCLUDING activation · TWO alpha regimes.

DRILLS on a JOINING CLIENT, ALL with voice OFF: D1 TTL crossing while open · D2 whole close ramp ·
D3 low-fps reveal · D4 join->leave->rejoin · D5 slot recycle under retained lines + companion (colour
change while LIVE must repaint) · **D6 a LIVE line whose sender departs inside the 11 s window**.

FILED, NOT BUNDLED: instant overflow pop vs «исчезают плавно» · ALPHA-JUMP `bornMs` pair-match.

---

# ROUND 16 — asked and answered

**A1 A SITE LIST, BUILT ONE ROUND AFTER I CALLED SITE LISTS OUT (measured).** `g_bySlot` has FOUR
writers: `:87` (`RequestLocal`, DIRECT — **the local F1 picker BYPASSES `StoreForSlot` entirely**, so
D5's companion drill would FAIL against my own fix), `:99` `StoreForSlot`, `:127` `ResetSlots`, `:133`
`OnSlotDisconnected`. And `:125-126` documents `ResetSlots` as running on the **BRINGUP THREAD**, while
`chat_feed.cpp:43` documents `g_lines` as **GAME-THREAD ONLY** -> "push on change" would put a
cross-thread write into the store. **Round 15's inversion is DEAD.**

**A2 THE PART I NEVER SAW.** The colour has **TWO HALVES**: the custom packed value in
`coop/nick_color`, and the default `kSlotCols[slot % 8]` palette in **`ui/hud.cpp:335-344`**, which is
what EVERY peer who never opened the picker renders as (`nick_color` stores 0 = "no custom"). Removing
`slot` from the published `Line` STRANDS the default half, and resolving it inside `nick_color` is the
`coop/ -> ui/` inversion round 2 dissolved.

**A3 `nick_color.cpp:3` ALREADY INCLUDES `chat_feed.h` — the reverse include is a CYCLE (measured).**

**THE ROOT NONE OF THE LAST FOUR ROUNDS NAMED: the colour axis has TWO OWNERS, and the header claiming
one is WRONG.** `nick_color.h:3` says "The COLOR AXIS has ONE owner: this module" while `ui/hud.cpp`
owns the palette most peers actually display. Bake-at-push, bake-at-retirement and push-on-change were
all arranging deck chairs on a SPLIT OWNERSHIP.

**FIX = the one the header already claims.** `kSlotCols` MOVES INTO `nick_color` (per-slot DATA, not
`ui/` code, so the dependency goes DOWN, not up); `PackedForSlot` returns a REAL colour always instead
of `0 = no custom`. One value, one owner. The freeze then becomes answerable:
  - `Republish()` recomputes the colour for LIVE entries, on the game thread it already runs on.
  - The **teardown fan-out arc A already established** (`player_handshake.cpp:273-277`, already calling
    FOUR `OnSlotDisconnected` handlers) gains a FIFTH that FREEZES the colour of that slot's entries.
  Disconnect is where authority dies, so disconnect is where the freeze belongs — and it is a ROW
  TRANSITION, the shape arc A built for exactly this.

# SHAPE AFTER ROUND 16

COLOUR — ONE OWNER FOR REAL:
  - `kSlotCols` moves `ui/hud.cpp` -> `coop/player/nick_color`; `PackedForSlot` always returns a real
    colour (no more `0 = no custom` sentinel at the draw site). No `coop/ -> ui/` edge, no cycle.
  - LIVE entries: colour recomputed in `Republish()` (game thread, already runs on every mutation+Tick).
  - FREEZE at DISCONNECT via a FIFTH handler in the arc-A teardown fan-out
    (`chat_feed::OnSlotDisconnected`), because disconnect is where authority dies. Retirement is
    IRRELEVANT to colour.
  - `slot` still leaves the published `Line`; the resolved colour rides it instead.

TRANSITIONS: `Promote()` pending->live · `Retire()` live->retained (**the only path OUT of the live
set**) · `Reset()` drops everything. Invariant: *no mutation of the store is observable except through
a republished snapshot.*

IDENTITY: `seq` monotone; `bornMs` = fade clock only; fix both false comments.
`RevealActive()` = predicate, TWO consumers (`imgui_overlay.cpp:458` + `hud.cpp:416`); never
`AnyOpen()`/`CaptureActive()`. ACTIVATION: `HasAny()` = LIVE count > 0.
RAMP: own `steady_clock`, stored-target comparison, exact 0/1.
SNAPSHOT: POD holds the UNION (~4.9 KB), copied ONLY on a `gen` change.

BUILD ORDER: 0 D0 digest (incl. resolved colour) + baseline x2 + 3 must-FAIL mutations RED (voice off)
· 0b harness hold-chat-open · **0c the colour-ownership merge (`kSlotCols` -> `nick_color`), its own
commit, digest identical** · 1 owning type (3 transitions, `seq`, -`slot`, teardown handler, both
comment fixes) · 2 F1-F4 + activation + `RevealActive()` · 3 rename `g_history` -> `g_sendRecall`.

DRILLS on a JOINING CLIENT, voice OFF: D1 TTL crossing while open · D2 whole close ramp · D3 low-fps
reveal · D4 join->leave->rejoin · D5 slot recycle + companion (colour change while LIVE must repaint,
**via the F1 picker path `:87`, not `StoreForSlot`**) · D6 a LIVE line whose sender departs in the 11 s
window.

---

# ROUND 17 — asked and answered. THE COLOUR THREAD IS STOPPED BY THE PROJECT'S OWN OSCILLATION RULE.

**A1 (measured).** `IsCustom()` is a SHARED DISCRIMINATOR across THREE surfaces with THREE different
defaults: chat `kSlotCols[slot%8]` (`hud.cpp:388`), nameplate WHITE, scoreboard `host=gold /
client=soft-white` (`scoreboard.cpp:195-196`). Making `PackedForSlot` "always real" forces `IsCustom`
permanently TRUE and **silently repaints the plates and the scoreboard**, and `nick_color.cpp:47-48`
documents the sentinel as a USER-VISIBLE INI SEMANTIC ("an explicitly EMPTY value = the per-surface
defaults"). The `0` I called a wart is LOAD-BEARING in three places.

**A2 (measured).** `roster_ledger.h:169` — subscribers fire in REGISTRATION ORDER — and `:275` already
zeroes `g_bySlot` in the same body. Appending a freeze after it reads ZERO and freezes the WRONG
colour, re-introducing exactly the load-bearing ordering arc A removed.

**A3.** 0c's digest is chat-store-events only -> blind to two of the three surfaces 0c changes.
SEVENTH instance of that shape this pass.

## STOP SIGNAL APPLIED (LESSONS:617-627)

Rounds 13->14->15->16->17 were ALL the colour axis, and EACH fix replaced the previous one: bake at
push -> bake at retirement -> push-on-change -> merge the palette -> all dead. The lesson says treat
the SECOND reversal on one axis as a stop signal. I am FOUR reversals past it. Side by side, as the
lesson requires:
  - **THE GROUND:** the user asked for chat history on T. Nothing about colour.
  - **THE AXIS BEING ARGUED:** which live table a retained line's colour is read from, and when. That
    question exists ONLY because retention makes a line outlive its sender, and it carries a
    three-surface blast radius, its own ownership defect and its own ordering hazard.

## THE DISSOLUTION

**A retained line does not need anyone's colour.** It keeps the nick TEXT baked at push (attribution
stays correct) and renders the nick in a NEUTRAL HISTORY COLOUR. No freeze, no capture moment, no
ownership merge, no teardown handler, no `IsCustom` change, no ordering dependency — and it CANNOT
misattribute a message to a person who never sent it, which was the actual defect in round 13. It also
READS as what it is: history, not live chat.

CONSEQUENCES: `slot` STAYS on the published `Line` exactly as today (**round 13's removal was
premature**). Live lines keep late-binding, unchanged. `kSlotCols` stays in `ui/`. `nick_color` is
untouched. Drills D5 + companion and D6 are RETIRED with the axis that motivated them; 0c leaves the
build order.

# SHAPE AFTER ROUND 17

COLOUR: retained lines render the nick in a NEUTRAL history colour. Live lines unchanged. No new
ownership, no capture, no teardown handler. `slot` stays on the `Line`.

TRANSITIONS: `Promote()` pending->live · `Retire()` live->retained (the only path OUT of the live set)
· `Reset()` drops everything. Invariant: *no mutation of the store is observable except through a
republished snapshot.*

IDENTITY: `seq` monotone (ordering + any cache key); `bornMs` = fade clock only; fix both false
comments (`chat_feed.h:32`, `chat_feed.cpp:149`).

`RevealActive()` = predicate, TWO consumers (`imgui_overlay.cpp:458` + `hud.cpp:416`); NEVER
`AnyOpen()`/`CaptureActive()`. ACTIVATION: `HasAny()` = LIVE count > 0.
RAMP: own `steady_clock`; f(now, transitionStart, startValue, target); stored-target comparison;
exact 0/1.
SNAPSHOT: POD holds the UNION (~4.9 KB), copied ONLY on a `gen` change.

BUILD ORDER: 0 D0 digest probe + baseline x2 + 3 must-FAIL mutations RED (voice off) · 0b harness
hold-chat-open · 1 owning type (3 transitions, `seq`, both comment fixes; digest identical) · 2 F1-F4 +
activation + `RevealActive()` in both consumers · 3 rename `g_history` -> `g_sendRecall`.
NOT IN ORDER: row-split cache (unmeasured); the colour axis (dissolved).

FEATURE: F1 retention 12 count-bounded never age-bounded · F2 wall-clock symmetric ramp · F3 ONE walk
newest-first by `seq` that BOTH selects and measures, stopping at `0.5*H - pad` or 12 · F4 passive path
unchanged INCLUDING activation · TWO alpha regimes.

DRILLS on a JOINING CLIENT, voice OFF: D1 TTL crossing while open · D2 whole close ramp · D3 low-fps
reveal · D4 join->leave->rejoin. (D5/D6 retired with the colour axis.)

FILED, NOT BUNDLED: instant overflow pop vs «исчезают плавно» · ALPHA-JUMP `bornMs` pair-match ·
**the live-line colour repaint on a slot recycle inside the 11 s window (PRE-EXISTING, bounded)**.

---

# ROUND 18 — asked and answered, + THE USER'S DECISION

**A1 NOTHING MARKS RETAINED; ALPHA WOULD BE THE ROUND-5 TRAP.** A live line in its 1,500 ms tail also
has alpha -> 0, so an implementer reaching for alpha paints live chat as history. But the invariant is
REAL and follows from the three transitions: `g_lines` is FIFO and both drains pop the FRONT, so
retirement always happens in `seq` order -> **retained is a strict `seq`-PREFIX of the union**.
Membership is published as ONE NUMBER, **`firstLiveSeq`**: `seq < firstLiveSeq` is retained. No
per-line flag; derived from an invariant, not inferred from a rendering value. Was NOT in the shape.

**A2 CROSS-ANSWER CONTRADICTION — CONCEDED.** Round 15 rejected capture-at-retirement PARTLY because
the line visibly changes colour at the crossing; my round-17 dissolution REINTRODUCED the identical
flip (per-slot -> neutral), TTL-fired, mid-read, two lines from one sender in two colours. I missed it
because I was relieved to be rid of the ownership problem. And: "history looks different from live
chat" is a PRODUCT-FEEL call, not an engineering dissolution. What the dissolution genuinely buys
(removal of the ownership merge, the ordering hazard, the three-surface blast radius) stands.

**A3 IT COVERS ONLY `nickCol` (measured).** `hud.cpp:378-380` sets `body` to a YELLOW LITERAL for
`action` lines and `nickCol` is initialised FROM `body`, so a ten-minute-old "<nick> deleted an email"
would sit in full-saturation yellow inside a grey block. If history is visually distinct at all, the
treatment must be the WHOLE ROW.

## USER DECISION (2026-07-29): **(b) — no visual distinction. History just appears.**

The cost was named before the choice and accepted: a retained line from a departed sender renders in
whatever colour that slot now holds (a narrow misattribution). Per the standing rule, that is the
user's call and the work proceeds.

**CONSEQUENCE — THE WHOLE COLOUR AXIS RESOLVES TO "CHANGE NOTHING":**
  - `slot` STAYS on the published `Line` (round 13's removal stays reverted).
  - Colour resolution stays LATE-BOUND at `hud.cpp:384/388`, exactly as today.
  - NO capture, NO freeze, NO teardown handler, NO ownership merge, NO `IsCustom` change, NO
    `kSlotCols` move, NO neutral history colour, NO whole-row treatment.
  - `nick_color` is not touched. 0c leaves the build order. D5/D6 stay retired.
  - There is NO crossing flip of any kind, because nothing changes at the crossing.
  - **FILED, NOT FIXED (user-accepted):** a retained line's colour follows its slot's CURRENT occupant.

Six rounds on the colour axis produced a diff of ZERO. That is a real result: the design got smaller.

# SHAPE AFTER ROUND 18 (user decision folded in)

MEMBERSHIP: publish `firstLiveSeq`; `seq < firstLiveSeq` = retained. Retained is a strict `seq`-prefix
by construction of the three transitions. NEVER infer membership from alpha.

TRANSITIONS: `Promote()` pending->live · `Retire()` live->retained (**the only path OUT of the live
set**) · `Reset()` drops everything. Invariant: *no mutation of the store is observable except through
a republished snapshot.*

IDENTITY: `seq` monotone (identity + ordering + membership); `bornMs` = fade clock only; fix both false
comments (`chat_feed.h:32`, `chat_feed.cpp:149`).

COLOUR: unchanged from today in every respect.

`RevealActive()` = predicate, TWO consumers (`imgui_overlay.cpp:458` + `hud.cpp:416`); NEVER
`AnyOpen()`/`CaptureActive()`. ACTIVATION: `HasAny()` = LIVE count > 0.
RAMP: own `steady_clock`; f(now, transitionStart, startValue, target); stored-target comparison;
exact 0/1 by construction.
SNAPSHOT: POD holds the UNION (~4.9 KB) + `firstLiveSeq` + `gen`; copied ONLY on a `gen` change.

BUILD ORDER: 0 D0 store-event digest + baseline x2 + 3 must-FAIL mutations RED (voice off) · 0b harness
hold-chat-open · 1 owning type (3 transitions, `seq`, `firstLiveSeq`, both comment fixes; digest
identical) · 2 F1-F4 + activation + `RevealActive()` in both consumers · 3 rename `g_history` ->
`g_sendRecall`. NOT IN ORDER: row-split cache (unmeasured); the colour axis (zero diff).

FEATURE: F1 retention 12 count-bounded never age-bounded · F2 wall-clock symmetric ramp · F3 ONE walk
newest-first by `seq` that BOTH selects and measures, stopping at `0.5*H - pad` or 12 · F4 passive path
unchanged INCLUDING activation · TWO alpha regimes (live = bornMs TTL, retained = 0).

DRILLS on a JOINING CLIENT, voice OFF: D1 TTL crossing while open · D2 whole close ramp · D3 low-fps
reveal · D4 join->leave->rejoin.

FILED, NOT BUNDLED: instant overflow pop vs «исчезают плавно» · ALPHA-JUMP `bornMs` pair-match ·
retained-line colour follows the slot's current occupant (USER-ACCEPTED).

---

# ROUND 19 — asked and answered

**A1 NOTHING READS `firstLiveSeq`. DELETED.** Walked every consumer: alpha comes from the store
(retained 0, live TTL) and composes via `max(store, ramp)` for BOTH; the F3 walk orders by `seq` alone;
layout membership is the ALPHA PREDICATE; `HasAny()` is a store-side LIVE COUNT. `firstLiveSeq` was
minted ONE ROUND AGO to serve a visual treatment that no longer exists, and I carried it into the
post-decision shape out of MOMENTUM. A write-only published field — the exact shape `registry_gate`
exists to reject. **GONE.**

**A2 MOOT, OBSERVATION KEPT.** With an empty live set it would have had no defined value, and `0`
would classify every retained line as LIVE — on the quiet-lobby case that IS the user's headline.
`liveCount` is the same fact with a TOTAL definition and no sentinel, and it is what `HasAny()` needs
anyway.

**A3 NEVER NAMED — AND I PICK IT RATHER THAN ASK.** Eighteen shapes specified the ramp's clock,
symmetry, target and exactness and never its DURATION; D3 had no pass criterion. The number is
**220 ms**, from `chat_feed.cpp:26` `kFadeInMs = 220` — the arrival ramp the user already tuned for
THIS surface ("short enough to feel instant, long enough to read as motion"). Reuse of an established
constant from the same subsystem, not a fresh game-feel guess, so it does not go back to the user.
**D3 CRITERION: the reveal completes in 220 ms +/- one frame at ANY frame rate** — precisely what the
wall-clock ramp buys and what a frame-count ramp would fail at 4.2 fps.

# SHAPE AFTER ROUND 19

STORE — three transitions on an owning type: `Promote()` pending->live · `Retire()` live->retained
(**the only path OUT of the live set**) · `Reset()` drops everything.
Invariant: *no mutation of the store is observable except through a republished snapshot.*

PUBLISHED SNAPSHOT: the UNION array (~4.9 KB) + `liveCount` + `gen`. Copied by the render side ONLY on
a `gen` change. (`firstLiveSeq` deleted — nothing read it.)

IDENTITY: `seq` monotone (identity + ordering); `bornMs` = FADE CLOCK only; fix both false comments
(`chat_feed.h:32`, `chat_feed.cpp:149`).

ALPHA: TWO regimes — live = `bornMs` TTL fade, retained = 0. Final = `max(storeAlpha, revealRamp)`.
Layout membership is a PREDICATE (alpha >= threshold), never a consequence of alpha.

COLOUR: **zero diff.** Late-bound at `hud.cpp:384/388` exactly as today; `slot` stays on the `Line`;
`nick_color` untouched. Retained line on an emptied slot falls back to `kSlotCols[slot % 8]` — defined.

RAMP: own `steady_clock`; value = f(now, transitionStart, startValue, target); transitionStart set when
the OBSERVED target differs from the STORED one; exact 0/1 by construction; **duration 220 ms
(= `kFadeInMs`)**; symmetric open and close.

GATES: `RevealActive()` (ramp != 0) is a PREDICATE WITH TWO CONSUMERS — `imgui_overlay.cpp:458` (a
frame exists) and `hud.cpp:416` `hud::IsActive()` (the chat is DRAWN via `:359`). **NEVER `AnyOpen()`,
NEVER `CaptureActive()`.** ACTIVATION: `HasAny()` = LIVE count > 0.

FEATURE: F1 retention 12, count-bounded, never age-bounded · F2 the ramp above · F3 ONE walk
newest-first by `seq` that BOTH selects and measures, stopping at `0.5*H - pad` or 12 entries ·
F4 passive path unchanged INCLUDING activation.

BUILD ORDER: 0 D0 store-event digest + baseline x2 + 3 must-FAIL mutations RED (voice off) · 0b harness
hold-chat-open · 1 owning type (3 transitions, `seq`, both comment fixes; digest identical) · 2 F1-F4 +
activation + `RevealActive()` in both consumers · 3 rename `g_history` -> `g_sendRecall`.
NOT IN ORDER: row-split cache (unmeasured); the colour axis (zero diff).

DRILLS on a JOINING CLIENT, voice OFF: D1 TTL crossing while open · D2 whole close ramp (`totalH`
monotone down to the passive baseline) · **D3 reveal completes in 220 ms +/- one frame at any fps** ·
D4 join->leave->rejoin.

FILED, NOT BUNDLED: instant overflow pop vs «исчезают плавно» · ALPHA-JUMP `bornMs` pair-match ·
retained-line colour follows the slot's current occupant (USER-ACCEPTED, option b).

---

# USER, mid-round-19 (2026-07-29) — THE ACCEPTANCE DRILL, VERBATIM

> "The smoke test should put 10 messages in chat, then wait for chat to settle 10 seconds, screenshot
> it from host and client, then another client joins, should see clear chat, then upon opening he
> should see its old messages"

STEPS, unambiguous parts:
  1. Put **10 messages** in chat (> `kMaxLines = 6`, so the OVERFLOW drain fires — the drill exercises
     `Retire()` via BOTH paths, not just TTL).
  2. **Wait 10 s to settle.** (`kTtlMs = 11000`, so at 10 s the oldest lines are in the fade tail and
     about to expire — this straddles the TTL boundary ON PURPOSE.)
  3. **Screenshot host + client.** Passive state: this is the F4 no-regression evidence, with EYES.
  4. **Another client JOINS** -> must see a **CLEAR chat** (nothing passive).
  5. **On opening (T)** he "should see its old messages".

**STEP 5 IS AMBIGUOUS AND THE READING CHANGES THE WHOLE FEATURE:**
  (A) "its" = THE CHAT'S old messages -> the late joiner is SEEDED with the lobby's history. This makes
      chat history a **WIRE** feature: a new ReliableKind, a protocol bump, a bounded seed payload, and
      an attacker-influenced blob delivered to a joining peer (`docs/security/` surface, cf. the three
      save-transfer lanes that kill a joining client with one packet).
  (B) "its" = the messages HE has received since joining -> chat history is LOCAL-ONLY, and step 5 just
      asserts the reveal works for him too. Zero wire change.
  Grammatically "its" (not "his") points at (A). Step 4 ("should see clear chat") is consistent with
  BOTH: under (A) the seeded lines are already past TTL so they are retained-and-invisible until T.

**PRINCIPLE 8 APPLIES EITHER WAY.** CLAUDE.md: "a new lane is not DONE until its mid-join row exists".
The design so far has NO late-join answer for retention. Under (A) the answer is SEED; under (B) the
answer is "history begins at join", which is a DEFINED semantic (you weren't there, you didn't hear
it), not a suppressive filter — legitimate, but it must be WRITTEN, and it is not.

DRILL VALUE INDEPENDENT OF THE READING: steps 1-4 subsume D1 (TTL crossing), part of D4 (join), and
add the OVERFLOW drain and EYES on the passive path that none of D1-D4 had. It also gives the D0
digest its scripted scenario for free.

---

# ROUND 20 — asked and answered

**A1 THE USER ALREADY DECIDED IT.** Under (B) the joiner's feed is `Reset()` at join and he received
none of the 10 messages -> a (B) build can put NOTHING on screen at step 5. The user wrote a step
UNSATISFIABLE under (B). **It is (A): the late joiner is SEEDED with the lobby's history.** Escalating
would have asked the user to re-decide what the drill already decides.

**A2 BOTH REAL (measured).** A seed landing directly in retained is a **FOURTH transition** (`Seed()`);
the invariant survives (it still publishes) but the arm count changes and must be WRITTEN. Worse:
`seq` is a LOCAL counter, so a message arriving during the join window gets a LOWER `seq` than a seed
delivered afterwards -> the newest-first walk sorts ten-minute-old history ABOVE it. **Seed entries
must be assigned `seq` BELOW every existing entry** (trivial on a bounded array; not in the design).

**A3 THIS SESSION'S ORDERING WAS LUCKY.** The host's retained text was admitted through `OnReliable`,
which had NO strict decode until **`84e0a4e3` this morning** -> the seed would have re-emitted
ill-formed bytes to every future joiner, across sessions. **That fix is a PRECONDITION of this feature.**
Delivery rides **`ConnectReplayForSlot` / `QueueConnectBroadcastForSlot`** (the shape every row in
`COOP_EVENT_JOIN.md` §3.4 uses), NOT a new kind. Refusal is **PER LINE, never whole-blob**, or it is
the fourth "one packet kills a joining client" in `docs/security/`.

**A4 STEP 4 IS NOT FALSIFIABLE; MY STEP-2 CLAIM WAS UNMEASURED (measured).** The join ITSELF pushes
chat: `:506` "<nick> joined the game" on observers, `:523` `PushDelayed("Joined <host>'s game", 5000)`
on the joiner -> "clear chat" fails on lines the feature never touched; the drill must EXPECT them. And
`_type_chat` carries ~1.8 s of sleeps/message with a measured real cadence of **~3.3 s**, so ten
messages take ~30 s and the earliest six **expire while still being sent**. At the screenshot ~3 lines
are LIVE and all 10 are RETAINED. The drill is right; my reason for it was wrong.

**SCOPE REFRAME: chat history is now a WIRE feature** (bounded seed on the connect-replay lane, proto
bump, per-line validation, a principle-8 mid-join row the design was missing). RE-AUDIT: retention
memory is per-peer AND per-seed (round 4's arithmetic); three transitions become four; `seq` stops
being purely local.

---

# USER, mid-round-20 (2026-07-29) — SCROLL, VERBATIM

> "Also when someone opens chat to read history new messages should ALWAYS arrive and move the chat.
> When chat DOESN'T MOVE from new messages is only when user decided to scroll history chat with the
> pg up/pg down buttons"

**THIS REVISES THE EARLIER "last N lines" ANSWER.** N is now the **VIEWPORT**, not the retention cap:
if you can page BACK, retention must hold MORE than one screenful. Round 4's "cap == reveal, retaining
more than we can show is dead memory" is **RETRACTED** — paging is exactly the consumer that makes the
extra retention live.

TWO MODES (standard chat semantics, Minecraft/Discord shape):
  - **FOLLOW** (default): new messages arrive AND move the view. Always.
  - **PINNED**: entered ONLY by the user pressing PgUp/PgDn. New messages still arrive and are still
    retained — only the VIEW stops moving.

MEASURED, and it de-risks the whole thing: `imgui_widgets.cpp:4597-4601` calls
`SetKeyOwner(ImGuiKey_PageUp/PageDown, id)` **only inside `if (is_multiline)`**. Our bar is
single-line `InputTextWithHint` (`chat_input.cpp:109`), so **InputText does NOT claim PgUp/PgDn** and
they are free while the chat bar holds keyboard focus.

OPEN, GENERATED BY THIS REQUIREMENT (for the critic, not the user, unless genuinely product):
  - What RETURNS to FOLLOW? (PgDn to the bottom? closing chat? sending a message?)
  - The retention cap is no longer 12. What is it, and what bounds it now that paging is the consumer?
  - F3's walk needs an OFFSET (an anchor), not just "newest-first".
  - **Anchor invalidation:** if you are pinned at the oldest lines and new messages evict them, the
    anchor's entry is GONE. `seq` is the stable anchor key — but what is the behaviour at the edge?
  - Does PINNED survive a close/reopen, or does closing chat reset to FOLLOW?
  - Interaction with the SEED: a joiner is seeded, opens, pages back — the seed IS the paged content.
