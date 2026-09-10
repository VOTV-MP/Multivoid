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
project(votv_coop LANGUAGES CXX C ASM_MASM)  # C for the MinHook sources (third_party/minhook);
                                             # ASM_MASM for the UE4SS cppmod no-op vtable stubs
                                             # (src/loader/cppmod_stubs.asm, D-3 spike 2026-08-21)

# The VOTV game version this build targets -- ONE half of the mod's version
# identity (the other half is kProtocolVersion, the build number; see
# include/coop/version.h.in). Reflection offsets and BP field names shift
# between cooks, so the mod is incompatible across game versions. Update on
# every game-recook adaptation. (The old project-VERSION mod semver was
# retired 2026-07-19 -- Paper-Minecraft pair, user decision.)
set(VOTVCOOP_GAME_TARGET "0.9.0n")

# The BUILD NUMBER (the identity's other half) is kProtocolVersion -- parsed
# out of protocol.h so the Paper-style pair lands in the generated
# coop/version.h and the VERSIONINFO resource (the filename no longer carries
# it -- the OUTPUT_NAME is the UE4SS contract name main.dll, see below). The
# configure depends on protocol.h so a proto bump re-stamps both automatically.
set(VOTVCOOP_PROTOCOL_H ${CMAKE_CURRENT_SOURCE_DIR}/include/coop/net/protocol.h)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${VOTVCOOP_PROTOCOL_H})
file(READ ${VOTVCOOP_PROTOCOL_H} _votvcoop_proto_h)
string(REGEX MATCH "kProtocolVersion = ([0-9]+)" _votvcoop_proto_m "${_votvcoop_proto_h}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "could not parse kProtocolVersion out of protocol.h")
endif()
set(VOTVCOOP_BUILD ${CMAKE_MATCH_1})
message(STATUS "multivoid identity: game ${VOTVCOOP_GAME_TARGET}, build ${VOTVCOOP_BUILD}")

# Materialize VOTVCOOP_GAME_TARGET into a generated header that the DLL
# includes (boot banner + the announce/gate identity).
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

# Dear ImGui (MIT, vendored as git submodule, pinned v1.92.9 since 2026-07-30 --
# 1.92's ImFont/ImFontBaked split IS the dynamic atlas; see
# research/findings/tooling/votv-imgui-192-upgrade-DESIGN-2026-07-30.md) -- OUR OWN copy for
# the in-game dev menu + (future) main-menu MP server browser. NEVER UE4SS's ImGui
# (RULE 3, no UE4SS at runtime). Compiled into our own static target so it picks up
# the same static-CRT as the mod (like minhook). Backends: DX11 + DX12 (we
# auto-detect the RHI at the swapchain) + Win32 (input). Source in third_party/imgui.
# Overridable so an upgrade can be PRICED by the compiler rather than by grep:
# point it at a worktree of another tag and build (the arc-D gate did exactly this
# for 1.92.8 -- see research/findings/join-identity/votv-arc-d-gate-measurements-2026-07-28.md).
# Unset = the pinned submodule, which is what ships.
if(NOT IMGUI_DIR)
    set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
endif()

# FreeType (FTL license, vendored 2.13.3) -- the imgui_freetype atlas rasterizer
# (proper hinting; visibly sharper than stb_truetype at 16-18 px UI sizes, user
# 2026-07-04). All optional deps OFF/internal so the build stays self-contained
# (RULE 3: zero loose runtime files, zero system dev packages).
# FreeType's own cmake_minimum_required(3.0...3.5) resets the policy scope, so
# CMP0091 (MSVC_RUNTIME_LIBRARY) would record as OLD on its targets and the
# static-CRT property below would be silently ignored (audit WARN-1: Release
# was /MT only by cl's no-flag default; Debug would mix /MT into /MTd). Force
# NEW for subdirectories so the property is honored deterministically.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(FT_DISABLE_ZLIB     ON  CACHE BOOL "" FORCE)  # internal zlib copy
set(FT_DISABLE_BZIP2    ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON  CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL    ON  CACHE BOOL "" FORCE)
add_subdirectory(third_party/freetype EXCLUDE_FROM_ALL)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
# IMGUI_ENABLE_FREETYPE makes FreeType the DEFAULT atlas builder (since 1.81) --
# every font in the atlas (chat, panels, nameplates) rasterizes through it.
# IMGUI_USE_WCHAR32 widens ImWchar to 32 bits (imgui.h:273) and raises
# IM_UNICODE_CODEPOINT_MAX from 0xFFFF to 0x10FFFF (imgui.h:2515). REQUIRED, not
# a preference: 1,232 of the emoji donor's 1,418 codepoints are above the BMP, so
# on the default 16-bit build a glyph RANGE cannot even express U+1F600 and
# imgui.cpp:1512 discards the codepoint AddInputCharacterUTF16 reassembles from a
# surrogate pair -- an emoji could be neither baked nor typed.
#
# IMGUI_DISABLE_DEFAULT_FONT_VECTOR drops the SECOND default font 1.92 embeds
# beside ProggyClean -- 14,562 bytes of compressed font data
# (imgui_draw.cpp:6560). Safe because ui/fonts.cpp's last-resort path names
# AddFontDefaultBitmap() explicitly rather than letting AddFontDefault's
# size heuristic (imgui_draw.cpp:3180-3186) choose the vector one.
#
# They ride HERE and not in imconfig.h because third_party/imgui is a git
# submodule (the superproject gitlink pins it; v1.92.9 as of 2026-07-30):
# editing the header is a dirty submodule no fresh clone reproduces.
# IMGUI_ENABLE_FREETYPE below is the same
# pattern, and ODR is safe because votv-coop is this target's only consumer.
target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_FREETYPE IMGUI_USE_WCHAR32
    IMGUI_DISABLE_DEFAULT_FONT_VECTOR)
