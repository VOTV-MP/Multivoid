// atlas_probe -- M1a of the arc-D gate (research/findings/join-identity/
// votv-nickname-arbitration-roster-id-DESIGN-2026-07-27.md section 9b.7 item 1).
//
// Bakes the SAME atlas ui::fonts::Load() bakes -- same families, same per-role px,
// same (family, px, bold) dedup -- across a repertoire x scale x family-config
// matrix, headless, and reports what the design needs to decide arm A vs arm B:
//
//   texture W x H and bytes, whether ImFontAtlas::Build() SUCCEEDS, bake ms, glyph
//   count, and the per-ImFont index-table cost (which is what IMGUI_USE_WCHAR32
//   actually inflates: ImFont::IndexAdvanceX/IndexLookup are sized to the MAX
//   codepoint present -- imgui_draw.cpp:3656-3669 GrowIndex(max_codepoint + 1)).
//
// DEV TOOL. Never linked into the mod (RULE 3 / probes are RULE-2 exempt).
// Build twice -- once with -DWCHAR32=OFF, once ON -- to price the define.

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/freetype/imgui_freetype.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---- mirrored from src/votv-coop/include/ui/fonts.h + src/ui/fonts.cpp -------
constexpr float kUiPx        = 16.f;
constexpr float kChatPx      = 18.f;
constexpr float kNameplatePx = 16.f;

struct FamilyDesc { const char* label; const char* regular; const char* bold; };
struct RoleDesc   { const char* label; float basePx; bool bold; };

// Same order as ui::fonts::Family.
const FamilyDesc kFamilies[] = {
    { "JetBrains Mono",  "JetBrainsMono-Regular.ttf", "JetBrainsMono-Bold.ttf" },
    { "Roboto",          "Roboto-Regular.ttf",        "Roboto-Bold.ttf"        },
    { "Cascadia Code",   "CascadiaCode-Regular.ttf",  "CascadiaCode-Bold.ttf"  },
    { "Fixedsys (VOTV)", "FSEX300.ttf",               "FSEX300.ttf"            },
};
const int kFamilyCount = int(sizeof(kFamilies) / sizeof(kFamilies[0]));
// Same order as ui::fonts::Role.
const RoleDesc kRoles[] = {
    { "Menu",      kUiPx,        false },
    { "Chat",      kChatPx,      true  },
    { "Net",       kUiPx,        false },
    { "Nameplate", kNameplatePx, false },
    { "Toast",     kUiPx,        false },
};
constexpr int kRoleCount = int(sizeof(kRoles) / sizeof(kRoles[0]));

// config_registry::kFontRoleDefaultFamily -- menu/chat/toast = Fixedsys(3),
// net/nameplate = Roboto(1).
const int kDefaultFamilyForRole[kRoleCount] = { 3, 3, 1, 1, 3 };
// The worst resident set a user can select. CORRECTED 2026-07-28 (/qf D2 R1 Q4):
// the previous row {0,1,2,3,0} produced FOUR faces, and the gate doc recorded
// four as the ceiling. It is FIVE. Menu/Net/Nameplate/Toast are all (px 16,
// regular), so four distinct families across them = four distinct faces; Chat is
// (px 18, BOLD), whose dedup key can never equal any of them whatever family it
// picks. So the ceiling is 4 + 1. The old row wasted one of the four families by
// giving Toast Menu's, and paid for it by understating every worst-case cell.
const int kWorstFamilyForRole[kRoleCount]   = { 0, 0, 1, 2, 3 };

std::string g_assetDir;   // our four embedded families
std::string g_donorDir;   // downloaded OFL/CC donors

// ---- repertoire tiers --------------------------------------------------------
enum Tier {
    T_TODAY = 0,   // Latin-1 + Cyrillic, no donor  (what ships as of b131)
    T_CN,          // + ImGui ChineseSimplifiedCommon from Noto Sans SC
    T_CNJP,        // + ImGui Japanese too (Noto Sans JP merged)
    T_CN_EMOJI,    // T_CN + single-codepoint COLR emoji (Twemoji Mozilla)
    T_CNJP_EMOJI,  // T_CNJP + emoji
    // ---- 2026-07-29, USER: "lets bake all, including hieroglyphs to see how
    // much it will actually cost". The tiers above answer "what did arc D2
    // consider"; these answer "what does EVERYTHING cost", measured.
    T_SHIP,        // what b132 ACTUALLY ships: Latin-1 + Cyrillic + emoji
    T_LATEXT,      // + Latin Ext-A/B + Greek -- ZERO donor bytes, our own faces have them
    T_FULLCJK,     // + the WHOLE CJK Unified block 4E00..9FFF (20,992 cp), not ImGui's "common"
    T_EVERYTHING,  // + kana + Hangul syllables + Thai. The ceiling.
    T_COUNT
};
const char* kTierName[T_COUNT] = {
    "today (Latin+Cyrillic)", "+CN common", "+CN+JP", "+CN +emoji", "+CN+JP +emoji",
    "SHIPPING b132", "+LatExt+Greek", "+FULL CJK", "EVERYTHING",
};

