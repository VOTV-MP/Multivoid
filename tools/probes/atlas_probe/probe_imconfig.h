// probe_imconfig.h -- IMGUI_USER_CONFIG for the 1.92 probe arms.
//
// WHY: the shipping mod is built Release, so NDEBUG makes ImGui's default
// IM_ASSERT (== assert()) a NO-OP. Every IM_ASSERT_USER_ERROR in 1.92 --
// including the two the atlas raises when a legacy backend meets a demand
// bake (imgui_draw.cpp:2814-2817) -- is therefore invisible in the DLL we
// ship, and continues into whatever state follows. A probe that ABORTS on
// those asserts would measure a regime we do not ship.
//
// So: record and keep going. Same non-fatal behaviour as our Release build,
// but the message is captured instead of discarded.
#pragma once

void ProbeAssertRecord(const char* expr, const char* file, int line);

#define IM_ASSERT(_EXPR) \
    do { if (!(_EXPR)) ProbeAssertRecord(#_EXPR, __FILE__, __LINE__); } while (0)
