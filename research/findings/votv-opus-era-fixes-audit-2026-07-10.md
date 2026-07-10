# Opus 4.8-era fixes audit (2026-07-10)

**Trigger:** user — "let's check previous bug fixes like rock — we did it with opus48, which can
make stupid mistakes... architectural mistakes of opus too."
**Method:** 4 parallel audit agents over the Opus-window commits (1e3c81f5 2026-07-06 →
4a085d82 2026-07-09), two lenses each (correctness + architecture: RULE-1 crutches, wrong layer,
authority direction, ANTI-SMEAR, RULE-2 baggage, folder placement, file sizes). The two most
severe findings were then SPOT-VERIFIED by the primary against the current tree (quoted below).
**Status:** AUDIT. Findings 1-2 primary-verified; the rest are agent-reported with quoted
load-bearing lines (each re-verify before fixing). HEAD at audit: 85849def.

---

## VERDICT SUMMARY
- **F2 rock drop-intent lane (556a91d7): DO NOT hands-on as-is** — findings 1+2 (verified) mean
  the main pickup path can author a false intent AND the lane's central race defense is
  unimplemented at the transport.
- serverbox_sync Inc-1: one HIGH teardown leak (breaker never restored).
- host-wipe v107: one HIGH latent (episode latch has no self-deadline on the SnapshotBegin flake).
- hand-item/pile v105-v106b: one HIGH latent (remote hand mirrors census-adoptable) + RULE-2
  retirements owed.
- email/peer-action + modularization/UI: solid; MEDIUM/LOW residue only.

---

## FINDINGS (ranked)

### 1. CRITICAL · rock drop-intent · PRIMARY-VERIFIED
`prop_drop_intent.cpp:107` — the hand-view-actor exclusion runs at ENQUEUE time only; `Tick()`'s
drain loop (gates: IsLiveByIndex / EidForActor / PeekIncomingSpawn / parked-key) never re-checks
`LocalHandActor()`. host_spawn_watcher.cpp:217-221 documents that at Finish-return `holding_actor`
is NOT yet written (why IT excludes at drain time, :383). A client hold-R pickup can therefore
enqueue the hand display husk; the husk carries the item's parked key (prop_drop_intent.h:25-28's
own model) → a FALSE PropDropIntent fires while the item is in hand → host spawns a duplicate
world prop, park consumed. **Fix: re-check `e.actor == LocalHandActor()` inside the drain loop.**
(lesson-reuse-proven-author-not-raw-reimpl instance.)

### 2. HIGH · rock drop-intent transport · PRIMARY-VERIFIED
`src/coop/net/session_lanes.h` — `LaneForKind` has NO case for PropDropIntent → `default: return
Lane::Normal;` (:95) while PropDestroy is pinned `Lane::Bulk` (:43). The design's v2-killer
defense (in-order husk-destroy-before-intent) requires same-lane FIFO; the file's own comment :39
states the cross-lane hazard. Under Bulk backpressure the intent can overtake the pickup-destroy
(place lost host-side) or the husk-destroy can land after the re-spawn (fresh copy retracted on
all peers). **Fix: pin `case ReliableKind::PropDropIntent: return Lane::Bulk;`** (ReliableKind
checklist item (d) was skipped).

### 3. HIGH · serverbox_sync teardown leak (agent-verified, quoted)
`serverbox_sync.cpp:321-327` — `KillLocalBreaker()` does `SetActorTickEnabled(obj,false)` (:228)
but `OnDisconnect` only resets `g_breakerKilled=false` — the ticker is NEVER re-enabled. A client
that disconnects and keeps playing solo (or re-hosts) has servers that never break again —
permanent SP-behavior leak; contradicts the event_fire_sync restore precedent one entry above in
the fanout. **Fix: track the neutralized breaker (ptr+live-idx) and restore tick in OnDisconnect.**