// Latin-1 + Latin Ext-A/B + Greek + Cyrillic, from our OWN embedded faces --
// the gate doc measured their union as carrying "Latin Ext-A/B complete,
// Greek 135/144", so this tier costs NO new donor bytes at all.
const ImWchar kBaseWideRanges[] = {
    0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement (ships today)
    0x0100, 0x017F,   // Latin Extended-A   -- Polish, Czech, Turkish, Romanian...
    0x0180, 0x024F,   // Latin Extended-B
    0x0370, 0x03FF,   // Greek and Coptic
    0x0400, 0x052F,   // Cyrillic + Supplement (ships today)
    0x2DE0, 0x2DFF, 0xA640, 0xA69F,   // Cyrillic Extended-A/B (ships today)
    0xFFFD, 0xFFFD,   // the fallback glyph -- must be baked (arc D2 lesson)
    0,
};
// The WHOLE unified block, not a "common" subset: the arc-D2 finding was that
// no common-hanzi tier guarantees a whole name, so the honest ceiling is all of it.
const ImWchar kFullCjkRanges[]  = { 0x4E00, 0x9FFF, 0, };
const ImWchar kKanaRanges[]     = { 0x3040, 0x30FF, 0x31F0, 0x31FF, 0, };
const ImWchar kHangulRanges[]   = { 0x1100, 0x11FF, 0xAC00, 0xD7A3, 0, };
const ImWchar kThaiRanges[]     = { 0x0E00, 0x0E7F, 0, };

// Single-codepoint emoji ranges. Absent codepoints cost nothing: the freetype
// builder skips glyphs the donor's cmap does not have (imgui_freetype.cpp:512+).
const ImWchar kEmojiRanges[] = {
    0x2190, 0x21FF, 0x2300, 0x23FF, 0x2600, 0x27BF, 0x2B00, 0x2BFF,
#ifdef IMGUI_USE_WCHAR32
    0x1F000, 0x1FAFF,
#endif
    0,
};

struct Result {
    bool   built     = false;
    int    texW      = 0, texH = 0;
    size_t texBytes  = 0;
    bool   rgba      = false;
    double ms        = 0.0;
    int    faces     = 0;
    int    glyphs    = 0;
    size_t indexBytes = 0;   // sum over faces of IndexAdvanceX+IndexLookup capacity
    int    maxCp     = 0;
};

struct Face { int fam; int pxq; bool bold; };

void AddSource(ImFontAtlas* atlas, const std::string& path, float px,
               bool merge, const ImWchar* ranges, bool color) {
    ImFontConfig cfg;
    cfg.MergeMode        = merge;
    cfg.GlyphRanges      = ranges;
    if (color) cfg.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_LoadColor;
    if (!atlas->AddFontFromFileTTF(path.c_str(), px, &cfg, ranges))
        std::printf("  !! missing donor: %s\n", path.c_str());
}

