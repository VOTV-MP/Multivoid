// ui/atlas_watch.cpp -- see ui/atlas_watch.h.

#include "ui/atlas_watch.h"

#include "coop/text/repertoire.h"
#include "ui/fonts.h"
#include "ue_wrap/core/log.h"

#include "imgui.h"
#include "imgui_internal.h"   // ImFontAtlasBuilder: the packer counters + BakedPool

namespace ui::atlas_watch {
namespace {

// ImGui defines these as file-local macros, so they cannot be included; copied with their
// meaning: UNUSED means the codepoint was never requested from this baked, NOT_FOUND that it
// was requested and every source failed to produce it. The second is written both when no
// source has the glyph and when the packer could not find room, which is why the detector
// below must subtract the cases it can explain. Only NOT_FOUND is named: UNUSED and every real
// glyph index are covered by anything-else.
constexpr ImU16 kIndexNotFound = 0xFFFE;

constexpr uint32_t kEmojiProbe = 0x1F600;  // GRINNING FACE -- the donor's, and in the repertoire

// The per-build memo. 0 means no build checked yet; texture ids count from 1, so 0 can never
// be a real id.
int g_checkedTexId = 0;
// Geometry as last logged, so the line appears on change instead of per frame.
int      g_logW = 0, g_logH = 0;
// The packer's discarded-surface counter as last seen; -1 means never sampled.
int      g_lastDiscardedSurface = -1;
// The per-baked glyph count, keyed by baked id; ImGui storage because the pool is unbounded
// and entries are reused for other font and size pairs.
ImGuiStorage g_glyphCount;
// The regime complaint, once per process: with the capability flag off, every check in this
// file is green by construction and would read as evidence.
bool g_warnedRegime = false;
// The rate limit for the per-frame bake-volume line, a stutter signal rather than an error.
double g_lastVolumeLog = 0.0;

ImFontAtlasBuilder* Builder(ImFontAtlas* atlas) { return atlas ? atlas->Builder : nullptr; }

// The superset invariant. The glyph count is not monotonic, and a high-water mark would be
// silently wrong: discarding bakes clears a baked's glyphs, and that is driven from inside the
// atlas's make-space, the very pressure this file watches. A same-font same-size baked
// therefore restarts at zero, and a remembered high-water mark would skip every re-baked glyph
// below it, forever, without a symptom; so a decrease is treated as a reset and the walk
// starts again from zero, and pool entries reused for a different font and size pair are
// covered by the same rule. It asks about rasterised glyphs, not about entries in the glyph
// list, and that distinction is a measurement: the first run reported TAB as an
// out-of-repertoire bake on a tree where TAB is excluded by category. ImGui synthesises the
// tab glyph itself, copying the space glyph's advance with no source, and never goes near the
// source accept check, so no exclude list can suppress it; an exemption for TAB would hide the
// next such glyph. A synthesised glyph occupies no texture area (its pack id stays invalid),
// and the invariant is about pixels (two names must not look alike while folding apart), so a
// glyph with no pixels cannot violate it. Anything a source actually rasterised still is.
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

// The pack-failure detector. Three lookup states are measured distinct: beyond the table or
// UNUSED means never requested; NOT_FOUND means requested and every source failed; anything
// else is a real glyph index. NOT_FOUND alone proves nothing, since an excluded codepoint and
// a genuinely absent one produce it too, so the predicate subtracts both explanations. What is
// left is a glyph a source has, that we did not forbid, and that still did not bake, which can
// only be the packer.
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

// The per-build selftest. Assert the phenomenon, not the precondition: "did the donor
// resource load" goes green on a build compiled without colour loading, where every colour
// glyph bakes invisible rather than missing, so the atlas is full of emoji nobody can see and
// every check passes. Two kinds of question, two APIs, and under a lazy atlas the distinction
// is the whole design: whether this build can draw a codepoint is a cmap fact, a pure walk of
// the sources that never touches the atlas, and every presence check uses it, because asking
// the atlas about an absent codepoint simply bakes one and the check becomes green by
// construction (that is also what lets the red case exist); whether a coloured emoji reached
// the texture is about rasterised pixels, so it must read a baked glyph, and nothing preloads,
// so on a normal boot nothing has drawn an emoji before this runs. A read-only-if-already-baked
// rule would make the single instrument that proves colour loading permanently green by skip.
// So it bakes one, deliberately: one glyph of pack area, once per build, the only intentional
// bake in this file.
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

    // The cross-merge's own two claims, each a defect that once shipped.
    ok(f->IsGlyphInFont(0x0400),
       "U+0400 is present (JetBrains Mono lacks it; a backstop must supply it)");
    ok(f->IsGlyphInFont(0xFFFD),
       "U+FFFD is present (six of seven faces lack it; absent text fell to '?')");

    // The red case. Without one, an always-true instrument is indistinguishable from a working
    // one. U+4E00 is the first CJK ideograph and no embedded face or donor carries it; if this
    // ever goes green, the repertoire table and the fonts that shipped describe different builds.
    ok(!f->IsGlyphInFont(0x4E00), "U+4E00 is ABSENT (the instrument can still say no)");

