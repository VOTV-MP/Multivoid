# Tier B (TLS on the control plane) + Tier C (per-session signaling tokens) — DESIGN

> **SUPERSEDED AS A PLAN, 2026-07-20 (same day).** Read **`docs/security/README.md` FIRST** — it holds
> the threat model this document never had, and the measured facts that reordered the work. In short:
> `IP_AllowWithoutAuth = 2` means **GNS does not authenticate peers**, so the real gap is peer
> identity, not transport encryption; and `signalingToken` is a shared secret every mod user already
> holds, so **Tier C dissolves into peer certificates** rather than being built as designed.
> **Arcs 1-2 remain accurate as an AS-BUILT record** (§5b) and stay shipped. **Arcs 3 / 3b / 4 / 5 are
> ON HOLD** pending the CA spike in `docs/security/README.md` §4. The `net.master.insecure` flag
> discussed in later sessions was **never built and should not be** — see the retraction table there.

**Type:** DESIGN for the whole plan (converged: 20-round `/qf`, critic verdict "that holds" at R19,
R20 aimed at auto-renewal) — **now MIXED status**: **arcs 1-2 are AS-BUILT + LIVE** (see §5b for the
as-built record and its drills), arcs 3 / 3b / 4 / 5 remain DESIGN. Nothing here is VERIFIED in the
hands-on sense: the evidence so far is drills + a LAN smoke + real server-side logs, not a play session.
Supersedes the "NEXT: Tier B TLS" line in `votv-master-server-RE-and-rust-port-scope-2026-07-16.md`
(which now points here for status).

**Root being fixed** (security audit 2026-07-16, the one UNFIXED item) — stated as it was BEFORE
this work; as of 2026-07-20 the master half is TLS and the signaling half is not yet: the coop control
plane was **cleartext** — master HTTP :10001 + signaling TCP :10000. The signaling bearer
token and the TURN credentials are sniffable on the wire, which buys an on-path attacker
signaling MITM, relay theft, and — because the signaling server binds no identity to the
token — **identity hijack** (any token holder may register ANY identity and evict the
incumbent).

Population context: **zero public releases exist**; the only peers are our 4 dev installs,
and `deploy-all` writes all four in one run. So a hard cutover is available and no
migration shims are owed (RULE 2).

---

## 1. Fact base (measured; each was checked against the code or the live box this pass)

