# Security findings — the register

**One row per finding. This file is the register of WHAT is broken.** How to fix it lives in the
`PLAN_*` files; where the work stands lives in `EXECUTION.md`. Read `THREAT_MODEL.md` first — it
holds the model these severities are ranked against.

**EVIDENCE:** `[V]` measured personally with a citation · `[A]` reported by a read-only audit agent,
**not yet personally re-verified** · `[?]` unverified.

**STATUS:** **OPEN** (no fix built) · **DESIGN** (fix designed, not built) · **BUILT** (shipped, not
hands-on) · **VERIFIED** (hands-on or matching live log — say which) · **DISMISSED** (with evidence).

> **As of 2026-07-20:** 3 rows **BUILT** (W1, W1b, W2 — commit `6f0c2bf8`, not hostile-drilled), 18
> OPEN. Two audit agents ran (authority/server; wire-parsing) on HEAD `3de0398b`; five rows were
> personally verified on `cc0d8686`, and **W1b was found by a `/qf` round that neither audit caught.**

> **Before fixing any `[A]` row, re-read the cited site yourself** (Rule S5). On 2026-07-20 doing
> this to five rows produced three corrections to the fixes — see `EXECUTION.md` §5.

---

## CRITICAL — remote crash, world destruction, or session takeover

| ID | Finding | Evidence | Attacker → gain | Plan | Status |
|---|---|---|---|---|---|
| **P1** | **Peers are not authenticated.** GNS `IP_AllowWithoutAuth` defaults to `2` ("don't attempt authentication") in the opensource build and we never override it. Encryption is on (AES-256-GCM) so passive sniffing fails, but unauthenticated DH means an active attacker at the rendezvous runs two exchanges and relays | `[V]` `csteamnetworkingsockets.cpp:88-91`; `steamnetworkingtypes.h:1250-1262` | Path attacker → **reads voice + text chat** of a private session | `PLAN_01` | **OPEN** |
| **A1** | **Signaling identity is self-asserted; a duplicate evicts the incumbent.** The only authorization is the shared token every mod user holds; `hostIdentity` is returned by `/v1/join` to anonymous callers | `[A]` `signaling.rs:166-217`, `:214`, `:228-236`; `master.rs:293-301,409,508,518` | Stranger with `nc` → **evicts the real host and receives every joiner** | `PLAN_01` | **OPEN** |
| **A2** | **"Locked" lobbies are not locked.** A tree-wide grep for `join[_]?secret` returns **one hit**, a comment in `session.h:102` describing it as future work. The master's own comment used to assert the challenge exists | `[V]` verified 2026-07-20: `master.rs:499` vs the grep; accept path `session_status.cpp:154-200` | Anyone → **enters any stranger's world**, locked or not | `PLAN_04` §1 | **OPEN** — the **false comment is DELETED** (`6f0c2bf8`, rule S1); the challenge itself is unbuilt and held for the CA spike |
| **A3** | **`PropDestroy` / `PropConvert` / `PropRelease` trust any client's assertion about any entity** — only an eid-range check. The holder table exists and is consulted correctly in `trash_grab_intent.cpp:298-302`, but not here. Aggravating: these kinds are client-relayable, so a forged destroy is **fanned out to other clients before the host validates it** | `[A]` `event_dispatch_entity.cpp:253,316,73`, ranges `:286-292,348-363`; relay `session_lanes.h:181-185`, `session.cpp:454` vs `:463` | Any peer → **irreversibly wipes a stranger's persistent world** | `PLAN_03` §2 | **OPEN** |
| **W1** | **`OnBegin` reserved from an uncapped `uint32_t`.** `totalBytes = 0xFFFFFFFF` → 4 GiB reserve → uncaught `bad_alloc`; no try/catch around the reliable drain, so the process terminated | `[V]` verified 2026-07-20: `save_transfer.cpp:857`, sole guard `:845` | Hostile host → **killed any joining client with one 16-byte packet** | `PLAN_02` §2 | **BUILT** `6f0c2bf8` — the reserve was DELETED, not capped (a census proved it a pure allocation hint). Smoke transferred 20 990 211 B / 367 chunks after removal. Hostile-host drill NOT run |
| **W1b** | **`OnBegin` had no guard against a SECOND Begin** — `g_cliTotal`/`g_cliChunkCount`/`g_cliCrc`/`g_cliSidecarBytes` were reassigned mid-`Receiving` from any later host packet, moving the completion denominator and CRC out from under a stream in flight | `[V]` found 2026-07-20 by a `/qf` round, not by either audit; writer census of `g_cli*`, `save_transfer.cpp:842-859` vs resets at `:821`, `:901` | Hostile host → **corrupts an in-flight transfer's completion logic** | `PLAN_02` §2 | **BUILT** `6f0c2bf8` — fails loudly; a second Begin inside one arm is always a protocol violation |
| **W2** | **`DeserializeSidecar` reserved from an unvalidated entry count** before the (correctly bounds-checked) walk. Runs on the net thread | `[V]` verified 2026-07-20: `save_identity_map.cpp:262` read, `:266` reserve, correct walk `:269+` | Hostile host → **remote OOM kill** | `PLAN_02` §2 | **BUILT** `6f0c2bf8` — ceiling check mirroring the `keyLen` precedent at `:280`. Passed on real data: `(20179-12)/23 = 876 >= 873` |
| **W3** | **Save chunks accepted before `Begin`, with no cap.** In `WaitingBegin` the sink appends but `MaybeFinishLocked_` returns at `:283` on `!g_cliHaveBegin`, so the overflow check at `:285` never runs. Chunks must arrive **in order** (`:367-372`), which bounds nothing. `[V]` the pre-Begin window is LEGITIMATE — not wire reordering, but `OnBegin` running on the game thread while `BulkSink_` runs on the net thread | `[V]` verified 2026-07-20: `save_transfer.cpp:363-379`, `:283-290`; threads `save_transfer.h:151-152` vs `:361` | Hostile host → **remote OOM grind** (not one packet) | `PLAN_02` §2 | **OPEN** — deliberately not fixed with a sized window (a crutch); root fix is a net-thread Begin latch, own commit |

