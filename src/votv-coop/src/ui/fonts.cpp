// ui/fonts.cpp -- see ui/fonts.h.

#include "ui/fonts.h"

#include "ue_wrap/log.h"

#include "imgui.h"

#include <windows.h>

#include <string>

#include "../../resources/font_resource_ids.h"

namespace ui::fonts {
namespace {

ImFont* g_chat = nullptr;

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
    // atlas must NOT take ownership (it would FREE a resource pointer on rebuild).
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
    if (g_chat) return;
    ImGuiIO& io = ImGui::GetIO();

    ImFontConfig cfg;
    // 2x oversampling (the chat-imgui-samp values): crisper glyphs at UI size for a
    // one-time atlas-size cost.
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    // Default (Basic Latin + Latin-1) + Cyrillic (0400-045F, 2DE0-2DFF, A640-A69F --
    // all of Russian). Roboto covers the set (cmap-verified); the fallbacks do too.
    const ImWchar* ranges = io.Fonts->GetGlyphRangesCyrillic();

    // PRIMARY: Roboto embedded in the DLL as RCDATA (user 2026-07-04: the font
    // everywhere + no loose files). The FIRST font added becomes ImGui's default,
    // so the whole overlay picks it up with no per-window changes; the bold
    // chat-size face is a second atlas entry.
    ImFont* base = AddFromResource(IDR_FONT_ROBOTO_REGULAR, kUiPx, cfg, ranges);
    if (base) {
        ImFont* bold = AddFromResource(IDR_FONT_ROBOTO_BOLD, kChatPx, cfg, ranges);
        g_chat = bold ? bold : base;
        UE_LOGI("fonts: overlay font = Roboto (embedded; ui %.0f px, chat %s %.0f px, "
                "Cyrillic, 2x oversample)", kUiPx, bold ? "bold" : "regular", kChatPx);
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
        base = AddFromFile(c.reg, kUiPx, cfg, ranges);
        if (!base) continue;
        ImFont* bold = AddFromFile(c.bold, kChatPx, cfg, ranges);
        g_chat = bold ? bold : base;
        UE_LOGW("fonts: embedded Roboto unavailable -- overlay font = %s (ui %.0f px, "
                "chat %.0f px)", c.tag, kUiPx, kChatPx);
        return;
    }

    // Last resort: the builtin ProggyClean (ASCII-only) so the overlay still renders.
    io.Fonts->AddFontDefault();
    UE_LOGW("fonts: no font loaded -- overlay stays on the ImGui default "
            "(ASCII-only; Cyrillic renders as '?')");
}

ImFont* Chat() { return g_chat; }

void OnContextDestroyed() { g_chat = nullptr; }

}  // namespace ui::fonts
