# Nickname arbitration + roster identity/ID — DESIGN (2026-07-27)

**STATUS (2026-07-28, per arc — reconciled against the code, not against labels):**
- **ARC A — BUILT, DRILLED, AUDITED. NOT hands-on.** Proto 130, commits `72805d96`..`89620d59`.
  The drills ran on DLL `c4d1d8d4bf1dcd3e`; the player-list fixes of `89620d59` landed after them, so
  what is DEPLOYED now is `51ec0c60cc698e68` (UI-only delta -- no sync path touched, drills unaffected). Four drills PASS (departure / replacement-with-loss-injection /
  successor-ban / idempotency-by-count), two audits at 0 CRITICAL. Evidence and the corrections the
  build measured against this design are in §3 AS-BUILT; the per-drill RUN/OPEN status is in
  §3 "Arc A drills". **No human has touched it** — every claim is autonomous.
- **ARC B (nickname arbitration) — BUILT + DRILLED 2026-07-28, commit `8592fb5e`, proto 132, DLL
  `BC0285ADAECBCB12`. NOT hands-on.** `coop/player/nickname_arbiter.{h,cpp}`; the host arbitrates at
  the Join seam (`player_handshake.cpp`) and the assignment rides the existing RosterRow FIXED PREFIX
  to the named peer, which adopts it into `g_localNick`. Evidence: `nickname-arbiter selftest
  PASS (14/14)` on every peer, a 4-peer drill where all four ask for "Pelmentor" and the boards
  PHOTOGRAPH `Pelmentor / Pelmentor2 / Pelmentor3 / Pelmentor4`
  (`research/nickarb_shots/`), the four `multivoid.ini` files reading those names back, smoke4 PASS.
  See §9c for the as-built and the three defects the drill found.
- **ARC D1 (the codec) — BUILT 2026-07-28 in TWO parts: `9ae83454` (entry) and
  `2c3975d2`+`f19eedac` (egress). The first part alone was INCOMPLETE and its evidence
  OVERSTATED — see §9c.6.** `coop/text/utf8_codec.{h,cpp}` is the one owner of BOTH directions:
  decode at entry and `CopyUtf8ToBuffer` out into every fixed `char[]`. Evidence on the current
  bytes: `utf8-codec selftest PASS (27/27)` and `nickname-arbiter PASS (14/14)` on every peer,
  `nick_gate: PASS` with its three narrow detectors injection-proven, smoke4 PASS, and a 4-peer
  drill photographing `Пельменьмень / 2 / 3 / 4` — **13 characters, 26 bytes, past the 23-byte
  cliff that blanked the row** — plus an in-world nameplate `Пельменьмень4 (<1ms)`.
  **Cyrillic needs no new font bytes** (all seven embedded families were cmap-measured to cover
  U+0400-04FF).
- **ARC D2 (the repertoire — CJK + emoji donors) — DESIGN ONLY, NOT BUILT.** Code-verified
  2026-07-28: no donor font exists under `src/votv-coop/assets/fonts/`, and
  `ImGuiFreeTypeBuilderFlags_LoadColor` appears nowhere in the tree. CJK and emoji therefore render as
  the fallback glyph. The five §9b.7 measurements that gated it are all RUN
  (`votv-arc-d-gate-measurements-2026-07-28.md`).
- **ARC C (per-slot receive gate) — DESIGN ONLY, NOT BUILT** (§8; unchanged this session).

**§9b.8's "B and D are ONE delivery" was OVERTAKEN BY THE BUILD.** A `/qf` round held that ordering
against the ask and it did not survive: arc B alone over today's alphabet IS the literal ask, and what
D changes in B is the fold key plus the cap unit — one function and one constant, not the authority
structure. B shipped first and D1 followed the same day; nothing had to be redone. The residual cost
the reframe predicted is real and cosmetic: names arbitrated before D1 landed were folded over an
alphabet that silently stripped non-ASCII, so a Cyrillic-named player could be renamed once more when
D1 arrived. Nothing persists across it (bans key on IP, seen-players on GUID).

Arcs A/B/C: 42-round `/qf`, the critic returned "that holds" at R42. Arc D: 19 rounds, NOT
converged, stopped deliberately once the questions moved onto the drill (§9b). Round-by-round log:
`<scratchpad>/qf_thread.md` (session-local; the durable content is this document).

**The user's ask (2026-07-26 directive, verbatim intent):** duplicate nicknames must RESOLVE the
way other multiplayer games do — a joiner whose name is taken gets a forced numeric suffix, so
host Pelmentor + three client Pelmentors read as "Pelmentor (host), Pelmentor2, Pelmentor3,
Pelmentor4"; it must be ROBUST; it is needed for future `/commands` that target players by NAME
and by ID; and the TAB player list must show IDs. The fix itself is "в будущем" — this pass owed
only the design.

**The four product questions are ANSWERED (user, 2026-07-27) — see §9, now DECISIONS.** Two answers
changed the scope: the TAB becomes the SA-MP-shaped full-information surface (delta measured: one
column, the ID) and international text became mandatory, which opened **ARC D**. Arc D then ran its
own 19-round `/qf` and the scope widened twice more (CJK, then colour emoji) — its design of record is
**§9b**; §9a is retained only as the fact base that pass started from. **Arc D and arc B are ONE
delivery** (§9b.8): the dependency runs both ways. Arc A is independent and can land first.

---

## 1. What this turned out to be

The ask is two features, and the second one is blocked by a defect nobody had noticed:

- **`roster.cpp:85-86` skips rows whose slot is not `IsSlotConnected`, and a client only ever fills
  `peerConns_[0]` (`session.h:374-377`). So on a client, TAB today lists ONLY that client and the
  host.** In a four-player lobby a client sees two rows. There is also no `PlayerLeft` wire kind
  anywhere in the tree (grep is empty), so absence has no representation a client could learn from.

"TAB must show IDs" presumes TAB lists everyone. So the work splits into two sequenced arcs, and
the first one fixes a pre-existing bug rather than adding the requested feature.

A third arc was discovered and filed but is NOT part of this work (§8).

---

## 2. The spine — the occupancy TOKEN

Slots are recycled (`FindFreePeerSlotForClient` returns the lowest free slot,
`session_status.cpp:87-93`), so "slot 2" is not a person. Everything below exists to make a slot's
CURRENT occupant unambiguous without anyone having to OBSERVE a departure.

**T1 — wire row identity is `(slot, playerNo)`.** Any change of `playerNo` on a slot is a
REPLACEMENT: the receiver performs death-of-old + birth-of-new atomically, draining the old eid
association BEFORE installing the new one (otherwise it is the one-actor-two-eids shape v122
closed) and resetting latches. `playerNo == 0` means the slot is empty.

**T2 — a receiver never observes absence, only the current token.** One mechanism therefore heals
both a lost departure and a missed replacement. This matters because a recycled slot can go X → Y
with no zero in between, and a polled boolean edge can miss the transition entirely.

**T3 — two structures, one owner each.**
- The **GENERATION** is net-layer-owned: an atomic per-slot value, minted wherever
  `peerConns_[slot]` is written, cleared as the LAST release-ordered write of the close path AFTER
  the `reliableInbox_` erase (the erase is under a different mutex — `session_status.cpp:303-326`),
  and zeroed in `Session::Start` beside the existing `expectedEpoch_` clear.
- The **LEDGER** is game-thread-owned with a single GT writer, and only READS the generation.
- **Gate rule:** every `peerConns_` store carries EITHER a mint OR an explicit "no mint needed"
  annotation with a reason. There are five stores today (client `session_start.cpp:198`/`:317`;
  host `session_status.cpp:187`/`:232`; the clear at `:304`); the two client stores carry a reason,
  because a client never consumes a generation. A sixth site breaks the build until someone decides.

**Why not the existing `ownEpoch_`:** it is minted in the SENDER's process
(`session_start.cpp:101-110`) and rides the header, so it is a peer-DECLARED value. A rejoining
peer that re-declares its prior epoch would show the host no token change — no replacement, no
teardown, the previous occupant's person-state surviving under a new person. Authority must never
rest on a peer-declared value.

**T3b — who checks, and where.** A per-tick GT reconcile ON THE HOST conforms ledger rows to the
generation array: zero empties a row, a change performs the replacement. This covers
departure-without-successor, fast replacement, and a missed edge in one motion. It SKIPS slot 0
(host-self is seeded, not connection-derived — `peerConns_[0]` is never stored on a host). The
CLIENT ledger is entirely wire-driven: a client's slots 1-3 have permanently zero generations, so a
reconcile there would erase exactly the rows the wire just delivered. The reconcile runs only while
`running()`, respects `SuppressPeerLeaveEdges` (`event_feed.cpp:71-79`), and the ledger CLEAR
happens at session STOP, with `Reset()` at start kept as an idempotent belt — otherwise the window
between stop and the next start fans out four false departures with toasts into the menu (the
2026-07-15/16 class).

*"Skip slot 0" is about the host's LOCAL reconcile. The host's OUTBOUND pulse enumerates ALL slots
INCLUDING 0 — the host describes itself.*

**T4 — three distinct moments.** The generation is minted at ACCEPT (internal, unreferenced); the
host's LOCAL row is born at READY (never at accept: birth-at-accept plus teardown-on-row-transition
would resurrect the false "left the game" the 2026-07-16 fix removed); PUBLICATION to clients waits
for Join-received — after the version gate (which can Kick, `player_handshake_version.cpp:120`) and
after arbitration — so no born/died pair is ever emitted for someone who never entered.
On a CLIENT, slot 0's ROW is born from Join/RosterRow, while the host's EID is owned by
`AssignPeerSlot` (`player_handshake.cpp:831-845`); the Join-side mirror install for slot 0 is
retired in arc A, separating identity from eid (they have two authors today, idempotently).

**T4b — park gate.** While `LocalPeerId()` is `kPeerIdUnknown`, inbound roster rows are PARKED (at
most four) and applied right after the stamp, so a row about our own slot can never be misapplied
as a remote row. Ordering (same lane; `AssignPeerSlot` sent first) is real but NOT load-bearing.

