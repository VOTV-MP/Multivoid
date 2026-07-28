// resources/font_resource_ids.h -- RCDATA ids for the vendored overlay TTFs
// embedded INTO the DLL (user 2026-07-04: no loose font files; the overlay is
// self-contained per RULE 3 -- a UE .pak would need the engine's pak FS to read).
// Shared by fonts.rc (the embed) and ui/fonts.cpp (the load).
//
// Families (Regular + Bold pairs; licenses in assets/fonts/LICENSE-*.txt):
//   Roboto         -- Apache 2.0
//   JetBrains Mono -- OFL 1.1
//   Cascadia Code  -- OFL 1.1
//   Fixedsys Excelsior 3.01 -- freeware (Darien Valentine); VOTV's own terminal
//     font (FSEX300 -> font_terminal). Single weight -> bold reuses Regular.
//
// Plus ONE donor, which is not a family the user can pick: the colour emoji face
// merged LAST into every baked role (arc D2). It has no bold and no UI label --
// nobody chooses to render their menu in emoji; it exists so that a name or a
// chat line containing one draws the emoji instead of the fallback box.

#pragma once

#define IDR_FONT_ROBOTO_REGULAR   301
#define IDR_FONT_ROBOTO_BOLD      302
#define IDR_FONT_JBMONO_REGULAR   303
#define IDR_FONT_JBMONO_BOLD      304
#define IDR_FONT_CASCADIA_REGULAR 305
#define IDR_FONT_CASCADIA_BOLD    306
#define IDR_FONT_FIXEDSYS_REGULAR 307
#define IDR_FONT_EMOJI_DONOR      308
