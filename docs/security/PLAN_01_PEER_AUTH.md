# Plan 01 — peer authentication (the root fix)

**Closes:** `TRACKER` **P1** (peers unauthenticated) + **A1** (signaling identity self-asserted).
**Dissolves:** Tier C, the shared-`signalingToken`-as-identity problem, and most of the rationale for
TLS arcs 3/3b/5.
**Status: DESIGN — gated on the spike in §2. No code written.**

This is the largest and highest-value item in the folder. Everything else in `TRACKER.md` is a
bounded bug; this is a missing architectural layer.

---

## 1. The root

`SUBSTRATE.md` §1: GNS encrypts with AES-256-GCM but leaves `IP_AllowWithoutAuth = 2`, so the
Diffie-Hellman exchange is unauthenticated. An active attacker at the rendezvous runs two exchanges
and relays — **both ends see a healthy encrypted connection.**

`SUBSTRATE.md` §3: the only thing gating signaling registration is a static token every mod user
holds, and the identity string is taken verbatim from the greeting, with `cl.insert` evicting the
incumbent.

These are one problem: **nothing in the system binds a connection to a chosen peer.**

### The converged direction

> The master runs a small CA. On `/v1/host` and `/v1/join` it mints a **short-lived GNS certificate**
> binding that peer's identity, signed by our CA key. Clients set `IP_AllowWithoutAuth = 0` and carry
> the CA public key **compiled in**. GNS then authenticates peers cryptographically, by itself.

Why this is the right layer:

- Kills the middle-man attack **at the root** — a certificate cannot be forged without the CA key, so
  it stops mattering whether the attacker reached the rendezvous.
- **Dissolves Tier C**: the certificate *is* the identity proof; per-session tokens were groping at
  the same problem with weaker crypto.
- Makes A1 disappear rather than patching it: a registration is accepted because it presents a signed
  identity, not because it knows a public constant.
- Does not touch the transport, does not need the master rewritten, does not retire arcs 1-2.

---

## 2. HALT GATE — the spike, before any implementation

**Do this first. Nothing else in this plan may start until the spike reports.**

The spike is a **local two-process run**, no game, no network:

1. Build Valve's `certtool` from `src/steamnetworkingsockets/certtool/`.
2. Mint a CA keypair and a leaf cert for a test identity.
3. In a minimal harness, call `CertStore_AddCertFromBase64` to install the CA root, and
   `SetCertificate()` to present the leaf.
4. Set `IP_AllowWithoutAuth = 0` on both processes.
5. Connect them.

### What the spike must report

| Question | Why it gates the design |
|---|---|
| Do two processes connect at all with `AllowWithoutAuth = 0`? | If not, the whole plan is dead and we fall back to option C (signed master responses, `DECISIONS.md` §2) |
| Does an **unsigned** peer get rejected, with a distinguishable error? | If rejection is silent or indistinguishable from a network failure, the UX and the diagnostics both need design |
| Is `certstore.h` (an internal `src/` header) actually linkable from our build? | `SUBSTRATE.md` §2 caveat — if not, we need a different install path for the root |
| What does cert **expiry** do to a live connection? | Decides TTL and whether a long session needs re-minting mid-game |
| How large is the cert on the wire, and what does minting cost the master? | Decides whether minting on every `/v1/join` is affordable |

**Record the spike's answers in this file (§7) before writing production code.** A "it probably
works" from reading Valve's source is not a spike result.

---

## 3. Implementation arcs (post-spike, order matters)

Each arc is independently shippable and independently reversible.

### Arc A — CA infrastructure on the VPS

- Generate the CA keypair **offline**; the private key never enters the repo, the container, or a log.
- Store it with the same discipline as the TLS key: systemd `LoadCredential`, root-only.
- Write down the recovery procedure: what happens if the CA key is lost (all clients need a new
  compiled-in root → a build + a forced update) and if it is *leaked* (revocation via
  `CertStore_AddKeyRevocation`, plus a root rotation).
- **Acceptance:** the master can mint and print a valid leaf cert on request; `certtool` verifies it
  against the root.

### Arc B — minting at the control-plane endpoints

- `/v1/host` and `/v1/join` return a leaf cert bound to the identity that endpoint just issued.
- **TTL short** — long enough for a join (minutes), not for a session. Exact value from the spike's
  expiry answer.