**T5 — the teardown trigger is the LEDGER ROW TRANSITION**, identically on every peer.
`OnSlotReplaced` passes a SNAPSHOT `(slot, outgoing row, incoming row)`, so subscribers never race
the clear — today the "X left the game" toast at `event_feed.cpp:129` prints BEFORE
`OnSlotDisconnected` at `:130` and `player_handshake.cpp:352-355` documents the deliberately late
nick clear that makes it work. At session end ONE owner (the ledger clear) walks the occupied rows
and emits one transition each.
**Firing-set expansion:** on a client the fanout will now run for slots 1-3, where no subscriber
body has ever run. Substantively this fixes latent leaks (a client currently keeps a departed
peer's nameplate pref, nick colour, chat bubble and hand mirror forever), but every existing
subscriber body must be READ, not assumed benign — named work item in arc A.

**T6 — one subscriber list.** The DEFENCE is the TYPE: `PerSlotState<T>` registers itself in its
constructor, so anything declared through it is covered by construction. A CI gate scanning raw
`[kMaxPeers]` arrays across ALL of `coop/` (explicit allowlist for genuinely non-person arrays) is
the second layer. The residual — person-state that is neither — is NAMED, not called coverage; if
this class recurs a third time the answer is READ-SIDE gating (arc C), not a longer checklist.
`g_guidBySlot` / `g_skinBySlot` / `g_joinSentBySlot` / `g_joinAnnouncedBySlot` become FIELDS OF THE
LEDGER ROW (four arrays deleted, RULE 2), so replacement clears them by construction. The allowlist
grants NO exemption to the file being rewritten.

**T7 — wire content.** Only `playerNo` travels (`uint16`); the generation never leaves the host.
Minting skips 0 and any value currently held by a LIVE row, so a post-wrap number cannot collide
with a live row and "a change is a replacement" holds for any session length (65,535 joins is ~45
days at one per minute — reachable for the future dedicated server). PER-FIELD IGNORE: for a row
describing slot 0 or the receiver's own slot, arc A ignores the NICK field (`playerNo` is ALWAYS
applied — it is the host-issued ID, unlearnable otherwise); rows about OTHER client slots DO apply
the nick, since a client never sees their Join.

**T8 — lifetime.** The ledger clears at session stop (belt at start); the high-water map and the
`playerNo` counter clear in `player_handshake::Reset()` (`:254`, called from
`event_feed::OnSessionStart` `:88`); the generation array is cleared by the net layer only —
a GT write into it would break T3's one-writer claim.

**T9 — RULE 2.** `g_remoteNickBySlot` is DELETED in arc A; the nick becomes a ledger-row field and
`NicknameForSlot` becomes a thin ledger read with an UNCHANGED signature, so its ~14 call sites
across 9 files keep compiling untouched. **Authorship handoff:** arc A = the Join path authors slot
0's nick while RosterRow ignores it; arc B = the row becomes the author and the Join-side nick write
is retired in the same commit. One writer at all times.

**T10 — three narration facts, not one axis.** TAB (the ledger) is the PRESENCE authority; toasts
are NARRATION with their own measured seams: "connecting" at Join, "joined the game" at the
puppet-appearance seam (2026-07-03 measured a `ClientWorldReady`+5s announce running ~6 s ahead of
the puppet), "left" on the row transition. A peer whose row exists but whose puppet never spawned is
now VISIBLE in TAB, where today it appears nowhere.

**T11 — destructive actions validate WHERE THEY READ, atomically.** Measured pre-existing defect:
`BanSlot(g_banSlot, reason)` (`admin_panel.cpp:209`) uses a slot captured when the modal OPENED,
across an arbitrary typing delay, while slots recycle — a permanent IP ban can land on the
SUCCESSOR. `KickSlot` (`scoreboard.cpp:159`, `admin_panel.cpp:97`) and the second confirm modal
(`scoreboard.cpp:285`, its own `g_banConfirmSlot`) share the shape; `moderation::` has eight
external call sites, six slot-addressed.
The chain: the modal stashes `(slot, playerNo)` from the POD row; `BanPlayer(slot, playerNo,
reason)` posts to GT; the task finds the ledger row by `playerNo` and takes THE GENERATION THAT ROW
WAS BORN FROM; `Session` compares it against the live generation ATOMICALLY with the address
resolve, failing otherwise. This works precisely because a value from the LAGGING MIRROR is checked
against the LIVE AUTHORITY: if the ledger is stale it carries the predecessor's generation and the
action aborts. Comparing `playerNo` against `playerNo` inside one mirror would push toward
fail-OPEN and merely narrow the window to one tick. The token is in the API SIGNATURE, so a
tokenless call does not compile — the defence cannot be forgotten at a call site. Slot 0 needs no
token (`ValidClientSlot` already requires `>= 1`, `moderation.cpp:38-40`).

**T12 — never persist a session-scoped value in a cross-session store.** `seen_players` and
`ban_list` keep the REQUESTED BASE, not the arbitrated suffixed name, which in a later session is a
fingerprint of someone else's occupancy. Safe because both are keyed by guid/IP
(`ban_list.cpp:124-127`), so the nick is a display column and two people requesting the same base
stay distinct rows. The live admin view still reads the ledger.

**T13 — the publication boundary does not move.** `roster.cpp` publishes a POD snapshot precisely
because the UI draws on the Present thread while `NicknameForSlot` is `UE_ASSERT_GAME_THREAD`. The
ledger becomes the new INPUT to `roster::Refresh()`, which stays the sole GT publisher; the ID
column rides that POD as ONE new `uint16` field.

**T14 — out of session is a NAMED exception.** With no session there is no ledger, so `Refresh()`'s
`!running` branch synthesises one local row from the REQUEST with a blank ID column. "One
derivation" holds WITHIN a session.

---

## 3. ARC A — roster state + ID (ships first; fixes pre-existing defects)

> **AS-BUILT 2026-07-27 — four of five commits landed; the arc is NOT finished and NOT verified.**
> Protocol 129 → 130, DLL `multivoid-0.9.0n-130.dll`, Release builds clean. **No smoke, no hands-on,
> no audit.** Commits:
> - `72805d96` — the net-layer per-slot occupancy GENERATION + `tools/net/peerconn_gate.ps1` (the
>   standing gate; wired into `build-core.yml`, so **the release fingerprint is now stale and the
>   re-commit ritual is owed**).
> - `b196f595` — `coop/player/roster_ledger.{h,cpp}` (rows, occupancy, `PerSlotState<T>`,
>   `SubscribeSlotReplaced`, the host reconcile) + the `PlayerJoined → RosterRow` widening + the
>   repair pulse + the park gate; five per-slot arrays and `g_lastReadyBySlot` deleted (RULE 2);
>   `player_handshake.cpp` 847 → 684 with `player_handshake_roster.cpp` 353 extracted first.
> - `431decd9` — `roster::Refresh()` reads the ledger (the client-TAB fix) + the TAB ID column.
> - `701a1740` — `moderation::PlayerToken` in the API signature + `Session::KickWithToken` /
>   `GetPeerAddressWithToken` (the ban-hits-the-successor fix).
>
> **Three measured corrections to this document**, all found by reading the code during the build:
> 1. §2 T3's gate rule censused FIVE `peerConns_` write sites; there are **SEVEN**. The census
>    grepped `.store(` and both `.exchange(0)` clears were invisible to it. `Session::Kick`'s is
>    load-bearing — without its generation clear a kicked slot keeps a live generation and the ledger
>    never empties the row. The gate script now censuses by OPERATION KIND for exactly this reason
>    (`lesson_census_the_operation_kind_not_only_the_sites`).
> 2. §2 T6 / T9 put `joinSent` in the ledger row. It **cannot** be one: a client sends its Join to
>    slot 0 before it knows who is there, so the occupancy-gated setter would drop the latch and the
>    client would re-send its Join every tick forever. It describes the LINK, not the person →
>    `PerSlotState<bool>`. (Caught by reasoning through the client path, not by a test.)
> 3. §3 item 6's "four non-teardown stores" is wrong; see the note on that item.
>
> **Item 6's real fix LANDED** (`13470f10`) after the T5 read of all 20 fan-out bodies: zero unsafe,
> the host-authoritative ones self-gate on `Role::Host`, and six are the client-side leak fix.
> `babf501f` folded two smoke findings (a mis-scoped GT assert on `Reset`, and a log line I dropped
> that took mp.py's `xpeer_identity` counter with it).
>
> ### VERIFIED BY AUTONOMOUS RUN (2026-07-27, `multivoid-0.9.0n-130.dll`)
>
> **4-peer smoke, PASS** — host accepted `[1,2,3]`; every client sees the host + both peers via
> relay; `xpeer=[2,3]/[1,3]/[1,2]`; 0 puppet-spawn failures, 0 malformed drops, 0 stale-gen drops,
> 0 HotPathGuard violations. Ledger: slot 0 → #1 (role constant), slots 1-3 → #2/#3/#4 (monotonic;
> the host never draws #1). **IDEMPOTENCY PROVEN**: exactly 4 ledger transitions and exactly 2
> identity installs per client across the whole run, while the repair pulse re-asserted every 1-5 s
> throughout — the receiver treats rows as STATE, not events.
>
> **DEPARTURE drill, PASS** — the case the smoke does not cover, and the arc's central claim.
> Instrument: `<scratchpad>/arcA_departure_drill.ps1` (kills ONE client mid-session under a widened
> `smoke4 --duration`). With four peers up, killing CLIENT_2 produced, on the HOST **and on BOTH
> SURVIVING CLIENTS**, within the same second:
> ```
> feed: "Client2 left the game"
> net: peer slot 2 (#3) left -- puppet destroyed
> ledger: slot 2 emptied (was #3 'Client2')
> ```
> Exactly ONE emptying per peer (no double-fire under the pulse). **The two client lines are the
> whole point:** before this arc a client could not learn of a third peer's departure at all —
> `IsSlotReady(2)` is permanently false there, so no edge ever fired, the frozen puppet stayed in
> the world for the rest of the session, and the roster row never existed to begin with. CLIENT1's
> `pose-diag[slot 2] fresh=0/s` from 22:31:42 until the destroy at 22:31:48 is that frozen body,
> now actually collected.
>
> Three instrument bugs were fixed to get this drill honest, each measured rather than guessed: the
> log is ROTATED per boot (a line-count baseline can never be exceeded); the process name is
> `VotV-Win64-Shipping`, `VotV` is only the window title; and smoke4's default monitor window
> expired and killed the peers mid-settle.
>
> **REPLACEMENT drill + SUCCESSOR-BAN drill, PASS** (2026-07-27 late, DLL `c4d1d8d4bf1dcd3e`).
> Instrument: `tools/net/replacement_drill.ps1`. Two new `[dev]` flags make the claims observable:
> `roster_drop_empty_rows` (CLIENT_1 discards every inbound row that says a slot is EMPTY, so a
> departure can reach it ONLY as the successor's row) and `roster_token_selftest` (the HOST captures
> a moderation token when a slot is first occupied, HOLDS it across the departure as an open ban
> modal would, and fires the REAL `moderation::BanPlayer` with it once someone else holds the seat).
> CLIENT_2 runs without injection as the control; CLIENT_3 is the seat that changes hands.
>
> Replacement, on the LOSS-INJECTED peer — ~14 emptying rows discarded over the 38 s gap, absence
> never observed, and then in one tick:
> ```
> 23:42:21  ledger: slot 3 REPLACED -- #4 'Client3' -> #5
> 23:42:21  net: peer slot 3 (#4) left -- puppet destroyed
> 23:42:21  players::Registry: released Player Element eid=57344 for peerSlot=3
> 23:42:29  players::Registry: established MIRROR ... peerSlot=3 (puppet=00007FFE95699DC0)
> ```
> Death strictly before birth, from a TOKEN CHANGE ALONE. The control peer took the ordinary
> `emptied` -> `occupied by #5` path to the same end state.
>
> Successor-ban, on the HOST:
> ```
> roster_token_selftest: slot 3 changed hands #4 -> #5 --
>     NEGATIVE(stale gen=3)=REJECTED  POSITIVE(live gen=4)=ACCEPTED
> roster_token_selftest: PASS (net-layer generation guard)
> moderation: ban of #4 ABORTED -- slot 3 now holds #5
> ```
> The POSITIVE control is what makes the negative mean anything (a check that refused everything
> would pass a negative-only drill). Banlist ABSENT before and after. HotPathGuard 0 on all four
> peers.
>
> Three instrument corrections were measured on the way, all in the harness rather than the code:
> PowerShell unwraps a one-element result (the grading pass threw `op_Addition` AFTER `finally` had
> killed the peers); `installed cross-peer identity` is a deliberately install-only log line, so
> grading on it reports a false absence — the honest markers are the Registry's own
> `released`/`established MIRROR` pair; and a fixed settle killed the victim 23 s after it joined,
> before it had a body on the observers, so the teardown assertions failed against a peer with
> nothing to tear down. Every wait is now a stated precondition with a named timeout.
>
> **AUDITED (2026-07-27): 0 CRITICAL.** Perf agent: every pump-reachable function is O(1)-or-
> throttled, `Transition` cannot fire per-tick, the render thread never reads the ledger. Correctness
> agent: the wire parse is bounds-clean, `RosterRow` is absent from `IsClientRelayableReliableKind`
> so a forged row is never relayed, the token fails CLOSED, and the sampled teardown bodies are
> role-safe. Findings folded: **IMPORTANT** — `Reset()`'s comment claimed "every subscriber
> early-returns on an unoccupied outgoing row", which was false (`OnSlotReplaced_ArmPulse` carries no
> such guard) and rested on an unenforced precondition; `Reset()` no longer fans out to
> SlotReplaced subscribers at all, so bringup-thread safety is structural. **MINOR** — the stale
> `PlayerJoined` / `g_skinBySlot` prose in `protocol.h` / `session_lanes.h` /
> `player_handshake_detail.h` was swept (historical provenance lines kept on purpose).
> **MINOR, NOT actioned:** `session.h` is 817 LOC, 2% over the soft cap — the arc-A generation API is
> class MEMBERS, so it cannot move to a sibling header without a mixin refactor of the core net
> class, and the audit's alternative (pushing `peerGenerationForSlot` out-of-line) would pessimize a
> per-tick call for a cosmetic gain. Flagged per the file-size rule; extract when the file is next
> touched for a real reason.
>
> A defect the drill caught that the audit did not: `roster_token_selftest::Install` printed its
> arming line **14,095 times in 4 minutes** — `coop::subsystems::Install` is a per-tick RETRY pump
> (`net_pump.cpp:720`), not boot code, and the perf agent classified that same call site COLD. Latched;
> the count is now 1. See `memory/lesson_subsystems_install_is_a_per_tick_retry_pump.md`.
>
> **Remaining:** hands-on (never done for arc A); the smaller arc-A drills in the list below
> (successor voice silence, version-gate-rejected joiner, parked-row ordering, session-end counting).


