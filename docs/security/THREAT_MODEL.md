# Threat model — who the attacker is, and what is worth protecting

Written 2026-07-20, after 26 `/qf` rounds of design that never had one. This document is the
**ranking authority** for every severity in `TRACKER.md`: a finding is CRITICAL because of what it
does to an asset listed here, not because of how clever the attack is.

**Evidence tags used throughout this folder:** `[V]` measured personally with a file:line citation ·
`[A]` reported by a read-only audit agent, **not yet personally re-verified** · `[?]` unverified.

---

## 1. Who the attacker is

Players join lobbies from a **public list**, so **peers are strangers**. There is no account system
and no identity beyond what a client asserts. The realistic adversaries, in descending likelihood:

| # | Adversary | Capability | Cost to become |
|---|---|---|---|
| 1 | **A malicious peer** | Joins your lobby, or you join theirs. The mod itself is the toolkit; no privileges needed | Zero |
| 2 | **A malicious host** | You join a stranger's server and they feed you data — including a ~17 MB save blob | Zero |
| 3 | **Someone on the network path** | Shared wifi, hostel, hostile VPN exit, ISP, ARP spoofing on a shared LAN | Low, but must be *positioned* |
| 4 | **A stranger with `curl`** | The master's endpoints are largely unauthenticated — no game client required | Zero |

**The direction people forget is #2.** Joining a stranger's lobby is at least as dangerous as
hosting one, because the host hands the client a save blob that goes into a native deserializer
(`TRACKER` **S1**).

### Explicitly out of scope as an adversary

- **A determined attacker with the CA private key**, or root on our VPS. If the master is owned,
  everything downstream of it is owned. Defended by ordinary server hygiene, not by this model.
- **A cheating peer in the game-mechanics sense** (aimbot, item duping for their own benefit).
  That is a fairness problem, not a security one, and at a 3-peer cap among people who chose to play
  together it is social. It becomes in-scope only if a mechanic cheat can corrupt *another* player's
  persistent world — at which point it is filed as an authority finding (**A3**, **A4**).

---

## 2. What is worth protecting

Ranked by **real harm to real people**, which is not the same as ranked by technical severity.

| # | Asset | Why it ranks here | Findings that touch it |
|---|---|---|---|
| 1 | **Privacy of voice + text chat** | Live humans talking, believing the game is private. Harm is immediate, personal, and cannot be undone | **P1** |
| 2 | **Integrity of a host's world/save** | VOTV saves are persistent and represent tens of hours. Destruction is irreversible — there is no server-side backup | **A3**, **A4** |
| 3 | **Availability of your own lobby** | Being evicted from your own session by a stranger, or having the lobby list flooded | **A1**, **A7** |
| 4 | **The client process not being crashed or exploited** | One packet should never kill a player. Memory-safety bugs escalate past a crash | **W1**, **W2**, **W3**, **W4**, **W5**, **S1** |
| 5 | **A host's home IP** | Most hosts are on residential connections; the address is personally identifying | **A9** |
| 6 | **Our VPS bandwidth and the lobby list's usefulness** | Our operating cost, and everyone's ability to discover games | **A6**, **A7** |

Assets 1-4 belong to **users**. Asset 6 belongs to **us**. When they conflict, users win.

---

## 3. What we deliberately do NOT protect

Writing these down matters as much as the list above — **they are where effort was being wasted.**
A mechanism justified only by an item in this section should not be built.

### Confidentiality of the lobby list

It is public data by construction: the entire point is that anyone can enumerate it. Nothing is
gained by encrypting it. *(The user identified this unprompted, and was right — it is what triggered
the whole reframe.)*

### Confidentiality of `signalingToken`

`[V]` `master.rs` `ice_block()` hands **the same static shared secret to every client**. Anyone who
installs the mod has it. Encrypting its transmission is theatre.

The fix is not a better-protected token — it is to stop having a shared secret be load-bearing for
identity at all (`TRACKER` **A1**, `PLAN_01_PEER_AUTH.md`). See
`[[lesson-shared-secret-handed-to-every-user-is-not-a-secret]]`.

### Ban evasion by changing IP or identity

Inherent without accounts, and the GUID is client-supplied so it is equally forgeable. At a 3-peer
cap this is a social problem, not an engineering one.

**Distinct from `TRACKER` S2**, which is a *correctness bug* in the ban mechanism (it may write our
own TURN address into the banlist) and is in scope.

### Protecting a host from a peer who was invited

If you hand someone your lobby address and they join, they are inside your world by your own
decision. We defend the **integrity of the shared world against unilateral destruction** (A3/A4) —
we do not attempt to make a joined peer harmless.

---

## 4. The one-sentence version

> Peers are strangers on an encrypted-but-**unauthenticated** transport, so the control plane is the
> only place identity can be established — and everything a peer sends must be treated as hostile at
> the moment it is **applied**, not at the moment it was sent.

That sentence generates the two largest work items in this folder: `PLAN_01_PEER_AUTH.md` (establish
identity) and `PLAN_02_WIRE_HARDENING.md` + `PLAN_03_AUTHORITY.md` (distrust the applied byte).

---

Next: `SUBSTRATE.md` — the measured facts this model rests on.