- Rate-limit minting per IP, mirroring the A6 fix — a mint is a signature operation and must not
  become a CPU amplifier.
- **Acceptance:** `curl` against a staging master returns a cert that `certtool` validates; a second
  request for the same identity from a different IP is refused or rate-limited.

### Arc C — client side

- Compile in the CA root; install it at session start via `CertStore_AddCertFromBase64`.
- Present the minted leaf via `SetCertificate()`.
- Flip `IP_AllowWithoutAuth` to `0`.
- **Wire-format change → bump `kProtocolVersion`** per the standing rule.
- **Acceptance:** two real peers connect; a peer with no cert is rejected; the rejection surfaces a
  readable message, not a generic timeout.

### Arc D — the direct-connect answer

`SUBSTRATE.md` §4: direct hosting is deliberately master-independent, and **this plan must not break
it.** A peer connecting to `1.2.3.4:7777` with no master involved has no minted certificate.

Options, to be decided **during** the spike, not after:

| Option | Shape | Cost |
|---|---|---|
| **D1** | Direct connections run `AllowWithoutAuth = 2` as today, and the UI marks the session as unauthenticated | Honest, cheap, preserves the RULE-1 direct path. Two code paths — needs a RULE-2 justification |
| **D2** | Host self-signs; the client pins on first use (TOFU) | No master dependency, but TOFU on a one-shot connection buys little |
| **D3** | Direct connect requires a cert fetched earlier from the master | Kills the offline/LAN case. **Probably unacceptable** |

**Current lean: D1**, because the threat model's path attacker is far less likely on a hand-shared
direct address, and killing the offline path to close a lesser vector is the wrong trade. Not
decided.

---

## 3b. The MTA shape that applies here

`[A]` `MTA_PRECEDENT.md` §8. MTA never reads the sender from a packet field: the serial is fetched
from the **network layer keyed by the connection** (`CGame.cpp:1829-1842`), and
`CPacketTranslator.cpp:228-249` resolves the source player from the **socket**, destroying any packet
that declares `RequiresSourcePlayer()` before parsing if no player maps to it.

> **That is A1's fix in one sentence:** `hostIdentity` and peer slot must come from the transport
> (GNS) identity, never from the signaling greeting or a payload body.

**Important negative — do not model our peer auth on MTA's.** How the serial is *derived and attested*
is not in the repo: `GetClientSerialAndVersion` is a pure virtual
(`Server/sdk/net/CNetServer.h:149`) forwarded into the closed-source `net.dll`. Only the *shape*
ports; the load-bearing part is unpublished. Our certificate plan is strictly better than what MTA can
show us here.

Worth remembering for §5: `CAccountManager.cpp:566-583` is a TOFU pattern — for privileged accounts a
correct password is **not sufficient**, the serial must be pre-authorized, and an unknown one is
recorded as *pending* with an out-of-band approval step rather than allowed or silently refused.

## 4. What this plan explicitly does NOT do

- It does not authenticate *people*, only connections. A stranger who joins your public lobby is
  still a stranger — that is `PLAN_03_AUTHORITY.md`'s problem.
- It does not stop a malicious host feeding a hostile save blob (**S1**).
- It does not make the master trustworthy to itself: whoever holds the CA key can impersonate anyone
  (`THREAT_MODEL.md` §1, out of scope).

---

## 5. Interaction with the self-hoster

A self-hosted master needs its own CA identity, and clients compile in **our** root. Unresolved:

- Does a self-hoster's cohort get a different build, a config-supplied root, or D1 treatment?
- If a config-supplied root is accepted, the "insecure master" question returns — but this time with
  an actual security meaning, unlike the flag dropped in `DECISIONS.md` §2.

**Decide when the self-hoster cohort exists.** It does not yet.

---

## 6. Open questions

1. `certstore.h` is an internal header — acceptable long-term? (spike answers half of this)
2. Server-side minting cost per join at scale.
3. Cert TTL vs session length; re-mint mid-session or not.
4. Revocation UX — how does a client learn its cert was revoked?
5. The direct-connect option (§3 Arc D).
6. Self-hoster root distribution (§5).

---

## 7. Spike results

**Not run.** Fill this section in before starting Arc A, with the raw output — not a summary.

---

Back to: `README.md` · `TRACKER.md` rows **P1**, **A1**.