| # | Fact | Where measured |
|---|---|---|
| F1 | Master = hand-rolled async HTTP directly on `tokio::net::TcpStream` (no axum/hyper); signaling = line relay, one task owning both split halves via `select!` | `tools/coop-server-rs/src/bin/{master,signaling}.rs` |
| F2 | The signaling token is verified **only at the greeting**, statelessly; the relay loop never re-checks. A bad greeting is **silently dropped** today | `signaling.rs:163-169` |
| F3 | Client master HTTP = WinHTTP, one shared `http::Post/Get` path, `flags=0` ("plain HTTP") | `coop/net/http_client.cpp:71-88` |
| F4 | Signaling client = raw nonblocking Winsock polled at ~200 Hz; `sendQueue_` holds plaintext lines, **survives reconnects**, and the greeting is re-fronted on every `ConnectLocked` | `coop/net/signaling_client.cpp:281,288-301,318-` |
| F5 | Live box: **:80 free**, certbot **absent**, :443/:8443 = a foreign tenant's xray (no reverse proxy possible). Units are `DynamicUser=yes` + `ProtectSystem=strict`; secrets already ride `EnvironmentFile` (root-read, then privileges drop) | `ss -tlnp` on <coop-vps>; `vps_provision.sh:94-120,175` |
| F6 | GNS has ONE `SendSignal` site; post-connect signals are only reliable-msg acks / ConnectionClosed / NoConnection — the data path is UDP/ICE and signaling-independent. `TimeoutInitial` default = 10000 ms, not overridden by us | `steamnetworkingsockets_p2p.cpp:1801`, `csteamnetworkingsockets.cpp:76` |
| F7 | A heartbeat against an empty registry returns **403** — so a master **restart** and a TTL **reap** produce the same observable | `master.rs:418-421` (code-measured; arc 1 observes it live) |
| F8 | `h_host` has **no session precondition** (rate limits only) → the resume re-announce path is reachable while heartbeats 403 | `master.rs:345-352` |
| F9 | Heartbeat cadence 30 s; `LOBBY_TTL` 90 s; sweeper 30 s → real row lifetime 90..120 s (constant-derived; arc 1 observes the real reap) | `lobby_announcer.cpp:94-99`, `master.rs:41,798` |
| F10 | The heartbeat **response body is ignored** today (status only) → a token push needs new plumbing | `lobby_announcer.cpp:116-119` |
| F11 | `session_start` hard-fails P2P on an empty `signalingToken` | `coop/net/session_start.cpp:247-252` |
| F12 | The master env carries `COOP_SIGNALING_URL` as a **bare IP**, and the master's response **overrides** the client's configured signaling URL → clients dial signaling by IP today; the compiled hostname constant only serves the master-down path | live `/etc/coop-master.env`; `session_manager.cpp:276,372,537` |
| F13 | `CloseSocketLocked()` **clears `inBuf_`** from inside the locked recv loop while line dispatch runs after that block → a server line delivered together with FIN is **erased before it is ever parsed**; and the parser's hex path would reject a non-hex line anyway | `signaling_client.cpp:213-219,335-338,384-` |
| F14 | A short send (`0 < r < l`) hard-closes the connection today | `signaling_client.cpp:364-373` |
| F15 | `Enqueue` caps the queue at 32 lines, dropping the **oldest** — deliberate (GNS re-sends rendezvous: `p2p.cpp:2097-2110`) | `signaling_client.cpp:288-301` |
| F16 | Direct lobbies receive **no** ICE block; LAN topology never enters the P2P branch; the env-host announce is best-effort → LAN/direct play survives a master outage | `master.rs:405-409`, `session_start.cpp:247`, `session_manager.cpp:342` |
| F17 | rustls does **not** support renegotiation; schannel on Win10 19045 tops out at **TLS 1.2** | rustls docs + platform |
| F18 | The `COOP_MASTER_SIGNING_SECRET` named in the RE doc exists **only** as a reserved variable in the retired Python master — the Rust deployment neither reads nor writes it | `coop_master_server.py:58,84`; `vps_provision.sh` env blocks |
| F19 | Token charsets are safe for `:`-joining: identity = `h`/`c` + lowercase hex, sessionId = hex, exp = decimal, MAC = base64 | `common.rs:50-54` |
| F20 | 11 tools speak to these endpoints. `master_smoke.py` is the Python **master's** last consumer; `p2p_smoke.py` is the Python **signaling** server's; `tier2_host_button_vps_probe.py` targets the VPS by **raw IP** | systematic sweep of `tools/*.{py,ps1,sh,bat}` |

---

## 2. Tier B — TLS

### 2.1 Server

- **Certificate:** `certbot certonly --standalone -d master.multivoid.dev` over HTTP-01 on
  :80 (measured free; ufw already allows it with the comment `coop letsencrypt http-01`).
  A reverse proxy is impossible — :443 belongs to another tenant on that box.
- **Termination in our own binaries:** `tokio-rustls` in both bins, wrapping the accepted
  stream before the existing handler. `send_tls13_tickets(0)`.
- **Credentials:** cert + key reach the `DynamicUser` services through systemd
  `LoadCredential=`; the HMAC signing secret rides the existing `EnvironmentFile` pattern
  with **generate-if-absent** semantics (as `SIG_TOKEN`/`TURN_SECRET` already do,
  `vps_provision.sh:67-77`) — a re-provision must never silently invalidate live tokens.
- **Renewal:** a certbot deploy hook restarts both services. Tolerable: the outage is
  ≤6 s (sub-second restart + the client's 5 s backoff) against GNS's measured 10 s
  `TimeoutInitial`, and established P2P data does not traverse signaling (F6).
- **Observability:** both bins log the signing secret's **fingerprint** at startup (drift
  detection) and log **at accept time** — `[listener=plain|tls] [ip]` — because the
  master's 400 path is silent today and arc 5's gate must be a measurement, not a hope.

### 2.2 Client

- **Master:** add `WINHTTP_FLAG_SECURE`; the port is unchanged in kind (WinHTTP is
  port-agnostic for TLS). Let's Encrypt is trusted by the Windows root store.
