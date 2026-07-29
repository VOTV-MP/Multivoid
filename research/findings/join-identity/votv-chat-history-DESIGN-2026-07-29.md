# Chat history — design of record (21-round `/qf`, NOT converged, NOT built)

**Status: DESIGN. Nothing built. The pass did NOT converge** — rounds 11-21 were each given explicit
permission to converge and each found a real defect. Stopped by the user's decision to do the feature
WHOLE (host-owned canonical log) in a later session.

**Verbatim `/qf` transcript:** `votv-chat-history-qf-thread-2026-07-29.md` (1,352 lines, all 21 rounds,
every question and every answer). Read it before re-deriving anything here — its value is as much the
**reversals** as the conclusions.

**HEAD at close:** `84e0a4e3`. DLL `1180972ccefc365803756cef68681fe0`, proto **132 unchanged**.

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