Own protocol bump. **Zero new wire kinds. Zero new external primitives beyond the net-layer
generation.**

1. `PlayerJoined` is widened into a true slot-state row (`+playerNo`) that MAY describe slot 0 and
   the receiver's OWN slot; those rows carry `eid = 0` (the existing sentinel convention,
   `player_handshake.cpp:495-496`) so the receiver skips mirror install. Internal rename
   `PlayerJoined → RosterRow` (wire value unchanged; the current name lies about the semantics).
2. ONE DERIVATION (in-session): the ledger feeds `Refresh()`; the client roster stops deriving from
   connection state. The display FALLBACK moves INTO the ledger — it returns a ready string — so
   the six measured my-vs-theirs sites collapse to one (`chat_sync.cpp:128`,
   `peer_action_feed.cpp:51`, `roster.cpp:64`, `roster.cpp:95`, plus the renderer-side defaults
   `scoreboard.cpp:115` and `admin_panel.cpp:87`).
3. APPLY = idempotent STORES + EDGE EFFECTS, each gated on an actual change or an existing latch.
   The handler performs five effects today (`:688` mirror install, `:710` skin, `:715` prefs,
   `:719` colour, `:729` toast); under a pulse the toast would spam and the mirror would reinstall
   for nothing. A receiver NEVER applies declared fields (skin/colour/prefs) from a row describing
   its OWN slot — otherwise the host pushes back its cached copy of one's own skin (the authority
   inversion `nick_color.cpp:108-113` exists for). Change detection means a steady-state pulse
   touches the engine zero times.
4. `playerNo`: session-monotonic; host = **#1 as a ROLE CONSTANT** (the counter starts at 2 and the
   host never draws from it, so a re-seed cannot mint a second #1); never reused within a session.
   The host's row 0 is cleared only by session stop/`Reset()` and is seeded by an idempotent,
   role-gated, asserted `EnsureRowZeroSeeded()` called as a PRECONDITION of arbitration — not "the
   pump ticks first". TAB gains the ID column.
5. REPAIR: a host re-assert pulse enumerating ALL slots INCLUDING `playerNo == 0` rows (otherwise
   absence never heals), plus a full re-assert on any roster change. ADAPTIVE period: ~1 s for the
   first ~10 s after a Join — the measured R-A burst window, and exactly when a joiner presses TAB —
   decaying to ~5 s. Counted: a row is ~40 B, so four rows are ~160 B/s fast and ~32 B/s steady;
   against the measured R-A context (~512 KB buffer, losses during a ~500 KB save-transfer burst)
   the pulse cannot CAUSE backpressure, only be a victim, which is what it heals from.
6. MIGRATION: the four measured non-teardown person-state stores — `voice_playback channels_[slot]`,
   `players_registry playerBySlot_`, `player_inventory_sync`, `item_activate` — are migrated onto
   `OnSlotReplaced` IN THIS ARC. Shipping the mechanism without migrating its consumers would be
   framework-without-consumers. Every EXISTING subscriber body is also reviewed for the client
   firing-set expansion (T5).

   > **MEASURED CORRECTION 2026-07-27 (build pass) — this item is WRONG as written, and the true
   > shape is bigger.** All four ARE already reached on a departure: `subsystems::DisconnectSlot`
   > (`subsystems.cpp:343-373`) fans out to ~18 per-slot subsystems including `voice_chat::
   > OnDisconnectSlot`, `item_activate::OnDisconnectForSlot` and `player_inventory_sync::
   > OnDisconnectForSlot`, and `puppet_drive::DestroySlot` → `Registry::UnregisterPuppet` →
   > `DropPlayerElement_` covers `playerBySlot_`. So they are not "non-teardown stores" and there is
   > nothing to migrate one-by-one.
   >
   > The REAL defect is one level up, in what DRIVES that fan-out: `net_pump.cpp:463-472` fires it
   > off a FALLING EDGE of `IsSlotReady`. Two consequences, both structural:
   >   - **On a CLIENT the fan-out never runs for slots 1-3 AT ALL** — `IsSlotReady(2)` is
   >     permanently false there (a client only fills `peerConns_[0]`), so the edge never rises and
   >     therefore never falls. A client keeps a departed third peer's voice channel, inventory
   >     bookkeeping, item-activate state and Player Element for the whole session. Same root as the
   >     TAB defect, and it was invisible because the edge simply never fired rather than firing
   >     wrongly.
   >   - **On the HOST a fast REPLACEMENT skips the edge** — ready→ready across one 8 ms tick.
   >
   > The fix is therefore ONE change, not four: drive `DisconnectSlot` from the ledger ROW
   > TRANSITION (which compares values, not edges) instead of from the `IsSlotReady` edge. That is
   > the invariant rather than a site list.

   **AS-BUILT 2026-07-27 (`b196f595` + `13470f10`) — this item's "NOT BUILT" stood stale until the
   2026-07-28 sweep.** `net_pump.cpp:290` registers `OnSlotReplaced_TearDownWorld` and
   `player_handshake.cpp:368` registers `OnSlotReplaced_TearDownPerson`; `subsystems::DisconnectSlot`
   is composed at `net_pump.cpp:258-273` with the T5 read recorded in comment. The ~20 bodies WERE
   read before the change (zero unsafe: the host-authoritative ones self-gate on `Role::Host`, six
   ARE the client leak fix, none sends from a client), and the departure + replacement drills both
   PASS on host and on both surviving clients. Evidence: `[[project-arc-a-roster-ledger-2026-07-27]]`.