- **Signaling:** a schannel layer between the socket and the line protocol, driven from
  the existing nonblocking `Poll()`:
  - states `Handshaking → Established → Failed`; **handshake deadline 10 s** → close →
    the existing 5 s backoff;
  - the plaintext queue is **untouched until Established** (the greeting is sent first by
    construction — see §2.3);
  - **pop-after-full-record-flush**: the front line stays queued while its ciphertext is
    written; a mid-record disconnect discards the partial ciphertext and the line is
    re-sent whole on the next channel, preserving F4's loss-prevention;
  - the ciphertext out-buffer is **bounded by construction**: schannel `StreamSizes`
    gives one maximum record (~16–17 KB) and the next line is encrypted only when that
    buffer is empty, so the plaintext 32-line cap (F15) remains the only queue bound;
  - **short-send semantics change** (this also fixes a pre-existing needless drop, F14):
    `0 < r < l` advances a write offset and retries next Poll instead of closing;
  - `SECBUFFER_EXTRA` leftovers are carried in **both** directions through one shared
    inbound ciphertext buffer;
  - `SEC_I_RENEGOTIATE` = a distinct log line + drop + reconnect. **No speculative
    re-entry machine** — it is dead against rustls + `tickets(0)`, has no nameable case in
    our population, and OPUS §11 forbids building for hypotheticals.
- **Validation — there is NO certificate pinning anywhere.** The chain is validated to the
  system root store plus hostname; the SSPI flags are merely set *explicitly*: no
  `SCH_CRED_MANUAL_CRED_VALIDATION`, no `SCH_CRED_NO_SERVERNAME_CHECK`,
  `pszTargetName` = the URL hostname, revocation checked with ignore-offline /
  ignore-no-revocation. A 60-day LE rotation is therefore a non-event.
- **SSPI lifecycle is explicit and measured** (reconnects get much more frequent under
  C2/C3): `CloseSocketLocked` also calls `DeleteSecurityContext` and resets the ciphertext
  buffer + handshake state; the credentials handle is **per-client** (acquired once,
  reused across reconnects, freed in the destructor). A 20-cycle reconnect drill watches
  process RSS and handle count so a context leak shows up linearly.

### 2.3 Two structural repairs the TLS work depends on

- **The greeting leaves `sendQueue_`** and becomes its own field that the flush path always
  sends first on a fresh socket. This kills a whole class: a stale-token greeting could
  otherwise survive in the preserved queue, and the 32-cap's front-drop (F15) could even
  evict the greeting itself. The fragile `front()`-compare at `:281` goes with it (RULE 2).
- **Lines are drained before `inBuf_` is cleared** on close, and `ERR <code>` is parsed
  ahead of the hex path — otherwise the Tier-C trigger below could never fire (F13).

### 2.4 Scheme grammar

**SCHEMELESS = SECURE.** The compiled constants stay bare (`master.multivoid.dev:<port>`),
so the `DisplayMaster` "DEFAULT" mask, which compares by exact equality
(`session_manager.cpp:108`), is untouched; only the port number changes, once. An explicit
`http://` / `tcp://` is a deliberate **per-URL operator downgrade** for self-hosting
without a domain, and it prints verbatim (never masked). Master and signaling URLs are
independent strings, so mixed combinations are naturally representable.

**Config layers that can supply an endpoint** (the enumerate-every-layer rule): compiled
constants · ini `net.master`/`net.signaling` under the `net.master.custom=1` gate · env
`VOTVCOOP_MASTER_URL`/`_NET_SIGNALING` · **the master's response, i.e. the server's own
`COOP_SIGNALING_URL`** · `g_fallbackHostCfg` (dies with D1) · the Rust differential
harness. The boot/version popup and `ui/server_browser` have no endpoint source of their
own — both route through `session_manager::MasterUrl()` ← `cfg::ReadMasterUrl()`.

---

## 3. Tier C — per-session, identity-bound signaling tokens

**Token:** `v1:<exp>:<identity>:<sessionId>:<base64 HMAC-SHA256(secret, "v1:exp:identity:sessionId")>`
— the version is *inside* the MAC (no prefix-swap downgrade), the charsets make `:`-joining
unambiguous (F19), the parse is `splitn(5)` with the MAC last. TTL 12 h, a **server-side**
env knob only (no client knob — the ini-`[dev]`-not-env rule is not even engaged). The
client treats the token as **opaque**: it never parses it, never persists it to the ini,
and it is never logged.

