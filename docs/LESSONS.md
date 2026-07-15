# LESSONS — what VOTV_MP has learned the hard way

The single browsable ledger of durable lessons + **DIG-RULE** lessons. Each row is a takeaway, a
"look here FIRST next time" pointer where one applies, and a link to the full `memory/` file (the
authoritative detail). This complements — does not replace — `MEMORY.md` (the terse auto-memory index
loaded each session): `MEMORY.md` is the machine index; this is the human-readable, categorized digest.

**Maintained by `/documentize`** (Step 3.5): every sweep ADDS the session's new lessons and RECONCILES
existing rows for staleness (a row whose cited symbol/path moved is fixed or archived — a lesson pointing
at a dead symbol sends the next session on a worse dig than no lesson at all). A lesson earns a row only
if it saves a FUTURE dig.

> Links use the `memory/` slug: `memory/<slug>.md`.

---

## 0. The DIG-RULE (why this file exists)

**When a dig produces a hard-won measured fact, record it as a durable lesson so a future session reads it
instead of re-excavating the same hole.** Born because the project dug the same place twice (rock F2,
2026-07-08/09) and the user made it a rule. Two faces:

- **MAP-ALL-WIRE-EVENTS** — when a synced entity does not APPEAR / STICK / flickers / resets on a peer,
  MAP the full set of wire events ONE action emits (both peers, tick order) BEFORE building any fix; a
  later same-key event can silently UNDO an earlier one. `memory/feedback_map_all_wire_events_before_fixing_missing_sync.md`
- **PROBE-DON'T-GUESS** — MEASURE the root with a read-only probe before building a fix; a design stays
  provisional until the probe discriminates it. `memory/feedback_probe_dont_guess_rule.md`
- **VERIFY, don't re-derive** — recalled memory reflects what was true WHEN WRITTEN; verify a cited
  file/function/offset/flag still exists before recommending it (this is why staleness reconciliation is
  part of every sweep).

---

## 1. How to work (process / working agreements)

- **Run `/qf` (up to 15 rounds) BEFORE any non-trivial implementation + when planning new changes** —
  default to it; the adversarial pass is where crutches/wrong-layer/un-measured-assumption get caught
  before cementing. `memory/feedback_qf_before_implementation.md`
- **The /qf critic has a SELECTIVE-TRUST blind spot** — it interrogates claims individually + accepts the
  primary's answers as settled, so "trust a source for X, distrust it for Y" slips a whole pass. Skill
  patched 2026-07-13 (source-consistency / cross-answer / undone-measurement angles); still check manually.
  `memory/feedback_qf_selective_trust_blindspot.md`
- **The /qf critic escalates WITHIN THE FRAME it is handed** — so a MIGRATION design (repoint/rebind/re-key)
  that migrates the ONE identity map the brief names, while a PARALLEL map keyed on the same entity finalizes
  late, slips a whole multi-round pass (11 rounds + a "that holds" missed the host-only KerfurId table). When
  a design migrates identity, ENUMERATE every map that keys on the entity + prove the op updates or gates ALL
  of them. Skill patched 2026-07-13 nite (IDENTITY-MAP-COMPLETENESS angle + brief-enumeration + convergence
  bar). `memory/feedback_qf_enumerate_identity_maps_on_migration.md`
- **The /qf critic inherits the primary's BRIEF as ground truth (blind-spot #3: CARRIED-FRAMING)** — the
  fresh-per-round critic sees only what the primary wrote, and the primary writes its own brief, so a
  load-bearing NOUN it introduced as an inference and hardened by repetition ("the existing two-phase arm
  record" — actually FOUR distinct converge mechanisms) launders into an apparent fact every fresh critic
  reads blind. Worst in `/qf N` auto-loop (N self-summarized briefs, no external check). Fix: tag facts by
  PROVENANCE (measured-artifact vs carried-framing) + code-verify the 1-2 nouns the design hangs on before
  convergence + SURFACE to the user after any material REFRAME instead of auto-continuing (the user, holding
  the real history + raw artifacts, is the only party who catches framing drift, retroactive-foundation-
  invalidation, and cross-artifact synthesis). Skill patched 2026-07-14 (FRAMING-PROVENANCE angle + brief
  provenance-tag + reframe-surface + carried-primitive convergence bar).
  `memory/feedback_qf_challenge_carried_framing_not_just_the_frame.md`
- **A NEGATIVE grep is only evidence if the pattern can match a KNOWN-POSITIVE line** — before concluding
  "0 matches -> never happens / mechanism dead / gate clean", prove the pattern CAN match the positive case
  (grep one real hit; check the log line even CONTAINS the field you filter on). A query structurally blind
  to its target returns 0 and the null reads as PROOF. Worst kind: not a case that never arose, but one that
  arose EVERY time and was invisible to the query. Cost 2026-07-14: `grep 'grab_hook\[destroy-seam\].*kerfur'`
  =0 "proved" the destroy-seam never fires for kerfur (the line prints actor/key/eid, NO class) -> declared
  `TryCaptureKerfurPropDestroy` dead -> nearly RULE-2-deleted the guard sitting on bug1's actual relay.
  Corollary: when ONE negative-grep turns out blind, RE-RUN the audit on every other "0 fires" in the
  inventory. `memory/lesson_negative_grep_verify_against_known_positive.md`
- **Before changing a FUNCTION's behavior, enumerate ALL its call sites + state what each expects; before
  SUBTRACTING an output at a seam, enumerate every other producer/consumer at that seam** — acting on an
  incomplete map of what you're touching is ONE recurring root with many faces (a "mechanism" that is N
  mechanisms; a converge fn with an unenumerated 3rd/4th caller; a "suppressor" that is 3 coordinating
  broadcasters; a proxy criterion that only correlates with the real fact). A subtraction breaks unenumerated
  consumers SILENTLY (no error). Prefer the DIRECT fact over a PROXY. Cost 2026-07-14: captured-B wired into
  `ConvergeAfterConversion` without mapping its 4 callers (the POLL death-watch was the one that duped); fixed
  by enumerating the seam's 3 PropSpawn broadcasters FIRST -> "track-but-don't-broadcast" (remove the output,
  keep the tracked-flag contract the others coordinate on). `memory/feedback_enumerate_call_sites_before_changing_behavior.md`