    // The negative controls: the exclude mechanism asserted from the config end, which the
    // superset invariant cannot reach, since it only fires once something draws an offending
    // codepoint, and a config nobody's text exercises is a hole in it. The two configs least
    // likely to be exercised are the two no drill types into: the backstop merges and the emoji
    // donor. Census-derived and measured against the shipped font cmaps (the repertoire generator
    // reads the same files): U+00AD, the soft hyphen, is in all seven embedded faces and not the
    // donor; U+E0B0, a Powerline glyph, is in JetBrains Mono only and reaches through the backstop
    // merge; U+E0067, a tag character, is in the donor only, and the tag class is the one whose
    // index tables cost megabytes per face if it ever bakes. Each row is a conjunction and both
    // halves earn their place: a face must still carry the codepoint or the probe is vacuous (it
    // would pass on a build where the supplying font was dropped), and the table must still forbid
    // it. The presence check walks the sources' cmaps and does not consult the exclude ranges,
    // measured, so the conjunction is well-formed rather than self-cancelling.
    static const struct { uint32_t cp; const char* what; } kNegative[] = {
        {0x00AD,  "U+00AD is carried (all 7 faces) and EXCLUDED"},
        {0xE0B0,  "U+E0B0 is carried (JetBrains Mono) and EXCLUDED -- the backstop path"},
        {0xE0067, "U+E0067 is carried (the donor) and EXCLUDED -- the TAG class"},
    };
    for (const auto& p : kNegative)
        ok(f->IsGlyphInFont(static_cast<ImWchar>(p.cp)) && coop::text::InExcludeSet(p.cp),
           p.what);

    // ...and the field itself, per config, the failure the three probes are named for and still
    // cannot see: a probe can only say the table forbids this, while whether a given font config
    // ever received the table is a different fact. The fonts module sets it in two funnels
    // precisely so it cannot be a site list, and this is the runtime half of that argument: a
    // config added later is caught here, at boot, instead of by the invariant after something
    // draws the wrong thing. Content, not pointer: ImGui copies the list into its own allocation,
    // so identity against the fonts module's list is false for every source by construction. And
    // it compares against the text module's ranges directly, not against the fonts module's
    // accessor, which returns null under the no-exclude drill row; comparing against that would
    // pass in exactly the state this exists to detect.
    size_t nRanges = 0;
    const coop::text::CodepointRange* ranges = coop::text::ExcludeRanges(&nRanges);
    int badSources = 0, firstBad = -1;
    for (int s = 0; s < atlas->Sources.Size; ++s) {
        const ImWchar* list = atlas->Sources[s].GlyphExcludeRanges;
        bool good = (list != nullptr);
        if (good) {
            // Length first, walking the list's own terminator, so a short list is never read past
            // its end. Unambiguous because no value in the table can be zero: the repertoire's
            // static assertion forbids a leading U+0000 and every end is at least its begin.
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

    // The one deliberate bake.
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

    // A positive line carrying its counts, and that shape is load-bearing: the smoke asserted this
    // selftest by grepping for the absence of a failure line, which is sound only while the
    // selftest runs unconditionally at boot. It is conditional now, fired on a texture-id edge, so
    // passed and never-ran would produce the identical log; the smoke driver asserts the presence
    // of this line instead.
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

    // The regime first. With the capability flag cleared the atlas is preloaded and frozen for the
    // frame, so every assertion below is green by construction, at the precondition rather than
    // the assertion. An error, not a warning: a build in that state is not the build this file
    // describes.
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

    // Geometry, logged on change. There is no single bake to time under a lazy atlas, so the
    // honest numbers are the geometry when it moves plus the per-frame glyph delta below; without
    // this line nobody could read the atlas of a shipped build.
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

    // The superset invariant, over every live baked. Linear in new glyphs: the walk only covers
    // the range that changed, and most frames change nothing.
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

    // Bake volume, rate-limited. Not an error: the signal to look at first if a hands-on reports a
    // stutter while text with fresh codepoints appears.
    if (newGlyphs > 64) {
        const double now = ImGui::GetTime();
        if (now - g_lastVolumeLog > 5.0) {
            g_lastVolumeLog = now;
            UE_LOGI("fonts: %d glyphs rasterised in one frame (atlas %dx%d)", newGlyphs,
                    tex ? tex->Width : 0, tex ? tex->Height : 0);
        }
    }

    // The pack-failure detector, on its own trigger. A pack failure never adds a glyph, so the
    // superset scan above is structurally blind to it; what moves is the packer's discarded
    // surface (make-space had to free room) or the texture id (it had to grow). Scanning on that
    // edge keeps the lookup-table walk off every frame.
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

    // The selftest, per build. Every repack mints a fresh texture with a new id, so this catches
    // boot, rescale, the font family switch and every grow, in one integer compare per frame.
    if (tex && tex->UniqueID != g_checkedTexId) {
        g_checkedTexId = tex->UniqueID;
        RunSelftest(atlas, tex);
    }
}

}  // namespace ui::atlas_watch
