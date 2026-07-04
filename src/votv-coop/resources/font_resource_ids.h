// resources/font_resource_ids.h -- RCDATA ids for the vendored Roboto TTFs
// embedded INTO the DLL (user 2026-07-04: no loose font files; the overlay is
// self-contained per RULE 3 -- a UE .pak would need the engine's pak FS to read).
// Shared by fonts.rc (the embed) and ui/fonts.cpp (the load).

#pragma once

#define IDR_FONT_ROBOTO_REGULAR 301
#define IDR_FONT_ROBOTO_BOLD    302
