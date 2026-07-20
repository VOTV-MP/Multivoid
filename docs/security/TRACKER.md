# Security findings — living tracker

**Update every session.** One row per finding. Read `README.md` first — it holds the threat model
these severities are ranked against.

**EVIDENCE legend:** `[V]` measured personally with a citation · `[A]` reported by a read-only audit
agent, **not yet personally re-verified** · `[?]` unverified.

**STATUS legend:** **OPEN** (no fix built) · **DESIGN** (fix designed, not built) · **BUILT** (shipped,
not hands-on) · **VERIFIED** (hands-on or matching live log — say which) · **DISMISSED** (with the
evidence for dismissal).

**As of 2026-07-20 every row is OPEN.** Nothing in this tracker has been fixed. Two audit agents ran
(authority/server; wire-parsing) on HEAD `3de0398b`.

> **Before fixing any `[A]` row, re-read the cited site yourself.** These came from agents; the
> project rule is to verify a handed-down measurement before building on it.

---

## CRITICAL — remote crash, world destruction, or session takeover

| ID | Finding | Evidence | Attacker → gain | Fix layer | Status |
|---|---|---|---|---|---|
| **P1** | **Peers are not authenticated.** GNS `IP_AllowWithoutAuth` defaults to `2` ("don't attempt authentication") in the opensource build and we never override it. Encryption is on (AES-256-GCM) so passive sniffing fails, but unauthenticated DH means an active attacker at the rendezvous runs two exchanges and relays | `[V]` `csteamnetworkingsockets.cpp:88-91`; `steamnetworkingtypes.h:1250-1262` | Path attacker → **reads voice + text chat** of a private session | Peer certs from our own CA + `AllowWithoutAuth=0` (README §4) | **OPEN** |
| **A1** | **Signaling identity is self-asserted; a duplicate evicts the incumbent.** The only authorization is the shared token, which every mod user has; `hostIdentity` is returned by `/v1/join` to anonymous callers | `[A]` `signaling.rs:166-217`, `:214`, `:228-236`; `master.rs:293-301,409,508,518` | Stranger with `nc` → **evicts the real host and receives every joiner** | Server-issued signaling ticket (HMAC over identity, short TTL) verified before registration | **OPEN** |
| **A2** | **"Locked" lobbies are not locked — and a false comment says otherwise.** `master.rs:498-499` asserts a "game-layer post-Connected join-secret challenge"; `joinSecret` appears **nowhere else in the tree**. `session.h:102-104` correctly says it is not built | `[A]` `master.rs:498-499` vs `session.h:102-104`; accept path `session_status.cpp:154-200` | Anyone → **enters any stranger's world**, locked or not | Implement the challenge; **delete the false comment today regardless** | **OPEN** |
| **A3** | **`PropDestroy` / `PropConvert` / `PropRelease` trust any client's assertion about any entity** — only an eid-range check. The holder table exists and is consulted correctly in `trash_grab_intent.cpp:298-302`, but not here. Aggravating: these kinds are client-relayable, so a forged destroy is **fanned out to other clients before the host validates it** | `[A]` `event_dispatch_entity.cpp:253,316,73`, ranges `:286-292,348-363`; relay `session_lanes.h:181-185`, `session.cpp:454` vs `:463` | Any peer → **irreversibly wipes a stranger's persistent world** | Route through the holder predicate; convert commands → host-validated intents; **validate before relay** | **OPEN** |
| **W1** | **`save_transfer::OnBegin` reserves from an uncapped `uint32_t`.** `totalBytes = 0xFFFFFFFF` → 4 GiB reserve → uncaught `bad_alloc`. There is no try/catch around the reliable drain, so this terminates the process | `[A]` `save_transfer.cpp:857`, `:845`; no sender-slot gate at `event_feed.cpp:164-171` | Hostile host → **kills any joining client with one 16-byte packet** | Cap against `kMaxSaveBlobBytes` at apply | **OPEN** |
| **W2** | **`DeserializeSidecar` reserves from an unvalidated entry count** before the (correctly bounds-checked) walk. Runs on the net thread | `[A]` `save_identity_map.cpp:262,266`; via `save_transfer.cpp:313` | Hostile host → **remote OOM kill** | Validate `count` against remaining bytes before reserve | **OPEN** |
| **W3** | **Save chunks accepted before `Begin`, with no cap.** In `WaitingBegin` the overflow check never runs; a host that streams chunks and never sends `Begin` grows the buffer without limit | `[A]` `save_transfer.cpp:355-371`, `:285`; sink `session.cpp:414-422` | Hostile host → **remote OOM kill** | Cap `g_cliBuf` in the sink itself | **OPEN** |

