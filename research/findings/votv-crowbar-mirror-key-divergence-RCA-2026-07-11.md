# crowbar mirror key divergence (log RCA 2026-07-11)

Read-only root-cause hunt: why did a CLIENT prop mirror's actual `Key` field
(`NTwJKp5scUeEO750CNizdA`) diverge from its wire identity
(`ze6-4i3ZIJbCR4mEvzRWog`), producing the host-side crowbar dupe.

Log evidence: `host_crowbar_session.log` + `client_crowbar_session.log`
(scratchpad copies, 2026-07-11 session). Code as of HEAD `2221b5e5` + WIP.

## Verdict (one line)

The FRESH-SPAWN path of `remote_prop_spawn::OnSpawn` resolves `setKey` on the
LEAF class (`prop_crowbar_C`) via exact-owner `FindFunction` -> **miss** (setKey
is declared on `prop_C`/Aprop_C base) -> setKey never called -> Aprop_C UCS
auto-mints a NewGuid Key at `FinishSpawningActor` -> the mirror's FIELD key is
`NTwJKp5s...` while every registry/tracker binding carries the WIRE key
`ze6...`. The client-side destroy seam reads the FIELD key -> the DESTROY
crosses the wire under a key the host never had -> host's authoritative `ze6`
crowbar survives = the dupe. **Same `FindFunction`-exact-owner bug family as
the already-confirmed `prop_drop_intent::HostSpawnPlacedProp` miss** — but on
the RECEIVER mirror-spawn side, which the primary believed was correct
("resolves on the Aprop_C base"): that is true only for `g_propSetKeyFn` used
by the Gap-I-1 FUZZY-REKEY path; the fresh-spawn path deliberately re-resolves
per leaf class (Audit Fix 4, 2026-05-27) and misses every Aprop_C SUBCLASS
that doesn't redeclare `setKey`.

## 1. Reconstructed timeline

Control first — the rock (`prop_C`, the class that DECLARES setKey) works:

- client 12779/37096 `11:47:50 OnSpawn cls='prop_C' key='hFTW...'` ->
  37097 `setKey('hFTW7IgOnAc0UUAOgYGsGA') ok on 'prop_C' (FName idx=2405215)`
- client 38467 `11:49:19 grab_hook[destroy-seam]: CLIENT broadcasting DESTROY
  ... key='hFTW7IgOnAc0UUAOgYGsGA' eid=0` — field key == wire key, host
  resolves, no dupe. (Note eid=0 here too — the destroy seam never resolves a
  mirror's eid; the KEY is the sole effective routing for client->host
  destroys of mirrors. See §2b.)

Crowbar (`prop_crowbar_C`, subclass — setKey NOT redeclared):

