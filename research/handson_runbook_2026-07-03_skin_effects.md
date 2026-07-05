# Hands-on runbook 2026-07-03 — skin take-3 + join-line + TIME instant + NORMALS + EVENTS-NOW

**Deployed: DLL `D43455FC4787FFE4` on all 4 installs** (hash-verified; protocol **v96** —
both peers MUST be on this DLL to connect). Supersede chain: `1CDD6079A5241162`
(your hands-on build) → `121C31D2BEFE85B4` (+eventforce autotest) → `59D77AFC4329DB78`
(**+ the WISP MIRROR LANE** — see its section below) → `08B357DDE6F6ACE4` (+the killerwisp
probe) → `7BCE41C4B6DC9C99` (2026-07-04 night: + KILLERWISP v2 choreography/aggro + the
coop NO-PAUSE fix — sections 0a/0b below; wire unchanged v96) →
`D43455FC4787FFE4` (2026-07-04 day: + the RE-HOST CRASH root fix — section 0c below;
wire unchanged v96) → `AE547EFE0ED156E7` (2026-07-04 eve: + GHOST-LOBBY delist 0d +
CLIENT NATIVE SAVE CYCLE OFF 0e; wire unchanged v96) → `08CC1DFAA4B0DF2E`
(2026-07-04 late eve: + SAVE-VERSION stamp 0f + WIND PROBE 0g; wire unchanged v96) →
`8A771BBCD61A073A` (2026-07-04 ~15:30: + QUIT-TO-MENU CRASH+DOUBLE-MENU root fix 0h/0i
+ CHAT/ROBOTO upgrade 0j; wire unchanged v96) → `BBFDC5DB5B725545` (2026-07-04 ~16:00:
+ audit-hardening — by-index guards on the 3 remaining recycled-slot sites (npc-mirror
drain / held-prop release / daynightcycle cache) + font-latch reset; both audits 0
CRITICAL; wire unchanged v96) → **`43EE5D0F4B954293` (2026-07-04 late PM: + UI
RESOLUTION SCALE + font families JetBrains Mono[default]/Roboto/Cascadia + freetype
rasterizer 0k; audits 0 CRITICAL, 3 WARN closed same build; wire unchanged v96) →
`68DE991B1FABCB74` (2026-07-04 late PM #2: + UI size pref 1.25x default + F1 slider
+ nameplate distance 1.5x + chat-resurrect probe 0l [detector entry-keyed+tail-gated
per audit F1/F2, overflow drops logged]; wire unchanged v96) →
`0E52826C38A4F42E` (2026-07-04 ~17:20: + PUPPET-Z ANCHORED-ZERO [the 16:48
twitch/sunk root -- the spawn chain-measure race deleted, RULE 2] + host lobby
T-chat + input bar = chat column + Roboto default 0m; wire unchanged v96) →
`460E5BE600E2EEAA` (2026-07-04 ~18:45: + the 17:10 HOST-DEATH diagnosis/fixes
[SO no longer absorbed + PE depth probe + mem heartbeat] 0n-a + KEYPAD red-button
press replication 0n-b + NAMEPLATE occlusion gray 0n-c; both audits 0 CRITICAL,
finding-5 ctor hardening folded in same build; wire unchanged v96) →
`E3EEAEFF25DB7802` (2026-07-04 ~19:20: the 18:41-test fixes, superseded same hour) →
**`6EE1A44107003C9F` (2026-07-04 ~19:35: the 18:41-test fixes + perf-audit tuning --
(1) SETTLED-SCAN: stream-settle discipline extracted to coop/scan/
settled_object_scan.h + adopted by ALL eight GUObjectArray index consumers
[keypad/power/window/grime/turbine/atv/trash_pile + interactable_channel], the root
of "keypad 0-sync after host session-start world reload" (tail-scan pruned-to-0,
recycled slots below cursor); churny classes settle fast (trash_pile/grime/window
settleScans=2), zero NEVER settles (deliberate -- see the header's 18:41 note);
(2) PILE ordinal-bind GATE: only GAMEMODE-sourced BeginDeferred spawns consume the
save-identity cursor (wire-driven convert-LAND nativizations were shifting every
subsequent bind = the 18:45 wrong-type morphs) + pos+chipType probes at the grab
seams (E-PRESS client / EXEC host) + throttled gamemode-class resolve; wire
unchanged v96; perf audit 0 CRITICAL [3 of 5 WARNs taken, zero-settle + backstop-150
REJECTED with rationale], correctness audit 0 findings; autonomous smoke x2: host
keypad=14 FULL-scan post-session-start + connect-snapshot "sent 14 (of 14 indexed)"
[18:44 was 0-of-0], bind cursor+position oracles agree, mem flat ~3GB. NEW OPEN
FINDING from the smokes: a JOIN-WINDOW SPAWN-NULL BURST on the client -- the host's
PropSpawn/proxy burst landing during the client's world load gets engine
BeginDeferred=null (smoke-2: 871 engine + 92 remote_prop nulls at 19:37:58-19:38:00;
piles self-heal via the native bind, but up to ~92 keyed-prop mirrors are LOST for
the session -- ghost-sync props; June archives + 07-03 real-save client show 0 =
newly visible). Root class = [[feedback-snapshot-before-state-ready]]: wire spawns
must DEFER to world-ready, not fire-and-fail; owner remote_prop/mirror_defer; NEXT
session thread)** →
**`A9109CB3AA370629` (2026-07-04 ~20:45: the join-window SPAWN-NULL burst ROOT-FIXED
at the PUMP -- deeper than the predicted per-owner defer: IDA proved
UWorld::SpawnActor (0x142C12D20) silently returns null while
[World+0x10C]&2 = bIsRunningConstructionScript (sole writer AActor::
ExecuteConstruction 0x1428c5fe4, scope-guarded around every BP actor's SCS+UCS)
or [World+0x10D]&0x20 = bIsTearingDown; our posted-task pump drained on ANY
game-thread ProcessEvent, including dispatches NESTED inside another actor's
construction script -- during a save-load's mass construction nearly every
dispatch is such, so every spawn a task issued fire-and-failed for the whole
load tail. Fix: ue_wrap/spawn_gate reads those two bits via the engine's own
world-resolution path (cached GameInstance -> virtual UObject::GetWorld,
vtbl+0x160) and the detour DEFERS the drain while the window holds -- one owner,
every posted task now runs at top-level GT context, FIFO preserved; +
sdk_profile.h split at the 1500 hard cap (content names -> sdk_profile_names.h,
commit 6e7ec390). Perf audit 0 CRITICAL (empty-queue fast path unchanged;
worst-burst ~6 ms/s core; flag = game_thread.cpp 1065/800, standing pe_detour
extraction restated); autonomous smoke PASS: spawn-nulls 0/0 vs 871+92+puppet,
[ERROR] 0/0 vs 967, 73 keyed mirrors spawned, keypad 14/14 no regression,
gate episodes 0-1 ms (57 client / 16 host), no 5s hold-warns, RSS flat ~3GB,
s_asdasd restored byte-identical; wire unchanged v96)** →
**`F9591CF919CDF0FD` (2026-07-04 ~21:20: + event_active_sync Phase 0 -- the
join-during-event READ-ONLY probe per docs/COOP_EVENT_JOIN.md 3.5: host 1 Hz
activeEvents_senders membership diff -> `event_active: BEGIN/END class=...`
edge log + join-edge would-be-EventSnapshot log in ConnectReplayForSlot; no
wire, wire stays v96; perf audit 0 CRITICAL, correctness audit 0 findings;
smoke PASS x2: resolved offsets 0xE68/0xE70, primed n=0, join-edge "0 in
flight" for slot 1, 0 ERR/WARN both peers, spawn-nulls 0/0, one-time resolve
tick 4.2 ms then silent, RSS flat ~3GB, s_asdasd restored)** →
**`38E3C707C1862931` (2026-07-04 ~21:50: pe_detour.cpp EXTRACTED from
game_thread.cpp, 1065 -> 529+557+127, hot path inline in the private seam
header -- pure move, `77f56f9e`; move-fidelity audit 0 findings; smoke PASS
0 ERR/WARN, nulls 0/0, gate episodes 0-1 ms. SAME session, the event_active
seam then PROVEN live on this code: eventforce run 21:38 -> BEGIN obelisk_C
n=1 within 1 s, BEGIN trigger_alarm_C n=2, END elapsed=65s, eventforce
VERDICT PASS + client REPLAY intact)** →
**`829A3681BA8ACDA0` (2026-07-04 ~23:15: PIRAMID MIRROR LANE, wire v96 -> v97
-- coop/creatures/piramid_sync + PyramidGather=85 + piramid2_C on the WA
allowlist + 'piramid' verdict FLIP to no-replay + piramidSpawner_C EX-catch
(both runTrigger outputs proven PE-invisible) + WA pose-walk dead-retire
(latent mirror leak closed for all 17 WA classes) + kMaxInterceptors 24->40
(table was 23/24). Autonomous e2e PASS 23:19 via piramidforce probe: host
ex-enroll -> registry BEGIN -> ~2 km march pose-streamed -> gather relay ->
client replay OK dist=9495 attempts=1 -> wisp consumed via npc lane -> END
self-destroy dead-retired -> client mirror K2'd -> registry END elapsed=211s;
0 ERROR both peers, spawn-nulls 0/0, s_asdasd restored byte-identical)** →
**`E09121F58CE2A5C6` (2026-07-05 ~00:30: JOIN-DURING-EVENT PHASE 1, wire v97 -> v98
-- ReliableKind::EventSnapshot=86 (one 98 B msg per in-flight activeEvents entry at
the joiner's world-ready edge) + event_active kClassRowMap (24 RE-verified
class->row links; unmapped = LOUD WARN) + event_fire ACTIVE-OVERRIDE replay
(ReplayInFlightRow; the passEvents history-skip no longer marks the session
replayed-set) + piramid_sync join-edge gather re-send + probe-counter reset nit.
Autonomous MID-JOIN e2e PASS 00:06: obelisk FORCED with NO client -> client
launched 27 s later -> host `join-edge slot=1 SNAPSHOT class=obelisk_C
row=obelisk` -> client `REPLAY runEvent 'obelisk' (in-flight active-override)`
dispatched + trigger_alarm_C unmapped-skip; 0 ERROR both peers, RSS ~3 GB flat,
s_asdasd restored byte-identical; perf audit PASS all functions)** →
**`D593B401A1665F34` (2026-07-05 ~09:50, wire v98 unchanged — ТЕКУЩИЙ: EVENT-JOIN
PHASE 2a, event_cue join re-send — a mid-shower joiner now gets live
already-broadcast cosmetic cues (starRain) re-sent ToSlot at world-ready;
newer-than-last-poll cues ride the next Tick broadcast instead (no double).
Autonomous MID-SHOWER JOIN e2e PASS 09:36: starRain fired host-alone 09:36:05
(`runEvent('starRain'...) dispatched`) -> client launched -> first connected poll
broadcast (send-gate dropped it for the loading slot) -> 09:36:45 host
`connect-snapshot -- re-sent live 'starRain' (cue 0) to slot 1` -> client EXACTLY
ONE `event_cue: replayed 'starRain'`; 0 ERROR both, RSS ~2.9 GB, s_asdasd restored
byte-identical; perf+correctness audits run, dev-driver honesty fix folded in —
shipping paths identical to the e2e build FA6A7531DFAB27E6)** →
**`BD2951BA2B83E68E` (2026-07-05 ~10:10, wire v98 unchanged — ТЕКУЩИЙ: NET-STATS
overlay (user ask): coop/net/net_stats one-owner traffic counters (Session's
sent_/recv_ MOVED there, bytes counted at every GNS choke point; rates = GNS
real-time telemetry summed in the existing 1 Hz net-thread sample) + ui/
net_stats_panel top-right passive panel (rates now / session totals / pkt/s /
peers+ping / 60 s sparkline) + F1 > Network > Stats toggle, OFF by default,
persisted ui.netstats. Autonomous LAN smoke PASS 10:05 (host+client, 0 ERROR
both, no RAM breach) with the panel force-enabled on both local rigs — visual
verdict = runbook 0t)** →
**`2BD2D893CDE3CA13` (2026-07-05 ~11:30, wire v98 unchanged — ТЕКУЩИЙ: the 0s
mid-join FAILURE root-fix (user live repro: joiner saw NO pyramid). Host-side
enroll/tracking seams were connection-gated — actors spawned while the host was
ALONE never entered WaMirrors/NpcMirrors, so the join snapshots re-sent nothing.
Fixed the whole class per rule 1: EX-catch + both spawn interceptors + both
pose-tick lifecycles (drain + dead-retire) now HOSTING-gated; only sends stay
peer-gated. NOT smoke-tested (user at PC) — the user's re-test of the exact
repro is the verification; expected log lines in section 0s-FIX)** →
**`F66DAE4B7621F846` (2026-07-05 ~12:00, wire v98 — ТЕКУЩИЙ: + the audit-found
sibling of the same class: kerfur_convert's conversion poll was fully
connected-gated (a SOLO-host radial toggle never converged -> the kerfur
invisible to a later joiner + dupe-on-grab) -> host branch now hosting-gated;
AND the latent 07-03 race it exposed: the per-tick npc dead-retire raced the
5 Hz conversion poll and erased the kerfur death evidence even CONNECTED ->
kerfur-family deaths returned to the poll (one owner per death edge). Kerfur
toggle deserves a re-check in the next hands-on too)** →
**`1747556099B48E79` (2026-07-05 ~13:00, wire v98 unchanged — ТЕКУЩИЙ: [WA-TRACE]
step/state telemetry across the WHOLE WorldActor pose chain, born from the
0s re-test verdict «клиент видит пирамиду, но она замёрзла на спавне за красными
барьерами». Static analysis cleared every link (send loop, serialize, store,
take, apply, drive, engine write, restored-BP-tick X/Y) — AND exposed that BOTH
prior proofs were blind: the «first batch» client log only proves ARRIVAL (the
apply loop skipped entries SILENTLY), and the 07-04 piramid e2e was geometry-blind
(wisps re-pinned 150 m around the HOST pyramid which marches only ~50 m to arrive
→ `replay OK dist=9495 attempts=1` is equally consistent with a FROZEN client
mirror). So: per [[feedback-probe-dont-guess-rule]], 1 Hz traces at all 5 hops —
host-read (TickPoseStream), host-serialize (net thread), client-store (net thread,
+staleDrops), client-apply (per-entry OUTCOME incl. the formerly-silent skips),
client-drive (pre/cur/tgt/post + K2_SetActorLocation/Rotation RETURN values,
formerly discarded). The next 0s run pinpoints the frozen hop by log diff)** →
**`012EBD5A0D31232A` (2026-07-05 ~14:00, wire v98→v99 — ТЕКУЩИЙ: the SCALE root-fix.
The user's trace run 11:25-11:26 PROVED the pose chain alive end-to-end (host-read
moves, serialize moves, client-store staleDrops=0, apply runs, drive post==cur,
applyLoc=1; host vs client actor delta ~100 units = interp lag) — so «далеко и
маленькая» was never position: the game spawns piramid2_C at SCALE 2 via the
deferred-spawn FTransform, our EntitySpawnPayload carried loc/rot only, the mirror
spawned at scale 1 → renders half-size with half-length legs while its Z streams
from the host's scale-2 hover (10000*scaleX above ground) → floats high + reads as
«far beyond the mountain». Fix: EntitySpawnPayload +Scale3D 104→116 (v99), filled
at ALL SIX senders (WA: interceptor ReadSpawnXform @+0x20 / ex-enroll / connect-
snapshot; NPC: interceptor / ex-spawn / connect-snapshot), applied at both
receivers' spawn transforms (WA OnWorldActorSpawn; NPC SpawnFreshNpcMirror, +3
defaulted params — adopt paths bind existing actors, unit is correct there);
SanitizeWireScaleAxis at the trust boundary (scale-0 wire = invisible actor).
Enroll/materialize log lines now print scale=(...) — the re-run assert)** →
**`75BD579DC792E7F7` (2026-07-05 ~14:40, wire v99 — ТЕКУЩИЙ: the FACING fix.
Scale verdict live-PASSED (user: «пирамида идёт и у клиента»), remaining defect =
facing. Root per RE + the WA-TRACE yaw=0.0 evidence: the actor root NEVER yaws —
the visible heading is the movementVector/Arrow ArrowComponents' world rotation
(host Turning tick step RInterpTo's them; the AnimBP orients the body via its
piramid2 back-ref), and the mirror's suppressed brain never turns them. Fix in
piramid_sync (no wire change): client derives heading from streamed-position XY
deltas — velocity direction == heading by construction of the native march —
eases it at the native full-walk turn rate (1.0), holds it while standing (the
native multiplyWalk>0 gate), and writes BOTH ArrowComponents' world rotation
every tick. Aux-latched: an offset miss disables only the heading drive)** →
**`B079A094E0D6B865` (2026-07-05 ~15:20, wire v99→v100 — ТЕКУЩИЙ: heading
derivation LIVE-REFUTED by the user («сбивается иногда, смотрит немного не
туда») — the native heading keeps RInterpTo-easing toward the walk target for
up to 10 s AFTER motion stops (the mov-timeline ramp-down), which no position
delta can observe; plus the delta signal itself rides the already-interp'd
position (double smoothing). RULE-1 fix: stream the TRUE host heading —
WorldActorPoseSnapshot +auxYaw 28→32 (v100), host TickPoseStream fills it per
class (piramid2_C: movementVector world yaw via piramid_sync::ReadHostHeadingYaw;
every other class: == actor yaw), the WorldActor element interps auxYaw through
the same LerpWindow as the other angles, and the client drive hands the interp'd
value to piramid_sync::ApplyMirrorHeadingYaw (both ArrowComponents) right after
each pose apply. The delta-derivation (DriveMirrorHeadings + MirrorHeading map)
is GONE per RULE 2. WA-TRACE host-read/client-apply lines now print aux=)**.
Late-eve autonomy
("Go next"): baseline smoke PASS; events feature verified e2e (`eventforce_test: VERDICT
PASS` — obelisk armed=0 shots=1 → NOW! → shots=0 [FIRED], client `REPLAY runEvent
'obelisk'` same second); wisp lane e2e x2 (32/32 all four legs); killerwisp probe (chain
alive; the gap = missing peer kill choreography → CLOSED by v2). What autonomy CANNOT see:
everything visual — your hands-on below still decides those.

