# docs/security/ — the security knowledge base (threat model + source of truth)

**The concept (folder-per-domain-concept rule):** everything about **what an attacker can do to a
Multivoid player, host, or to our own infrastructure, and what we do about it.** Not "encryption",
not "TLS" — those are mechanisms, and this folder exists precisely because the project spent a day
and a half building a mechanism before anyone wrote down the threat.

Two files, the established README+TRACKER shape:

- **This file** — the threat model, the measured substrate facts, what we deliberately do NOT
  protect, and the design history (including what was retracted, so nobody re-derives it).
- **`TRACKER.md`** — one row per finding: severity, evidence, status, the fix layer.

Cross-cutting contracts stay where they live; this folder LINKS, never restates (RULE 2):
`docs/COOP_SYNC_MAP.md` (which file owns a lane) · `docs/COOP_DISPATCH_VISIBILITY.md` ·
`research/findings/network/votv-tls-tier-b-c-DESIGN-2026-07-20.md` (the TLS design of record —
still valid as a record of what was built, but read section "What the TLS work actually bought"
below before treating it as the plan).

---

## 1. The threat model

Written 2026-07-20, after 26 `/qf` rounds of design that never had one. **Every claim below is
tagged.** `[V]` = measured this session with a file:line citation. `[A]` = reported by a read-only
audit agent, NOT yet personally re-verified. `[?]` = unverified.

### Who the attacker is

Players join lobbies from a **public list**, so **peers are strangers**. There is no account system
and no identity beyond what a client asserts. The realistic adversaries, in descending likelihood:

1. **A malicious peer** — anyone who joins your lobby, or whose lobby you join. Needs no privileges;
   the mod itself is the toolkit. This is the overwhelming majority of the real risk.
2. **A malicious host** — you join a stranger's server and they feed you data (a ~17 MB save blob).
   The direction people forget: joining is at least as dangerous as hosting.
3. **Someone on the network path** — shared wifi, hostel, hostile VPN exit, ISP. Cheap to become
   (ARP spoofing on a shared LAN is a standard utility), but must be *positioned*.
4. **A stranger with `curl`** — the master's endpoints are largely unauthenticated, so parts of the
   attack surface need no game client at all.

### What is worth protecting (ranked by real harm to real people)

| # | Asset | Why it matters |
|---|---|---|
| 1 | **Privacy of voice + text chat** | Live humans talking, believing the game is private |
| 2 | **Integrity of a host's world/save** | VOTV saves are persistent; destruction is irreversible |
| 3 | **Availability of your own lobby** | Being evicted from your own session by a stranger |
| 4 | **The client process not being crashed/exploited** | One packet should never kill a player |
| 5 | **A host's home IP** | Most hosts are on residential connections |
| 6 | **Our VPS bandwidth + the lobby list's usefulness** | Our cost, everyone's discovery |

### What we deliberately do NOT protect, and why

Writing these down matters as much as the list above — they are where effort was being wasted.

- **Confidentiality of the lobby list.** It is public data by construction. Nothing is gained by
  encrypting it. (The user identified this unprompted; he was right.)
- **Confidentiality of `signalingToken`.** `[V]` `master.rs` `ice_block()` hands the same static
  shared secret to every client. Anyone who installs the mod has it. Encrypting its transmission is
  theatre; the fix is to stop having a shared secret at all (TRACKER **A1**).
- **Ban evasion by changing IP/identity.** Inherent without accounts, and a GUID is client-supplied
  so it is equally forgeable. A social problem at a 3-peer cap, not an engineering one. (Distinct
  from TRACKER **S2**, which is a *correctness bug* in the ban mechanism.)

---

## 2. The measured substrate

The facts everything else rests on. Re-measure before contradicting.

### The game data path is encrypted but NOT authenticated `[V]`

- `third_party/GameNetworkingSockets/.../csteamnetworkingsockets.cpp:97` —
  `Unencrypted` default `0` = "not allowed". We never override it (grepped the whole `src/` tree).
  Cipher is **AES-256-GCM** (`steamnetworkingsockets_connections.cpp:1243-1255`), keys from an
  ephemeral Curve25519 exchange.
- Same file `:88-91` — in the **opensource** build `IP_AllowWithoutAuth` defaults to **2**, with
  Valve's own comment *"We don't have a trusted third party, so allow this by default, and don't
  warn about it."* We never override it either.
- `include/steam/steamnetworkingtypes.h:1250-1262` documents `2` as **"don't attempt
  authentication, or complain if peer is unauthenticated."**

**Consequence, and it is the single most important fact in this folder:**

> A **passive** eavesdropper cannot read voice or chat — that protection is real and already
> working. An **active** attacker who reaches the rendezvous can run two key exchanges and sit in
> the middle, because unauthenticated Diffie-Hellman offers no defence against one. Both ends see a
> healthy encrypted connection.

Because GNS does not authenticate peers, **the control plane is the only place in the entire system
where "the peer you connected to is the peer you chose" can be established.** That is why control-plane
integrity matters and control-plane confidentiality mostly does not.

### GNS ships a full CA implementation we have never used `[V]`

- `src/steamnetworkingsockets/steamnetworkingsockets_certstore.h:112-147` —
  `CertStore_AddCertFromBase64` (install a trusted root), `CertStore_CheckCert`,
  `CertStore_AddKeyRevocation`. Caveat: this is `src/`, an **internal** header, not `include/`.
- `include/steam/isteamnetworkingsockets.h:804` — `SetCertificate()` is public API.
- `src/steamnetworkingsockets/certtool/` — Valve's Ed25519 cert mint/sign tool.
- `src/common/crypto_25519.h:125,146` — `GenerateSignature` / `VerifySignature` are already linked
  into our process.

