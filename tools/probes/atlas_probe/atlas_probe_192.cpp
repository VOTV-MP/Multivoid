// atlas_probe_192 -- arm B of the arc-D fork (design section 9b.7 items 1+2).
//
// atlas_probe.cpp measures 1.91.5, where a repertoire must be BAKED UP FRONT.
// This one measures Dear ImGui 1.92.8, where glyphs load ON DEMAND, so the
// question is not "does the whole repertoire fit" but "what does the atlas
// actually cost once real text has been drawn". Same three resident faces,
// same per-role px, so the two tables are directly comparable.
//
// DEV TOOL. Never linked into the mod.

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/freetype/imgui_freetype.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Non-fatal assert capture -- see probe_imconfig.h for why aborting would
// measure a regime we do not ship.
static std::vector<std::string> g_asserts;
void ProbeAssertRecord(const char* expr, const char* file, int line) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s:%d  %s", file, line, expr);
    g_asserts.push_back(buf);
}

namespace {

constexpr float kUiPx   = 16.f;
constexpr float kChatPx = 18.f;

std::string g_assetDir, g_donorDir;

// The default resident set, deduped exactly like ui::fonts::BakeEmbeddedRoles:
//   Fixedsys 16 regular (menu+toast), Fixedsys 18 "bold" (chat), Roboto 16 (net+nameplate)
struct FaceSpec { const char* file; float px; };

// A null renderer that satisfies 1.92's texture protocol.
void ServiceTextures() {
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->Status == ImTextureStatus_WantCreate) {
            tex->SetTexID((ImTextureID)(intptr_t)1);
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantUpdates) {
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantDestroy) {
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

std::string Repeat(const char* utf8, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += utf8;
    return s;
}

// Distinct codepoints, so the atlas is really asked for N glyphs (not one N times).
std::string RangeUtf8(unsigned first, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        unsigned c = first + unsigned(i);
        char buf[5]; int len = 0;
        if (c < 0x800)        { buf[0] = char(0xC0 | (c >> 6));  buf[1] = char(0x80 | (c & 0x3F)); len = 2; }
        else if (c < 0x10000) { buf[0] = char(0xE0 | (c >> 12)); buf[1] = char(0x80 | ((c >> 6) & 0x3F)); buf[2] = char(0x80 | (c & 0x3F)); len = 3; }
        else                  { buf[0] = char(0xF0 | (c >> 18)); buf[1] = char(0x80 | ((c >> 12) & 0x3F)); buf[2] = char(0x80 | ((c >> 6) & 0x3F)); buf[3] = char(0x80 | (c & 0x3F)); len = 4; }
        s.append(buf, len);
    }
    return s;
}

struct Load { const char* label; std::string text; };

void Run(float scale, bool withDonors) {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920, 1080);
    io.DeltaTime   = 1.0f / 60.0f;
    // 1.92's on-demand path is only active when the backend declares it.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->TexMaxWidth = 4096;   // same practical ceiling the 1.91.5 builder used

    const FaceSpec specs[] = {
        { "FSEX300.ttf",         kUiPx   * scale },
        { "FSEX300.ttf",         kChatPx * scale },
        { "Roboto-Regular.ttf",  kUiPx   * scale },
    };
    std::vector<ImFont*> fonts;
    int donorsMissing = 0;
    for (const FaceSpec& s : specs) {
        ImFontConfig cfg;
        ImFont* f = io.Fonts->AddFontFromFileTTF((g_assetDir + s.file).c_str(), s.px, &cfg);
        if (withDonors && f) {
            ImFontConfig m; m.MergeMode = true;
            // The CJK donors are downloads, not vendored -- absent, the hanzi rows
            // below report "0 glyphs, 0.25 MB" and read as a cheap atlas rather
            // than as an empty one. Count and say so.
            for (const char* d : { "NotoSansSC-Regular.ttf", "NotoSansJP-Regular.ttf",
                                   "Twemoji.Mozilla.ttf" })
                if (!io.Fonts->AddFontFromFileTTF((g_donorDir + d).c_str(), s.px, &m))
                    ++donorsMissing;
        }
        if (f) fonts.push_back(f);
    }
    if (donorsMissing) {
        std::printf("  scale x%.2f  donors=yes -- SKIPPED: %d donor file(s) missing under %s. "
                    "The hanzi/emoji rows would be measurements of an EMPTY atlas.\n",
                    scale, donorsMissing, g_donorDir.c_str());
        ImGui::DestroyContext(ctx);
        return;
    }

