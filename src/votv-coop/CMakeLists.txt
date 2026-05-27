# votv-coop -- the standalone coop mod (RULE No.3: no UE4SS at runtime).
#
# Builds a standalone x64 DLL injected into VotV-Win64-Shipping.exe (UE4.27).
# Principle 7 keeps two subtrees:
#   src/ue_wrap/   -- engine-wrapper layer (reflection, offsets, hooks)
#   src/coop/      -- gameplay/network layer (RemotePlayer, sessions, UDP)
# Bootstrap/loader lives in src/bootstrap/.
#
# Current stage: proof-of-life bootstrap only. Reflection + hooking added
# behind ue_wrap/ next.

cmake_minimum_required(VERSION 3.20)
project(votv_coop CXX C)  # C for the MinHook sources (third_party/minhook)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MinHook (WP13: established inline-hook library, MIT). We compile its C
# sources into our own static target rather than add_subdirectory(), so it picks
# up the SAME static-CRT runtime as the mod (a runtime mismatch would break the
# link). Source lives in third_party/minhook (git submodule).
set(MINHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook)
add_library(minhook STATIC
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde32.c
    ${MINHOOK_DIR}/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC ${MINHOOK_DIR}/include)

add_library(votv-coop SHARED
    src/bootstrap/dllmain.cpp
    src/ue_wrap/log.cpp
    src/ue_wrap/sig_scan.cpp
    src/ue_wrap/reflection.cpp
    src/ue_wrap/hook.cpp
    src/ue_wrap/game_thread.cpp
    src/ue_wrap/engine.cpp
    src/ue_wrap/engine_bones.cpp
    src/ue_wrap/engine_component.cpp
    src/ue_wrap/engine_pawn.cpp
    src/ue_wrap/engine_widget.cpp
    src/ue_wrap/reflected_offset.cpp
    src/ue_wrap/call.cpp
    src/ue_wrap/puppet.cpp
    src/ue_wrap/hud_feed.cpp
    src/ue_wrap/prop.cpp
    src/coop/remote_player.cpp
    src/coop/remote_prop.cpp
    src/coop/nameplate.cpp
    src/coop/event_feed.cpp
    src/coop/grab_observer.cpp
    src/coop/item_activate.cpp
    src/coop/flashlight_click_sound.cpp
    src/coop/shutdown.cpp
    src/coop/players_registry.cpp
    src/coop/npc_sync.cpp
    src/coop/prop_lifecycle.cpp
    src/coop/prop_snapshot.cpp
    src/coop/weather_sync.cpp
    src/coop/net/transport.cpp
    src/coop/net/session.cpp
    src/coop/net/reliable_channel.cpp
    src/dev/common.cpp
    src/dev/force_weather.cpp
    src/dev/freecam.cpp
    src/dev/flashlight_setup.cpp
    src/dev/pos_hud.cpp
    src/dev/restore_vitals.cpp
    src/dev/teleport_client.cpp
    src/harness/harness.cpp
    src/harness/autotest.cpp
    src/harness/config.cpp
    src/harness/screenshot.cpp
    src/harness/sdk_check.cpp
)

target_include_directories(votv-coop PRIVATE include)
set_target_properties(votv-coop PROPERTIES OUTPUT_NAME "votv-coop")
# version.lib: read the running exe's file version in the health check.
# minhook: our inline-hook substrate (game-thread dispatcher).
# ws2_32: Winsock for the UDP coop transport (src/coop/net).
target_link_libraries(votv-coop PRIVATE version minhook gdiplus ws2_32)

# Standalone shipping loader: a xinput1_3.dll proxy that auto-loads the payload
# (votv-coop.dll) on game start, replacing the dev-only inject.ps1. VOTV imports
# only XInputGetState/XInputSetState from xinput1_3.dll; those are forwarded to
# System32's xinput1_4.dll via the .def. See src/loader/.
add_library(xinput1_3 SHARED
    src/loader/xinput_proxy.cpp
)
# No "lib" prefix; the file must be named exactly xinput1_3.dll.
set_target_properties(xinput1_3 PROPERTIES OUTPUT_NAME "xinput1_3" PREFIX "")

# Static CRT so the injected DLL doesn't depend on a runtime the host
# process may not have loaded.
if (MSVC)
    set_property(TARGET votv-coop PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(votv-coop PRIVATE /W4 /permissive-)

    # MinHook must match the mod's static CRT or the link fails.
    set_property(TARGET minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    set_property(TARGET xinput1_3 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(xinput1_3 PRIVATE /W4 /permissive-)
    # Export forwarders are declared via #pragma comment(linker,"/export:...")
    # in src/loader/xinput_proxy.cpp.
endif()