Result Bake(Tier tier, float scale, const int* roleFamily) {
    ImFontAtlas atlas;
    // Dedup exactly like BakeEmbeddedRoles: one entry per (family, px*4, bold).
    std::vector<Face> faces;
    for (int r = 0; r < kRoleCount; ++r) {
        const int   fam = roleFamily[r];
        const float px  = kRoles[r].basePx * scale;
        const int   pxq = int(px * 4.f + 0.5f);
        bool seen = false;
        for (const Face& f : faces)
            if (f.fam == fam && f.pxq == pxq && f.bold == kRoles[r].bold) { seen = true; break; }
        if (!seen) faces.push_back({ fam, pxq, kRoles[r].bold });
    }

    const ImWchar* cyr = atlas.GetGlyphRangesCyrillic();
    for (const Face& f : faces) {
        const float px = float(f.pxq) / 4.f;
        const FamilyDesc& fd = kFamilies[f.fam];
        const bool wideBase = (tier == T_LATEXT || tier == T_FULLCJK || tier == T_EVERYTHING);
        AddSource(&atlas, g_assetDir + (f.bold ? fd.bold : fd.regular), px, false,
                  wideBase ? kBaseWideRanges : cyr, false);
        // Cross-merge the other three families behind the chosen one, exactly as
        // ui/fonts.cpp MergeBackstops does -- otherwise a face that lacks Greek
        // (JetBrains) makes this tier look cheaper than it actually ships.
        if (wideBase)
            for (int o = 0; o < kFamilyCount; ++o) {
                if (o == f.fam) continue;
                AddSource(&atlas, g_assetDir + (f.bold ? kFamilies[o].bold : kFamilies[o].regular),
                          px, true, kBaseWideRanges, false);
            }
        // Scripts with no glyphs in ANY embedded face -- these need a donor.
        if (tier == T_FULLCJK || tier == T_EVERYTHING)
            AddSource(&atlas, g_donorDir + "NotoSansSC-Regular.ttf", px, true,
                      kFullCjkRanges, false);
        if (tier == T_EVERYTHING) {
            AddSource(&atlas, g_donorDir + "NotoSansJP-Regular.ttf", px, true, kKanaRanges, false);
            // Hangul: MEASUREMENT-ONLY donor. malgun.ttf is a Windows system font and is
            // NOT redistributable -- shipping would need Noto Sans KR (OFL). This row is
            // measuring the ATLAS, which follows glyph COUNT and pixel size, not foundry.
            AddSource(&atlas, "C:\\Windows\\Fonts\\malgun.ttf", px, true, kHangulRanges, false);
            AddSource(&atlas, g_donorDir + "NotoSansJP-Regular.ttf", px, true, kThaiRanges, false);
        }
        if (tier == T_SHIP || tier == T_LATEXT || tier == T_FULLCJK || tier == T_EVERYTHING)
            AddSource(&atlas, g_donorDir + "Twemoji.Mozilla.ttf", px, true, kEmojiRanges, true);
        if (tier == T_CN || tier == T_CNJP || tier == T_CN_EMOJI || tier == T_CNJP_EMOJI)
            AddSource(&atlas, g_donorDir + "NotoSansSC-Regular.ttf", px, true,
                      atlas.GetGlyphRangesChineseSimplifiedCommon(), false);
        if (tier == T_CNJP || tier == T_CNJP_EMOJI)
            AddSource(&atlas, g_donorDir + "NotoSansJP-Regular.ttf", px, true,
                      atlas.GetGlyphRangesJapanese(), false);
        if (tier == T_CN_EMOJI || tier == T_CNJP_EMOJI)
            AddSource(&atlas, g_donorDir + "Twemoji.Mozilla.ttf", px, true,
                      kEmojiRanges, true);
    }

    Result res;
    res.faces = int(faces.size());
    const auto t0 = std::chrono::steady_clock::now();
    res.built = atlas.Build();
    const auto t1 = std::chrono::steady_clock::now();
    res.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!res.built) return res;

    res.texW = atlas.TexWidth;
    res.texH = atlas.TexHeight;
    res.rgba = (atlas.TexPixelsRGBA32 != nullptr);
    // What the GPU gets: both our halves call GetTexDataAsRGBA32
    // (imgui_impl_dx11.cpp:330, imgui_impl_dx12.cpp:310) -> always 4 B/px.
    res.texBytes = size_t(atlas.TexWidth) * size_t(atlas.TexHeight) * 4u;
    for (ImFont* f : atlas.Fonts) {
        res.glyphs += f->Glyphs.Size;
        res.indexBytes += size_t(f->IndexAdvanceX.Size) * sizeof(float)
                        + size_t(f->IndexLookup.Size)   * sizeof(ImWchar);
        for (const ImFontGlyph& g : f->Glyphs)
            if (int(g.Codepoint) > res.maxCp) res.maxCp = int(g.Codepoint);
    }
    return res;
}

void Row(const char* cfgName, Tier tier, float scale, const int* roleFamily) {
    const Result r = Bake(tier, scale, roleFamily);
    std::printf("%-8s %-16s x%.2f  faces=%d  ", cfgName, kTierName[tier], scale, r.faces);
    if (!r.built) { std::printf("BUILD FAILED (atlas did not fit)\n"); return; }
    std::printf("tex=%5dx%-5d %8.2f MB  glyphs=%-6d maxCp=U+%05X  idx=%7.2f MB  %7.1f ms  %s\n",
                r.texW, r.texH, double(r.texBytes) / (1024.0 * 1024.0),
                r.glyphs, r.maxCp, double(r.indexBytes) / (1024.0 * 1024.0),
                r.ms, r.rgba ? "RGBA32" : "Alpha8");
}

