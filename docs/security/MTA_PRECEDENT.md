# MTA precedent — what 15 years of adversarial multiplayer actually shipped

Researched 2026-07-20 against the vendored `reference/mtasa-blue/`, under the standing rule
(2026-05-28) to default to MTA's architectural shape. All citations are `[A]` — read by a research
agent, **not personally re-verified**. Re-read a site before porting it (Rule S5).

**The encouraging headline:** MTA's real anti-cheat (memory scanning, module verification) lives
entirely in the closed-source client `net.dll` — a grep of `Server/mods/deathmatch/logic/` for
`cheat|IsCheat|VERIFY_|detect` returns **zero hits**. Everything below is plain server-side
validation. MTA survived a genuinely hostile player base on mechanically simple, open, portable
checks. None of it requires an anti-cheat apparatus we said we would not build.

---

## 1. The one structural fact

> **MTA's server assigns authority; clients never claim it.** There is no claim table a client
> writes into. The server picks a syncer per element, *pushes* that assignment to the chosen client,
> and checks every inbound sync against its own record.

`CUnoccupiedVehicleSync.cpp:194-209` — `StartSync()` sends the packet, then
`pVehicle->SetSyncer(pPlayer)`. Selection is proximity/least-loaded (`:228-261`).

**Our `device_occupancy` is the same data structure.** MTA's is only (a) server-owned and (b) read on
the receive path. That is the whole of **A4**.

---

## 2. Authority checked on receive — four explicit sites

| File:line | Check |
|---|---|
| `CUnoccupiedVehicleSync.cpp:285` | `pVehicle->GetSyncer() == pPlayer && pVehicle->CanUpdateSync(...)` |
| `CPedSync.cpp:243` | `pPed->GetSyncer() != pPlayer \|\| !pPed->CanUpdateSync(...)` → `continue` |
| `CObjectSync.cpp:214` | `pObject->GetSyncer() == pPlayer && ...` |
| `CGame.cpp:2430` | vehicle damage: syncer **or** occupant |

`CanUpdateSync(ucTimeContext)` is a paired **generation counter** the server bumps whenever it
changes the element, killing replay of otherwise-authorized packets. Cheap; port it with the check.

**A caution, not a pattern to copy:** `CUnoccupiedVehicleSync.cpp:472` still **broadcasts** the packet
after the loop even when every entity failed the syncer check. That is the same relay-before-validate
shape as our **A3**, and it is arguably a bug in MTA too — mitigated there only because receiving
clients do nothing authoritative with it. **Do not cite MTA as precedent for our relay ordering.**

---

## 3. How a claim is transferred — the answer to our Principle-8 problem

`CUnoccupiedVehicleSync.cpp:476-510` — `Packet_UnoccupiedVehiclePushSync`. A **non**-syncer who bumped
a vehicle may request takeover, and must pass **four** server-side conditions (`:490-492`):

1. not already the syncer,
2. `GetTimeSinceLastPush() >= MIN_PUSH_ANTISPAM_RATE` (1500 ms, `CUnoccupiedVehicleSync.h:15`),
3. **server-side** proximity — `IsPointNearPoint3D` against the server's own copy of both positions,
4. same dimension.

Only then `OverrideSyncer()`.

**This is the missing piece of `PLAN_03`.** Our open question was "what happens to a claim whose holder
departed or stalled" — MTA's answer is that takeover is a **rate-limited request validated against
host state**, never a client assertion. That plus a TTL covers the whole Principle-8 row set.

Related: `CGame.cpp:3018-3137` (`Packet_Vehicle_InOut`) is the full request/grant template — identity,
spawn state, busy state, server-computed distance, seat validity — and on failure it sends an explicit
`VEHICLE_ATTEMPT_FAILED` **reply** (`:3042`) rather than dropping. **We drop silently**, which desyncs
the client's local prediction; the refusal packet lets it roll back.

---

## 4. Parse/apply separation — the structural answer to the whole W-series

`CPacketTranslator.cpp:250-256`: every packet's validation lives in one `Read()` returning bool, and a
failed read **deletes the packet before any handler sees it**. No handler can run on a partially
parsed packet.