- **"per rule 1" = full green light** for the root-cause fix in its complete form (incl. hard
  architectural change). Don't scope down, don't ask "is this too big". `memory/feedback_no_crutch_questions_act_autonomously.md`
- **No design/architect AGENTS** — design yourself from code + docs + MTA; search + audit agents OK. `memory/feedback_no_design_architect_agents.md`
- **Claude OWNS every mechanical chore** (ini flags, grep/log-read, build, deploy) — never hand them to the
  user; the user does only in-game actions. `memory/feedback_claude_owns_all_mechanical_chores.md`
- **User ON the PC = USER tests**; Claude launches only user-AWAY + green-lit. `memory/feedback_user_tests_claude_prepares_ground.md`
- **Ask in PLAIN TEXT, never the AskUserQuestion UI.** `memory/feedback_ask_in_text_not_question_ui.md`
- **Never assert a VOTV game-domain fact from assumption** — verify vs SDK/bp_reflect/wiki FIRST. `memory/feedback_verify_game_domain_facts.md`
- **RE all related blueprints STATICALLY before any runtime probe.** `memory/feedback_re_blueprints_before_probes.md`
- **Commit autonomously at verified checkpoints; still ASK before PUSH.** `memory/feedback_commit_autonomously.md`
- **Never retire a load-bearing fix on an unverified theory.** `memory/feedback_verify_before_retiring_a_fix.md`
- **SAME bug after 2+ targeted fixes = the patch LEVEL is wrong; the root is architectural** — stop patching, re-root. `memory/feedback_recurring_bug_is_architectural.md`
- **A cross-cutting axis has ONE owner** — handlers CAPTURE, never apply (anti-smear). `memory/feedback_one_owner_order_axis.md`
- **Fix a mirror-identity race WORKING first, generalize only after N>=3.** `memory/feedback_fix_then_generalize_mirror_identity.md`
- **Every source FOLDER = ONE domain concept; no catch-all names.** `memory/feedback_folder_per_domain_concept_rule.md`
- **RULE 2 does NOT apply to probes/diagnostics/tools** (they may stay). `memory/feedback_rule2_exempts_probes_diagnostics_tools.md`
- **Test/probe flags live in `votv-coop.ini [dev]`, NOT bats/env.** `memory/feedback_test_flags_in_ini_not_bats_or_env.md`
- **`docs/piles/` is the LIVING pile KB** — mark DESIGN vs AS-BUILT vs VERIFIED. `memory/feedback_docs_piles_living_knowledge_base.md`
- **A diagnostic probe's built-in comparability/quiescent tag is only as good as its DERIVED inputs** —
  validate EACH gate input against the codebase's MEASURED field-behavior before trusting the tag; a wrong
  input silently mislabels samples (a clean diff on mislabeled data is worse than none). Two inputs broke
  the desk_diag `q=Y` tag (game-jittered knobs → always-N; unchecked active-filter integration → q=Y
  mid-ramp). Prefer clean DISCRETE state over float-delta heuristics; first check the log = do `q=Y`
  samples cluster at the real pauses. `memory/lesson_comparability_tag_inputs_need_measured_validation.md`

## 2. Join-window identity & the DUP-prone zone (measure before touching)

- **A client grab/drop of a host-owned keyed prop = a MOVE with TWO same-key halves** (SPAWN author + a
  co-fired grab-hook DESTROY); the DESTROY is usually the killer. *Look FIRST:* the destroy-seam, not the
  spawn author. `memory/lesson_client_keyed_prop_move_two_wire_halves.md`
- **A host OnSpawn log line != VISIBLE** — check the immediate same-tick OnDestroy (destroy-by-key kills
  the newest). `memory/lesson_onspawn_log_not_proof_check_immediate_destroy.md`
- **REUSE the proven author (with its gates), don't raw-reimplement** — a raw MarkPropElement broke the
  E-grab decline. `memory/lesson_reuse_proven_author_not_raw_reimpl.md`
- **A join reconcile that DESTROYS local actors needs a quiescence gate + caps.** `memory/feedback_join_reconcile_sweep_safety.md`
- **An op applied BEFORE the state it reads is ready recurs** — gate/defer (snapshot-before-state-ready). `memory/feedback_snapshot_before_state_ready.md`
- **chipPiles persist in `primitivesData`; off-kerfurs in `objectsData`** (different save lanes). `memory/lesson_chippile_saved_in_primitivesData_not_objectsData.md`
- **DELIVERY-axis: join DELIVERY vs IDENTITY are separate; ONE owner = `prop_snapshot`.** `memory/feedback_deliver_missing_owner_delivery_axis.md`
- **Check the EXISTING barrier's ANCHOR before building compensation layers** — the two-authority
  join seam (4 roots, 4 layers, 3 days) was ONE mis-anchored edge: the v56 gate/replay WAS the MTA
  join barrier, but ClientWorldReady fired at "world up" (seconds before the loadObjects tail
  settled). *Look FIRST* on a window/race class's SECOND compensation layer: does a READY edge
  exist, what PREDICATE fires it ("world up" != "world settled"), is a stronger client-local
  signal already trusted elsewhere (the doom sweep's probe was). Moving an edge beats compensating
  for it. `memory/lesson_check_existing_barrier_anchor_before_compensating.md`
- **Harness TimelineThread call sites: an Arm() stays ATOMICS-ONLY (or Post to the GT)** —
  DriveMenuModeJoinWorldBoot runs OFF the game thread (harness.cpp:285-288); extending a directly-
  called Arm to touch plain GT-owned state is a silent data race (2026-07-12 audit CRITICAL: 8
  fields + a std::string; fixed `7847021e` via an atomic request flag consumed by the GT ticker +
  UE_ASSERT_GAME_THREAD on GT-only entries). *Look FIRST:* the caller's thread — grep the enclosing
  harness function for TimelineThread comments / Post-and-wait siblings.
  `memory/lesson_harness_timeline_thread_arm_sites_atomics_only.md`
- **In-episode wire expressions WERE provisional — the JOIN BARRIER removed the window (2026-07-12
  `bbf91f39`, SUPERSEDED AT SOURCE):** ClientWorldReady now announces at load-tail quiescence
  (`coop/session/world_load_episode` probe latch — the MTA INITIAL_DATA_STREAM shape), so no wire
  prop expression can arrive mid-churn; the capture/revalidation/netting machinery (takes 1-4) was
  RULE-2 deleted. The measured record (fresh mirrors churn-killed ~2 s; converge targets recreated
  only from save-WORLD records; phase replay inverting a destroy->spawn pair; doom judges LAST)
  stays the FIRST read for a prop bug in the DEGRADED mode ("latching DEGRADED" in the client log)
  or the TRAVEL window (no travel-start gate yet). *Look FIRST:* client log — "load-tail QUIESCED"
  must precede "ClientWorldReady announced"; NO [SPAWN-DEFER] lines exist anymore (one appearing =
  someone resurrected the dead machinery); per-doom cls/key/loc lines + the dead-row tripwire still
  live in the sweep. `memory/lesson_join_window_wire_expression_provisional.md`
- **VOTV's OWN save ships DUPLICATE interactable Keys** (85 trashBitsPile_C across 4 keys — save-born
  clone families; the 06-24 sweep silently doomed "80 trashBitsPile" for weeks). Key uniqueness is OUR
  invariant: the HOST re-keys duplicates at enroll (MarkPropElement, the one owner; GT-gated setKey;
  dead incumbent = churn recreate inheriting identity). Take-4: the `2fefd161` re-key was INERT (162x
  "setKey not found" — trashBitsPile is actor_save_C lineage; repaired by the SuperStruct-climbing
  resolver, `460da7e4`). *Look FIRST:* host-log "KEY-UNIQUENESS ... re-keyed -> 'rk_'" SUCCESS burst
  ("re-key FAILED" = the authority is NOT working); same-key multiplicity histogram in the adopt burst.
  `memory/lesson_votv_save_ships_duplicate_interactable_keys.md`
