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
project(votv_coop CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(votv-coop SHARED
    src/bootstrap/dllmain.cpp
    src/ue_wrap/sig_scan.cpp
    src/ue_wrap/reflection.cpp
)

target_include_directories(votv-coop PRIVATE include)
set_target_properties(votv-coop PROPERTIES OUTPUT_NAME "votv-coop")

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

    set_property(TARGET xinput1_3 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(xinput1_3 PRIVATE /W4 /permissive-)
    # Export forwarders are declared via #pragma comment(linker,"/export:...")
    # in src/loader/xinput_proxy.cpp.
endif()
