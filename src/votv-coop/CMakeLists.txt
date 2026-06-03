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
project(votv_coop VERSION 0.0.1 LANGUAGES CXX C)  # C for the MinHook sources (third_party/minhook)

# Build-metadata suffix per SemVer 2.0 (text after `+`). Identifies the VOTV
# game version this build targets. Reflection offsets and BP field names shift
# between cooks, so the mod is incompatible across game versions -- the suffix
# is the canonical "tested-against" marker in tags + filenames + the boot
# banner. Update on every game-recook adaptation.
set(VOTVCOOP_GAME_TARGET "0.9.0-n")

# Materialize PROJECT_VERSION + VOTVCOOP_GAME_TARGET into a generated header
# that the DLL includes (boot banner + any future feature gates that depend on
# the build metadata).
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/coop/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/coop/version.h
    @ONLY)

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

# Dear ImGui (MIT, vendored as git submodule, pinned v1.91.5) -- OUR OWN copy for
# the in-game dev menu + (future) main-menu MP server browser. NEVER UE4SS's ImGui
# (RULE 3, no UE4SS at runtime). Compiled into our own static target so it picks up
# the same static-CRT as the mod (like minhook). Backends: DX11 + DX12 (we
# auto-detect the RHI at the swapchain) + Win32 (input). Source in third_party/imgui.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)

# --- GameNetworkingSockets (BSD-3, vendored as git submodule, pinned v1.5.1) ---
#
# Wire-layer substrate for the 4-PR GNS migration plan in
# research/findings/votv-gns-integration-plan-2026-05-27.md +
# research/findings/votv-gns-sandbox-pass-2026-05-28.md (sandbox PASS report).
#
# PR-1 vendored + linked the lib behind a /WHOLEARCHIVE smoke. PR-2 wired
# coop::net::Session (src/coop/net/session.cpp) over GNS's CreateListenSocketIP /
# ConnectByIPAddress / SendMessageToConnection / ReceiveMessagesOnConnection,
# deleted the hand-rolled UDP transport + Hello/HelloAck handshake + reliable
# ARQ + Ping/Pong RTT, dropped the smoke + /WHOLEARCHIVE. PR-3 adds lanes
# (ConfigureConnectionLanes) + kMaxPeers=4.
#
# Static-link configuration (sandbox-verified 2026-05-28):
#   USE_CRYPTO=BCrypt           Windows-native AES+SHA, no external crypto dep
#   USE_CRYPTO25519=Reference   donna impl bundled in GNS src/common/
#   Protobuf_USE_STATIC_LIBS    static libprotobuf via vcpkg x64-windows-static
#   MSVC_CRT_STATIC=ON          /MT to match our DLL's static CRT
#   BUILD_STATIC_LIB=ON         everything else OFF
#
# Build requires vcpkg with protobuf:x64-windows-static pre-installed; the
# caller passes CMAKE_TOOLCHAIN_FILE + VCPKG_TARGET_TRIPLET + (if vcpkg cloned
# shallow) VCPKG_MANIFEST_MODE=OFF. See BUILDING.md.
set(BUILD_STATIC_LIB         ON  CACHE BOOL   "" FORCE)
set(BUILD_SHARED_LIB         OFF CACHE BOOL   "" FORCE)
set(BUILD_EXAMPLES           OFF CACHE BOOL   "" FORCE)
set(BUILD_TESTS              OFF CACHE BOOL   "" FORCE)
set(BUILD_TOOLS              OFF CACHE BOOL   "" FORCE)
set(USE_CRYPTO               "BCrypt"    CACHE STRING "" FORCE)
set(USE_CRYPTO25519          "Reference" CACHE STRING "" FORCE)
set(Protobuf_USE_STATIC_LIBS ON  CACHE BOOL   "" FORCE)
# ENABLE_ICE compiles GNS's STUN/TURN/ICE client (P2P NAT-punch path) into
# the static lib. RULE 2 (no migration baggage): we don't ship code for
# unshipped features. Re-enable in the PR-6 commit that adds the first ICE
# caller (P2P signaling via the master server).
set(ENABLE_ICE               OFF CACHE BOOL   "" FORCE)
set(USE_STEAMWEBRTC          OFF CACHE BOOL   "" FORCE)