- **MirrorManager\<Prop\> MIXES census LOCAL rows with wire rows (one actor can carry BOTH)** — an
  actor->eid reverse meaning "established cross-peer identity" must filter `IsMirror()`
  (`ResolveMirrorEidByActor(wireMirrorOnly)`), else it kills the Gap-I-1 divergent-key dedup.
  *Look FIRST:* mirror_manager.h "MIXES" block. `memory/lesson_prop_mirror_manager_mixes_local_and_wire_rows.md`
- **A NEW generic catch/express lane must inherit EVERY existing owner boundary; "UNTRACKED = mine"
  premises die when per-tick claimers ship** — spawn_authority Inc-1's seam drain (07-10) lacked the
  kerfur OWNER BOUNDARY the census lane already carried → the 5 Hz kerfur converge lost every race,
  silently released the dead NPC, no KerfurConvert → the take-8 five-for-five toggle dupe. Fix = kerfur
  FIRST REFUSAL at the express chokepoints (TryAdoptFreshKerfurProp: UNTRACKED + dead-NPC-watch match,
  event-driven at the spawn edge; the poll stays as backstop), `ded3f793`. *Look FIRST:* a poll-class
  "converge found nothing" WARN right after a `spawn-seam adopted`/generic express of the same
  class+position = the race; when adding a lane, DIFF its gates against every sibling lane.
  `memory/lesson_new_generic_lane_must_inherit_owner_boundaries.md`
- **ChildActorComponent children are OUTSIDE the world-object universe** — a kerfur eye cam
  (prop_camera_good_C) passes every "world prop" filter (keyed, Aprop lineage, live) but the game's
  own rule is `Aprop_C::ignoreSave = ignoreSav || IsChildActor()` (prop_base bytecode): its Key is
  per-peer random, cross-peer identity impossible in principle. Enrolling/broadcasting them = floating
  CCTV mirrors on the joiner + the joiner's own eye cams doomed (take-7). And a SEND-side exclusion
  must gate EVERY payload builder — the steady re-seed express bypassed enrollment (elementId=0 keyed
  payloads) while the gated destroy seam made its orphans PERMANENT (audit CRITICAL). *Look FIRST:*
  `ue_wrap::engine::IsChildActor` (six consult surfaces, `c93617be`); any NEW prop enumeration must
  consult it. `memory/lesson_child_actors_excluded_from_world_object_universe.md`
- **Identity-critical log lines carry cls+key+loc (USER RULE)** — a class histogram alone makes
  per-entity RCA impossible; cold paths only, never at the POST-native destroy seam (PendingKill),
  throttle mass arms. `memory/feedback_identity_logs_carry_key_and_loc.md`

## 3. Sync architecture (owners, routers, lifecycle)

- **Follow MTA architecture when possible** (vendored `reference/mtasa-blue/`). `memory/feedback_follow_mta_architecture.md`
- **A new `ReliableKind` wires in THREE places** — check the router checklist. `memory/feedback_reliablekind_router_checklist.md`
- **Host TRACKING/enroll gates on HOSTING, never `connected()`.** `memory/lesson_tracking_gates_on_hosting_not_connected.md`
- **EVERY session-end path runs the FULL teardown fanout** — AND session-scoped UI (chat feed/input/
  bubbles/nameplate/voice_panel) dies at the FLEE funnel (`FleeToMainMenu`), NOT `DisconnectAll` (which
  also runs on the HOST keeping its world, so it must not clear on-screen UI). *Look FIRST:* net_pump.cpp:184
  (chat-leak-into-menu, 2026-07-15). `memory/lesson_every_session_end_path_full_teardown_fanout.md`
- **Every client-side SUPPRESSION is a LOAN, not a purchase (N=3: weather 06-11, serverbox 07-09,
  garbage_sync 07-10).** Persistent-state neutralizations (tick-disable, field-zero, TimeScale=0,
  suppress flags) need an EXPLICIT OnDisconnect restore; fn-body PRE-cancels SELF-restore ONLY when
  gated on `s->running()`/`connected()` — a bare `role()==Client` gate keeps suppressing in SOLO play
  forever (Stop never resets cfg_.role). *Look FIRST:* name the restore mechanism in the SAME commit;
  census: grep bare role-gates without running(). `memory/lesson_suppression_needs_paired_restore_or_running_gate.md`