So peer authentication needs **no new client dependency**. See section 4.

### The control plane hands out a shared secret `[V]`

`tools/coop-server-rs/src/bin/master.rs` `ice_block()` returns `CFG.signaling_token` — identical for
every client — plus HMAC-minted, time-limited TURN credentials, on every `/v1/host` and `/v1/join`.

---

## 3. What the TLS work actually bought

Arcs 1 and 2 (server TLS on new ports with Let's Encrypt + auto-renewal; client master traffic over
TLS) are **BUILT, COMMITTED and LIVE** — that part of
`research/findings/network/votv-tls-tier-b-c-DESIGN-2026-07-20.md` is accurate as-built.

The honest assessment, written 2026-07-20:

- TLS gives the master channel **confidentiality** (of data this model says is not worth hiding) and
  **transport integrity** (which does matter — it stops a path attacker rewriting `signalingUrl`).
- It closes **none** of the findings in `TRACKER.md` completely.
- Once peers are authenticated by certificate (section 4), TLS on the control plane becomes a
  reasonable defence-in-depth layer rather than a load-bearing one.

**Keep it. Do not treat it as the security plan.** The remaining TLS arcs (3, 3b, 5) are on hold
pending the section-4 decision, because arc 3 (schannel inside the 200 Hz `Poll()`) is the most
delicate work in the sequence and may be unnecessary.

### Design positions retracted this session — do not re-derive them

Recorded so a future session does not walk back into them. Each died to a measurement.

| Position I held | Why it died |
|---|---|
| Flip the master env to `tcp://host:port` as arc 3's step 0 | `[V]` `signaling_client.cpp:113-118` splits on `rfind(':')` with no scheme strip → `getaddrinfo("tcp://…")` fails. Would have broken all 4 installs |
| "SCHEMELESS = SECURE" is one grammar for both transports | A label minted for a WinHTTP flag. The real invariant: config fixes transport on both ends, no upgrade negotiation exists, so a mismatch **always fails loudly** and can never silently downgrade |
| Arc 2 shipped a remote "downgrade vector" | `[V]` all 7 http call sites pass the LOCAL `masterUrl`; no remote string ever reaches the scheme parser. The scheme retire is **RULE 2 only**, not a security fix |
| Provenance = compare the effective endpoint to the compiled constant | `[V]` `protocol.h:1086-1087` — the arc-3b port flip would break the string compare. Raised to: **provenance is a property of the MASTER, decided once; everything it supplies inherits it** |
| The arc-3 cutover cannot be seamless | An unexamined default. `[V]` the version gate fires at the top of `JoinLobby` before any rendezvous, and the population is 4 atomically-deployed installs |
| …so serve the endpoint by client version | Over-engineering for a measurably empty cohort, and a **second** mechanism beside the version gate (RULE 2). Both cutover plans dropped |
| A `net.master.insecure` flag is needed | Symptom of having no threat model. It selects a transport for a control plane whose confidentiality is worth ~nothing, while the actual authentication gap sits in GNS. **Do not build it** unless section 4 lands somewhere that needs it |

---

## 4. The proper fix, and the open decision

The threat model points at one root: **nothing authenticates a peer.** Three candidate shapes were
weighed; the trilemma turned out to be false — "control plane over GNS" and "our own CA for peer
certs" are independent, and only the second is required.

**The converged direction (DESIGN, not built):**

> The master runs a small CA. On `/v1/host` and `/v1/join` it mints a short-lived GNS certificate
> binding that peer's identity, signed by our key. Clients set `IP_AllowWithoutAuth = 0` and carry
> the CA public key compiled in. GNS then authenticates peers cryptographically, by itself.

Why this is the right layer:

- Kills the middle-man attack **at the root** — a certificate cannot be forged without the CA key,
  so it does not matter whether the attacker reached the rendezvous.
- **Dissolves Tier C.** Per-session identity tokens were groping at the same problem with weaker
  crypto; the certificate *is* the identity proof.
- Dissolves the shared `signalingToken` as a security-relevant object.
- Does not touch the transport, does not need the master rewritten, does not retire arcs 1-2.

**The gating measurement, not yet done:** a local two-process run — `certtool` mints a CA,
`CertStore_AddCertFromBase64` installs the root, `IP_AllowWithoutAuth = 0`, two processes connect.
That decides whether peer authenticity is achievable on our build at all, and it settles the whole
architecture question before any code is written. **Do this first.**

Open sub-questions, unresolved: how a self-hoster's master gets a CA identity clients will trust;
whether the internal `certstore.h` dependency is acceptable long-term; server-side minting cost.

---

## 5. Standing rules this folder enforces

1. **Never write a comment asserting a security control that does not exist.** Two were found in one
   day (`TRACKER` **A2**, **W6**), and in both cases the false comment is *why* the gap persisted —
   a later reader trusted it. If a control is absent, the comment must say so, as
   `event_dispatch_entity.cpp:259-264` correctly does.
2. **Validate before relay.** A forged op must not be fanned out to other peers ahead of the host's
   own validation (`TRACKER` **A3**).
3. **Caps belong on the apply side, not only the send side.** Every "the peer is a well-behaved copy
   of this build" assumption is a vulnerability; that assumption is the root of the entire
   Critical/High block in `TRACKER.md`.
4. **Escaping belongs at the render layer.** Do not mangle game-facing strings to protect a web page
   (`TRACKER` **A8**).
5. **Verify a handed-down finding before fixing it.** Everything tagged `[A]` came from an audit
   agent reading code. Re-read the site yourself first — per
   `[[feedback-verify-handed-down-measurement-before-building]]`.
