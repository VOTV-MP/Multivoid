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
#include <string>
#include <vector>

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
    for (const FaceSpec& s : specs) {
        ImFontConfig cfg;
        ImFont* f = io.Fonts->AddFontFromFileTTF((g_assetDir + s.file).c_str(), s.px, &cfg);
        if (withDonors && f) {
            ImFontConfig m; m.MergeMode = true;
            io.Fonts->AddFontFromFileTTF((g_donorDir + "NotoSansSC-Regular.ttf").c_str(), s.px, &m);
            io.Fonts->AddFontFromFileTTF((g_donorDir + "NotoSansJP-Regular.ttf").c_str(), s.px, &m);
            io.Fonts->AddFontFromFileTTF((g_donorDir + "Twemoji.Mozilla.ttf").c_str(), s.px, &m);
        }
        if (f) fonts.push_back(f);
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
        io.Fonts->AddFontFromFileTTF((g_donorDir + "Twemoji.Mozilla.ttf").c_str(), 32.f, &m);

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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: atlas_probe_192 <assets/fonts dir> <donor dir>\n"); return 2; }
    g_assetDir = argv[1]; g_donorDir = argv[2];
    if (g_assetDir.back() != '\\' && g_assetDir.back() != '/') g_assetDir += "\\";
    if (g_donorDir.back() != '\\' && g_donorDir.back() != '/') g_donorDir += "\\";
    std::printf("imgui %s   ImWchar = %d-bit   dynamic-font path (RendererHasTextures)\n\n",
                IMGUI_VERSION, int(sizeof(ImWchar) * 8));
    std::printf("=== COLR raster + format cost, on the arm that SHIPS ===\n");
    ColorRasterCheck192();
    std::printf("\n");
    for (float s : { 1.0f, 2.0f }) { Run(s, false); Run(s, true); }
    return 0;
}
