# Release checklist — Multivoid

> Created 2026-07-19 with the v122 version-identity lane. ZERO releases have shipped yet;
> this is the mechanism that keeps the identity honest when they start. The identity model
> is the Paper pair (game target + build number) — see
> `research/findings/architecture-audits/votv-version-identity-v122-DESIGN-2026-07-19.md`.

A RELEASE is: a tagged build whose artifact is `multivoid-<game>-<build>.dll`
(e.g. `multivoid-0.9.0n-122.dll`), published with its SHA256 (the Paper downloads shape).

## Per-release steps

1. **Bump the build number** — `kProtocolVersion` in
   `src/votv-coop/include/coop/net/protocol.h`. EVERY release bumps it, even without a wire
   change (the build number is the release identity; two different released DLLs must never
   share one). A wire change already forces this via the standing rule.
2. **Game target current?** `VOTVCOOP_GAME_TARGET` in `src/votv-coop/CMakeLists.txt` must
   match the VOTV cook the release targets ("0.9.0n" style, no dashes).
3. **Build Release** — the artifact name self-derives (CMake parses protocol.h). Verify:
   `build/votv-coop/Release/multivoid-<game>-<build>.dll` exists with the expected numbers.
4. **Publish** the DLL + `xinput1_3.dll` + the artifact's SHA256 on the release page
   (github.com/VOTV-MP/Multivoid/releases — the master's /v1/latest default points there since `dcc988c7`).
5. **Update the master's LATEST record** (informational toast only — it NEVER gates a join;
   forgetting this step's worst case is a missing toast, not a broken join):
   on the coop box edit `/etc/coop-master.env`:
   `COOP_LATEST_PROTO=<build>`, `COOP_LATEST_MOD=<game>-b<build>` (display tag),
   then `systemctl restart coop-master`. Unset/0 = "no released record" = clients stay
   silent (the pre-release state).
6. **Tag** the commit `v<game>-b<build>`.

## Invariants the code enforces (do not re-implement per release)

- Join gate = byte-equality on (game target, build): browser pre-flight popup, Join-seam
  wire gate, header backstop. Old cohorts keep playing among themselves (per-lobby equality,
  never latest-only — the Minecraft rule, user directive 2026-07-19).
- The loader scans `multivoid-*.dll`, loads the highest build, and pops the in-game
  "MOD INSTALL PROBLEM" dialog when several version files coexist — a manual updater who
  forgot to delete the old file gets told in-game, not by support.