- **Pre-world subsystems Install at StartCoopSession, NOT world-gated.** `memory/feedback_preworld_install_at_startcoopsession.md`
- **When a release VERB can't be caught, STREAM THROUGH the state.** `memory/lesson_stream_through_release_not_verb.md`
- **An e2e assert must DISCRIMINATE the axis it claims.** `memory/lesson_e2e_assert_must_discriminate_the_axis.md`
- **The join-window PropSnapPos POSITION reconcile is eid-generic at the receiver** — a new
  save-authoritative pos reconcile is SEND-SIDE ONLY (capture baseline + flush); the chip overlay
  auto-skips a non-chip eid, so no dup. *Look FIRST:* `FlushDivergedSavePositionsForSlot` +
  `UpdateChipHostPos`. `memory/lesson_pos_reconcile_generalizes_via_generic_receiver.md`
- (Mirror STATE, not the verb — not because the verb is invisible (the GNatives substrate can now see EX_Local*), but because state-mirroring is convergent, path-agnostic, and handles the client's autonomous mutator. Verb brackets are for identity-flip / intent-attribution, where the fact you need exists only inside the verb's execution window.)**To sync a VOTV world SYSTEM (servers/alarm/…), mirror the STATE + drive the notify-free re-applier
  from the host — NEVER intercept the mutating verb** (breakServer/runTrigger are `EX_Local*` invisible).
  Poll the state field 1 Hz → broadcast on change → client raw-writes + reflected `check()`/`runTrigger`;
  client neutralizes its own autonomous mutator (disable ticker tick / zero the data array). *Look FIRST:*
  `coop/world/alarm_sync.cpp` (one instance) + `coop/interactables/serverbox_sync.cpp` (an array).
  `memory/lesson_votv_world_system_sync_mirror_state_not_verb.md`
- **`coop/world/email_sync` is PEER-SYMMETRIC** (each peer forwards its OWN new inbox rows) — so a client's
  FALSE self-authored email/notice is broadcast to the host + all peers = permanent SHARED-inbox pollution,
  not a cosmetic flash. Before designing a "hide a client's wrong notice" fix, check the channel's
  direction. `memory/lesson_email_sync_peer_symmetric_client_false_notice_pollutes_shared.md`
- **`server` naming/placement:** a signal-SERVER sync goes with its signal siblings in
  `coop/interactables/` (signal_sync/console_state_sync), named after the engine class (`serverBox_C` →
  `serverbox_sync`) — NOT `coop/world/` (a 14-file catch-all), and NOT "server_sync" (ambiguous with the
  NETWORK server that saturates this mod). Instance of `memory/feedback_folder_per_domain_concept_rule.md`.
- **VOTV shared-world RNG concentrates in 2 directors (`daynightCycle`/`mainGamemode`) + ~30 `ticker_*`/
  event spawners + signal/server/loot rollers — host-ownable via mirror-step-3, but our `npc_sync`
  suppress is an ALLOWLIST (15 of ~40 spawn classes) so it inherently lags** → the rule-1 root is
  STRUCTURAL (client runs NO world-spawn ticker; allowlist = MIRROR set only). Only 3 systems seed a
  `RandomStream` (garbagePileSpawner/radiotower/xmaslight) → seed-replicate; all else unseeded → suppress
  or intent. Every gap row is STATIC-INFERRED → run a LIVE client-roll probe before any fix. *Look FIRST:*
  `docs/COOP_RNG_AUTHORITY.md` (living tracker) + `memory/lesson_votv_rng_host_ownable_at_ticker_director_layer.md`.
- **RNG IN a per-peer sim's RATE/output formula = a MECHANIC desync, not a display bug** — mirroring the
  output chases the divergence forever (the client re-sims with its own RNG between ticks); the host must
  own the SIM and roll the RNG, client SUPPRESSES its tick. Found only by reading the RATE block
  byte-by-byte (desk `DL_downloading @66736`: `RandomFloatInRange` needle + `RandomFloat` noise sit IN the
  rate). A "numbers differ between peers" report on anything that ACCUMULATES → read the rate, don't
  output-mirror. *Look FIRST:* `research/findings/computers-devices/votv-desk-download-machine-RE-2026-07-15.md`.
  `memory/lesson_rng_in_rate_path_is_mechanic_desync.md`

- **Classify an ambient spawner's tier by its ANCHOR read** (minutes in the dump): player-camera source
  → OWNER-EFFECT; absolute float coords → world host-auth; navmesh random-walk var → world roamer; a
  PRODUCT that stalks the local player → OWNER-ENTITY. Two wrong name-and-vibes calls reversed in one
  day (pinecone wrongly suppressed; sky wisps wrongly per-peer). *Look FIRST:*
  `research/findings/world-systems/votv-ambient-anchor-audit-RE-2026-07-10.md` + the tier table in spawn_authority.h.
  `memory/lesson_ambient_spawner_anchor_read_decides_tier.md`
- **Peer-keyed mirror lanes have 3 measured traps** (owner_entity_sync audit): a CLIENT has no transport
  edge for another client's slot → leaver teardown must be HOST-FANNED; your own mirror spawn re-enters
  every BeginDeferred hook → ScopedMirrorSpawn-exclude EVERYWHERE incl. the rng census; collision must
  drop INSIDE the deferred window (BeginPlay overlap runs during Finish). *Look FIRST:*
  `coop/creatures/owner_entity_sync.cpp` (reference impl). `memory/lesson_peer_keyed_mirror_lane_traps.md`

- **Continuously-MOVING display state needs an unreliable pose-rate stream, not reliable snapshots** —
  the hand-item swing rendered at "1 fps" under a 0.5 s drift-gated reliable resend; split identity
  (reliable announce) from motion (MsgType::HandPose=35, RagdollPose plumbing end-to-end).
  *Look FIRST:* `memory/lesson_continuous_motion_needs_pose_stream.md` (v109 `a3c55529`).