// Does a COLR/CPAL donor actually RASTERISE colour pixels under our FreeType
// configuration -- or does "COLR v0, no CBDT" stay a TABLE fact while FreeType
// hands back the (empty) base outline? Bakes one emoji and reads the bitmap.
void ColorRasterCheck() {
    for (int withFlag = 0; withFlag < 2; ++withFlag) {
        ImFontAtlas atlas;
        ImFontConfig cfg;
        if (withFlag) cfg.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_LoadColor;
        static const ImWchar r[] = {
#ifdef IMGUI_USE_WCHAR32
            0x1F600, 0x1F600, 0x1F60D, 0x1F60D,
#endif
            0x263A, 0x263A, 0,
        };
        cfg.GlyphRanges = r;
        ImFont* f = atlas.AddFontFromFileTTF((g_donorDir + "Twemoji.Mozilla.ttf").c_str(),
                                             32.f, &cfg, r);
        if (!f) { std::printf("  LoadColor=%d: donor missing\n", withFlag); continue; }
        if (!atlas.Build()) { std::printf("  LoadColor=%d: BUILD FAILED\n", withFlag); continue; }

        // Count glyphs, and -- for the coloured path -- how many texels inside the
        // baked glyph boxes are NOT greyscale (r==g==b), which is what proves colour
        // pixels reached the atlas rather than a white-on-alpha mask.
        int coloredFlag = 0, nonGrey = 0, visible = 0;
        for (const ImFontGlyph& g : f->Glyphs) {
            if (g.Colored) ++coloredFlag;
            if (g.Visible) ++visible;
        }
        if (atlas.TexPixelsRGBA32) {
            const int w = atlas.TexWidth;
            for (const ImFontGlyph& g : f->Glyphs) {
                const int x0 = int(g.U0 * w), x1 = int(g.U1 * w);
                const int y0 = int(g.V0 * atlas.TexHeight), y1 = int(g.V1 * atlas.TexHeight);
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) {
                        const unsigned p = atlas.TexPixelsRGBA32[y * w + x];
                        const unsigned rr = p & 0xFF, gg = (p >> 8) & 0xFF, bb = (p >> 16) & 0xFF;
                        if (rr != gg || gg != bb) ++nonGrey;
                    }
            }
        }
        std::printf("  LoadColor=%d: glyphs=%d visible=%d Colored-flag=%d  buffer=%s"
                    "  non-greyscale texels=%d\n",
                    withFlag, f->Glyphs.Size, visible, coloredFlag,
                    atlas.TexPixelsRGBA32 ? "RGBA32" : "Alpha8", nonGrey);
    }
}

// ---- the DEMAND question (/qf D2 R1 C10) -------------------------------------
//
// Every cell above bakes the WHOLE 3,349-codepoint CN-common set, and that is
// where 16 MB / 139 ms comes from. Nobody needs the whole set: a name is <= 20
// codepoints and a chat backlog is bounded. This bakes today's repertoire plus
// exactly N distinct hanzi -- the shape a demand-driven accumulator would
// actually produce on the PINNED 1.91.5 -- so the arm-A cost can be priced
// against what users generate rather than against the dictionary.
Result BakeDemand(int nHanzi, int nEmoji, float scale, const int* roleFamily) {
    ImFontAtlas atlas;
    std::vector<Face> faces;
    for (int r = 0; r < kRoleCount; ++r) {
        const int   fam = roleFamily[r];
        const float px  = kRoles[r].basePx * scale;
        const int   pxq = int(px * 4.f + 0.5f);
        bool seen = false;
        for (const Face& f : faces)
            if (f.fam == fam && f.pxq == pxq && f.bold == kRoles[r].bold) { seen = true; break; }
        if (!seen) faces.push_back({ fam, pxq, kRoles[r].bold });
    }

    // An explicit [cp,cp] pair per demanded codepoint -- exactly what
    // ImFontGlyphRangesBuilder::BuildRanges emits for a scattered set. Hanzi are
    // walked from the start of URO so the set is real, distinct codepoints.
    std::vector<ImWchar> hz, em;
    for (int i = 0; i < nHanzi; ++i) {
        const ImWchar cp = ImWchar(0x4E00 + i * 7);   // spread, not a contiguous run
        hz.push_back(cp); hz.push_back(cp);
    }
    hz.push_back(0);
    for (int i = 0; i < nEmoji; ++i) {
#ifdef IMGUI_USE_WCHAR32
        const ImWchar cp = ImWchar(0x1F600 + i);
#else
        const ImWchar cp = ImWchar(0x2600 + i);
#endif
        em.push_back(cp); em.push_back(cp);
    }
    em.push_back(0);

    const ImWchar* cyr = atlas.GetGlyphRangesCyrillic();
    for (const Face& f : faces) {
        const float px = float(f.pxq) / 4.f;
        const FamilyDesc& fd = kFamilies[f.fam];
        AddSource(&atlas, g_assetDir + (f.bold ? fd.bold : fd.regular), px, false, cyr, false);
        if (nHanzi > 0)
            AddSource(&atlas, g_donorDir + "NotoSansSC-Regular.ttf", px, true, hz.data(), false);
        if (nEmoji > 0)
            AddSource(&atlas, g_donorDir + "Twemoji.Mozilla.ttf", px, true, em.data(), true);
    }

    Result res;
    res.faces = int(faces.size());
    const auto t0 = std::chrono::steady_clock::now();
    res.built = atlas.Build();
    const auto t1 = std::chrono::steady_clock::now();
    res.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!res.built) return res;
    res.texW = atlas.TexWidth; res.texH = atlas.TexHeight;
    res.rgba = (atlas.TexPixelsRGBA32 != nullptr);
    res.texBytes = size_t(atlas.TexWidth) * size_t(atlas.TexHeight) * 4u;
    for (ImFont* f : atlas.Fonts) {
        res.glyphs += f->Glyphs.Size;
        res.indexBytes += size_t(f->IndexAdvanceX.Size) * sizeof(float)
                        + size_t(f->IndexLookup.Size)   * sizeof(ImWchar);
        for (const ImFontGlyph& g : f->Glyphs)
            if (int(g.Codepoint) > res.maxCp) res.maxCp = int(g.Codepoint);
    }
    return res;
}

