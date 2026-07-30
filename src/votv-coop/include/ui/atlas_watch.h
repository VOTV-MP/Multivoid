// ui/atlas_watch.h -- what the font atlas is doing, asserted every frame.
//
// WHY THIS EXISTS AT ALL. Until 2026-07-30 the atlas was EAGER: ui::fonts::Load
// handed ImGui an inclusion list, ImGui baked exactly that, and one boot-time
// selftest could describe the whole texture for the life of the process. ImGui
// 1.92's atlas is LAZY -- a codepoint is rasterised the first time something
// draws it, the texture grows and repacks under pressure, and bakes are
// discarded when the packer needs room. There is no longer a moment at which the
// atlas is finished, so there is no longer a moment at which a single check can
// speak for it.
//
// Three things therefore have to be watched continuously, and each has its OWN
// trigger because they fail independently:
//
//   1. THE SUPERSET INVARIANT -- every glyph that gets baked must be in the
//      repertoire. Under the old regime this held by construction (a source
//      could only be asked for what was listed). The new mechanism is
//      subtractive (GlyphExcludeRanges), so a config that misses the field, or a
//      Windows fallback face carrying scripts our embedded faces do not, bakes a
//      SUPERSET -- and the fold table says those codepoints are the sentinel, so
//      two legible names collapse to one key. Triggered by a change in a baked's
//      Glyphs.Size.
//
//   2. PACK FAILURE -- a glyph that could not fit gets IndexLookup[cp] =
//      NOT_FOUND and draws as the fallback box, silently and for the LIFE of that
//      baked (a baked drawn every frame is never discarded, so the box does not
//      heal). That is this whole saga's symptom arriving through the mechanism
//      meant to end it. It does NOT change Glyphs.Size -- the glyph is never
//      added -- so it needs its own trigger: an edge on the packer's discarded
//      surface or on the texture's UniqueID, both of which move exactly when the
//      packer has been forced to act.
//
//   3. THE SELFTEST -- per BUILD, not per Load(). Every repack mints a fresh
//      ImTextureData with a new UniqueID, so keying on that catches boot,
//      rescale, the F1 family switch AND every grow, in one integer compare per
//      frame. A flag set by Load() would be blind to precisely the builds a
//      growing atlas performs.
//
// It DETECTS; it does not PREVENT. The alternative to a superset font is no font
// at all, so the OS-fallback path logs loudly rather than refusing a source.
#pragma once

namespace ui::atlas_watch {

// Call once per frame, INSIDE the frame (after ImGui::NewFrame). In-frame is
// required, not incidental: the deliberate emoji bake the colour check performs
// must happen where baking is legal, and running out of frame is what poisoned
// ImFontAtlas::TexIsBuilt in the b132 selftest.
void OnFrame();

// The context (and therefore the atlas) is gone. Clears the per-build memo --
// ImFontAtlas::TexNextUniqueID restarts at 1 in the constructor, so a remembered
// ID would match the NEW atlas's first texture and silently skip the only check
// that proves the colour emoji rasterised.
void OnContextDestroyed();

}  // namespace ui::atlas_watch
