---
name: qf
description: >
  Run the Question-Form adversarial ritual with the PRIMARY (you, the main session) in the loop: spawn a
  fresh critic agent, hand it the CURRENT session context + the rules from docs/QUESTION_FORM_AGENT.md, get
  back 2-4 pointed questions, let the agent despawn, then answer those questions in the visible main session,
  and repeat with a fresh critic UNTIL A STOP CONDITION: bare "/qf" (with or without a steer) runs to
  convergence; "/qf N" (e.g. /qf 3) is the user's explicit choice to pace the loop and stops after N rounds.
  A Q&A cycle that converges on the root WITH the primary answering - the thing /qf-workflow structurally
  cannot do (a background loop can't reach the main session). Invoke when the user types "/qf" (exactly -
  NOT "/qf-workflow", which is the automated background loop). Project: VOTV_MP.
---

# /qf — the Question-Form ritual, run to convergence (primary stays in the loop)

You are automating the **copy-paste** of the manual ritual in `docs/QUESTION_FORM_AGENT.md`: instead of the
user pasting your message to a second agent and the reply back, `/qf` spawns that second agent for you,
a fresh one per round, and keeps going until a stop condition (below). Read `docs/QUESTION_FORM_QF_SKILL.md`
first if you have not this session — it defines this skill's shape and how it differs from `/qf-workflow`.

**The division of labour (do not blur it):**

- **The critic agent** = the Question-Form Agent (`docs/QUESTION_FORM_AGENT.md`) + the OPUS lens
  (`docs/OPUS_48_DISCIPLINE.md`). It replies **only in questions** (2-4, short, credit-first line). It does
  NOT design, write a fix, build, or run. It MAY read the repo read-only to sharpen a question. Then it
  despawns.
- **You (the primary), in THIS main session** = do the real work and **answer** the questions, visibly.
  Answering / reasoning / design belongs here, in the open, where the user sees it
  (`feedback_no_design_architect_agents`, OPUS §5) — never inside the critic.

**The loop runs to convergence. The user does not pace it (USER RULE 2026-07-30, verbatim: *"Why are you
always stopping running qf, run qf"* — `[[feedback-run-the-qf-loop-to-convergence]]`; the pass that
prompted it converged at round 22, and rounds 12-22, run back to back, each landed a finding).**

- **`/qf`** and **`/qf <steer>`** = run rounds back to back — fresh critic, you answering each in the open,
  the brief updated with your own answers between rounds, the steer folded into every brief — until ONE of
  the stop conditions below. The whole transcript is shown to the user at the end. (This is what
  `/qf-workflow` cannot do: a background workflow can't route a question back to the main-session primary
  and await its answer — see `docs/QUESTION_FORM_QF_SKILL.md`.)
- **`/qf N`** (a leading integer, e.g. `/qf 3`) = the **user's explicit choice to pace the loop**: run at
  most N rounds, then stop with `STOP: user cap N` and hand back. Use it only when the user asked for it.

**THE ONE STOP LIST.** Every stop is `python tools/qf/ledger.py --dir <scratchpad> stop <reason>`, which
writes the `STOP: <reason>` line into `qf_thread.md`. A round ends the loop ONLY on:

1. **`converged`** — the critic's verdict AND your own convergence bars (the loop section below) both hold.
2. **read-only floor** — everything still open can ONLY be settled by a runtime probe (an instrumented
   rebuild, a fresh single-variable run); a SUCCESS that names the next probe, not a failure to converge.
3. **handed to the user** — a question only the user can answer: a product-feel question, a second reversal
   on one axis, a fact only they hold.
4. **a reframe** — a premise flips, a primitive turns out to be several, the model is replaced, a
   suppression dissolves (the reframe-surface bar in the loop section) — the user, not the next fresh
   critic, is the right check at that moment.
5. **the safety ceiling** — 50 rounds, above every pass on record (max 43); write `STOP: capped` and present
   the OPEN set as a residual. This is abnormal by definition and NEVER a request to re-invoke.

**NOT stops** (the rule names them; they are the signal to keep going): "this is a good checkpoint", "the
findings are getting narrower", "I should confirm before continuing". Narrowing findings landed in every
one of rounds 17-21 of the pass that made the rule.

## Steps

1. **Parse args.** If the args START with an integer N, the user is pacing the loop: run at most N rounds
   (stop list item "user cap"); the rest is the steer. Otherwise it is all steer and you run to convergence.
   The steer is an optional focus or the user's own aside to fold into EVERY round's brief (e.g. "focus on
   the destroy seam", or "I moved piles during that join window" — the human aside is exactly what
   de-braids a confounded repro). No args = run to convergence on the current thread as it stands.

