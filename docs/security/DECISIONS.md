# Decision record — what was decided, what was retracted, and why

The purpose of this file is **to stop a future session re-deriving a position that already died to a
measurement.** Every row here cost real time to reach. Read it before proposing a security
mechanism; if your idea is in the "retracted" table, the burden is on you to produce a *new*
measurement, not a new argument.

---

## 1. What the TLS work actually bought (2026-07-20)

Arcs 1 and 2 — server TLS on new ports with Let's Encrypt + auto-renewal, and client master traffic
over TLS — are **BUILT, COMMITTED and LIVE** (`7aff6b73`, `87e66bce`). That part of
`research/findings/network/votv-tls-tier-b-c-DESIGN-2026-07-20.md` is accurate as an as-built record.

The honest assessment:

- TLS gives the master channel **confidentiality** — of data `THREAT_MODEL.md` §3 says is not worth
  hiding — and **transport integrity**, which *does* matter: it stops a path attacker rewriting
  `signalingUrl` in flight.
- It closes **none** of the findings in `TRACKER.md` completely.
- Once peers are authenticated by certificate, TLS on the control plane becomes reasonable
  defence-in-depth rather than a load-bearing control.

**Verdict: keep it, do not treat it as the security plan.** The remaining arcs (3, 3b, 5) are ON
HOLD pending the CA spike — arc 3 puts schannel inside the 200 Hz `Poll()`, which is the most
delicate work in the sequence, and it may turn out to be unnecessary.

### Status of every TLS arc

| Arc | What it was | Status as of 2026-07-20 |
|---|---|---|
| 1 | Server TLS, rustls, parallel ports 10443/10442, LE + renewal | **BUILT + LIVE** (`7aff6b73`) |
| 2 | Client reaches master over TLS; schemeless = secure | **BUILT + LIVE** (`87e66bce`) |
| 3 | Signaling transport over TLS (schannel in the 200 Hz `Poll()`) | **ON HOLD** — may be unnecessary |
| 3b | Port flip / plaintext-port retire | **ON HOLD** — depends on 3 |
| 4 | "Tier C" per-session signaling tokens | **DISSOLVED** — superseded by peer certs (`PLAN_01`) |
| 5 | Retire the plaintext pair behind a positive gate | **ON HOLD** — depends on 3/3b |

---

## 2. Positions retracted 2026-07-20 — do not re-derive

Each died to a measurement, not to an argument.

| Position held | Why it died |
|---|---|
| Flip the master env to `tcp://host:port` as arc 3's step 0 | `[V]` `signaling_client.cpp:113-118` splits on `rfind(':')` with no scheme strip → `getaddrinfo("tcp://…")` fails. **Would have broken all 4 deployed installs at once.** My own comment in `vps_provision.sh:170-173` already said this; I built on memory of it instead of re-reading |
| "SCHEMELESS = SECURE" is one grammar spanning both transports | A label minted for a WinHTTP flag, then over-generalized. The real invariant: **config fixes transport on both ends and no upgrade negotiation exists, so a mismatch always fails loudly** and can never silently downgrade |
| Arc 2 shipped a remote "downgrade vector" | `[V]` all 7 http call sites pass the **local** `masterUrl`; no remote string ever reaches the scheme parser. The scheme retire is **RULE 2 hygiene only**, not a security fix |
| Provenance = compare the effective endpoint against the compiled constant | `[V]` `protocol.h:1086-1087` — the arc-3b port flip would break the string compare. Raised to the correct invariant: **provenance is a property of the MASTER, decided once in `Configure()`; everything it supplies inherits it** |
| The arc-3 cutover cannot be seamless | An unexamined default. `[V]` the version gate fires at the top of `JoinLobby` before any rendezvous, and the population is 4 atomically-deployed installs |
| …so serve the endpoint by client version | Over-engineering for a **measurably empty** cohort, and a *second* mechanism beside the version gate (RULE 2). Both cutover plans dropped |
| A `net.master.insecure` ini flag is needed | Symptom of having no threat model. It selects a transport for a control plane whose confidentiality is worth ~nothing, while the actual authentication gap sits in GNS. **Do not build it** unless a future plan lands somewhere that genuinely needs it |
| Sign the master's responses with a pinned key (option "C") | Not wrong, but **partial** — it prevents a rewritten `signalingUrl` and leaves the GNS connection itself unauthenticated. Patches one vector; peer certs close the class |

---

## 3. The false trilemma (the most useful thing learned this session)

Three options were weighed for closing the peer-identity gap:

- **A** — TLS everywhere (half-built)
- **B** — move the control plane onto GNS **with our own CA**
- **C** — sign master responses with a pinned key

**B was priced as expensive** — the master is Rust, GNS is C++, UDP-only control plane, a hand-rolled
RPC, arcs 1-2 thrown away — so the comparison collapsed into "is B worth that cost?"

A critic round split B in two:

1. **"control plane over GNS"** — genuinely expensive, and
2. **"our own CA for peer certificates"** — cheap, and it **does not touch the transport at all**.

Only the second was ever required. The trilemma was never real: the answer is the cheap half of B,
and A and C become secondary questions about a channel whose confidentiality the threat model values
at ~zero.

Durable lesson: `[[lesson-split-fused-options-before-comparing-architectures]]`.

---

## 4. Standing decisions (current, not retracted)

| Decision | Rationale | Date |
|---|---|---|
| **Peer certificates from our own CA are the root fix** | Closes the class rather than a vector; needs no new client dependency (`SUBSTRATE.md` §2) | 2026-07-20 |
| **Tier C (per-session signaling tokens) is cancelled** | Weaker crypto groping at the same problem the certificate solves outright | 2026-07-20 |
| **Arcs 1-2 stay shipped** | Defence in depth, already paid for, zero ongoing cost | 2026-07-20 |
| **Validation belongs on the apply side** | Send-side caps are not caps; a hostile peer does not run our sender | 2026-07-20 |
| **Escaping belongs at the render layer** | Mangling game strings to protect a web page is the wrong layer and breaks legitimate names | 2026-07-20 |
| **The threat model is written before the mechanism** | 26 `/qf` rounds produced a mechanism aimed at the wrong target | 2026-07-20 |

---

Next: `RULES.md` (the standing engineering rules) · `PLAN_01_PEER_AUTH.md` (where the root fix goes).
