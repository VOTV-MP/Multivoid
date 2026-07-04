// ui/fonts.cpp -- see ui/fonts.h.

#include "ui/fonts.h"

#include "harness/config.h"
#include "ui/scale.h"
#include "ue_wrap/log.h"

#include "imgui.h"

#include <windows.h>

#include <string>

#include "../../resources/font_resource_ids.h"

namespace ui::fonts {
namespace {

ImFont* g_chat = nullptr;
float   g_chatPx = kChatPx;      // px the chat font was last baked at
Family  g_family = Family::Roboto;  // default (user 2026-07-04, after comparing: "Roboto самый лучший")
bool    g_familyRead = false;    // ini read once; SetFamily overrides after

struct FamilyDesc {
    const char* iniValue;  // votv-coop.ini ui.font token
    const char* label;     // UI label
    int regularId;         // RCDATA ids
    int boldId;
};
constexpr FamilyDesc kFamilies[kFamilyCount] = {
    { "jetbrains", "JetBrains Mono", IDR_FONT_JBMONO_REGULAR,   IDR_FONT_JBMONO_BOLD },
    { "roboto",    "Roboto",         IDR_FONT_ROBOTO_REGULAR,   IDR_FONT_ROBOTO_BOLD },
    { "cascadia",  "Cascadia Code",  IDR_FONT_CASCADIA_REGULAR, IDR_FONT_CASCADIA_BOLD },
};

Family FamilyFromIni() {
    const std::string v = harness::config::ReadIniValue("ui.font", "roboto");
    for (int i = 0; i < kFamilyCount; ++i)
        if (v == kFamilies[i].iniValue) return static_cast<Family>(i);
    return Family::Roboto;
}

// Locate an RCDATA TTF embedded in OUR module (not the game exe).
const void* ResourceTtf(int id, int* outSize) {
    *outSize = 0;
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ResourceTtf), &self);
    if (!self) return nullptr;
    HRSRC res = ::FindResourceW(self, MAKEINTRESOURCEW(id), reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!res) return nullptr;
    HGLOBAL glob = ::LoadResource(self, res);
    if (!glob) return nullptr;
    const DWORD sz = ::SizeofResource(self, res);
    const void* p = ::LockResource(glob);
    if (!p || sz == 0) return nullptr;
    *outSize = static_cast<int>(sz);
    return p;
}

ImFont* AddFromResource(int id, float px, const ImFontConfig& baseCfg, const ImWchar* ranges) {
    int sz = 0;
    const void* data = ResourceTtf(id, &sz);
    if (!data) return nullptr;
    // The resource lives in the mapped DLL image for the process lifetime, so the
    // atlas must NOT take ownership (it would FREE a resource pointer on rebuild --
    // and Load() rebuilds on every scale/family change).
    // const_cast: ImGui's signature takes void* but only reads when not owned.
    ImFontConfig cfg = baseCfg;
    cfg.FontDataOwnedByAtlas = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        const_cast<void*>(data), sz, px, &cfg, ranges);
}

ImFont* AddFromFile(const std::string& path, float px, const ImFontConfig& cfg,
                    const ImWchar* ranges) {
    const DWORD attrs = ::GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return nullptr;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg, ranges);
}

}  // namespace

void Load() {
    ImGuiIO& io = ImGui::GetIO();
    // Re-entrant: a scale/family change re-bakes the whole atlas. Clear drops the
    // previous fonts + pixel data (our TTF pointers survive -- not atlas-owned).
    io.Fonts->Clear();
    g_chat = nullptr;

    if (!g_familyRead) {
        g_family = FamilyFromIni();
        g_familyRead = true;
    }
    const FamilyDesc& fam = kFamilies[static_cast<int>(g_family)];

    // Bake at the REAL pixel size for the live resolution (ui::scale) -- never
    // io.FontGlobalScale (that stretches the 1x bitmap and blurs).
    const float s = ui::scale::Ui();
    const float uiPx = kUiPx * s;
    const float chatPx = kChatPx * s;
    g_chatPx = chatPx;

    ImFontConfig cfg;
    // No OversampleH/V: the freetype builder (IMGUI_ENABLE_FREETYPE makes it the
    // default) IGNORES oversampling (imgui_freetype.cpp "FIXME: not supported")
    // and hints properly instead -- setting it would only bloat a hypothetical
    // stb fallback's atlas 4x (audit WARN-3).
    // Default (Basic Latin + Latin-1) + Cyrillic (0400-045F, 2DE0-2DFF, A640-A69F --
    // all of Russian). Every vendored family covers the set (cmap-verified);
    // the system fallbacks do too.
    const ImWchar* ranges = io.Fonts->GetGlyphRangesCyrillic();

    // PRIMARY: the chosen family embedded in the DLL as RCDATA (no loose files,
    // RULE 3). The FIRST font added becomes ImGui's default, so the whole overlay
    // picks it up with no per-window changes; the bold chat-size face is a second
    // atlas entry.
    ImFont* base = AddFromResource(fam.regularId, uiPx, cfg, ranges);
    if (base) {
        ImFont* bold = AddFromResource(fam.boldId, chatPx, cfg, ranges);
        g_chat = bold ? bold : base;
        UE_LOGI("fonts: overlay font = %s (embedded; ui %.0f px, chat %s %.0f px, "
                "scale %.2f, Cyrillic, freetype)",
                fam.label, uiPx, bold ? "bold" : "regular", chatPx, s);
        return;
    }

    // FALLBACK: Windows system fonts (an unthinkable resource-load failure).
    char windir[MAX_PATH] = {};
    ::GetWindowsDirectoryA(windir, sizeof(windir));
    const std::string win = windir[0] ? std::string(windir) + "\\Fonts\\" : std::string();
    struct Cand { std::string reg, bold; const char* tag; };
    const Cand cands[] = {
        { win + "tahoma.ttf",  win + "tahomabd.ttf", "Tahoma (system)" },
        { win + "segoeui.ttf", win + "segoeuib.ttf", "Segoe UI (system)" },
    };
    for (const Cand& c : cands) {
        base = AddFromFile(c.reg, uiPx, cfg, ranges);
        if (!base) continue;
        ImFont* bold = AddFromFile(c.bold, chatPx, cfg, ranges);
        g_chat = bold ? bold : base;
        UE_LOGW("fonts: embedded %s unavailable -- overlay font = %s (ui %.0f px, "
                "chat %.0f px, scale %.2f)", fam.label, c.tag, uiPx, chatPx, s);
        return;
    }

    // Last resort: the builtin ProggyClean (ASCII-only) so the overlay still renders.
    io.Fonts->AddFontDefault();
    UE_LOGW("fonts: no font loaded -- overlay stays on the ImGui default "
            "(ASCII-only; Cyrillic renders as '?')");
}

ImFont* Chat() { return g_chat; }

float ChatPx() { return g_chatPx; }

Family CurrentFamily() { return g_family; }

const char* FamilyLabel(Family f) {
    const int i = static_cast<int>(f);
    return (i >= 0 && i < kFamilyCount) ? kFamilies[i].label : "?";
}

void SetFamily(Family f) {
    const int i = static_cast<int>(f);
    if (i < 0 || i >= kFamilyCount || f == g_family) return;
    g_family = f;
    g_familyRead = true;  // the user's live choice wins over the ini read
    harness::config::WriteIniValue("ui.font", kFamilies[i].iniValue);
    ui::scale::RequestRebuild();  // atlas re-bakes before the next frame
}

void OnContextDestroyed() { g_chat = nullptr; }

}  // namespace ui::fonts