    const std::vector<Load> loads = {
        { "latin+cyrillic only", "The quick brown fox 0123456789 Съешь ещё этих мягких булок" },
        { "+ 20 hanzi",          RangeUtf8(0x4E00, 20) },
        { "+ 200 hanzi",         RangeUtf8(0x4E00, 200) },
        { "+ 3000 hanzi",        RangeUtf8(0x4E00, 3000) },
        { "+ 40 emoji",          RangeUtf8(0x1F600, 40) },
    };

    std::string acc;
    std::printf("  scale x%.2f  donors=%s\n", scale, withDonors ? "yes" : "no");
    for (const Load& l : loads) {
        acc += l.text;
        double ms = 0.0;
        // Two frames: the first loads glyphs, the second is the steady state.
        for (int frame = 0; frame < 2; ++frame) {
            const auto t0 = std::chrono::steady_clock::now();
            ImGui::NewFrame();
            ImGui::Begin("probe");
            for (ImFont* f : fonts) {
                ImGui::PushFont(f, f->LegacySize);
                ImGui::TextUnformatted(acc.c_str());
                ImGui::PopFont();
            }
            ImGui::End();
            ImGui::Render();
            ServiceTextures();
            const auto t1 = std::chrono::steady_clock::now();
            if (frame == 0) ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
        int w = 0, h = 0, glyphs = 0;
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) { w = tex->Width; h = tex->Height; }
        for (ImFont* f : fonts)
            if (ImFontBaked* b = f->GetFontBaked(f->LegacySize)) glyphs += b->Glyphs.Size;
        std::printf("    %-22s tex=%5dx%-5d %7.2f MB  glyphs=%-6d  load-frame %6.1f ms\n",
                    l.label, w, h, double(w) * double(h) * 4.0 / (1024.0 * 1024.0), glyphs, ms);
    }
    ImGui::DestroyContext(ctx);
}

