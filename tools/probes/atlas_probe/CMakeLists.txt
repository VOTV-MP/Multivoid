# atlas_probe -- standalone dev harness for the arc-D atlas gate (M1a).
#
# Deliberately does NOT reuse src/votv-coop/CMakeLists.txt: that project also
# configures GNS + protobuf + opus, and this probe needs only imgui + freetype.
# Building it can therefore never disturb the shipping build tree.
#
#   cmake -S tools/probes/atlas_probe -B build/atlas_probe16 -DWCHAR32=OFF
#   cmake -S tools/probes/atlas_probe -B build/atlas_probe32 -DWCHAR32=ON
#   cmake --build build/atlas_probeNN --config Release
cmake_minimum_required(VERSION 3.20)
project(atlas_probe CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(WCHAR32 "Compile imgui with IMGUI_USE_WCHAR32 (32-bit ImWchar)" OFF)

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
# PROBE_192 builds the same harness against a v1.92.9 worktree of the imgui
# submodule -- that is how the benefit gets MEASURED instead of read off the
# changelog. 1.92.9 is the version the upgrade design measured the port against
# (votv-imgui-192-upgrade-DESIGN-2026-07-30.md section 2); the older 1.92.8
# worktree this option used to point at is retired. Create the worktree first:
#   git -C src/votv-coop/third_party/imgui worktree add ../../../build/imgui1929 v1.92.9
option(PROBE_192 "Build the 1.92.9 (dynamic font) probe instead of the 1.91.5 one" OFF)
if(PROBE_192)
    set(IMGUI_DIR ${REPO}/build/imgui1929)
else()
    set(IMGUI_DIR ${REPO}/src/votv-coop/third_party/imgui)
endif()

# Same FreeType configuration the mod ships with (src/votv-coop/CMakeLists.txt:78-82)
# -- FT_DISABLE_PNG is why the emoji donor must be COLR/CPAL, not CBDT.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(FT_DISABLE_ZLIB     ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON  CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL    ON  CACHE BOOL "" FORCE)
add_subdirectory(${REPO}/src/votv-coop/third_party/freetype freetype-build EXCLUDE_FROM_ALL)

add_library(imgui_probe STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
)
target_include_directories(imgui_probe PUBLIC ${IMGUI_DIR})
target_compile_definitions(imgui_probe PUBLIC IMGUI_ENABLE_FREETYPE
    IMGUI_DISABLE_WIN32_FUNCTIONS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
if(PROBE_192)
    # ARM L needs to SEE the IM_ASSERT_USER_ERRORs a legacy backend provokes
    # without dying on them -- our Release DLL does not die on them either
    # (NDEBUG strips assert). probe_imconfig.h records instead of aborting.
    # The include dir also carries the mod's generated repertoire table, so the
    # arm bakes the real ranges rather than an approximation of them.
    target_compile_definitions(imgui_probe PUBLIC IMGUI_USER_CONFIG="probe_imconfig.h")
    target_include_directories(imgui_probe PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR} ${REPO}/src/votv-coop/include)
endif()
if(WCHAR32)
    target_compile_definitions(imgui_probe PUBLIC IMGUI_USE_WCHAR32)
endif()
target_link_libraries(imgui_probe PUBLIC freetype)

if(PROBE_192)
    add_executable(atlas_probe atlas_probe_192.cpp)
else()
    add_executable(atlas_probe atlas_probe.cpp)
endif()
target_link_libraries(atlas_probe PRIVATE imgui_probe)
