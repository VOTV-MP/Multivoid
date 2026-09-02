---
name: qf-workflow
description: >
  Launch the Question-Form adversarial root-finding workflow: ingest the host + client run logs, then
  interrogate -> measure open questions to convergence (SEARCH + AUDIT only; no design). Invoke when the
  user says "/qf-workflow", "run the question-form workflow", "run qf-workflow <one sentence>", or wants
  the two-agent adversarial loop over the current logs without hand copy-pasting. Project: VOTV_MP.
---

# /qf-workflow — automate the Question-Form adversarial loop

You are launching the background workflow that replaces the manual copy-paste ritual of
`docs/QUESTION_FORM_AGENT.md`. Read `docs/QUESTION_FORM_WORKFLOW.md` first if you have not this session —
it defines the roles and the ONE hard boundary.

**The boundary (do not cross it):** this loop uses ONLY SEARCH (ingest logs, measure) and AUDIT (the
Question-Form critic) agents. It converges on a **verified fact base + residual unknowns** — NEVER a
designed or committed fix. Design stays in THIS main session where the user sees it
(`feedback_no_design_architect_agents`, OPUS §5). Do not extend the run to "also design the fix."

## Steps

1. **Take the user's one sentence** (everything after `/qf-workflow`) as the `statement`. If the user gave
   nothing, ask for one plain-text sentence naming the failure under investigation (and any de-brief the
   logs can't show — e.g. "this run mixed a join with manual pile-throwing") rather than launching blind;
   the human aside is exactly what de-braids a confounded repro.

2. **Launch the workflow** (it runs in the background; do not block):
   ```
   Workflow({
     scriptPath: "tools/workflows/qf_root_loop.js",
     args: { statement: "<the user's sentence>" }
   })
   ```
   Defaults inside the script: logs = `Game_0.9.0n_HOST` + `Game_0.9.0n_CLIENT_1`
   (`WindowsNoEditor/VotV/Binaries/Win64/votv-coop.log`), `maxRounds: 4`. Only pass `args.logs` /
   `args.maxRounds` if the user asked to change them.

3. **Report the launch**: give the user the runId and tell them to watch `/workflows` (live tree +
   per-round narrator line). It runs in the background — you will be notified when it converges or caps.

4. **When it completes, verify the PROOF-OF-READ, then relay the fact base — do not bury it.** The result
   carries `lastCriticProofOfRead` (`{qfDoc, opusDoc}`) — two verbatim fragments the critic quoted from
   `docs/QUESTION_FORM_AGENT.md` and `docs/OPUS_48_DISCIPLINE.md`. Confirm both fragments actually appear
   (verbatim) in their respective files before trusting the round; a missing, fabricated, or non-matching
   fragment means the critic ran on the prompt's inline summary alone — flag the run as low-confidence and
   relaunch. Do NOT relay the proof fragments to the user (they're a gate, not signal). Then read
   `terminalReason` and report it honestly:
   - `converged` — the fact base is settled.
   - `read-only-floor` — all read-only measurement is exhausted; the remainder is runtime-gated. This is a
     SUCCESS, not a failure to converge: relay `runtimeProbesNeeded` (the named next probe, e.g. an
     instrumented rebuild) as the concrete next step. Do NOT describe it as "did not converge."
   - `capped` — `maxRounds` hit while claims were still measurable read-only; the honest "ran out of room"
     ending — present the residual unknowns and suggest a relaunch.
   Then summarize the measured evidence (each finding WITH its verbatim citation and confidence) and the
   residual unknowns (with the missing tool each needs). Lead with what is now MEASURED vs what is still
   INFERRED. Then stop — the root analysis and any rule-1 fix are the user's call to make next, in this
   visible session.

## Guardrails

- Never mark the workflow's output "verified root" — it is a settled evidence base; a converged fact base
  can still support a wrong design.
- The workflow reads logs/code read-only; it does not build, deploy, or run the game.
- If the run caps without converging, that is a legitimate result: present the residual unknowns; the
  next move is the user adding what only they know, then a relaunch (`resumeFromRunId` re-uses unchanged
  agent results). `read-only-floor` is distinct from `capped` — it means the run measured everything it
  could read-only and named the runtime probe the remainder needs; relay it as a result, not a shortfall.
