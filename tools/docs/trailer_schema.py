"""trailer_schema -- the ONE definition of the `Docs-Census:` trailer's columns and what each is FOR.

WHY THIS FILE EXISTS. The trailer's vocabulary lived in FOUR hand-written lists with no check between
them -- `status_census.py`'s `RATCHET_COLS`, its `format_trailer` order, and `docs_census_gate.py`'s
own `RATCHET_COLS` (a byte-identical copy) plus `VERDICT_COLS` -- while the sibling `/qf` skill had
already solved exactly this with `tools/qf/schema_sync.py --check`. `[V]` 2026-09-03: the trailer
wrote 23 columns and the gate read back 15; the EIGHT unread were `labels`, `cited-dead`, `accretion`,
`sweep-cursor`, `sweep-cycle`, `research-base`, `new`, `foreign`, and six more report-only numbers were
about to be added. That is ONE missing rule, not fourteen omissions.

THE RULE: every column declares its KIND at birth.

  IDENTITY    a sha or a base the gate checks for tiling / distinctness.
  VERDICT     a hand-verdict count; the gate asserts sum(VERDICT counts) == rows.
  RATCHETED   monotone non-increasing across closes -- and ONLY for a number the CLOSING SESSION CAN
              MOVE. A whole-corpus citation count is NOT one: `[V]` a single rename touches 5 tracked
              docs against a close radius of ~50, so an ordinary extraction would refuse a session
              over work it did not do (round 19, Q4).
  MONOTONE    a CUMULATIVE total that may never DECREASE -- the opposite direction to RATCHETED, and
              a distinct kind for that reason. It exists because CI cannot read the private history
              where the underlying ledger lives, so the only property it can check on a running total
              is that a close never un-records what an earlier one recorded.
  GATED       a named REFUSAL path exists for it (in the close, in CI, or both).
  REPORTED    printed and never enforced -- declared so on purpose, so "nothing reads it" is a
              decision on the record rather than an omission nobody noticed.

Both `status_census.py` and `docs_census_gate.py` import THIS; the gate additionally asserts that every
column present in a trailer carries a declared kind, so an undeclared column cannot ship.
"""

IDENTITY = ("base", "census", "research-base")
VERDICT = ("still-open", "actually-done", "stale-done", "partial", "still-true", "not-a-label",
           "drift-ok")
RATCHETED = ("ro-bytes", "ro-longest", "mem-over200", "memref-dead", "wikilinks-dead",
             "pairing-unref", "pairing-dead", "accretion")
MONOTONE = ("resolved", "flips")
GATED = ("rows", "cited-dead")
REPORTED = ("labels", "cite-drift", "running-totals",
            "ro-moved", "sweep-cursor", "sweep-cycle", "new", "foreign")

# The written order. `rows` leads the counts because the VERDICT identity is stated against it.
ORDER = ("base", "rows", "labels") + VERDICT + (
    "cited-dead", "cite-drift", "running-totals",
    "accretion", "resolved", "flips",
    "ro-bytes", "ro-longest", "ro-moved", "mem-over200", "memref-dead",
    "wikilinks-dead", "pairing-unref", "pairing-dead",
    "sweep-cursor", "sweep-cycle", "census", "research-base", "new", "foreign")

KIND = {}
for _name, _cols in (("identity", IDENTITY), ("verdict", VERDICT), ("ratcheted", RATCHETED),
                     ("monotone", MONOTONE), ("gated", GATED), ("reported", REPORTED)):
    for _c in _cols:
        KIND[_c] = _name

TARGETS = {"ro-bytes": 58 * 1024, "ro-longest": 15, "mem-over200": 0, "memref-dead": 0}

# Every column in ORDER has a kind, and every kind's column is in ORDER: the two lists cannot drift.
assert set(ORDER) == set(KIND), (set(ORDER) ^ set(KIND))
