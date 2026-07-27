# Nickname arbitration + roster identity/ID — DESIGN (2026-07-27)

**STATUS: DESIGN, converged. NOT built, NOT smoked, NOT hands-on.**
42-round `/qf` pass; the critic returned "that holds" at R42. Round-by-round log:
`<scratchpad>/qf_thread.md` (session-local; the durable content is this document).

**The user's ask (2026-07-26 directive, verbatim intent):** duplicate nicknames must RESOLVE the
way other multiplayer games do — a joiner whose name is taken gets a forced numeric suffix, so
host Pelmentor + three client Pelmentors read as "Pelmentor (host), Pelmentor2, Pelmentor3,
Pelmentor4"; it must be ROBUST; it is needed for future `/commands` that target players by NAME
and by ID; and the TAB player list must show IDs. The fix itself is "в будущем" — this pass owed
only the design.

**Build green-light NOT given.** Four product questions (§9) are the user's to answer first.

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
7. Cosmetic follow-through: after (2) a client renders `ping`/`link` for slots 1-3 for the first
   time; they land on `rttMsForSlot() == -1` beside "VIA HOST" and need a deliberate presentation.

### Arc A drills
- A client sees all four rows; a departing third peer disappears from a client's TAB.
- Third peer leaves, fourth takes its slot, the departure row is INJECTED-LOST → the receiver sees
  the token change, performs death+birth, renders the new person with no ghost frame.
- The successor has SILENCE on the voice channel and their OWN inventory.
- A departed peer's bubble / colour / plate / hand state is cleared ON A CLIENT.
- Steady-state pulse: zero engine calls, no toast.
- A version-gate-rejected joiner never appears on any client.
- A doomed never-ready connect emits no toast and creates no row.
- Join during a save-transfer burst with row loss injected → full roster within the adaptive window.
- Ban-modal successor drill: open the modal, let the target leave, let a new peer take the slot,
  press Ban → the action ABORTS and the newcomer is NOT banned.
- Accept-ordering drill: successor accepted before the GT tick → Ban aborts.
- Session-end counting drill: three peers → exactly THREE transitions per subscriber, no menu toast,
  next session starts with all migrated stores empty.
- A row arriving before the `LocalPeerId` stamp is parked and applied correctly.

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

## 9. PRODUCT QUESTIONS — the user's to answer before any build

1. **Literal "(host)" suffix?** The example reads "Pelmentor (host)". The scoreboard already conveys
   the role by gold nick colour and the Link column ("LAN HOST"/"P2P HOST"). Is a literal suffix
   wanted, or is the existing role marker enough?
2. **Name drift.** With no guid reclaim (§4), four friends reconnecting through a long session drift
   upward ("Pelmentor7" in a lobby of four). Acceptable as cosmetic until peer certificates make a
   safe reclaim possible?
3. **Cyrillic.** `SanitizeNickname` STRIPS non-ASCII (`:199-203`), so "Пельментор" collapses to the
   "Player" fallback — a Russian-speaking lobby becomes Player/Player2/Player3, and arbitration makes
   that long-standing gap visible. Widen the alphabet? **Measured cost:** `NarrowNick` caps at 23
   bytes and turns failure into an EMPTY string (`roster.cpp:28-35`), and Cyrillic is 2 bytes/char in
   UTF-8, so a 12+ character name would render BLANK, not truncated. Widening therefore also
   requires widening the display buffers and fixing that failure mode — a small separate piece of
   work with its own drill, not a one-line condition change.
4. **Suffix vs ID divergence.** They coincide in a fresh lobby (Pelmentor2 = #2) but diverge after
   churn ("#7 Pelmentor3"). Internally harmless (the suffix is never parsed), but user-visible.
   Keep as-is (stable ID + short names), or derive the suffix from the ID (they always agree, but
   names grow)?

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
