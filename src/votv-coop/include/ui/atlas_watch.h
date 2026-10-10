// ui/atlas_watch.h -- what the font atlas is doing, asserted every frame.
//
// ImGui 1.92's atlas is LAZY: a codepoint is rasterised the first time something draws it, the
// texture grows and repacks under pressure, and bakes are discarded when the packer needs room, so
// no single boot-time check can speak for it. Three watches, each with its OWN trigger:
//   1. THE SUPERSET INVARIANT -- every baked glyph must be in the repertoire, or in the script
//      AllowScript names: the system face merged for the active language (ui/fonts.cpp) bakes that
//      script on purpose, and nothing else. The mechanism is subtractive (GlyphExcludeRanges), so a
//      config missing the field, or a Windows fallback face carrying scripts our embedded faces do
//      not, bakes a SUPERSET; the fold table treats those codepoints as the sentinel, and two
//      legible names collapse to one key. Triggered by a change in a baked's Glyphs.Size.
//   2. PACK FAILURE -- a glyph that could not fit gets IndexLookup[cp] = NOT_FOUND and draws the
//      fallback box for the LIFE of that baked, which never heals, because a baked drawn every
//      frame is never discarded. Glyphs.Size does not move, so the trigger is an edge on the
//      packer's discarded surface or on the texture's UniqueID.
#pragma once

#include "coop/text/repertoire.h"   // CodepointRange

#include <cstddef>
#include <cstdint>

namespace ui::atlas_watch {

// The ImFontConfig::Name of the system face ui/fonts.cpp merges for the active language's script, so
// the exclude check holds that source to its own list and every other to the generated table.
inline constexpr const char kSystemSourceName[] = "multivoid:system";

// The script ranges that system face may bake: a baked glyph inside them is expected, not a superset
// fault, and the exclude check holds the named source to the generated table united with their
// complement. Called by ui/fonts.cpp at each Load, before any face is added and on every path, so
// the selftest's script arm runs whether the face loaded or not (null when the language needs no
// script); the table is static and is kept across a context's destruction. Render thread.
void AllowScript(const coop::text::CodepointRange* ranges, size_t count);

// Call once per frame, INSIDE the frame (after ImGui::NewFrame). In-frame is required, not
// incidental: the deliberate emoji bake the colour check performs must happen where baking is
// legal, and running it out of frame poisons ImFontAtlas::TexIsBuilt.
//
// The third watch runs here: THE SELFTEST, per BUILD rather than per Load(). Every repack mints a
// fresh ImTextureData with a new UniqueID, so keying on that catches boot, rescale, the family
// switch and every grow, in one integer compare per frame; a flag set by Load() would be blind to
// exactly the builds a growing atlas performs. All three DETECT and none PREVENT -- the alternative
// to a superset font is no font at all, so the OS-fallback path logs loudly rather than refusing a
// source.
void OnFrame();

// The atlas's state for a drill on the game thread. RequestCensus asks, and the next frame the watcher
// runs answers, running the pack-failure scan over every baked for it; CensusFor is true once that
// frame ran and fills `out`. One request at a time: a drill asks again only after its answer.
struct Census {
    int      texW = 0, texH = 0;     // the texture now
    int      maxW = 0, maxH = 0;     // the ceiling it may grow to
    int      packedPx = 0;           // the packer's packed surface
    int      failures = 0;           // codepoints a source has, that we did not exclude, that did not bake
    uint32_t firstFailure = 0;
    float    chatPx = 0.f;           // the chat role's baked size, and the UI scale it came from: what the
    float    uiScale = 0.f;          // packed surface depends on, so a count measured here scales to another
};
uint32_t RequestCensus();
bool CensusFor(uint32_t request, Census* out);

// The context (and therefore the atlas) is gone. Clears the per-build memo --
// ImFontAtlas::TexNextUniqueID restarts at 1 in the constructor, so a remembered
// ID would match the NEW atlas's first texture and silently skip the only check
// that proves the colour emoji rasterised.
void OnContextDestroyed();

}  // namespace ui::atlas_watch