| time | peer | log line | what happened |
|---|---|---|---|
| 11:53:31 | host 21690 | `Aprop.Init POST: HOST broadcasting SPAWN cls='prop_crowbar_C' key='ze6-...'` | host drops crowbar |
| 11:53:31 | client 41153-41158 | `OnSpawn cls='prop_crowbar_C' key='ze6-...'` -> **41154 `WARN setKey UFunction not found on class 'prop_crowbar_C' -- spawn will use auto-generated Key`** -> spawned `000001B85043C100` -> `CreateOrAdoptPropMirror eid=6229 bound ... key='ze6...'` | 1st mirror; field key = some auto guid G1, wire binding = ze6 |
| 11:53:34 | host 21715 | `HOST broadcasting DESTROY ... key='ze6...' eid=6229` | host hold-R picks it up; client despawns mirror **via eid=6229** (eid routing saves the host->client direction; field key irrelevant) |
| 11:54:00 | host 21909 | `Aprop.Init POST: HOST broadcasting SPAWN ... key='ze6-...' loc=(36.4,562.4,6273.4)` | host places it again — host's native place cycle PRESERVES ze6 |
| 11:54:00 | client 41473-41477 | `OnSpawn key='ze6...'` -> **41474 same WARN** -> spawned `000001B7A62B7080` -> `CreateOrAdoptPropMirror eid=6230 bound to actor=000001B7A62B7080 key='ze6...'` | 2nd mirror; **field Key auto-minted = `NTwJKp5scUeEO750CNizdA`** (first existence of that key, minted inside FinishSpawningActor UCS this instant), wire binding = ze6 |
| 11:54:06 | client 41547 | `grab_hook[destroy-seam]: CLIENT broadcasting DESTROY actor=000001B7A62B7080 key='NTwJKp5s...' eid=0` | client hold-R pickup destroys the world mirror; seam reads the actor's FIELD key + local-element eid (mirror -> none -> 0) |
| 11:54:06 | host 21973-21974 | `OnDestroy: key 'NTwJKp5s...' eid=0 has no local actor YET -- DEFERRING` | **host has no such key -> its `ze6` crowbar survives = the user-visible dupe** |
| 11:54:07+ | client 41549 | `[probe garbage_pickup]: holding_actor=000001B79041C100 holdingKey='NTwJKp5s...'` | the game's in-hand item inherits the (divergent) field key |
| 11:54:45 | client 42022 | `[PROP-DROP] CLIENT authored drop intent key='NTwJKp5s...'` | client places; drop-intent carries the divergent key |
| 11:54:45 | host 22236 | `[PROP-DROP] HOST spawned client-placed prop key='NTwJKp5s...'` — but its own leaf-class setKey miss (the CONFIRMED `prop_drop_intent` bug) mints yet another guid | host actor's real key = `WuyzlzSzpDNO0zIAyQASZw` |
| 11:54:45 | client 42025-42028 | `OnSpawn key='Wuyz...'` -> `Gap-I-1 FUZZY MATCH ... -> existing actor 000001B75EF43880 ... rekeying` -> `Gap-I-1 rekey ok -- actor ... now has wire key 'Wuyz...'` | the FUZZY path's base-resolved `g_propSetKeyFn` **succeeds on a `prop_crowbar_C` instance** — proving base-class setKey dispatch on the leaf actor is safe |

First occurrence of `NTwJKp5` anywhere in either log: client 41547 (11:54:06
destroy broadcast). No earlier client-local crowbar spawn/adopt minted it —
it was born at the 11:54:00 fresh mirror spawn and first OBSERVED when the
destroy seam read the field.

## 2. Exact code path of the divergence

`src/votv-coop/src/coop/props/remote_prop_spawn.cpp`:

- **:812** `void* setKeyFn = R::FindFunction(actorClass, P::name::PropSetKeyFn);`
  — `actorClass` is the LEAF wire class (`prop_crowbar_C`).
- `ue_wrap/reflection.cpp:419-432` `FindFunction` skips any object whose
  `OuterOf(obj) != owningClass` — **exact-owner, no SuperStruct climb**
  (`[[lesson-findfunction-exact-owner-no-superstruct-climb]]`). `setKey` is
  Outer'd to `prop_C` (Aprop_C), so the crowbar lookup returns null.
- **:813-815** miss is only a WARN ("spawn will use auto-generated Key") —
  the spawn proceeds keyless.
- `FinishSpawningActor` (**:874-882**) runs the UCS -> Aprop_C init pass,
  which (per the :797-803 comment + `prop_synth_key.h:4`) mints
  `KismetGuidLibrary::NewGuid -> FName -> self->Key` when
  `ResetKey==true || Key==None`. Key was None -> mint = `NTwJKp5s...`.
- **:995** `RegisterPropMirror(payload.elementId, spawned, keyW, ...)` and
  **:1002** `IndexActorKey(spawned, keyW)` then bind the mirror registry +
  key index under the WIRE key `ze6...` — **binding under the wire key while
  the actor's field carries the auto-minted guid is exactly the divergence**.

Why it detonates: `src/votv-coop/src/coop/props/prop_destroy_seam.cpp`
`DestroySeamBody`:
- **:58** `destroyEid = PT::GetPropElementIdForActor(self)` — the LOCAL
  element tracker only; a MIRROR is not in it -> `kInvalidId` -> wire eid=0
  (:121). The seam never consults the mirror registry
  (`Registry::EidForActor` / `ResolveMirrorEidByActor`), so eid can't rescue
  the routing.
