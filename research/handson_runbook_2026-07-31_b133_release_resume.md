# b133 SHIPPED as a DELIBERATE PROCESS EXCEPTION — and the fingerprint debt it leaves

**Status 2026-07-31: `v0.9.0n-b133-dev` IS PUBLISHED, from LOCALLY BUILT bytes, by explicit
user decision. Both ledger rows are closed. The CI release lane was NOT used, and the reason is
recorded on the release page itself.**

## What happened, in order

1. Tagged `v0.9.0n-b133-dev` on `2f32d1af`; consume row + proto 134 pushed (`01210231`).
2. Release run `30610396964`: the judge **PASSED every check and voted PUBLISH**, then the run
   stopped at the fingerprint step:

   ```
   FINGERPRINT: ok   -- msvc_toolset = 14.51.36231
   FINGERPRINT: ok   -- windows_sdk  = 10.0.26100.0
   FINGERPRINT: FAIL -- build_core_sha256 committed '1707b4b8...' vs live 'db42a196...'
   ```

   **Cause, measured:** `build-core.yml` was last touched by `0d84cc5a` (the imgui flip added a
   gate step); the committed fingerprint dates from `8aa241ce`. A REAL build-path change — the
   gate did its job.
3. The documented recovery needs a local smoke of the CI bytes. The user was leaving for
   **three weeks** and chose, explicitly, to make this release an exception rather than leave
   the fixes unreleased for that long.
4. So the shipped bytes were built HERE, from the tagged commit:
   - `git checkout v0.9.0n-b133-dev` -> build -> `multivoid-0.9.0n-133.dll` + `xinput1_3.dll`
   - deployed and smoked: **PASS**, and on BOTH peers `config-selftest: DONE fail=0`,
     `repertoire selftest: PASS`, `font selftest: DONE fail=0`
   - **the deployed DLL's sha256 was verified byte-identical to the published asset**
     (`8b512623...`) — the bytes that were smoked ARE the bytes that shipped
   - the GitHub issue #5 fix inside them was hands-on verified by the user earlier the same day
5. Body written by the canonical `New-ReleaseBody` (so the `source:` / `sha256:` machine keys and
   the `## What's new` section stay parseable), with the exception stated in an `ExtraLines`
   block at the top of the release page. `ledger_lint`: **0 FAIL**.
6. `releases/latest` -> 404, i.e. the dev prerelease is correctly NOT surfaced as latest.

## THE DEBT THIS LEAVES (do this before the NEXT release)

**The fingerprint is still stale — the next release will refuse the same way.** The expensive
half is already done and banked:

- Cacheless build **`30610531855`** was dispatched 06:44 UTC and completes on GitHub regardless
  of this machine. Its artifact carries `fingerprint-dump.json` and persists 90 days.

So, next session, BEFORE tagging b134:

```
gh run view 30610531855 --json status,conclusion
gh run download 30610531855 -D <dir>          # do NOT re-dispatch: it is a 34 min build
# deploy the artifact's DLL pair, then:
VOTVCOOP_RUN_CONFIG_SELFTEST=1 python tools/mp.py smoke
# require PASS + the three selftest lines on BOTH peers, THEN:
cp <dir>/**/fingerprint-dump.json tools/release/fingerprint.json
git add tools/release/fingerprint.json && git commit && git push origin main
```

That returns the release lane to normal. **The next release goes through CI** — this exception
is one release wide, and the release page says so publicly.

## What NOT to conclude from this

- **This is not a precedent.** It was a dated, reasoned, user-made call with the tradeoff written
  on the public release page, not a discovery that the CI lane is optional.
- **Do not "fix" the fingerprint by committing the dump without the smoke.** The gate asserts
  "this build path was proven runnable"; committing it unsmoked asserts something nobody
  measured, on bytes that go to real players. The reason b133 was safe to ship locally is that
  its bytes WERE smoked and hash-matched — that evidence is what replaced the CI rebuild, not
  the absence of a check.
- Do not retag or mint a new number for b133. It is published and its ledger rows are closed.