---

## HIGH — resource exhaustion, griefing, cross-peer corruption

| ID | Finding | Evidence | Attacker → gain | Fix layer | Status |
|---|---|---|---|---|---|
| **W4** | **`blob_chunks::Assembler` — ~246× amplification, no assembly cap.** `blobSeq` is attacker-chosen and default-inserts a map node, then reserves `chunks * 220`. One 228-byte packet costs ~56 KB. Reachable client→host **with no join or role gate** on 8 separate lanes; `laptop_sync`'s sweep sits behind `EnsureResolved()` so a menu/loading peer never reclaims. `order_sync.cpp:268` caps its table — proving this is an oversight | `[A]` `blob_chunks.cpp:66-87`, `blob_chunks.h:85`; lanes incl. `comp_sync.cpp:251`, `meadow_db_sync.cpp:733,747`, `laptop_buffer_sync.cpp:532` | Any peer → **memory exhaustion on host and clients** | Per-sender assembly cap, mirroring `order_sync`'s `kMaxAssembly` | **OPEN** |
| **W5** | **`owner_entity_sync::OnSpawnMsg` — 65 536 real actor spawns per peer.** The `kMaxOwned=8` backstop is send-side only; the receive path spawns for every unseen `(slot, seq)`. On the relay whitelist, so it hits every client too | `[A]` `owner_entity_sync.cpp:350`; `session_lanes.h:210` | Any peer → **freezes host and all clients** | Receive-side `g_mirrors` cap | **OPEN** |
| **W6** | **`TrashCarryPose` has no role gate and no finite check** — its four siblings all have both. NaN flows into `SetActorLocation`. **The comment claiming per-entry float validation happens at apply is false** | `[A]` `session_trashcarry.cpp:58`, false comment `:61-62`; apply `trash_clump_pose_stream.cpp:60`; correct siblings `session_streams.cpp:346,365,383,402` | Any client → **drives the host's props to NaN/arbitrary transforms** | Role gate + `FiniteVec`, matching `npc_mirror.cpp:638` | **OPEN** |
| **A4** | **Whole symmetric families accept any peer's writes.** Doors, lights, containers, keypads, power, ATV, trash piles, sleep, inventory blob, email delete, and the entire desk/laptop/drive/rack/meadow chain. The `device_occupancy` claim table is **advisory** — read by senders deciding whether to stream, never by a host receive path | `[A]` `event_dispatch_state.cpp:40-74,75,97,118,143,226,259,288,361,406,424`; `event_dispatch_signal.cpp:40-520`, `:390-391` | Any peer → **drives another player's desk mid-session, deletes their email, wipes DB rows, unlocks doors** | Make the claim table **enforcing on receive**, one predicate at the family seam. Needs a Principle-8 late-join answer for a departed holder's claim | **OPEN** |
| **A5** | **`BalanceDelta` is an unbounded client-authored economy write.** Length is validated; the value is not | `[A]` `event_feed.cpp:294-306` → `balance_sync.cpp:96-103` | Any peer → **sets the shared balance to ±2³¹** | RULE 2: retire the client→host delta (its only user is the dev `+1000` button, `add_points.cpp:12-22`); balance is already host-authored | **OPEN** |
| **A6** | **Unlimited TURN credential minting.** `/v1/heartbeat` re-mints on every call at 240/min, and heartbeat only needs a token from your own lobby | `[A]` `master.rs:414-435,482-519`, limits `:56-57` | Anyone → **our VPS bandwidth**, coturn saturation | Drop minting from heartbeat entirely; set coturn `user-quota`/`total-quota` | **OPEN** |

