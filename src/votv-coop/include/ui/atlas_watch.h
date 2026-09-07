// ui/atlas_watch.h -- what the font atlas is doing, asserted every frame.
//
// ImGui 1.92's atlas is LAZY: a codepoint is rasterised the first time something draws it, the
// texture grows and repacks under pressure, and bakes are discarded when the packer needs room.
// Nothing ever finishes it, so no single boot-time check can speak for it. Three things are watched
// continuously, each with its OWN trigger, because they fail independently.
//   1. THE SUPERSET INVARIANT -- every baked glyph must be in the repertoire. The mechanism is
//      subtractive (GlyphExcludeRanges), so a config missing the field, or a Windows fallback face
//      carrying scripts our embedded faces do not, bakes a SUPERSET; the fold table treats those
//      codepoints as the sentinel, and two legible names collapse to one key. Triggered by a change
//      in a baked's Glyphs.Size.
//   2. PACK FAILURE -- a glyph that could not fit gets IndexLookup[cp] = NOT_FOUND and draws the
//      fallback box for the LIFE of that baked, which never heals, because a baked drawn every
//      frame is never discarded. Glyphs.Size does not move, so the trigger is an edge on the
//      packer's discarded surface or on the texture's UniqueID.
#pragma once

namespace ui::atlas_watch {

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

// The context (and therefore the atlas) is gone. Clears the per-build memo --
// ImFontAtlas::TexNextUniqueID restarts at 1 in the constructor, so a remembered
// ID would match the NEW atlas's first texture and silently skip the only check
// that proves the colour emoji rasterised.
void OnContextDestroyed();

}  // namespace ui::atlas_watch
