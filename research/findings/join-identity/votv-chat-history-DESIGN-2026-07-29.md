# Chat history — design of record (two `/qf` passes, 21 + 17 rounds, NEITHER converged, NOT built)

**Status: DESIGN. Nothing built as of HEAD `bafa8e42`.** DLL `1180972ccefc365803756cef68681fe0`,
proto **132 unchanged**.

**READ §10 FIRST.** Pass 2 (2026-07-29 evening, 17 rounds) is the current design; it supersedes the
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