---

## HIGH — resource exhaustion, griefing, cross-peer corruption

| ID | Finding | Evidence | Attacker → gain | Plan | Status |
|---|---|---|---|---|---|
| **W4** | **`blob_chunks::Assembler` — ~246x amplification, no assembly cap.** `blobSeq` is attacker-chosen and default-inserts a map node, then reserves `chunks * 220`. One 228-byte packet costs ~56 KB. Reachable client→host **with no join or role gate** on 8 lanes; `laptop_sync`'s sweep sits behind `EnsureResolved()` so a menu/loading peer never reclaims. `order_sync.cpp:268` caps its table — proving this is an oversight | `[A]` `blob_chunks.cpp:66-87`, `blob_chunks.h:85`; lanes incl. `comp_sync.cpp:251`, `meadow_db_sync.cpp:733,747`, `laptop_buffer_sync.cpp:532` | Any peer → **memory exhaustion on host and clients** | `PLAN_02` §3 | **OPEN** |
| **W5** | **`owner_entity_sync::OnSpawnMsg` — 65 536 real actor spawns per peer.** The `kMaxOwned=8` backstop is **send-side only**; the receive path spawns for every unseen `(slot, seq)`. On the relay whitelist, so it hits every client too | `[A]` `owner_entity_sync.cpp:350`; `session_lanes.h:210` | Any peer → **freezes host and all clients** | `PLAN_02` §3 | **OPEN** |
| **W6** | **`TrashCarryPose` has no role gate and no finite check** — its five siblings have both. NaN flows into `SetActorLocation`. The comment at `:61-62` was a **fused claim, half false**: the ctx-freshness gate is real, the per-entry float validation does not exist | `[V]` verified 2026-07-20: `session_trashcarry.cpp:58-70` (zero hits for `IsHost`/`isfinite`/`ValidatePose`); apply `trash_clump_pose_stream.cpp:49,60`; siblings `session_streams.cpp:198,222,260,297,324` | Any client → **drives the host's props to NaN/arbitrary transforms** | `PLAN_02` §3 | **OPEN** — the **comment is corrected** (`6f0c2bf8`, rule S1); the role gate + finite check are unbuilt |
| **A4** | **Whole symmetric families accept any peer's writes.** Doors, lights, containers, keypads, power, ATV, trash piles, sleep, inventory blob, email delete, and the entire desk/laptop/drive/rack/meadow chain. The `device_occupancy` claim table is **advisory** — read by senders deciding whether to stream, never by a host receive path | `[A]` `event_dispatch_state.cpp:40-74,75,97,118,143,226,259,288,361,406,424`; `event_dispatch_signal.cpp:40-520`, `:390-391` | Any peer → **drives another player's desk mid-session, deletes their email, wipes DB rows, unlocks doors** | `PLAN_03` §3 | **OPEN** |
| **A5** | **`BalanceDelta` is an unbounded client-authored economy write.** Length is validated; the value is not | `[A]` `event_feed.cpp:294-306` → `balance_sync.cpp:96-103` | Any peer → **sets the shared balance to ±2³¹** | `PLAN_04` §3 | **OPEN** |
| **A6** | **Unlimited TURN credential minting.** `/v1/heartbeat` re-mints on every call at 240/min, and heartbeat only needs a token from your own lobby | `[A]` `master.rs:414-435,482-519`, limits `:56-57` | Anyone → **our VPS bandwidth**, coturn saturation | `PLAN_04` §4 | **OPEN** |

