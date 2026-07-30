// ui/atlas_watch.cpp -- see ui/atlas_watch.h.

#include "ui/atlas_watch.h"

#include "coop/text/repertoire.h"
#include "ui/fonts.h"
#include "ue_wrap/core/log.h"

#include "imgui.h"
#include "imgui_internal.h"   // ImFontAtlasBuilder: the packer counters + BakedPool

namespace ui::atlas_watch {
namespace {

// imgui_draw.cpp:2663-2664 defines these as file-local macros, so they cannot be
// included. Copied WITH their meaning rather than their spelling, because the
// meaning is what the detector rests on:
//   UNUSED    -- this codepoint was never requested from this baked
//   NOT_FOUND -- it WAS requested and every source failed to produce it
// The second is written both when no source has the glyph and when the packer
// could not find room (imgui_draw.cpp, ImFontBaked_BuildLoadGlyph tail), which
// is exactly why the detector below must subtract the cases we can explain.
// Only NOT_FOUND is named here: UNUSED (0xFFFF) and every real glyph index are
// covered by "anything else", so a constant for them would be dead weight.
constexpr ImU16 kIndexNotFound = 0xFFFE;

constexpr uint32_t kEmojiProbe = 0x1F600;  // GRINNING FACE -- the donor's, and in the repertoire

// Per-BUILD memo. 0 means "no build checked yet"; ImTextureData::UniqueID counts
// from 1 (ImFontAtlas::ImFontAtlas resets TexNextUniqueID) so 0 can never be a
// real id.
int g_checkedTexId = 0;
// Geometry as last logged, so the line appears on change instead of per frame.
int      g_logW = 0, g_logH = 0;
// The packer's discarded-surface counter as last seen. -1 = never sampled.
int      g_lastDiscardedSurface = -1;
// Per-baked Glyphs.Size, keyed by ImFontBaked::BakedId. ImGuiStorage because the
// pool is unbounded and entries are reused for other (font, size) pairs.
ImGuiStorage g_glyphCount;
// Regime complaint, once per process: with the capability flag off, every check
// in this file is green by construction and would read as evidence.
bool g_warnedRegime = false;
// Rate limit for the per-frame bake-volume line (a stutter signal, not an error).
double g_lastVolumeLog = 0.0;

ImFontAtlasBuilder* Builder(ImFontAtlas* atlas) { return atlas ? atlas->Builder : nullptr; }

// ---------------------------------------------------------------------------
// 1. THE SUPERSET INVARIANT
//
// Glyphs.Size IS NOT MONOTONIC, and a high-water mark would be silently wrong.
// ImFontAtlasBuildDiscardBakes reaches ImFontBaked::ClearOutputData, which does
// Glyphs.clear() -- and it is driven from inside ImFontAtlasTextureMakeSpace,
// i.e. from the very pressure this file exists to watch. A same-font same-size
// baked therefore restarts at zero, and a remembered high-water mark would skip
// every re-baked glyph below it, forever, without a symptom. So a DECREASE is
// treated as a reset and the walk starts again from zero. Pool entries reused
// for a different (font, size) pair are covered by the same rule.
// IT ASKS ABOUT RASTERISED GLYPHS, NOT ABOUT ENTRIES IN Glyphs -- and that
// distinction is a measurement, not a nicety. Its first run reported U+0009 as an
// out-of-repertoire bake on a tree where TAB is excluded by category (it is Cc,
// hence no-ink). ImGui SYNTHESISES the tab glyph itself, in
// ImFontAtlasBuildSetupFontBakedBlanks: it copies the space glyph's advance,
// calls ImFontAtlasBakedAddFontGlyph with src == NULL, and never goes near
// ImFontAtlasBuildAcceptCodepointForSource -- so no exclude list can suppress it,
// on any config, ever.
//
// An exemption for U+0009 would be a crutch that hides the next such glyph. The
// structural truth is that a synthesised glyph occupies NO TEXTURE AREA: its
// PackId stays Invalid because nothing was packed. And the invariant this
// instrument enforces is about PIXELS -- two names must not look alike while
// folding apart -- so a glyph with no pixels cannot violate it and has no
// business being reported. Anything a SOURCE actually rasterised still is.
int ScanNewGlyphs(ImFontBaked& baked, int& outOffenders, uint32_t& outFirst) {
    const int now = baked.Glyphs.Size;
    const int was = g_glyphCount.GetInt(baked.BakedId, 0);
    if (now == was) return 0;
    const int from = (now < was) ? 0 : was;   // a decrease means it was cleared
    g_glyphCount.SetInt(baked.BakedId, now);
    for (int i = from; i < now; ++i) {
        const ImFontGlyph& g = baked.Glyphs[i];
        if (g.PackId == ImFontAtlasRectId_Invalid) continue;   // synthesised; no pixels
        if (coop::text::InRepertoire(g.Codepoint)) continue;
        if (++outOffenders == 1) outFirst = g.Codepoint;
    }
    return now - from;
}

// ---------------------------------------------------------------------------
// 2. THE PACK-FAILURE DETECTOR
//
// Three IndexLookup states are measured distinct: >= Size or UNUSED means never
// requested; NOT_FOUND means requested and every source failed; anything else is
// a real glyph index. NOT_FOUND alone proves nothing -- it is also what an
// excluded codepoint and a genuinely-absent one produce -- so the predicate has
// to subtract both explanations. What is left is "a source HAS this glyph, we
// did not forbid it, and it still did not bake", which can only be the packer.
int ScanPackFailures(ImFontBaked& baked, uint32_t& outFirst) {
    ImFont* font = baked.OwnerFont;
    if (!font) return 0;
    int found = 0;
    const int n = baked.IndexLookup.Size;
    for (int cp = 0; cp < n; ++cp) {
        if (baked.IndexLookup[cp] != kIndexNotFound) continue;
        if (coop::text::InExcludeSet(static_cast<uint32_t>(cp))) continue;   // we forbade it
        if (!font->IsGlyphInFont(static_cast<ImWchar>(cp))) continue;        // nothing has it
        if (++found == 1) outFirst = static_cast<uint32_t>(cp);
    }
    return found;
}

// ---------------------------------------------------------------------------
// 3. THE PER-BUILD SELFTEST
//
// ASSERT THE PHENOMENON, NOT THE PRECONDITION. "Did the donor resource load?"
// goes green on a build compiled without ImGuiFreeTypeLoaderFlags_LoadColor,
// where every COLR glyph bakes with Visible == 0 -- invisible, not missing, so
// the atlas is full of emoji nobody can see and every check passes.
// [[lesson-an-instrument-blind-to-the-phenomenon-always-passes]]
//
// TWO KINDS OF QUESTION, TWO APIS, and under a lazy atlas the distinction is the
// whole design of this function:
//   - "can this build DRAW cp" is a cmap fact -> ImFont::IsGlyphInFont, a pure
//     walk of Sources that never touches the atlas. Every presence check uses
//     it, because under a lazy atlas asking the ATLAS about an absent codepoint
//     simply bakes one and the check becomes green by construction. That is also
//     what lets the RED case exist at all.
//   - "did a COLOURED emoji reach the texture" is genuinely about rasterised
//     pixels, so it must read a baked glyph -- and post-flip nothing preloads,
//     so on a normal boot NOTHING has drawn an emoji before this runs. A
//     "read it only if already baked" rule would make the single instrument that
//     proves LoadColor worked permanently green-by-skip, which is
//     [[lesson-an-instrument-never-shown-failing-passes-by-construction]]
//     arriving through the front door of the commit that advertises emoji. So it
//     BAKES ONE, DELIBERATELY -- one glyph of pack area, once per build, and it
//     is the only intentional bake in this file.
void RunSelftest(ImFontAtlas* atlas, ImTextureData* tex) {
    ImFont* f = ui::fonts::FontFor(ui::fonts::Role::Nameplate);
    if (!f) { UE_LOGE("font selftest: FAIL -- no nameplate face"); return; }

    int pass = 0, total = 0;
    auto ok = [&](bool cond, const char* what) {
        ++total;
        if (cond) ++pass;
        else UE_LOGE("font selftest: FAIL -- %s", what);
    };

    ok(f->IsGlyphInFont(kEmojiProbe), "the donor supplies U+1F600 (grinning face)");

    // The cross-merge's own two claims, each a defect that shipped in b132.
    ok(f->IsGlyphInFont(0x0400),
       "U+0400 is present (JetBrains Mono lacks it; a backstop must supply it)");
    ok(f->IsGlyphInFont(0xFFFD),
       "U+FFFD is present (six of seven faces lack it; absent text fell to '?')");

    // THE RED CASE. Without one, an always-true instrument is indistinguishable
    // from a working one. U+4E00 is the first CJK ideograph and no embedded face
    // or donor carries it -- if this ever goes green, the repertoire table and
    // the fonts that shipped are describing different builds.
    ok(!f->IsGlyphInFont(0x4E00), "U+4E00 is ABSENT (the instrument can still say no)");

    // THE NEGATIVE CONTROLS -- the exclude mechanism asserted from the CONFIG
    // end, which the superset invariant cannot reach.
    //
    // That invariant only fires once something DRAWS an offending codepoint, so a
    // config nobody's text exercises is a hole in it. The two configs least
    // likely to be exercised are exactly the two no drill types into: the
    // backstop merges and the emoji donor.
    //
    // Census-derived and MEASURED 2026-07-30 against the shipped .ttf cmaps
    // (tools/text/build_repertoire.py reads the same files), not assumed:
    //   U+00AD  SOFT HYPHEN   all seven embedded faces, NOT the donor
    //   U+E0B0  Powerline     JetBrains Mono only -- reaches via MergeBackstops
    //   U+E0067 TAG LATIN g   the donor only -- and the TAG class is the one
    //                         whose index tables cost 1.04 -> 7.34 MB per face
    //                         if it ever bakes
    //
    // EACH ROW IS A CONJUNCTION AND BOTH HALVES EARN THEIR PLACE. A face must
    // still CARRY the codepoint or the probe is vacuous -- it would pass on a
    // build where the font that supplied it was dropped, proving nothing while
    // looking green. And the table must still FORBID it. IsGlyphInFont walks the
    // sources' cmaps (imgui_draw.cpp:5391) and does NOT consult
    // GlyphExcludeRanges, so the conjunction is well-formed rather than
    // self-cancelling -- measured, because if it did consult it, one half would
    // make the other unreachable.
    static const struct { uint32_t cp; const char* what; } kNegative[] = {
        {0x00AD,  "U+00AD is carried (all 7 faces) and EXCLUDED"},
        {0xE0B0,  "U+E0B0 is carried (JetBrains Mono) and EXCLUDED -- the backstop path"},
        {0xE0067, "U+E0067 is carried (the donor) and EXCLUDED -- the TAG class"},
    };
    for (const auto& p : kNegative)
        ok(f->IsGlyphInFont(static_cast<ImWchar>(p.cp)) && coop::text::InExcludeSet(p.cp),
           p.what);

    // ...and the FIELD ITSELF, per config, which is the failure the three probes
    // are NAMED for and still cannot see. A probe can only say "the TABLE forbids
    // this"; whether a given ImFontConfig ever RECEIVED the table is a different
    // fact. ui/fonts.cpp sets it in two funnels precisely so it cannot be a site
    // list, and this is the runtime half of that argument -- a fifth config added
    // later is caught here, at boot, instead of by the invariant after something
    // draws the wrong thing.
    //
    // CONTENT, NOT POINTER: ImGui ImMemdups the list into its own allocation
    // (imgui_draw.cpp:3116), so identity against ui::fonts::ExcludeList() is
    // false for every source by construction.
    //
    // AND IT COMPARES AGAINST coop::text::ExcludeRanges DIRECTLY, not against
    // ui::fonts::ExcludeList(), which returns nullptr under the
    // dev.atlas_no_exclude_drill row. Comparing against that would make this
    // check pass (NULL == NULL) in exactly the state it exists to detect --
    // [[lesson-an-instrument-never-shown-failing-passes-by-construction]] through
    // the back door of a drill knob.
    size_t nRanges = 0;
    const coop::text::CodepointRange* ranges = coop::text::ExcludeRanges(&nRanges);
    int badSources = 0, firstBad = -1;
    for (int s = 0; s < atlas->Sources.Size; ++s) {
        const ImWchar* list = atlas->Sources[s].GlyphExcludeRanges;
        bool good = (list != nullptr);
        if (good) {
            // Length first, walking the list's OWN terminator, so a short list is
            // never read past its end. Unambiguous because no value in the table
            // can be zero: the static_assert in coop/text/repertoire.cpp forbids
            // a leading U+0000 and every `end` is >= its `begin`.
            size_t len = 0;
            while (list[len] != 0) ++len;
            good = (len == nRanges * 2);
            for (size_t i = 0; good && i < nRanges; ++i)
                good = list[i * 2] == static_cast<ImWchar>(ranges[i].begin) &&
                       list[i * 2 + 1] == static_cast<ImWchar>(ranges[i].end);
        }
        if (!good && ++badSources == 1) firstBad = s;
    }
    ok(badSources == 0, "every ImFontConfig carries the generated exclude table");
    if (badSources > 0)
        UE_LOGE("font selftest:   %d of %d source(s) lack the generated exclude table, first "
                "index %d -- that config bakes its WHOLE cmap, and the fold table sentinels "
                "every codepoint of it, so two legible names can collapse to one key.",
                badSources, atlas->Sources.Size, firstBad);

    // THE ONE DELIBERATE BAKE.
    const float px = ui::fonts::PxFor(ui::fonts::Role::Nameplate);
    ImFontBaked* baked = f->GetFontBaked(px);
    const ImFontGlyph* emoji =
        baked ? baked->FindGlyphNoFallback(static_cast<ImWchar>(kEmojiProbe)) : nullptr;
    ok(emoji != nullptr, "U+1F600 bakes at the nameplate size");
    ok(emoji && emoji->Colored, "U+1F600 is flagged Colored (COLR layers loaded)");
    ok(emoji && emoji->Visible, "U+1F600 has pixels (LoadColor is on)");

    int nonGrey = 0;
    if (emoji && tex && tex->Pixels && tex->Format == ImTextureFormat_RGBA32) {
        const int w = tex->Width, h = tex->Height;
        const int x0 = int(emoji->U0 * w), x1 = int(emoji->U1 * w);
        const int y0 = int(emoji->V0 * h), y1 = int(emoji->V1 * h);
        const unsigned* px32 = reinterpret_cast<const unsigned*>(tex->Pixels);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const unsigned p = px32[y * w + x];
                const unsigned r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
                if (r != g || g != b) ++nonGrey;
            }
    }
    ok(nonGrey > 0, "U+1F600's atlas box holds non-greyscale texels (it is COLOURED)");