target_link_libraries(imgui PUBLIC freetype)

# --- GameNetworkingSockets (BSD-3, vendored as git submodule, pinned v1.5.1) ---
#
# Wire-layer substrate for the 4-PR GNS migration plan in
# research/findings/network/votv-gns-integration-plan-2026-05-27.md +
# research/findings/network/votv-gns-sandbox-pass-2026-05-28.md (sandbox PASS report).
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
# ENABLE_ICE compiles GNS's STUN/TURN/ICE client (P2P NAT-punch path);
# USE_STEAMWEBRTC additionally builds Google's libwebrtc-lite (the ICE backend)
# from the vendored 2020 snapshot. Enabled for the zero-open-ports P2P
# connectivity ladder (research/findings/
# votv-zero-ports-connectivity-ladder-design-2026-06-05.md).
#
# Stage-0 build spike (2026-06-05) proved the webrtc-from-source build links
# /MT clean on MSVC 14.50 (abseil, OpenSSL 3.x adapters, /EHs-c-, 138 TUs,
# GameNetworkingSockets.dll + example_chat.exe linked + ran). The one blocker
# was a two-abseil CMake ALIAS collision: vcpkg's MODERN protobuf pulls a modern
# abseil whose target names collide with the 2020 abseil webrtc-lite needs.
# FIX (vcpkg.json alongside this file): pin protobuf to 3.21.12, the last
# pre-abseil release -> zero modern abseil -> only the 2020 abseil exists ->
# no collision, and add_subdirectory(GNS) below still works as-is.
set(ENABLE_ICE               ON  CACHE BOOL   "" FORCE)
set(USE_STEAMWEBRTC          ON  CACHE BOOL   "" FORCE)

# Static-CRT for GNS targets. Note: GNS's own MSVC_CRT_STATIC option does
# `string(REPLACE "/MD" "/MT")` on CMAKE_*_FLAGS, which is a no-op under
# CMP0091=NEW (default since CMake 3.15) where the runtime is a TARGET
# property, not part of the flag string. We set the global property here
# BEFORE add_subdirectory so every GNS target inherits MultiThreaded /MT.
# (Our own targets ALSO set this property explicitly below for clarity.)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(third_party/GameNetworkingSockets EXCLUDE_FROM_ALL)

# --- voice chat substrate (v66; votv-voice-chat-port-design-2026-06-12.md) ---
# libopus (BSD-3, vendored shallow clone pinned v1.5.2) -- the voice codec
# (the same lib Simple Voice Chat wraps via opus4j). Own CMake; inherits the
# global /MT runtime property set above.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING        OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS      OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)

# miniaudio (public-domain/MIT-0 dual, vendored single header pinned 0.11.22)
# -- capture + playback devices + enumeration (WASAPI underneath), replacing
# SVC's javax.sound/OpenAL stack. One C TU (minhook pattern: our own static
# target on the same CRT). Spatialization is OURS (the SVC REDUCED-mode pan),
# so no 3D-audio engine features are compiled in.
add_library(miniaudio STATIC third_party/miniaudio/miniaudio.c)
target_include_directories(miniaudio PUBLIC third_party/miniaudio)

# nlohmann/json (MIT, header-only, via the vcpkg manifest) -- parses the master
# server's JSON (lobby list + host/join responses) in coop/net/lobby_*. RULE 13:
# don't hand-roll a JSON parser for untrusted network input.
find_package(nlohmann_json CONFIG REQUIRED)