---

## MEDIUM

| ID | Finding | Evidence | Attacker → gain | Plan | Status |
|---|---|---|---|---|---|
| **A8** | **Future website: stored XSS.** `clamp_str` strips control/bidi/zero-width but deliberately not `< > & " '` — correct for a game label, fatal if the site uses `innerHTML`. Planted by one anonymous `POST /v1/host` | `[A]` `master.rs:356-364` → `common.rs:78-111` | Anyone → **XSS on every site visitor** | `PLAN_05` | **OPEN** |
| **A9** | **`/v1/join` discloses a direct host's raw IP to anonymous callers** before any admission decision | `[V]` verified 2026-07-20: `master.rs:500-506` returns `addr` on the `conn=="direct"` branch | Scraper → **harvests home IPs of all direct hosts** | `PLAN_04` §2 | **OPEN** |
| **W7** | **FName pool poisoning.** Wire strings validated by length only reach `StringToFName`; UE never frees FName entries | `[A]` `signal_wire.cpp:106,116-117` → `signal_dynamic.cpp:31-38`; ungated `meadow_db_sync.cpp:733` | Any peer → **permanent process-lifetime memory growth** | `PLAN_02` §4 | **OPEN** |
| **W8** | **Inbound append lanes ignore their own caps.** The floppybox 15-entry cap is a `UE_LOGW` that does not reject; laptop buffer `appendTail` has no cap | `[A]` `floppybox_sync.cpp:398-403`; `laptop_buffer_sync.cpp:313-316` | Any peer → unbounded engine-array growth | `PLAN_02` §4 | **OPEN** |
| **W9** | **Unbounded wire-keyed side maps.** `g_lidPending[p.eid]` inserts exactly when the eid does NOT resolve (the garbage case); `g_tombs` grows per unmatched delete | `[A]` `laptop_sync.cpp:508`; `meadow_db_sync.cpp:744` | Any peer → nuisance growth (TTL-bounded) | `PLAN_02` §4 | **OPEN** |
| **W10** | **Reliable inbox cap is global, not per-peer** (`kReliableInboxCap = 8192`) | `[A]` `session.cpp:433` | One flooding peer → **starves every other peer's events** | `PLAN_02` §4 | **OPEN** |
| **A7** | **Lobby-list flooding evicts real lobbies.** At the 1000 global cap, `evict_if_full` drops the stalest *real* lobby. 8 per /64 x 125 /64s — trivial with a routed /48 | `[A]` `master.rs:45-46,308-324,350-352` | Anyone → **pushes real lobbies out of the list** | `PLAN_04` §5 | **OPEN** |

