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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: atlas_probe_192 <assets/fonts dir> <donor dir>\n"); return 2; }
    g_assetDir = argv[1]; g_donorDir = argv[2];
    if (g_assetDir.back() != '\\' && g_assetDir.back() != '/') g_assetDir += "\\";
    if (g_donorDir.back() != '\\' && g_donorDir.back() != '/') g_donorDir += "\\";
    std::printf("imgui %s   ImWchar = %d-bit   dynamic-font path (RendererHasTextures)\n\n",
                IMGUI_VERSION, int(sizeof(ImWchar) * 8));
    for (float s : { 1.0f, 2.0f }) { Run(s, false); Run(s, true); }
    return 0;
}
