# Chat history — design of record + AS-BUILT (three `/qf` passes, 21 + 17 + 4 rounds, NONE converged)

**Status: BUILT AND DEFECTIVE, 2026-07-29. NOT hands-on. DO NOT hand this to the user as-is.**
Local half `8eea0af6`, wire half `3729097e`. DLL `multivoid-0.9.0n-133.dll`
`b54cad7d9f1a01a8951509ca58cfc1d4`, proto **132 -> 133**.

**READ §19 FIRST** — the IMPLEMENTATION `/qf` pass (2026-07-29 late, 4 rounds) found **twelve
defects in the shipped code**, one of them (§19 #5, the ESC close path) certain to fire in the
first thirty seconds of any hands-on. §20 then records the USER'S REFRAME of 2026-07-29 which
DISSOLVES two of the twelve and changes the wire (proto 134).

**§18 (AS-BUILT) is the record of what BUILDING changed** — three of §12's claims are corrected
there by measurement. Read it second. Then §10.

**The honest shape of this document:** §§1-17 designed the feature, §18 recorded building it,
§19 recorded that building it was not the same as getting it right, and §20 is the user's answer
to the one question §19 could not decide alone. A status of "BUILT" in §18 was TRUE and was
also, on its own, misleading — see `[[lesson-built-and-drilled-is-not-the-same-as-correct]]`.

**§10 onward is the pass-2 design as written BEFORE the build.** Pass 2 (2026-07-29 evening, 17 rounds) is the current design; it supersedes the
open half of pass 1 and REVERSES one of pass 1's conclusions on the user's instruction. §§1-9 below
are pass 1 and are kept because their *deleted* work is what stops the next session re-deriving it —
but where §§1-9 and §10 disagree, **§10 wins**.

| | pass 1 | pass 2 |
|---|---|---|
| date | 2026-07-29 day | 2026-07-29 evening |
| rounds | 21 | 17 |
| converged? | **no** | **no** |
| stopped by | user: "do it whole, next session" | primary: the defect rate was itself the finding |
| sections | §§1-9 | §§10-16 |

**Verbatim pass-1 transcript:** `votv-chat-history-qf-thread-2026-07-29.md` (1,352 lines).
Pass 2 has no separate transcript file — its rounds are summarised round-by-round in §16.

---

## 1. What the user asked for (verbatim, in order)

> «Надо бы еще потом заняться чатом, потому что сейчас нету истории у него. А она должна быть когда
> игрок активирует чат на T. А пока он не активировал чат, никакой истории и тд нет, чат как обычно
> работает - сообщения появляются плавно и счезают плавно, а когда чат активирован то мы плавно
> проявляем всю или какую-то часть истории чата.»

Then, in answer to two product questions: **"1. The last N lines. 2. Close like minecraft."**
Then, to "should history look visually different from recent lines": **"B"** (no distinction).

Then the acceptance drill:

> "The smoke test should put 10 messages in chat, then wait for chat to settle 10 seconds, screenshot
> it from host and client, then another client joins, should see clear chat, then upon opening he
> should see its old messages"

Then:

> "Also when someone opens chat to read history new messages should ALWAYS arrive and move the chat.
> When chat DOESN'T MOVE from new messages is only when user decided to scroll history chat with the
> pg up/pg down buttons"

**USER DECISION 2026-07-29 (closing):** *"We will do it whole. But in the next session."* — i.e. the
host-owned canonical log is in scope; do not ship the local half alone.

## 2. The finding that reframes the whole feature

**`chat_feed` is a FEED, not a chat log.** Censused all 15 `chat_feed::Push` call sites: they include
`nick_color.cpp:92` *"Nickname color: applied (synced to other players)"*, `local_body.cpp:83`
*"Skin: X"*, `nameplate.cpp:202`, `player_handshake.cpp:469` *"Connecting to X's game..."*, plus two
`[1c-test]` debug lines. Typed chat, peer-action lines and the local player's own first-person UI
confirmations share one store, and **no field distinguishes them**.

Consequences:
- "Seed the joiner with the host's retained set" would send the host its **own UI notices** as "the
  lobby's history".
- The host's set is the host's **view** — subject to the host's own TTL, its own overflow, its own
  `Reset`. It is not the lobby's truth.
- **"The lobby's history" does not exist anywhere today.** It has to be CREATED: a host-owned
  canonical log (what was said, by whom, in what order, wire-sourced only), with the local feed
  becoming a VIEW of it. That is also the MTA shape — the server owns the log, clients render it.

This is why the user chose WHOLE.

## 3. Settled design — the local half (rounds 1-19, every piece measured)

| # | Decision | Why (measured) |
|---|---|---|
| S1 | Owning type with **four** transitions: `Promote()` pending->live, `Retire()` live->retained (**the only path OUT of the live set**), `Seed()` -> retained, `Reset()` drops all | A bare `std::deque` keeps `clear`/`erase`/`pop_back` compiling; a prose chokepoint is not a chokepoint. `Reset():254` was a THIRD drain the first shape missed. Promotion is a move INTO live, so it cannot route through `Retire()` |
| S2 | Invariant: *no mutation of the store is observable except through a republished snapshot* | One sentence, no "and". `Reset():256-258` writes `g_pub` directly and `Tick():234` returns before `Republish` — two paths mutating published state without bumping a generation |
| S3 | `seq` (monotone, ++ per push) is entry identity; `bornMs` is the FADE CLOCK only | `bornMs` has two assignments and `Tick():224` hoists `now` OUTSIDE the promotion loop -> identical stamps by construction. **Two existing comments assert uniqueness that does not hold: `chat_feed.h:32` and `chat_feed.cpp:149`** (the ALPHA-JUMP probe pair-matches on it) |
| S4 | Published snapshot = the UNION + `liveCount` + `gen`; render re-copies ONLY on a `gen` change | `Line` ~272 B, so 6 -> ~1.6 KB and 18 -> ~4.9 KB per `GetSnapshot`. `firstLiveSeq` was minted and DELETED the next round — nothing read it |
| S5 | Two alpha regimes: live = `bornMs` TTL, retained = 0. Final = `max(storeAlpha, revealRamp)` | An overflow victim popped at 200 ms sits at alpha 1.0 for ~9.3 s on the birth clock |
| S6 | **Layout membership is a PREDICATE** (alpha >= threshold), never a consequence of alpha | An invisible row still occupies height; an asymptoting ramp would keep 12 rows in `totalH` forever |
| S7 | Reveal ramp owns its own `steady_clock`; value = f(now, transitionStart, startValue, target); transitionStart set when the OBSERVED target differs from the STORED one; exact 0/1; symmetric; **duration 220 ms** | `io.DeltaTime` comes from `ImGui_ImplWin32_NewFrame()` INSIDE the render gate, so the first frame after a 60 s quiet lobby carries `DeltaTime ~ 60` and a dt ramp saturates in ONE frame. 220 ms = `chat_feed.cpp:26 kFadeInMs`, already tuned for this surface |
| S8 | `RevealActive()` is a predicate with **TWO consumers**: `imgui_overlay.cpp:458` (a frame exists) and `hud.cpp:416` `hud::IsActive()` (the chat is DRAWN via `:359`). **NEVER `AnyOpen()`, NEVER `CaptureActive()`** | `ChatOpen()` feeds `CaptureActive():135`; widening it eats a keystroke for the whole close duration after every send. The curtain comment at `:455-456` is the cited precedent for render-gate-only |
| S9 | `HasAny()` = **LIVE count > 0** | Retained lines riding the published count would pin the ImGui frame ON permanently after one message |
| S10 | Reveal = ONE walk, newest-first by `seq`, that BOTH selects and measures, stopping at `0.5*H - pad` or N entries | 12 entries != 12 rows: at `wrapW = 640*S` a 203-byte message wraps to ~3 rows, so 12 worst-case entries is ~36 rows / ~880 px against ~540 available — off the top of the screen |
| S11 | Passive path unchanged, INCLUDING activation | «пока он не активировал чат, никакой истории и тд нет» — a taller passive stack is history leaking into the passive path |
| S12 | Colour: **ZERO DIFF** | Six rounds and four reversals produced no change. See §5 |

## 4. Open — the wire half (rounds 20-21, NOT designed)

- **The canonical log** (§2). Host-owned, wire-sourced only, with a discriminator `chat_feed` does not
  have today.
- **Delivery** rides `ConnectReplayForSlot` / `QueueConnectBroadcastForSlot` — the shape every row in
  `COOP_EVENT_JOIN.md` §3.4 uses — NOT a new ReliableKind.
- **Refusal is PER LINE, never whole-blob**, or it becomes the fourth "one packet kills a joining
  client" in `docs/security/TRACKER.md`.
- **Ordering:** `seq` is LOCAL, so a message arriving during the join window takes a LOWER `seq` than a
  seed delivered after it — ten-minute-old history would sort NEWER. Seed entries need `seq` below every
  existing entry, and dedupe needs an **origin-scoped key** (sender slot + the sender's own counter)
  that does not exist on the wire today.
- **A host line still LIVE at snapshot time must land RETAINED on the joiner**, or his passive feed shows
  a conversation he was not in and the drill's step 4 fails by construction.
- **Which `Reset` orders against the seed is UNMEASURED.** `net_pump.cpp:200` is the LEAVE funnel
  (`FleeToMainMenu`); the join-time clear is `event_feed.cpp:121` via `OnSessionStart`, on BOTH roles.
  If the clear lands after the seed, the seed is erased.
- **Scroll (FOLLOW vs PINNED).** Measured and de-risked: `imgui_widgets.cpp:4597-4601` claims
  PgUp/PgDn **only `if (is_multiline)`**, and our bar is single-line `InputTextWithHint`
  (`chat_input.cpp:109`) — so those keys are FREE while chat holds focus. Undesigned: what returns to
  FOLLOW, the retention cap now that paging is the consumer, the walk's anchor, anchor invalidation when
  the pinned entry is evicted, whether PINNED survives close/reopen, and the `totalH` SNAP when closing
  from PINNED (a jump on the axis `hud.cpp:310-318` proves the last blink was actually on).
- **The retention cap is no longer 12.** "Last N lines" is the VIEWPORT; paging is exactly the consumer
  that makes extra retention live. Round 4's "cap == reveal, retaining more than we can show is dead
  memory" is RETRACTED.

## 5. What was designed and DELETED — so it is not re-derived

Six rounds (13-18) argued the nick colour of a retained line. **Every fix replaced the one before it,
and the net diff is ZERO.** Recorded per `[[lesson-oscillation-means-the-axis-is-not-what-decided-it]]`:

| round | proposal | why it died |
|---|---|---|
| 13 | Bake the resolved colour onto the `Entry` at PUSH | Freezes live lines against the F1 picker; makes `chat_feed` a second owner of the colour axis (`nick_color.h:3` claims sole ownership) |
| 14 | Capture at RETIREMENT instead | `player_handshake.cpp:275` -> `nick_color.cpp:130-134` zeroes `g_bySlot[slot]` at DISCONNECT, and arc A's teardown also fires on a REPLACEMENT — so authority dies up to 8 s BEFORE retirement, and the capture reads a cleared or replacement slot |
| 15 | Invert: `nick_color` PUSHES into the store on change | `g_bySlot` has FOUR writers and only one is `StoreForSlot` — `RequestLocal:87` writes directly, so the local F1 picker never pushes; and `ResetSlots:127` runs on the **bringup thread** while `g_lines` is documented game-thread-only (`chat_feed.cpp:43`) |
| 16 | Move `kSlotCols` into `nick_color`, make `PackedForSlot` always return a real colour | `IsCustom()` is a shared discriminator at THREE surfaces with THREE different defaults (chat `kSlotCols[slot%8]`, nameplate white, scoreboard `host=gold/client=soft-white`, `scoreboard.cpp:195-196`) — it would silently repaint plates and scoreboard, and delete the documented ini semantic at `nick_color.cpp:47-48` ("an explicitly EMPTY value = the per-surface defaults") |
| 17 | Dissolve: retained lines render in a NEUTRAL history colour | Reintroduces the crossing flip round 15 rejected; and `hud.cpp:378-380` colours the `action` predicate from a yellow literal that `nickCol` inherits, so a grey history block would hold full-saturation yellow. **Also: it is a product-feel call, not an engineering dissolution** |
| 18 | (question put to the user) | User chose **(b) no visual distinction** -> the entire axis collapses to a ZERO diff |

**Net: `slot` stays on the published `Line`, colour stays late-bound at `hud.cpp:384/388`, `nick_color`
is untouched.** A retained line on an emptied slot falls back to `kSlotCols[slot % 8]` — defined, not
undefined (`OnSlotDisconnected` zeroes the entry, `IsCustom` is then false).

**USER-ACCEPTED, FILED NOT FIXED:** a retained line's colour follows its slot's CURRENT occupant, so
after a slot recycle an old message renders in a new peer's colour. The cost was stated before the
choice.

## 6. Instruments — and the blind-instrument count

**Seven** distinct instrument-blindness findings in one pass. Every drill below carries its
precondition.

- **D0** store-event digest + baseline x2 + **three injected must-FAIL mutations proven RED**
  (`kMaxLines` 6->5, `kTtlMs` 11000->10000, skip-one-trim). Store-side and event-sampled: `FadeAlpha` is
  wall-clock and `totalH` is render-thread, so a frame-indexed digest does not reproduce byte-equal
  across two runs of the SAME bytes.
- **D0b** harness capability that HOLDS CHAT OPEN. `mp.py:1397` `_type_chat` presses T, types, presses
  Enter — it can never leave chat open, so D1-D3 cannot run today.
- **ALL DRILLS: `voice.enabled=false`.** `config_registry_rows.inc:113` defaults it **true**, so
  `voice_chat::Enabled()` makes `hud::IsActive()` unconditionally true and **every drill would pass
  while both gate defects shipped**.
- **D1** TTL crossing while open · **D2** the WHOLE close ramp (`totalH` monotone down to the passive
  baseline; "a frame after settle" is a one-frame window and would sample nothing if no frame is built)
  · **D3** reveal completes in 220 ms +/- one frame at ANY fps · **D4** join -> leave -> rejoin (two of
  three drains are CLIENT-only: `player_handshake.cpp:520` gates `PushDelayed` on
  `role == Client && slot == 0`; `net_pump.cpp:196-198` says in its own comment that its funnel is not
  the host path).
- **The user's acceptance drill** subsumes D1, part of D4, and adds the OVERFLOW drain plus EYES on the
  passive path. Two corrections to its literal text: step 4's "clear chat" is **not falsifiable** as
  written, because the join itself pushes `player_handshake.cpp:506` "<nick> joined the game" and `:523`
  `PushDelayed("Joined <host>'s game", 5000)`; and `_type_chat`'s measured cadence is **~3.3 s**, so ten
  messages take ~30 s and the earliest six EXPIRE WHILE STILL BEING SENT — at the screenshot ~3 lines
  are live and all 10 are retained. (Which is the right state to photograph; the drill is good, the
  original reasoning for it was wrong.)

## 7. Precondition already shipped

**`84e0a4e3` (W11) is a precondition of the seed, not a neighbour.** Chat's `OnReliable` had no
`FromUtf8Strict` until this session, so a host seeding its retained set would have re-emitted
ill-formed bytes to every future joiner, across sessions.

## 8. Filed, not bundled

- The instant overflow pop vs «сообщения ... исчезают плавно» — `TrimOverflow` pops at alpha 1.0, so a
  7th message makes the oldest line VANISH. The user's own sentence says it fades. Pre-existing.
- `chat_feed.cpp:149`'s ALPHA-JUMP probe pair-matches on `bornMs` and its comment asserts a uniqueness
  that does not hold — it can pair the WRONG two entries.
- The retained-line colour following the slot's current occupant (§5, user-accepted).
- The row-split cache: `forEachRow` runs `CalcWordWrapPositionA` twice per frame, but **nobody has
  measured that it costs anything at `kMaxLines=6`**. It left the build order for that reason.

---
---

# PASS 2 (2026-07-29 evening, 17 rounds, NOT converged) — the current design

## 10. Why there was a second pass, and what it changed about pass 1

Pass 1 ended on the REFRAME that `chat_feed` is a FEED and not a chat log, so the lobby's history had
to be CREATED rather than snapshotted. The user answered *"we will do it whole, but in the next
session."* Pass 2 designed the whole thing: the host-owned log, the wire, the store, the reveal and the
scroll.

It ran 17 rounds and **did not converge either.** Every round from 5 to 17 found a real defect. That
rate is itself the finding and is recorded in §16 — this feature touches a store with three threads,
two clocks, two tiers, a wire authority inversion and a two-stage render gate, and the defect density
reflects that rather than reflecting sloppiness in any one round.

**Pass 2 reverses pass 1 on exactly one thing**, on the user's explicit instruction (§11.2): §5's
colour axis, which pass 1 closed at a ZERO diff, is now a **freeze**.

## 11. USER DECISIONS (2026-07-29 evening) — three product forks, all answered

1. **Which lines are «история чата»?** → *"Делаем как ты порекомендовал."* So: **typed chat +
   peer-action lines + join/leave RETAIN**; the local player's own toasts (`Skin: X`,
   `Nickname color: applied`, `Nameplate: shown`, `Connecting to X's game...`, `Joined X's game`), the
   two `[1c-test]` debug lines, and the three `sleep_sync` status lines **do NOT**.
2. **The recycled-slot colour** → *"Freeze. Old chat history is essentially a frozen history."*
   **This REVERSES §5/§12's "ZERO DIFF" conclusion.** Cost stated before the choice and accepted: a
   peer who recolours mid-session no longer repaints their already-visible lines — which the NICKNAME
   never did either (`chat_sync.cpp:152` already bakes the nick TEXT at push).
3. **The round-trip echo** (host-authored chat means a client's own line appears after an RTT) →
   *"Не знаю. Делай как лучше."* → **primary's call: no optimistic echo.** Grounds: MTA
   (`CConsoleCommands.cpp:404-406` broadcasts a player's own line back with NO exclude argument —
   contrast `CGame.cpp:1426` which passes one) and Minecraft both render on the server's broadcast; and
   this pass demonstrated that every new state added to this store produced a defect. The
   `clientMsgId`-reconciliation alternative is recorded in §14 as a purely **ADDITIVE** escape hatch,
   deliberately NOT built.

## 12. THE DESIGN (pass-2 final form)

### 12a. The store — `chat_feed` grows a retained tier

| # | decision | why (measured) |
|---|---|---|
| S1 | Owning type, four transitions: `Promote()` pending→live, `Retire()` live→retained (**the only path OUT of live**), `Seed()` → retained bypassing live, `Reset()` | a bare `std::deque` keeps `clear`/`erase`/`pop_back` compiling; `Reset():254` was a third drain the first shape missed |
| S2 | Invariant: no mutation observable except through a republished snapshot | `Tick():234` returns before `Republish()`; `Reset():254-262` writes `g_pub` directly |
| S3 | **ONE totally-ordered sort key.** Wire row → `(lineSeq, 0)`. Local row → `(highestApplied, localTiebreak++)`. A local row authored **before any wire position exists** is provisionally based at 0 and **RE-BASED** at seed-apply | without it, `player_handshake.cpp:469` "Connecting to X's game..." renders as the joiner's OLDEST history |
| S3b | Dedup is a **contiguous applied RANGE `[lo, hi]`** — apply iff outside, then extend | a high-watermark alone cannot express a seed, which delivers OLDER rows. `lineSeq > highestApplied` would have **discarded the entire seed** |
| S13 | `TrimOverflow` becomes `Retire()`; both store exits unify | the 7th line pops instantly today *because it is destroyed*; under the retained tier it is not, so the fade becomes natural. Transient 7th row during the fade |
| S14 | The retention class is a **REQUIRED parameter with NO default**, on the ambiguous entry points only (bare `Push` + `PushDelayed` = **16 of the 21** `chat_feed::Push*` call sites) | no predicate over the data decides it — tested, 12/15 (§13.4). `PushChat`/`PushAction` fix their class at the entry point |
| S23 | The client's applied range lives **inside the owning type**, cleared by the SAME `Reset()` | a free-floating watermark is exactly how `chat_log::Reset()` itself was forgotten |
| S15 | While the reveal is active the **TTL clock does not advance** — a game-thread `g_suspendedMs` accumulator, **never a `bornMs` write**. Each entry snapshots `bornSuspendedMs` at BIRTH; effective age = `(now - e.bornMs) - (g_suspendedMs - e.bornSuspendedMs)` | chat open/close flips from THREE threads (`imgui_overlay.cpp:193/215/426`, `chat_input.cpp:122`, `net_pump.cpp:203`) while `g_lines` is game-thread-only. **The birth snapshot makes the uint64 wrap structurally impossible** — without it, a line born mid-reveal has `now-bornMs ≈ 0` against a large accumulator → wrap → `>= kTtlMs` → **popped on the very next Tick**. `PushDelayed`'s `dueMs` stays WALL-CLOCK |
| S15b | INVARIANT: **no clock field on a retained-at-birth row is ever an input** | |
| S17 | The ALPHA-JUMP probe re-keys onto the entry's identity and compares **STORE** alpha only | `Tick():219` hoists `now` outside the promotion loop → `bornMs` is NOT unique, so the probe can pair the WRONG two entries (§8 filed this; the re-key closes it for free) |

### 12b. The render half

| # | decision | why (measured) |
|---|---|---|
| S4 | Snapshot = live + (retained **only while the reveal is active**) + `liveCount` + `gen` | publishing 100 retained unconditionally = 27 KB copy + 10,000 nested probe iterations **at 60 Hz** |
| S4b | The retained region publishes **ONCE at reveal-open** and again only on change; the per-tick republish covers the LIVE region alone (≤6 rows) | a retained row's STORE alpha is a constant 0 and never needs recomputing. Per-tick cost is UNCHANGED from today |
| S5 | The ramp applies to **BOTH** tiers; the only per-tier difference is the **target on CLOSE** — retained → 0, live → its own TTL alpha | Minecraft shows everything opaque while chat is open; a dim row mid-stack is the seam "B" forbids. A live row at 0.3 rises to 1 over the ramp |
| S6 | Layout membership is a PREDICATE over the **DRAWN** alpha | an invisible row must not occupy height, and "invisible" means invisible ON SCREEN |
| S16 | **The ramp is RENDER-SIDE — a CONSTRAINT, not an implementation detail.** The published **STORE** alpha stays the TTL curve | otherwise S5's 0.3→1.0 rise on a same-`bornMs` tail entry is EXACTLY the ALPHA-JUMP "can't happen" condition (`chat_feed.cpp:146-157`), whose SILENCE `hud.cpp:313-317` cites as its evidence about the 2026-07-09 flicker |
| S7 | The reveal ramp owns its own `steady_clock`; `f(now, transitionStart, startValue, target)`; exact 0/1 endpoints; symmetric; **220 ms** (= `kFadeInMs`) | `io.DeltaTime` comes from `ImGui_ImplWin32_NewFrame()` INSIDE the render gate, so the first frame after a quiet lobby carries the whole gate-off gap and a dt ramp saturates in ONE frame |
| S18 | `RevealActive()` = **open OR still ramping** = `chat_input::IsOpen() \|\| (now - closeStartMs < kRevealMs)`; both terms atomics readable from any thread | as "chat open" alone the retained tier drops AT the instant of close and the fade-OUT draws **zero frames**. Precedent: `join_curtain`, `imgui_overlay.cpp:453-456` |
| S8 | `RevealActive()` has **THREE** consumers: the render gate (`imgui_overlay.cpp:457-458`), `hud::IsActive()` (`:359` draws the chat), and the **game-thread pump** (S15). NEVER `AnyOpen()`, NEVER `CaptureActive()` | `AnyOpen()` feeds `CaptureActive():135` — widening it eats a keystroke for the whole close duration after every send |
| S9 | `HasAny()` = **LIVE** count > 0 | rationale is SEMANTIC, not cost: its original "would pin the frame ON forever" justification is UNFALSIFIABLE, because `hud::IsActive()` is a 5-term disjunction ending in `voice_chat::Enabled()` whose registry default is **true** |
| S10 | The reveal is ONE walk, newest-first by the S3 key, that BOTH selects and measures, stopping at `0.5*H - pad` (+1 partially-clipped row) | 12 entries ≠ 12 rows: at `wrapW` a 203-byte message wraps to ~3 rows, so 12 worst-case entries is ~36 rows / ~880 px against ~540 available |
| S11 | The passive path is UNCHANGED **while chat is NOT activated** | «пока он не активировал чат, никакой истории и тд нет» |
| S12′ | **Colour is FROZEN into the row at BIRTH** (§11.2 — REVERSES pass 1's §5) — but the **receiver resolves it at APPLY**, not the host | `PackedForSlot` returns **0** for a peer with no custom colour, so freezing the raw packed value would keep `hud.cpp:388`'s `kSlotCols[l.slot % 8]` alive; and host-resolving would wire-freeze a render-side constant table. So `ChatSpeaker` carries the CUSTOM colour (0 = none) + slot, the receiver resolves `custom ? custom : kSlotCols[slot%8]` once at apply and stores the ARGB. **`Line.slot` then has ZERO consumers and comes OUT (RULE 2)**; `nickLen` SURVIVES (censused: `hud.cpp:382/391-392` need it as the split point INSIDE `text`) |
| S19 | `Seed()` **never** touches `chat_bubbles` — structural, not a flag test | `chat_sync.cpp:157` fires `OnChatLine` unconditionally; reusing the apply path would give a joiner N overhead world bubbles for conversations from before it existed (the widened-firing-set class) |

### 12c. The wire half — chat becomes HOST-AUTHORED

| # | decision | why (measured) |
|---|---|---|
| W1 | New module `coop/comms/chat_log.{h,cpp}` — the host-owned RECORD. `{u32 lineSeq; u16 speakerId; std::string msg}`, host-monotone `lineSeq` | `coop/comms/` already owns the chat concept (folder-per-domain holds) |
| W2 | **`ChatMessage` becomes client→host ONLY and LEAVES `IsClientRelayableReliableKind`** (RULE 2 — no parallel relay path). The host commits with a `lineSeq` and broadcasts an AUTHORED row to ALL clients **including the origin** | `session.cpp:459-481`: the relay fires on the **NET thread** at receive time, before the `reliableInbox_` drains on the game thread where a `lineSeq` would be assigned. **At relay time the seq does not exist yet.** So the commit and the broadcast must be ONE act at ONE authority on ONE thread. MTA precedent: `CConsoleCommands.cpp:404-406` |
| W3 | `ReliableKind::ChatLine = 119` = `{lineSeq, speakerId, slot, flags, len, text[203]}` = **212 B** (cap 228). `flags bit0 = seed`. `speakerId` is a **DISPLAY IDENTITY**; `slot` is a **WORLD-ENTITY HANDLE** driving `chat_bubbles` only | a composed line is up to **285 B** (`kNickMaxBytes` 80 + 2 + `text[203]`) against `kMaxReliablePayload` **228** — one datagram cannot carry it, and that is what forces the two-row shape |
| W4 | `ReliableKind::ChatSpeaker = 120` = `{speakerId, slot, nickLen, nick[80], packedCustom}`, precedes **every live line UNCONDITIONALLY**; the seed burst dedupes WITHIN the burst | a global "last-sent binding" memory silently strands a joiner who never saw an earlier binding. Chat is human-rate, so ~90 B per message buys statelessness: no per-recipient set, no versioning, no delivery bookkeeping. `speakerId` is a **per-burst index** — no minting policy, no eviction policy, nothing to bound |
| W5 | Delivery rides `subsystems::ConnectReplayForSlot(slot)` — the shape every `COOP_EVENT_JOIN.md` §3.4 row uses. **One reliable message PER LINE**, never a blob | a whole-blob would be the fourth "one packet kills a joining client" row in `docs/security/TRACKER.md` |
| W6 | Pin `ChatLine`/`ChatSpeaker` to `Lane::Normal` (`ChatMessage`'s lane, via `default:`) | GNS orders WITHIN a lane; speaker-before-line and seed-before-live both depend on it |
| W7 | **`ChatLine` goes INTO `IsPreWorldSendableKind`**, and a row applied before my own world-ready lands **RETAINED** | the pre-world gate is a SILENT DROP (`session.cpp:144/209`, `session_relay.cpp:87`), so a client typing during its own load window would see its line VANISH. Exact precedent: `session_lanes.h:253-265` added `SkinChange` for this class (audit 2026-07-02 HIGH), then `NameplateChange` + `NickColorChange`. Principle 8, killed at the gate |
| W8 | Seeded rows land **RETAINED**, never live | makes the drill's step 4 "should see clear chat" true |
| W9 | `chat_log::Reset()` beside `chat_feed::Reset()` at BOTH sites (`event_feed.cpp:121` in `OnSessionStart`, `net_pump.cpp:200` the leave funnel) | otherwise lobby A's conversation seeds lobby B's joiner after a stop-and-re-host in one process |
| W10 | **TWO histories, as a DECISION.** The log = the lobby's chat record (host-owned, wire-sourced, identical for everyone). The feed = each peer's VIEW history, including its own UI notices. Provable claim, stated narrowly: **the CHAT SUBSEQUENCE is identical across peers; the FULL order is per-peer** | a strict view-of-the-log would delete the UI notices from the feed — a behaviour change nobody asked for |

### 12d. Scroll — FOLLOW vs PINNED

| # | decision | why (measured) |
|---|---|---|
| C1 | Two modes: **FOLLOW** (default, arriving lines move the view) and **PINNED** | the user's rule verbatim |
| C2 | The anchor is `(sortKey, rowWithinEntry)` | an index shifts under every push and every trim |
| C3 | **Scroll in ROWS, not entries**: a page = `visibleRows - 1` rows, with one row of carried context | `hud.cpp:330` `rowH = px + S(2.f)` is CONSTANT for every wrapped row (applied per row at `:362`/`:392` via `forEachRow`), so row-paging is **exactly invertible and skip-free by construction**. Paging by a height-measured viewport over variable-height entries is NOT invertible — PgUp-then-PgDn would skip or repeat |
| C4 | PgUp from FOLLOW enters PINNED; PgDn past the newest row **or closing chat** leaves it. **PINNED never survives a close** | reopening always starts at the newest, matching "activate chat → see the recent history" |
| C5 | While PINNED, **retained→gone** is frozen (the RETENTION cap). At 2× the cap the CEILING wins: evict oldest, clamp the anchor, log | the anchor is a SORT KEY, so live→retained keeps it in the ordered set — only retained→gone removes a key. (`TrimOverflow` is **COUNT**-driven, so arriving messages DO retire live rows while you read, exactly as the user requires) |
| C6 | No `totalH` snap on close | |
| C7 | The reader is the **PUBLIC** `ImGui::IsKeyPressed(ImGuiKey_PageUp, /*repeat=*/true)` inside our own chat draw while chat is open — NOT the WndProc (which would steal PgUp from the game whenever chat is closed) | PgUp/PgDn are **free**, measured on three axes: `chat_input.cpp:94` sets `ImGuiWindowFlags_NoNav` and `NavUpdatePageUpPageDown` returns 0 on its FIRST line for `NoNavInputs`; `imgui_widgets.cpp:4597-4601` InputText claims them only `if (is_multiline)` and ours is single-line; `InputTextEx` never calls `SetActiveIdUsingAllKeyboardKeys` (that is `BoxSelectActivateDrag`, `imgui_widgets.cpp:7298`), and the public `IsKeyPressed` forwards `ImGuiKeyOwner_Any`, reading through the lock. **`imgui_internal.h:3202`'s flags-overload is NOT included by this tree and would not compile** |

### 12e. Already true — needs no code

**"2. Close like minecraft" ALREADY HOLDS.** `chat_input.cpp:122` calls `Close()` on submit today.

## 13. Designed and DELETED in pass 2 — so it is not re-derived

1. **S24 / S24′ — a transition at the TOP edge of the reveal block.** Round 15 found that with chat
   open and the block at budget, an arriving line pushes the topmost entry out of the SELECTION at
   full opacity — the same instant-vanish class S13 dissolved at the store edge. Two fixes were
   designed and **both died**:
   - *S24 (per-entry departure state in the render half)* — would have been the first stateful thing
     in the render half, with four unanswered clear-points (close, `Reset`, the PINNED freeze, the 2×
     eviction). That is R14's homeless-state defect one layer up.
   - *S24′ (a clip rect)* — reclassified the overflow as a VIEWPORT event and used
     `ImDrawList::PushClipRect` (`imgui.h:3071`, real, works on the background list). **The claim that
     the row "slides up and out" is FALSE**: `hud.cpp:369` recomputes `y = anchorBottomY - totalH`
     every frame, so a new line jumps the block by one `rowH` in ONE frame. Nothing slides. Making it
     slide needs an interpolated offset carried across frames — exactly what S24 was rejected for.
   - **RESOLUTION: accept the instant jump, add no state.** The user's «плавно» statements are about
     the PASSIVE path (S13 fixed that at the store). Minecraft — the named reference — does not
     animate chat scroll either. The "+1" partially-clipped row stays as a scroll affordance, not as
     a fix; if it looks wrong in hands-on it drops to a clean edge, one line either way.
2. **Optimistic local echo** (§11.3) — the `clientMsgId` reconciliation. Deliberately not built;
   additive if the latency is ever felt.
3. **"Retain iff the line names a peer"** as a data predicate — TESTED against the census, **12 of
   15**, failing on exactly the wrong rows: `player_handshake.cpp:469/523` NAME a peer (slot 0) while
   being purely the local player's own status, and narrowing to "a peer other than me" does not help
   because on a client the host IS another peer. **No predicate over the data decides the class** —
   hence S14's required parameter.
4. **A per-recipient "last sent binding" table** — replaced by W4's unconditional pairing.

## 14. Drills — they SPLIT, and the user's drill is their UNION

A local-only build has no seed, so the user's step 4 **cannot** pass — the joiner's history would be
genuinely empty and correctly so. Reporting a local-half shipment against the user's drill would be
the failure this project has already catalogued twice.

- **D-L (local half).** 10 messages → settle → screenshot host+client → **open and HOLD** chat by
  pressing the REAL `T` and stopping before Enter → assert the reveal walk's own emitted retained/live
  counts. **Plus: type a message while chat is HELD OPEN and assert it appears and the view moves** —
  without that case S15's underflow ships green. Injection **`VOTVCOOP_CHAT_NO_RETAIN=1`** (Retire
  drops instead of retains) must go **RED**.
- **D-W (wire half).** The joiner half. Injection **`VOTVCOOP_CHAT_SEED_SUPPRESS=1`** must go **RED**.
  **Plus: send chat DURING the joiner's load window, then assert the post-join reveal shows BOTH those
  lines and the older ones IN THE RIGHT ORDER** — the exact scenario whose two defects round 11 found.
- **ALL DRILLS: `voice.enabled=false` on every peer.** `config_registry_rows.inc:113` defaults it
  **true**, so `hud::IsActive()` is unconditionally true and the gate under test is unreachable.
- **The fixture presses the REAL bind and does not force a surface.** `mp.py:1209-1213` says so in
  writing about `VOTVCOOP_SCOREBOARD_OPEN`: *"the forced board counts as an interactive surface on the
  HOST and swallows the chat bind, and pressing the actual bind tests the actual bind."* What is
  missing today is only a `_type_chat` mode that **stops before Enter**.
- Env (not ini) is correct for both injections: `feedback_test_flags_in_ini_not_bats_or_env` §4 carves
  out autonomous-only scenario triggers. Honest residual: env-only triggers are policed by **nothing
  mechanical** — only by the discipline that an injection is not real until shown RED.

## 15. Residuals — deliberate, both

- **The pinned-frame cost is UNMEASURED** → the one remaining BUILD gate. Measure the frame-time delta
  with an idle overlay, before and after, on a peer with `voice.enabled=false`.
- **Retention 100 is CHOSEN, not derived.** It comes from the user's own reference ("close like
  minecraft") and is **bounded** by measurement — 27 KB store, ~21.5 KB seed, 1.2 % of the 8192
  reliable-inbox cap (`session.cpp:446`), and the seed rides world-ready so it is NOT in
  `save_transfer`'s window. Chosen by product reference, bounded by measurement.

## 16. What pass 2 caught — the case for why 17 rounds was not waste

Every round from 5 to 17 found something real. The six that would have shipped:

| round | defect |
|---|---|
| R9 | an unsigned underflow popping **every new message one tick after it arrives** — the user's one non-negotiable requirement |
| R11 | the dedup rule would have **discarded the entire seed**: joiner history silently empty, no error anywhere |
| R10 | two quantities named `alpha` — unfixed, **history would never have drawn at all** |
| R8 | a WndProc/render write into a store documented game-thread-only |
| R13 | two peers holding **permanently different names** for one message |
| R14 | the client's dedup range had no home and no reset — a rejoin's seed discarded |

**The pass's signature failure, five times, always caught by the question and never by the primary:
ONE NAME COVERING TWO QUANTITIES** — `seq` (local entry identity vs the host's wire order), `alpha`
(the store's TTL curve vs the drawn composition), `eviction` (live→retained vs retained→gone), "the
build gate" (the pinned frame vs the 60 Hz republish), and `slot` (display identity vs world-entity
handle). See `[[lesson-one-name-for-two-quantities]]`.

**Twice an answer was inherited across a change that invalidated it:** R2's gate widening
(`ChatLine` into `IsPreWorldSendableKind`) broke R1's ordering proof and nobody re-derived it until
R11; and R2's withdrawal of "the host names once" was correct under host-*relay* and wrong the moment
the design became host-*authored*, which took until R13. See
`[[lesson-a-reframe-invalidates-answers-that-cite-it]]`.

## 17. NEXT

**Build order: the LOCAL half first, then the WIRE half.** They are two shipments with two drills
(§14). The user's own drill goes green only when both are in.

Nothing is built. Three product forks are closed (§11); two residuals are open (§15).


---

## 18. AS-BUILT (2026-07-29) — what building changed

Two shipments, as planned. Both drills exist, both went green, both were shown RED under
injection. **Neither has been seen by a human.**

| | local half | wire half |
|---|---|---|
| commit | `8eea0af6` | `3729097e` |
| drill | `mp.py chathistory` | `mp.py chatseed` |
| injection | `VOTVCOOP_CHAT_NO_RETAIN=1` | `VOTVCOOP_CHAT_SEED_SUPPRESS=1` |
| clean | PASS 4/4 | PASS 4/4 |
| injected | RED on H1+H4 | RED on W1-W4 |

### 18.1 Three design claims CORRECTED by measurement

1. **W7 is REVERSED. `ChatLine` is deliberately NOT in `IsPreWorldSendableKind`.** W7
   put it there to stop a client's own line vanishing in its load window, and that
   reasoning was sound about a hole that does not exist: the pre-world gate is the
   HOST's send gate toward a joining slot, and a client's send toward slot 0 is never
   gated (`ClientConnectEdge` marks slot 0 ready immediately). Nothing a client types
   pre-world is lost — it reaches the host, is committed, and comes back in that
   client's own seed. Keeping ChatLine pre-world-sendable would instead have created
   the very interleave R11 warned about, because the seed would then land UNDER rows the
   client already applied. The dedup is still a contiguous RANGE, and a gap now LOGS —
   that log is the tripwire if this premise is ever changed back.
2. **`IsSlotReady` is TRANSPORT-level, not world-ready** (`session.h:397` — it reads
   `peerLanesConfigured_`; the world gate is `IsSlotWorldReady`). The design's broadcast
   loop would have sent live rows to a connected-but-unseeded slot. AS-BUILT the host
   holds its own **per-slot seed gate**: a slot receives live rows only after its seed
   has been sent, the gate is set before the empty-record early return (or an empty
   lobby's first conversation never starts), and it is cleared in `DisconnectSlot` so a
   recycled slot is re-seeded before it hears anything.
3. **W3's 212 bytes is 211.** `protocol.h` is inside `#pragma pack(push,1)`, so
   `ChatLinePayload` has no trailing padding. `ChatSpeakerPayload` is 88 as designed.

### 18.2 What the injected runs caught (both in the LOCAL half, both fixed)

- **The reveal marker sat below an early return.** With nothing to draw, `Draw()`
  returned before reaching it, so the marker did not fire until the next message
  arrived — and that message then fell OUTSIDE the window it was supposed to be inside.
  Moved above the early return and reduced to store facts.
  `[[lesson-an-instrument-whose-window-moves-with-what-it-measures]]`.
- **The pin was committed inside the key handler.** With fewer rows than the viewport
  holds, PgUp's overshoot clamps straight back to the newest line, so the pin was set
  and immediately cleared — and both edges were announced, so the drill read a
  never-moved view as a successful page. AS-BUILT the key press states an INTENT and the
  pin is decided once, after the clamps.

Both were found because the injection was run, not because the clean run was read. A
control that only ever runs green is a control that has never been tested.

### 18.3 Fixed in passing (pre-existing, found by self-audit)

The composer cut a 285-byte composed line to 256 with a raw `resize()` — on a BYTE,
splitting a multi-byte sequence onto the exact surface `84e0a4e3` hardened. It now cuts
on a character (`coop::text::CapUtf8Bytes`), once, at birth. The header comment claiming
the buffer was sized to fit (§"STILL FALSE IN CODE") is gone; so are the `bornMs`-is-
identity claims, the probe having been re-keyed onto the sort key.

### 18.4 Stale claims inside the pass-1/pass-2 sections (superseded, kept for the record)

Read §18 first; where the passes disagree with it, §18 wins. Three specific claims below are now FALSE
and are named here so a grep does not resurrect them:

- **§6 "D0b: `_type_chat` can never leave chat open, so D1-D3 cannot run today"** — no longer true.
  `mp.py:1421` `_type_chat(..., submit=False)` stops before Enter; both drills use it.
- **§3 S8 "TWO consumers" / §12b S8 "THREE consumers" of `RevealActive()`** — as built there are two
  CALL SITES and one is internal: `hud::IsActive()` (`hud.cpp:309`) and the store's own
  snapshot+suspension path. The render gate consumes it transitively through `hud::IsActive()`, which
  was already in the gate; no third call site was needed.
- **§4 / §12c W7 "`ChatLine` goes INTO `IsPreWorldSendableKind`"** — REVERSED, see §18.1.

### 18.5 Residuals

- **NOT hands-on.** Everything above is a fixture pressing real keys.
- **The "+1 clipped row"** survives as designed and reads correctly in the D-L shots
  (`research/chat_shots/`), but its visible height is `budget mod rowH` and can be near
  zero at some resolutions. §13.1's "one line either way" still applies.
- **The reveal block's height** was cut by a third on the user's note the same day
  ("the history box is too high") — `kRevealHeightFrac = 2/3` in `chat_view.cpp`.
- **The cap path is not drilled.** A >255-byte composed line is covered by
  `CapUtf8Bytes`' own selftest, not by D-L or D-W.
- **Both injections are env-only**, policed by nothing mechanical beyond having been
  shown RED.

---

# PASS 3 (2026-07-29 late) — the IMPLEMENTATION pass, 4 rounds, NOT converged

## 19. TWELVE DEFECTS IN THE SHIPPED CODE

**Why this pass existed.** Passes 1 and 2 never converged (21 and 17 rounds; every round from 5
onward found a real defect). The user asked, verbatim: *"Go next. Qf on chat never converged
remember, so whats our next step?"* The answer recorded at the end of pass 2 was: do NOT run a
third pass against the same brief — run `/qf` phase IMPLEMENTATION against the real diff. That is
this pass. The surface is `8eea0af6` + `3729097e` (2,045 insertions), not a description of them.

**It worked, and the reason it worked is itself the finding.** A design brief is the primary's own
prose about its own plan; it can be argued with indefinitely. Code can only be measured. Four
rounds over the diff produced twelve defects, every one citing a line — more than 38 rounds over
the briefs produced. **Round 4 was as productive as round 1, so this pass ALSO did not converge.**

### The twelve, ranked by what a human hits

| # | defect | evidence | severity |
|---|---|---|---|
| 5 | **THE ESC PATH — the user's own "close like minecraft".** `imgui_overlay.cpp:214` closes chat on ESC and FALLS THROUGH so the native pause menu opens; `:359` gates `hud::Render()` (hence `chat_view::Draw`) on `!PauseMenuOpen()`. So `Draw` stops running entirely: (a) the 220 ms fade draws ZERO frames — the `RevealActive()` clause added to `hud::IsActive()` at `hud.cpp:304-309` exists precisely to keep that fade alive and is defeated one level up; (b) `SetPinned(false)` (`chat_view.cpp:127`) never runs, so `SetRetentionFrozen(true)` stays latched for the whole pause; (c) `g_revealValue`/`g_revealTo` freeze at 1.0 — either `Draw` resumes and flashes the live feed to full alpha for 220 ms minutes later, or `IsActive()` stays false and the NEXT open takes `Ramp`'s `target == g_revealTo` branch with a huge elapsed and has **no fade-in at all**. Exactly one fires; both are visible. | code | **CRITICAL** |
| 8 | **Key order non-monotone on the joiner path, RACILY.** `AnnounceJoinerOnce` (`player_handshake.cpp:513`) pushes `Keep::History` at PUPPET SPAWN, racing the seed. If it wins, `g_wireBase` is still 0 so it keys at `(0<<32)|n` while seeded rows key at `>= 1<<32`; on retirement `Retire`'s `push_back` (`chat_feed.cpp:261`) appends it BEHIND them and `retained_` is non-monotone — making `chat_feed.h:106-108`'s documented *"ascending by key"* invariant FALSE, which `chat_view.cpp:233`'s `key >= g_anchorKey` fallback relies on. Display is ACCIDENTALLY correct (`Republish` emits retained before live). **And `chat_feed.cpp:129-133` names `"Connecting to <host>'s game..."` as the case `g_wireBase` prevents — that line is `Keep::Transient` and can NEVER reach the retained tier, while the line that does reach it is the one `g_wireBase` cannot help.** | code | HIGH |
| 1 | **Three constants disagree.** `CapRetained` allows `kMaxRetained*2` = 200 while pinned (`chat_feed.cpp:275`); `Republish` publishes at most `kMaxRetained` = 100 walking from the FRONT (`:328`); `Snapshot::lines[]` is physically sized `kMaxLines + kMaxRetained` = 106 and cannot hold 200. TTL suspension blocks the EXPIRY exit but NOT the OVERFLOW one (`Store::Birth` still runs `while (live_.size() > kMaxLines) Retire`), so in a 100-row lobby with a reader paged back the 7th new message pushes a row to retained index >=100 where it is out of both windows: **it disappears from the screen.** | code | HIGH |
| 4 | **The GAP tripwire performs the corruption it warns about.** `OnChatLine:311-321` logs the applied-range GAP and then falls through to `ApplyRow` and widens `[lo,hi]` ACROSS it — the exact thing its own comment (`:317`) says *"would silently swallow the rows inside it"*. | code | HIGH |
| 10 | **Per-frame wrap cost unmeasured at scale.** `forEachRow` re-runs `strlen` + `CalcWordWrapPositionA` over every published entry every frame with no memo keyed on `(gen, wrapW, px)`; the build loop walks ~106 entries producing ~212 rows in order to DRAW ~18. ~27K char-ops/frame plus `CalcTextSizeA` per drawn segment, landing exactly while the user holds the history open. The 117-119 fps cited in §18 was measured over **25** rows, not 106 — a quarter scale. | inferred; the fps number is measured but at the wrong scale | HIGH |
| 2+6 | **Send result discarded, and it is ONE invariant with #4.** `SendLine`/`SendSpeaker` (`chat_sync.cpp:138,150`) drop `SendReliableToSlot`'s bool while `QueueSend` checks it, and `sent` (`:386`) counts loop iterations — so `"connect-seed -- sent %d history line(s)"` prints 100 even if every send failed. Three of `SendReliableToSlot`'s false-returns are SILENT (`session.cpp:129` slot range, `:145` pre-world gate, `:146` `hConn==0`). **The premise WAS measured this pass and currently holds:** `peerLanesConfigured_` clears only at connection close (`session_status.cpp:316`) and in `KickClaimed` (`:420`); `g_seeded` clears only via `OnSlotDisconnected`; `hConn==0` and the world-ready gate both coincide with disconnect; 211 < `kMaxReliablePayload` 228 so the cap cannot reject; only `AllocateMessage`-null remains and it logs. A silent hole is UNREACHABLE today. The invariant to hold is *"every authored line is queued to every seeded slot, or that slot stops being seeded"* — and the fix must close both ends. | measured | MEDIUM |
| 12 | **Three dev flags bypass the config ratchet.** `VOTVCOOP_CHAT_NO_RETAIN` / `_SEED_SUPPRESS` / `_CORRUPT_WIRE` are bare `ReadEnv("literal")` with no registry row, so `registry_gate.ps1` cannot see them, `config_review` cannot report them, and `multivoid.ini.example` never lists them. OPUS section 3: *"ini [dev] for flags; never bats/env"*. These three gate the release drills. Latching is inconsistent for no reason: `NoRetain()` is a function-local static, the other two call `ReadEnv` (a `GetEnvironmentVariableW` syscall) per sent line and per join. | measured | MEDIUM |
| 7 | **`chat_input` writes render-thread state from the WndProc.** `chat_input.cpp:62` says Open/Close are reached from THREE threads and that the store *"is told through SetChatOpen, which writes atomics only"* — then `Open()`/`Close()` ALSO write `g_buf[0]` and `g_histPos`, both declared render-thread-only, while `InputTextWithHint` may be mid-frame. Pre-existing (v60-era), low severity, and the comment actively misleads. **This is `[[lesson-census-the-direction-not-only-the-operation]]` failing one level up: the census counted what those functions tell the STORE and never censused what else they WRITE.** | measured | LOW |
| 3 | **Host reveal != joiner reveal, permanently.** 5 local `Keep::History` sites + `PushAction` retain join/leave/skin/turned-away lines while `chat_log` records typed chat ONLY. **RESOLVED BY THE USER — see section 20.** | measured | user's call |
| 11 | **`nt == 0` drops all older history.** `chat_view.cpp:210`'s fused `if (nt == 0 or nt > first) break;` has two conditions wanting different verbs. Unreachable today (zero rows needs empty text; `ApplyRow` composes `nick + ": " + text` and every local push has a literal), but a future empty line would terminate the build loop and silently drop every older row. `continue` and `break`. | measured | LATENT |
| 9 | **An `--inject` control proves ONE BIT, not coverage.** NONE of the twelve would turn `chathistory` or `chatseed` red: ESC is avoided by construction, 24 and 10 messages never approach 100/200, join lines are not wire rows so W2 stays contiguous. Section 18 reported *"both PASS 4/4, both shown RED under injection"* as evidence of instrument quality; it is evidence of non-blindness on **two specific axes**. | measured | INSTRUMENT |

### Also measured this pass (not defects)

- **`ConnectReplayForSlot` RE-FIRES mid-session** on a world-change re-announce (cave travel);
  `meadow_db_sync.cpp:783` carries `g_seededOnce[]` for exactly this. Chat has no latch, so the
  whole record is re-sent (~21 KB) and silently deduped by the client's range. **Bandwidth, not
  corruption** — `chat_feed::Reset()`/`chat_sync::Reset()` are SESSION-scoped only
  (`net_pump.cpp:201-202`, `event_feed.cpp:123-124`), never per world change.
- **Guard asymmetry:** `ChatLine` has a `senderPeerSlot != 0` guard
  (`event_dispatch_world.cpp:256`); **`ChatSpeaker` has NONE** (`:236-247`). Latent only —
  `ChatSpeaker` is not relay-whitelisted so a client's can only reach the HOST, which never
  consumes `g_speakers` because it never accepts a `ChatLine`. One relay-whitelist edit from real.
- **A client typing during its OWN load window is handled correctly** — the pre-world gate is
  host->joiner, client->host is ungated, and the joiner's seed contains the line.
- **The speaker lane IS ordered.** `LaneForKind` sends both `ChatSpeaker` and `ChatLine` through
  `default: Lane::Normal`; GNS guarantees in-order reliable delivery within a lane. The seed emits
  `Speaker(id)` immediately before the first line using it, and `nBound = 0` on overflow only
  re-binds forward, so no line can reference a binding it has not seen.
- **Overlapping joiners do NOT open a gap.** Both the seed loop and `AuthorAndBroadcast` run on
  the game thread and `g_seeded[M] = true` is set BEFORE the `ForEach`, so a join line authored
  during M's seed is either already in the record the loop reads or arrives live after — never
  neither. Principle 8 holds here without new work.

## 20. USER REFRAME (2026-07-29) — the lobby record owns EVERY history line

**Verbatim:** *"Playes should get all history, including player messages and chat event feed
messages. When i hands on this, I want it to be 100 percent built properly."*

This overrules the primary's position on section 19 #3 (which held that a joiner SHOULD NOT have
events it missed replayed). It is also the smaller architecture, and it DISSOLVES two defects
instead of patching them.

### What it dissolves

Today `Keep::History` means *"retain this locally"* and each peer composes its own event lines
from its own wire events. If the host's `chat_log` owns them instead, then **every retained row is
a wire row with a real `lineSeq`**:

- `retained_` becomes monotone BY CONSTRUCTION -> **#8 stops existing** rather than being fixed,
  and `chat_feed.h:106-108`'s documented invariant becomes TRUE.
- Host and joiner cannot disagree -> **#3 stops existing**.
- `Keep` then has ONE remaining value, so the enum is DELETED (RULE 2). `Push()` becomes
  local-transient-only and `PushWireChat` becomes the ONLY path into the retained tier.
- `g_wireBase`/`NextKey` shrink to ordering transient lines, which never retain.

### The shape (DESIGN — one round run, NOT converged, NOT built)

- Host-only `AnnounceLobbyEvent(...)` appends to `chat_log`, broadcasts `ChatSpeaker` +
  `ChatLine` with a new `Event` flag, and applies locally.
- `ChatLinePayload.flags` gains `Event` (compose `nick + text`, not `nick + ": " + text`) and
  `Action` (yellow predicate). **No size change** — `flags` already exists — but the semantics
  change, so **proto 133 -> 134**.
- The host-only gate lives INSIDE the announce API, not at the call sites.

### Lobby-wide lines that move to host authorship

| site | line |
|---|---|
| `event_feed.cpp:78` | `<nick> left the game` |
| `player_handshake.cpp:513` | `<nick> joined the game` (fires at PUPPET SPAWN) |
| `player_handshake_prefs.cpp:112` **and** `:134` | `<nick> changed skin to X` — host and client branches COLLAPSE to one (RULE 2 win) |
| `player_handshake_version.cpp:97` | `<nick> was turned away: <reason>` |
| `peer_action_feed.cpp:62` | `<nick> deleted an email: X` (yellow) |

Staying LOCAL + transient: `Connecting to <host>'s game...`, the device-busy notices
(`AnnounceDirect`), sleep / nameplate / save-transfer / local_body status.

### What the ONE design round already moved — and why nothing was written

The design changed shape TWICE in a single round, which is why no code exists:

1. **"Five sites" was a site list, not an invariant.** `peer_action_feed::Announce` has **SEVEN**
   call sites in four subsystems, in two classes: actor-local branches (`email_sync.cpp:393` uses
   `localSlot`) and receiver branches (`:504`, `signal_catch_sync.cpp:363/372` use `senderSlot`).
   Under host authorship a client's actor-local call must go silent or it double-renders against
   the host's authored row (the RULE 2 two-implementations trap). **The invariant is that
   `Announce` ITSELF becomes the seam** — no-op on a client, author+broadcast on the host — which
   makes every caller correct untouched.
2. **Chat bubbles would speak the event lines.** `ApplyRow` fires `chat_bubbles::OnChatLine` for
   every non-`seeded` row (`chat_sync.cpp:120`), so an authored event row would put a speech
   bubble reading *"was turned away: Game version mismatch"* over whoever occupies that recycled
   slot. Bubbles must gate on `!event` STRUCTURALLY, the same way seeded rows already do.

Also settled in that round: **`ui.chat.peer_actions` stays a RECEIVER decision applied at apply
time**, never at author time — one player's cosmetic toggle must not edit the lobby's permanent
record — and a seeded action row for a toggled-off peer takes the same path and is simply not
rendered.

### THE GATING MEASUREMENT — not done

**Census all seven `peer_action_feed::Announce` sites** (actor-local vs receiver) and prove the
HOST reaches a branch for every client-initiated action. `signal_catch_sync.cpp:269` is
unclassified. If any action has no host-side receiver branch it goes **silent for everyone**
instead of costing a round trip — strictly worse than today. This is read-only and it gates the
build.

### The risk named and NOT yet verified

A client prints `"X left the game"` from its own disconnect detection today. Under host authorship
it waits for the host's authored row — fine for peer departures, but **nobody authors the HOST's
own departure**. The belief is that the flee/teardown path already suppresses those lines
(`net_pump.cpp:185-215` neutralizes the edge detectors; `g_suppressLeaveLines`). **VERIFY, do not
assume** — this inversion is exactly what creates that class of hole.

## 21. NEXT (supersedes section 17)

1. **The Announce census** (section 20) — read-only, gates everything.
2. **Finish the design pass on the reframe** with the census in hand. It has had ONE round and
   changed shape twice in it; that is not a converged design.
3. **Build as two arcs:** the record generalization (proto 134), then the eleven remaining
   section-19 fixes with **#5 (ESC) FIRST** — it is the one certain to fire in the first thirty
   seconds of a hands-on.
4. Only then hands-on. The user's bar, verbatim: *"100 percent built properly."*
