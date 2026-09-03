# The signal chain's build log, s17-s30b (2026-07-16 .. 2026-07-20)

**Moved here VERBATIM on 2026-09-03** out of `CLAUDE.md`'s reading-order entry `4e.`, where it had
accreted to 271 lines -- 24 KB, a fifth of the whole reading order, and the single entry that set
`ro-longest` at 275 against a target of 15. It is a session-by-session record, which is not what a
reading order is for; the entry there is now a pointer to this file.

Nothing was rewritten or summarised -- with ONE exception, stated here rather than left to be
discovered: the text below is the entry's own, de-indented by its three-space continuation. It
carries one session that is NOT about signals, s29b (the Multivoid rebrand), because it was
written inside this entry as it grew and moving it elsewhere would have meant re-filing claims
rather than relocating them. Read it as the record of what was written, when.

The exception is s30 / s30b, the two SECURITY sessions of the same window, which were CUT rather
than moved. A verbatim move out of an unpublished file into a tracked one is a PUBLICATION, and
the fidelity argument above is the right test for a move and the wrong one for a publication.

For the CURRENT state of the signal chain read `README.md` (the native pipeline, the four sync shapes,
the mixed-ownership rule) and `TRACKER.md` (element-by-element status) beside this file. Where this
log and those disagree, they are newer.

---