- **:68** `keyStr = ue_wrap::prop::GetInteractableKeyString(self)` — reads
  the actor's FIELD key (`NTwJKp5s`), not the tracker's wire binding.
- **:124-127** broadcasts DESTROY under the field key. Host: no match.

(2b side note: this asymmetry means for client->host destroys of ANY mirror,
the field key is the only working identity channel; host->client destroys
ride the mirror-registry eid and are field-key-immune, which is why eid=6229
despawned the client's first mirror fine at 11:53:34.)

## 3. Can the fuzzy-adopt/rekey path bind-without-rekey?

Yes — three ways (remote_prop_spawn.cpp), all binding the wire identity
without touching the actor's field key:

- **:639-644** local-held guard: "Rekey is skipped too" (comment :637-638) ->
  `RegisterPropMirror` only.
- **:650-658** under-active-drive guard: registers the mirror, no rekey.
- **:681-701** setKey failure fall-throughs (unresolved fn / NAME_None /
  param miss / Call fail): all log WARN and continue to :711
  `IndexActorKey(fuzzy, keyW)` + :723 `RegisterPropMirror(...)` — bind-
  without-rekey by design ("resolution binds via our key->actor index,
  independent of whether the engine-level setKey write above succeeded",
  :702-710).

But in THIS incident the fuzzy path is NOT the culprit: its rekey resolved
via `g_propSetKeyFn` (**:94-95**, resolved on `P::name::PropClass` =
`L"prop_C"` — the declaring class, sdk_profile_names.h:687) and SUCCEEDED on
the crowbar at 11:54:45 (client 42027 "rekey ok"). The divergence arose on
the FRESH-SPAWN path, which does NOT use `g_propSetKeyFn`.

## 4. Could ResetKey@0x362 / init / loadData have rewritten the key later?

No rewrite was needed and none observed. The mint happened AT spawn, inside
`FinishSpawningActor`'s UCS (the documented Aprop_C behavior: mint when
`ResetKey==true || Key==None`; here Key==None because setKey never landed).
Evidence that init does NOT re-mint a present key on this class: the HOST's
own crowbar kept `ze6` across its native pickup->place cycle (host 21690
11:53:31 -> 21909 11:54:00, same key after a full destroy+respawn through the
game's own place path), and the client rock mirror kept `hFTW...` after a
successful pre-Finish setKey. So `ResetKey` is effectively false for
`prop_crowbar_C`; the sole mint trigger was the None key.

## 5. Minimal root-fix location (no implementation here)

`remote_prop_spawn.cpp:812` — the fresh-spawn setKey resolution must survive
Aprop_C SUBCLASSES. Resolve on the leaf class FIRST (preserves Audit Fix 4's
concern: chipPile/clump/trashBits have their OWN setKey overloads), and on
miss FALL BACK to the base-class resolution (`g_propSetKeyFn`, already
resolved on `prop_C` at :94-95) when `IsDescendantOfProp(spawned)` — i.e. a
SuperStruct-climbing resolve, matching what ProcessEvent dispatch itself
does. Safety of dispatching the base UFunction on a leaf instance is already
PROVEN in this very log (Gap-I-1 rekey ok on `prop_crowbar_C`, client 42027).
Same fix family as the primary's in-flight `prop_drop_intent::
HostSpawnPlacedProp` fix; both are instances of
`[[lesson-findfunction-exact-owner-no-superstruct-climb]]`.

Secondary hardening (separate decision, not the root): `prop_destroy_seam.cpp
:58/:68` could also resolve the MIRROR registry identity
(`Registry::EidForActor`) so a client destroy of a mirror rides the wire eid
even if a field key ever diverges again — but with setKey fixed the field key
never diverges, so per RULE 1 this is optional belt-and-suspenders, not the fix.