    // A POSITIVE line carrying its counts, and that shape is load-bearing. The
    // smoke used to assert this selftest by grepping for the ABSENCE of
    // "selftest: FAIL", which is sound only while the selftest runs
    // unconditionally at boot. It is conditional now -- it fires on a texture-id
    // edge -- so "passed" and "never ran" would produce the identical log.
    // tools/mp.py asserts the presence of this line instead.
    UE_LOGI("font selftest: DONE fail=%d (%d/%d) -- atlas %dx%d %s texid=%d, %d colour "
            "texels in one emoji (baked deliberately; every presence check is cmap-only "
            "and bakes nothing)",
            total - pass, pass, total,
            tex ? tex->Width : 0, tex ? tex->Height : 0,
            (tex && tex->Format == ImTextureFormat_RGBA32) ? "RGBA32" : "Alpha8",
            tex ? tex->UniqueID : 0, nonGrey);
}

}  // namespace

void OnContextDestroyed() {
    g_checkedTexId = 0;
    g_logW = g_logH = 0;
    g_lastDiscardedSurface = -1;
    g_glyphCount.Clear();
}

void OnFrame() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontAtlas* atlas = io.Fonts;
    if (!atlas) return;

    // REGIME FIRST. With the capability flag cleared the atlas is preloaded and
    // frozen for the frame, so every assertion below is green by construction --
    // [[lesson-an-instrument-never-shown-failing-passes-by-construction]] one
    // level up, at the precondition rather than the assertion. This is an ERROR
    // and not a WARN: a build in that state is not the build this file describes.
    if ((io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) == 0) {
        if (!g_warnedRegime) {
            g_warnedRegime = true;
            UE_LOGE("atlas watch: ImGuiBackendFlags_RendererHasTextures is CLEARED -- the "
                    "atlas is eager and every check in this file passes by construction. "
                    "Treat any 'font selftest: DONE' from this run as unproven.");
        }
        return;
    }

    ImTextureData* tex = atlas->TexData;
    ImFontAtlasBuilder* b = Builder(atlas);

    // Geometry, logged ON CHANGE. This replaces the boot-time "atlas baked in
    // %.1f ms (%dx%d)" line the eager Build() used to emit: there is no longer a
    // single bake to time, so the honest numbers are the geometry when it moves
    // plus the per-frame glyph delta below. Shipping the flip without this would
    // leave a build whose atlas nobody can read.
    if (tex && (tex->Width != g_logW || tex->Height != g_logH)) {
        g_logW = tex->Width;
        g_logH = tex->Height;
        UE_LOGI("fonts: atlas geometry %dx%d %s (texid=%d, packed %d px, discarded %d px, "
                "%d bakeds) -- lazy atlas, so this line moves instead of a boot bake time",
                tex->Width, tex->Height,
                tex->Format == ImTextureFormat_RGBA32 ? "RGBA32" : "Alpha8", tex->UniqueID,
                b ? b->RectsPackedSurface : 0, b ? b->RectsDiscardedSurface : 0,
                b ? b->BakedPool.Size : 0);
    }

    // The superset invariant, over every live baked. O(new glyphs): the walk only
    // covers the range that changed, and most frames change nothing.
    int newGlyphs = 0, offenders = 0;
    uint32_t firstOffender = 0;
    if (b)
        for (int i = 0; i < b->BakedPool.Size; ++i)
            newGlyphs += ScanNewGlyphs(b->BakedPool[i], offenders, firstOffender);
    if (offenders > 0)
        UE_LOGE("atlas watch: %d glyph(s) baked OUTSIDE the repertoire, first U+%04X. The "
                "fold table maps those to the sentinel, so two legible names can collapse "
                "to one key. A font source is missing GlyphExcludeRanges, or an OS fallback "
                "face carries scripts our embedded families do not.",
                offenders, firstOffender);

    // Bake volume, rate-limited. Not an error -- the signal to look at first if a
    // hands-on reports a stutter while text with fresh codepoints appears.
    if (newGlyphs > 64) {
        const double now = ImGui::GetTime();
        if (now - g_lastVolumeLog > 5.0) {
            g_lastVolumeLog = now;
            UE_LOGI("fonts: %d glyphs rasterised in one frame (atlas %dx%d)", newGlyphs,
                    tex ? tex->Width : 0, tex ? tex->Height : 0);
        }
    }

    // The pack-failure detector, on ITS OWN trigger. A pack failure never adds a
    // glyph, so the superset scan above is structurally blind to it; what does
    // move is the packer's discarded surface (MakeSpace had to free room) or the
    // texture id (it had to grow). Scanning on that edge keeps the O(IndexLookup)
    // walk off every frame.
    const int discarded = b ? b->RectsDiscardedSurface : 0;
    const bool pressure = b && (discarded != g_lastDiscardedSurface);
    const bool rebuilt  = tex && (tex->UniqueID != g_checkedTexId);
    if (b && (pressure || rebuilt)) {
        const int was = g_lastDiscardedSurface;
        g_lastDiscardedSurface = discarded;
        int failures = 0;
        uint32_t firstFailure = 0;
        for (int i = 0; i < b->BakedPool.Size; ++i)
            failures += ScanPackFailures(b->BakedPool[i], firstFailure);
        if (failures > 0)
            UE_LOGE("atlas watch: %d codepoint(s) a source HAS and we did NOT exclude failed "
                    "to bake, first U+%04X -- the packer ran out of room. They draw as the "
                    "fallback box for the LIFE of that baked (a baked drawn every frame is "
                    "never discarded), which is the exact symptom this atlas work exists to "
                    "delete. atlas %dx%d, packed %d px, discarded %d px (was %d).",
                    failures, firstFailure, tex ? tex->Width : 0, tex ? tex->Height : 0,
                    b->RectsPackedSurface, discarded, was);
    }

    // The selftest, per BUILD. Every repack mints a fresh ImTextureData with a
    // new UniqueID, so this catches boot, rescale, the F1 family switch and every
    // grow -- in one integer compare per frame.
    if (tex && tex->UniqueID != g_checkedTexId) {
        g_checkedTexId = tex->UniqueID;
        RunSelftest(atlas, tex);
    }
}

}  // namespace ui::atlas_watch