7. Cosmetic follow-through: after (2) a client renders `ping`/`link` for slots 1-3 for the first
   time; they land on `rttMsForSlot() == -1` beside "VIA HOST" and need a deliberate presentation.

   **2026-07-27, ON THE FIRST SCREENSHOT: this is NOT cosmetic. It is a fused-axis defect, and the
   fix is a wire change.**

   > ### AS-BUILT 2026-07-28 — v131, proto 130 → 131 (`890b7fae` + `2a12fc5d` + `1fc74b0c`)
   >
   > Built after a 15-round `/qf` design pass and a 2-round decisions pass. Every claim below the
   > line is preserved as it was WRITTEN, because two of them turned out to be **wrong**, and the
   > way they were wrong is the durable part:
   >
   > **CORRECTION 1 — "WHY THE CLIENT CANNOT SIMPLY DO BETTER" is FALSE as stated.**
   > `session_status.cpp:491-494` bailed on `if (hConn == 0) return false;` **above** the
   > `cfg_.topology == LanDirect` branch that returns `"LAN"` — and that branch needs no connection
   > at all. An ownership guard sitting above a config-derived answer is what made a shared fact look
   > role-exclusive; a client could always have printed `LAN` correctly. The publish rule still fires,
   > but for the **PING**, which is per-connection in every topology and which no client can ever
   > derive for another client. See `[[lesson-verify-role-exclusivity-before-publishing]]`.
   >
   > **CORRECTION 2 — the host row does NOT keep `LAN HOST`.** That was role wearing transport's
   > clothes: the same fusion one layer down. Role moved to a `HOST` tag beside the NAME; the host's
   > Link and Ping cells read `n/a` (nothing to measure — their traffic never crosses a socket),
   > distinct from `--` (no sample yet).
   >
   > **What shipped:** `RosterRow` gains `[u8 linkKind][i16 pingMs]` in the **FIXED PREFIX** after
   > `eid` — not the tail, because the tail's offset arithmetic lives inside `if (applyDeclared)`,
   > which is skipped for exactly the host row and the receiver's own row, i.e. the two rows this fix
   > exists for. `roster_ledger::RefreshLinkFacts` fills rows 0..3 **immediately before each send**
   > (never on a clock of its own). Every kind is measured FROM THE CONNECTION — the GNS `Relayed`
   > flag and the remote address — never from `cfg_.topology`, which used to label a port-forwarded
   > WAN peer `LAN`; when GNS leaves the address all-zero the classifier falls back to its
   > `LoopbackBuffers`/`Fast` bits and answers `Unknown` rather than asserting `Direct`.
   > `roster::Refresh` has **zero role branching**. RULE 2 deletions: `"VIA HOST"`, the forced
   > `ping = -1`, `RemotePlayer::SetPing`/`GetPing`/`pingMs_`, the per-tick `event_feed` RTT fan-out,
   > the `strstr` over a display string, `LinkLabelForSlot`, `Session::topology()`. `ui/link_format`
   > is the one renderer for scoreboard + admin panel + nameplate (three hand-copied cascades before).
   >
   > **Evidence:** all four boards + nameplates photographed on the shipped bytes
   > (`research/roster_shots/`, `research/plate_shots/client1_1.png` — peer plates carry `(<1ms)`,
   > the host plate is bare, exactly inverted from 07-27); `link-classify selftest PASS (15/15)` on
   > every peer, machine-asserting `Direct`/`Relayed`, which **no LAN drill can reach**; `ledger:`
   > flat at 4 lines per peer over 4 minutes; smoke4 PASS ×2; two audits, 0 CRITICAL, folded.
   > **NOT hands-on.** Full record: `[[project-v131-player-list-one-question-2026-07-28]]`.

   What the user saw on a 4-peer LAN session where every peer is on 127.0.0.1:

   | board | Host row | own row | other client rows |
   |---|---|---|---|
   | HOST    | `LAN HOST` | (is the host) | `LAN  <1ms` | 
   | CLIENT  | `LAN <1ms` | `LAN` | **`VIA HOST  --`** |

   Their words: *"all clients i assume were connected via lan, why it says via host on 2 clients and
   lan on one client. It should be all the same, no special treatment."* They are right, and the
   reason is not presentation.

   **ROOT CAUSE — one column, TWO axes.** `Link` fuses (a) the peer's TRANSPORT to the session
   (LAN / P2P / P2P RELAY) with (b) MY ROUTE to that peer (direct / relayed by the host). On the
   HOST's board the two axes coincide — the host holds a direct connection to everyone — so the
   fusion was invisible for as long as the host's board was the only one that listed peers. Arc A
   made a CLIENT list peers for the first time, and there the axes diverge: two rows report
   transport, two report routing, in the same column, with no way for the reader to know which.

   **WHY THE CLIENT CANNOT SIMPLY DO BETTER.** `Session::LinkLabelForSlot` (`session_status.cpp:484`)
   reads `peerConns_[peerSlot]` — a connection THIS peer owns. A client owns only `peerConns_[0]`,
   so it is structurally incapable of labelling another client's transport, exactly the
   host/client asymmetry that produced the arc-A teardown bug. Same for `rttMsForSlot`. The client
   is not being lazy; it does not have the fact.

   **THE RULE-1 FIX: the host PUBLISHES what only the host knows.** The scoreboard's connection
   column should answer ONE question on every row of every board — *how is this player connected to
   the session* — which in a host-authoritative game means their link to the HOST. That is the
   number every other multiplayer scoreboard shows. The host is the only peer holding it, and
   `RosterRow` is already the channel by which the host asserts per-occupant facts, so the transport
   label and the RTT-to-host join the row. Then:
     - every row on every board describes the same property, no row is special;
     - "VIA HOST" disappears entirely — it was answering a question nobody asked;
     - a client's own row keeps showing its own link (which IS its link to the host, so it is the
       same axis, not an exception);
     - the host's own row keeps `LAN HOST` / `P2P HOST`: it is not connected TO the session, it IS
       the session.
   This is a WIRE CHANGE (RosterRow gains a link label + a ping) and therefore bumps
   `kProtocolVersion` per the standing rule.

   **Also owed (same surface, same session):** the table draws no header row at all — `TableSetupColumn`
   names the columns and `TableHeadersRow()` is never called, so the ID/Link/Ping columns are
   unlabelled. User: *"the rows are not even named properly."*

### Arc A drills

Status as of 2026-07-27 late. RUN = an instrument executed it and its verdict is recorded in the
AS-BUILT block above; OPEN = no instrument exists yet.

- **RUN** A client sees all four rows; a departing third peer disappears from a client's TAB.
  (`tools/net/departure_drill.ps1`.)