## 2026-07-05 ~15:20 (DLL `B079A094E0D6B865` — ТЕКУЩИЙ, wire v100)

### 0s-FACING2. HEADING ТЕПЕРЬ СТРИМИТСЯ С ХОСТА (истина вместо деривации)
Твой вердикт 0s-FACING: работает, но «сбивается иногда» и лёгкий рассинхрон направления.
Причина принципиальная: натив доворачивает heading до 10 секунд ПОСЛЕ остановки (плавное
гашение mov-таймлайна + RInterpTo к цели) — из дельт позиции это невидимо, плюс дельты сами
идут по уже сглаженной интерп-позиции (двойное сглаживание = лаг на поворотах). FIX v100:
heading хоста (world yaw компонента movementVector) теперь едет В САМОМ pose-потоке
(auxYaw, +4 байта на запись), интерполируется у клиента тем же окном, что и позиция, и
пишется в оба ArrowComponent'а после каждого pose-apply. Деривация удалена полностью.

ПРОГОН (те же 2 минуты): piramid → NOW!, 60-90 с, посмотри именно РАЗВОРОТЫ:
- смена направления марша: клиент доворачивает СИНХРОННО с хостом (лаг = интерп ~75 мс);
- остановка у виспа (gather): пирамида ДОВОРАЧИВАЕТСЯ на виспа стоя — теперь и у клиента;
- лог-ассерт: хост `[WA-TRACE host-read] ... aux=<меняется>` при повороте, клиент
  `[WA-TRACE client-apply] ... aux=<та же величина>`.