// ---- the CROSS-MERGE question (/qf D2 R3 RF-C, R4 Q1/Q3) ---------------------
//
// Today every role bakes ONE face over GetGlyphRangesCyrillic() -- Latin-1 +
// Cyrillic, a few hundred codepoints. RF-C proposes merging the OTHER embedded
// faces in behind the user's chosen one so the guaranteed repertoire is the
// UNION of what we already ship (8,148 cp by cmap) rather than the intersection
// (717). "Zero added DLL bytes" was true and irrelevant: this measures the ATLAS
// cost, which is the claim that matters -- and whether Build() still succeeds
// when the demand accumulator converges on the WHOLE union.
Result BakeCrossMerge(bool crossMerge, bool fullUnionRange, float scale,
                      const int* roleFamily) {
    ImFontAtlas atlas;
    std::vector<Face> faces;
    for (int r = 0; r < kRoleCount; ++r) {
        const int   fam = roleFamily[r];
        const float px  = kRoles[r].basePx * scale;
        const int   pxq = int(px * 4.f + 0.5f);
        bool seen = false;
        for (const Face& f : faces)
            if (f.fam == fam && f.pxq == pxq && f.bold == kRoles[r].bold) { seen = true; break; }
        if (!seen) faces.push_back({ fam, pxq, kRoles[r].bold });
    }

    // BMP-wide so every codepoint any embedded face HAS is actually baked -- the
    // converged ceiling of the accumulator, not a sample of it.
    static const ImWchar kAll[] = { 0x0020, 0xFFFD, 0 };
    const ImWchar* cyr = atlas.GetGlyphRangesCyrillic();
    const ImWchar* rng = fullUnionRange ? kAll : cyr;

    for (const Face& f : faces) {
        const float px = float(f.pxq) / 4.f;
        AddSource(&atlas, g_assetDir + (f.bold ? kFamilies[f.fam].bold : kFamilies[f.fam].regular),
                  px, false, rng, false);
        if (crossMerge)
            for (int o = 0; o < kFamilyCount; ++o) {
                if (o == f.fam) continue;
                AddSource(&atlas, g_assetDir + (f.bold ? kFamilies[o].bold : kFamilies[o].regular),
                          px, true, rng, false);
            }
    }

    Result res;
    res.faces = int(faces.size());
    const auto t0 = std::chrono::steady_clock::now();
    res.built = atlas.Build();
    const auto t1 = std::chrono::steady_clock::now();
    res.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!res.built) return res;
    res.texW = atlas.TexWidth; res.texH = atlas.TexHeight;
    res.rgba = (atlas.TexPixelsRGBA32 != nullptr);
    res.texBytes = size_t(atlas.TexWidth) * size_t(atlas.TexHeight) * 4u;
    for (ImFont* f : atlas.Fonts) {
        res.glyphs += f->Glyphs.Size;
        res.indexBytes += size_t(f->IndexAdvanceX.Size) * sizeof(float)
                        + size_t(f->IndexLookup.Size)   * sizeof(ImWchar);
        for (const ImFontGlyph& g : f->Glyphs)
            if (int(g.Codepoint) > res.maxCp) res.maxCp = int(g.Codepoint);
    }
    return res;
}

void CrossRow(const char* cfgName, bool cross, bool full, float scale, const int* fam) {
    const Result r = BakeCrossMerge(cross, full, scale, fam);
    char what[48];
    std::snprintf(what, sizeof(what), "%s/%s", cross ? "cross" : "single",
                  full ? "union-rng" : "today-rng");
    std::printf("%-8s %-16s x%.2f  faces=%d  ", cfgName, what, scale, r.faces);
    if (!r.built) { std::printf("BUILD FAILED (atlas did not fit)\n"); return; }
    std::printf("tex=%5dx%-5d %8.2f MB  glyphs=%-6d maxCp=U+%05X  idx=%7.2f MB  %7.1f ms  %s\n",
                r.texW, r.texH, double(r.texBytes) / (1024.0 * 1024.0),
                r.glyphs, r.maxCp, double(r.indexBytes) / (1024.0 * 1024.0),
                r.ms, r.rgba ? "RGBA32" : "Alpha8");
}