---

## MEDIUM

| ID | Finding | Evidence | Attacker → gain | Fix layer | Status |
|---|---|---|---|---|---|
| **A8** | **Future website: stored XSS.** `clamp_str` strips control/bidi/zero-width but deliberately not `< > & " '` — correct for a game label, fatal if the site uses `innerHTML`. Planted by one anonymous `POST /v1/host` | `[A]` `master.rs:356-364` → `common.rs:78-111` | Anyone → **XSS on every site visitor** | **Render layer only** (see below). Do NOT strip in `clamp_str` — wrong layer, breaks legitimate names | **OPEN** |
| **W7** | **FName pool poisoning.** Wire strings validated by length only reach `StringToFName`; UE never frees FName entries | `[A]` `signal_wire.cpp:106,116-117` → `signal_dynamic.cpp:31-38`; ungated `meadow_db_sync.cpp:733` | Any peer → **permanent process-lifetime memory growth** | Allow-list/charset the names, or intern against a bounded set | **OPEN** |
| **W8** | **Inbound append lanes ignore their own caps.** The floppybox 15-entry cap is a `UE_LOGW` that does not reject; laptop buffer `appendTail` has no cap | `[A]` `floppybox_sync.cpp:398-403`; `laptop_buffer_sync.cpp:313-316` | Any peer → unbounded engine-array growth | Enforce the cap at apply | **OPEN** |
| **W9** | **Unbounded wire-keyed side maps.** `g_lidPending[p.eid]` inserts exactly when the eid does NOT resolve (the garbage case); `g_tombs` grows per unmatched delete | `[A]` `laptop_sync.cpp:508`; `meadow_db_sync.cpp:744` | Any peer → nuisance growth (TTL-bounded) | Bound the maps | **OPEN** |
| **W10** | **Reliable inbox cap is global, not per-peer** (`kReliableInboxCap = 8192`) | `[A]` `session.cpp:433` | One flooding peer → **starves every other peer's events** | Per-peer accounting | **OPEN** |
| **A7** | **Lobby-list flooding evicts real lobbies.** At the 1000 global cap, `evict_if_full` drops the stalest *real* lobby. 8 per /64 × 125 /64s — trivial with a routed /48 | `[A]` `master.rs:45-46,308-324,350-352` | Anyone → **pushes real lobbies out of the list** | Refuse new `/v1/host` at the cap (429); reserve eviction for TTL-stale rows, which `sweeper()` already handles | **OPEN** |
| **A9** | **`/v1/join` discloses a direct host's raw IP to anonymous callers** before any admission decision | `[A]` `master.rs:500-506` | Scraper → **harvests home IPs of all direct hosts** | Disappears once A2's join secret exists; gate `addr` behind it | **OPEN** |

### A8 — exactly what the website must do

Recorded here because it must be true *before the site is written*:

1. **Never `innerHTML` a lobby field.** Use `textContent` / `createTextNode` or a default-escaping
   template (React `{}`, Vue `{{ }}`, Jinja/Handlebars autoescape). Forbid `dangerouslySetInnerHTML`,
   `v-html`, `{{{ }}}` on these fields.
2. Never interpolate into an **attribute** without attribute-escaping; never into
   `href`/`src`/`style`/inline `on*` at all.
3. Never build a JSON island by concatenating them into a `<script>` block — `</script>` inside a
   name breaks out even with HTML escaping.
4. Ship a CSP: `default-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'`.
5. The site consumes **`/v1/lobbies` only** — never proxy `/v1/host`, `/v1/join` or `/healthz` to a
   browser.
