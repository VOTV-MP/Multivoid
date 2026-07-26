# Release notes (the changelog authority)

One file per build number: `b<N>.md`. Its content becomes the `## What's new`
section of release `v<game>-b<N>[-dev]`'s page, rendered by `New-ReleaseBody`
(ledger_lib.ps1) — the ONE body writer used by publish, retro regeneration, and
recovery republish alike.

Rules (enforced where noted):

- **The git-tracked file is the authority; the release page is a publish-time
  copy.** A published body is written once and never machine-rewritten (a re-run
  on a published tag is an ALREADY_PUBLISHED no-op).
- **Write-once after publish.** Enforced by ledger_lint's NOTES_DRIFT check:
  every live release's `## What's new` section must equal its notes file, so a
  post-publish edit of the file fails the next release run. A sanctioned
  correction edits BOTH: fix the file, then regenerate the live body from it
  (`gh release edit` with a body rebuilt by `New-ReleaseBody`), then run
  `ledger_lint.ps1` locally.
- **Format** (enforced by the judge's NOTES_OK check, pre-build): non-empty;
  plain markdown bullets; NO leading `#` heading (the template owns the
  heading); no line may match the machine-key grammar (`source:` / `sha256:`).
- **Content discipline** (human-gated — no lint can judge prose truth): verbs
  are status claims (`lesson_public_claim_surfaces_carry_verdict_discipline`).
  Write what the build DOES, anchored to the protocol.h consume comment and the
  git range; never credit a fix that landed after the tag. The notes text is
  shown to the user at RELEASE.md step 0.5 before the tag is pushed.
- Missing notes file for the tagged N = the judge refuses (NOTES_OK), stateless:
  add the file on main, re-dispatch the same tag.
