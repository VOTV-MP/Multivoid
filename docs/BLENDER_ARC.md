# BLENDER_ARC — VotvIO: the Blender 5.1 addon that imports a VOTV .sav into a full scene

> Living doc for the VotvIO arc. Started 2026-08-29 (user ask, verbatim: «Нужен аддон для блендера 5.1,
> который позволит импортировать .sav сохранение Voices Of The Void — далее аддон прогрузит всю карту и
> все ассеты, которые валяются на карте, все модели и все entity, все npc»; precedent bar named by the
> user: SourceIO — self-contained, no manual export steps). Addon name **VotvIO** (user's pick).
> Design of record: `research/findings/tooling/votv-blender-sav-importer-DESIGN-2026-08-29.md` (local-only,
> like all research/ pointers) + its QF transcript sibling. Status: **P0+P1+P2 BUILT the same day**
> (commits `[blender] VotvIO P0+P1` + `P2`), deployed to `extensions/user_default/votvio` and enabled.

## 0. AS-BUILT (2026-08-29, headless smoke on the real s_1234.sav + untitled_1)

- `.sav` (20.3 MB) parses in **0.4 s**; full import **~106 s cold**: **74,492 objects** — 5,091 save
  props with real meshes, 2,172 map statics, **64,823 ISM/HISM/foliage instances** (native-tail parser,
  self-validating scan anchored on `NumBuiltInstances`), **256/256 landscape components** from
  in-package heightmaps, 352 lights; 906 mesh datablocks, 655 decoded textures, 0 assembly warnings.
- Reconcile (interim v1): level actors implementing `int_save`/`int_primitive` (checked live via the
  class package `Interfaces[]`, inherited) are skipped — 6,812 skipped; save rows re-express them.
  The kismet gatherer table (48 classes) is still the P2-polish item; `loadTransform=false` fixtures
  may sit at row transforms until then.
- Resolution ladder shipped: SCS templates → **CDO `name` → list_props** (the measured universal
  mechanism: every prop_C descendant carries `name` in its CDO; potato/coal_s/drive/paper/wbox
  confirmed) → curated supplement (dirthole mounds). Residual placeholders: 376 (trashBitsPile 264 —
  procedural pile visuals, prop_C 47, barnshelf 23, small tail).
- New measured facts vs the design: `FMeshUVHalf.to_mesh_uv_float` returns RAW half bits (decoded
  ourselves); the "Invalid boolean value" storm on some meshes is a POST-LOD tail failure
  (occluder/speedtree section) — geometry parses, materials come from the property block; ISM
  instance data confirmed as 64-byte FMatrix bulk arrays.
- Renders: scratchpad `votvio_shot.png` / `votvio_wide.png` / `votvio_top.png`.

**v3 same day (user feedback pass):** the resolver now composes the **SCS TREE**
(SCS_Node graph; flattening it was why dish heads sat buried at the actor origin) and
`pose_random.py` gives articulated fixtures seeded working poses (dish axis_Z/axis_Y,
windturbine axis_room/axis_blades, radar rot_Z; measured pivots). Reconcile refined:
a level int_save actor is skipped only when its CLASS has save rows — watchtowers/
fences render again, turbines spawn posed from their 4 rows. `materials.py` is the
census-grounded **family analog of the game's material system** (tex / ag=emissive
mask / ao / normal / rough, color/emissioncolor, Masked/Translucent/Additive, foliage,
triplanar box-mapping, built-in water shader from `w_absorb`), plus a **terrain style**
option (green default / snow / dirt, slope-rock blend) replacing the white ground.
**Import radius** option: 0 = whole map; else meters around the base (origin =
`baseBuilding_C` root): at 150 m the scene is 9,544 objects vs 77,209 full.