# Static-CRT for GNS targets. Note: GNS's own MSVC_CRT_STATIC option does
# `string(REPLACE "/MD" "/MT")` on CMAKE_*_FLAGS, which is a no-op under
# CMP0091=NEW (default since CMake 3.15) where the runtime is a TARGET
# property, not part of the flag string. We set the global property here
# BEFORE add_subdirectory so every GNS target inherits MultiThreaded /MT.
# (Our own targets ALSO set this property explicitly below for clarity.)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(third_party/GameNetworkingSockets EXCLUDE_FROM_ALL)

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
    src/ue_wrap/engine_mainplayer.cpp
    src/ue_wrap/engine_playerragdoll.cpp
    src/ue_wrap/engine_attach.cpp
    src/ue_wrap/engine_pawn.cpp
    src/ue_wrap/engine_widget.cpp
    src/ue_wrap/reflected_offset.cpp
    src/ue_wrap/call.cpp
    src/ue_wrap/fname_utils.cpp
    src/ue_wrap/puppet.cpp
    src/ue_wrap/hud_feed.cpp
    src/ue_wrap/prop.cpp
    src/ue_wrap/vitals.cpp
    src/coop/remote_player.cpp
    src/coop/remote_prop.cpp
    src/coop/save_guard.cpp
    src/coop/save_block.cpp
    src/coop/save_button_disable.cpp
    src/coop/nameplate.cpp
    src/coop/event_feed.cpp
    src/coop/player_handshake.cpp
    src/coop/grab_observer.cpp
    src/coop/item_activate.cpp
    src/coop/player_damage.cpp
    src/coop/flashlight_click_sound.cpp
    src/coop/shutdown.cpp
    src/coop/players_registry.cpp
    src/coop/npc_mirror.cpp
    src/coop/npc_sync.cpp
    src/coop/prop_echo_suppress.cpp
    src/coop/prop_element_tracker.cpp
    src/coop/prop_lifecycle.cpp
    src/coop/prop_snapshot.cpp
    src/coop/prop_synth_key.cpp
    src/coop/remote_prop_spawn.cpp
    src/coop/garbage_sync.cpp
    src/coop/trash_collect_sync.cpp
    src/coop/weather_sync.cpp
    src/coop/weather_fog.cpp
    src/coop/weather_lightning.cpp
    src/coop/weather_redsky.cpp
    src/coop/ini_config.cpp
    src/coop/element/element.cpp
    src/coop/element/element_deleter.cpp
    src/coop/element/registry.cpp
    src/coop/net/session.cpp
    src/coop/net/session_status.cpp
    src/coop/net/session_relay.cpp
    src/coop/net_pump.cpp
    src/coop/dev/force_weather.cpp
    src/coop/dev/freecam.cpp
    src/coop/dev/flashlight_setup.cpp
    src/coop/dev/pos_hud.cpp
    src/coop/dev/restore_vitals.cpp
    src/coop/dev/spawn_npc.cpp
    src/coop/dev/teleport_client.cpp
    src/ui/imgui_overlay.cpp
    src/ui/dev_menu.cpp
    src/ui/scoreboard.cpp
    src/coop/roster.cpp
    src/coop/ban_list.cpp
    src/coop/moderation.cpp
    src/harness/harness.cpp
    src/harness/autotest.cpp
    src/harness/autotest_dispatch.cpp
    src/harness/autotest_vitals.cpp
    src/harness/autotest_ragdoll_spawn_probe.cpp
    src/harness/autotest_menutravel_probe.cpp
    src/harness/autotest_fog_probe.cpp
    src/harness/autotest_saveui.cpp
    src/harness/config.cpp
    src/harness/screenshot.cpp
    src/harness/sdk_check.cpp
)

target_include_directories(votv-coop PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/generated)
set_target_properties(votv-coop PROPERTIES OUTPUT_NAME "votv-coop")
# version.lib: read the running exe's file version in the health check.
# minhook: our inline-hook substrate (game-thread dispatcher).
# ws2_32: Winsock for the UDP coop transport (src/coop/net).
target_link_libraries(votv-coop PRIVATE
    version
    minhook
    imgui
    d3d11    # dummy-swapchain creation to read the IDXGISwapChain vtable (ImGui present hook)
    dxgi
    gdiplus
    ws2_32
    # PR-1 GNS link (vendored static lib, BSD-3). bcrypt: Windows SDK provides
    # AES+SHA backend referenced by USE_CRYPTO=BCrypt. crypt32: needed for
    # cert/PKI symbols some GNS modules reference (transitive).
    GameNetworkingSockets::static
    bcrypt
    crypt32
)
target_include_directories(votv-coop PRIVATE
    third_party/GameNetworkingSockets/include)

# PR-2 (2026-05-28): /WHOLEARCHIVE removed -- the real GNS callers in
# src/coop/net/session.cpp now anchor the linker so /OPT:REF can trim unused
# GNS objects naturally. Final votv-coop.dll shrinks accordingly (the unused
# WebRTC + p2p_ice + steam_messages code paths fall away).

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
    # /EHa (asynchronous EH) -- LOAD-BEARING for the ProcessEvent crash
    # firewall (ue_wrap/game_thread.cpp). The firewall's outer SEH
    # __except absorbs any AV that propagates out of a posted task /
    # observer so the engine keeps running. Under the MSVC default /EHsc,
    # an SEH unwind does NOT run C++ destructors -- so a std::lock_guard
    # held by the faulting task (e.g. the element Registry mutex inside
    # SnapshotActorsByType) leaked LOCKED and the next game-thread tick
    # deadlocked, permanently freezing the host (diagnosed 2026-05-30 from
    # the Tier 8 4-peer smoke: host went silent the instant a client's
    # connect-edge snapshot enumeration AV'd). /EHa makes the unwind run
    # C++ destructors on the structured-exception path, so locks + RAII
    # release correctly. NOTE: this overrides CMake's default /EHsc on the
    # command line (harmless D9025 "overriding /EHsc with /EHa"; /EHa wins).
    # PRIVATE -> third-party GNS/minhook stay /EHsc (per-TU codegen flag,
    # mixing in one image is supported).
    target_compile_options(votv-coop PRIVATE /W4 /permissive- /EHa)
    # /MAP emits votv-coop.map (symbol -> RVA) so a faulting IP captured by the
    # game_thread Pump firewall (absorbed-fault localization) maps to a function
    # name by grep via tools/maprva.py -- no IDA needed. The .map lands in
    # build/ (gitignored); link-time cost is trivial.
    target_link_options(votv-coop PRIVATE /MAP)

    # MinHook must match the mod's static CRT or the link fails.
    set_property(TARGET minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Dear ImGui likewise (created before the global CMAKE_MSVC_RUNTIME_LIBRARY).
    set_property(TARGET imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    set_property(TARGET xinput1_3 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(xinput1_3 PRIVATE /W4 /permissive-)
    # Export forwarders are declared via #pragma comment(linker,"/export:...")
    # in src/loader/xinput_proxy.cpp.
endif()
