# Crash class: RE-HOST second world load — GC/worker walks a dangling UObject (2 instances)

## ROOT CAUSE FOUND + FIXED 2026-07-04 (same day as instance 2)

**Root:** `g_storySave` (src/votv-coop/src/ue_wrap/engine.cpp) — the LoadStorySave boot-poll
cache of the loaded USaveGame* — was PROCESS-scoped. On a re-host, LoadStorySave skipped the
disk load (instance-2 log has "(save 's_abobka' registered)" but NO "loaded save 's_abobka'"
line — the smoking gun) and re-registered the FIRST session's GC-purged save object via
`setSaveSlotObject` — planting a dangling UObject* into the GameInstance UPROPERTY.
The register-context evidence closes the loop: at the fatal, R14 = 0x127501F7E10 = the
mainGameInstance_C our boot log resolved at 11:09:26, and RDI = R14+0x1A8 = the reference slot
the GC handler was processing (the disasm shows `mov rax,[rdi]` = load the referenced object
in the same handler) — i.e. the GC mark phase walked gameInstance+0x1A8 (the save-slot-object
property, first BP property after native UGameInstance sizeof 0x1A8) into freed memory ->
garbage InternalIndex -> chunk NULL -> AV. The per-frame absorbed BP-VM AVs were the world
building itself from the freed save (loadObjects=1).

Same class, second latent bug fixed with it: `g_gameModeApplied` latch was also process-scoped
(instance-2 log: NO ApplyGameModeFromSlot line in session 2 — a sandbox-after-story re-host
would have carried the wrong GameMode).

**Fix (campaign scope, one owner):** the cache belongs to ONE load campaign (a continuous
LoadStorySave/StartFreshGame poll sequence targeting one slot). `ValidateCachedSaveForCampaign`
at the boot-phase top: full reset on (polling WORLD changed || target slot changed) — the
world identity is the campaign-end signal (every menu return creates a new menu world), so a
session that reached gameplay UNOBSERVED by the poll (boot-poll timeout, native-menu load)
still forces the reset (closes the two residual gaps the correctness audit flagged) ->
disk reload = fresh pointer AND fresh content (autosaves rewrite the slot); IsLiveByIndex
guards reuse WITHIN a campaign; ResolveSavePrefixFn IsLiveByIndex-revalidates the BP CDO;
ResetCachedSave keeps only its content-invalidation semantic (v56 rejoin re-download).
Diagnosability rider: the PE firewall's absorbed-AV log now prints the fault module+RVA
(TaskFaultFilter reused in RunDetourSEH/RunObserverSEH/RunInterceptorSEH) — the "is it our
callback or the engine's BP-VM?" question this diagnosis needed a minidump for is now one
log line.

Verification state: build+deploy 4/4 (`D43455FC4787FFE4`); user hands-on re-host = the verdict.

---

## Instance 2 — 2026-07-04 11:11, PLAIN re-host, NO wisp death (user-filed)

**User sequence:** mp_host_game.bat (scenario 'play' auto-loads s_1234 11:09:26) -> user quits to
main menu 11:09:56 (clean "host session ended") -> MULTIPLAYER -> HOST NEW story game 'abobka'
(ImGui save-name) -> LoadStorySave 11:11:02 -> in gameplay 11:11:03 -> fatal 11:11:05.
DLL `7BCE41C4B6DC9C99` (proto v96, kwisp-v2+pause-guard build). NO client ever joined; NO
killerwisp in the fresh day-1 world -> **wisp death is NOT part of the trigger. Trigger =
second story-save load in one process (re-host).** 2/2 reproductions of the class.

**Fatal (IDA-mapped, VERIFIED both dumps):** exception IP RVA `0x132D02D` in BOTH instances
(2026-07-03 base 0x7FF602F20000, 2026-07-04 base 0x7FF7F2EB0000; all 7 stack frames' RVAs
identical: 0x132D02D 0x131AE0F 0x131D8A7 0x113D0E1 0x113EEBC 0x128CD8B 0x1287121). The faulting
instruction (sub_14132CAC0+0x56D):
```
movsxd rax, edx              ; edx = InternalIndex % NumElementsPerChunk
lea    rdx, [rax+rax*2]      ; idx*3 (FUObjectItem = 24 bytes)
mov    rax, cs:qword_144D8F920   ; = GUObjectArray+0x10 = ObjObjects.Chunks   [V: our boot
mov    rcx, [rax+rcx*8]      ;   chunk = Chunks[chunkIdx]                      log resolves
mov    eax, [rcx+rdx*8+8]    ; AV: chunk==NULL -> read 0 + idx*24 + 8          GUObjectArray
                             ;   (0x7d0 -> idx 83; 0x350 -> idx 35)            rva 0x4d8f910]
```
= a task-graph worker resolving an FUObjectItem from an object's **InternalIndex that is
garbage** (chunk index beyond every allocated chunk). Classic signature of following a
**reference to a freed/corrupt UObject** during GC reachability / async work on
TaskGraphThreadNP. The 0x7d0-vs-0x350 delta is just whatever garbage the freed memory held.