// ---- the OS-AS-DONOR question (/qf D2 R5 RF-E, R6 Q4) ------------------------
//
// The subsetted donors are ~1 MB files. A real system face is 2-29 MB and a
// .ttc collection at that. Does merging one behave like a subset once the RANGE
// is demand-sized, or does the file size / cmap ceiling leak into the atlas and
// the index tables? Measured, not reasoned: this is the bill RF-E's "zero DLL
// bytes" is being priced against.
Result BakeOsDonor(int nHanzi, int nEmoji, float scale, const int* roleFamily) {
    ImFontAtlas atlas;
    std::vector<Face> faces;
    for (int r = 0; r < kRoleCount; ++r) {
        const int   fam = roleFamily[r];
        const float px  = kRoles[r].basePx * scale;
        const int   pxq = int(px * 4.f + 0.5f);
        bool seen = false;
        for (const Face& f : faces)
            if (f.fam == fam && f.pxq == pxq && f.bold == kRoles[r].bold) { seen = true; break; }
        if (!seen) faces.push_back({ fam, pxq, kRoles[r].bold });
    }

    std::vector<ImWchar> hz, em;
    for (int i = 0; i < nHanzi; ++i) { const ImWchar c = ImWchar(0x4E00 + i * 7); hz.push_back(c); hz.push_back(c); }
    hz.push_back(0);
    for (int i = 0; i < nEmoji; ++i) {
#ifdef IMGUI_USE_WCHAR32
        const ImWchar c = ImWchar(0x1F600 + i);
#else
        const ImWchar c = ImWchar(0x2600 + i);
#endif
        em.push_back(c); em.push_back(c);
    }
    em.push_back(0);

    const ImWchar* cyr = atlas.GetGlyphRangesCyrillic();
    for (const Face& f : faces) {
        const float px = float(f.pxq) / 4.f;
        AddSource(&atlas, g_assetDir + (f.bold ? kFamilies[f.fam].bold : kFamilies[f.fam].regular),
                  px, false, cyr, false);
        // The OS faces, merged straight off %WINDIR%\Fonts -- no subsetting, no
        // shipping, exactly what fonts.cpp:214-217 already does for Tahoma.
        if (nHanzi > 0) {
            ImFontConfig cfg; cfg.MergeMode = true; cfg.GlyphRanges = hz.data(); cfg.FontNo = 0;
            if (!atlas.AddFontFromFileTTF("C:\\Windows\\Fonts\\simsun.ttc", px, &cfg, hz.data()))
                std::printf("  !! simsun.ttc did not load\n");
        }
        if (nEmoji > 0) {
            ImFontConfig cfg; cfg.MergeMode = true; cfg.GlyphRanges = em.data();
            cfg.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_LoadColor;
            if (!atlas.AddFontFromFileTTF("C:\\Windows\\Fonts\\seguiemj.ttf", px, &cfg, em.data()))
                std::printf("  !! seguiemj.ttf did not load\n");
        }
    }

    Result res;
    res.faces = int(faces.size());
    const auto t0 = std::chrono::steady_clock::now();
    res.built = atlas.Build();
    const auto t1 = std::chrono::steady_clock::now();
    res.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!res.built) return res;
    res.texW = atlas.TexWidth; res.texH = atlas.TexHeight;
    res.rgba = (atlas.TexPixelsRGBA32 != nullptr);
    res.texBytes = size_t(atlas.TexWidth) * size_t(atlas.TexHeight) * 4u;
    for (ImFont* f : atlas.Fonts) {
        res.glyphs += f->Glyphs.Size;
        res.indexBytes += size_t(f->IndexAdvanceX.Size) * sizeof(float)
                        + size_t(f->IndexLookup.Size)   * sizeof(ImWchar);
        for (const ImFontGlyph& g : f->Glyphs)
            if (int(g.Codepoint) > res.maxCp) res.maxCp = int(g.Codepoint);
    }
    return res;
}

void OsRow(const char* cfgName, int nHanzi, int nEmoji, float scale, const int* fam) {
    const Result r = BakeOsDonor(nHanzi, nEmoji, scale, fam);
    char what[48];
    std::snprintf(what, sizeof(what), "OS +%dhz +%dem", nHanzi, nEmoji);
    std::printf("%-8s %-16s x%.2f  faces=%d  ", cfgName, what, scale, r.faces);
    if (!r.built) { std::printf("BUILD FAILED (atlas did not fit)\n"); return; }
    std::printf("tex=%5dx%-5d %8.2f MB  glyphs=%-6d maxCp=U+%05X  idx=%7.2f MB  %7.1f ms  %s\n",
                r.texW, r.texH, double(r.texBytes) / (1024.0 * 1024.0),
                r.glyphs, r.maxCp, double(r.indexBytes) / (1024.0 * 1024.0),
                r.ms, r.rgba ? "RGBA32" : "Alpha8");
}