Current front (2026-07-16 eve): **v112 BUILT** (the BUGS-v111 fix: claim-free `DeskInput`
field-delta lane + per-channel exact-snap interp + charge-event cooldown; commit `7d57478f`,
smoke PASS x2, awaiting the USER hands-on take) — design of record
`votv-desk-input-lane-DESIGN-2026-07-16.md`. The downstream lanes OPEN-4..9 have a CONVERGED
architecture design (`votv-signal-chain-all-units-DESIGN-2026-07-16.md`, 9-round /qf):
presser-authored state broadcasts + tier rule (PE seam > raw-field poll > VM-bracket).
**L4 dishes = BUILT v113 (2026-07-16 night s17, commit `f204c0f7`, smoke PASS, NOT hands-on):**
impl design `votv-dish-L4-impl-DESIGN-2026-07-16.md` (10-round /qf "that holds") on the
impl-RE fact base `votv-dish-impl-RE-2026-07-16.md`. Client dish sim parked + own-ping
kill-with-cleanup (detector reworked to the coord identity-tuple change-edge); DishPose=39
host pose stream + ONE ApplyDishRow; DishArm=99 host-polarity ARM axis (single author;
v70 pending-adopt RETIRED, DeskState 60->52 B); DishCalib=101 symmetric lane.
**L7 caddy+task = BUILT v114 (2026-07-17 s18, commit `ba8ce297`, smoke PASS, NOT hands-on):**
impl design `votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md` (9-round /qf "that holds"):
ReelSlot=102 presser slot edges + ReelPose=40 host corrector (client accrual NOT parked —
written park-doctrine deviation) + TaskNewState=103 host mirror (writers host-only by census)
+ savedScalar birth channel (PropSpawn _pad2 + flag 0x40; PropDropIntent 168->172, BOTH
kinds) + ReelEjectIntent=104 (client-eject birth via the F2 author).
**v115 desk AUDIO mirror + cursor v2 = BUILT (2026-07-17 s19, commit `c5ff11a4`, smoke PASS x2
+ e2e audio self-test proven, NOT hands-on):** the user's mid-v114-take reports (observer hears
no keypress/beep/loop; cursor jerks; momentum tail lost) fixed at the NATIVE AUDIO SEAM —
Func-patch AudioComponent:Play + ActorComponent:SetActive/Activate (all whitelist-comp sites
measured EX_VirtualFunction on native targets -> ->Func; the first FORWARD-use of the Func
seam), relayed DeskSndFx=105, pointer whitelist = the desk's 6 unit-1 comps, ScopedWireApply
echo guard, loops join-re-asserted + host-owned leaver teardown; desk_cursor v2 = claim-
DECOUPLED stream + settle-gated momentum tail (0.25px/500ms, cap 15s) + adaptive interp window;
RULE-2: PlayScanEffects beep + PlayPingSuccess replay RETIRED. Design
`votv-desk-audio-mirror-v115-DESIGN-2026-07-17.md` (6-round /qf "that holds").
**v115b PHANTOM ping-FSM fix = BUILT (2026-07-17 s19 eve, commit `de31889e`, smoke PASS x2,
NOT hands-on):** the user's LIVE 14:46 ping test caught the v112 CoordIsPing raw apply WAKING
a parallel ping sim on the host (the FSM is a LATENT tick machine gated on coord_isPing —
analogd uber @82980 -> @80105, ==1.0 latches; run-flag and display-gate are ONE fused field)
-> divergent verdicts + phantom ARM raising the mirrored detector + double coordLog authorship
+ a false post-catch DISARM stomp. Fix (3.5-round /qf, reframe surfaced, user green-lit):
receivers NEVER machine-apply CoordIsPing (bookkeeping only), desk FSM-hold claim
(device_occupancy host reconciler) covers the pinger's run, arm-poll re-init-window predicate
(DISARM suppressed while signalData lives), + solo-host connect seed (audit CRIT-1). Design
`votv-ping-fsm-phantom-v115b-DESIGN-2026-07-17.md`.
**v116 catch-attribution retire + laptop_C lane + catch->feed = BUILT+PUSHED (2026-07-17 s19
nite, commit `613f2ac4` + ue_wrap 6-subfolder split `9d24ac0c`, proto 116, smoke PASS x3,
NOT hands-on):** the take-3 live test measured the LOST-CATCH root — the claim-gated catch
detector raced the FSM-hold release the successful ping itself triggers (17:04:46/47; the
baseline roll-forward ate the edge) -> host NO SIGNAL + frozen host dishes + client 24-dish
self-slew, ALL from one eaten edge. v116 (9-round /qf, two "that holds"): claim gates RETIRED
(unprimed change-edge = authority), kind=2 feed-silent connect seed, host IsRecent dup guard,
settled-dish lookAt slew fallback, catch -> peer_action_feed line per peer; NEW laptop lane
(LaptopState=106: power b8-replay + ATOMIC floppy scalars+content + HOST disc-content
authority; disc destroy rides the v106 seam, eject rides birth channels; buffer+portable PC =
OPEN-10); ini diag-battery OFF + HOST perf_probe (OPEN-1 instrumentation). Design
`votv-v116-catch-attribution-and-laptop-DESIGN-2026-07-17.md`, RE
`votv-laptop-pc-RE-2026-07-17.md`. DLL `bcf0f58e4423cb66` x4. Two PRODUCT QUESTIONS pending
the user: observer mid-ping display mirror; kind=1 delete-feed line. NEXT = **hands-on
(v112..v116 batched, runbook take 4 — SIX unverified layers, prefixes attribute; RELAUNCH
both peers, proto 116) then per-user-directive EVERY remaining design/question gets its own
/qf up to 15 rounds next session: cursor-R1 (if take-4 still jerks), the
event_dispatch_signal.cpp extraction + L6 -> L8 -> L5 -> L9 (0x45 probe in L5/L9), laptop v2
(OPEN-10), the two product questions, env-host checkbox one-liner.**
**s20 (2026-07-18) DELIVERED: the router extraction (`e88cc5e0`), v117 L6 deck playback
(`c077e910`), v118 L8 physMods (`45a886a4`, 1 CRIT caught+fixed).
s20b (2026-07-18/19 night): the 0x45 HALT gate RUN + PASSED (gnatives_probe v4: all L5/L9
verbs measured 0x45; `upd` REJECTED ~238/s ambient; perf 0.015 ms/fr; SIZES offsets [V] but
0 rows -> L9 image size unmeasured) and **v119 L5 drive chain BUILT (`b7ed3799`, /qf 7/15,
proto 119, DLL `b9b0727e04d38e0e` x4, smoke x2, NOT hands-on)**: DriveSlotState=109
(idempotent any-peer slot lines; receiver overlap SELF-SIMULATES inserts; deterministic
eject-latch completion) + DrivePayload=110 (signal_wire codec sans image; authored-birth
broadcast-at-adoption) + RackState=111 (the L8 canonical shape on 0x70 rows;
CONTENT-correlated deny/reap). Audits folded: perf 0 CRIT (5 fixes) + correctness 1 CRIT
(stale pending replay) + 1 MAJOR (slot-only reap rejected). Design
`votv-drive-chain-L5-impl-DESIGN-2026-07-18.md`. comp_0 REFRAME measured (savedSignals_comp_0
= the deck save mirror, NOT a DB — RE doc corrected). s20c (2026-07-19 day) DELIVERED: **v120 L9 meadow DB
BUILT+PUSHED (`6967a13a`, proto 120, DLL `452973c707d9cb8d` x4, smoke x2 + e2e digest
0->1->0 proven cross-peer, NOT hands-on)** — 15-round /qf "that holds"; content-hash
MULTISET shadow (positional walk + pointer RowKeys invalid: sortSignal deep-copies
FStrings); MeadowAppend=112/Delete=113/Order=114; ORDER SYNCED per the user's rule-1
decision (host-canonical, byte-permute + genSignalList re-applier, FIFO guard: order
sends deferred while lines pend); join seed seedDelta(h)=cur-snap-unmaskedPendingNet at
the save_transfer OnRequest snapshot; client send gate until own ClientWorldReady;
3 audits folded (perf CRIT FindClass; selftest retry; order HIGH FIFO). Design
`votv-meadow-db-L9-impl-DESIGN-2026-07-19.md`.
s21 (2026-07-18) DELIVERED: **v121 OPEN-10 laptop v2 BUILT+COMMITTED (`035a6031`,
PUSHED 2026-07-19 (d0c7b9e0..43426e82); proto 121, DLL `a451fce7cb674d04` x4, smoke x3 + BOTH
selftest circles digest-proven cross-peer, NOT hands-on)** — 11-round /qf "that holds";
two TRACKER premises measured FALSE (floppyTypes/floppyData = prop_floppyBox_C's; the
portable PC = a remote TERMINAL, bindPC(gamemode.laptop.laptop) -> claim question
dissolved). Buffer QUAD edit-script lane (laptop_buffer_sync: no-move grammar, host
canonical = the ack, eager widget rebuild) + lid op=6 + floppybox LIFO value-ops
(floppybox_sync); v116 op=4 chunker RETIRED -> blob_chunks (LaptopBlob=115/
LaptopQuad=116/FloppyBoxState=117; LaptopStatePayload 216->16 B). Audits: perf 0 CRIT;
correctness CRIT-1 (56KB blob cap + ignored canonical sends -> bounded+checked) +
IMPORTANT-2 (per-sender park map); smoke-caught ready-predicate mismatch fixed. Design
`votv-laptop-v2-OPEN10-impl-DESIGN-2026-07-18.md`.
s21b (2026-07-18 eve) DELIVERED + **ALL PUSHED (903bd0e7..52ce476a, leak audit clean)**:
the rack extraction as THREE commits — `bc14fa33` LivePropActor promotion (3 identical
eid->live-Prop copies -> coop::element), `5971cdd7` [dev] drive_selftest (the FROZEN
standalone digest instrument; host seeds a rack — fresh saves have none), `73dc9ba1` the
extraction (drive_sync 1007->606 + drive_rack_sync 553; owner API MarkDirtyFromVerb +
TryConsumeDenyReap, one-way; verb registration stays in drive_sync — vm_dispatch is
one-cb-per-name and putDriveIn is shared). 8-round /qf "that holds"; behavior preservation
MEASURED: digest equality cross-peer AND cross-commit (baseline x2 unsplit vs extraction),
literal body-diff SAME, reconnect cycle (host re-prime x2 + two seeds + second circle);
audits perf 0 CRIT + correctness FAITHFUL. DLL `6431c14382b38437` x4, proto 121 unchanged.
DISCOVERED pre-existing stable-ID residual: a rejoining client whose save CONTAINS the
rack = ONE actor under TWO eids (provisional client-band + adopted host) -> doomed
client-band ops silently dropped, canonical heals (design doc §8; queued for the
stable-ID thread). Design `votv-rack-extraction-DESIGN-2026-07-18.md`.
s22 (2026-07-18 night) DELIVERED: **v122 stable-ID ROOT no-passive-mint BUILT+COMMITTED
(`4403606c` + `77559d4b`, PUSHED 2026-07-19; proto 121, DLL
`06b9e2d23c84037f` x4, smoke A/B x2, NOT hands-on).** The s21b residual was the tip of a
measured class: the client passive census silently minted keyed Elements (~2200 zombie
double-rows PER join; local-first structural) + a host reverse-steal/fuzzy-REKEY door the
2026-06-10 ghost-twin cure RODE ON. 8-round /qf (reframe user-green-lit per rule 1):
(B) census = key-index-only on client (EnrollSource at all 12 Mark sites); (S) sweep keyed
universe = the key index + current-key doom re-validation + DrainDeadKeyIndexEntries;
(A') CreateOrAdoptPropMirror one-actor-one-row authority (host wall / client dissolve-on-
host-word + held-eid fanout / peer reject); (H) HostAuthorityHandback_ at both OnSpawn
resolution points (enroll+re-express replaces the stack/rekey corruption). A/B vs s21b:
sweep universe 2236->1, rack ONE host eid + single ops, digest circles cross-peer, zero
guard fires, health classes byte-identical. Audits perf 0 CRIT/0 WARN + correctness 0 CRIT
(IMPORTANT-1 fixed in-session). Design `votv-stable-id-no-passive-mint-DESIGN-2026-07-18.md`
+ docs: ENTITY_EXPRESSION_MAP v122 note, STABLE_ID_SIDECAR keyed-half CLOSED, LESSONS §2 x2.
s23 (2026-07-18/19, incl. 2 autonomous ticks) DELIVERED, 8 commits (PUSHED 2026-07-19):
env-host checkbox fix `2de5ad31`; TWO features `197d11e5` (device-busy LOCAL chat line
"<HolderNick> is using <native unit name>" at all 3 deny surfaces, aim-seam memo, ungated
AnnounceDirect grammar owner; activity feed NEVER renders "You" -- nickname always, RULE 2);
THREE extractions by the frozen-instrument recipe: session_streams `06921557` (session.cpp
1208->679+620; mutant-proven 4p relay matrix), net_pump decomposition `de249463` (1237->744 +
registry_reaper 401 + puppet_drive 218; /qf 6r), component_calls `b5c1b911` (console_desk
1021->928). Proto 121. TWO PRODUCT QUESTIONS pending the user:
busy-line nick-first wording; peer_actions-toggle bypass. prop_identity+laptop-lid MOOT (<800).
s24 (2026-07-19) DELIVERED: **coords_panel extraction `129fb004`** (the s23c deferral resolved,
2-round /qf: console_desk 928->822 + ue_wrap/desk/coords_panel 173; seams AtlasUiCoordsSlot +
CallUpdateCoordCoords publics; literal-diff PASS w/ mutate control, smoke PASS both-peer resolve
lines, audit 0 CRIT) + the **7-PHASE long-term arc user-approved + FIXED in docs/ROADMAP.md**
(`6dd99e97` + notes commit:
coop -> sandbox -> LuaJIT -> Lua API (C++ core stays) -> resource system (modes+plugins=one) ->
dedicated (headless host game; ghost-host/no-redistribution/Wine-spike notes) -> resource infra
(client AC trust note) -> 8. native server = MTA authority INVERSION (per-element syncers,
verified vs vendored CUnoccupiedVehicleSync; BP-VM research branch)).
s24b (2026-07-19 day) DELIVERED: **the console_desk residual cut CLOSED** — `f74d05dc` retires
the positional g_fields table (named offsets, self-binding {name,&var} rows; the /qf R1 find:
literal-diff AND the compiler are both BLIND to a missed index renumber; correspondence script
w/ mutate control; one-shot 23-pair offset dump == header reference both peers) + `f9dfb5d5`
comp_pane extraction (ue_wrap/desk/comp_pane 58+212, own g_required latch = 4 field offsets
REQUIRED + opportunistic rest; NEW public console_desk::AtlasWidget() seam; comp pane chosen
over the v70 catch surface — MEASURED: its DL_* offsets straddle the cut). console_desk 822 ->
**740 UNDER CAP**; 6-round /qf "that holds"; body-diff 11 regions + mutate PASS; smoke PASS x2
+ 60s re-smoke on final bytes; audit 6/6 PASS 0 findings. DLL `0D82CF460B6ADC62` x4, proto 121.
**NEW PRINCIPLE 8 (user, 2026-07-19): mid-activity join is ALWAYS handled per RULE 1** (see
the principles list above).
s25 (2026-07-19, "go next") DELIVERED: **weather_sync 1154->784 CLOSED under cap** —
`828844b2` coop/world/weather_rain (the rain+snow cycle-side sub-lane, the fog/lightning/
redsky FAMILY axis; own 5 mutator resolves, in-module causeRain echo interceptor,
outcome-struct keeps the fused "applied" line byte-identical; RULE-2: 3 thin forwards
RETIRED, 16-row caller census migrated) + `cd59ad13` coop/dev/weather_probe (probe block +
ReadComponentIsActive; seams weather_rain::Cycle() + WindRoll accessors). 7-round /qf
"that holds"; NEW instrument: in-smoke WEATHER TEST (shell env VOTVCOOP_RUN_WEATHER_TEST=1
propagates via mp.py) + lan-test literals imported verbatim as the gate + probe min-floors
+ injection-proven WARN gate; baseline-first, identical s_1234/ini/env both runs. Audit 6/6
PASS 0 findings. DLL `c2b33b2a4b1e5d3f` x4, proto 121. NOT hands-on (rides take 4). Docs
`e68d38e7`. >800 residue: kerfur_convert 1259, harness 1222, autotest 1003 (audit flag).
s26 (2026-07-19, "go next") DELIVERED: **autotest.cpp 1002 CLOSED (dissolved)** -- the
island's one-feature-per-file convention applied via THREE commits (`89ce6602` ReadEnv
retire 8 sites -> config::ReadEnv; `f299107c` 9 routines -> 6 new TUs: clump 200 /
weather 198 / flashlight 154 / worldrules 69 / worldctx 49 / tracker_selftest 77, residual
grab-only 393; `cc4c93c3` pure git mv -> autotest_grab.cpp, 99% rename, --follow intact).
4-round /qf "that holds"; evidence: body-diff 556 nb-lines verbatim + residual
sequence-equality + 7 must-FAIL mutate controls; TWO differential smoke pairs exercised
ALL TEN moved routines cross-peer (pair A = 8 scenarios, pair B = grab+clumpvis;
one-writer-per-axis scenario census; 36 verdict keys identical baseline vs post;
baselines on post-commit-0 bytes). DLL `b62c64263f8075f0`
x4, proto 121. Design `votv-autotest-dissolve-DESIGN-2026-07-19.md`. >800 residue:
kerfur_convert 1259, harness 1223, autotest_vitals 1013 (flag: PuppetFrame rig out
first), autotest_chippile 877 (single-family, watch).
s27 (2026-07-19 eve, "go next: kerfur, vitals, harness; тесты в конце") DELIVERED —
**the >800 queue CLOSED, 11 commits `bcd7b44b`..`7ccf3a58` incl. docs, PUSHED 2026-07-19 (leak audit clean)**:
kerfur_convert 1259 -> 633 + kerfur_convert_client 395 (ghost custody + wire apply) +
kerfur_convert_host 390 (converge + OnConvertRequest + bracket; NEW one-way
RecordSeamConvergedInBracket; two-layer Install handoff; ONE documented fail-closed
deviation: DISABLED-state requests now DROP — latent pre-cut over-read, unreachable on
this build); autotest_vitals 1013 dissolved -> ragdoll 373 (rename, --follow intact) +
damage 238 + dmghazard 252 + playerdmg 158 + puppetframe 175 (s26 flag resolved);
harness 1223 -> 526 + harness/session_runtime 709 (lifecycle driver owns g_session;
Session() accessor; nick-color parse -> nick_color; RULE-2 retire of the DEAD netloopback
scenario + its displayOffsetX chain — alias-vocabulary census). Three /qf passes (5+4+7
rounds, "that holds"); per-commit literal body-diff instruments + mutate controls
(scratchpad/s27); TWO audits PASS 0 behavioral (cosmetics fixed `de304643`). The runtime
differential batch was USER-CANCELLED after run 1 (kt-baseline kerfurtoggle PASS;
re-runnable: scratchpad/s27/batch.sh + 3 frozen DLLs + s_1234 snapshot). DLL
`61f56942` x4 (HOST/CLIENT_1/2/3), proto 121; s_1234 restored. Design
`votv-s27-three-cuts-DESIGN-2026-07-19.md`.
s28 (2026-07-19, "go next: remote prop, npc sync, puppet; БЕЗ smoke") DELIVERED — the next
>800 tier CUT, NO runtime smoke per user directive (equivalence = body-diff instruments +
18 must-FAIL mutates + per-commit Release builds + 2 audits PASS 0 findings >=80):
remote_prop 1180 -> 758 (`6c910046` convert TU 291 zero-new-seams + `d0c7879e` physics TU
196, DrivePropThrown -> internal.h); npc_sync 989 -> 709 (`fd7c7409` install TU 327 +
npc_sync_internal.h: 5 shared globals install-side-defined, 3 callbacks anon->named, ONE
enumerated edit store->SetSession byte-equal); puppet 972 -> 491 (`ca12e11d` spawn TU 512:
the head-gate hook moved WHOLE with its sole install site — /qf find that dissolved the
seam + the anon-close relocation; puppet_internal.h templates+g_meshComp+LiveAnimInstance).
m6 mutate EXPOSED the whitelist-misses-dropped-lines instrument blind spot (fixed to
exact-content; lesson sharpened). Three /qf passes (5+3+2). Docs+cosmetic commit follows;
NOT PUSHED (6 local commits over origin `7ccf3a58`). DLL `1626b6e0` x4, proto 121.
NEW >800 queue: save_transfer 925, meadow_db_sync 884, chippile 877 (watch),
player_handshake 828; near-cap prop.cpp 799, item_activate 792.
s29 (2026-07-19 eve) DELIVERED: **b122 version identity "multivoid" (`5246844a`, proto
121->122, DLL `multivoid-0.9.0n-122.dll 4C994D0E` x4)** — the Paper-pair identity (game
target + build number, mod semver DELETED per user; display "votv-coop 0.9.0n b122");
3-layer per-lobby EQUALITY join gate (browser pre-flight RefuseJoin popup / Join-seam
wire gate in player_handshake_version.cpp + host feed line / header backstop); DLL
RENAMED multivoid-<game>-<build>.dll (proxy scans, loads highest build, dup install ->
"MOD INSTALL PROBLEM" popup ui/boot_warning_dialog); Rust master +game deployed to the
VPS (latest = proto 0 "no released record" -> client silent); docs/RELEASE.md checklist.
13-round /qf "that holds" + 2 user reframes; drills A1/A2/B/B2/B3 + dup + smoke x2 all
real-log PASS; audits perf 0 CRIT + correctness 1 finding fixed (SEH guard). Design
`votv-version-identity-v122-DESIGN-2026-07-19.md`. NOT hands-on.
s30 and s30b (2026-07-20) -- the TLS arcs and the threat-model reframe -- are NOT recorded
here. They were security sessions, and this project keeps that material out of the public tree
by policy (`docs/DOCS_ARC.md`; `docs/security/` is deliberately unpublished, because a register
of open weaknesses is an exploit map). Their record lives there instead, and is kept current
there: the decision rows in `DECISIONS.md`, the session row in `EXECUTION.md`, and the plans in
`PLAN_0*.md`. Nothing about the signal chain is in them.
s29b (2026-07-19 eve, mid-take-4-prep): **REBRAND Multivoid SHIPPED** — repo moved to
github.com/VOTV-MP/Multivoid (org VOTV-MP; old repo frozen w/ MOVED notice; made PUBLIC
per user), README rewritten (no master-endpoint reveal / no PaperMC attribution /
Multivoid-server = FUTURE dedicated server phase 8), domain multivoid.dev LIVE
(master.multivoid.dev grey-cloud -> the box; votv.mp retired), VoidTogether removed
from reference/ (RE docs remain). INTERNAL sweep per rule 1: compiled endpoints ->
master.multivoid.dev:10001/10000 (WinHttpConnect + getaddrinfo resolve hostnames —
measured; curl healthz via hostname OK), runtime artifacts renamed multivoid.ini /
multivoid.log(+prev) / multivoid-loaded.txt / multivoid-compat-report.txt /
multivoid-players.txt / multivoid-banlist.txt / LogicMods/multivoid, display identity
"Multivoid ..." (menu label / boot banner / log banner / UA), tools+bats+mp.py swept,
4 installs disk-migrated (ini rewritten IP->hostname; guid/skins preserved), Rust
master LATEST_URL -> VOTV-MP/Multivoid/releases redeployed+curl-verified. DLL
`9370C1C1C7690B21` x4, smoke PASS on final bytes, proto 122 unchanged (no wire change).
Legacy name survives ONLY in the loader's dup/legacy detector (votv-coop.dll) +
historical docs. Env vars VOTVCOOP_* + CMake target/src-folder names stay working-name.**