- **The client join world-load episode now guards TWO consumers** — the v106 keyed-destroy broadcast
  suppression AND the email shadow diff (2026-07-11 `848a1fc0`: priming/diffing saveSlot.emails across
  the client's own load mis-read 2 swapped default rows as player deletes → EmailDelete broadcast →
  host rows deleted). Any poll-diff over save-backed state must gate on `world_load_episode::InEpisode()`.
  `src/coop/world/email_sync.cpp` + `coop/session/world_load_episode.h`.

## 4. Dispatch, hooks & input seams

- **BP INNER calls (`EX_CallMath`/`EX_*`) BYPASS ProcessEvent** — a PE hook won't fire. THIRD instance
  2026-07-10: the T1 probe's PE-table interceptors on `Delay`/`K2_SetTimer*`/`SetActorTickInterval`/`QuitGame`
  were BLIND for a whole smoke (caught by its own positive control; moved to the Func-patch seam `7109efd1`).
  BONUS: a Func-patch POST hook's `sourceObject = FFrame::Object` = the CALLING BP actor — free per-caller
  attribution, no param stepping. FOURTH instance 2026-07-10 eve (the INVERSE trap): the STATIC dump
  `$type` cannot PREDICT visibility either way — garbagePile/pinecone read `EX_CallMath` yet measurably
  FIRE the PE POST; chipPile reads the same and doesn't. Only a live catch classifies a caller.
  *Look FIRST:* the dispatch map's MECHANISM row + its live-catch evidence, never the dump alone.
  `memory/lesson_ex_callmath_invisible_to_processevent.md`
- **`R::FindFunction(cls, name)` is EXACT-OWNER — no SuperStruct climb**: a parent-class UFunction
  (AActor::SetLifeSpan) looked up on a BP leaf returns NULL every call + pays a futile full-array walk
  (audit CRITICAL 2026-07-10: the ambient-mirror lifespan backstop was silently dead). SECOND STRIKE
  2026-07-11: BOTH spawn-by-key sites resolved `setKey` on the LEAF wire class → prop_crowbar_C mirrors
  spawned keyless → field key diverged from the wire binding → pickup-destroys missed the host = the
  host-side crowbar DUPE (rocks masked it: a rock IS prop_C). THIRD STRIKE 2026-07-12: the take-3
  KEY-UNIQUENESS re-key silently no-opped — trashBitsPile_C's setKey lives on actor_save_C, outside the
  hardcoded Aprop_C fallback; fixed by `R::SuperStructOf` + a chain-climbing ResolveSetKeyFn
  (`460da7e4`) — reuse that resolver shape. Resolve on the DECLARING class + cache;
  when adding any reflected-call site, grep for other leaf-class resolves of the same fn.
  *Look FIRST:* the SDK header for which class declares the fn + the RCA finding
  `research/findings/props-lifecycle/votv-crowbar-mirror-key-divergence-RCA-2026-07-11.md`.
  `memory/lesson_findfunction_exact_owner_no_superstruct_climb.md`
- **VOTV damage NEVER touches UE TakeDamage/ApplyDamage** — melee = `mainPlayer.attack` →
  per-class `addDamage`/`damageByPlayer`, ALL EX_Local-invisible inward from `attack`; the ONE
  Func-patchable choke is `VictoryFloatMinusEquals` (every prop+creature health write; FFrame::Object =
  target). A client's hits are LOCAL-ONLY today (user live 2026-07-11: zero damage cross-peer, silent
  crowbar door hits). The mannequin is a PROP (`Aprop_mannequin_C : Aprop_C`), not a Character.
  *Look FIRST:* `research/findings/player-puppet/votv-melee-damage-path-RE-2026-07-11.md` (chain + ranked hook seams).
  `memory/lesson_votv_damage_bypasses_ue_takedamage.md`
- **A SCRIPT-fn called via `EX_Local*` is invisible to BOTH the PE hook AND the Func-patch** — patch the
  NATIVE calls inside it. **Boundary sharpened 2026-07-13: this is THE ONLY remaining invisible class,
  and it's SOLVABLE (GNatives swap = a third hook primitive); EX_CallMath was NEVER part of the wall
  (native targets = Func-patchable). Check the CALLEE's nativeness before declaring a wall.**
  **Spike-measured 2026-07-13:** `GNatives_table`@`0x144D8ECD0`; LocalVirtual=op 0x45@`0x1414751A0`
  (12-byte FScriptName operand), LocalFinal=op 0x46@`0x141474FB0` (8-byte UFunction*). **0x45 IS the
  kerfur flip opener — LIVE-CONFIRMED [V] hands-on (STEP 1.0 v3, 2026-07-13): `dropKerfurProp`
  (Context=`kerfurOmega_C`) / `spawnKerfuro` (Context=`prop_kerfurOmega_C`) both fire via 0x45 on both
  peers.** *Look FIRST:* `docs/COOP_VM_DISPATCH_PLAN.md` +
  `research/findings/world-systems/votv-vm-dispatch-RE-2026-07-13.md`.
  `memory/lesson_script_fn_invisible_to_func_patch.md`