// Same as BakeDemand but with an explicit BASE range, so a range WIDENING can be
// priced against the allocation the emoji tier already bought (/qf D2 R11 Q4).
Result BakeWide(const ImWchar* baseRange, int nEmoji, float scale, const int* roleFamily) {
    ImFontAtlas atlas;
    std::vector<Face> faces;
    for (int r = 0; r < kRoleCount; ++r) {
        const int   fam = roleFamily[r];
        const float px  = kRoles[r].basePx * scale;
        const int   pxq = int(px * 4.f + 0.5f);
        bool seen = false;
        for (const Face& f : faces)
            if (f.fam == fam && f.pxq == pxq && f.bold == kRoles[r].bold) { seen = true; break; }
        if (!seen) faces.push_back({ fam, pxq, kRoles[r].bold });
    }
    std::vector<ImWchar> em;
    for (int i = 0; i < nEmoji; ++i) {
#ifdef IMGUI_USE_WCHAR32
        const ImWchar c = ImWchar(0x1F300 + i);
#else
        const ImWchar c = ImWchar(0x2600 + i);
#endif
        em.push_back(c); em.push_back(c);
    }
    em.push_back(0);

    for (const Face& f : faces) {
        const float px = float(f.pxq) / 4.f;
        AddSource(&atlas, g_assetDir + (f.bold ? kFamilies[f.fam].bold : kFamilies[f.fam].regular),
                  px, false, baseRange, false);
        for (int o = 0; o < kFamilyCount; ++o) {
            if (o == f.fam) continue;
            AddSource(&atlas, g_assetDir + (f.bold ? kFamilies[o].bold : kFamilies[o].regular),
                      px, true, baseRange, false);
        }
        if (nEmoji > 0)
            AddSource(&atlas, g_donorDir + "Twemoji.Mozilla.ttf", px, true, em.data(), true);
    }

    Result res;
    res.faces = int(faces.size());
    const auto t0 = std::chrono::steady_clock::now();
    res.built = atlas.Build();
    const auto t1 = std::chrono::steady_clock::now();
    res.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!res.built) return res;
    res.texW = atlas.TexWidth; res.texH = atlas.TexHeight;
    res.rgba = (atlas.TexPixelsRGBA32 != nullptr);
    res.texBytes = size_t(atlas.TexWidth) * size_t(atlas.TexHeight) * 4u;
    for (ImFont* f : atlas.Fonts) {
        res.glyphs += f->Glyphs.Size;
        res.indexBytes += size_t(f->IndexAdvanceX.Size) * sizeof(float)
                        + size_t(f->IndexLookup.Size)   * sizeof(ImWchar);
        for (const ImFontGlyph& g : f->Glyphs)
            if (int(g.Codepoint) > res.maxCp) res.maxCp = int(g.Codepoint);
    }
    return res;
}

void WideRow(const char* cfgName, const ImWchar* base, int nEmoji, float scale, const int* fam) {
    const Result r = BakeWide(base, nEmoji, scale, fam);
    char what[48];
    std::snprintf(what, sizeof(what), "WIDE +%dem", nEmoji);
    std::printf("%-8s %-16s x%.2f  faces=%d  ", cfgName, what, scale, r.faces);
    if (!r.built) { std::printf("BUILD FAILED (atlas did not fit)\n"); return; }
    std::printf("tex=%5dx%-5d %8.2f MB  glyphs=%-6d maxCp=U+%05X  idx=%7.2f MB  %7.1f ms  %s\n",
                r.texW, r.texH, double(r.texBytes) / (1024.0 * 1024.0),
                r.glyphs, r.maxCp, double(r.indexBytes) / (1024.0 * 1024.0),
                r.ms, r.rgba ? "RGBA32" : "Alpha8");
}