// R2 of the build pass: claim 6(a) ("colour is VRAM-neutral") was measured on
// 1.91.5, the build C1 RETIRES. 1.92 uploads through ImTextureData with its own
// Format, and exposes ImFontAtlas::TexDesiredFormat -- so the Alpha8 option that
// did not exist at the 1.91.5 upload seam exists here. Re-measure on the arm
// that actually ships: does LoadColor paint, and what does it cost against the
// Alpha8 alternative?
void ColorRasterCheck192() {
    for (int mode = 0; mode < 3; ++mode) {
        // 0 = no LoadColor, default format. 1 = LoadColor. 2 = LoadColor + ask for Alpha8.
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920, 1080);
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        if (mode == 2) io.Fonts->TexDesiredFormat = ImTextureFormat_Alpha8;

        ImFontConfig base;
        ImFont* f = io.Fonts->AddFontFromFileTTF((g_assetDir + "Roboto-Regular.ttf").c_str(), 32.f, &base);
        ImFontConfig m; m.MergeMode = true;
        if (mode != 0) m.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LoadColor;
        // CORRECTED 2026-07-30: this used to merge g_donorDir's full
        // Twemoji.Mozilla.ttf, an ad-hoc download no longer on disk -- so the arm
        // silently measured an atlas with NO donor in it and reported Colored=0 /
        // non-greyscale=0, which reads exactly like "LoadColor is broken in 1.92".
        // [[lesson-an-instrument-can-fail-the-feature-it-tests]] The donor that
        // SHIPS is the generated subset in assets/fonts, which is also the only
        // one whose answer matters.
        const std::string donorPath = g_assetDir + "TwemojiMozilla-Subset.ttf";
        if (!io.Fonts->AddFontFromFileTTF(donorPath.c_str(), 32.f, &m)) {
            std::printf("  ABORT mode=%d: donor not loadable at %s -- any verdict below "
                        "would be the instrument's, not the feature's\n", mode, donorPath.c_str());
            ImGui::DestroyContext(ctx);
            continue;
        }

        const char* emoji = "\xF0\x9F\x98\x80\xF0\x9F\x98\x8D\xE2\x98\xBA";  // U+1F600 U+1F60D U+263A
        for (int frame = 0; frame < 2; ++frame) {
            ImGui::NewFrame();
            ImGui::Begin("probe");
            ImGui::PushFont(f, f->LegacySize);
            ImGui::TextUnformatted(emoji);
            ImGui::PopFont();
            ImGui::End();
            ImGui::Render();
            ServiceTextures();
        }
        int visible = 0, colored = 0;
        if (ImFontBaked* b = f->GetFontBaked(f->LegacySize))
            for (const ImFontGlyph& g : b->Glyphs) {
                if (g.Visible && g.Codepoint > 0x2000) ++visible;
                if (g.Colored) ++colored;
            }
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            int nonGrey = 0;
            if (tex->Pixels && tex->BytesPerPixel == 4)
                for (int i = 0; i < tex->Width * tex->Height; ++i) {
                    const unsigned char* p = tex->Pixels + i * 4;
                    if (p[0] != p[1] || p[1] != p[2]) ++nonGrey;
                }
            std::printf("  mode=%d (%s) tex=%dx%d fmt=%s bpp=%d UseColors=%d  "
                        "emoji-visible=%d Colored=%d  non-greyscale texels=%d  %.2f MB\n",
                        mode,
                        mode == 0 ? "no LoadColor" : (mode == 1 ? "LoadColor" : "LoadColor + want Alpha8"),
                        tex->Width, tex->Height,
                        tex->Format == ImTextureFormat_RGBA32 ? "RGBA32" : "Alpha8",
                        tex->BytesPerPixel, int(tex->UseColors), visible, colored, nonGrey,
                        double(tex->GetSizeInBytes()) / (1024.0 * 1024.0));
        }
        ImGui::DestroyContext(ctx);
    }
}

// ============================================================================
// ARM L -- the section 3.2 question of votv-imgui-192-upgrade-DESIGN-2026-07-30:
// under a STRAIGHT port (legacy backend, RendererHasTextures NOT set, which is
// what imgui_impl_dx12.cpp:987 forces on DX12), what happens when a codepoint
// OUTSIDE the preloaded GlyphRanges is drawn?
//
// Under 1.91.5 the answer was flat: FallbackGlyph, nothing else moved. Under
// 1.92 ImFontBaked_BuildLoadGlyph is NOT gated on the capability flag
// (imgui_draw.cpp:4562-4571), so the glyph BAKES -- into an atlas a legacy
// backend uploaded exactly once. Reading the source cannot say whether the
// result is a correct draw, one invisible glyph, or a whole UI of misaligned
// text, because that depends on whether the pack GROWS or REPACKS the texture.
// So: measure it, on the REAL atlas (same families, same per-role px, same
// dedup, same generated repertoire, same donor as ui::fonts::Load).
//
// The probe codepoints are not synthetic: section 2.7 measured that all seven
// text faces carry General Punctuation and the ruble sign while the shipped
// repertoire excludes them. "A Russian speaker types an em dash in chat" is
// exactly this case.
// ============================================================================

