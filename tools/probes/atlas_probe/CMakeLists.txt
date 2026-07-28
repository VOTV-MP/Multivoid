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
# PROBE_192 builds the same harness against a v1.92.8 worktree of the imgui
# submodule -- that is how arm B's benefit gets MEASURED instead of read off
# the changelog. Create the worktree first:
#   git -C src/votv-coop/third_party/imgui worktree add ../../../build/imgui1928 v1.92.8
option(PROBE_192 "Build the 1.92.8 (dynamic font) probe instead of the 1.91.5 one" OFF)
if(PROBE_192)
    set(IMGUI_DIR ${REPO}/build/imgui1928)
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