**Our W-series exists partly because we validate inline at the apply site**, so a missed check lets a
hostile value reach engine code.

### The primitive we should have used

`Shared/sdk/net/bitstream.h:219`:

```cpp
bool CanReadNumberOfBytes(int iLength) const
{ return iLength >= 0 && iLength <= (GetNumberOfUnreadBits() + 7) / 8; }
```

Called at `:292` **before** `bufferArray.resize(uiLength)` at `:296`.

> **A length read off the wire is validated against the bytes actually remaining in the datagram
> before it is used to size an allocation.** No magic constant, no per-field tuning.

And its generalization to element counts, `CLuaArguments.cpp:543-549`:
*count x minimum-encoded-size-per-element <= remaining bits.*

**This is a direct hit on how we fixed W1/W2 and how we have not yet fixed W4/W5.** Our fixes are
correct but **site-by-site**; this is the invariant form. See `PLAN_02` §5.

Also: `CLuaArguments.h:47` caps wire-driven recursion at depth 64.

**Negative finding — do NOT copy:** malformed packets are dropped **silently**, with no counter and no
disconnect (`CPacketTranslator.cpp:254`). MTA's model is *strict at admission, permissive-but-inert
during play*. With our **W10** (global reliable inbox cap), one peer's garbage starves the others, so
we need a per-peer malformed counter feeding eviction — **which MTA does not have.**

---

## 5. Default-deny allowlist for client-authored actions — cheapest broad win for A3/A4

`CGame.cpp:2663-2710` — `Packet_LuaEvent`. A client may ask the server to run an event **only** if that
event was registered with `bAllowRemoteTrigger` (`CEvents.cpp:21-46`). Internal events register with
`false`. A client naming a non-triggerable or non-existent event gets nothing applied, and the server
fires `onPlayerTriggerInvalidEvent` **at the offender** so scripts can log/kick/ban. The error log is
itself rate-limited (`:2696-2699`) so the log cannot become the DoS.

**Port:** a per-kind `bAllowClientAuthored` flag defaulting to **false**, checked once in our dispatch
switch, collapses a large fraction of **A3** + **A4** into one enforcement point instead of ~30
handler-by-handler audits. Our `session_lanes.h:181-185` relay whitelist is the same idea applied to
*forwarding*; what is missing is the same idea applied to *applying*.

The offender callback is also the signal **W10** needs to turn a dropped packet into an evictable event.

---

## 6. Host-side shadow of client-reported values — the shape of the A5 fix

`CUnoccupiedVehicleSync.cpp:329-352`: the client's claimed health is compared against the server's
`GetLastSyncedHealth()`; only a *decrease* fires damage, with the delta computed **server-side**.
`SetLastSyncedHealth` is deliberately kept separate from `SetHealth` so script changes cannot be
laundered through the sync path.

> The server keeps its own shadow of every client-reported value and reasons about the **delta**, not
> the absolute.

Also `:295-321`: client positions within epsilon of the server's are cleared, so no-op syncs do not
propagate.

---

## 7. Flood protection — `CConnectHistory` is a drop-in

`CConnectHistory.cpp` (195 lines, no dependencies): a generic "N events per IP per sample period, then
timed ignore" sliding window. Instantiated three times with different budgets — connects
(`CGame.cpp:163`, 4/30 s), logins (`CAccountManager.cpp:24`, 6/30 s, fed on **failures only**), and
browser queries (`ASE.cpp`).

**`RemoveExpired()` at `:95-143` erases a host's map entry entirely when its history empties
(`:138-139`)** — so an attacker choosing keys cannot grow the table. **That is the fix pattern for our
W9.**

`ASE::DoPulse` (`ASE.cpp:153-243`) layers three defences worth copying:

1. `for (uint i = 0; i < 100; i++)` at `:176` — **bounded work per tick** regardless of queue depth.
2. `:183-196` — the per-IP protection is **disabled once its own table exceeds 100 entries**, so the
   mitigation cannot become the memory sink under a distributed flood. Deliberate, and subtle.