---

## ARCHITECTURAL / needs a live measurement

| ID | Finding | Evidence | Note | Plan | Status |
|---|---|---|---|---|---|
| **S1** | **The save blob is a fully trusted parse surface.** After a CRC (integrity, not authenticity) a ~17 MB attacker-controlled blob is handed to the game's **native GVAS deserializer**, which then spawns entities. Nothing validates structure between wire and engine | `[A]` `save_transfer.cpp:291` | Inherent to the current design and the **largest remaining trust surface**; any UE-side deserializer bug is remotely reachable by joining a hostile lobby. **No cheap fix** — recorded and weighed, not scheduled | — | **OPEN** |
| **S2** | **IP bans may be useless — or catastrophic — on TURN-relayed connections.** The ban reads `m_addrRemote`, which for a relayed connection is plausibly coturn's address. Either the ban is a no-op, or it writes **our own TURN IP** into the banlist, after which the fail-closed filter rejects every future relayed joiner | `[A]` `session_status.cpp:69-82`, `:76-80`; relay known elsewhere `:416,:439` | **Measurement task, not a fix task** | `PLAN_04` §6 | **OPEN** |

---

## Checked and clean — do not re-audit

Recorded so a future pass does not spend effort here. All `[A]`.

- **Header + reliable framing.** `ParseHeader`/`PeekProtocolVersion` (`protocol.h:4877,4895`) check
  length before memcpy; `session.cpp:404-424` validates `payloadLen` against `kMaxReliablePayload`.
- **Per-kind length validation.** A repo-wide scan for a struct reinterpret without a nearby
  `payloadLen` check found exactly one hit (`order_sync.cpp:231`), guarded at `:224`.
- **Batch parsers.** npc / worldactor / trashcarry cap `bh.count` then check
  `len >= hdr + n*stride`. The 9 scalar stream channels validate size + `ValidatePose`/`isfinite`.
  (The trashcarry *role gate* is still missing — that is **W6**; its length handling is fine.)
- **Voice.** Min-len, `routeSlot` range, `copyLen` clamp, `opusLen` vs cap and body, ring index modulo.
- **Relay.** Rejects oversize before the fixed-buffer memcpy; only whitelisted kinds forward, verbatim.
- **All wire strings.** Every one clamps length; NUL-less fields copy into an oversized zeroed local.
  No `strlen` on a wire array anywhere.
- **Filesystem paths.** GUID validated to 32 hex chars twice; skin names allow-listed to
  `[0-9a-zA-Z_-]`; save slot filename is PID-derived. **No peer string reaches a path.**
- **Format strings.** Zero non-literal `UE_LOG*`/printf format arguments repo-wide.
- **Peer-slot impersonation.** `VerifySenderEidRange` partitions host/client eid ranges;
  `HandleAssignPeerSlot` gates on role, sender slot 0, and slot range. The slot is transport-bound
  and every payload-carried slot is cross-checked.
- **Master token handling.** `/v1/visibility` and `/v1/leave` verify the bearer with a constant-time
  compare against the specific lobby; tokens are 192-bit CSPRNG. `resolve_client_ip` trusts XFF only
  from loopback and takes the rightmost element. **This part is well built.**
- **Moderation file format.** `CleanField` strips `|`/CR/LF; nicks are alnum+space via
  `SanitizeNickname`.

### Dismissed with evidence

| Item | Why dismissed |
|---|---|
| **`ct_eq` length leak** (`common.rs:116-125`) | Tokens are fixed-length CSPRNG; length reveals nothing |
| **Ban evasion by new IP/identity** | Inherent without accounts; the GUID is client-supplied and equally forgeable. Social problem at a 3-peer cap. **Not** the same as **S2** |

---

Fix order and current progress: **`EXECUTION.md`**.
