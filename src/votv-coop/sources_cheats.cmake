# votv-coop/sources_cheats.cmake -- the cheat tools (src/coop/cheats/, the cheat panes of src/ui/ and
# their dispatch), one per line.
#
# Split from sources.cmake when that list passed the 800-line cap. sources.cmake includes this and
# appends VOTVCOOP_CHEATS_SOURCES to VOTVCOOP_SOURCES.

set(VOTVCOOP_CHEATS_SOURCES
    src/coop/cheats/balance_readout.cpp
    src/coop/cheats/cheat_request.cpp
    src/coop/cheats/self_teleport.cpp
    src/coop/cheats/cheat_run.cpp
    src/coop/cheats/item_catalog.cpp
    src/coop/cheats/item_spawn.cpp
    src/coop/dispatch/event_dispatch_cheat.cpp
    src/ui/cheat_access_pane.cpp
    src/ui/cheat_economy_pane.cpp
    src/ui/cheat_entities_pane.cpp
    src/ui/cheat_events_pane.cpp
    src/ui/cheat_items_pane.cpp
    src/ui/cheat_player_picker.cpp
    src/ui/cheat_world_pane.cpp
)