3. Response **caching** (`:247-259`) — flooding queries costs a `memcpy`, not a serialization.

`CHTTPD.cpp:231-232` adds: when already flooding, **skip parsing** an oversized Authorization header —
do not spend CPU parsing input you have already decided to reject.

**Negative finding:** there is **no per-player gameplay-packet budget** anywhere. Rate limiting is
entirely at the connection/login/query layer. `CBandwidthSettings.h` is **send-side** sync-rate
reduction by distance — despite the name it is not a defence. Do not be misled.

---

## 8. Identity is bound to the transport, never to a packet field

`CGame.cpp:1829-1842` — the serial is **not read from the join packet**; it is fetched from the network
layer keyed by the connection (`GetClientSerialAndVersion(Packet.GetSourceSocket(), ...)`), validated
to `^[A-F0-9]{32}$` (`:1855`), and duplicates are rejected (`:1867`).

`CPacketTranslator.cpp:228-249` — the source player is resolved from the **socket**, and a packet whose
type declares `RequiresSourcePlayer()` (default true; only the two join packets override) is
**destroyed before parsing** if no player maps to that socket. Every handler starts from
`GetSourcePlayer()`. **The sender is never a wire field.**

> **That is our A1 fix in one sentence:** `hostIdentity` and peer slot must come from the transport
> (GNS) identity, never from the signaling greeting or a payload.

**Important negative:** how the serial is *derived and attested* is **not in this repo** —
`GetClientSerialAndVersion` is a pure virtual (`Server/sdk/net/CNetServer.h:149`) forwarded into the
closed-source `net.dll`. **Do not model our peer authentication on MTA's** — the load-bearing part is
unpublished. Only the *shape* ports.

`CAccountManager.cpp:566-583` adds a TOFU pattern worth remembering: for privileged accounts a correct
password is **not sufficient** — the serial must be on an authorized list, and an unknown one is
recorded as *pending* with an out-of-band approval step rather than allowed or silently refused.

---

## 9. The server browser — this reframes A7

**Announcement is an unauthenticated HTTP GET.** `CMasterServerAnnouncer.h:235-267` string-substitutes
the server's own advertised values into a URL template and fires it. **No signature, no token, no
nonce, no shared secret.** (The master side is not in this repo — do not assume it has no anti-flood;
assume it is unpublished.)

What makes that survivable is on the **client**: `CServerList.cpp:490-570` — every listed entry must
independently answer a **direct UDP query** before it is usable. Escalate to TCP at 2 s (`:520`),
resend at 4 s (`:533`), drop after `GetMaxRetries()` (`:559`). Outbound queries are themselves paced
(`:87-91`) so refreshing a big list is not a packet cannon.

> **The master list is a hint, not an assertion.** A fabricated announcement costs the attacker a
> listing that never responds and is culled within seconds.

**This reframes A7.** Rather than only tightening `evict_if_full`, make **retention conditional on
liveness the master can verify**. Today `evict_if_full` treats a never-probed fake and a real lobby
identically — which is exactly what makes the /64 flood work. MTA's answer is not to authenticate the
announcement but to make it **cheap to disprove**.

---

## 10. Where MTA has no answer for us

| Our finding | Why MTA does not help |
|---|---|
| **S2** — bans on TURN-relayed connections | `CBanManager.cpp:86-102` writes the address naively (`AddBan(CPlayer*)` records only the IP). MTA is direct-UDP only and never faced relays. **Its mitigation is indirect:** because identity (serial) is transport-bound and address-independent, its bans do not *depend* on IP being meaningful. If our peer identity were a real key, S2 would degrade from "broken" to "IP-blind but identity-correct". Steal the two guards regardless: `IsValidIP()` and `!IsSpecificallyBanned()` before insert — the second is exactly what would stop us writing our own TURN IP into the banlist |
| **W10** — per-peer packet budget | Does not exist in MTA at the gameplay layer. We must invent it |
| **S1** — trusted save blob into a native deserializer | No analogue. MTA never hands a client a bulk save blob |

---

## 11. Anti-cheat, position validation, anti-dupe (measured 2026-07-20)