Твой вердикт 0s-SCALE: пирамида у клиента ИДЁТ (scale-фикс подтверждён), но смотрит не в
сторону движения. Корень (RE + трасса yaw=0.0): root пирамиды никогда не вращается — видимый
разворот тела = world-rotation компонентов movementVector/Arrow, которые у клиента никто не
вертел (мозг подавлен). FIX: клиент выводит heading из дельт стримленной позиции (направление
движения == heading по построению нативного марша) и каждый тик пишет его в оба компонента —
ровно то состояние, что держит хостовый Turning-степ; стоя — держит последний heading (как
натив). Провод не менялся (v99 тот же).

ПРОГОН (те же 2 минуты): хост+клиент → F1 → EVENTS → piramid → NOW! → 60-90 с.
Ожидание ВИЗУАЛ: пирамида у клиента смотрит ТУДА, КУДА ИДЁТ (в пределах пары секунд
доворота после смены направления — нативная скорость поворота медленная, у хоста так же);
при остановке (gather) — держит направление. Размер/позиция — как в 0s-SCALE.

## 2026-07-05 ~14:00 (DLL `012EBD5A0D31232A` — superseded by `75BD579DC792E7F7`; SCALE подтверждён твоим прогоном — «пирамида идёт и у клиента»; facing — раздел 0s-FACING выше)

### 0s-SCALE. ПИРАМИДА «ДАЛЕКО И МАЛЕНЬКАЯ» — корень найден трассой, SCALE зашит в провод
Твоя трасса 11:25 доказала: позиции ЖИВЫ (хост и клиент расходятся на ~100 юнитов —
чистый интерп-лаг), applyLoc=1, staleDrops=0. Расхождение было НЕ позицией — размером:
игра спавнит piramid2_C в scale 2 через spawn-transform, наш spawn-payload scale не нёс
→ mirror в scale 1: вдвое меньше, ноги вдвое короче, а Z с провода — хостовый (hover
scale-2, 200 м над землёй) → пирамида ВИСИТ высоко, мозг читает «далеко за горой».
«Топчется» на скринах — момент gather: пирамида стоит по дизайну у обоих.
FIX v99: Scale3D в EntitySpawnPayload — все 6 отправителей + оба приёмника.

ПРОГОН (те же 2 минуты): хост+клиент → F1 → EVENTS → piramid → NOW! → 60-90 с.
Ожидание ВИЗУАЛ: пирамида у клиента ТОГО ЖЕ размера, на ТОМ ЖЕ месте, ноги достают
до земли; идёт по карте синхронно с хостом. Лог-ассерты: хост
`world-actor[host ex-enroll]: ... scale=(2.00,2.00,2.00)`; клиент
`world-actor[client OnSpawn]: materialized mirror ... scale=(2.00,2.00,2.00)`;
[WA-TRACE] строки продолжают идти (телеметрия постоянная). Если scale в логе 1.00 на
ХОСТЕ — спавнер даёт scale не транформом, скажи мне (буду читать актора после Finish).

## 2026-07-05 ~13:00 (DLL `1747556099B48E79` — superseded by `012EBD5A0D31232A`; трасса ОТРАБОТАЛА прогоном 11:25: цепочка жива, корень = SCALE, раздел 0s-SCALE выше; [WA-TRACE] телеметрия остаётся в DLL)

### 0s-TRACE. ПИРАМИДА ЗАМЁРЗЛА У КЛИЕНТА — трассировка каждого шага (твой запрос «lets log each step, each state»)
Твой вердикт 0s-FIX: пирамида у клиента ПОЯВИЛАСЬ (fix трекинга сработал), но
замёрзла на спавне за красными барьерами и не соответствует хосту. Статический
разбор оправдал каждое звено цепи — значит, меряем. В DLL добавлены 1 Гц
`[WA-TRACE]` строки на всех 5 хопах WA-pose потока (постоянная телеметрия: активна
только пока живёт ивент-актёр).

ПРОГОН (2 минуты, тот же сценарий — mid-join НЕ обязателен, можно джойн до ивента):
1. Хост + клиент в сессии → F1 → EVENTS → piramid → NOW!
2. Дай пирамиде пошагать 60-90 секунд на хосте. Смотреть на экраны не обязательно —
   решают логи. Выйди из сессии, скажи мне «готово».

