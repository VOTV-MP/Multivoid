# Standing security rules

Five rules this folder enforces. Each was born from a specific failure; the failure is named so the
rule is not mistaken for taste. **A code review may cite these by number.**

---

## Rule S1 — Never write a comment asserting a security control that does not exist

**Two were found in one day**, and in both cases the false comment is *why* the gap persisted: a
later reader trusted it and moved on.

| Where | The comment claims | Reality |
|---|---|---|
| `master.rs:498-499` | "the real admission gate is the game-layer post-Connected join-secret challenge" | `[V]` `joinSecret` appears **nowhere in the tree** except a comment in `session.h:102` describing it as future work |
| `session_trashcarry.cpp:61-62` | "Per-entry float validation + the ctx-freshness gate happen at the game-thread apply" | `[V]` **fused claim, half false.** The ctx gate is real (`trash_clump_pose_stream.cpp:49`); the float validation does not exist anywhere on that path |

Note the second shape especially: a comment that is *partly* true is worse than one that is wholly
false, because spot-checking it confirms the true half.

**The rule:** if a control is absent, the comment must say so. `event_dispatch_entity.cpp:259-264`
does this correctly — copy that shape. Deleting a false comment is **never** blocked on building the
control it describes; the deletion ships first.

Lesson: `[[lesson-false-security-comment-worse-than-none]]`.

---

## Rule S2 — Validate before relay

A forged op must not be fanned out to other peers ahead of the host's own validation. Once relayed,
the damage is multi-peer and the host's later rejection cannot recall it.

Current violation: `TRACKER` **A3** — `session.cpp:454` relays before `:463` validates.

---

## Rule S3 — Caps belong on the apply side, not only the send side

Every "the peer is a well-behaved copy of this build" assumption is a vulnerability. A send-side cap
constrains **our** sender; the attacker does not run our sender.

**This is the root shape of the entire Critical/High block in `TRACKER.md`** — W1, W2, W3, W4, W5,
W8 are all one mistake repeated at six seams.

Diagnostic question for any new lane: *"if the peer sends the largest value this field can hold,
what does the receiver allocate?"*

Lesson: `[[lesson-send-side-caps-are-not-caps]]`.

---

## Rule S4 — Escaping belongs at the render layer

Do not mangle game-facing strings to protect a consumer that has not been written yet. Stripping
`< > & " '` in `clamp_str` would break legitimate lobby names and still not protect an
attribute context or a JSON island.

The render rules are written in `PLAN_05_WEBSITE.md` **before the site exists**, which is the point.

---

## Rule S5 — Verify a handed-down finding before fixing it

Everything tagged `[A]` came from an audit agent reading code. **Re-read the site yourself before
building on it** — per `[[feedback-verify-handed-down-measurement-before-building]]`.

This is not ceremony. On 2026-07-20, verifying five `[A]` rows produced three corrections that
changed the fix:

- **W1** — the tracker's proposed fix cited `kMaxSaveBlobBytes`; `[V]` **that constant does not
  exist**. The fix has to create it.
- **W3** — the growth is real, but there *is* a sequential index check (`save_transfer.cpp:367-372`),
  so the attacker must send chunks in order. Changes the exploit shape, not the severity.
- **W6** — the false comment is a **fused** claim, only half wrong (see Rule S1).

A fix plan built on an unverified row inherits the agent's blind spots.

---

## Rule S6 — A security fix needs a Principle-8 late-join answer

CLAUDE.md principle 8: mid-activity join is always handled. A validation that rejects a *legitimate*
peer who joined at an awkward moment is not a security fix — it is an availability bug wearing one.

Concretely: making the `device_occupancy` claim table enforcing (**A4**) must answer *"what happens
to a claim whose holder departed mid-activity?"* before it ships, or the first fix produces a
permanently locked desk. That answer is owed in `PLAN_03_AUTHORITY.md`.

---

Next: `TRACKER.md` (the findings) · `EXECUTION.md` (the board).