**Minting:** in the `ice_block` of the `/v1/host` and `/v1/join` responses, bound to the
identity and sessionId minted in that same response. This is what kills the hijack door:
the HMAC itself proves "the master minted this identity for this lobby", so no membership
table is needed on the signaling side, which stays fully stateless.

**Verification (signaling, greeting only):** HMAC + exp + greeting-identity == token
identity.

**Refresh — gated on lobby liveness, not on a grace horizon:**
`POST /v1/signaling_refresh {token}` → HMAC valid (exp ignored) **+ the lobby is alive**
**+ identity charset `'c'` only** → re-mint the same identity + sessionId with a fresh exp.
Hosts are refused here on purpose: they already have a refresh lane (below), and allowing
them would let a leaked host token be laundered into a resume-valid one.
A leaked client token's exposure is therefore bounded by its lobby's lifetime, and lobby
death (leave / TTL reap) revokes the whole chain.

**Host lane:** every heartbeat response carries a fresh token (new plumbing — F10 shows the
body is ignored today), and an `ERR badtoken` triggers an **out-of-cycle heartbeat pull**.

**One write-through owner:** all three fresh-token sources (join response, heartbeat push,
resume response) route through a single `Session::AdoptSignalingToken()` that updates
`Session::cfg_` **and** the live `SignalingClient` atomically — so no later path (re-dial,
re-create from cfg, resume) can resurrect a stale copy. The full copy map is: transient
`HostInfo`/`JoinInfo` locals · `Session::cfg_` · `SignalingClient::token_`+`greeting_` ·
`g_fallbackHostCfg` (deleted by D1).

**Trigger, delivery-independent.** `ERR badtoken` is the *fast* path; the **guarantee** is
the invariant *three consecutive cycles of "Established → greeting sent → closed" = auth
failure → refresh*. A cert problem can never drive that loop: it fails **before**
Established with a specific `SEC_E_` code and takes its own branch (log + backoff, no
refresh). The counter resets on the same thing it counts — a connection that reached
Established, greeted, and **survived ≥30 s** — never on a bare successful connect.
A server restart consequently cannot mint a refresh storm: the token is stateless-verified
so the reconnect succeeds, and a long-lived connection had already reset the counter.

**Refusal → bounded terminal teardown:** the signaling client goes `Stopped` (no reconnect
loop), the queue is dropped with a count log, and the session layer emits a user-visible
feed line. GNS liveness owns the session's fate independently. Hosts never reach this
state (they own the heartbeat lane).

### 3.1 C2 — host-resume

A heartbeat 403 (F7: restart-loss and reap are indistinguishable) makes the announcer
auto-re-announce with `{resume_identity, resume_token}`. The master verifies the HMAC
**and a strict exp** (the host's token is ≤30 s old thanks to the heartbeat push, so
strictness costs nothing while it makes long-expired leaked tokens useless) and then
**re-adopts both the prior hostIdentity and the prior sessionId** — which is what keeps
every connected client's token valid across the event.

- **Freshness-wins:** resume is refused if the live lobby heartbeated less than the
  freshness threshold ago (75 s, derived from the measured worst case of one missed beat
  → +60 s, and below the earliest possible reap at 90 s; **recomputed from the arc-1
  observations before arc 4 consumes it**). The gate can only ever fire when the row
  exists and is being heartbeaten — i.e. the double-host / partition case — because resume
  itself is triggered by the row's absence.
- **Refusal-reason split** (this is what keeps the cap from being a crutch): a network /
  unreachable failure applies **no** cap and keeps the normal heartbeat cadence; an
  explicit freshness refusal **concedes immediately**, since it proves a live incumbent.
- `resume_token` lives in memory only. A host **game-process** restart is therefore not a
  terminal case at all — the coop session is gone with the process, and re-hosting is the
  only meaningful action.
- **Accepted terminal outcome, written down now:** if a master outage outlives the 12 h
  token TTL, resume is refused and the host must re-host. No other lane can mint (the
  lobby bearer is random and dies with the row), and during such an outage there is no
  lobby listing anyway.

### 3.2 C3 — the test harness