**v4 (same day, user field pass) — SUPERSEDED BY v5, its premise measured FALSE.**
v4 claimed "the umap holds only DELTA components of a BP actor" and built a tree-first
assembly pass on it. probe_base1..6 (2026-08-29, next session) measured: the cooked umap
serializes an export for EVERY component (the attach graph needs them) — only the
PROPERTIES are delta — and tree-first double-applied the root component's transform
(`world @ root_rel` where `world` already was the root's world), which floated the whole
base 61 m up. What v4 got right and what stays: the named imposter table (`rend`) and
**technical meshes hidden by default** (`misc/cube`, `misc/qweqwe`; "Show technical
meshes" option). The radiotower monolith's real root was ISM-blind template reading, not
missing exports.

**v5 (2026-08-29, the base+radiotower bench pass; commit `42bb819d`) — the flat-pass world
model.** The user's three field reports, each probed to a measured root on `untitled_1`:
- **База в воздухе**: tree-first's double root transform (above). Tree-first DELETED whole
  (RULE 2); the flat pass is the one placement path, with per-property template fallbacks
  (mesh, relative transform, visibility, ISM instances).
- **Лестница вышки отсутствует**: the ladder is a `ladder_old_C` ChildActor — a `*_CAT_*`
  actor ALREADY in PersistentLevel at its live transform — whose ISM `segment1..5` carry
  91 cooked rung instances but no mesh anywhere in `ladder_old`'s own package: the meshes
  live on the PARENT class `ladder_C` (`newladders/newladder_ladder1..5` + `ladderTop`).
  A child BP's template export is itself a DELTA vs the parent's template → template
  inheritance is now per-PROPERTY (`TemplateComp.inherit`). The resolver also reads
  ISM-typed templates (`kind="ISM"`) and their baked instances (`template_instances`),
  and the umap pass falls back to them when a delta has no native tail.
- **«Черновые» меши базы на правильном месте**: `base2_collision_rain` ships
  `bRenderInMainPass=False`, which was never read. Visibility is now per-flag
  delta-else-template (`bHiddenInGame`/`bVisible`/`bRenderInMainPass`) + actor-level
  `bHidden`.
- **Keyed-fixture reconcile** (the game's own `gatherDataFromKey` identity): a save row
  whose `key` matches a level actor of the same class at the cooked transform (±1 uu)
  KEEPS the level actor — exact UCS-built ISM instances (the mast's real segment Zs,
  25 panel lights, 27 greebles) instead of the class template's stale bake. 2,224/3,385
  rows of `s_test_screens2` match a level key; 750 kept in the 150 m bench;
  trashBitsPile placeholders 327→17. Keys resolve delta-first, then class CDO
  (`radiotower_C` keys itself `'radiotower'` in the CDO).
- **Events collection** (hidden): event-scripted level actors the game shows only at
  runtime (`arirShip_tower_C`, `trigger_agrav_C` — cooked data carries no hidden flag,
  measured). Curated, grown per measured case. The «корабль ариралов над базой» was this
  actor's `warparrow_appear` mesh; the crushed cars were save rows of the old save (the
  bench moved to `s_test_screens2`).
Bench evidence (verify3, console listing): base 75 objects Z 60.7–71.2 (0 above 80);
ladder 92 rungs Z 60.9–150.9 with the metal meshes; `radiotower_2` complete from umap
data with NO save-spawn duplicate; collision shells 0; floaters near base = one burger
saved at Z=83 m (save truth). Full-map smoke: 79,129 objects / 110 s / 0 warnings.
Плавающие бургеры/сэндвичи — правда сейва (probe5: `prop_burger_C` rows at Z 83–183 m).

**v6 (2026-08-29, same session; commit `31551742`) — six more field reports, each measured:**
- **Арбуз с материалом воды**: `_family` матчил ПОДСТРОКИ имён ("mat_watermelon" ⊃
  "mat_water"). Water = корень MIC-цепочки против измеренного набора базовых материалов
  (`mat_water/2/waterRiver/frozenWater/bucketWater/gldsrcQuakeWater`) или параметр
  `w_absorb`; mesh_hint-проброс удалён (RULE 2). Никогда не классифицировать ассеты по
  подстроке имени.
- **Река без воды**: река = **85 `SplineMeshComponent` у `Landscape_0`**, гнущих
  `meshes/misc/river` по сплайнам Эрмита — тип, который не читался вовсе. Новый
  `spline_mesh.py` воспроизводит slice-математику USplineMeshComponent (Hermite
  pos/tangent + SplineUpDir-фрейм, 2D-скейл, roll, offset) в numpy;
  `mesh_build.build_spline_mesh` запекает сегменты; материал `Inst_waterRiver` корнем в
  `mat_waterRiver` → водная семья. Полная карта: 85 сегментов X −404..1032 Y −932..927.
- **Грайм-декали (пустые кубики)**: VOTV красит грязь `DecalComponent`-ами (992 в umap,
  936 grime_*-акторов; материал часто через level-MID, чей родитель — на шаблоне класса,
  наследуется по-свойству через SuperStruct). Декали = тонкие двусторонние квады (±2 см
  вдоль оси проекции) с alpha-blend материалом. **Грайм-классы — `int_primitive`:
  их выражает `primitivesData` сейва** — примитивы теперь идут через `place_row`, а
  `spawn_plan` эмитит DECAL-строки из шаблонов класса (739 DecalGrunge_dirt + 259
  dynamicWallDirt + кровь/масло в полной сцене); мусорные кучи-примитивы попутно
  разрешились в меши chipsPile.
- **Дверцы шкафчиков**: дверь висит на петле-`ArrowComponent`, чей шаблонный rel
  (+27.9,+47,+132.2) в точности компенсирует смещение двери — а резолвер читал только
  SM/SK/Scene шаблоны. Теперь читаются шаблоны ВСЕХ `*Component` (kind OTHER, трансформы).
  19/19 шкафчиков: дверь в 0.00 м от корпуса, закрыта.
- **Бункер без стен**: альфа-бункер = **BSP уровня** (61 `ModelComponent`, ни одного
  StaticMesh в боксе комнаты). Новый `bsp_model.py` парсит кукнутый хвост UModel
  (StripFlags/Bounds/Vectors/Points/Nodes/Surfs/Verts, каждый bulk-заголовок валидируется,
  offset-самовосстановление ±4/±20); импортёр строит Model уровня одним мешем: 5 568
  треугольников, 25 материалов через ImportMap, 439 вершин в боксе бункера.
- **Стекло главного окна**: у панелей `newbaseWindow2_sig2_*` в куке все слоты —
  плейсхолдер `WorldGridMaterial`, и пакет d_window не ссылается НИ на один материал
  (рантайм-система мытья/битья) → кураторский слот-оверрайд → `inst_glass`
  (`PLACEHOLDER_SLOT_OVERRIDES` в materials.py).
- **Лестницы/дверцы в воздухе в комнате workstation**: ChildActor-цепочки, запаркованные
  под `scene_dynamicClutter` базы, игра расставляет event-графом в рантайме (измерено:
  все 5 лестниц + дверцы карголифта сериализованы В ОРИДЖИНЕ базы) → скрытая коллекция
  **Unplaced**, рекурсивно по цепочке детей.
Полный смоук: 79 264 объекта / 111 с / 0 предупреждений. verify4 на обоих blend: река 85
сегментов с Inst_waterRiver; арбуз mat_watermelon (0 водных попаданий); шкафчики 19/19;
BSP-вершины в боксе 439; панели inst_glass; Unplaced скрыт; лестниц на ориджине в Statics 0.

**v7 (same session) — пять репортов: UV декалей по-движковому, обе стороны, окно по
inst_newwindow, экраны с настоящим контентом, террейн по weightmap.** Репорты юзера:
декали «не существуют», muralы растянуты/в стенах/не видны, у главного окна не тот
diffuse, RT-экраны/часы — white noise (неправильно), террейн далёк от игры. Корни, все
измерены: (1) **UV декали был повёрнут на 90°** — у бокса baseMural длинная ось Z, арт
горизонтальный: UE-конвенция U←локальная Z, V←Y (`decals._uv`; D3D-флип V поглощается
нижним ориджином UV Блендера); (2) проекция шла только вдоль +X — движок красит ОБЕ
принимающие грани → два прохода ±X, mural получил лист на видимой стороне (578 вершин =
2 листа; 818/976 декалей двухсторонние), winding по направлению каста; (3) grime-текстуры
светло-серые (RGB 0.65), а `mat_decal_grunge` рисует их UNLIT (MSM_Unlit, замерено) —
на лит-стене это тёмные пятна: затемнение ×(0.5,0.47,0.42) + буст альфы ×1.3 (калибровка
по скрину игры); (4) **`tex_decalWindow` — вовсе не грязь, а нарисованное окно** (PNG);
настоящий материал панелей — `inst_newwindow` из ImportMap d_window: ThinTranslucent,
tint (0.611,0.708,0.667), канва мытья `tex_windowDirtDefault` 1645×512 (три панели в
одной, UV панелей режут её на трети: u 0.25..0.75 у средней), CDO d_window несёт
дефолт-JPEG и size 1645×512 — `_build_dirty_glass` переписан на эти константы;
(5) экраны: `CachedExpressionData` хранит дефолты параметров + referenced-текстуры —
часы = атлас `digits` 1280×128 (родитель `mat_clockMat`: num=0 → «0», цвет КРАСНЫЙ,
unlit) + `digit_dots`, analogDS_screen = оранжевый (1,0.25,0) по игровому
`TilingNoise_contrast`, graph = жёлтая трасса, bulbs = зелёный по `noise_mask`,
polarity/frequency = оранжевые кольца, screenGrid = сетка 8/2, TV = игровая статика
`tex_hugeNoise`; white-noise нод в экранных материалах НОЛЬ (verify_v7); (6) террейн:
`Landscape.LandscapeMaterial = /Game/inst_mainLandscape` — слои grass/gravel/dirt/rock/
sand с текстурами (`tex_pineGrass2` тайл 4096uu=41 м, `tex_gravel2` 2048uu...) и
weightmap-аллокациями per-component (128×128 B8G8R8A8 в самом umap, канал на компонент,
перепаковка сабсекций как у heightmap; ScaleBias Z/W = полтексельный сдвиг → ориджин 0;
`DataLayer` = маска дыр, не краска — скип): `landscape_material` мешает детальные
текстуры в мировых XY по весам из `ComponentUV`; ценз слоёв: grass 196 комп., +gravel 37,
+dirt/rock/sand единицы. Плюс 6-й инстанс дельта-урока: `_rel_matrix` в umap_import был
«всё-или-ничего» — частичная дельта зануляла шаблонный поворот; теперь пер-свойство.
Стенд: 224 с, 976/7 декалей, 4 ландшафт-компонента (grass+gravel+dirt каждый),
0 предупреждений; verify_v7 зелёный по всем пяти пунктам. НЕ hands-on.

**v7b (same session, сразу после осмотра юзером) — три до-репорта.** (1) «декали есть, но не
видно — двигать хотя бы на 0.01»: офсет 6 мм → **13 мм** + каждый декаль-объект получает ориджин
в ЦЕНТРОИДЕ листа (EEVEE сортирует BLENDED по ориджину объекта — все 976 сидели в мировом нуле)
+ backface culling на декаль-материалах (задний лист двусторонней пары не призрачит сквозь
стену); (2) «шум RGB разноцветный на мониторах — неправильно»: `tex_hugeNoise` — цветное
конфетти, а кук-дефолты mat_tvScreen = active=1/static=0 (контент, не шум) → TV тёмный без
эмиссии; серый TilingNoise остаётся только на настоящих noise-материалах; (3) «часы — два
растянутых нуля; надо время ИЗ СЕЙВА как stale frame»: замерено — clock2 несёт ДВА диджит-слота
(один квад = ПАРА цифр, UV 0..1 на две ячейки; слева часы, справа минуты, dots между), игровой
`num` — двузначный, шейдер делит u на 0.5 → `screens.build_digit_pair` (u-half → tens/ones →
ячейка атласа) + `assemble._append_materials` подставляет `votv_digits_HH`/`_MM` из
`savedtime=(H,M,S)` сейва. Стенд-сейв 01:32 → слоты `votv_digits_01`+`votv_digits_32` (verify).
Плюс materials.py 825 LOC → **screens.py извлечён** (607+226, оба под капом; эквивалентность =
повторный стенд с бит-в-бит отчётом). Стенд 244 с, 0 предупреждений. НЕ hands-on.

**v7c (same session) — пост-шип аудит: 1 CRITICAL + 3 IMPORTANT свёрнуты.** C1 (настоящий):
`ray_cast` НЕ откидывает бэкфейсы → обратный проход бил ту же одностороннюю грань СЗАДИ и строил
второй лист бит-в-бит поверх первого (удвоенная непрозрачность на полу/земле; «818/976
двухсторонних» из v7 — в основном ЭТОТ артефакт, и калибровка затемнения грайма частично сидела
на удвоении) → корневой фикс: хит с нормалью ВДОЛЬ луча = бэкфейс, принадлежит встречному
проходу — отбрасывается в касте, phase A требует `denom < -1e-4`. I1: `_rel_matrix`-переход
измерен транс-диффом (probe_relmatrix): из 36 582 компонентов сдвинулись 103 — sign_C 66
(5-я панель+текст), cord_C 32, cargoLift_C 3 (кнопка, 6.5 м), portal_C 2; структура не тронута,
новые позиции = шаблонные поля по семантике кука. I2: UV ландшафта векторизован
(`foreach_get/foreach_set`; было ~63k интерпретаторных итераций на компонент ≈ 25–75 с на
полной карте) + индекс `{имя→экспорт}` вместо линейного скана ExportMap на каждую weightmap.
I3: >4 слоёв на компонент теперь пишет warning (раньше молча резалось). M1: не-grunge альфа
вернула кламп 1.0; M2: `_wind` суммирует нормали 8 граней (одна вырожденная не решает за весь
лист); M3: мёртвая переменная убрана.

**v7d (same session) — КОРЕНЬ невидимых декалей: ВСЕ статик-меши импортировались ВЫВЕРНУТЫМИ.**
Полевой репорт (два скрина): лист декали сидит ВНУТРИ стены, виден только камерой из стены;
вытащенный — невидим, «обращённый» — виден; то же с грязью на полу. Диагноз: Y-зеркало UE→Blender
САМО переводит D3D-шный CW-front в блендеровский CCW-front, а `mesh_build._assemble` ДОПОЛНИТЕЛЬНО
свопил индексы «(i0,i2,i1) — the y-mirror flips handedness» — двойная компенсация, каждый меш
наизнанку. Жило с v1 БЕЗ симптома (материалы рендерятся двусторонне) и всплыло только у первого
ПОТРЕБИТЕЛЯ нормалей: `ray_cast` возвращал нормали приёмников ВНУТРЬ → декали поднимались на
13 мм В стену и заворачивались лицом в полость (C1-фильтр v7c честно верил вывернутым нормалям);
ландшафт был иммунен (его сетка уже строилась нормалями вверх — потому декали на земле работали
в v6d и прятали класс). Фикс: естественный порядок индексов в `_assemble` + BSP-фан; инструмент —
ВЫПУКЛЫЙ меш (куб + арбуз): 100% нормалей наружу от центра (вывернутый читает 0%). Стенд 198 с,
975/8 декалей (38 «промахов» v7c оказались лицевыми попаданиями под маской вывернутых нормалей),
0 предупреждений. Верификация: полы комнатных мешей вверх (signalroom 115/20); «приёмник-позади»
пофейсово — **80/80 листов OK** (центроидные ложно-красные = угловые обёртки с диагональной
средней нормалью). Урок спарен:
[[lesson-inside-out-import-surfaces-only-at-a-normals-consumer]]. НЕ hands-on.

**v7e (same session) — ложная эмиссия у 151 из 569 материалов (репорт: curtains_fr,
prop_cubicle4). Настоящий гейт игры — static switch `useEmissive`.** Раскопано в два слоя:
(1) `mat_object.CachedExpressionData`: дефолт `emisive_strength`=1.0, дефолт **`ag` = engine
Black** — strength без маски мёртв (второй дивиденд урока
[[lesson-cooked-material-cachedexpressiondata-keeps-defaults]]); но у выживших после ag-гейта
(банан/церковь/подушка/мост, strength=100!) ag оказался ОДНОЙ И ТОЙ ЖЕ маской ЛАМПЫ
`tex_ceillampMask` — карго-культный клон лампового MIC; (2) дискриминатор:
**`StaticParameters.StaticSwitchParameters.useEmissive`** — у банана FALSE, у `inst_alamp2_on`
TRUE (ag=tex_alarmGlow), у `alamp2_off` FALSE; без оверрайда в цепочке = выключено (родительский
дефолт). Фикс: `_analyze` собирает static-switches по цепочке (leaf-first, только bOverride);
эмиссив-бранч требует `useemissive`=TRUE (для `emisive_strength`-семейства) + загруженную
ag-маску. ПОПУТНО: `ensure_mesh` перенесён ЗА радиус-гейт umap-цикла (дальние меши/материалы/
текстуры не строились зря — сотни zero-user датаблоков) + `orphans_purge` перед сохранением
стенда. Экраны/цифры/additive не затронуты (свои бранчи).

**v8 (same session) — json-грамматика примитивов, оконная dyn-грязь, постеры, z-fight,
лампы xray.** Пять репортов. Измерено: (1) **json примитив-ряда = `[вариант, размер%]`** —
crack/blood ПЕРСИСТЯТ индекс варианта (`[14,100]`, `[7,100]`; `-1`=случайный), второе число —
процент размера (oil 300, poo 50): `decals.row_variant_size` + сохранённый вариант побеждает
seeded-выбор, размер умножает DecalSize; (2) **mat_dynamicWallDirt: size=2048,
decalScale=(200²)** — каждая декаль семплит ОКНО 400/2048≈19.5% большой простыни
`tex_dirtGrimeOverlay` (DXT5, настоящая альфа 20%) со случайным per-instance offset (рантаймный
MID `offset`) — мы вжимали все 2048px в каждый квад, отсюда «весь квадрат со швами» и «слишком
много декалей»: оконный семплинг с 12 seeded-бакетами (`get_decal_material` seed-параметр;
seed теперь ПЕР-ИНСТАНСНЫЙ через `queue_decal` — одинаковый label вырождал бакеты и джиттер);
(3) **постеры**: класс в сейве = `poster.poster_c` БЕЗ `_C`; сначала предположил «кастом →
seeded», но ЮЗЕР поправил (в этом сейве кот hang-in-there и карта тарелок) и ряд ДОКАЗАЛ:
**ints[0][0] = индекс страницы постера** (замерено: 0=«I BELIEVE», 4=кот «HANG IN THAERE :D»,
8=карта с буквами тарелок; CDO `index:-1`, BP ставит `tex_poster_<index>` через
`SetTextureParameterValue`) → `decals.poster_index(row)` подставляет ТОЧНУЮ страницу из
сейва, seeded — только фолбэк (ноль пустышек `00000000texture` в блендах); (4) z-fight: базовый лифт 13→20 мм +
пер-декальный джиттер 0–8 мм (`decal_lift`; стопки декалей больше не делят глубину), альфа-буст
×1.3 у grunge откачен (его причина — вывернутые нормали — убита в v7d); **v8c (`730a3d26`):
и затемнение ×0.5 туда же** — полевое сравнение (наша сцена vs игра, одна комната) показало
угольные потёки и буро-грязевую палитру против серо-зелёной игровой; unlit-шейдер grunge рисует
RGB текстуры КАК ЕСТЬ, а декаль и её стена освещаются у нас одинаково, так что немодифицированная
текстура держит и относительный контраст, и цветовое разнообразие (мшистые трещины остаются
зелёными). Оба множителя калибровались ПОВЕРХ невидимости с чужими корнями — RULE 2; (5) все LIGHT-объекты
`show_in_front=True` (USER) + дефолтные Cube/Light/Camera factory-startup удаляются в smoke
(134/134 ламп xray). Верификация: 12 различных окон dyn, постеры 10/10 на tex_poster_N,
crack-варианты из сейва живые, 974/9, 0 предупреждений. **Вопрос юзера «носят ли декали
привязку к поверхности» — НЕТ, измерено:** ряд сейва = class+transform+key+json ЦЕЛИКОМ,
umap-декаль = AttachParent (иерархия трансформа, не привязка); deferred-декаль в UE ни к чему
не «клеится» — каждый кадр проецируется на всё в своём OBB, трансформ и ЕСТЬ вся информация
(позиция НА поверхности, локальный X внутрь) — наша ±X-проекция уже воспроизводит именно это;
единственный источник расхождений — различие геометрии-приёмника (мы прячем пропы при касте).

**v6d (same session, `0e37227b`) — декали ПРОЕЦИРУЮТСЯ на приёмники.** Репорт юзера:
повёрнуты не так + торчат, нет маски поверхности. Свободный квад заменён проекцией:
декали копятся в очередь и после сборки statics/BSP/ландшафта кастят сетку лучей вдоль
своей локальной X через глубину бокса — промахи отбрасываются (маска: декаль наполовину
со стены кончается на краю стены, в пустоте — исчезает), попадания становятся вершинами
+6 мм по нормали попадания (облегает, заворачивает за угол), грани через скачок глубины
режутся (нет паутины через проёмы). Перф: 3×3 разведка → плоский приёмник достраивается
аналитически без лучей; на время каста луч-сцена сжата до Statics+Landscape. Стенд:
976/7, 449→172 с. **Тестовый цикл (USER): только radius-стенд, полную карту не гонять.**

**v6c (same session, `a0a00ad3`) — семейства грайма + CDO-material + грязное стекло.**
Грайм в сцене был одним тёмным сплаттером (739× `dirt_0`), в игре — трещины/потёки/пятна:
классы-варианты (crack/leaky/dusty/light/grainy) НЕ несут материала в куке — BP выбирает
рантаймом из нумерованных семейств пака (`inst_decalCrack_0..16`,
`inst_DecalGrunge_leak_0..7`, `_dirt_0..34`, `inst_decalLeaves_1..4`).
`decals.GRIME_FAMILY`: класс → семейство, сид-выбор per-instance + случайный поворот
вокруг оси проекции (потёки НЕ вертятся — их CDO `randomOrientation=False`). Классы с
фиксированным материалом (пиво/кофе/вино/бензин) держат его в CDO-переменной `material`,
а ШАБЛОННЫЙ DecalMaterial у них врёт (родительская кровь) — резолвер читает
`cdo_material`, он старше шаблона. Стенд: 80 различных декальных материалов (было ~8);
crack=134 == umap-ценз grime_crack точно. Стекло главного окна — мытьевое ГРЯЗНОЕ:
оверрайд панелей теперь строит glass + `tex_decalWindow` кайму (цвет/альфа/шершавость от
грязи), не голый inst_glass.

**v6b (same session, `49454cfb`) — RT-экраны + коллизия class_package.** Белые экраны
воркстейшна и часов: их кукнутые материалы (`mat_tvScreen`, `mat_analogDS_*`,
`mat_polarity`, `mat_frequency`, `inst_segmentDigits*`) НЕ несут ни одного текстурного
параметра — игра рисует их в рантайме (RT/стрелочные шейдеры/7-сегментная логика).
Честная статика = родной «нет сигнала»: кураторские `_SCREEN_ROOT_PREFIXES` → тёмный
CRT с бело-шумной эмиссией (у стола 11 экранных слотов). Часы вскрыли дефект
`class_package`: `clocks_C` коллизится с МЕШ-пакетом `meshes/clocks/clocks` и меш
побеждал — BP-дом `objects/clocks` не грузился; теперь при коллизии предпочитается
`/objects/` — одно это разрешило ~110 шаблонных мешей по карте (no_mesh 499→389).
Полный смоук: 79 353 объекта / 128 с / 0 предупреждений.

## 1. What it is

A Blender 5.1 **extension** (`extensions/user_default/votvio`, manifest-based, python 3.13 + numpy,
**zero wheels**) that reads a `.sav` slot + the game's own `VotV-WindowsNoEditor.pak` directly and
reconstructs the world **as the game would load it**: the whole `untitled_1` map (static meshes,
landscape, foliage, lights), every saved prop/entity/vehicle/NPC from the save's `objectsData` /
`primitivesData` / `GObjStack`, reconciled per the game's own `loadObjects` semantics.

## 2. The measured foundation (one line each; details in the design doc)

- Pak: v11, **unencrypted, uncompressed** (footer fact: all 5 compression slots empty), 42,941 files.
- Save: GVAS `saveSlot_C`; own ~200-line reader parses the real 20 MB `s_1234.sav` clean end-to-end.
- `untitled_1.umap`: 50,951 exports parse in ~10 s (pyUE4Parse); `StreamingLevels=[]` — one persistent world.
- pyUE4Parse StaticMesh 4.27 bug root-caused (unconditional `minMobileLODIdx`) + fixed + geometry proven.
- Vendored parser runs under **Blender's own 3.13.9** (headless smoke: pak → cube geometry, 0.4 s).
- Texture census, FULL population: 4,033 textures, 8 formats, **zero BC7** ⇒ numpy decoders suffice.
- Archetype fact: 57 % of umap SMComponents inherit their mesh from the BP template ⇒
  **ClassTemplateResolver** (TemplateIndex-first) — full census: 81.5 % direct + justified exclusions
  (772 spawner previews) + 193-comp curated supplement; ledger balances exactly.
- Gatherer census (corrected): **48** classes override `gatherDataFromKey`; the 45 `…KeyT` classes are the
  separate trigger lane (doors etc. — never destroyed on load, stay as-cooked).
- NPC: kerfurOmega rows live in `objectsData` with transforms; SK geometry via the proven
  `ue_skelmesh.py` port (self-run: 102 bones / 4,332 verts, round-trip OK).
- Blender assembly measured trivial (20k objects 1.49 s); cold import ~2–4 min grounded, warm < 1 min.

## 3. Decisions (with dates)

- 2026-08-29 user: name **VotvIO**; this doc is the living arc doc.
- 2026-08-29 /qf: self-contained pure-python (SourceIO bar) — external .NET/FModel lanes rejected,
  recorded in the design doc §3.
- 2026-08-29 /qf: acceptance = **machine diff vs the live game** (UE4SS Lua probe, same save, post-load
  snapshot, self-calibrated tolerances) — the probe is **P2 step 1**, built before any P2 verdict.
  Exclusion rows always carry a measured justification; a noisy diff is never silenced by widening.
- 2026-08-29: generated tables (`int_save` membership, gatherer 48, trigger 45, UCS supplement 193,
  exclusions) derive **from the pak at addon build time** — the CXX dump is not a dependency.

## 4. Build plan

- **P0** — extension skeleton + .sav parse + manifest + placeholders. **BUILT.**
- **P1** — save props with real meshes + `tex` BaseColor materials. **BUILT** (5,091 meshed).
- **P2** — umap/landscape/foliage/lights + interim reconcile. **BUILT** (see §0). The formal
  acceptance probe (UE4SS Lua dump + t0/t0+5s calibration + machine diff) is still OWED and remains
  the verification gate — visual renders are smoke, not acceptance.
- **P3** — NPC SK bind pose (473 level SK comps + 23 save SK rows currently placeholders),
  gatherer kismet table, UCS supplement growth. ~~grime decals~~ / ~~splines~~ / ~~BSP~~ —
  CLOSED by the v6 wave (§0 v6/v6c/v6d: projected decals, the river spline deform, the level Model).

## 5. Residual ledger

> **Session handoff 2026-08-29 (end of the v5..v7 bench-fix session).** The build-day verdict
> («Полное говно, масса проблем») has been worked down on the radius bench: FIFTEEN user field
> reports fixed across v5/v6/v6b/v6c/v6d/v7 (§0 above), addon deployed after each wave.
> **Bench save = `s_test_screens2.sav` (user decision — no event objects)**; bench command:
> `VOTVIO_SMOKE_RADIUS=150 VOTVIO_SMOKE_BLEND=<path> blender --background --factory-startup
> --python tools/blender/votvio/tests/smoke.py -- %LOCALAPPDATA%\VotV\Saved\SaveGames\
> s_test_screens2.sav` (~224 s with two-sided decal projection) → scratchpad
> `votvio_base150.blend`.
> **Working agreement (both USER rules): NO renders — hand over the `.blend`; and the test loop
> runs the BENCH ONLY — no full-map smokes per fix**
> ([[feedback-votvio-hand-over-blend-no-renders]]). NOTE: scratchpad `votvio_smoke.blend` (full
> map) was last rebuilt at v6c — its decals are still pre-projection quads AND it predates the
> v7 UV/terrain/screen fixes; rebuild on request.

| Open | What | Phase |
|---|---|---|
| — | ~~база в воздухе / лестница вышки / черновые шеллы~~ **CLOSED v5 `42bb819d`** (§0 v5) | — |
| — | ~~река без воды / арбуз-вода / грайм-кубики / дверцы шкафчиков / бункер / стекло / лестницы в воздухе~~ **CLOSED v6 `31551742`** (§0 v6) | — |
| — | ~~декали «не существуют» / muralы 90°+в стенах / не тот diffuse окна / white-noise экраны / плоский террейн~~ **CLOSED v7** (§0 v7: UE decal-UV + двусторонняя проекция + UNLIT-затемнение грайма + inst_newwindow + CachedExpressionData-экраны + weightmap-слои) | — |
| — | the user's NEXT field-fix batch (pending their .blend inspection) | next |
| — | lake SURFACE: the river splines cross the lake area (segments up to ~55 m wide) — whether the lake reads as water in the scene is unverified | next |
| — | **acceptance probe + calibration + machine diff** (the design's own gate) | next |
| — | gatherer table from the 48 kismet bodies — the v5 keyed-fixture reconcile (row key ↔ level actor at cooked transform) covers the fixture half measurement-driven; the kismet table still owed for loadTransform semantics of rows that MOVED | P3 |
| O8 | non-main-map saves (`Level != Untitled_1`) — the generic attempt + warning IS built (`import_op.py`), never validated on a real dream/tutorial save | P3 |
| O12 | ~~MIC vocabulary census~~ **CLOSED `c19662e2`** — full-population census (3,243 materials) + family builder shipped (`materials.py`); residual = fidelity items (triplanar is an approximation; a base Material's non-parameterized default textures are unreachable in cook → grey fallback; **placeholder slots get curated overrides — `PLACEHOLDER_SLOT_OVERRIDES`**) | — |
| O13 | ~~SplineMeshComponent deform~~ **CLOSED v6 `31551742`** — `spline_mesh.py` Hermite slice math; 85/89 map splines are the river, 4 carry no mesh | — |
| O14 | ~~grime decals~~ **CLOSED v6c+v6d** — variant families + CDO material (`decals.GRIME_FAMILY`, 80 materials) and real PROJECTION (mask/wrap/no-webbing, §0 v6d); residuals: receivers are STRUCTURE only (a decal that sat on a prop projects past it), leaky rusty/wet colour tints live in bytecode, trashBitsPile 81 full-map placeholders | P3 |
| O15 | ~~landscape textures~~ **CLOSED v7** — weightmap-TRUE layer blending shipped (`landscape.py` weight extraction + `materials.landscape_material`, the game's own `inst_mainLandscape` layer textures/tiling); procedural GREEN/SNOW/DIRT stays as the with_textures=False fallback; residual: layer NORMAL/height maps unused, macro variety (grassPatches) unbaked | — |
| — | BSP UV scale is the classic /128 texel guess — bunker wall texture density unverified | P3 |
| — | prop_C stragglers (44) + prop_barnshelf (23) placeholders | P3 |
| — | SK geometry port of `ue_skelmesh.py` (NPC bind pose) | P3 |

## 6. Dev notes

- Repo home: `tools/blender/votvio/` + `tools/blender/deploy_addon.ps1` → `extensions/user_default/votvio`.
- Headless loop: `blender --background --factory-startup --python tools/blender/votvio/tests/smoke.py`
  — ALWAYS with `VOTVIO_SMOKE_RADIUS=150` (USER rule: tests load base+workstation only, never the
  whole map).
- License: MIT (repo license; GPL-compatible per Blender extension rules); vendored pyUE4Parse is MIT with
  attribution; the empirical vendor-patch list is in the design doc §1 (Blender side).