ЧТО СКАЖУТ ЛОГИ (каждая строка 1 Гц, eid пирамиды один и тот же):
- ХОСТ `[WA-TRACE host-read] eid=N loc=(...)` — координаты, снятые с живого актёра.
  ЗАМЁРЗЛИ на споне → сломано чтение хоста (актёр ходит не root'ом) — дальше можно не смотреть.
- ХОСТ `[WA-TRACE host-serialize] n=1 first: eid=N (...)` — что реально уходит в сеть.
  host-read едет, serialize замёрз → сломан game→net хэндофф.
- КЛИЕНТ `[WA-TRACE client-store] n=1 seq=... (...) staleDrops=K` — что пришло по проводу.
  Нет строк / staleDrops растёт лавиной → провод/seq-гард.
- КЛИЕНТ `[WA-TRACE client-apply] eid=N wire=(...) -> SetTargetPose` — или ГРОМКИЙ SKIP
  с причиной (no-element / element-not-mirror / out-of-range) — раньше эти скипы были немыми.
- КЛИЕНТ `[WA-TRACE client-drive] eid=N pre=(...) cur=(...) tgt=(...) post=(...)
  window=W applyLoc=L applyRot=R`:
  - `applyLoc=0` → K2_SetActorLocation ОТКАЗЫВАЕТ (раньше возврат выбрасывался) — корень найден;
  - `tgt` едет, `post` едет, а `pre` каждый раз отскакивает к спавну → BP-тик пирамиды
    перезаписывает наш драйв (проигранная борьба за кадр);
  - `GUARD-FAIL ... liveByIdx=0` → mirror-элемент потерял актора;
  - `tgt` сам замёрз → смотри хопы выше.
Плюс known-gap, найденный этим же разбором: спавн-транформ клиентского mirror'а не несёт
SCALE (пирамида на хосте scale 2, у клиента 1 — вдвое меньше). Чиню после разморозки.

## 2026-07-05 ~11:30 (DLL `2BD2D893CDE3CA13` — superseded by `1747556099B48E79`; 0s-FIX отработан: спавн-доставка ПОДТВЕРЖДЕНА твоим прогоном 10:48-10:51, замёрзший pose — раздел 0s-TRACE выше)

### 0s-FIX. ПИРАМИДА MID-JOIN — root-fix твоего репорта («у клиента ничего нету»)
ROOT CAUSE: трекинг ивент-актёров на хосте отсекался на `connected()` — пирамида
(и её killerwisp'ы), заспавнившиеся ПОКА ХОСТ ОДИН, никогда не попадали в
WaMirrors/NpcMirrors → join-снапшоту было нечего слать. FIX (per rule 1, весь класс):
трекинг/lifecycle теперь гейтится на «hosting» (сессия есть + роль Host), пиры не
нужны; только SEND остался peer-зависимым; dead-retire тоже работает в одиночестве.
ПОВТОРИ свой сценарий: хост один → пирамида пошла → клиент подключается.
Ожидаемые строки: хост при спавне (ещё один): `world-actor[host ex-enroll]:
'piramid2_C' eid=N` + 4x `npc-sync[ex-spawn]: enrolled 'killerwisp_C'`; хост при
джойне: `world-actor: connect-snapshot -- sent N existing WA(s) to slot 1` (N>=1);
клиент: `world-actor[client OnSpawn]: materialized mirror ... piramid2_C` + ВИЗУАЛ:
пирамида идёт, экран трясётся от шагов, сифон при gather.

### 0t. NET-СТАТИСТИКА (новая фича по твоему запросу) — визуальный вердикт
Оверлей сетевой статистики (host + клиенты, ВЫКЛ по умолчанию; на твоих локальных
копиях я оставил ВКЛ через ui.netstats=1, чтобы ты сразу увидел). F1 > Network > Stats —
галка + живая сводка. Панель справа-сверху: точка-статус, пиры + пинг, приём/отдача
СЕЙЧАС (стрелки: голубая вниз = приём, оранжевая вверх = отдача), тоталы за сессию
справа, граф 60 с (голубая заливка = rx, оранжевая линия = tx), pkt/s снизу.
1. Проверь визуал: читаемость, не мешает ли, «slick или нет» — любые пожелания.
2. Тотал «скачано» на клиенте должен заметно прыгнуть при джойне (сейв-трансфер + снапшот).
3. При отключении сессии панель должна показать offline + нули в rate, тоталы остаться.
4. Скрой панель галкой в F1 — она должна исчезнуть мгновенно и остаться выключенной
   после перезапуска (votv-coop.ini ui.netstats).

## 2026-07-05 ~00:30 (DLL `E09121F58CE2A5C6` — superseded by `D593B401A1665F34`, wire v98; разделы ниже действительны)

### 0s. ВХОД ВО ВРЕМЯ СОБЫТИЯ (join-during-event) — приёмочный тест Phase 1
Автономика уже доказала провод (см. цепочку DLL выше: обелиск, форс ДО подключения
клиента → снапшот → override-replay). Твой проход решает ВИЗУАЛ + пирамиду:
1. Хост: запусти мир, F1 → EVENTS → piramid → NOW! (клиент ещё НЕ подключён).
2. Когда пирамида зашагала — клиент подключается (обычный вход в сессию).
3. Ожидание: клиент, загрузившись, видит ТУ ЖЕ пирамиду в ТОМ ЖЕ месте пути (WA-снапшот),
   виспы на местах (npc-снапшот). Если вход попал НА gather (~10 с лучей) — лучи/монтаж
   должны идти и у клиента (лог: `piramid-gather[host]: join-edge slot=1 re-sent
   in-flight gather` + клиентский `piramid-gather[client]: replay OK`).
4. То же с обелиском (replay-строка): хост F1 → EVENTS → obelisk → NOW!, клиент входит
   после — у клиента обелиск должен ПОЯВИТЬСЯ при подходе (его коробка армится реплеем;
   лог: `event_fire: client REPLAY runEvent 'obelisk' (in-flight active-override)`).
5. Смотри WARN-строки `NO class->row map entry` в клиентском логе — это НЕ ошибки, это
   список классов для Phase 2 карты; назови их мне, если увидишь.

## 2026-07-04 ~23:15 (DLL `829A3681BA8ACDA0` — superseded by `E09121F58CE2A5C6`)

### 0r. ПИРАМИДА — визуальный проход (devs'-gauntlet acceptance; автономика уже доказала поток)
Автономный e2e УЖЕ прошёл (см. цепочку DLL выше): mirror + подавление мозга + gather-replay
+ смерть-ретайр — всё по логам. Твой проход решает ВИЗУАЛ (то, что автономика не видит):
1. Хост: F1 → EVENTS → piramid → NOW! (или дождись день 30/31 по сюжету). Пирамида
   спавнится у спавнера (район Signal Lab) и шагает к центру карты.
2. Оба экрана: ОДНА и та же пирамида, тот же путь, ноги шагают процедурно у клиента,
   звуки шагов/пинг на месте. Клиентский лог: `world-actor[client OnSpawn]: materialized
   mirror ... piramid2_C` + `piramid-brain[client]: ... tick restored`.
3. Gather: когда пирамида доходит до киллервиспа — у ОБОИХ montage + лучи + всасывание
   виспа. Клиентский лог: `piramid-gather[client]: replay OK`.
4. Конец: пирамида уходит (SW угол) и исчезает у обоих; хостовый лог
   `world-actor[host dead-retire]` + `event_active: END class=piramid2_C`.
5. Отдельно (когда-нибудь): КЛИЕНТ стоит в Signal Lab на заармленной хостом коробке —
   проверить, что оверлап хоста принимает марионетку (пункт 3.1 в docs/events/piramid.md).

## 2026-07-04 ~21:50 (DLL `38E3C707C1862931` — superseded by `829A3681BA8ACDA0`)

### 0q. event_active Phase 0 — SEAM УЖЕ ДОКАЗАН автономно; твой прогон опционален
Хостовый read-only зонд родного реестра активных событий (activeEvents_senders).
**Автономный eventforce-прогон 21:38 уже доказал seam на живом событии**: forced
obelisk → `BEGIN class=obelisk_C n=1` через 1 с; цепочка тревоги → `BEGIN
class=trigger_alarm_C n=2` (refcount) + `END ... elapsed=65s`; eventforce VERDICT
PASS + клиентский REPLAY нетронут. Опциональная ручная проверка (если хочешь
увидеть сам): F1 → EVENTS → NOW! → хостовый лог BEGIN/END; джойн клиента во время
события → `join-edge slot=N WOULD snapshot class=...`. Ничего визуального у
клиента НЕ изменится — это Phase 0 (лог, провода нет); картинка хост-vs-клиент не
критерий. NEXT-код: пирамидный mirror-lane (docs/events/piramid.md, DESIGN) +
Phase 1 EventSnapshot.

## 2026-07-04 ~21:20 (DLL `F9591CF919CDF0FD` — superseded by `38E3C707C1862931`; 0q см. выше)

## 2026-07-04 ~20:45 (DLL `A9109CB3AA370629` — superseded by `F9591CF919CDF0FD`; сценарий 0p актуален)

### 0p. Join-window SPAWN-NULL burst — ROOT-FIXED (pump drain-defer)
Корень (IDA): наш pump исполнял отложенные задачи на ЛЮБОМ ProcessEvent — в т.ч.
вложенном в чужой ConstructionScript, где UWorld::SpawnActor молча возвращает null
(Shipping без LogSpawn). На джойне (массовая конструкция акторов сейва ~5 с)
это убивало ВСЕ наши спавны окна: 871 прокси + 92 keyed-зеркала (не ретраились =
пропс-призраки на всю сессию) + puppet. Фикс: детур откладывает дренаж, пока мир
отказывает в спавнах (`ue_wrap/spawn_gate`). Смок: нулей 0/0, ERROR 0/0 (было 967),
73 зеркала заспавнились. Твой тест — обычный джойн клиентом: (1) в клиентском логе
НЕ должно быть `BeginDeferred returned null`-пачек; (2) предметы, которые хост
двигал/держал ДО твоего захода, должны корректно жить у клиента (раньше до ~92
пропсов могли молча не зеркалиться до конца сессии). Отдельного сценария не нужно —
0o-a/0o-b на этом же DLL закрывают и это.

## 2026-07-04 ~19:35 (DLL `6EE1A44107003C9F` — superseded by `A9109CB3AA370629`; сценарии 0o-a/0o-b актуальны)

### 0o-a. Твой 18:41 «keypad вообще без синка» — ROOT-FIXED (settled-scan)
Корень: на старте сессии хост ПЕРЕЗАГРУЖАЕТ мир; периодический tail-scan вычистил
умершие кейпады старого мира (14 -> 0), а новые легли в переиспользованные слоты
GUObjectArray НИЖЕ курсора — tail их не видел до конца сессии (то же убило
power/window/grime/atv: у ВСЕХ ноль в 18:41:10, при живых door/light/container).
Фикс: settle-дисциплина (full-walk пока счётчик меняется) теперь у ВСЕХ восьми
индексов. Автономный смок уже показал: после session-start keypad=14 «full scan»
+ connect-snapshot «sent 14 (of 14 indexed)». Твой тест: тот же сценарий, что в
18:41 (загрузиться в меню/соло -> захостить -> клиент жмёт кейпад) — LED/буфер
должны зеркалиться в обе стороны. Если нет — хвост обоих логов с `keypad:`.

### 0o-b. Твой 18:45 «pile морфится не в тот тип» — ГЕЙТ + ПРОБЫ
Найден статический корень: клиентский ordinal-бинд (k-й keyless спавн <-> k-я
запись карты) съедался НАШИМИ ЖЕ спавнами (конверт-ленды/нативизации проходили
тот же BeginDeferred-шов) — один лишний спавн сдвигает ВСЕ последующие бинды =
массовая путаница идентичности = чужой тип у грабнутого пайла. Фикс: курсор
потребляют только спавны ИЗ ФРЕЙМА ГЕЙМОДА (loadObjects/loadPrimitives). Плюс
пробы: клиент логирует `E-PRESS ... at(x,y,z) chipType=N`, хост — `EXEC ...
at(x,y,z) chipType=N` для того же eid. Твой тест: пограби 3-5 РАЗНОТИПНЫХ пайлов
клиентом — тип клампа/лендинга должен совпадать с тем, что лежал. Если снова не
тот тип — пришли обе строки (E-PRESS + EXEC) для одного eid: несовпадение
позиций назовёт остаточный корень точно.

## 2026-07-04 ~18:45 (DLL `460E5BE600E2EEAA` deployed 4/4 hash-verified)

### 0n-a. Твой 17:10 «хост жрал память и его убило» — диагноз + фиксы/пробы
Что нашлось в логе+WER: в 17:09:46 хост поймал **stack overflow (0xC00000FD) ВНУТРИ
скрипт-VM** на `ReceiveDestroyed` — вложенная BP destroy-цепочка (актор в своём Destroyed
уничтожает следующий, тот следующий, ...) съела стек; наш SEH-щит «поглотил» это и
продолжил — через секунду (17:09:47, WER) процесс добило вторым AV на уже мёртвом стеке.
Личность цепочки лог НЕ назвал (наши destroy-пути все логируются — тишина = инициатор не
наш логируемый код). Память в логе не видна вообще (0 наблюдаемости). Фиксы per rule 1 +
probe-don't-guess:
1. **Stack overflow больше НЕ поглощается** — процесс падает в WER ровно в апексе рекурсии,
   и дамп (Панель управления WER / ProgramData\Microsoft\Windows\WER) назовёт всю цепочку.
2. **PE depth-probe**: при глубине вложенных диспатчей 128/256/512/... в лог пишется
   `game_thread: PE recursion depth=N -- function=... class=...` — повторяющаяся пара
   function/class в этих строках = участники цикла. Цена: инкремент thread-local на диспатч.
3. **Память в лог**: раз в ~30 с `mem: ws=XMB private=YMB peak-ws=ZMB` — в следующий раз
   увидим, росла ли она минутами (утечка) или это был всплеск самой рекурсии.
ТЕСТ: играй как обычно. Если хост снова умрёт — мне нужны от тебя: время + хвост
votv-coop.log (там будут `PE recursion depth` строки с именами) + `mem:` строки до смерти.
Этого хватит на именной root-fix.

### 0n-b. KEYPAD: красная кнопка клиента (17:07) — root-fixed
Твой репорт: клиент жмёт красную кнопку — состояние всегда перезаписывается на зелёный.
Root: наша же host-authority защита 2026-06-17 была слишком широкой — хост не принимал от
клиента НИКАКОЙ `active`, а красная кнопка с пустым буфером даже не классифицировалась как
событие (гейт требовал напечатанных цифр). Лог 17:08:42: `applied active=0 (from slot 1)`
→ тут же `sent active=1` — бой на перезапись. Фикс (MTA input-replication, как цифры):
нажатие красной/зелёной кнопки теперь распознаётся по нативным hover-флагам (isDeny/isAcc —
прицел на кнопке в момент флипа) и уезжает на хост СОБЫТИЕМ; хост реплеит его нативным
open(false/true) — свет/звук/замок двери от его собственной цепочки — и рассылает результат.
Транзиенты (save-transfer active=0, ev=None) как раньше НЕ трогают питание хоста — корень
06-17 закрыт где был. ТЕСТ: клиент у кейпада жмёт красную (буфер пуст) → LED красный у
ОБОИХ, дверь заперта, зелёный больше не возвращается; хост-лог:
`keypad: native Open(0) replayed ... (from slot 1)`. Обратно: правильный код/зелёная —
как раньше. Неправильный короткий код на зелёном кейпаде теперь тоже перезапирает дверь у
всех (это нативное SP-поведение).

### 0n-c. NAMEPLATE occlusion — серый ник за препятствием (minecraft-style)
Ник видно сквозь стены как раньше (оверлей), но если между камерой и головой пира есть
геометрия/закрытая дверь/проп — ник СЕРЫЙ и вся плашка чуть притушена; вышел из-за угла —
снова белый. Тела игроков сами не блокируют (трейс по WorldStatic+WorldDynamic, pawn — нет).
Hurt-flash (красный) имеет приоритет над серым. ТЕСТ: смотри на пира через стену/дверь →
серый; в прямой видимости → белый; ранение за стеной → красный.

## 2026-07-04 EVE (your live-test reports; DLL `AE547EFE0ED156E7` deployed 4/4 hash-verified)

### 0d. GHOST LOBBY after host death — root-fixed (`c8aec14c` + race close `f5a02741`)
Your report: host died to the killerwisp, all fled to the menu — the server STAYED listed.
Root: the lobby heartbeat thread was only stopped on boot-failure paths; the host-session
running->stopped edge (death OR quit-to-menu) never sent /v1/leave, so the master kept the
dead lobby alive forever. Fix (one owner): EndHostedLobby (delist + heartbeat stop + host
state clear) now fires at the host-session-ended edge, same place the menu-flee is posted.
Audited (1 agent): 1 race found (fast re-host mid-delist wiped the new host request) —
closed in `f5a02741`. TEST: host, умри/выйди в меню → на втором пире обнови браузер
серверов — строка должна ИСЧЕЗНУТЬ (host log:
`session_manager: EndHostedLobby -- lobby retired`).

### 0e. CLIENT NATIVE SAVE CYCLE OFF (`99eb4566`) — your mandate «выключить полностью»
Until now the client's native save cycle still RAN every autosave interval (full world
gather) and only the disk write was blocked. Now the cycle is off at the game's own gate:
we hold gamemode.disableSave=true on the client — saveSlot_C::save checks it at its HEAD
[verified in bytecode] before the gather and the write funnel; every trigger (autosave/
sleep/menu/quicksave) funnels through there; nothing in the game's bytecode ever writes
the flag back. The SaveGameToSlot disk hook stays as the belt for the two ungated paths
(savePlayerOnly + direct trigger writes). TEST (client): play past an autosave interval —
NO «SAVED» toast on the client, client log has ONE
`save_block: client native save cycle OFF -- disableSave=true` line per world, and NO new
`save_block: BLOCKED client world-save` lines during normal play (a BLOCKED line now =
a gate-leak report, tell me). Host saves normally — host log unchanged.

### Your 4 other findings (recorded OPEN, next threads)
1. Wind + dust desync — probe SHIPPED, see 0g below. 2. Luggage->inventory->drop-all:
   dropped items never appear on the client (inventory-drop spawn path uncaught). 3. Kwisp
   client death too FAST vs native pacing. 4. Kwisp beam-"РУКИ" not mirrored on watching
   peers (kill syncs, beam VFX doesn't). Partial 0b verdict recorded: kill chain confirmed
   live BOTH directions.

## 2026-07-04 LATE EVE (DLL `08CC1DFAA4B0DF2E` deployed 4/4 hash-verified)

### 0f. MP save shows «unk!» version — root-fixed (`c81f1c2d`), 1-minute test
Your report: сейв из Multiplayer -> New Game & Host (s_aboba2) показывает красное «unk!»
вместо «0.9.0». Root: наш create писал на диск ГОЛЫЙ CDO-дефолтный saveSlot_C; нативный
create виджета штампует tempSave.version = lib_C::gameVersion() [verified: ui_saveSlots
button_create bytecode] — пустая Version и красит «unk!» и фейлит version-check при
launch. Fix: зовём тот же gameVersion и переносим движковый FString в поле Version перед
записью. Audited (1 agent): все PASS, 0 CRITICAL. TEST: Multiplayer -> New Game & Host
с новым именем → в списке сейвов у нового слота **зелёная «0.9.0»**, не «unk!»
(лог: `save_browser: CreateNamedSave -- stamped Version '0.9.0'`). Старые unk!-сейвы
не перештамповываются (создание-время фикс); скажи если надо мигрировать существующие.

### 0g. WIND/DUST desync — PROBE armed (`6398ff53`), capture on next sighting
Статически v50-синк ветра верен end-to-end (пере-верифицировано из свежего байткода:
вся видимая часть — пылинки eff_wind, звук, качание листвы — производная intensity;
intensity ← spring ← windTarget, который мы стримим каждые ~2.5 с; клиентский RNG-реролл
подавлен). Живое расхождение механика не объясняет → на обоих пирах взведена проба:
`weather_probe=1` УЖЕ прописан в ini всех 3 инсталлов; в логах ~1 Гц строки
`[probe wind] role=... target=(...) |target|=... roll fired=N suppressed=M`.
КОГДА снова увидишь «у одного ветер/пыль, у другого нет» — просто скажи мне (примерно
во сколько); я сравню HOST vs CLIENT строки. Чтение: |target| разошёлся = не долетает
запись синка; fired=suppressed=0 на клиенте = диспатч реролла обходит ProcessEvent;
всё равно = расходится что-то вне ветра (например листья autumnLeafSpawner — известный
per-peer RNG, не покрыт сознательно).

## 2026-07-04 ~15:30 (DLL `8A771BBCD61A073A` deployed 4/4 hash-verified)

### 0h. CLIENT CRASH после ESC->menu — root-fixed (твой 14:45 репорт)
Твой краш (`Failed to find function setRainProperties in ArrowComponent ...MirrorAxis`):
выход в меню через РОДНОЕ меню игры при живой сессии шёл через ветку balloon-guard,
которая НЕ делала teardown подсистем (а flee вдобавок гасит и дисконнект-эдж, который
сделал бы его сам) — кэши погоды/времени/неба переживали смерть мира, и отложенное
применение погоды попадало в ПЕРЕИСПОЛЬЗОВАННЫЙ слот старого daynightCycle → BP-тело
исполнялось на чужом self → движковый фатал по имени setRainProperties. Fix: этот путь
теперь делает ПОЛНЫЙ teardown (puppet destroy + per-slot + DisconnectAll — общий хелпер
с death-путём) ДО flee. TEST: клиент в сессии → ESC → Menu → чистый выход в меню, без
краша; в логе `net: gameplay->MENU ... ending the session` + НЕТ фатала.

### 0i. Двойной выход в меню у хоста — root-fixed (тот же бранч)
Наш flee диспатчил transition("/Game/menu") ПОВЕРХ уже летящего родного перехода —
меню грузилось дважды. Теперь travel=false на этом пути (сессия останавливается,
bypass взводится, второго transition НЕТ). TEST: хост в сессии → ESC → Menu → меню
открывается ОДИН раз (лог: `native menu travel already in flight; no second transition`).

### 0j. ЧАТ: Roboto везде + кириллица + анимация + фишки (твои запросы по chat-imgui-samp)
Roboto Regular 16px = шрифт ВСЕГО оверлея (браузер/скорборд/меню/консоль), Bold 18px =
чат; шрифты ВШИТЫ В DLL (никаких файлов рядом; .pak не нужен). Чат теперь UTF-8:
**русский в чате и в никах наконец рендерится** (раньше '?'). Плюс: fade-IN у строк
(220мс) + прежний fade-out; цветной ник по слоту игрока (8 цветов); 4-сторонний контур
вместо тени; перенос длинных строк; история отправленных Up/Down в поле чата; в консоли
фокус при открытии + история команд; Enter = сабмит в поле имени нового сейва и в
Direct connect. TEST: напиши в чат по-русски с обоих пиров; посмотри цвет ников; Up в
поле чата возвращает прошлое сообщение; весь UI стал Roboto.

### 0k. UI SCALE + ШРИФТЫ (JetBrains Mono default) + FreeType (2026-07-04 late PM, DLL `43EE5D0F4B954293`)
Весь оверлей теперь МАСШТАБИРУЕТСЯ под разрешение (высота/1080): шрифты ПЕРЕПЕКАЮТСЯ в
реальный px (не растяжение = не мыло), отступы/окна/колонки/чат-перенос — всё
пропорционально. Растеризатор атласа = imgui_freetype (вшитый FreeType) — хинтинг
заметно чётче stb на 16-18px; ЭТО КАСАЕТСЯ И НИКНЕЙМОВ над головами (один атлас).
Шрифт по умолчанию = **JetBrains Mono** (твой запрос «посмотреть vs Roboto»);
переключение НА ЛЕТУ: F1 > Cosmetics > Interface (JetBrains Mono / Roboto /
Cascadia Code), сохраняется в votv-coop.ini `ui.font`. TEST: (1) на большом
разрешении открой браузер серверов / чат / F1 — размеры больше не «мелкие», текст
чёткий; (2) пере switched шрифты в F1 — весь оверлей меняется мгновенно; (3) чат
по-русски — рендер в каждом из 3 шрифтов; (4) если играешь в окне — потяни размер
окна: UI перепечётся под новую высоту (лог `imgui_overlay: UI re-scaled (factor ...)`).
Status: build+deploy 4/4 hash-verified `43EE5D0F4B954293`; 2 аудита (перф 0 CRITICAL /
3 WARN — все закрыты в этом же билде; корректность — чисто); твой взгляд на
чёткость/размер = вердикт.

### 0l. UI ПОБОЛЬШЕ + слайдер + nameplate-дистанция + чат-проба (DLL `68DE991B1FABCB74`)
(1) Весь оверлей теперь крупнее из коробки: множитель размера **1.25x по умолчанию**
поверх авто-масштаба разрешения; **слайдер F1 > Cosmetics > Interface > UI size**
(0.75x-1.75x, применяется при отпускании, сохраняется в votv-coop.ini `ui.scale`).
TEST: открой F1 — всё заметно крупнее; подвигай слайдер — весь UI (чат, меню,
никнеймы) перепекается под новый размер. (2) NAMEPLATES: дистанция читаемости
никнеймов увеличена в 1.5 раза (полный размер до ~9 м, пол на ~22.5 м вместо ~15).
TEST: отойди от пира на 10-20 м — ник дольше остаётся читаемым. (3) ЧАТ-ГЛЮК
(твоё «исчезнувшее сообщение появляется и снова исчезает»): статически стор/сеть
чистые (эхо отправителю исключено, alpha монотонна) — в ленту встроена ПОСТОЯННАЯ
проба: каждый push/expiry логируется, повторный push того же текста <60 c после
смерти = `feed: RESURRECT`, невозможный подъём alpha = `feed: ALPHA-JUMP`. TEST:
играй как обычно; когда снова увидишь глюк — скажи время, я сниму лог (строки
`feed:` вокруг момента назовут механизм). Status: build+deploy 4/4 hash-verified.

### 0m. ТВОИ 3 РЕПОРТА 16:48 -- root-fixed (DLL `0E52826C38A4F42E`)
(1) **ХОСТ TWITCHING + НА 1/4 ПОД ЗЕМЛЁЙ У КЛИЕНТА** -- корень НАЙДЕН В ТВОИХ ЖЕ ЛОГАХ
(бок-о-бок): клиент замерил цепочку меша паппета хоста СРАЗУ после загрузки мира --
BP ещё не докомпозировал авторский -85 (лог 16:43:06 `chain=0.00 -> meshOffsetZ_=-85`);
актор паппета навсегда держался на 85 см ниже -> капсула в полу -> движок выталкивает
вверх, наш пер-тик пишет вниз = twitching, средняя поза = просадка. У хоста тот же замер
дал -85.00 -> оффсет 0 -> у него ты выглядел нормально. FIX: замер-гонка УДАЛЁН --
обе стороны mainPlayer_C, устоявшиеся цепочки идентичны ПО КЛАССУ, паппет = wire-поза
дословно (оффсеты Z/yaw выпилены целиком, RULE 2). Это же закрывает старый бэклог
"floating/tall puppet" (та же гонка, другой знак). TEST: как в 16:48 -- хост+клиент,
смотри на паппет хоста у клиента: стоит НА земле, не дёргается. В логе клиента:
`chain diag ... offset is ANCHORED 0`.
(2) **ЧАТ НА T У ХОСТА ДО ПЕРВОГО КЛИЕНТА** -- гейт требовал живого линка (connected());
хост без клиентов = Handshaking -> T молчал. FIX: чат доступен всю ЖИЗНЬ хост-сессии
(лобби живо с нуля клиентов); отправка по проводу best-effort, своя строка в ленте
всегда. TEST: захости, сразу T -- поле открывается, своё сообщение видно.
(3) **ПОЛОСКА ВВОДА T ШИРИНОЙ В ЭКРАН** -- FIX: инпут = ширине КОЛОНКИ чата (та же
ширина переноса ленты, ~42% экрана max), подсказка ушла в placeholder внутри поля.
(4) Бонус: **дефолтный шрифт = Roboto** (твой вердикт «самый лучший»).
Status: build+deploy 4/4 hash-verified `0E52826C38A4F42E`; аудит на диффе. Твой
повтор сценария 16:48 = вердикт.

## 2026-07-04 DAY ADDITION (DLL `D43455FC4787FFE4`)

### 0c. RE-HOST CRASH root-fixed (your 11:11 crash report) — 2-minute test
Your crash (host new game 'abobka' after quitting the first session to the menu) was
root-caused from the two minidumps + the host log: our LoadStorySave cached the FIRST
session's save object at process scope; the re-host skipped the disk load and re-registered
the GC-freed pointer into the GameInstance — the world built from freed memory and the GC
crashed on a worker thread (both days' crashes = byte-identical stack). Fixed at the root:
the save cache is now campaign-scoped (a NEW host boot always reloads the slot from disk —
also fixes stale save CONTENT after autosaves) + liveness-guarded; the GameMode derive
latch rides the same scope (a sandbox-after-story re-host would have carried the wrong
mode).
TEST (exactly your crash sequence): mp_host_game.bat → reach gameplay → выйти в главное
меню → MULTIPLAYER → HOST NEW game (новое имя) → мир должен загрузиться БЕЗ краша.
Повтори 2-3 раза подряд (можно и загрузку СУЩЕСТВУЮЩЕГО сейва вторым заходом). Host log
proof: `engine: save cache -- NEW load campaign (target 's_<имя>', cached 's_1234',
polling world changed) -> full reset, (re)load from disk` + a fresh
`LoadStorySave -- loaded save 's_<имя>' = <ptr> (idx N)` line per re-host. Status:
build+deploy 4/4 hash-verified; 2 audit agents run on the diff; your re-host IS the verdict.

## 2026-07-04 NIGHT ADDITIONS (commit `769d02f7`, DLL `7BCE41C4B6DC9C99` → superseded by `D43455FC4787FFE4`, same features)

### 0a. COOP NO-PAUSE (your report: клиенты замирают на ESC) — 5-second test
Root fix (state-level, one owner): while connected, a paused world is un-paused every
gameplay tick via the game's own SetGamePaused(false) — the ESC pause is EX_CallMath
(PE-invisible), so the STATE is enforced, not the call sites; the console `pause` command
(a GameplayStatics-bypassing path) is caught by the same poll. ESC menu stays fully usable;
solo pause untouched; a solo host (no clients) can still pause.
TEST: host+client session → on the CLIENT press ESC → мир за меню продолжает тикать (на
экране хоста паппет клиента живой, время идёт); ESC-меню кликабельно как обычно. Then the
same with ESC on the HOST. Client log: `pause_guard: world pause detected in a coop session
-- un-pausing`. Status: build+deploy verified; autonomous e2e queued
(VOTVCOOP_RUN_PAUSE_TEST) — YOUR ESC IS the verdict.

### 0b. KILLERWISP v2 — full kill choreography + fair aggro (probe e2e still queued)
What changed: aggro = uniform RANDOM among players in 5000u+LOS with stickiness (no more
host-preference); the wisp swoops to CONTACT before the kill fires (no more 5-m "grab");
the VICTIM gets the native experience (grabbed onto the wisp's socket, movement cut, camera
decoupled, own body visible, lifted ~5 m over 3.5 s, tear montage, death); every OTHER
screen shows the victim's puppet held at the socket riding the lift + the tear. Host safety
during a false-grab hardened (canRagdoll belt — the wisp can no longer ragdoll-kill the
host when its real victim is a client).
TEST (night not required): F1 > Game > Entities > "Spawn killerwisp on client" — as the
client, expect the full grab/lift/death; as the host watching, the puppet rides the wisp.
Then both stand together + plain "Spawn killerWisp" a few times — kills should spread
randomly between you two. Host log: `wisp_aggro: picked victim slot=`, `CLOSING`, `CONTACT`,
`RELAYED grab`; client log: `wisp_hold[self]: grabbed by wispEid=`. Status: built + 2 agent
audits folded + generic smoke PASS x2; the choreography e2e probe run is QUEUED (env flakes
ate three attempts — the s_1234 poisoning lesson); your hands-on is the visual verdict.

## EVENING ADDITIONS (after your two reports)

### 1. DARK-SUIT root fix (all v1sc suits) — rebuilt paks deployed
Your re-diagnosis was right: not inner geometry — NORMALS. The converter recomputed
shading normals from face windings; the GoldSrc scientist COAT is a double-sided sheet
(outer+inner copies), so the accumulation cancelled itself -> near-black white coat from
most angles (your screenshot: white sleeves = single-sided, dark body = doubled). Root
fix: the mdl's AUTHORED normals (studiomdl smoothing groups) now ride the whole pipeline
(extract -> repose rotation -> cook pack); the recompute is deleted. Offline lambert
renders (game-matching backface cull): coat charcoal -> proper white cloth shading,
front/side/back. ALL 15 deployed models rebuilt on their same profiles + redeployed
(walter `cd369fed` / sci `3cacd30b` / luther `99043feb` / einstein `3bf9d7ac` ...), your
source-folder paks + convert.bat dist updated too.
TEST: sci/walter/luther/einstein etc. — the coat must read as WHITE cloth with normal
shading from every angle (no more charcoal side/back).

### 2. EVENTS: почему "тыкаю и ничего" + the NOW! button
RE of the map trigger graph (votv-event-trigger-graph-RE-2026-07-03.md): for 14 events
the fire button only ARMS a level VOLUME; the effect fires when you WALK INTO it
(obelisk's volume = the base entrance — exactly your "сработал когда зашёл на базу").
The F1 events tab now shows, per volume-gated row:
  [volume-gated] -> idle;  [ARMED - walk-in pending] -> fire pressed, waiting for the
  walk-in;  [FIRED] -> consumed (N=0). Badges refresh ~1 Hz while the tab is open.
And a **NOW! button** per such row = arm (clients still get the arm broadcast) + drive
the volume's OWN overlap handler with your pawn — the native walk-in dispatch, no faked
state. Dangerous rows keep the Ctrl+click guard.
Non-volume rows got gate notes in the tooltip (signals = instant in the SETI pool;
bedEvent = fires on next sleep; agrav = needs isPhysicalEvents; treehouse = instant
build step; etc).
Found + documented a GAME bug: paperGray's activator carries bigmRoar's box key (arming
paperGray activates the wrong volume). NOW! on paperGray still works (drives the box
directly); its ARMED badge can never light up through the native arm.
TEST: F1 > Events > obelisk -> row shows [volume-gated]; press the fire button -> badge
goes [ARMED]; press NOW! -> the obelisk chain completes immediately (alarm etc.) without
walking anywhere. Try wisps / mann / vent the same way.

---
Autonomous 2-peer LAN smoke: PASS (both peers stable, puppets spawned, no RAM breach); rig log
lines below are from that run. For the test, the inis are pre-set: HOST `player_skin=kerfur_omega`,
CLIENT `player_skin=kerfur_mynet` (change freely in F1).

## What was ACTUALLY wrong in take-2 (your screenshots), root-caused from your logs + bytecode

1. **Violet light on EVERY skin** — the take-2 bitfield guess (template byte XOR class-CDO byte)
   died on the very first real template: the lifeLight template overrides TWO flags in one packed
   byte, so every read logged `multi-bit flag delta t=10 cdo=20 -- default kept` (your log, 25x)
   and the belly light (LightColor 255,156,255 = that violet) got instanced on every rig — the
   "1 SCS comp(s)" in every take-2 rig line WAS the light. FIX: reflection now reads the REAL
   FBoolProperty bit mask (calibrated slot +0x78, bVisible mask 0x20 — confirmed live); the
   template's authored bit is read directly. lifeLight bVisible=FALSE -> never instanced.
2. **mynet effects across the whole screen** — two unhonored template flags:
   - the 11 digital-grid decals author **bAbsoluteRotation=TRUE** (the projection box points
     world-DOWN, always — grid patches on the floor under her limbs). Our KeepRelative attach made
     the boxes tumble with the limb bones until one swallowed the camera -> grid smeared over the
     screen. FIX: SetAbsolute per the template flags after attach.
   - all 17 electricity emitters author **bStartWithTickEnabled=FALSE** (the native sim never
     advances — authored-off decoration). Ours ticked at full rate -> continuous particle flood.
     FIX: tick disabled right after spawn, per the template.
   - bonus: the 3 zapp crackle loops now carry their authored `att_small` attenuation (was null ->
     crackle audible way too far).
3. **mynet double footsteps** — the native mynet's own step() calls lib_C::step with **volume=0**
   (the default surface footstep is MUTED) and plays boltrix_mediumHit itself at the actor
   location, vol 1, att_default. We were playing BOTH the default step and boltrix. FIX: step
   modes per bytecode — REPLACE (mynet): the puppet's lib step runs at volume 0 (trace/water/
   friction still native) + boltrix at native volume; ADDITIVE (keljoy): default step stays,
   squeak layered with the native stepped() math (clamp(MaxWalkSpeed/400, .5, 2)*vol -> /4 volume,
   /2+1 pitch, attached to the body).
4. **(audit catch) mynet step BURST never actually spawned** — since take-1, SpawnStepBurst filled
   the SpawnEmitterAtLocation frame under three WRONG param names (SpawnLocation/SpawnRotation/
   bAutoActivate vs the real Location/Rotation/bAutoActivateSystem), so the emitter spawned inert
   at the world origin — plus 3 ParamFrame error log lines per step. Correctness agent caught it;
   fixed — the per-step electric burst at the feet is expected to be VISIBLE for the first time.

## Also in this build (your request)

**"<nick> joined the game" now fires at the joiner's APPEARANCE** — the moment its puppet spawns
on your screen — instead of ClientWorldReady+5s (measured live: world-ready 15:27:23, puppet
15:27:34 — the old line ran ~6 s before the body). No artificial delay. "Connecting to the
game..." still comes at connect; your own "Joined <host>'s game" keeps its +5 s (loading screen).
Test: host up, client joins — the "joined the game" line must land at the SAME moment the robot
pops in, not before.

## Tests

1. **Any kerfur skin (the violet report):** cycle omega / ariral / keith / maxwell etc. NO violet
   light on any of them, no light cast on the ground at night. The omega body should now look
   EXACTLY like a crafted NPC kerfur next to it (same mesh+materials, incl. whatever chest glow
   the naturals show — that part is baked in the game's own materials).
2. **Omega face:** live blue animated face on the screen (blinking), `_m` pink, `_h` green.
3. **mynet (the flood report):** standing still — NO screen flood; the correct native look is
   modest: grid glow patches on the FLOOR under her, spark crackle audio nearby only. Walking —
   an electric burst + boltrix hit per step on the PUPPET (host view), and NO default footstep
   under it (electric hit only).
4. **mynet own body (boundary, deliberate):** your OWN steps as mynet still sound default to
   YOUR ears + the visual burst; the electric REPLACEMENT sound can't be layered on the local
   body without doubling (the native local step path is EX-invisible / unmutable). Remote peers
   hear you correctly. If this bothers you, say so — the next step would be a lib_C::step VM
   patch to mute the local default (heavier, doable).
5. **keljoy:** default step + squeak on top (squeak now quieter + slightly pitched-up — the
   native stepped() mix, not the flat 0.6 of take-2).
6. **Ragdoll / skin switching / both-role** as take-2 (rig hides with the plushie, full teardown
   on switch, host-worn skins mirror to the client).

## Log proof (votv-coop.log) — already seen in the smoke run

- `reflection: FBoolProperty payload slot calibrated to +0x78 (SceneComponent CDO bVisible mask 20)`
- `skin_effects: rig 'kerfur_omega' ... 0 SCS comp(s), face=YES (type=0 fmi=1)` (take-2 said
  "1 SCS comp(s)" — that comp was the violet light)
- `skin_effects: rig 'kerfur_mynet' ... 31 SCS comp(s), ... stepSound=YES, stepBurst=YES` (was 32)
- `player_handshake: slot 1 joined the game (announced at puppet appearance)` in the SAME second
  as `net: first remote pose on slot 1 -> auto-spawning puppet`
- NO `scs_rig: multi-bit flag delta` (code deleted), NO `bool property ... not found`, NO
  `bStartWithTickEnabled unresolved`, NO `load MISS`, NO `ParamFrame::Set: unknown param`.

## Known boundaries

- Own-body REPLACE sound (test 4 above).
- The ADDITIVE squeak plays regardless of floor surface; the native game skips it on water
  (water replaces the whole step chain) — puppet squeak on water is a minor fidelity edge.
- Face gated to the 4 omega bodies; maid/krampus are mesh-only skins (their base-class SCS is
  all dormant sentient nodes = nothing instanced, correct).
- The col (paintable) kerfur NPC's picked color is still not on the wire (separate note).

## TIME (your report: "не работает нормально и мгновенно")

Root 1: set-clock wrote only the named clock; the sun re-derives from totalTime every tick and
never moved until the minute pulse. Root 2: the CLIENT's HUD clock/day were structurally FROZEN
(TimeScale=0 = no minute pulse; the wire only carried the sun accumulators). Now: dev-menu
set-clock/sun-slider write the FULL state (sun jumps the same frame, HUD + scheduler + save
consistent), and v96 TimeSync carries the named clock -- the client's clock/day converge <=2 s.
Test: host F1 > set Day/Time -> lighting changes INSTANTLY on host; client sun follows <=2 s and
its HUD clock/day now match the host. Forward day-jumps still fire skipped scheduled events
natively (that part is the game's own settime walk).

## WISP LANE (late eve, "Go next"): the wisps event now MIRRORS -- first creature-event lane

The `wisps` swarm (the biggest creature-event gap) is now host-authoritative + mirrored:
the swarm's 32 wisp_C spawn via EX_CallMath (interceptor-blind) -> caught by a source-gated
Func-thunk on BeginDeferred -> enrolled + broadcast; clients materialize mirrors that fall,
FADE IN AT LANDING (the lane drives the fade edge the parked mirror can't compute), wander
(pose stream, >31-NPC fair rotation), and vanish on BOTH peers at dawn/approach (the
PE-invisible self-despawn is caught by a new pose-walk dead-retire -> EntityDestroy).
Ambient forest wisps (the ticker's) deliberately stay per-peer local -- same decision as the
colored wisps. Autonomous e2e smoke: 32/32 enrolled -> 32/32 mirrored -> forced midday ->
32/32 dead-retired -> 32/32 client teardowns, zero errors.
TEST (needs NIGHT): host F1 > Events > wisps -> NOW! -> BOTH peers should see the same
glowing swarm land in the forest ring (~0.5-0.75 km out) and wander; jump the clock to day
-> they fade out on both. KNOWN fidelity edge: the mirror's fade-in fires when the streamed
pose reads grounded -- if a wisp descends "not falling" (CMC mode) the fade could fire mid-air.
KILLERWISP vs peers: the probe proved the June chain ALIVE (acquired, relayed, client died
ok=1); the missing kill choreography + aggro fairness were then BUILT as v2 (`769d02f7`) --
see section 0b at the TOP of this runbook for the test.

## EVENT TRIGGERING + MIRROR (your report) -- what changed + what I need from you

- Trigger dispatch itself is instant (runEvent/runSpecialEvent on the eventer, same frame). BUT
  many rows only ARM a controller that waits for its native day/time window (the menu shows each
  row's native Day + HH:MM) -- with the time fix above, the workflow "trigger -> set clock to the
  row's window" is now actually instant. If you want one-click "fire AT its native time" (auto
  clock jump), say so -- easy to add now.
- MIRROR coverage today: save/story/cosmetic flips + scare arms REPLAY on the client; the 16
  world-actor classes (saucers/ships/UFOs/droppers) mirror live via the WA lane; **Character
  creatures (wisps, boars, grays invasions, buster...) have NO lane yet** -- the client will not
  see those spawns. That's the big known gap, and it's per-creature-type work (identity + pose
  interp like the kerfur lane).
- WHICH events did you trigger when the mirror looked broken? Name 2-3 -- I'll check each against
  the lane matrix and either fix the verdict or build the missing lane next.

## INNER-MESH -- **REVERTED** (user 18:3x: "ты удалил им всем руки")

The interior-scan strip was WRONG: dm_base is not a hidden inner shell on these models -- it
carries the VISIBLE forearms+hands (in bind pose the outstretched arms live in the same narrow
Z band 36-61 as the coat torso, which the bbox analysis misread as "coat band only"). All three
models lost their arms. Full revert `9963078f` (converter back to pre-scan, RULE 2: the
enclosure heuristic + strip/keep flags deleted); paks rebuilt from the original MDLs with the
pre-scan converter (the exact code state of the verified 25-skin census `805ae0f8`) and
redeployed: sci `5c4dcd1b` / walter `8839e725` / luther `2fc3593c` -- hash-identical across
your source folder + models/ + host+client+copy2. Portable dist rebuilt reverted; your own
convert.bat copy (walter2 folder) was still the clean 07-02 build, untouched.
The original "inner вылазит" complaint is REOPENED -- a future fix needs per-model VISUAL
before/after proof (rendered), not a geometric enclosure heuristic.
