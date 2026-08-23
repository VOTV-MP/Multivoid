# Security policy

Multivoid is **alpha** software that puts your game session on a network. This page says how to
report a problem, what we consider in scope, and — honestly — what protection the mod does and does
not give you today.

## Reporting a vulnerability

**Please do not open a public issue for a security problem.** Use one of these instead:

| Channel | How |
|---|---|
| **Preferred** | GitHub → [**Report a vulnerability**](https://github.com/VOTV-MP/Multivoid/security/advisories/new) (private advisory; only the maintainer sees it) |
| Alternative | A direct message to **Pelmentor** on the [Discord](https://discord.gg/bA6tGBvGMN) |

A good report contains: what you did, what happened, and what an attacker gains. If you have a
proof of concept, keep it in the private report — do not post it publicly.

**What to expect.** This is a one-person hobby project, not a company: there is no bounty, no SLA,
and no security team. What you will get is an acknowledgement within a few days, an honest answer
about whether it is already known, and credit in the release notes when the fix ships — unless you
would rather stay anonymous. If a report shows people are actively at risk, it jumps the queue.

## Scope

**In scope** — anything in this repository and the services it talks to:

- the mod itself (`multivoid-*.dll`) — the network layer, the save transfer, the message parsers,
  the authority checks, the overlay
- the master / signaling servers (`tools/coop-server-rs/`) and `master.multivoid.dev`
- the website and the release/installer path (a malicious update channel, a tampered artifact)

**Out of scope** — report these to the people who own them, not to us:

- *Voices of the Void* itself, Unreal Engine 4, and anything in a vendored dependency
  (GameNetworkingSockets, MinHook, Dear ImGui, UE4SS)
- cheating in a co-op session — see below; it is a design position, not a vulnerability
- anything that needs the attacker to already be running code on your machine

## What Multivoid protects, and what it does not

Being straight about this matters more than sounding secure.

**What holds:** peer traffic runs over GameNetworkingSockets and is **encrypted** (AES-256-GCM), so
someone sniffing the network between two players does not read the session. Traffic with the master
and signaling servers runs over TLS. The mod never touches original game files and needs no
elevated privileges.

**What does not hold, and you should plan around it:**

- **Sessions are not hardened against a hostile participant.** Multivoid is built for playing with
  people you know. The host trusts joiners far more than a public-server game would, and a peer who
  deliberately modifies their client can affect the shared world. **Do not host a session for
  strangers and expect the mod to defend your save.** Back up saves before testing — that advice on
  the README is not boilerplate.
- **Peer identity is weak today.** Encryption is on, but the co-op transport does not yet prove
  *who* is on the other end; establishing that properly is active work and the next security arc.
- **Cheat prevention is deliberately not a feature.** Co-op is not competitive, and an
  anti-cheat layer is a different problem from an authority layer. Fixing "any peer can assert
  anything" is on the roadmap; detecting a cheater is not.

Known weaknesses are tracked privately while they are open, and described publicly once they are
fixed — in the release notes for the build that fixes them. If you believe a specific weakness is
dangerous enough that players should be warned before a fix exists, say so in your report and we
will publish a warning.

## Supported versions

Only the **latest release** gets fixes. Multivoid is versioned as a game-target + build-number pair
(for example `0.9.0n b134`), and peers must run the exact same pair to play together, so there is no
long-lived older branch to patch. Update before reporting, and include your build number.
