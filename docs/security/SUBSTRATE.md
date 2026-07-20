# The measured substrate — facts everything else rests on

**Re-measure before contradicting anything here.** Each claim carries its citation. If a citation no
longer resolves, fix the citation in this file — do not silently keep the claim.

Last verified: **2026-07-20**, HEAD `cc0d8686`.

---

## 1. The game data path is encrypted but NOT authenticated `[V]`

This is **the single most important fact in this folder.**

### Encryption is mandatory and real

- `third_party/GameNetworkingSockets/.../csteamnetworkingsockets.cpp:97` — `Unencrypted` defaults to
  `0` = "not allowed". We never override it (grepped the whole `src/` tree).
- Cipher is **AES-256-GCM** (`steamnetworkingsockets_connections.cpp:1243-1255`), with keys from an
  ephemeral Curve25519 exchange.

### Peer authentication is off by default, and we never turn it on

- Same file `:88-91` — in the **opensource** build `IP_AllowWithoutAuth` defaults to **2**, carrying
  Valve's own comment: *"We don't have a trusted third party, so allow this by default, and don't
  warn about it."* We never override it either.
- `include/steam/steamnetworkingtypes.h:1250-1262` documents `2` as **"don't attempt authentication,
  or complain if peer is unauthenticated."**

The default differs between builds: the Steam build gets `0` because Steam *is* the trusted third
party. We took the opensource build and inherited the permissive default without noticing.

### The consequence

> A **passive** eavesdropper cannot read voice or chat — that protection is real and already
> working. An **active** attacker who reaches the rendezvous can run two key exchanges and sit in
> the middle, because unauthenticated Diffie-Hellman offers no defence against one. **Both ends see
> a healthy encrypted connection**, with no warning, no indicator, and nothing in any log.

Because GNS does not authenticate peers, **the control plane is the only place in the entire system
where "the peer you connected to is the peer you chose" can be established.** That is why
control-plane *integrity* matters and control-plane *confidentiality* mostly does not — and it is
why `PLAN_01_PEER_AUTH.md` is the root fix rather than more transport encryption.

Full lesson: `[[lesson-gns-encrypted-but-peer-unauthenticated]]`.

---

## 2. GNS ships a full CA implementation we have never used `[V]`

Peer authentication needs **no new client dependency** — the machinery is already linked into our
process.

| Capability | Location | Note |
|---|---|---|
| Install a trusted root | `src/steamnetworkingsockets/steamnetworkingsockets_certstore.h:112-147` — `CertStore_AddCertFromBase64` | **Internal `src/` header, not `include/`** — see caveat below |
| Validate a cert chain | same file — `CertStore_CheckCert` | |
| Revoke a key | same file — `CertStore_AddKeyRevocation` | Gives us a revocation story if a peer cert leaks |
| Present our own cert | `include/steam/isteamnetworkingsockets.h:804` — `SetCertificate()` | **Public API** |
| Mint / sign certs | `src/steamnetworkingsockets/certtool/` | Valve's Ed25519 tool; the CA side |
| Raw sign / verify | `src/common/crypto_25519.h:125,146` — `GenerateSignature` / `VerifySignature` | Already linked |

**Caveat, and it is the open risk in the whole plan:** `certstore.h` lives under `src/`, meaning it
is not part of GNS's supported public surface and could change shape on an upstream bump. We vendor
GNS, so this is survivable, but it is a real long-term dependency question — tracked as an open
sub-question in `PLAN_01_PEER_AUTH.md` §6.

---

## 3. The control plane hands out a shared secret `[V]`

`tools/coop-server-rs/src/bin/master.rs` `ice_block()` returns:

- `CFG.signaling_token` — **identical for every client**, read from the server env, and
- HMAC-minted, time-limited TURN credentials,

on **every** `/v1/host` and `/v1/join`.

Two consequences, and the second is the one that matters:

1. Encrypting the token's transmission protects nothing — an attacker who wants it installs the mod.
2. **A shared bearer cannot authenticate anybody.** `signaling.rs:166-217` authorizes a registration
   with the token alone, then takes the identity string **verbatim from the greeting**, and
   `cl.insert` **evicts the incumbent**. That is `TRACKER` **A1**.

---

## 4. Direct hosting is genuinely master-independent `[V]`

`session_manager.cpp:341-372` — a host can run with no master at all, and a client can connect
straight to an open port. This was built per RULE 1 and is **not** a fallback path bolted on.

Security-relevant because it bounds the master's blast radius: a compromised or offline master
degrades **discovery**, not the ability to play. Any plan that makes the master load-bearing for
*connection* (as naive peer-cert minting would) must keep this path working — see
`PLAN_01_PEER_AUTH.md` §5, which is where the direct-connect answer is owed.

---

## 5. What the TLS work (arcs 1-2) actually changed

See `DECISIONS.md` for the full assessment. In substrate terms: the **master channel** now has
transport confidentiality and integrity. The **game data path between peers** is untouched by it —
that path was already encrypted, and is still unauthenticated. TLS on the control plane closed no
finding in `TRACKER.md`.

---

Next: `TRACKER.md` — the findings this substrate produces.
