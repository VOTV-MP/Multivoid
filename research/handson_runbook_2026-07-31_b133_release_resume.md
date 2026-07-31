# RESUME: finish the v0.9.0n-b133-dev release (fingerprint recovery)

**Status 2026-07-31 12:50 local: the tag is pushed, the judge PASSED every check and voted
PUBLISH, and the run stopped on a FINGERPRINT refusal. Nothing is burned. Numbers never burn
from an image/build-path roll (RELEASE.md, "When something goes wrong").**

## What already happened

| thing | state |
|---|---|
| tag `v0.9.0n-b133-dev` | pushed, on `2f32d1affb9bdf10447d9145c166b4832d1e449f` |
| consume row (N=133) + proto 134 | pushed, commit `01210231` |
| `tools/release/notes/b133.md` | on main, judge `NOTES_OK: PASS` |
| release run `30610396964` | **failed** at the fingerprint step only |
| cacheless build `30610531855` | dispatched 06:44 UTC — **completes with or without this PC** |
| issue #5 | commented + CLOSED; a release link is still OWED there |

The refusal, verbatim:

```
FINGERPRINT: ok   -- msvc_toolset  = 14.51.36231
FINGERPRINT: ok   -- windows_sdk   = 10.0.26100.0
FINGERPRINT: FAIL -- build_core_sha256 committed '1707b4b8b78f27795f7639411cc37f9cc61d90f0f6e0e9a9bbbc487cde84f1a8'
                                      vs live 'db42a196fc1e150309ec4f98d068f552d3c67217da9da02cba4353d7841ec149'
FINGERPRINT: build path changed since the last proven-runnable smoke -- re-smoke + re-commit the fingerprint.
```

**Cause (measured, not guessed):** `build-core.yml` was last touched by `0d84cc5a` (the imgui
flip added a gate step); the committed fingerprint is from `8aa241ce`. So the mismatch is a
REAL build-path change, exactly what the gate is for — not a spurious refusal.

## The four steps (~10 min, the expensive CI build is already banked)

1. **Get the artifact from the ALREADY-COMPLETED build** — do NOT re-dispatch, it is 34 min:
   ```
   gh run view 30610531855 --json status,conclusion
   gh run download 30610531855 -D /tmp/b133ci
   ```
   It carries the built DLLs and `fingerprint-dump.json`.

2. **Smoke the CI BYTES locally.** This is the whole point of the gate — the assertion being
   committed is "this build path produces runnable bytes", and only a real run can make it.
   Deploy the artifact's `multivoid-0.9.0n-133.dll` + `xinput1_3.dll` into the four installs
   (or run `tools/deploy-all.ps1` after copying them into `build/votv-coop/Release/`), then:
   ```
   VOTVCOOP_RUN_CONFIG_SELFTEST=1 python tools/mp.py smoke
   ```
   Require: verdict PASS, and on BOTH peers `config-selftest: DONE fail=0`,
   `repertoire selftest: PASS`, `font selftest: DONE fail=0`.

3. **Commit the fingerprint** (only after step 2 passes):
   ```
   cp /tmp/b133ci/**/fingerprint-dump.json tools/release/fingerprint.json
   git add tools/release/fingerprint.json && git commit && git push origin main
   ```

4. **Re-dispatch the release on the SAME tag** (stateless re-run, no retag, no new number):
   ```
   gh workflow run release-core.yml -f tag=v0.9.0n-b133-dev
   gh run watch <id>
   ```

## After it publishes

- Append the `published` row to `tools/release/LEDGER.tsv`:
  `published<TAB>133<TAB>0.9.0n<TAB>v0.9.0n-b133-dev<TAB>2f32d1affb9bdf10447d9145c166b4832d1e449f<TAB><date>`
  and push it.
- **Post the release link on GitHub issue #5** — the closing comment promises it
  ("It ships in the next build; I'll drop a link here once it's published").
- Dev release, so step 6/7 of RELEASE.md (the `COOP_LATEST_*` env on the box) do NOT apply.

## Do NOT

- Do not bypass the fingerprint check to get the release out. It gates "proven runnable" on a
  build path that genuinely moved; committing the fingerprint without step 2 is asserting
  something nobody measured, on bytes that go to real players.
- Do not retag or mint a new number. The refusal is stateless; `v0.9.0n-b133-dev` and its
  consume row are valid and the judge already voted PUBLISH on them.