- **RUN** Third peer leaves, fourth takes its slot, the departure row is INJECTED-LOST → the receiver
  sees the token change, performs death+birth, renders the new person with no ghost frame.
  (`tools/net/replacement_drill.ps1` + `[dev] roster_drop_empty_rows`; death/birth graded on the
  Registry's own `released` → `established MIRROR` ordering.)
- **RUN** Ban-modal successor drill: a token captured when the slot was first occupied, HELD across
  the departure, fired at the successor → the action ABORTS and the newcomer is NOT banned.
  (`[dev] roster_token_selftest`; asserts the PAIR stale=REJECTED + live=ACCEPTED, and the banlist
  file is compared before/after.)
- **RUN (subsumed)** Accept-ordering drill: the guard that covers it is the generation check against
  the LIVE net authority rather than the ledger mirror, which is precisely the NEGATIVE assertion the
  successor drill makes. A successor accepted before the game thread reconciles is already refused
  there. No separate instrument needed; recorded so it is not re-derived.
- **RUN (by count)** Steady-state pulse: zero engine calls, no toast — proven as exactly 4 ledger
  transitions and 2 identity installs per client across a whole run while the pulse re-asserted every
  1-5 s.
- **OPEN** The successor has SILENCE on the voice channel and their OWN inventory.
- **OPEN** A departed peer's bubble / colour / plate / hand state is cleared ON A CLIENT.
- **OPEN** A version-gate-rejected joiner never appears on any client.
- **OPEN** A doomed never-ready connect emits no toast and creates no row.
- **OPEN** Join during a save-transfer burst with row loss injected → full roster within the adaptive
  window. (`roster_drop_empty_rows` injects EMPTY-row loss only; this needs loss of OCCUPIED rows,
  which is a different injection.)
- **OPEN** Session-end counting drill: three peers → exactly THREE transitions per subscriber, no menu
  toast, next session starts with all migrated stores empty.
- **OPEN** A row arriving before the `LocalPeerId` stamp is parked and applied correctly. (The park
  path exists and is code-reviewed; nothing has forced the race.)

---

## 4. ARC B — nickname arbitration (depends on arc A)

**Authority:** the host owns what it ARBITRATES (the nick); the peer owns what it DECLARES (skin,
colour, nameplate prefs) — the host merely RELAYS those, as `BuildPlayerJoinedPayload` already does
(`:601-623`). The three peer-authored forgery-guarded lanes (`player_handshake_prefs.cpp`
`:104`/`:169`/`:233`) are UNTOUCHED. Relaying is not authoring: only the peer's guarded lane can
CHANGE such a value.

**Two homes, one writer each.** `g_localNick` is the REQUEST (ini/menu), immutable after bringup;
its accessor is RENAMED `RequestedNickname()` with no alias kept (RULE 2), and its only callers are
the Join payload builder and the ledger's fallback. The ARBITRATED name lives only in the ledger.

**Arbitration** runs host-side at Join-received, after sanitize and BEFORE store / nameplate /
toast / relay / `seen_players` — i.e. before the name is ever published, so the placeholder → name
transition is never a rename of a published name. The collision set is the ledger plus the per-base
high-water map; rows that exist without a nick yet (connected, not yet Joined) are present and block
nothing. Arbitration is latched per slot, so a repeat Join reuses the assigned nick and number.

**Suffix algorithm.** The candidate is BUILT as `(shrunk base + k)` and FREENESS is checked on THAT
final string, **under the FOLD KEY**, together with a `SanitizeNickname` FIXED-POINT postcondition
asserted in code. Both halves are load-bearing:
- searching on the full base and shrinking afterwards means a 20-char base + "2" is 21 chars, the
  receiver's hard break at `:212` cuts the SUFFIX DIGIT, and the name collapses back into the
  collision;
- checking freeness on the exact string would let "pelmentor" pass while colliding under the very
  key `/commands` resolve by (the sanitizer is case-PRESERVING, `:199-203`).
`k` starts above the base's high-water mark, so a name issued this session is never re-issued to a
different person. First-come-first-served; existing holders are never renamed; the display keeps the
user's case. A fresh lobby reproduces the user's literal 2/3/4 example.

**High-water map.** Per-fold-base counter (the value is 4 bytes; the KEY is a string, so memory
grows per distinct base). CAP 256 bases with least-recently-issued eviction (~10 KB ceiling).
Documented consequence: an evicted base has no live holder by construction (live rows are checked
separately), so the only loss is the never-re-point property for a base unused for hundreds of joins.

**Fold.** ASCII lowercase ONLY — complete over the sanitizer's alphabet. Spaces and punctuation are
deliberately NOT folded (that would merge "Bob Ross" with "BobRoss"; anti-impersonation is a
separate concern). The fold and the candidate generator live in ONE TU and are shared with the
future `/command` resolver, so assignable == targetable. The compile-time wrapper ratchet (a nick
type with `operator==` deleted) is DEFERRED to consumer #2 — building it now would be machinery for
a consumer that does not exist. The positive-controlled grep gate ships now, with its
false-negative documented so a green gate is never read as proof.

**No guid reclaim.** The v73 guid is peer-DECLARED (the joiner's own ini `player_guid=`,
`config.cpp:557-570`; receive-side validation is only "32 hex chars", `:374-381`), so reclaiming a
name or ID by it would let a stranger take both. Name drift across many reconnects is ACCEPTED as
cosmetic and documented; reclaim is deferred to peer certificates (Tier C), where the key becomes
cryptographic and nothing else in this design changes.

**Display and nameplate.** All six my-vs-theirs sites read the ledger; the ledger falls back to the
request until the canon lands. `RemotePlayer::nickname_` becomes an explicit CACHE with ONE feeding
path — `RepublishNameplateForSlot(slot)` reading the ledger, invoked from the change-detected GT
step (gated on an actual nick change AND the puppet existing); `puppet_drive` calls the same
function at spawn, closing the identity-before-puppet race its `:131-137` comment describes. Today
that field has three writers (`:524`, `:725`, `puppet_drive.cpp:138`).

**Adopt.** A roster row whose slot == mine writes the LEDGER (not the request) on the game thread,
reusing the `SanitizeNickname` store path (`:227-233`). The `g_localNick` discipline is rewritten
explicitly (bringup-once before the pump, then GT-owned, asserted) rather than silently violated.
INVARIANT: roster-row application stays ENGINE-FREE, or it loses the pre-world classification
`session_lanes.h:251` grants it.

**Invariants.** Nick = session display/targeting handle. Guid = declared per-install key, NOT an
authority key. IP = ban key. The SUFFIX IS NEVER PARSED — targeting is by fold-key or `playerNo`,
and TAB prints both ("#7 Pelmentor2"), so a human never has to infer one from the other.

**Same-commit fixes.** `server_browser.cpp:114` currently promises the typed name is "sent on Join +
shown on your nameplate/scoreboard", which stops being true once the host can rename — reworded to
say the name is sent as a REQUEST and the host may hand back a numbered variant. The refuse-feed
dedupe key moves from `(nick, reason)` to `(senderSlot, reason)`
(`player_handshake_version.cpp:89-96`), so two same-named refused peers no longer collapse into one
feed line.

### Arc B drills
Four peers with the same nick INCLUDING the host (proving the row-0 seed); leave/rejoin; fast
leave+rejoin inside one tick; a 20-char base asserting UNIQUENESS of the shrunk result (not merely
sanitizer-invariance) and a base ending in space/dash; two peers whose garbage sanitizes to "Player"
→ "Player2"; a case-variant collision ("pelmentor" vs "Pelmentor") proving fold-key uniqueness;
plate ordering A (puppet spawns before the row) and B (the pulse repairs the row after the plate was
labelled); a skin-change + nameplate-toggle regression proving the pref lanes are untouched.

**Accepted residual (~1 RTT):** the joiner's own chat echo composed before its first roster row
renders the requested name in its OWN scrollback only. Every other surface resolves receiver-side.

---

## 5. Out of scope

Mid-session nickname change. No wire exists for it (only `NickColorChange=88` for colour), the nick
is join-time-fixed, and the user asked for join-time resolution.

---

## 6. MTA precedent and the deliberate divergences

Per RULE 2026-05-28 the MTA equivalent was read first. Two divergences, both recorded:
- **MTA REFUSES a duplicate nick** — `NICK_CLASH` → `DisconnectPlayer` (Server
  `CGame.cpp:2065-2068`). We suffix-resolve instead, per the user's directive: at four-friend scale
  with no account identity, refusal is hostile UX.
- **MTA REUSES element ids** (`CElementIDs::PopUniqueID` / `PushUniqueID` over a `CStack`) and
  targets by nick. We add a session-monotonic `playerNo` because kick/ban are destructive and a
  stale ID must FAIL CLOSED rather than mis-target a slot's new occupant — and because the user
  asked for IDs in TAB. Only ONE id space is human-visible; the slot is never displayed or typed.

---

## 7. Filed, not fixed here

Two pre-existing security-tracker rows surfaced while measuring the guid:
- the peer-DECLARED guid already keys per-player inventory AND `seen_players`, so a copied install
  folder (one ini line) means two peers share an inventory;
- an empty/invalid guid is a shared empty key.

Neither is caused or fixed by this design; both belong in `docs/security/TRACKER.md`.

---

## 8. ARC C — per-slot ingest gating (filed, NOT in A or B)

The token is a ROSTER-ROW concept. Per-slot INGEST (pose, voice, hand, bubbles) is not gated by it,
so in the pre-row window a client files the new occupant's state under the old occupant's slot.
**This happens TODAY without any token — the design neither worsens nor fixes it**; T4/A5 shrink the
window to ~1 RTT normally. The drill promise is narrowed accordingly: arc A eliminates the
roster/identity ghost, NOT ingest cross-attribution. Arc C is also the designated answer if the
stale-person-state class recurs a third time (read-side gating, not a longer checklist).

---

## 9. PRODUCT DECISIONS — ANSWERED by the user 2026-07-27

The four questions below were put to the user and answered. Recorded as decisions; two of them
CHANGE the scope above, and one CORRECTS a measurement this document previously carried.

**D1 — no role in the name. DECIDED.** User: "в нике не должно быть ролей." No "(host)" suffix
anywhere; the host's role stays expressed by the gold nick colour and the TAB Link column
("LAN HOST"/"P2P HOST"). §2/§4 already assume this — now it is fixed, not a default.

**D2 — the digit is a DISAMBIGUATOR, nothing else. DECIDED.** User: "Цифра в никнейме вообще никак
не должна быть связана/привязана с ID игрока. Мы добавляем цифру просто чтобы у всех игроков был
уникальный Nameplate и они не путались если два Pelmentor гуляют по комнате." So: the suffix
carries NO identity, is never parsed, and must never be derived from `playerNo` (this re-confirms
the R10 reversal). Drift across reconnects stays accepted as cosmetic (§4, no guid reclaim).
**Live tension to resolve in the arc-B build pass:** the §4 high-water rule (`k` starts above the
base's high-water mark) exists so a name issued this session is never re-pointed at a different
person — which serves the very confusion the user named — but it is also the sole cause of upward
drift. Dense-smallest-free gives shorter names and re-points; high-water gives stable names and
drift. The user's stated GOAL (two Pelmentors in one room must not be confusable) argues for
high-water; recorded here so the build does not silently pick the other one.

**D3 — the ID never appears on the nameplate; TAB carries everything. DECIDED + SCOPE ADDED.**
User: "ID не должно быть в никнейме тоже (nameplate я имею в виду). А в tab ВСЯ информация пусть
будет прям как в SA-MP tab." The in-world nameplate renders the arbitrated NAME ONLY. The
scoreboard becomes the full-information surface, SA-MP-shaped. **Measured delta** (`ui/scoreboard.cpp:104-112`):
today the table is `Player | [Mic] | Link | Ping`; SA-MP's is `ID | Name | Score | Ping`. So the
only missing column is **ID** — which is exactly arc A's `playerNo` — and VOTV has no score
concept, so that column has no honest analogue and is NOT invented. Mic/Link are ours and stay.
Divergence between the TAB number and the name's digit ("#7 Pelmentor3") is explicitly accepted
("Номер ID в TAB разные от текучки это норм").

**D4 — Cyrillic MUST be supported. DECIDED → its own arc (below).** User: "Кириллицу надо
поддерживать." This makes a previously-optional widening mandatory, and it is NOT a one-line change
to `SanitizeNickname`.

### 9a. ARC D — Cyrillic nicknames (NEW scope; fact base measured 2026-07-27)

**CORRECTION to the earlier cost note.** This document previously claimed a 12+ character Cyrillic
name "renders BLANK" and cited `roster.cpp:28-35` for `NarrowNick`. Re-measured: there are THREE
distinct narrowing sites with THREE different failure modes, and the blank one is not the nameplate.

| Site | Shape | Failure on Cyrillic |
|---|---|---|
| `player_handshake.cpp:199-212` `SanitizeNickname` | keeps ASCII alnum + `-_.` + space; 20-wchar cap | every Cyrillic char STRIPPED → empty → `"Player"` fallback (`:224`). This is the root gate. |
| `coop/player/roster.cpp:28-35` `NarrowNick` (TAB rows) | `WideCharToMultiByte` into **23 bytes**; on insufficient buffer the API returns 0 → `out[0]='\0'` | Cyrillic is 2 bytes/char UTF-8, so a **12+ char** name yields an EMPTY row nick; the scoreboard then prints the `"Remote player"` fallback (`scoreboard.cpp:115`). This is the blank case. |
| `coop/player/nameplate.cpp:79-86` `CopyNickAscii` | `(c >= 32 && c < 127) ? c : '?'` into `char[24]` | the in-world plate renders `??????????` — mangled, not blank. |
| `moderation.cpp:44-51` (ban record), `seen_players.cpp:141`, `object_overlay.cpp:106`, `player_inventory_sync.cpp:111-116` | same two shapes (`'?'` substitution / byte cap) | informational surfaces; each asserts the ASCII invariant in a COMMENT, so widening silently falsifies four comments. |

**What is already solved (measured, and it makes this cheaper than feared):**
- **Rendering is not a blocker.** Our ImGui atlas bakes `io.Fonts->GetGlyphRangesCyrillic()`
  (`ui/fonts.cpp:188`), and the default family Fixedsys Excelsior covers Cyrillic (cmap-verified
  5992 cp, `fonts.cpp:44-46`). The nameplate is OUR overlay (there is a dedicated `Role::Nameplate`
  font), not the game's UMG — so the game's Latin-only `font_ui` never enters this path.
- **The wire is not a blocker.** The nick field is `[u8 len][UTF-8 bytes]`
  (`player_handshake.cpp:424-433`, built by `ToUtf8` `:139-148`), so 20 Cyrillic chars = 40 bytes
  fits in the 255-byte length prefix. **No protocol bump for the alphabet itself.**

**Therefore ARC D is: one alphabet decision + a byte-vs-character audit of the fixed buffers, not a
font or transport project.** Open design points it must answer, none of them settled here:
1. **Which alphabet?** Cyrillic-plus-ASCII, or a general Unicode-category rule? The sanitizer's
   stated job (`:167-181`) is blocking control chars, RTL override, and combining diacritics — a
   category rule must preserve those defences explicitly, not by accident.
2. **The cap changes meaning.** 20 is a *character* cap at `:194` but every downstream buffer is a
   *byte* buffer. Either the buffers grow to `20 chars x 4 bytes + NUL`, or the cap becomes a byte
   cap and a 20-char Cyrillic name is refused at entry. Truncating UTF-8 mid-sequence must be
   impossible by construction, and `NarrowNick`'s silent all-or-nothing failure must become a
   defined truncation.
3. **The fold key breaks.** §4's fold is "ASCII lowercase ONLY — complete over the sanitizer's
   alphabet"; widening falsifies that completeness claim, so "пельментор" and "Пельментор" would
   collide on the nameplate while resolving as two different targets. Cyrillic case-folding
   (including Ё/ё) becomes load-bearing for arbitration AND for the future `/commands` resolver.
4. **Four ASCII-asserting comments** must be corrected in the same commit (RULE 2 — a false comment
   about an invariant is worse than none: `[[lesson-false-security-comment-worse-than-none]]`).

The fact base above stands. **§9b supersedes its framing:** a 19-round `/qf` moved the root twice, so
"one alphabet decision + a buffer audit" is NOT what arc D turned out to be.

---

## 9b. ARC D — international text in names and chat (19-round `/qf`, 2026-07-27)

**STATUS: DESIGN. Not built, not drilled, not hands-on.** The pass ran 19 rounds and the critic was
still returning material questions at 19 — but by R17 the questions had moved off the design and onto
the *drill*, so the pass was stopped deliberately rather than declared converged. **This is not a
"that holds".** Five measurements gate the build (§9b.7).

### 9b.1 What the scope became

The user widened the ask three times mid-pass: Cyrillic → "и еще китайские и японские иероглифы и не
только" → "эмодзи в никах и в чате… per rule 1". Decisions taken, in order:

| # | Decision | Consequence accepted |
|---|---|---|
| D-a | Codec and repertoire ship TOGETHER (no window where a name is accepted but undrawable) | later refined by D-d |
| D-b | Base tier EMBEDDED, extended tier an optional MTA-style PACK ("не хотелось бы добавлять кучу мб внутрь dll") | ~+2.5-3 MB on a 15.4 MB DLL |
| D-c | Colour emoji embedded by default; **single-codepoint only** ("одиночных эмодзи достаточно") | no ZWJ families, no skin tones, **no flags** — we have no shaping engine |
| D-d | **Names accepted strictly within the embedded base tier; the pack extends READING only** | rare CJK is legal in chat, not in a name; refused at input with a visible reason |
| D-e | Mixed-script rule REJECTED ("двойников разрешаем") | Latin `Anna` and Cyrillic-А `Аnnа` render alike, get no suffix — a knowingly accepted residual |
| D-f | The pack ships as a **release asset**, not a tracked repo file | a binary in git history is paid by every cloner forever |

D-d was a REVERSAL of an earlier user decision, taken after a measurement (§9b.3) showed the earlier
one rested on a false picture. That correction is the most important event in the pass.

### 9b.2 The root moved twice

**Move 1 (R1).** The root is NOT that the ASCII allowlist strips Cyrillic — it is that **there is no
UTF-8 decode at entry at all**. `config.cpp:505-509` reads `net_nick` as `std::string` and does
`std::wstring(nick.begin(), nick.end())`, a per-BYTE Latin-1 widen; `session_runtime.cpp:380-384`
repeats the shape; `server_browser.cpp:118` (ImGui `InputText`) emits UTF-8 and `:121` writes those
bytes to the ini. So a UTF-8 name reaches `SanitizeNickname` as N mojibake wchars and is stripped
whole. Widening the alphabet without fixing this would legalise mojibake, not support Cyrillic.
This falsified the doc's earlier claim that "20 Cyrillic chars = 40 bytes fits the wire".

**Move 2 (R3).** The codec being designed **already ships**: `coop/comms/chat_sync.cpp`, in production
since 2026-07-04, holds `SanitizeUtf8:30-38` (denylist — strip control bytes, keep TAB, pass the
rest), `NickUtf8:42-68` (UTF-16→UTF-8 **with surrogate-pair handling** `:46-50`) and
`TrimAndCap:72-84` (byte cap that backs off past continuation bytes to a **character boundary**
`:79-81`). Arc D is therefore an EXTENSION of a proven primitive to the nick lane, not a new codec —
`[[lesson-reuse-proven-author-not-raw-reimpl]]`.

### 9b.3 The measurements that decided the design

| Measured | Where | What it decided |
|---|---|---|
| **Emoji are blocked by a define, not by fonts** | `imconfig.h:65` keeps `IMGUI_USE_WCHAR32` commented → `ImWchar` is 16-bit (`imgui.h:273`), `IM_UNICODE_CODEPOINT_MAX = 0xFFFF` (`:2515`), and `imgui.cpp:1512` DROPS the reassembled astral codepoint | No glyph range can express U+1F300; **precondition 0**, ahead of every font question |
| **The missing-glyph path collapses to ONE glyph** | `imgui_draw.cpp:3699-3712` picks `FallbackGlyph` from `{U+FFFD, '?', ' '}` and returns it for every absent codepoint | "Boxes are fine" was false — every missing codepoint looks the same |
| **Only FSEX300 of our seven TTFs carries U+FFFD** | cmap sweep, 2026-07-27 | Six families fall through to `'?'` — the ASCII squash this arc deletes. **U+FFFD must ride the donor merge.** |
| **All seven embedded TTFs cover U+0400-04FF; none covers CJK or emoji** | cmap sweep | Cyrillic ships with ZERO new font bytes |
| ~~**Colour path exists and costs the whole atlas**~~ **FALSIFIED 2026-07-28** | `LoadColor` switches the BUILD BUFFER to RGBA32 (`imgui_freetype.cpp:665-669`), but both halves upload via `GetTexDataAsRGBA32` (`imgui_impl_dx11.cpp:330`, `imgui_impl_dx12.cpp:310`), which allocates an RGBA32 copy regardless (`imgui_draw.cpp:2501-2523`) | ~~4x VRAM for ALL faces~~ — the GPU texture is ALREADY 4 B/px today; colour is VRAM-neutral and skips a conversion pass |
| **`FT_DISABLE_PNG ON`** | `CMakeLists.txt:80` | CBDT/sbix donors (Noto Color Emoji) cannot rasterize — the emoji donor must be **COLR/CPAL** |
| **No shaping engine** | `FT_DISABLE_HARFBUZZ ON` (`CMakeLists.txt:81`); ImGui does none | ZWJ sequences, skin-tone modifiers and regional-indicator flags cannot compose (accepted, D-c) |
| **Glyph cost in real CJK fonts** | outline bytes ÷ glyph count: SimSun 411 B, MS YaHei 620 B, Yu Gothic 522 B | ~3k common hanzi ≈ 1.2-1.9 MB; GB2312 (6,763) ≈ 2.8-4.2 MB; full CJK (~21k) ≈ 8.6-13 MB |
| **Vendored ImGui is 1.91.5** | `imgui.h:31-32` | Before 1.92's dynamic atlas → ranges must be baked up front |
| **MTA's precedent** | `CGraphics.cpp:1471,1482` registers `unifont.ttf` from `MTA\cgui\` via `AddFontResourceEx(FR_PRIVATE)`, credited `CCredits.cpp:285`; the file is NOT in their repo; `CMessageLoopHook.cpp:53` enables UTF-16 `WM_CHAR` | MTA ships CJK as a loose delivered file, exactly the pack shape — and solved the same entry problem |
| **We already load fonts from disk** | `fonts.cpp:133-138 AddFromFile` returns nullptr if absent | The pack mechanism is mostly built |

### 9b.4 D1 — the codec

ONE owner, used by the nick lane AND the chat lane, dissolving the **three** encoder copies that
exist today (`chat_sync.cpp:42-68`, `chat_feed::ToUtf8`, `player_handshake.cpp:139-148`) — RULE 2.

- **Decode UTF-8 at entry**; DELETE both Latin-1 widens (`config.cpp:509`, `session_runtime.cpp:383`).
- **FOUR truncators replaced in the same commit.** The census had been of *widens* until R16 caught
  it: `config.cpp:508 resize(255)`; `player_handshake.cpp:302` and `:573` `nickUtf8.resize(200)`
  AFTER `ToUtf8` — raw byte cuts that manufacture exactly the ill-formed sequences the new receive
  boundary destroys, i.e. **our own sender's nick would arrive as the placeholder**; and
  `SanitizeNickname:212`'s cap counted in UTF-16 units, which splits an astral surrogate pair.
- **Receive boundary establishes well-formedness where it READS**: `MultiByteToWideChar` gains
  `MB_ERR_INVALID_CHARS` (today `player_handshake.cpp:153-159` passes flags 0 and the ASCII allowlist
  is the only destroyer of ill-formed bytes) and rejects the whole field to the placeholder — no
  repair. Entry truncation is a cap on MY machine and guarantees nothing about a stranger's bytes.
- **Denylist, not allowlist**, keeping exactly the denials `SanitizeNickname:161-186` documents
  (control chars, U+202E RTL override, leading combining marks, over-long).
- **Capacity and truncation are TYPE properties**, not hand-edits: a nick type owning its capacity
  with no raw `resize`/`substr` in the API. The enumerated seven sinks were only the FIRST layer —
  `hud.cpp:49` recomposes via `snprintf` into `char line[64]`, a wrap row is `memcpy`d into
  `char buf[224]`, and both on-disk registries `snprintf` into fixed arrays. A list is blind to the
  next buffer; a type is not (OPUS §8).
- **Two constants, not one:** `kNickMaxChars` (codepoints — the display policy) and
  `kNickMaxBytes = kNickMaxChars * 4` (buffers + wire). A single byte cap would hand ASCII 20
  characters, Cyrillic 10 and CJK 6 — script-unfairness disguised as an invariant.
- **Width is NOT a truncation rule.** A width cap cuts at a different codepoint per viewer (family and
  scale are local), which would re-create the viewer-local identity problem. Identity lives in the
  stored codepoints; overflow is render-side, never changes the name, and any local ellipsis preserves
  the suffix. Truncation cuts only on a **grapheme boundary** — never before a combining mark, ZWJ or
  variation selector, never splitting a surrogate pair. Full UAX #29 is not vendored.
- **NFC has ONE normalizer: the ARBITER.** No normalization exists in the tree today, and
  `NormalizeString`'s Unicode tables differ across Windows versions, so normalizing on both ends could
  make two honest peers disagree about one name. ARC B already has the host canonicalize the name and
  hand it back, so only the host's tables matter. Entry-side normalization is demoted to a local
  nicety for MY OWN typed name and is explicitly NOT identity.

### 9b.5 D2 — the repertoire

- **BASE tier, embedded:** Latin + Cyrillic (measured present in all seven families) plus
  donor-supplied common hanzi, kana and single-codepoint COLR emoji, **merged into every
  (family × role) atlas**. The merge is *why* the tier is a build constant even though the family is a
  per-role user setting — the families' own cmaps are unequal, the donors' are not. **U+FFFD rides the
  same merge**, or six of seven families keep falling to `'?'`.
- **The acceptance predicate derives from the embedded donor blobs**, not from a hand-kept range list
  and not from the live atlas. A declared list and the real cmap are two owners of one axis; and the
  live atlas is machine-local in one branch (`fonts.cpp:208-221` falls back to a Windows system font
  if no RCDATA face bakes), which would silently change which names a machine accepts.
- **EXTENDED tier:** an optional pack, MTA's shape, delivered as a **release asset** and loaded by the
  existing `AddFromFile`. **Display-only, never identity** — nothing can own "your pack is the pack",
  so a truncated or substituted pack must not be able to change who you think someone is.
- **Two identity owners, two axes:** the version gate owns "peers run the same build"; a **CI
  staleness detector** owns "a build's identity actually covers its embedded fonts" (without it, a
  hand-built DLL with swapped fonts keeps its number and the first statement stops implying the
  second). The font set is NOT a wire axis.

### 9b.6 Rejection paths (out-of-repertoire input is WELL-FORMED, so fail-closed does not cover it)

| Entry point | Answer |
|---|---|
| My own name typed in the browser | Refuse in the field with a visible reason — the only place a human can correct it |
| `multivoid.ini` at boot, no host present | Placeholder + a visible warning; there is nobody to refuse to |
| A PEER's name off the wire | **Never break the join.** The host, already the canonical namer under ARC B, coerces it into the accepted repertoire at handback and falls back to the numbered placeholder if nothing survives. Kicking someone for their font settings is not a policy. |

### 9b.7 The five measurements that GATE the build

> **RUN 2026-07-28 — `votv-arc-d-gate-measurements-2026-07-28.md`.** Four are complete, the fifth
> (M1) is complete for the question it had to answer. Three claims in §9b.3/§9b.4/§9b.7 were
> FALSIFIED by the run and are struck below. Read the measurements doc before building any of arc D.
>
> **THE FORK RESOLVES TO ARM (A) — the donors ship on the PINNED ImGui 1.91.5.** An earlier
> revision of this stamp said arm (B) on a 64x/300x margin; that margin came from a probe which
> granted itself `RendererHasTextures`, a flag `imgui_impl_dx12.cpp:984` silently STRIPS on the
> legacy DX12 init we call. Arm B as it would actually ship buys none of it. The standing decision
> is `/qf` RF3: **1.92 is an OPTIMISATION gated on the descriptor-allocator migration, not a
> precondition for arc D2.** See the superseded-verdict box at the top of the measurements doc.



1. **The atlas drill.** Measure the **live** F1 path (`fonts.cpp:174-176` re-bakes the whole atlas on
   every scale/family change; DX12 tears the texture) over the **resident (family, px, bold) tuple
   set**, with `IMGUI_USE_WCHAR32` **ON**, and with **pack-present as an axis** (a pack-less drill
   would pass and then fail at the first user who installs one). Record texture bytes, rebuild ms,
   and **whether `ImFontAtlas::Build()` returns false / hits the texture-dimension limit** — that
   failure presents as a fontless UI, i.e. "the mod is broken", not "the atlas did not fit". This
   drill decides the remaining fork: **(A)** bake the base repertoire and pay the live-path rebuild vs
   **(B)** upgrade ImGui 1.91.5 → 1.92+ for on-demand glyphs.
   **DONE (offline) 2026-07-28 — resolves to (B).** 60 bakes over the real resident set (3 faces at
   default settings, not 5): arm A costs 16 MB / 139 ms at x1.0 and 64 MB / 416 ms at x2.0, paid on
   EVERY F1 scale/family change even when nobody types a hanzi; arm B costs 0.25 MB / 0.4 ms for a
   realistic load. **`Build()` never returned false** — the fontless-UI failure mode did not occur
   and is NOT the reason to reject (A). The in-game half was not run; it is owed only if (A) is
   revived.
2. **The 1.92 classified diff.** Arm (B) is UNPRICED until someone diffs the upgrade against our
   145 + 578-line DX11/DX12 overlay halves. Pricing it by category was the
   `[[lesson-price-a-dependency-by-repair-history-not-by-line-count]]` error, committed in R8 and
   named in R9.
   **DONE 2026-07-28: 11 call sites, 10 mechanical, ONE structural.** 1.92.8 still accepts our
   legacy `ImGui_ImplDX12_Init` signature but wraps it in an allocator that asserts on the SECOND
   simultaneous texture (`imgui_impl_dx12.cpp:894`), which a dynamic atlas needs — so the DX12 half
   must expose its existing `kTextureSlots` allocator as `SrvDescriptorAllocFn`/`FreeFn`. DX11 is
   unaffected. `ImWchar` is STILL 16-bit by default in 1.92.8: the upgrade does not subsume item 3.
3. ~~**The `ImWchar` census.**~~ `IMGUI_USE_WCHAR32` widens the type on every seam it crosses — the
   `ranges` plumbing, ~~both overlay halves~~, `imgui_impl_win32`'s `WM_CHAR` path. Vendored struct sizes
   are not the census.
   **DONE 2026-07-28 — and the "overlay halves" premise was FALSE.** The census is FOUR
   type-transparent `const ImWchar*` sites, all in `ui/fonts.cpp`; the overlay halves contain zero
   occurrences of `ImWchar` / `ImFont` / `ImFontAtlas` / `GetTexDataAs*`. The real constraint is one
   no round surfaced: `third_party/imgui` is a **git submodule**, so `imconfig.h` cannot be edited —
   the define must ride a `PUBLIC` compile definition on the `imgui` target, as
   `IMGUI_ENABLE_FREETYPE` already does (ODR-safe: `votv-coop` is its only consumer). The `WM_CHAR`
   path becomes CORRECT under the define, not broken.
4. **The entry ladder.** Rung 1: does default Win32 **clipboard paste** already deliver BMP CJK into
   the nick field (no clipboard override exists in our tree)? Only if it fails is rung 2 — IME
   composition over a window whose input we capture — a blocker. Plus `multivoid.ini`'s read encoding
   and its behaviour on a Notepad-written UTF-8 BOM. Without entry there is no feature: a Japanese
   player cannot type their own name.
   **DONE (static) 2026-07-28: rung 1 GREEN on the code path** — `imgui.cpp:14784-14805` is
   `CF_UNICODETEXT` → `WideCharToMultiByte(CP_UTF8)`, codepoint-transparent, and `InputText`'s 1.91.5
   storage is UTF-8, so the field is byte-transparent even at 16-bit `ImWchar`. **The in-game IME
   drill is still owed.** Two ini defects measured: the reader opens TEXT mode
   (`config.cpp:165` `_wfopen_s(…, L"r")`) and **no BOM handling exists anywhere** in `coop/config/`.
   Also: the §9b.4 widen census of TWO is really **seven** (six of the `wstring(begin,end)` idiom plus
   one written as a loop the idiom grep misses, `harness/session_runtime.cpp:380-384` — and this
   doc's `src/coop/harness/` path for it is stale; it lives at `src/harness/`).
5. **The donor files.** Name + license + measured bytes for the hanzi subset and the COLR emoji font,
   and **which hanzi set counts as "common"** — that choice now also defines which NAMES are accepted,
   so it is a product-visible boundary, not a size preference.
   **DONE 2026-07-28.** "Common" resolves to ImGui's own vendored sets, so it is a defined list, not
   a preference: CN-common = 3,349 codepoints, CN+JP union = 4,992. Measured subset bytes (fontTools,
   wght=400 instances): Noto Sans SC / CN-common (OFL 1.1) **1,037,496 B**; SC / union **1,583,628 B**;
   Twemoji Mozilla v0.7.0 single-codepoint emoji (MIT + CC-BY 4.0) **905,404 B** — and it is
   **COLR v0 + CPAL with no CBDT/sbix**, i.e. exactly what `FT_LOAD_COLOR` renders under
   `FT_DISABLE_PNG=ON`. Embedded tiers: CN+emoji ≈ **1.94 MB**, union+emoji ≈ **2.49 MB** (inside
   D-b's accepted budget).

### 9b.8 Ordering — D and B are ONE delivery

The pass started holding that arc D must land before arc B (B's fold key is defined over D's
alphabet). R19 found the dependency runs BOTH ways: D's single normalizer and its peer-name coercion
both rest on the host being the canonical namer, which is arc B. **They are two halves of one
mechanism and ship together.** Arc A (roster state + ID + the TAB ID column) is independent and can
land first.

### 9b.9 Also owed in the same commit

- The **five ASCII-asserting comments** (`roster.h:22-24`, `roster.cpp:26-27`, `moderation.cpp:42-43`,
  `player_inventory_sync.cpp:112`, `client_model.cpp:32`) become false at merge; `seen_players.cpp:48`
  makes it six. Only `SanitizeNickname`'s header is load-bearing (it documents real denials).
- **Disk migration.** `multivoid-banlist.txt` and `multivoid-players.txt` already hold nicks written
  THROUGH the Latin-1 widen being deleted. Their `|`-delimited formats are byte-transparent to UTF-8
  (`ban_list.cpp:65,71` — `CleanField` touches only `|`, `\n`, `\r`), so the format is fine and the
  stored ROWS are not. A read-side migration is owed, and the current `CleanField` must be read rather
  than the comment beside it.
- The `"Player"` fallback census stays split on its MY-NAME vs THEIRS axis; `peer_action_feed.cpp:53`
  prints a NAMELESS PEER using the MY-NAME literal — a pre-existing axis bug this commit must not
  cement. (`docs/LESSONS.md` cites `player_handshake.cpp:219` for the fallback; the code is at `:224`
  — stale citation, fix at the next sweep.)

### 9b.10 Accepted residuals (named, not hidden)

- `Anna` vs `Аnnа` (Cyrillic А) renders alike, is NFC-unequal, and gets no suffix — accepted by the
  user (D-e). UTS #39 confusable skeletons and peer certificates remain the filed future answer.
- Composed emoji (ZWJ families, skin tones, **flags**) will not compose — accepted (D-c). Fixing it
  means vendoring a shaping engine.
- Rare CJK is not usable in a NAME, only in chat (D-d).

---

## 10. Round-log highlights (what changed and why)

Kept because several of these were reversals, and a future reader should see the reasoning rather
than re-derive it.

| Round | Find | Outcome |
|---|---|---|
| R4 | The collision predicate keyed on mirror-element existence, but the nick store and the mirror install are NOT co-timed | Keyed on the store instead |
| R6 | Nick-as-EVENT needed a belt against enqueue loss | Reframed to host-canonical STATE; the new wire kind was deleted |
| R8 | The "display tuple" reframe would have made the host a fourth author of skin/colour/prefs | RETRACTED; boundary redrawn on ARBITRATION |
| R10 | Suffix-from-playerNo drifts names upward and the ask says 2/3/4 | Reverted to dense smallest-free |
| R12 | The collision set had silently lost the HOST | Host seeds row 0 |
| R16 | Guid-sticky reclaim rests on a peer-DECLARED value | Flip REVERTED; deferred to peer certificates |
| R19 | A client's TAB shows only two rows today; absence has no wire form | Arc split; presence encoded as `playerNo == 0` |
| R24 | A recycled slot goes X → Y with no zero between | `playerNo` promoted to the occupancy TOKEN |
| R26 | The token itself rested on the peer-minted `ownEpoch_` | Moved to a host-minted generation |
| R32 | The ban modal targets a snapshot-time slot index | Token in the API SIGNATURE, re-checked after the hop |
| R36 | Validating against the GT mirror only narrowed the window | Validate against the LIVE authority, atomically with the read |
| R39 | "Zero empties" would erase the host's own row every tick | Reconcile skips slot 0 on the host |
| R42 | — | "that holds" |

---

## 9c. ARC B + ARC D1 — AS-BUILT (2026-07-28)

**Commits `8592fb5e` (arc B) and `9ae83454` (arc D1). Proto 132, DLL `BC0285ADAECBCB12` deployed x4.
Every claim below is autonomous evidence — NO HUMAN HAS TOUCHED EITHER.**

### 9c.1 What shipped, against what the design said

| Design said | As built | Why it differs |
|---|---|---|
| B and D are ONE delivery (§9b.8) | B shipped first, D1 the same day | A `/qf` round held the ordering against the ask; B alone IS the literal ask, and D changes one function + one constant in it, not its authority structure |
| The arbiter keys on the nick STORE (R4) | Unchanged — it reads the ledger's occupied rows | — |
| The collision set needs occupancy-token scoping so a reconnect cannot ratchet | NOT built, and not needed | `ReconcileFromSession` already runs death FIRST and unconditionally (`roster_ledger.cpp:289`), so a reconnecting peer cannot collide with its own un-reaped row. Riding the ledger's existing guarantee beats adding a second one |
| A suffix is appended to a stem | The whole REQUESTED name is the stem | USER 2026-07-28. A kept `Pelmentor2` meeting another becomes `Pelmentor22`; stripping trailing digits would require deciding whether a number is a suffix we added or part of a chosen name, and nothing in the string says which |
| The handback is display-only; the ini keeps the REQUESTED name | The handback writes ALL THREE stores incl. `multivoid.ini` | USER 2026-07-28: "What user gets as a nickname gets recorded and persisted into his config files. It's not something temporary." |
| The widen census is 2 sites (§9b.4), later 7 | FOUR sites actually mattered, two of which no widen census could see | See 9c.3 |

### 9c.2 The invariant that made arc B one line instead of six

`AdoptCanonicalNickname` writes **`g_localNick`**, not the ledger row. Measured census: `LocalNickname()`
has SIX readers — `chat_sync.cpp:128` (chat authorship), `peer_action_feed.cpp:51`, `event_feed.cpp:136`
(the row-0 seed), `roster.cpp:73` and `:121` (the local row, which deliberately BYPASSES the ledger),
`player_handshake.cpp` (the Join payload) — plus 19 sites on the ledger-read verb
(`NicknameForSlot`/`DisplayName`) across 9 files. Writing the row alone would have left five surfaces
showing the name we asked for rather than the one we were given.

A claim this design carried and the code falsified: the per-tick `event_feed.cpp:136` re-seed was
believed to STOMP a ledger-written handback. It cannot — `EnsureRowZeroSeeded` (`roster_ledger.cpp:188`)
returns for every non-host before reaching the re-seed, and per R12 the host seeds row 0 first so the
host is never the suffixed peer. Right conclusion, wrong reason; the reason is now measured.

### 9c.3 Three defects the drills found (none was visible in code review)

1. **The host publishes a roster row BEFORE the joiner's Join arrives**, so the row's nick is empty,
   `SanitizeNickname("")` minted the display placeholder `"Player"`, and the joiner adopted the
   placeholder as its canonical name — and, after the persist decision, wrote it over the human's real
   name permanently. Measured: `nick: host renamed us 'Client1' -> 'Player'`. **Root fix:** an empty
   nick means NOT KNOWN YET; identity keeps it empty and the placeholder stays a display fallback the
   ledger already owns one copy of.
2. **`harness.cpp` narrowed the boot nickname with `c < 128 ? c : '?'`** — the site the widen census
   could never find, because the census was of WIDENS and this is a NARROW. Its effect was total and
   silent: a Cyrillic name read correctly from the ini became `????????` before the sanitizer or the
   wire saw it, so every downstream fix looked like it had not worked.
3. **`ReadEnv` used `GetEnvironmentVariableA`.** Windows holds the environment as UTF-16 and the
   A-variant converts down to the process ANSI codepage, so a Cyrillic env var arrived as cp1251 —
   not UTF-8, not well-formed UTF-8 either — and every correct layer above produced a row of U+FFFD.

Also caught, by the codec selftest, against our own build: the target compiled **without `/utf-8`**, so
MSVC decoded source literals with the SYSTEM codepage and the selftest counted three codepoints in a
two-character literal. Every non-ASCII literal in the tree was machine-dependent.

### 9c.4 Evidence

- `nickname-arbiter selftest: PASS (14/14)` and `utf8-codec selftest: PASS (17/17)` -- **these are the
  s2 numbers on the SUPERSEDED build; the current bytes print 27/27, see 9c.6** -- printed once per
  process on every peer. They cover what no LAN drill can stage: a suffix displacing stem characters at
  the cap, a variant colliding with a 20-character name a DIFFERENT player holds, the kept-name cases,
  the strict decoder refusing truncated / over-long / lone-continuation sequences, both caps landing on
  boundaries a raw `resize` would split.
- `nick_gate: PASS` — 746 files scanned, 7 carry the capacity vocabulary, and the detector matched its
  own known-bad fixture (the positive control the four-truncator census never had).
- 4-peer drill, all peers asking for the same name: boards photographed at
  `research/nickarb_shots/` for both `Pelmentor*` (ASCII) and `Пельмень*` (Cyrillic); the four
  `multivoid.ini` files read back the assigned names.
- `smoke4 PASS` on the shipped bytes. One intermediate FAIL was de-braided by changing exactly one
  variable: a 35 s monitor window is too short for three staggered clients; 60 s passes.

### 9c.6 The D1 residual — four narrows the widen census could not see (2026-07-28, same day)

**Arc D1 as first shipped (`9ae83454`) fixed encoding at ENTRY and left it unowned at EGRESS.** A
`/qf` critic round opened on arc D2 and its first question was not about fonts at all: it asked
whether `roster.cpp:29`, which writes a `char nick[24]` through
`WideCharToMultiByte(..., 23, ...)` under a comment asserting "peer nicknames are ASCII-sanitized
upstream", still returned 0 now that D1 had shipped a denylist and a CODEPOINT cap.

It did. **Measured** (python ctypes): `WideCharToMultiByte(CP_UTF8, cap=23)` on 19 Cyrillic
characters returns **0** with `ERROR_INSUFFICIENT_BUFFER` (122) — it does not truncate — so
`out[n]='\0'` with `n==0` stored the empty string. Cliffs: **12 Cyrillic / 8 hanzi / 6 emoji**.

The census by narrow verb, which D1 never ran, found four sites on the nick lane:

| site | mechanism | effect before |
|---|---|---|
| `roster.cpp:32` | `WCTMB` cap 23 | TAB row **blanked** |
| `moderation.cpp:47` | `WCTMB` cap 23 | ban record's nick **blanked** |
| **`nameplate.cpp:80`** | `c < 127 ? c : '?'` | the **floating nameplate squashed** to `????????` |
| `player_inventory_sync.cpp:114` | ASCII filter | name **dropped** from the record |

**The nameplate is the one that matters.** The originating request was literally *"просто чтобы у
всех был уникальный **Nameplate**"* — and the plate was the single surface where a Cyrillic name
still could not render. D1's status line above claimed `Пельмень / Пельмень2 / …` "in real
Cyrillic"; the photograph behind that claim is the **TAB board**, with no plate in frame, and
`Пельмень` is 8 characters / **16 bytes — under the cliff**. Every drill name passed by being
short. The evidence was real and proved the one path that already worked.

Fixed at the layer, not the sites: `coop::text::CopyUtf8ToBuffer` is the one egress, the five
`char nick[]` buffers are declared with `kNickBufBytes` instead of a literal 24, the three
"ASCII-sanitized upstream" comments are deleted (each was why the site under it looked correct),
and `nick_gate.ps1` grew three narrow detectors — each **injection-proven against the exact
retired code**. No proto bump: the wire nick was already `[uint8 nicklen][bytes]` at
`kNickMaxBytes`; every buffer here is a local snapshot row or a text-file field.

Two lessons, both about instruments rather than code:
`[[lesson-a-drill-that-stays-under-the-threshold-proves-the-wrong-half]]` and
`[[lesson-a-gate-on-one-verb-reads-as-a-gate-on-the-path]]`.

Also fixed this session, user-reported off the same drill screenshot: the plate centred the
composite `"<nick> (<ping>)"` on the anchor, so the NAME's centre sat left of the health bar's by
half the ping suffix — and would have slid sideways as the ping gained digits. The identity is now
centred and the annotation hangs off its right edge (`eb0bcdf5`; measured 0.0 px offset at 8x).

### 9c.5 What arc D2 still owes

CJK and emoji render as the fallback glyph until the donors are embedded. Everything needed is measured
(`votv-arc-d-gate-measurements-2026-07-28.md`): donor names, licences, subset bytes, the required
`LoadColor` flag, the merge-order hazard, and the fact that the atlas cost is 16-64 MB on the pinned
ImGui 1.91.5 versus 0.25 MB on 1.92. The open product boundary is unchanged: which hanzi set counts as
"common" decides which NAMES are accepted (D-d).