6. **If a reverse proxy is ever put in front of the master**, it must *overwrite* (not append)
   `X-Real-IP`, or every per-IP rate limit and cap (A6, A7) collapses into one bucket.

Affected fields: `name` (63), `world` (39), `version`/`game` (23). `lobbyId`/`proto`/`players_*`/`age`
are server-minted or integers and are safe.

---

## ARCHITECTURAL / needs a live measurement

| ID | Finding | Evidence | Note | Status |
|---|---|---|---|---|
| **S1** | **The save blob is a fully trusted parse surface.** After a CRC (integrity, not authenticity) a ~17 MB attacker-controlled blob is handed to the game's **native GVAS deserializer**, which then spawns entities. Nothing validates structure between wire and engine | `[A]` `save_transfer.cpp:291` | Inherent to the current design and the **largest remaining trust surface**; any UE-side deserializer bug is remotely reachable by joining a hostile lobby. No cheap fix — record it, weigh it | **OPEN** |
| **S2** | **IP bans may be useless — or catastrophic — on TURN-relayed connections.** The ban reads `m_addrRemote`, which for a relayed connection is plausibly coturn's address. Either the ban is a no-op, or it writes **our own TURN IP** into the banlist, after which the fail-closed filter rejects every future relayed joiner | `[A]` `session_status.cpp:69-82`, `:76-80`; relay is known elsewhere `:416,:439` | **Measure:** force two peers onto relay (`iceMode="relay"`, `session.h:97-100`) and log `m_addrRemote` at the host accept edge. If confirmed: key bans on the GUID (`moderation.cpp:104-118` already does for offline bans) and refuse to store a ban equal to the TURN host | **OPEN** |

---

## Checked and clean — do not re-audit these

Recorded so a future pass does not spend effort here. All `[A]`.

- **Header + reliable framing.** `ParseHeader`/`PeekProtocolVersion` (`protocol.h:4877,4895`) check
  length before memcpy; `session.cpp:404-424` validates `payloadLen` against `kMaxReliablePayload`.
- **Per-kind length validation.** A repo-wide scan for a struct reinterpret without a nearby
  `payloadLen` check found exactly one hit (`order_sync.cpp:231`), and it is guarded at `:224`.
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
- **Master token handling.** `/v1/visibility` and `/v1/leave` verify the bearer token with a
  constant-time compare against the specific lobby; tokens are 192-bit CSPRNG. `resolve_client_ip`
  trusts XFF only from loopback and takes the rightmost element. This part is well built.
- **Moderation file format.** `CleanField` strips `|`/CR/LF; nicks are alnum+space via
  `SanitizeNickname`.

### Dismissed with evidence

- **`ct_eq` length leak** (`common.rs:116-125`) — tokens are fixed-length CSPRNG; length reveals
  nothing.
- **Ban evasion by new IP/identity** — inherent without accounts; a GUID is client-supplied and
  equally forgeable. Social problem at a 3-peer cap. Not the same as **S2**.

---

## Suggested order

Cheapest-first within severity, and front-loading what gets more expensive later.

1. **A8** — costs nothing today, expensive after the site ships. Write the rules into the site's spec now.
2. **A2's false comment** — delete it today, independent of building the challenge.
3. **W1, W2, W3** — one-line apply-side caps; each is a remote process kill.
4. **W4**, then **W6** — a per-sender assembly cap; a role gate + finite check.
5. **A5**, **A6** — small deletions (retire the delta lane; stop minting on heartbeat).
6. **P1** — the CA spike first (README §4), because it also dissolves **A1** and Tier C.
7. **A1 + A2** — server-issued signaling tickets and the join secret, as one arc.
8. **A3 + A4** — the real work: make the existing holder/claim tables enforcing at the receive seam.
9. **S2** measurement, then **A7**, **A9**, **W5**, **W7-W10**.
