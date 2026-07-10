# Folder-placement audit — full sweep (2026-07-10)

**Trigger:** user — "check what else of .cpps sit in wrong folder" (after ambient_spawner_suppress
was found in coop/session/). **Method:** one audit agent, all 567 files under src/votv-coop/
{src,include} (322 src + 245 include), judged by each file's own purpose comment + load-bearing
code against the folder-per-domain-concept rule (CLAUDE.md / feedback_folder_per_domain_concept).
**Status:** AUDIT (read-only; nothing moved). ue_wrap/ verified clean in the principle-7 direction
(zero coop/ includes). HEAD at audit: 0c4c91e0.

## Ranked violations

| # | file(s) | why wrong | correct home | conf |
|---|---|---|---|---|
| 1 | coop/dev/teleport_client.* | shipped join/moderation verb (every join uses it via subsystems.cpp:40; F1 Admin teleport; event_feed applies its packet) living in dev-tools | coop/session/ or the moderation home (#10) | HIGH |
| 2 | session/ambient_spawner_suppress.* (KNOWN) + props/host_spawn_watcher.* | TWO HALVES of one concept: client suppress half + host mirror half of ambient-spawn authority, split across session/ and props/ | the pending spawn-authority home takes BOTH (T1 design) | HIGH |
| 3 | interactables/lerp_window.h | cross-element interpolation primitive (extracted from RemotePlayer + Npc; consumers = element/npc, element/world_actor, player/remote_player, atv/drone) | coop/element/ | HIGH |
| 4 | session/join_curtain.* | ImGui render component drawing in the DX Present hook inside session/; other half of the join-screen concept is ui/loading_screen | render half -> ui/; session keeps Show/Dismiss triggers | HIGH |
| 5 | session/ini_config.* vs harness/config.* + scattered per-file ini readers | config concept split 3+ ways; 8 coop/ TUs reach UP into harness/config.h for ModuleDir (gameplay depending on boot-glue) | ONE config home | HIGH (merge) |
| 6 | creatures/world_actor_sync.* + world_actor_mirror.cpp + world_actor_detail.h | mirrors the ~14 NON-Character event actors (saucers, mothership, ariral ships, jellyfish, firetank) — not creatures; element half already in element/world_actor; piramid_sync rides the same lane | world-event concept (coop/world/) or dedicated folder | MED |
| 7 | session/mirror_defer.* | join-window mirror visibility layer whose other half is element/quiescence_drain | coop/element/ | MED |
| 8 | player/inventory_pickup_sync.* | syncs the inventory-collect action — same axis as items/player_inventory_sync + items/inventory_wire (near-identical names in two folders = the smell) | coop/items/ | MED |
| 9 | session/event_feed.cpp + event_dispatch_{entity,state,world}.cpp + event_dispatch.h | the ReliableKind wire ROUTER (MTA CPacketHandler shape) is its own concept, not session lifecycle; public header still claims the outgrown feed concept | coop/dispatch/ (or beside net/); ties into the queued event_dispatch_intent extraction | MED |
| 10 | session/{moderation,ban_list,seen_players}.* (+ #1) | a crisp 3-4-file moderation concept without a home (MTA: CBanManager is its own module) | coop/moderation/ | MED |
| 11 | session/{save_block,save_button_disable,save_guard,save_indicator_suppress,save_transfer}.* | coherent 5-file coop-save-discipline family, largest family in session/ | coop/save/ (props/save_identity_* stays — prop identity) | MED |
| 12 | coop/scan/{incremental,settled}_object_scan.h | header-only GUObjectArray discovery, includes ONLY ue_wrap/reflection.h, zero coop state; "scan" names a technique not a concept | ue_wrap/ | MED |
| 13 | session/multiplayer_menu.* | injects the MULTIPLAYER UButton + opens the ImGui browser — UI feature whose other half is ui/server_browser | ui/ (milder: injection rides session state) | MED-LOW |

## Merge candidates (two-halves-of-one-concept)
ambient-spawn authority (#2) · configuration (#5) · join screen-cover (#4) · join-window mirror
settle (#7) · inventory sync (#8) · economy (LOW, flag only): world/balance_sync vs
items/order_sync.

## Folder-level verdict
`coop/session/` fails the name-the-concept-crisply test: it currently hosts ~6 concepts
(lifecycle, join, save family, moderation family, the wire router, config, a UI injection).
Executing 1-13 shrinks it back to genuine session lifecycle + join.

## Judged fine (pre-empted false positives)
net/session_*.cpp = transport data-plane TUs of the one Session class; dev/flashlight_setup
genuinely autotest-only; pause_guard = session-connected invariant; props/world_load_episode
borderline-LOW (join-episode flag armed/cleared/read by props-side seams — watch it); ue_wrap/
has no coop leakage.

## Sequencing recommendation (not executed)
No big-bang reorg. (a) #2 lands WITH the T1 spawn-authority design (already pre-registered in
docs/COOP_RNG_AUTHORITY.md). (b) The rest as a dedicated byte-faithful move series (the 06-29
reorg shape: one concept per commit, includes fixed, no logic changes), user-green-lit, ordered
1, 3, 4, 5 (HIGH) then 6-13. (c) #9's move pairs naturally with the queued event_dispatch_state
805-LOC extraction.