- **The `EX_LocalVirtualFunction` (0x45) operand is a 12-byte FScriptName `{ComparisonIndex@0,
  DisplayIndex@4, Number@8}`** — NOT `{CmpIdx, Number@4, Display@8}`. Shipping build: `CmpIdx==DispIdx`
  so bytes 0-7 read as the DUPLICATED index (`Init_904`=`0x0000038900000389`); real `Number` is `op[2]`
  (@byte 8), =0 for a clean verb name. Match `op[0]==StringToFName.ComparisonIndex && op[8]==Number` —
  raw bytes-0-7 vs an 8-byte FName NEVER matches (v1 probe's silent-miss). LIVE-measured; the probe-first
  STEP 1.0 caught it BEFORE the un-removable swap. *Look FIRST:* dump the live operand as THREE int32s,
  expect `op[0]==op[1]`. `memory/lesson_fscriptname_operand_layout_cmpidx_dispidx_number.md`
- **A guard/suppression that never LOGS is indistinguishable from one that never FIRES** — the client
  kerfur menu-cancel hooks the PE-VISIBLE menu entry, but the conversion verb is `EX_LocalVirtualFunction`
  (PE-invisible), so the cancel NEVER reached it (*"cancel/queue lines never appeared in any real session"*,
  `kerfur_convert.cpp:97,402`) — the client has been converting LOCALLY then reconciling after the fact, and
  that dead guard is the mechanism that made take-9-bug1 possible ("kerfur deleted on both peers"). THREE
  this session, same shape (dead cancel · Model-B eid-reuse [§3 said rebindInPlace, code mints per-form eids]
  · "the two-phase arm record" = actually FOUR converge mechanisms): the DOC describes intent, the CODE
  describes behavior, nobody diffed them. Instrument the SUPPRESSION path (one line on every fire); before
  building ON a documented mechanism, grep its fire-line in a real session to prove it RUNS. **INVERSE
  INSTANCE 2026-07-14 pm:** a no-log guard can be REACHED-BUT-DECLINING, not only never-fired — I read
  `TryCaptureKerfurPropDestroy`'s zero log lines as "structurally dead" and nearly deleted it; it was reached
  on every client turn-on and declined SILENTLY (its no-qualified-B path logged nothing). Instrumenting the
  DECLINE path (not just suppress) revealed it. So instrument EVERY exit, not just success. *Look FIRST:*
  grep the guard's log line in a real log — no line = never fires OR silently declining; add it on the fire
  AND decline paths and find out which BEFORE building on it or deleting it. **SECOND INVERSE 2026-07-14 eve
  (log LIES the OTHER way):** `prop_lifecycle.cpp` Init POST logged `"HOST broadcasting SPAWN"`
  UNCONDITIONALLY, above the sole-express suppress gate — so a SUPPRESSED conversion still printed a broadcast
  that never happened; behavior right, log invented a failure, and it cost a detour chasing a phantom
  double-broadcast in the 20:20 take. Same root one layer down (log-SITE vs code-PATH): **a fire-line must be
  emitted from INSIDE the branch it describes — logging before a gate logs an INTENTION, not an event**, and an
  intention-log makes a working seam read as broken. Fixed `6b246201`.
  `memory/lesson_guard_that_never_logs_is_a_dead_guard.md`
- **A VM-dispatch bracket (GNatives-swap wrapper / self-bracket) runs MID-BYTECODE — do ZERO engine
  calls in that window** — capture data only (pointers, eids off a LIVE actor, class checks) + a pure DATA
  STORE (no engine dispatch); DEFER every engine call (register, park=ProcessEvent, broadcast, converge) to
  the deferred barrier. A nested ProcessEvent pump mid-verb corrupts (measured `kerfur_convert.cpp:11-20`;
  park=PE `kerfur.cpp:132`). **KERFUR MID-VERB STORE CORRECTED 2026-07-14 pm (DRAIN retired, measured-false):**
  the mid-verb store is just CAPTURING B (the successor actor pointer + index) — NO drain, NO repoint, NO
  migrate. The FINAL fix feeds that captured-B to the DEFERRED converge (`TryCaptureKerfurPropDestroy` /
  `ConvergeAfterConversion`), which does its normal per-form eid mint + KerfurId re-key UNCHANGED; only WHICH-B
  is fixed (see `[[project-vm-dispatch-2a-capture-2026-07-14]]`). Core discipline (zero engine mid-verb, defer to barrier)
  is reusable for the whole VM-consumer class (kerfur/melee/smart-items). *Look FIRST:*
  `docs/COOP_VM_DISPATCH_PLAN.md` §3 (SUPERSEDED banner points at the A+ spec).
  `memory/lesson_vm_bracket_zero_engine_mid_verb.md`
- **The kerfur conversion verbs are SYNCHRONOUS bodies (no latent node) — so the form spawn
  (`FinishSpawningActor`) AND the `K2_DestroyActor(self)` both fire INSIDE the 0x45 bracket, every
  toggle** → capture-in-window is sound. `dropKerfurProp` 30 stmts, `spawnKerfuro` 23 stmts, both
  standalone, whole-body latent scan = NONE, none between any `BeginDeferred`/`FinishSpawning`.
  [V] two ways: import-resolved body walk + 18/18 hands-on (`722fbe18`). This is the load-bearing 2a
  premise — settled, do NOT re-dig. *Look FIRST:* `research/findings/world-systems/votv-vm-dispatch-RE-2026-07-13.md`
  (body walk + runtime). `memory/lesson_kerfur_verbs_synchronous_capture_in_window.md`
- **When a design MIGRATES identity at birth, it must cover EVERY identity map keyed on the entity** —
  GENERAL principle, holds for any repoint/rebind/re-key. TRAP that made it: "the eid" is not the whole
  identity surface — the kerfur HOST has a SECOND table (`g_actorToKerfurId`/`KerfurRecord.actor`,
  `kerfur_entity.cpp:62-64`; client is eid-based, no KerfurId map) that an eid-only rebind does NOT re-key →
  mid-window KerfurId resolves DEAD-A while eid resolves LIVE-B (heisenbug). **KERFUR RESOLUTION CORRECTED
  2026-07-14 pm (drain retired): kerfur migrates NOTHING and DRAINS NOTHING at birth** — the eid is per-form
  + K stable (`kerfur_convert.cpp:188-258`), and the FINAL fix leaves the existing converge (per-form eid mint
  + KerfurId re-key) UNCHANGED, fixing only WHICH-B (feed the captured successor to the guard). So the 2nd-map
  heisenbug cannot arise — nothing is migrated at birth to go half-done. The GENERAL enumerate-every-map
  principle still holds for ANY design that DOES migrate. *Look FIRST:* grep the entity's
  id/type + enumerate every keyed map BEFORE any migration design; `[[project-vm-dispatch-2a-capture-2026-07-14]]`
  for the A+ resolution. `memory/lesson_identity_migrate_at_birth_covers_every_map.md`
- **Before installing a PERMANENT / un-removable seam (process-lifetime GNatives swap, never un-swapped),
  measure its real cost in a THROWAWAY removable probe FIRST** — including the ENABLED=false disabled path
  (the eternal tax the process pays forever) and a WORST-CASE frame, not idle. You can't roll back a
  permanent seam; the probe you can delete. (impl /qf: the gate-2.2 probe used a simpler filter → 0.013/
  0.038 was a LOWER bound, not the real-filter gate.) *Look FIRST:* `docs/COOP_VM_DISPATCH_PLAN.md` §2.0
  (STEP 1.0). `memory/feedback_probe_first_for_unremovable_seams.md`
- **A destroy-seam consult runs POST-destruction: engine reads on the dying actor return ZEROS** —
  `GetActorLocation` on it reads (0,0,0) (RootComponent gone) while class/name/key MEMORY reads still
  work, so a proximity matcher silently mis-filters (take-10: the capture never fired all session).
  AND: matcher decline paths must NEVER be silent — take-10's two unlogged declines cost a full test
  cycle to localize. Positions come from caches (watch/stamps/element rows), never live dispatches on
  the dying actor. *Look FIRST:* `docs/COOP_VM_DISPATCH_PLAN.md` (the superseding temporal-pairing
  design). `memory/lesson_post_destroy_seam_reads_zeros_and_silent_declines.md`
- **BP-JSON call censuses: text-grepping an export for a NATIVE fn name gives FALSE NEGATIVES** — imported
  callees are bare `StackNode` indices; resolve `Imports[-idx-1].ObjectName` first (2026-07-10 twice:
  updateHold "no attach", delEmail "removes=[]"). *Look FIRST:* the resolver pattern in
  `tools/rng_census_analyze.py`. `memory/lesson_bp_json_grep_resolve_imports.md`
- **use-HOLD (`canBeUsedHold`) bypasses InputAction press-sims** — bind identity on the ENTITY-sim. `memory/lesson_use_hold_bypasses_press_seams.md`
- **An InputAction can have MULTIPLE delegate bindings — hook ALL.** `memory/lesson_input_action_multiple_delegate_bindings.md`
- **Every global `GetAsyncKeyState` hotkey poller gates on `!IsOverlayCapturingText()` too** — else it
  fires while the user types in chat (T then G triggered voice). `memory/lesson_hotkey_pollers_gate_on_overlay_text_capture.md`
- **A gated probe that "didn't fire": FIRST verify the GATE reads true.** `memory/lesson_gated_probe_verify_the_gate.md`
- **BP dynamic-multicast delegate UNBIND from C++ is an UNPROVEN capability here** (zero
  `RemoveDynamic`/`Unbind`/`ClearDelegate` in-tree) — before designing "suppress a BP event by killing its
  delegate handler", PROVE the unbind (layout RE + probe); it's a BUILD GATE. Prefer the proven
  caller-neutralization (disable a ticker / zero an array) or host-authoritative state.
  `memory/lesson_bp_delegate_unbind_unproven_capability.md`

## 5. Engine / UE4 facts

- **NEVER raw-write a UE field the game sets via a setter UFunction** — call the setter. `memory/feedback_no_raw_write_of_setter_managed_fields.md`
- **UE `TArray<struct>` stride = 16-ALIGNED size, NOT the raw `Size:`.** `memory/feedback_tarray_stride_aligned_not_raw_size.md`
- **plain `IsLive` passes a RECYCLED slot** — cached instances need `IsLiveByIndex`. A recycled-slot fix
  must sweep EVERY cache of that entity: the 07-04 audit converted `daynightcycle.cpp Cycle()` but MISSED
  `weather_sync.cpp ResolveCycle()` → the SAME crash recurred 2026-07-15 (`setRainParticles` on a recycled
  `crematorDoor`, exit-to-menu); fixed at weather_sync.cpp:154. `memory/lesson_islive_recycled_slot_blind_use_by_index.md`
- **A runtime-spawned `AStaticMeshActor` is STATIC mobility** → set Movable BEFORE `SetActorLocation` (a
  Static root silently no-ops the teleport). `memory/lesson_runtime_staticmeshactor_must_be_movable.md`
- **SEH shields must NEVER absorb `0xC00000FD`** (stack overflow). `memory/lesson_never_absorb_stack_overflow.md`
- **A bare proxy can NEVER be `lookAtActor`** — use a camera-ray cone. `memory/lesson_proxy_never_lookatactor_use_camera_cone.md`
- **`serverBox_C.check()` re-skins PURELY from raw `IsBroken@0x378` (never `damaged`)** — notify-free, so a
  visible break mirror = raw-write IsBroken + reflected `check()`. Offsets (CXXHeaderDump): servers@0x3F0 /
  brokenServers@0x8A0 / eff@0x400/0x404. **A base runs ~54 serverBoxes** (a farm, not a handful) — never
  assume a small fixed count; a 32-cap dropped 22 (smoke-caught). `memory/lesson_serverbox_check_reskins_from_isbroken.md`

- **VOTV `.sav` = uncompressed GVAS serialized DELTA-VS-CDO** — an absent property means "CDO default"
  (Points=10, health/maxHealth=100, Version=""); row metadata is readable OFF-THREAD via a tag-walk that
  seeks past payloads (`ue_wrap/gvas_meta`); never drive `LoadGameFromSlot` N times on the game thread
  for display data (the 2026-07-11 picker freeze). `b_` = the SANDBOX prefix, not a backup marker.
  `memory/lesson_gvas_savefile_delta_vs_cdo.md`

- **Injecting a native-parity UMG menu button = 5 gotchas** — (1) the style clone-source `tex_btnStart` is
  NULL at inject time → cloning silently falls back to Roboto/Center/white; set font/colour/justify
  DETERMINISTICALLY. (2) A spawned `UButtonSlot` (content slot) defaults to `HAlign_Center` → indented;
  set `HAlign_Fill(0)`+zero padding after SetContent (UMG.hpp:314-318; `Fill=0/Left=1/Center=2`). (3) An
  external-poll click on a real UButton must fire on the RELEASE edge (down-edge → overlay swallows the
  UP → button stuck DOWN). (4) Keep FSlateSound `ResourceObject`(0x00), zero ONLY the trailing TSharedPtr
  cache(0x08) → native `buttonclick`/`buttonrollover` play without aliasing. (5) Play VOTV sounds via
  `PlaySoundAtLocation` (null att = 2D) so the game's SoundClass/mix apply; the menu's press bg-dim is the
  submenu/loadLevel fade, NOT a per-button style (replicate with a modal ImGui backdrop).
  `memory/lesson_umg_injected_menu_button_native_parity.md`

## 6. Assets, models, geometry

- **Curating GAME assets = census EVERY asset** — games ship broken leftovers. `memory/lesson_game_asset_census_before_curation.md`
- **`mainPlayer_C` renders TWO overlapping bodies — apply mesh to BOTH slots.** `memory/lesson_attachparent_visibility_two_body.md`
- **Cooked UE meshes store CW-outward winding — MEASURE + match** (signed volume). `memory/lesson_winding_match_template_signed_volume.md`
- **Porting SCS templates: copy behavior flags BIT-EXACTLY** (dormancy). `memory/lesson_template_faithful_scs_dormancy.md`
- **Anim nodes INSIDE a state contribute NOTHING when it exits** (post-BUA seam). `memory/lesson_animbp_state_hosted_nodes_post_bua_seam.md`
- **A learned per-bone profile is exact ONLY on its source skeleton — MEASURE fit.** `memory/lesson_converter_fit_measured_not_assumed.md`
- **NEVER strip geometry from a shipped model on geometric heuristics** (need visual proof). `memory/lesson_never_strip_shipped_geometry_without_visual_proof.md`
- **VOTV's own fonts:** `FSEX300` = Fixedsys Excelsior (font_terminal, pixel); `ShareTechMono` = font_ui
  (subtitles, Latin-only subset). `memory/reference_votv_fonts.md`

## 7. Performance

- **`GetActorLocation`/`GetComponentLocation` are UFunction DISPATCHES, not raw reads** — never bulk-call
  per-tick over thousands of actors (invisible on a fresh save, hitches the host on a mature world);
  throttle / pre-filter / read the raw transform. *Look FIRST:* `engine.cpp GetActorLocation`. `memory/lesson_getactorlocation_is_a_ufunction_dispatch.md`
- **Per-tick `GUObjectArray` walk: cheap class check BEFORE `NameOf`.** `memory/lesson_full_array_walk_cheap_filter_before_nameof.md`
- **A periodic FPS hitch by PERIOD COINCIDENCE is not causation** — measure the real source. `memory/lesson_periodic_hitch_not_the_walk_by_period_coincidence.md`
- **A fixed-capacity hook table + ASYMMETRIC roles = a half-working fix.** `memory/lesson_hook_table_capacity_asymmetric_peers.md`
- **ImGui COMPOSITE widgets: commit via a DEBOUNCE on value-changed.** `memory/lesson_imgui_composite_commit_debounce.md`

## 8. Build / deploy / git hygiene

- **`deploy-all.ps1` deploys Release** → ALWAYS build Release + hash-verify. `memory/lesson_deploy_sources_release_config_not_relwithdebinfo.md`
- **The smoke HOST slot `s_1234` is STATEFUL — restore `coop_backup` FIRST.** `memory/lesson_s1234_host_slot_stateful_coop_backup.md`
- **`votv-coop.log` is TRUNCATED at boot (no rotation)** — copy a peer's log to the scratchpad BEFORE any
  mid-run relaunch or the previous life's evidence is destroyed (2026-07-10: an 18-min census slice lost).
  **Idle death claims BOTH peers — ROOT = STARVATION, now keepalive-fixed** (2026-07-10 night: harness
  save starts food=24.4, idle drain ~2.3 food/min, measured by the ticker's own pre-refill log;
  `[dev] vitals_keepalive_sec=180` `0211b9c5` pins vitals -> 65-min continuous run, zero deaths)
  (client ~18 min, HOST ~80 min — the 14:27:16 "LOCAL PLAYER DIED
  role=HOST" real log): a later "connect timed out" against such a host is CORRECT, not a join bug (the
  2026-07-10 "stale-slot race" candidate was exactly this, refuted from the saved logs). Long exposure
  runs must keep peers alive or script around per-peer deaths.
  `memory/lesson_copy_peer_log_before_relaunch.md`

- **OWNER-EFFECT RULE (user, 2026-07-10)** — player-proximity ambient effects (color wisps, fireflies,
  autumn leaves): the local peer KEEPS rolling them (never host-rolled, never suppressed) but a
  cross-peer mirror makes them visible to all peers. Shipped precedent = `coop/world/firefly_sync`
  (v51) — generalize THAT shape, don't invent. Look FIRST: docs/COOP_RNG_AUTHORITY.md USER DECISIONS +
  `memory/feedback_owner_effect_rule.md`

- **A one-shot session-start pass over world state parks/indexes NOTHING — the world materializes
  LATER** (2026-07-10, spawn_authority: initial park pass found 0 instances; the fix is
  hunt-until-first-hit at 1 Hz then relax). Instance #4 of the snapshot-before-state-ready class.
  Look FIRST: `memory/feedback_snapshot_before_state_ready.md`

- **Making a static/absent MIRROR MOVE can WAKE dormant per-peer OUTPUT generation** (2026-07-15, desk
  cursor v109). The jaggy-cursor fix animated the host's coords-panel cursor mirror; the host's native BP
  then began appending LOCAL `MOVE_*` coordLog lines from that motion (silent while the mirror was frozen),
  and `ProduceLogLines` running on "EVERY peer" shipped them → host shipped 78 log lines vs client 13 = a
  NEW divergence the fix created. You test the axis you fixed (cursor = smooth) and miss the downstream axis
  the motion now DRIVES (the log). Rule: after animating any mirror, enumerate every per-peer producer that
  reads the now-moving field (log producers, tick-sims, ship counters) and gate it to the owner / suppress
  the non-owner path. A "mirror the input" change is incomplete until "don't ALSO generate the output
  locally" is done. Family of OWNER-EFFECT + mirror-STATE-not-verb. Look FIRST:
  `memory/lesson_smooth_mirror_wakes_dormant_per_peer_generation.md`,
  `memory/project_desk_console_sync_2026-07-15.md`
- **NEVER `git add -A`/`<dir>` over held WIP — explicit paths or stash.** `memory/lesson_never_git_add_A_over_held_wip.md`
- **AUTONOMOUS pile test loop harness** (reference). `memory/reference_pile_test_harness.md`