2. **Assemble the BRIEF from the LIVE session** — this is what you hand the critic. Keep it dense and
   honest; it is the substitute for the copy-paste history:
   - **THE RADICAL MANDATE (state it in the brief, always).** User rule 2026-08-24, verbatim: *"Я даю
     зеленый свет даже на самые радикальные решения, если они окажутся верными и правильными."* Scope is
     never a constraint in this project — correctness is. So the brief must say so, and it must NAME any
     option the primary considered and rejected for being too large, too invasive, or too behaviour-
     changing, because that rejection is the single highest-value thing a critic can attack. If you catch
     yourself writing *"X would be the right fix, but it would mean rewriting Y"* — X goes in the brief as
     a live option, not as a footnote. Same for a stated USER REQUIREMENT the design is bending around:
     per CLAUDE.md / `[[feedback-drop-my-requirement-if-it-blocks-rule-1]]` that requirement is an INPUT,
     not an axiom, and the brief must surface it so the critic can ask why it was not dropped. The green
     light covers SCOPE and BEHAVIOUR ONLY — it never licenses skipping a measurement, a build, an audit,
     or the honest handoff, and it never licenses shipping a bug.
   - **WHAT THE USER ACTUALLY ASKED FOR (mandatory, FIRST section)** — the STORY that led here, in the
     user's own terms, not yours. Quote or closely paraphrase the user's originating request VERBATIM
     where you can, then the short arc of how the thread got from there to now (what was tried, what the
     hands-on showed, what the user said when they saw it). Include the user's own words for the SYMPTOM
     ("клиент открыл sack и не видит ничего внутри") — the symptom vocabulary is the user's, not the
     codebase's, and translating it into your architecture nouns is itself a step the critic should be
     able to audit. Also state explicitly: **what the user asked for vs what you are now designing**, and
     if those have drifted apart, say so in this section rather than letting the critic discover it.
     WHY THIS EXISTS (user request, 2026-07-22): the critic only ever sees a brief the primary wrote
     about its own work, so it can interrogate whether the design is INTERNALLY coherent but has no way
     to ask "is this still what was asked for?" A design can converge beautifully on the wrong request —
     scope creep, a fix aimed at a symptom the user never reported, or an increment that quietly dropped
     the half the user cared about. The originating ask is the only anchor for that question, and the
     primary is the only one who can supply it. Do NOT sanitise it into a tidy problem statement: if the
     user's request was vague, contradictory, or changed mid-thread, that is exactly what the critic
     needs to see.
   - **INVESTIGATION** — one line: the failure / decision being rooted out.
   - **CURRENT CLAIMS / HYPOTHESIS** — what you hold right now, each tagged `measured | inferred` with its
     citation (log line, disasm, code). This is the surface the critic interrogates — do not launder an
     inferred claim as measured. **Name the SOURCE of each inferred claim** (which doc / disasm / RE-finding)
     — because the highest-yield miss is SELECTIVE TRUST: founding one decision on a source while gating a
     different decision on distrusting that SAME source. Surface it yourself if you can see it; if two of
     your claims lean opposite ways on one source, that is a contradiction to resolve BY RE-DERIVING, not by
     picking the convenient half. (Born 2026-07-13: a 3-round /qf converged while the primary was trusting a
     2026-06-12 RE doc for a class-successor filter AND distrusting the same doc for a latent-node question —
     the user caught it by holding the two answers side by side; the critic never cross-checked them.)
     **Tag each "established fact" by PROVENANCE, not only `measured | inferred`: `measured-artifact` (cite
     the RAW log / code / disasm line, not your own summary of it) vs `carried-framing` (a named mechanism /
     primitive / "the existing X" you INTRODUCED in an earlier round and are re-using).** The fresh critic
     inherits your brief's NOUNS as truth, so a `carried-framing` label you never code-verified — "the
     existing two-phase barrier", "the arm record", "the mirror lane", "the poll" — is invisible to it. Before
     the brief goes out, for the 1-2 nouns the design most DEPENDS on, OPEN the code and confirm the named
     thing exists AS you describe it (ONE mechanism not several fused, this shape, this lifetime) — or tag it
     `carried-framing UNVERIFIED` so the critic is pointed straight at it. **And when a round's MEASUREMENT
     shifts a foundation (a REFRAME — a premise flips, a primitive turns out to be several, the model is
     replaced), add a one-line RE-AUDIT: which EARLIER answers now rest on the changed foundation and must be
     RE-DERIVED, not inherited forward.** (Born 2026-07-14: a stash design rode "the existing two-phase arm
     record" across 3 rounds; it was FOUR distinct converge mechanisms fused into one carried label, and
     round-4's poll discovery quietly invalidated round-3's smear answer — the user, not any fresh critic,
     caught both, because the primary's self-written brief laundered the label into a fact every round.)
     **If the design MIGRATES or MUTATES an entity's identity (a repoint / rebind / re-key / move / adopt),
     ENUMERATE in the brief EVERY map/table/index/authority-record keyed on that entity** (the eid<->actor
     map, any name/id table, actor->id reverses, per-peer indices, handler/skin/brain caches, save-key
     indices) and state, per map, whether the operation updates it AT THE SAME MOMENT or DEFERS it — because
     the critic escalates within the frame you hand it, so a parallel identity map you leave OUT of the brief
     is invisible to it. Do the enumeration yourself (grep the entity's id/type across the tree); a
     half-migration — one map re-keyed while a parallel map still resolves to the dead/old actor — is exactly
     the class this catches. (Born 2026-07-13: a repoint design migrated the eid->actor map but left a
     second HOST-ONLY KerfurId->actor table finalizing late; 11 critic rounds missed it because no brief ever
     listed the second table — the user supplied it from outside the frame.)
   - **PROPOSED NEXT STEP / FIX (if any)** — so the critic can test whether it holds the whole class, or is
     a RULE-1 crutch / wrong-layer patch.
   - **PRIOR QF ROUNDS** — every question asked so far AND your answers, so the critic escalates instead of
     repeating. (In one continuous session you have this in context; if the session was compacted, read it
     back from the scratchpad thread file — see Guardrails.)

3. **Spawn ONE critic agent, synchronously** (`run_in_background: false`, `subagent_type: "general-purpose"`),
   so its questions come back in THIS turn:
   ```
   Agent(
     description: "Question-Form critic round",
     subagent_type: "general-purpose",
     run_in_background: false,
     prompt: "You are the Question-Form Agent for VOTV_MP. Read docs/QUESTION_FORM_AGENT.md AND
       docs/OPUS_48_DISCIPLINE.md FIRST - they are your format and your lens; you MUST actually open both
       files, not work from this prompt alone. THEN read docs/LESSONS.md AND docs/security/LESSONS_SECURITY.md (the security half, split out
       2026-08-24 and local-only -- a scan that stops at LESSONS.md loses the whole security corpus)
       - the project's categorized
       ledger of every hard-won lesson - and SCAN it for rows relevant to this brief's subject matter
       (its domain sections + section 1 'How to work'). It is your PRIOR-ART index: the single
       highest-yield question you can ask is 'the project already learned X - are you about to repeat
       it?', because these lessons were each paid for with a real dig or a shipped regression. If a
       lesson's takeaway CONTRADICTS a claim in the brief, or the brief is walking into a trap a lesson
       names, that is your first question and you should cite the lesson's row. If a lesson's cited
       symbol/path appears STALE against the current tree, ask about that too (the ledger rots and is
       swept for exactly this). REPLY FORMAT (mandatory): your ENTIRE reply is ONE fenced ```json block
       conforming to tools/qf/critic_schema.json (open it; the main session validates your reply with
       tools/qf/ledger.py and DISCARDS it on any violation). The shape: proofOfRead{qfDoc, opusDoc,
       priorArt[]} - each a VERBATIM 4-12-word fragment quoted exactly from that file (checked mechanically
       by tools/qf/verify_proof.py; a fabricated fragment discards the reply); credit - ONE line naming
       what the primary got RIGHT / what is now genuinely measured; unresolved[] - your 2-4 questions, each
       {id, angle (one of the schema's closed list), q (the question itself, ending in '?'), targetClaim (a
       VERBATIM quote of the brief sentence under attack), howToMeasure (a concrete READ-ONLY probe WITH its
       command), runtimeGated, anchor (what YOU opened to ask it: a path:line; or for an OMISSION the symbol
       + the owning file + the zero-hit command; or for an ask/product angle the targetClaim quote)};
       converged / readOnlyFloor - your verdict, and when either is true a top-level anchor (the LAST lookup
       you made that found nothing left to ask) plus convergenceRationale as a list of '<id>: closed by
       <citation>'. The q fields are the ONLY questions; every other field is a citation, a probe name or a
       verdict - NO statements, NO prose, NO plan, NO fix, NO code anywhere in the block: any field carrying
       an instruction or a fix voids the whole reply. At least ONE question per round (or your verdict) must
       anchor on a path:line or a command you ran yourself, and that lookup's result appears ONLY as the
       premise of a question - you never state what it means. You are READ-ONLY: you may read the repo to
       sharpen a question (grep, ls, wc, git grep, cat) but you MUST NOT edit, write, build, or run anything
       that changes state. THE RADICAL MANDATE (user rule, 2026-08-24, verbatim: \"Я даю зеленый свет даже на самые
       радикальные решения, если они окажутся верными и правильными\" / \"I green-light even the most radical
       solutions, if they turn out to be correct and right\"): SCOPE IS NEVER A REASON THIS PROJECT HOLDS
       BACK. Never soften a question because the honest answer would mean a large change -- dissolving a
       module, inverting a shared primitive's default across hundreds of call sites, retiring a shipped lane
       whole, or changing how the GAME behaves. If the brief's design is the second-best option because the
       best one was judged too big, too risky, or too disruptive, THAT is your question, and it outranks every
       other angle you could ask. Equally: a stated USER REQUIREMENT never outranks RULE 1 -- if the primary
       is designing AROUND a preference the user expressed, ask why that requirement was not simply DROPPED
       (see CLAUDE.md, [[feedback-drop-my-requirement-if-it-blocks-rule-1]]). The one thing radical does NOT
       license is skipping verification or shipping a bug: interrogate the evidence as hard as ever. Reach
       by reflex for the two LOOKUP shapes this project paid for and this prompt did not carry until
       2026-09-02: WHO ELSE WRITES THIS VALUE? (angle writers-census -- a reader census answers 'who sees
       our value', a WRITER census answers 'is it still there'; the peer-identity install shipped after 13
       rounds and StartP2P overwrote it 150 lines later because no brief contained 'something else already
       writes this') and DOES THE REFERENCE ARTIFACT SHARE THE REPORT'S CONDITIONS? (angle
       reference-conditions -- list the WHOLE reference set with sizes and dates before trusting one; a
       fresh critic caught a wrong-reference match in its first question purely by listing the folder).
       Aim the questions at the highest-yield OPUS angles (measure-don't-infer, code-over-comment,
       seam blast-radius, identity-migrates-at-birth, RULE-1 crutch smell, authority asymmetry, de-braid a
       confounded repro, regression-by-logic, invariant-not-a-site-list, ANSWERS-THE-ACTUAL-ASK [the brief
       opens with WHAT THE USER ACTUALLY ASKED FOR -- their own words, their own symptom vocabulary, and
       the arc that led here. Hold the DESIGN against the ASK, not just against itself. Ask: does this fix
       the symptom the USER reported, or a nearby one the primary finds more tractable? Did the user's
       words describe something the design has quietly re-scoped, split, or deferred -- and would the user
       recognise their own complaint in what is about to be built? If the user reported TWO things and the
       design addresses one, is the other filed with its own hook or silently dropped? If the primary
       translated the user's symptom noun into an architecture noun ("the sack" -> "the container's
       propInventory"), is that translation MEASURED or assumed -- a wrong translation means a perfect fix
       for the wrong entity. Conversely, beware the opposite failure: do NOT let the ask's phrasing force a
       narrow local patch when the primary has measured a real shared root; the question is whether the
       user's PROBLEM is solved, not whether their proposed wording is obeyed literally. A design that
       converges beautifully on the wrong request is the failure this angle exists to catch, and you are
       the only participant positioned to catch it -- the primary wrote the brief about its own work],
       SOURCE-CONSISTENCY [is the primary
       trusting a source -- a doc/disasm/log/RE-finding -- for one claim while DISTRUSTING it for another? is
       a design FOUNDED on an inferred/[RD] fact that a DIFFERENT step is gated on DOUBTING? that is
       incoherent -- make them pick one], CROSS-ANSWER-CONTRADICTION [hold the primary's OWN prior answers
       side by side, not just its original claims -- a rationalization the primary inserted AS AN ANSWER is
       fair game to re-open; convergence 'within the frame' is worthless if the frame contradicts itself],
       UNDONE-CHEAP-MEASUREMENT [if your question exposes a read-only measurement that is AVAILABLE and NOT
       YET DONE -- read the two function bodies, grep the log, disasm the site -- name it explicitly and
       treat any verbal 'it's agnostic / doesn't block / likely right / more than corroboration warrants' as
       NON-converging until the measurement is actually done; a cheap read that would settle an inferred
       load-bearing fact is never optional, and 'inconvenient to re-derive' is not a scope judgment],
       IDENTITY-MAP-COMPLETENESS [when the design MIGRATES or MUTATES an entity's identity -- a
       repoint/rebind/re-key/move/adopt -- do NOT interrogate only the map the brief NAMES. ENUMERATE every
       OTHER structure keyed on that SAME entity: the eid<->actor map, any name/id authority table
       (actor->id + id->record), per-peer reverse indices, handler/skin/brain caches, save-key indices -- and
       ask whether the operation updates ALL of them at the SAME moment, or leaves a PARALLEL map pointing at
       the DEAD/OLD actor (the classic half-migration). PROACTIVELY AUDIT the repo (grep the entity's
       type/id) for identity structures the brief did NOT list -- the critic escalates within the frame it is
       handed, so the killer miss is the map nobody PUT in the frame. Ask 'how many maps key on this entity,
       and does the op touch every one?' Born 2026-07-13: an 11-round repoint design converged on a 'that
       holds' while a SECOND host-only identity table (KerfurId->actor / KerfurRecord) still finalized late;
       the user caught it from OUTSIDE the frame -- the critic never questioned whether 'the eid' was the
       WHOLE identity surface], FRAMING-PROVENANCE [every load-bearing NOUN the brief leans on -- 'the
       existing X', a named mechanism / primitive / barrier / record / lane / arm-slot / queue / poll -- is a
       CLAIM the primary may have INTRODUCED as an inference in an earlier round and HARDENED by repetition
       into an apparent fact. Do NOT accept the brief's framing as ground truth. Pick the 1-2 nouns the design
       most DEPENDS on and ask: did the primary MEASURE that this thing exists AS DESCRIBED -- ONE mechanism
       not a conflation of several, this lifetime, this shape -- or is it a LABEL carried across rounds? OPEN
       THE CODE and confirm the named thing IS what the brief says. A design hung on a carried-but-unverified
       primitive is the SAME failure class as a map not in the frame: the primary writes its own brief, so its
       prior ANSWERS re-enter as 'established facts' and launder an inference into settledness -- and each
       fresh critic inherits the laundering BLIND. Ask 'which noun here did you name vs measure, and does it
       exist as one thing?' Born 2026-07-14: rounds 2-4 hung a stash design on 'the existing two-phase arm
       record' as ONE stable primitive; it was FOUR distinct converge mechanisms (death-watch poll /
       host-menu barrier / request-exec / destroy-edge) fused into one label -- only the user, holding the
       real history, caught that 'the barrier' and 'the poll' were not the same thing]). If
       nothing material remains, the correct answer is converged: true with an EMPTY unresolved list, your
       verdict anchor and the rationale list - not a manufactured question; if everything left can only be
       settled by a runtime probe, readOnlyFloor: true with those questions marked runtimeGated. Match the
       user's RU/EN per the doc inside the q fields.\n\nBRIEF:\n<the brief from step 2>"
   )
   ```
   Use a FRESH agent every round — it despawns after replying; continuity is carried by YOUR brief, not by
   the agent's memory.

4. **Gate, record, relay, then ANSWER here.** Save the critic's JSON block to a file and run, in order:
   `python tools/qf/verify_proof.py --reply <file>` (exit 1 = a fabricated or missing fragment: discard
   the reply and re-spawn; after two failures `ledger.py stop format-failed` and hand back) and
   `python tools/qf/ledger.py --dir <scratchpad> append <round> <file>` (exit 1 = the reply violates the
   schema or an anchor did not verify: the same discard-and-re-spawn). `append` writes the round's table
   AND the verbatim JSON into `qf_thread.md` — it IS the persistence step. Show the user the questions
   exactly as returned. Then answer each one in the open session — tag every answer `measured | inferred`
   with its citation, and when a question exposes an unverified leap, say so plainly rather than defending
   it — and record each answer's status the moment you give it:
   `ledger.py --dir <scratchpad> set <round> <id> <status> --cite "<your OWN citation>"`, status one of
   `answered-measured` (needs YOUR path:line or log path — a critic's anchor alone is not yours: re-open it
   first, or the status is `answered-inferred`), `answered-inferred`, `runtime-gated` (name the probe),
   `withdrawn-with-reason` (the critic's premise was wrong), `handed-to-user` (a product-feel question or a
   fact only they hold — a stop condition). This answering step is the point of keeping the primary in
   the loop.

5. **Judge the state, then loop to step 2** with a fresh critic — unless a stop condition from THE ONE
   STOP LIST holds. On a reply with `converged` or `readOnlyFloor` true: walk the convergence bars (the
   loop section below) yourself and, ONLY if every one holds, `ledger.py --dir <scratchpad> attest <round>`;
   then `ledger.py --dir <scratchpad> status` prints the state — CONVERGED / READ-ONLY FLOOR / OPEN (with
   the open set) — and only a printed CONVERGED or READ-ONLY FLOOR is a stop. Every stop is
   `ledger.py --dir <scratchpad> stop <reason> [--note ...]`, which writes the `STOP:` line; then report:
   where the thread stands, which claims are now measured, which the critic was still pushing on, and (on
   `converged`) that the fact base is settled — the next move is the root analysis + a fix under the user's
   per-rule-1 green-light (that green-light is theirs to give, not yours to assume). Do not stop to report
   between rounds: a round summary is not a stop condition.

## The loop, and the convergence bars

The loop runs **within this main session**: for each round `i` — assemble the brief (step 2, now
including YOUR answers from rounds 1..i-1), spawn a fresh critic (step 3), relay its questions and
**answer them yourself in the open** (step 4), append the round to `<scratchpad>/qf_thread.md`, then
continue — until a stop condition from THE ONE STOP LIST. There is no round cap other than the safety
ceiling and the user's own `/qf N`. (History, 2026-07-09: a pass stopped at 6 rounds "converged", shipped
a fix that had only mapped ONE half of the user action, and REGRESSED into a dupe. A non-trivial
question/design/impl pass has wanted 15-22 rounds; fewer rounds = a half-mapped fix ships.)

- **Convergence is the critic's verdict AND the bars below, never a round count.** A user cap (`/qf N`)
  or the safety ceiling reached while the critic is still returning material questions means the thread is
  UNSETTLED — you have NOT converged. Do NOT declare it done and do NOT build: present the residual
  questions. **Treating "I hit a cap" as "converged" is the exact failure that shipped the dupe — never
  do it.**
- **`converged: true` is only real if the PRIMARY has completed the map.** Before you accept convergence,
  confirm you have mapped the WHOLE problem — for a sync bug, EVERY wire event that EACH user action emits,
  on BOTH peers (per `[[feedback-map-all-wire-events-before-fixing-missing-sync]]`). If a whole action-half
  (e.g. the GRAB when you only studied the DROP) was never interrogated, the thread has NOT converged no
  matter how calm the critic sounds — steer a round at the unmapped half.
- **`converged: true` is INVALID if the design no longer answers WHAT THE USER ASKED.** Before accepting
  convergence, re-read the brief's opening section and state, in one line, how the converged design
  resolves the symptom **in the user's own words**. If you cannot do that without redefining their
  complaint, the thread has drifted, not converged. Three specific drifts to check: (a) the design fixes
  a nearby, more tractable symptom rather than the reported one; (b) the user reported N things and the
  design silently covers fewer, with the remainder neither built nor FILED with its own hook; (c) the
  user's symptom noun was translated into an architecture noun and the translation was assumed rather
  than measured — a wrong translation yields a perfect fix for the wrong entity, and it will pass every
  internal-coherence check the critic can run. This is not a mandate to obey the user's phrasing
  literally over a measured shared root — it is a mandate to be able to SAY which of the two you are
  doing, and why, before you build. (Born 2026-07-22, user request: the critic only ever sees a brief the
  primary wrote about its own work, so nothing in the ritual was positioned to ask "is this still the
  ask?")
- **Stop on `converged` ONLY** when the critic returns `converged: true` AND every bar in this section
  holds — then report convergence; do not manufacture rounds past it.
- **`converged: true` is INVALID while a load-bearing inferred fact has an available-but-undone cheap
  measurement.** If any claim the design rests on is tagged `inferred`, and a read-only measurement that
  would move it to `measured` exists and is fast (read the two function bodies, grep the log, disasm the
  site) — the thread has NOT converged, no matter how calm the critic sounds. "It's agnostic / doesn't block
  / likely right / re-deriving is inconvenient" is NOT convergence — it is the exact rationalization that
  lets a crutch ship on an un-checked assumption. DO the measurement, THEN re-judge. (Born 2026-07-13: a
  pass nearly cemented a containment design on RE-doc prose because "the runtime counter will settle it
  anyway" — the cheap body-walk that would have settled it up front, and de-ambiguated the counter's failure
  modes, was skipped as "more than corroboration warrants." It was inconvenience mislabeled as scope.)
- **`converged: true` is INVALID while a MIGRATION design has not enumerated EVERY identity map keyed on the
  migrating entity.** If the design repoints/rebinds/re-keys/moves an entity's identity, convergence
  requires that you have LISTED every map/table/index/authority-record keyed on that entity and shown, per
  map, that the operation updates it (or intentionally defers it with the consumers gated). A calm critic `converged: true` does NOT settle this if the map set was never enumerated — the critic escalates within the
  frame it is handed, so a parallel identity map absent from the brief is invisible to it and its silence is
  not evidence of completeness. Grep the entity's id/type across the tree and prove the set is whole BEFORE
  accepting convergence. (Born 2026-07-13: an 11-round repoint DESIGN pass converged with the critic saying
  "that holds" while a SECOND host-only identity table finalized late; only the user, from outside the
  frame, asked "is the eid the only identity map?" — see [[feedback-qf-enumerate-identity-maps-on-migration]].)
- **`converged: true` is INVALID while the converged design is the SECOND-BEST one, chosen to avoid a large
  change.** The user's standing green light (2026-08-24) makes scope a non-constraint: *"Я даю зеленый свет
  даже на самые радикальные решения, если они окажутся верными и правильными."* So before accepting
  convergence, answer in one line: *is there a bigger fix that is MORE correct?* — dissolve the module,
  invert the shared primitive, retire the lane whole, change how the game behaves. If yes, the thread has
  not converged; it has settled for the tractable option, which is the same failure as a suppressive
  filter wearing a design's clothes. The green light is about SCOPE and BEHAVIOUR, never about skipping a
  measurement or shipping a bug — a radical fix still owes every gate a small one owes.
- **`converged: true` is INVALID while a load-bearing NAMED PRIMITIVE the design hangs on is `carried-framing`
  you never code-verified.** If the design leans on "the existing X" — a barrier, arm-record, queue, lane,
  poll, mechanism — you INTRODUCED as a label in an earlier round, convergence requires you have OPENED the
  code and confirmed that thing exists AS described (ONE mechanism not several fused, this lifetime, this
  shape). A fresh critic inherits your brief's nouns as fact and cannot see a laundered label, so its silence
  is NOT evidence the primitive is real. Code-verify the 1-2 nouns the design most depends on BEFORE accepting
  convergence. (Born 2026-07-14: a stash design converged-feeling across rounds on "the existing two-phase arm
  record"; it was four distinct converge mechanisms — see [[feedback-qf-challenge-carried-framing-not-just-the-frame]].)
- **After a material REFRAME, do NOT auto-continue the loop — SURFACE to the user first.** A reframe = the
  design changes SHAPE mid-pass (a premise flips, a primitive turns out to be several, the model is replaced,
  a suppression dissolves). The rounds AFTER a reframe re-harden the NEW framing with the exact
  self-summarization bias that hardened the old one: you write the next brief, the fresh critic inherits it
  blind, and nobody re-derives the earlier answers the reframe undermined. The single most valuable check at
  that moment is an EXTERNAL holder of the full history + the raw artifacts — the user — not another fresh
  critic reading your brief. So on a reframe, stop the auto-loop, present the reframe + what it invalidates,
  and let the user inject before continuing. (Born 2026-07-14: an impl pass reframed twice — round 1 the eid
  model, round 4 the poll discovery — and BOTH times the user's injection, not the next agent, kept it honest;
  every agent round in between hardened a framing that the next measurement moved.)
- **Between rounds, actually DO the cheap measuring a question demands** if it is read-only and fast (grep a
  log, read the current code, disasm a site) — the value of the primary being in the loop is that it can
  answer with a fresh measurement, not just reason. This is MANDATORY, not optional, for any inferred fact
  the design leans on (see the convergence rule above). Do NOT build/deploy/run the game inside a `/qf` round
  (that is a separate main-session step under the user's green-light).
- **Present the whole transcript at the end:** every round's questions + your answers, then the stop
  reason — where the thread converged, or the residual questions and what would settle them. The user
  reviews the full exchange and injects what only they know; a further `/qf` continues the same thread.

Each round still uses a FRESH critic agent that despawns — the loop lives in the main session (you), not in
any agent's memory. This is the key difference from `/qf-workflow`: there, the loop and BOTH parties are
subagents; here, one party is the real primary answering in the open.

## Phase the ritual: question -> design -> implementation (SEPARATE /qf passes)

**A single `/qf N` that converges is NOT the end of the ritual for a problem — it is the end of ONE phase.**
`/qf` interrogates whatever surface the brief presents. As the work moves from *understanding the failure*
to *choosing the fix* to *wiring it*, the surface changes — so each phase earns its OWN /qf pass with a
fresh brief centered on that phase. Do not treat "the fact base converged" as "the design is vetted": a
settled root does not mean the fix holds the class, avoids a crutch, or survives the seams. Run `/qf` again
on the design, and again on the implementation.

The phases (run each to convergence — the critic's `converged: true` plus your bars — before moving on):

1. **QUESTION / fact-base pass.** `/qf N` on the investigation: what is the root, which claims are
   `measured` vs `inferred`, de-braid the repro. Converges when the facts are settled. (Most first `/qf N`
   runs are this.)
2. **DESIGN / solution pass.** Once a design exists, run a SECOND `/qf` pass whose brief is the DESIGN
   itself — the fix as the surface to interrogate: does it hold the WHOLE class or is it a site-patch; is
   any part a RULE-1 crutch / wrong-layer patch; does it survive the seam blast-radius; identity-at-birth;
   authority direction; does it generalize (rule-of-three) or will you re-implement it thrice. This pass is
   as important as the first — a converged root with an un-vetted design is exactly how a crutch ships.
3. **IMPLEMENTATION pass (when the wiring has real choices).** A `/qf` on the concrete implementation: which
   seam to hook, cache index, defer window, eviction, the one must-measure-before-build probe. Converges on
   a build-ready plan (+ any read-only probe that gates the wiring).
4. **DIFF pass (after the code exists, BEFORE any smoke or handoff).** The critic reads the DIFF that will
   SHIP, not your prose about it — the one pass on record with that read target found 12 defects in 4
   rounds where two brief passes (21 + 17 rounds) found none (`docs/QF_ARC.md` L1). Licensed by the standing
   /qf-before-implementation rule (`[[feedback-qf-before-implementation]]`): a question-only critic, not a
   review agent. The brief is thin in ONE sense — no CLAIMS / HYPOTHESIS prose — and keeps: the ask (WHAT
   THE USER ACTUALLY ASKED FOR), the design's claimed invariants as a numbered list of FALSIFIABLE sentences
   (e.g. *"3. no client-authored `PropDestroy` reaches `ApplyDestroy` without passing `arbiter::Validate`
   first"*), the RANGE with its hash, and PRIOR ROUNDS as the ledger's rows (so the critic escalates instead
   of repeating). Range: uncommitted work = `git diff HEAD` (plus `git diff --cached` if staged); committed
   = `<base>..HEAD`; the brief states which and carries `git diff <range> | sha1sum` — a changed hash forces
   a re-read and is noted on the round (same pass, new round). Open it with `ledger.py pass diff`: in this
   phase EVERY question must anchor on a `path:line` in the diff or in a file it touches, or on an
   omission's zero-hit command (the most valuable class — a missing late-join row, a missing gate);
   `ledger.py append` discards a question without one (that question, not the reply). A converged reply
   carries the last lookup that found nothing. Converges when the critic returns `converged: true` and the
   ledger prints CONVERGED. The pre-deploy checklist is untouched; this pass sits before it.

**Depth per phase.** "Enough" is the critic's genuine `converged: true` WITH a complete map — not a fixed count,
and never a cap. Passes on record have converged anywhere between 5 and 43 rounds; do NOT stop at 6 and
call it converged — that premature stop is what shipped a half-mapped fix (2026-07-09) — and don't
manufacture empty rounds once it genuinely converges; convergence is the critic holding + the map being
complete, whichever round that lands on.

**Each phase is a distinct THREAD topic** (per the archive guardrail below): a design pass interrogates a
different surface than the question pass. Same-investigation continuation appends to `qf_thread.md`; when
you switch from the QUESTION pass to the DESIGN pass on the same problem, either keep appending (the design
brief cites the converged facts) or archive-and-restart if the question thread has grown long — the point
is the design pass's PRIOR ROUNDS reconstruction pulls the design rounds, not a stale question round.

(Worked example, 2026-07: the world-settings feature ran three passes — settings-sync QUESTION (5 rounds)
-> F1-panel DESIGN (2) -> IMPLEMENTATION (3) — each converging before the next. The rock-sync feature ran a
QUESTION+DESIGN pass of 6 rounds that reframed F1 and generalized F2 before any wiring. The design pass is
where the crutch gets caught, not the question pass.)

## Guardrails

- **Gate every reply mechanically, never by eye.** `tools/qf/verify_proof.py --reply <file>` checks the
  proof-of-read fragments against the docs (whitespace- and markdown-normalised, 4-12 words each) and
  `tools/qf/ledger.py append` checks the schema and re-verifies every anchor; a non-zero exit from either
  means the critic did NOT open what it claims and the reply is discarded and re-spawned. Two failures in a
  row = `ledger.py stop format-failed` and hand back. Do NOT relay the proof fragments to the user (a gate,
  not signal) — relay the credit line + the questions. (Before this gate existed the check was done by hand
  and recorded in ~5 % of rounds — `docs/QF_ARC.md` E5.)
- **The critic never designs; you never let a design come back from it.** If the agent replies with
  statements, a plan, or a fix — in ANY field of the block, not only `q`: a `howToMeasure` that changes
  something, a `credit` that argues, a `convergenceRationale` in prose — discard it and re-spawn asking for
  questions only. Design is yours, here.
- **Read-only.** The critic must not edit/build/run; you do not build or deploy inside a `/qf` round either
  — `/qf` is a reasoning round, not an execution step.
- **Fresh agent per round, primary carries the thread.** Do not keep a persistent critic via SendMessage —
  the user's model is a clean agent each time, fed fresh context.
- **Persist the thread; the ledger persists the critic VERBATIM.** `ledger.py append` writes each round's
  question table AND the critic's JSON unchanged under `### Critic (verbatim)` into
  `<scratchpad>/qf_thread.md`, and the state into `<scratchpad>/qf_ledger.json` (one writer for both). You
  append the rest by hand around it: the brief you sent (or its delta) BEFORE the round, your answers
  AFTER it — never a paraphrase of the critic, never a title you gave its question. The verbatim copy is
  what makes the critic measurable after the fact (`docs/QF_ARC.md` E3: 64 thread files held the
  primary's paraphrase of the questions, so the critic's own citation rate could not be measured at all).
  A `/qf` after a `/compact` reconstructs PRIOR QF ROUNDS from these two files; read them back in step 2
  if the session was reset (`ledger.py status` prints the open set).
- **Archive the old thread when `/qf` starts on a NEW problem.** `qf_thread.md` + `qf_ledger.json` are ONE
  single-topic log. Before the first round of a `/qf` on an investigation UNRELATED to whatever they
  currently hold, RENAME both — `qf_thread_<old-topic>_ARCHIVED.md` and `qf_ledger_<old-topic>_ARCHIVED.json`
  — and start fresh with `ledger.py pass <phase> --topic <new topic>`. Otherwise the step-2 PRIOR QF ROUNDS reconstruction (and a post-`/compact` reload) pulls in the stale
  topic's rounds and the critic escalates against the wrong thread. Same-topic continuation (another `/qf` on
  the live investigation) appends as normal — only a genuine topic switch triggers the archive.
- **Don't over-process a micro-step.** Per QUESTION_FORM_AGENT.md, sometimes the right critic answer is a
  `converged: true` with an empty question list — do not manufacture doubt to keep the loop spinning.
- **This is NOT `/qf-workflow`.** `/qf` = the manual ritual with YOU, the real primary, in the loop
  answering visibly, design allowed in the main session. `/qf-workflow` = the automated background
  fact-base loop where BOTH parties are subagents (no primary, no design, SEARCH + AUDIT only) — it
  structurally cannot have the primary answer, because a background loop can't reach the main session. Pick
  `/qf` when the user wants the primary to trade rounds with the critic; `/qf-workflow` when they want the
  logs measured to a fact base hands-off.