// ---- mirrored from src/ui/fonts.cpp + config_registry_rows.inc -------------
constexpr float kNameplatePx = 16.f;
struct FamilyL { const char* regular; const char* bold; };
const FamilyL kFamiliesL[] = {
    { "JetBrainsMono-Regular.ttf", "JetBrainsMono-Bold.ttf" },
    { "Roboto-Regular.ttf",        "Roboto-Bold.ttf"        },
    { "CascadiaCode-Regular.ttf",  "CascadiaCode-Bold.ttf"  },
    { "FSEX300.ttf",               "FSEX300.ttf"            },  // single weight
};
constexpr int kFamilyCountL = 4;
struct RoleL { const char* label; float px; bool bold; int fam; };
// kFontRoleDefaultFamily: menu/chat/toast = Fixedsys(3), net/nameplate = Roboto(1).
const RoleL kRolesL[] = {
    { "Menu",      kUiPx,        false, 3 },
    { "Chat",      kChatPx,      true,  3 },
    { "Net",       kUiPx,        false, 1 },
    { "Nameplate", kNameplatePx, false, 1 },
    { "Toast",     kUiPx,        false, 3 },
};
constexpr int kRoleCountL = 5;

// THE REAL generated table -- the same rows coop::text::InRepertoire folds
// against and ui::fonts::Load bakes. Not a hand-written approximation: the
// whole question is what happens at ITS boundary.
struct RngL { unsigned begin, end; };
constexpr RngL kRepertoireRows[] = {
#include "coop/text/repertoire_ranges.inc"
};
constexpr int kRepertoireRowCount = int(sizeof(kRepertoireRows) / sizeof(kRepertoireRows[0]));

const ImWchar* RepertoireRangesImGui() {
    static std::vector<ImWchar> v;
    if (v.empty()) {
        for (const RngL& r : kRepertoireRows) {
            v.push_back(ImWchar(r.begin));
            v.push_back(ImWchar(r.end));
        }
        v.push_back(0);
    }
    return v.data();
}

bool InRepertoireL(unsigned cp) {
    for (const RngL& r : kRepertoireRows)
        if (cp >= r.begin && cp <= r.end) return true;
    return false;
}

std::string Utf8Of(unsigned c) {
    char buf[5]; int len = 0;
    if (c < 0x80)         { buf[0] = char(c); len = 1; }
    else if (c < 0x800)   { buf[0] = char(0xC0 | (c >> 6));  buf[1] = char(0x80 | (c & 0x3F)); len = 2; }
    else if (c < 0x10000) { buf[0] = char(0xE0 | (c >> 12)); buf[1] = char(0x80 | ((c >> 6) & 0x3F)); buf[2] = char(0x80 | (c & 0x3F)); len = 3; }
    else                  { buf[0] = char(0xF0 | (c >> 18)); buf[1] = char(0x80 | ((c >> 12) & 0x3F)); buf[2] = char(0x80 | ((c >> 6) & 0x3F)); buf[3] = char(0x80 | (c & 0x3F)); len = 4; }
    return std::string(buf, len);
}

// Exactly ui::fonts::BakeEmbeddedRoles + MergeBackstops, on files instead of RCDATA.
std::vector<ImFont*> BakeRealRoles(ImFontAtlas* atlas, float scale, const ImWchar* ranges) {
    struct Cache { int fam, pxq; bool bold; ImFont* font; };
    std::vector<Cache> cache;
    std::vector<ImFont*> roleFonts(kRoleCountL, nullptr);
    for (int r = 0; r < kRoleCountL; ++r) {
        const RoleL& rd = kRolesL[r];
        const float px  = rd.px * scale;
        const int   pxq = int(px * 4.f + 0.5f);
        ImFont* font = nullptr;
        for (const Cache& c : cache)
            if (c.fam == rd.fam && c.pxq == pxq && c.bold == rd.bold) { font = c.font; break; }
        if (!font) {
            ImFontConfig cfg;
            font = atlas->AddFontFromFileTTF(
                (g_assetDir + (rd.bold ? kFamiliesL[rd.fam].bold : kFamiliesL[rd.fam].regular)).c_str(),
                px, &cfg, ranges);
            if (!font) { std::printf("  !! missing face for role %s\n", rd.label); continue; }
            // MergeBackstops: the other three families, then the colour donor.
            ImFontConfig merge; merge.MergeMode = true;
            for (int o = 0; o < kFamilyCountL; ++o) {
                if (o == rd.fam) continue;
                atlas->AddFontFromFileTTF(
                    (g_assetDir + (rd.bold ? kFamiliesL[o].bold : kFamiliesL[o].regular)).c_str(),
                    px, &merge, ranges);
            }
            ImFontConfig donor = merge;
            donor.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LoadColor;
            atlas->AddFontFromFileTTF((g_assetDir + "TwemojiMozilla-Subset.ttf").c_str(),
                                      px, &donor, ranges);
            cache.push_back({ rd.fam, pxq, rd.bold, font });
        }
        roleFonts[r] = font;
    }
    // Distinct faces only, in bake order.
    std::vector<ImFont*> faces;
    for (const Cache& c : cache) faces.push_back(c.font);
    return faces;
}