The user's forward question — *"what about anti-cheat, position checks, and how does MTA let server
owners build their own?"* — answered by measurement rather than recollection.

### MTA's anti-cheat is CLIENT-side only

`[V]` `CAntiCheat.cpp` / `CAntiCheatModule.cpp` exist **only** under `Client/mods/deathmatch/logic/`.
The Server tree contains no anti-cheat at all. What MTA calls its anti-cheat is a client integrity
module (memory/module verification) — an arms race deliberately kept out of the authority model.

### Server-side position validation is THREE arithmetic checks

`[V]` `CUnoccupiedVehicleSync.cpp:490-492` — the whole spatial rule set:

```cpp
pVehicle->GetSyncer() != pPlayer
  && pVehicle->GetTimeSinceLastPush() >= MIN_PUSH_ANTISPAM_RATE
  && IsPointNearPoint3D(pVehicle->GetPosition(), pPlayer->GetPosition(), iVehicleContactSyncRadius)
  && pVehicle->GetDimension() == pPlayer->GetDimension()
```

Proximity **against the server's own records**, a rate limit, and a dimension match.
`[V]` There is **no plausibility check** — nothing verifies that a reported position was physically
reachable, no collision test, no speed-hack detection. Fifteen years and thousands of concurrent peers
run on this.

**Why that matters to us:** all three are pure arithmetic over records the arbiter already holds. An
**engine-free arbiter can implement MTA's entire spatial validation**. The boundary is honest and
narrow: it can catch teleports, speed, and out-of-range interaction (distance + rate); it cannot check
geometry (walls, traversability) because the world lives in the engine — and MTA does not check
geometry either. VOTV's own interaction range is a free validator of exactly this shape.

### Anti-dupe is architecture, not a detector

Duplication is a **value** problem: two peers each decrement their own counter, or each spawn a copy.
Once the arbiter owns values and serialises intents, the second spend is simply rejected — there is
nothing to detect. See the cement-bucket `units` worked example in `COOP_SERVER_MODEL.md` §5. Our own
dupe history (zombie double-rows, ghost twins, the dupe matrix in `COOP_ENTITY_EXPRESSION_MAP.md`) is
identity-and-authority failure, not missing detection.

### How owners build their own — the resource event surface, default-deny

`[V]` The server exposes a broad Lua event surface (`onPlayerWasted`, `onVehicleEnter`,
`onElementModelChange`, `onPlayerWeaponSwitch`, `onElementAttach`, …) so resource authors write their
own rules. The load-bearing safety property is §5's `bAllowRemoteTrigger` default-deny: an event is
**not** client-triggerable unless its author opts in.

**Decision recorded (`TRACKER` F2):** build default-deny into the resource system at ROADMAP phase 6,
not afterwards. It is cheap at the start and near-impossible to retrofit once resources exist that
assume a permissive model.

---

## Port priority

| Mechanism | Closes | Portability |
|---|---|---|
| `CanReadNumberOfBytes` + count-vs-remaining | W1, W2, W4, W5 | **Direct** — ~5 lines, removes a bug class |
| Parse/apply split (`Read()` → bool) | W-series | **Direct**, but a refactor |
| `bAllowRemoteTrigger` default-deny | A3, A4 | **Direct, cheapest broad win** |
| `GetSyncer() == pPlayer` on receive | A3, A4 | **Direct** — makes our claim table authoritative |
| `CanUpdateSync` generation counter | A3 replay | **Direct**, cheap |
| Rate-limited + proximity-validated handoff | A4 Principle-8 | **Direct** — the claim-transfer answer |
| `CConnectHistory` sliding window | A6, A7, W10, W9 | **Direct** — 150 lines, drop-in |
| `ASE` bounded drain + self-capping table + caching | A7 | **Direct** |
| Liveness-probe list validation | A7 | **Adapt** — reframes the finding |
| Transport-bound identity | A1, P1 | **Shape only** — attestation is closed-source |

---

Back to: `README.md` · `TRACKER.md` · `PLAN_02` §5 · `PLAN_03` · `PLAN_04`.