# The translation units live in their own list (see sources.cmake for why).
include(${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake)

add_library(votv-coop SHARED ${VOTVCOOP_SOURCES})

target_include_directories(votv-coop PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/generated)
# The UE4SS mod-folder contract name (UE4SS_ARC WP-2 commit 3): the DLL ships
# as Mods/Multivoid/dlls/main.dll. The versioned multivoid-<game>-<build>.dll
# OUTPUT_NAME retired WITH its one consumer, the xinput proxy's scan -- the
# old "load-bearing, not cosmetic" justification was the proxy's sentence
# (UE4SS_ARC 7.3a item 2). The release identity lives in the zip name + the
# generated manifest written at packaging time, the boot banner
# (coop/version.h + kProtocolVersion), and -- for the bytes themselves -- the
# VERSIONINFO resource below, which deploy-mod.ps1 verifies against the tree.
set_target_properties(votv-coop PROPERTIES OUTPUT_NAME "main")

# Identity IN the bytes: a generated VERSIONINFO resource carrying the Paper
# pair (ProductVersion "<game> b<build>"). Closes the recorded gap "the DLL
# carries no VERSIONINFO resource -- nothing out-of-band identifies a built or
# installed DLL today" (UE4SS_ARC 7.3a), which started to matter exactly when
# the filename stopped carrying the identity. The numeric FILEVERSION carries
# the digits (<major>,<minor>,<build>,0); the letter suffix of the game target
# ("0.9.0n") fits only the string fields, which are the authoritative ones.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" _votvcoop_tgt_mm "${VOTVCOOP_GAME_TARGET}")
if("${_votvcoop_tgt_mm}" STREQUAL "")
    # Fail closed like the kProtocolVersion parse above: on a failed match CMake
    # KEEPS the previous CMAKE_MATCH_* values (documented sharp edge), so without
    # this guard a non-numeric target would silently stamp a stale quad. Tested
    # on the WHOLE match via STREQUAL -- `if(NOT CMAKE_MATCH_1)` is wrong here
    # because a legitimate major of "0" is CMake-falsy (measured: it refused the
    # real "0.9.0n" target).
    message(FATAL_ERROR "could not parse major.minor out of VOTVCOOP_GAME_TARGET '${VOTVCOOP_GAME_TARGET}'")
endif()
set(VOTVCOOP_TARGET_MAJOR "${CMAKE_MATCH_1}")
set(VOTVCOOP_TARGET_MINOR "${CMAKE_MATCH_2}")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc
    @ONLY)
target_sources(votv-coop PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc)
# version.lib: read the running exe's file version in the health check.
# minhook: our inline-hook substrate (game-thread dispatcher).
# ws2_32: Winsock for the UDP coop transport (src/coop/net).
target_link_libraries(votv-coop PRIVATE
    version
    minhook
    imgui
    nlohmann_json::nlohmann_json   # header-only; master-server JSON (coop/net/lobby_*)
    d3d11    # dummy-swapchain creation to read the IDXGISwapChain vtable (ImGui present hook)
    dxgi
    gdiplus
    ws2_32
    winhttp  # master-server HTTP client (coop/net/http_client)
    # PR-1 GNS link (vendored static lib, BSD-3). bcrypt: Windows SDK provides
    # AES+SHA backend referenced by USE_CRYPTO=BCrypt. crypt32: needed for
    # cert/PKI symbols some GNS modules reference (transitive).
    GameNetworkingSockets::static
    bcrypt
    crypt32
    opus       # v66 voice codec (third_party/opus, static)
    miniaudio  # v66 capture/playback devices (third_party/miniaudio, static)
)
target_include_directories(votv-coop PRIVATE
    third_party/GameNetworkingSockets/include)

# PR-2 (2026-05-28): /WHOLEARCHIVE removed -- the real GNS callers in
# src/coop/net/session.cpp now anchor the linker so /OPT:REF can trim unused
# GNS objects naturally. Final votv-coop.dll shrinks accordingly (the unused
# WebRTC + p2p_ice + steam_messages code paths fall away).

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
    # /utf-8: source AND execution charset are UTF-8. Without it MSVC decodes a
    # non-ASCII source literal with the SYSTEM codepage, so `L"中文"` written
    # literally compiles to a different string on a Russian machine than on an
    # English one -- caught by the arc-D codec selftest, which counted 3 codepoints
    # in a 2-character literal. Silent, machine-dependent, and now impossible.
    target_compile_options(votv-coop PRIVATE /W4 /permissive- /EHa /utf-8)
    # /MAP emits main.map (symbol -> RVA) so a faulting IP captured by the
    # game_thread Pump firewall (absorbed-fault localization) maps to a function
    # name by grepping the linker map -- no IDA needed. The .map lands in
    # build/ (gitignored); link-time cost is trivial.
    target_link_options(votv-coop PRIVATE /MAP)

    # MinHook must match the mod's static CRT or the link fails.
    set_property(TARGET minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Dear ImGui likewise (created before the global CMAKE_MSVC_RUNTIME_LIBRARY).
    set_property(TARGET imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # FreeType (the imgui_freetype rasterizer) must match the static CRT too.
    set_property(TARGET freetype PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()