int TotalGlyphs(const std::vector<ImFont*>& faces) {
    int n = 0;
    for (ImFont* f : faces)
        if (ImFontBaked* b = f->GetFontBaked(f->LegacySize)) n += b->Glyphs.Size;
    return n;
}

void LegacyDemandBake() {
    std::printf("=== ARM L: legacy backend (RendererHasTextures OFF) meets an "
                "out-of-repertoire codepoint ===\n");
    std::printf("  repertoire: %d rows (the real generated table)\n", kRepertoireRowCount);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920, 1080);
    io.DeltaTime   = 1.0f / 60.0f;
    // DELIBERATELY NOT SET -- this is the whole point of the arm.
    // io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->TexMaxWidth = 4096;

    std::vector<ImFont*> faces = BakeRealRoles(io.Fonts, 1.0f, RepertoireRangesImGui());
    if (faces.empty()) { std::printf("  ABORT: no faces baked\n"); ImGui::DestroyContext(ctx); return; }

    // What our overlay does, once, at Load(): Build() then GetTexDataAsRGBA32.
    const bool built = io.Fonts->Build();
    unsigned char* px = nullptr; int w = 0, h = 0, bpp = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h, &bpp);
    ImTextureData* tex0 = io.Fonts->TexData;
    std::printf("  after Load(): built=%d faces=%d tex=%p %dx%d bpp=%d glyphs=%d TexList=%d\n",
                int(built), int(faces.size()), (void*)tex0, w, h, bpp,
                TotalGlyphs(faces), io.Fonts->TexList.Size);

    // THE UPLOAD. A legacy backend copies these bytes to the GPU exactly once
    // (overlay_backend_dx11/dx12 do it inside Load()'s frame and never again).
    std::vector<unsigned char> uploaded(size_t(w) * size_t(h) * 4);
    if (px) std::memcpy(uploaded.data(), px, uploaded.size());

    // Witnesses: glyphs that were IN the preload and are already on the GPU.
    struct Witness { unsigned cp; const char* name; ImVec4 uv; bool had; };
    Witness wit[] = {
        { 0x0041,  "'A'",     {}, false },
        { 0x0410,  "U+0410",  {}, false },
        { 0xFFFD,  "U+FFFD",  {}, false },
        { 0x1F600, "U+1F600", {}, false },
    };
    ImFont* face0 = faces[0];
    ImFontBaked* baked0 = face0->GetFontBaked(face0->LegacySize);
    for (Witness& t : wit)
        if (baked0)
            if (const ImFontGlyph* g = baked0->FindGlyphNoFallback((ImWchar)t.cp)) {
                t.uv = ImVec4(g->U0, g->V0, g->U1, g->V1); t.had = true;
            }

    // The out-of-repertoire probes, in the order a real user would produce them.
    struct Probe { unsigned cp; const char* name; };
    const Probe probes[] = {
        { 0x2014, "EM DASH" },        { 0x2026, "ELLIPSIS" },
        { 0x201C, "LEFT DQUOTE" },    { 0x201D, "RIGHT DQUOTE" },
        { 0x20BD, "RUBLE SIGN" },     { 0x2116, "NUMERO SIGN" },
        { 0x2192, "RIGHTWARDS ARROW" }, { 0x00B1, "PLUS-MINUS (in repertoire, control)" },
    };

    // ------------------------------------------------------------------------
    // THE WINDOW MATTERS, AND IT COST THIS PROBE ONE FALSE VERDICT.
    // imgui.cpp:9089 UpdateFontsNewFrame sets atlas->Locked = true whenever
    // RendererHasTextures is off, and imgui.cpp:6174 UpdateFontsEndFrame clears
    // it. ImFontBaked_BuildLoadGlyph's first test is that same Locked flag
    // (imgui_draw.cpp:4565). So the legacy regime ONLY holds between NewFrame
    // and EndFrame -- and the first version of this arm inspected the atlas
    // AFTER Render(), in the unlocked gap, where FindGlyphNoFallback BAKES on
    // miss. It measured its own instrument and reported "the glyph bakes".
    // [[lesson-querying-a-lazy-cache-populates-it]]
    //
    // So every observation below is taken INSIDE the frame. Reading Glyphs.Size
    // is a pure read; FindGlyphNoFallback inside the frame cannot bake because
    // Locked short-circuits it, which is exactly the property under test.
    // ------------------------------------------------------------------------
    struct Obs {
        bool  locked = false, texIsBuilt = false, present = false;
        int   glyphs = 0, texListSize = 0, texW = 0, texH = 0;
        void* tex = nullptr;
        int   uvMoved = 0;
    };

    std::string acc = "The quick brown fox 0123456789 ";
    for (const Probe& p : probes) {
        g_asserts.clear();
        acc += Utf8Of(p.cp);
        Obs before{}, after{};

        auto observe = [&](Obs& o, unsigned cp) {
            o.locked      = io.Fonts->Locked;
            o.texIsBuilt  = io.Fonts->TexIsBuilt;
            o.tex         = (void*)io.Fonts->TexData;
            o.texW        = io.Fonts->TexData ? io.Fonts->TexData->Width  : 0;
            o.texH        = io.Fonts->TexData ? io.Fonts->TexData->Height : 0;
            o.texListSize = io.Fonts->TexList.Size;
            for (ImFont* f : faces)
                if (ImFontBaked* b = f->GetFontBaked(f->LegacySize)) o.glyphs += b->Glyphs.Size;
            ImFontBaked* b0 = face0->GetFontBaked(face0->LegacySize);
            o.present = b0 && b0->FindGlyphNoFallback((ImWchar)cp) != nullptr;
            for (Witness& t : wit) {
                if (!t.had || !b0) continue;
                const ImFontGlyph* g = b0->FindGlyphNoFallback((ImWchar)t.cp);
                if (!g) { ++o.uvMoved; continue; }
                if (g->U0 != t.uv.x || g->V0 != t.uv.y ||
                    g->U1 != t.uv.z || g->V1 != t.uv.w) ++o.uvMoved;
            }
        };

        for (int frame = 0; frame < 2; ++frame) {
            ImGui::NewFrame();
            if (frame == 0) observe(before, p.cp);   // in-frame, before the draw
            ImGui::Begin("probe");
            for (ImFont* f : faces) {
                ImGui::PushFont(f, f->LegacySize);
                ImGui::TextUnformatted(acc.c_str());
                ImGui::PopFont();
            }
            ImGui::End();
            if (frame == 1) observe(after, p.cp);    // in-frame, after the draw
            ImGui::Render();
            // NO ServiceTextures() -- a legacy backend does not implement the
            // texture protocol at all. That absence IS the regime under test.
        }

        // Does the CPU buffer still agree with what the backend uploaded once?
        long long texelsChanged = -1;
        ImTextureData* tex = io.Fonts->TexData;
        if (tex && tex->Pixels && tex->Width == w && tex->Height == h) {
            texelsChanged = 0;
            const unsigned* a = (const unsigned*)uploaded.data();
            const unsigned* c = (const unsigned*)tex->Pixels;
            for (long long i = 0; i < (long long)w * h; ++i) if (a[i] != c[i]) ++texelsChanged;
        }

        const bool grew = (after.glyphs != before.glyphs);
        std::printf("  %-33s in-rep=%d  locked=%d  glyphs %d->%-5d present-after=%-3s "
                    "TexIsBuilt=%d tex=%p %dx%d TexList=%d UV-moved=%d diverged=%lld  -> %s\n",
                    p.name, int(InRepertoireL(p.cp)), int(after.locked),
                    before.glyphs, after.glyphs, after.present ? "YES" : "no",
                    int(after.texIsBuilt), after.tex, after.texW, after.texH,
                    after.texListSize, after.uvMoved, texelsChanged,
                    grew ? "*** BAKED ON DEMAND ***"
                         : (after.present ? "was already baked"
                                          : "NOT baked -- draws FallbackGlyph (1.91.5 behaviour)"));
        for (const std::string& a : g_asserts)
            std::printf("      imgui error: %s\n", a.c_str());
    }

    // ------------------------------------------------------------------------
    // THE HAZARD THE ABOVE RULES OUT FOR DRAWING BUT NOT FOR US. Any of OUR
    // code that queries a glyph OUTSIDE NewFrame..EndFrame is in the unlocked
    // gap, so it bakes -- and ui/fonts.cpp:265,285,287 (the boot font selftest)
    // is exactly that: Load() runs before ImGui_ImplWin32_Init, with no frame in
    // scope at all. Measure what that one query does to a legacy build.
    // ------------------------------------------------------------------------
    std::printf("  --- the out-of-frame query (what ui/fonts.cpp's selftest does) ---\n");
    {
        const bool builtBefore = io.Fonts->TexIsBuilt;
        int glyphsBefore = 0;
        for (ImFont* f : faces)
            if (ImFontBaked* b = f->GetFontBaked(f->LegacySize)) glyphsBefore += b->Glyphs.Size;
        g_asserts.clear();
        ImFontBaked* b0 = face0->GetFontBaked(face0->LegacySize);
        const ImFontGlyph* g = b0 ? b0->FindGlyphNoFallback(0x2E18) : nullptr;  // outside the repertoire
        int glyphsAfter = 0;
        for (ImFont* f : faces)
            if (ImFontBaked* b = f->GetFontBaked(f->LegacySize)) glyphsAfter += b->Glyphs.Size;
        std::printf("    Locked=%d (no frame in scope)  FindGlyphNoFallback(U+2E18) -> %s  "
                    "glyphs %d->%d  TexIsBuilt %d->%d\n",
                    int(io.Fonts->Locked), g ? "glyph" : "NULL", glyphsBefore, glyphsAfter,
                    int(builtBefore), int(io.Fonts->TexIsBuilt));
        // If TexIsBuilt just went false, every subsequent legacy frame raises the
        // imgui_draw.cpp:2815 user error -- forever, because nothing re-uploads.
        ImGui::NewFrame();
        ImGui::Begin("probe"); ImGui::TextUnformatted("after"); ImGui::End();
        ImGui::Render();
        for (const std::string& a : g_asserts)
            std::printf("      imgui error: %s\n", a.c_str());
    }

    ImGui::DestroyContext(ctx);
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: atlas_probe_192 <assets/fonts dir> <donor dir>\n"); return 2; }
    g_assetDir = argv[1]; g_donorDir = argv[2];
    if (g_assetDir.back() != '\\' && g_assetDir.back() != '/') g_assetDir += "\\";
    if (g_donorDir.back() != '\\' && g_donorDir.back() != '/') g_donorDir += "\\";
    std::printf("imgui %s   ImWchar = %d-bit   dynamic-font path (RendererHasTextures)\n\n",
                IMGUI_VERSION, int(sizeof(ImWchar) * 8));
    LegacyDemandBake();
    std::printf("=== COLR raster + format cost, on the arm that SHIPS ===\n");
    ColorRasterCheck192();
    std::printf("\n");
    for (float s : { 1.0f, 2.0f }) { Run(s, false); Run(s, true); }
    return 0;
}