void DemandRow(const char* cfgName, int nHanzi, int nEmoji, float scale, const int* fam) {
    const Result r = BakeDemand(nHanzi, nEmoji, scale, fam);
    char what[48];
    std::snprintf(what, sizeof(what), "today +%dhz +%dem", nHanzi, nEmoji);
    std::printf("%-8s %-16s x%.2f  faces=%d  ", cfgName, what, scale, r.faces);
    if (!r.built) { std::printf("BUILD FAILED (atlas did not fit)\n"); return; }
    std::printf("tex=%5dx%-5d %8.2f MB  glyphs=%-6d maxCp=U+%05X  idx=%7.2f MB  %7.1f ms  %s\n",
                r.texW, r.texH, double(r.texBytes) / (1024.0 * 1024.0),
                r.glyphs, r.maxCp, double(r.indexBytes) / (1024.0 * 1024.0),
                r.ms, r.rgba ? "RGBA32" : "Alpha8");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: atlas_probe <assets/fonts dir> <donor dir>\n");
        return 2;
    }
    g_assetDir = argv[1]; g_donorDir = argv[2];
    if (!g_assetDir.empty() && g_assetDir.back() != '\\' && g_assetDir.back() != '/') g_assetDir += "\\";
    if (!g_donorDir.empty() && g_donorDir.back() != '\\' && g_donorDir.back() != '/') g_donorDir += "\\";

    ImGui::CreateContext();
    std::printf("ImWchar = %d-bit   IM_UNICODE_CODEPOINT_MAX = 0x%X   imgui %s\n\n",
                int(sizeof(ImWchar) * 8), IM_UNICODE_CODEPOINT_MAX, IMGUI_VERSION);
    std::printf("=== COLR raster check (does the emoji donor actually paint?) ===\n");
    ColorRasterCheck();
    std::printf("\n%-8s %-16s %-6s %-7s %s\n", "config", "repertoire", "scale", "faces",
                "texture / glyphs / index tables / bake");
    for (int cfg = 0; cfg < 2; ++cfg) {
        const int* fam = cfg == 0 ? kDefaultFamilyForRole : kWorstFamilyForRole;
        const char* name = cfg == 0 ? "default" : "worst";
        for (float scale : { 1.0f, 1.5f, 2.0f })
            for (int t = 0; t < T_COUNT; ++t)
                Row(name, Tier(t), scale, fam);
        std::printf("\n");
    }

    std::printf("=== CROSS-MERGE (RF-C): what the guaranteed-repertoire construction costs ===\n");
    for (int cfg = 0; cfg < 2; ++cfg) {
        const int* fam = cfg == 0 ? kDefaultFamilyForRole : kWorstFamilyForRole;
        const char* name = cfg == 0 ? "default" : "worst";
        for (float scale : { 1.0f, 2.0f }) {
            CrossRow(name, false, false, scale, fam);   // control: what SHIPS today
            CrossRow(name, true,  false, scale, fam);   // cross-merge, today's range
            CrossRow(name, false, true,  scale, fam);   // single face, whole BMP
            CrossRow(name, true,  true,  scale, fam);   // the converged ceiling
        }
        std::printf("\n");
    }

    // (/qf D2 R11 Q4): the atlas is power-of-two QUANTIZED, so a range widening
    // can be FREE if it fits inside an allocation already bought. Greek and
    // Latin-Ext are in the embedded cmaps (C11) and cost zero DLL bytes -- do
    // they fit inside the emoji tier's texture, or do they buy another doubling?
    std::printf("=== R11 Q4: does Greek + Latin-Ext fit inside the emoji allocation? ===\n");
    {
        // today's range, + Greek/LatExt, + both, each with the emoji donor.
        static const ImWchar kWide[] = {
            0x0020, 0x00FF,   // Latin-1 (what GetGlyphRangesCyrillic covers)
            0x0100, 0x024F,   // Latin Extended-A + B
            0x0370, 0x03FF,   // Greek
            0x0400, 0x052F,   // Cyrillic + Supplement
            0x2000, 0x206F,   // General punctuation
            0,
        };
        for (int cfg = 0; cfg < 2; ++cfg) {
            const int* fam = cfg == 0 ? kDefaultFamilyForRole : kWorstFamilyForRole;
            const char* name = cfg == 0 ? "default" : "worst";
            for (float scale : { 1.0f, 2.0f }) {
                DemandRow(name, 0, 1356, scale, fam);            // the decision's tier
                WideRow(name, kWide, 1356, scale, fam);          // + Greek/LatExt/punct
            }
            std::printf("\n");
        }
    }

    // The v8 pillar (/qf D2 R9 Q1): CJK is out, so the ONLY eager bake that
    // matters is today's repertoire + the emoji donor, cross-merged, at the
    // WORST face count and the WORST scale. Every earlier +em row also carried
    // hanzi, so none of them priced this configuration.
    std::printf("=== v8 EAGER BAKE: today + emoji only, cross-merged, LoadColor ON ===\n");
    for (int cfg = 0; cfg < 2; ++cfg) {
        const int* fam = cfg == 0 ? kDefaultFamilyForRole : kWorstFamilyForRole;
        const char* name = cfg == 0 ? "default" : "worst";
        for (float scale : { 1.0f, 1.5f, 2.0f }) {
            DemandRow(name, 0,    0, scale, fam);      // control: what ships today
            DemandRow(name, 0,  200, scale, fam);      // a realistic emoji working set
            DemandRow(name, 0, 1356, scale, fam);      // the WHOLE Twemoji donor, eager
        }
        std::printf("\n");
    }

    std::printf("=== OS-AS-DONOR (RF-E): merging the real system faces, unsubsetted ===\n");
    for (int cfg = 0; cfg < 2; ++cfg) {
        const int* fam = cfg == 0 ? kDefaultFamilyForRole : kWorstFamilyForRole;
        const char* name = cfg == 0 ? "default" : "worst";
        for (float scale : { 1.0f, 2.0f }) {
            OsRow(name, 4,   0,  scale, fam);
            OsRow(name, 20,  0,  scale, fam);
            OsRow(name, 60,  8,  scale, fam);
            OsRow(name, 200, 40, scale, fam);
        }
        std::printf("\n");
    }

    std::printf("=== DEMAND tiers -- what an accumulator actually bakes (1.91.5) ===\n");
    for (int cfg = 0; cfg < 2; ++cfg) {
        const int* fam = cfg == 0 ? kDefaultFamilyForRole : kWorstFamilyForRole;
        const char* name = cfg == 0 ? "default" : "worst";
        for (float scale : { 1.0f, 2.0f }) {
            DemandRow(name, 0,   0,  scale, fam);   // control: today, no donor
            DemandRow(name, 4,   0,  scale, fam);   // one short CJK name
            DemandRow(name, 20,  0,  scale, fam);   // four CJK names / a chat line
            DemandRow(name, 60,  8,  scale, fam);   // a busy 4-peer session
            DemandRow(name, 200, 40, scale, fam);   // a long CJK chat backlog
        }
        std::printf("\n");
    }
    ImGui::DestroyContext();
    return 0;
}