Retiring the static bearer would break the harness at arc 4, so its token source is
designed now: a **dev mint subcommand** in the Rust binary
(`coop-signaling --mint <identity> <sessionId>`, reading the same secret) — one auth
scheme, no second code path in the server. Both `p2p_smoke.py` and `master_smoke.py` move
onto the Rust binaries (cargo 1.97.0 is present locally). The Python master + signaling
pair then has **zero consumers**, which resolves the long-pending RULE-2 deletion as a
consequence of this work rather than as an open question.

---

## 4. Consequences reported to the user (not forks)

- **D1 — the master-down P2P host fallback dies structurally.** `session_start` hard-fails
  on an empty token (F11) and under Tier C only the secret's holder can mint: a host-local
  mint would put the HMAC secret on the client (anyone could then mint any identity,
  including another lobby's `h`), and keeping a static bearer beside HMAC would be two
  parallel auth schemes — RULE 2 plus the reopened hijack door. So the dead plumbing is
  deleted. LAN and direct play are unaffected (F16).
- **D2 — self-hosting without a domain** uses the explicit plaintext scheme. Pinned
  self-signed support is future-additive (a new validation mode over the same grammar) and
  is not built now: no such user exists (zero releases), and OPUS §11 forbids speculative
  frameworks.

---

## 5. Sequencing — one axis per arc

Release build + deploy + hash-verify + smoke after **each** arc (OPUS §1). TLS listeners
run on **new ports beside** the plaintext ones during the cutover — mirroring this
project's own Python→Rust parallel-port precedent — so no intermediate state is knowingly
broken and no arc's smoke is a ceremony.

| Arc | Content | Gate |
|---|---|---|
| 1 | Server TLS on new ports; `COOP_SIGNALING_URL` swept to an **explicit `tcp://` hostname** (so every build keeps working in the window); accept-time logging; the 11-consumer tool sweep | The OBSERVE drill: real reap, real cadence, real post-restart heartbeat 403 |
| 2 | Client master → https | Smoke + outside `https` healthz |
| 3 | Client signaling → schannel | **Env-path proof only** — the master still hands out `tcp://`, so no master-flow claim is made here |
| 3b | The master-env **flip** to the schemeless TLS signaling URL | Hash-verify that all 4 installs carry arc-3 bytes **first**; then a host→join→P2P-over-TLS drill |
| 4 | Tier C tokens + host-resume + harness mint + Python-pair deletion | A coordinated cutover, explicitly not seamless |
| 5 | **Stop** the plaintext listeners, then close ufw 10000/10001 (**:80 stays open forever** for HTTP-01) | **Positive**: all 4 installs observed registering on the TLS listener with distinct identities. Backstop: zero unknown-source plaintext connections over 24 h in the accept log. Reversible in one command |

The mechanism that makes a mixed population impossible is the **atomic all-4 deploy**, not
the b122 version gate — that gate only covers joining someone else's lobby, not a client's
own signaling dial. Hence arc 3b's hash check before the env flip.

**Drills.** Positive: outside https healthz · TLS signaling handshake · client refresh ·
host heartbeat-pull · expired-token refresh via the server-side short TTL · lobby-dead
refresh refused · host-resume keeping the same identity+sessionId · freshness-wins refusal ·
host-misses-a-beat-then-resumes ACCEPTED · simultaneous restart through the **real deploy-hook
script** (plus `certbot renew --dry-run`, run **after** the arc-5 firewall change) ·
post-renewal restart-and-reconnect · 20-cycle reconnect leak drill · host→join→P2P e2e over
TLS · secret-fingerprint equality · smoke x2.
Negative (all producible today with the single real LE cert): dialing the TLS port by
**raw IP** must FAIL on hostname mismatch · a **local self-signed** rustls reached via env
must FAIL on an untrusted root · master https on a bare IP must FAIL.

---

## 5b. Arc 1 — AS-BUILT + measured (2026-07-20)

Live on the box; the plaintext pair keeps serving beside it.

- Certificate `master.multivoid.dev` issued via `certonly --standalone` (HTTP-01, :80),
  issuer = LE ECDSA intermediate **YE1**, valid 2026-07-20 .. 2026-10-18.
- `tokio-rustls` in both bins on **new ports 10443 (master) / 10442 (signaling)**;
  cert+key arrive via `LoadCredential` at `/run/credentials/<svc>/`.
- **`COOP_REQUIRE_TLS=1`** (new): with it set and no cert configured the service refuses to
  start — production fails **closed**, while the local harness keeps its plaintext mode.
  The TLS decision is resolved **before any bind**, so a fail-closed start never flaps a
  cleartext port open (drilled: the FATAL line appears with no preceding "listening" line).
- `COOP_SIGNALING_URL` swept from the **bare IP** to `master.multivoid.dev:10000`. Note the
  refinement to §5's arc-1 row: the value is a **bare host:port, not `tcp://`** — the
  shipped client cannot parse URL schemes yet and would `getaddrinfo("tcp://master…")`.
  The explicit scheme lands with the arc-3 client that understands it.
- Accept-time logging `accept [listener=plain] [ip]` on the plaintext listeners (the
  arc-5 gate's evidence base); TLS handshake failures logged, successful TLS accepts not.

**Drills run (all PASS).** TLS healthz from outside with the real chain (`Verification: OK`,
CN=master.multivoid.dev ← Let's Encrypt) · plaintext healthz still alive · **negative**: the
same endpoint dialled by bare IP is refused (curl 60) · **negative**: a local self-signed
rustls is refused · TLS 1.2 AND 1.3 both negotiate — 1.2 matters because curl on the dev box
uses schannel and could not do 1.3, which incidentally re-confirms the Win10 ceiling · relay
over the signaling TLS port using the token the master minted (full chain) · bad token
dropped · fail-closed and harness modes both drilled.

**OBSERVE drill (the numbers the design refused to leave constant-derived).**
Real reap latency = **111–116 s** after the last heartbeat — inside the constant-derived
90..120 s window, so the model is confirmed and the 75 s freshness threshold stands (the
earliest possible reap remains 90 s by construction). Real heartbeat against a dead session
= **403 `unknown session or bad token`** — C2's trigger is now observed, not inferred.

**Renewal (hardened after the user flagged it; /qf R20).**
`certbot.timer` enabled; deploy hook `/etc/letsencrypt/renewal-hooks/deploy/coop-restart.sh`
restarts both services (LoadCredential is a start-time snapshot) and now **verifies both are
active afterwards**, failing loudly if not.
- **MEASURED: `certbot renew --dry-run` does NOT execute deploy hooks.** A passing dry-run
  proves nothing about the restart. `--run-deploy-hooks` was used to exercise the real chain
  end to end: `ActiveEnterTimestamp` moved 10:05:10 → 10:19:43 with a `coop-cert` syslog line
  and all 4 listeners back. Wrong-lineage runs are a no-op.
- **The alarm lives OFF the box**: `tools/cert_check.py` reads the certificate the listener is
  actually *serving*, which in one observation proves renewal + hook + restart + snapshot
  freshness (reading the file on the server would prove only the first). It runs on the dev
  machine, so it also covers "the box is down" and "the timer never fired" — silence is not
  health. Verified: 89.9 days → OK (exit 0); an impossible threshold → FAIL (exit 1).
  A `/healthz` cert field was considered and **dropped** as pure redundancy.
- Documented residual: if nobody runs the tooling for ~2 months, nothing warns. Bounded by
  certbot renewing at 30 days left and retrying twice daily.
- `vps_provision.sh` now encodes all of it (certbot, cert issuance, the LoadCredential
  drop-ins, the hook, TLS ports, `COOP_REQUIRE_TLS`, the hostname signaling URL, ufw), so a
  re-provision reproduces the box instead of yielding open ports and no certificate.

**Build-pipeline change.** Adding TLS pulled in `ring`, whose musl build needs a C
cross-compiler that the Windows dev box does not have. The build therefore **moved onto the
box** (`musl-tools` + `gcc` + rustup there, `CC_x86_64_unknown_linux_musl=musl-gcc`), which
removes the need for any cross toolchain and still produces the same static musl ELF. A 2 GB
swapfile was added first so a 1-vCPU/961 MB box cannot OOM-kill the live services mid-build.

## 6. What Tier B/C does NOT close

Game traffic between peers was never in scope here: GNS encrypts it end-to-end already.
Tier B closes the *control* plane (signaling tokens, TURN credentials, lobby-list privacy
against an on-path observer); Tier C closes identity hijack by a token holder. Neither
addresses a malicious *peer* — that remains the game layer's join-secret challenge and the
host ban list.