**Precursor (instance 2 host log, 11:11:05 — same second as the fatal):** our PE firewall
absorbed **detour-outer AVs every frame** on three objects of the NEW world:
`ReceiveBeginPlay` then per-frame `ReceiveTick` on self=0x12801000010, plus per-frame `Tick`
(widget-style) on 0x1281B5419D0 and 0x1273BE37000. detour-outer = the AV escaped the inner
per-callback wrappers, i.e. it happened in the engine's own ProcessEvent body (BP-VM deref of a
stale UObject*) or a pump lambda — game_thread.cpp:718 wraps the g_originalPE forward too. So
BP objects in the fresh world were ALREADY ticking on corrupt state before the worker tripped;
the firewall masked the game-thread symptom, the unguarded worker thread died on the same
corruption. Instance 1 crashed earlier in the load (log ended AT LoadStorySave) — same root,
earlier trip point.

**Working hypothesis (UNPROBED — probe-don't-guess applies):** something of OURS survives the
gameplay->menu->gameplay double transition holding/planting a stale UObject reference that GC
then walks: candidates = menu-world widget injections (InjectCanvasButton ran TWICE 11:09:57 +
11:10:00 on two different menu objects), save registration/CreateNamedSave writes, world-lifetime
pointers in a subsystem that re-Installs on second StartCoopSession, ElementDeleter/reap during
the second mass-purge. Vanilla load->menu->load works (players do it constantly), so the finger
points at us. NOT kwisp v2 (instance 1 predates it, v95).

**Instance-2 artifacts:** `UE4CC-Windows-D06163EF49E939FBC2DF83A565BC5F76_0000/` (dump + context),
`host_votv-coop_2026-07-04_rehost_plain.log` (full host log; AV spam at the tail).

**Next when picked up:** WinDbg the instance-2 dump (thread 20800) to read which object/token
stream the worker was walking; on repro, arm a probe that logs every UObject* we retain across
`harness: host session ended` and diff vs the new world; check InjectCanvasButton double-inject
lifecycle + what references our injected button after menu teardown.

---

## Instance 1 — 2026-07-03 12:53, re-host after wisp-killer death (original filing)

**User sequence:** host killed in-game by wisp killer -> BOTH host and client dropped to main
menu (SP death flow) -> host pressed Host Game, picked save '1234' (coop slot 'zcoop_12196')
-> fatal crash during world load.

**Crash:** EXCEPTION_ACCESS_VIOLATION reading 0x0000000000000350 (null-base + 0x350 field),
`Crash in runnable thread TaskGraphThreadNP 0` — an async-loading / task-graph worker, NOT the
game thread. Stack all in VotV-Win64-Shipping.exe (no symbol; addrs 0x7ff60424d02d,
0x7ff60423ae0f, 0x7ff60423d8a7, 0x7ff60405d0e1, 0x7ff60405eebc, 0x7ff6041acd8b, 0x7ff6041a7121
— module-relative RVAs need the session base from the minidump; tools/maprva.py).

**Host coop log ends at:** `12:53:47 engine: LoadStorySave` (slot 'zcoop_12196') — the very
start of the second-session world load. DLL was `FCBC75823B023A22` (proto v95).

**Context that makes this interesting:** this is the SECOND session in one process — full world
teardown (death -> menu) then re-host. Known-fragile seam: any of our subsystems holding
world-lifetime pointers across the teardown, or a hook touching an object mid-async-load.
Wisp-killer death path additionally ran right before teardown (wisp_attack_sync involved?).

**Artifacts here:**
- `UE4CC-Windows-F5AE656C4BE65DC7D6DC90A59E1711F8_0000/` — 12:53 host crash (CrashContext.runtime-xml + UE4Minidump.dmp)
- `UE4CC-Windows-9EB66147407AF73408943DBF3001A28C_0000/` — 12:51 dump (earlier; possibly the client or the death itself)
- `host_votv-coop.log` (+ `.prev` = the session BEFORE, with the wisp death), `client_votv-coop.log`

**Next when picked up:** open UE4Minidump.dmp (WinDbg/`!analyze`), map the faulting RVA in IDA,
diff against our hook sites; grep host_votv-coop.log.prev for the death/teardown sequence
(wisp kill ~12:5x) + check which subsystems re-Install on second StartCoopSession.