### 4. HIGH · host-wipe episode latch has no self-deadline (agent-verified, quoted)
`join_membership_sweep.cpp:587→646` — `NotifyQuiesced()` (the episode's only in-session clear) is
gated behind `if (!g_sweepPending) return;`. On the codebase's own documented SnapshotBegin-lost
flake (:474-476, covered for the reconcile queues but NOT the latch), `InEpisode()` sticks true
forever → ALL client keyed destroy broadcasts eaten for the session (prop_lifecycle.cpp:369-376)
→ silent world divergence + drop-intent lane dead. **Fix: bounded wall-clock watchdog independent
of g_sweepPending (or clear from quiescence_drain::OnTick).**

### 5. HIGH · remote hand mirrors census-adoptable (agent-verified, quoted)
`prop_element_tracker.cpp:590-596` — the census excludes only `LocalHandActor()`; a REMOTE peer's
display mirror (hand_item SpawnMirror = real Aprop_C, minting a NewGuid key) is adoptable by the
host's re-seed walk → phantom keyed prop broadcast to all peers + connect snapshot; its destroy is
echo-suppressed → permanent phantom. Latent (needs client holding a hotbar item across a host
census). **Fix: ONE boundary owner `hand_item::IsHandAxisActor()` (local + all g_mirrors) consulted
at census, FinishSpawn drain, drop-intent** — the same axis, one owner (ANTI-SMEAR).

### 6. MEDIUM cluster (agent-reported; verify before fixing)
- **email relay residue:** host still RELAYS a client EmailAppend (session_lanes.h:147 whitelist +
  no host-origin drop in OnReliable; serverbox has the parity check :307). One client-side
  regression re-opens the pollution vector. Fix: host-side drop of non-host EmailAppend + remove
  from relay whitelist + fix the stale lane comment.
- **serverbox client divergence unbounded:** edge-only apply, no parked-payload re-assert; the
  ≥5 surviving break/fix authors diverge silently until the next host edge; the one-shot connect
  snapshot is droppable with no recovery (`!Resolved()` window). Fix: park last payload +
  idempotent 1 Hz re-assert (subsumes both; Inc-2/3 ack'd in design docs).
- **peer-action announce attribution:** every local removal announces "You deleted" — a host-side
  game-sim delete misattributes to the host player; no bulk cap on the removed-branch (appends
  have kBulkAppendThreshold=32). Delete-verb census still open.
- **prop_drop_intent park set = volatile gate:** kMaxParked=64 FIFO eviction + Reset() wipe vs
  inventory that persists → an evicted/rejoined place silently reverts to the original F2 bug.
  Fix direction: derive from durable truth (host-side key-liveness already adjudicates); park as
  fast-path hint only.
- **join-edge GetActorLocation bulk scans:** 44127e2e throttled the 2 Hz cadence but kept two
  full ~2-3k-prop single-tick scans on the join edge (blob capture + firstRun flush). Chunk or
  reuse the capture.
- **echo-suppress mark leak:** hand-mirror MarkIncomingSpawn is never consumed (Init-POST never
  fires for BP-internal init) → set fills → clear-on-cap wipes in-flight marks (transient echo
  dupe); leaked stale mark at a recycled address + non-destructive Peek = permanent misclassify.
  Fix: consume at DestroyMirror / key by (ptr,InternalIndex) / oldest-eviction.
- **GHOST-RETIRE uncapped pass:** up to ~49% of live natives destroyable in one tick (violates
  the project's own pass-cap rule). Fix: pass-cap 40 + keep armed until drained.
- **freecam HOME/MMB ungated:** freecam.cpp:321 HOME toggle (a text-editing key!) lacks
  `!IsOverlayCapturingText()` — the exact lesson class ecadfeb7 shipped to close (it gated only
  MovementTick :215).
- **SEH handler doesn't unlatch text-capture:** imgui_overlay.cpp:442-455 — a fault in a surface
  render skips the SetOverlayCapturingText(false) publish and never closes chat_input → flag
  latched true → all gated hotkeys permanently dead + per-frame SEH spam. Fix: close chat_input +
  publish false in __except.
- **ini_config drifting to catch-all:** foreground-window check + overlay-capture atomic +
  ini parsing = three concepts in one session/ file. Extract an input-focus concept module.
- **TakeClumpBorn ignores stored InternalIndex** (trash_channel.cpp:270-277) — recycled-slot
  blindness; validate idx on consume.

### 7. LOW / RULE-2 / bookkeeping
- Retire `ExpressReleasedHandActor` (v106 "R-drop is not a spawn" model REFUTED by own RE; dead on
  the path it shipped for) once prop_drop_intent owns the drop; correct the header comment.
- [ROCK-DROP] diags (committed prop_lifecycle.cpp:198-211 + uncommitted WIP) assert the
  superseded "host will NOT see this" model — retire/re-word; WIP diffs verified log-only.
- hand_item Reset() doesn't null g_localPlayer/g_localPlayerIdx/g_session; kEmptyDebounceTicks=1
  machinery = RULE-2 dust; dropGrabObject dead thunk still alive.
- fonts.h Chat()/ChatPx() legacy aliases (2 callers) — migrate + delete; stale F1 help text +
  fonts.h header comment still document the dead global ui.font key; shutdown.h dead <atomic>.
- ToUtf8 duplicated (peer_action_feed vs chat_feed); topic.substr(0,80) can split a surrogate.
- email ShadowRow::topic per-poll wstring copy churn (~2k/s on big saves) — move-from or wchar[80].
- COOP_SYNC_MAP.md missing rows: serverbox_sync (ServerState=91), peer_action_feed; email row
  still describes pre-gate model.
- reflection.cpp 877 LOC past soft cap — proposal: split FProperty-walk family into
  reflection_props.cpp. prop_element_tracker/prop_lifecycle grew past-cap unflagged (extraction
  proposals: census family → prop_census.cpp; destroy seam → prop_destroy_seam.cpp).
- Memory >800 QUEUE stale: engine.cpp now 657, trash_collect_sync now 507 (extractions landed).

## Verified-clean highlights (suspicions refuted by the agents, kept for the record)
Modularization series byte-faithful (A1 DI has no fn-pointer risk; extractions multiset-diffed);
font atlas rebuild thread-correct + PushFont balanced; chat draw-list gates respected; F1 World
Rules layering proper; email delete apply cannot double-announce (synchronous shadow erase);
serverbox raw IsBroken write is the measured M2 apply, NOT a setter violation (verbs are
EX_Local + side-effect-entangled); v106b migration-first coherent; GHOST-RETIRE cannot destroy
peer-referenced actors; HandItem wire lane authority-correct.
