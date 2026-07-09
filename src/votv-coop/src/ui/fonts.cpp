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

// Per-role baked font + the px it was baked at + the chosen family. Menu is baked
// FIRST so its font is ImGui's default (every un-pushed panel follows it).
ImFont* g_roleFont[kRoleCount]   = {};
float   g_rolePx[kRoleCount]     = {};
Family  g_roleFamily[kRoleCount] = { Family::Fixedsys, Family::Fixedsys,
                                     Family::Fixedsys, Family::Fixedsys };
bool    g_rolesRead = false;     // ini read once; SetRoleFamily overrides after

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
    // VOTV's own terminal pixel font (FSEX300 -> font_terminal). Single weight,
    // so the chat "bold" face reuses Regular. Covers Cyrillic (cmap-verified, 5992 cp).
    { "fixedsys",  "Fixedsys (VOTV)", IDR_FONT_FIXEDSYS_REGULAR, IDR_FONT_FIXEDSYS_REGULAR },
};

struct RoleDesc {
    const char* iniKey;  // ui.font.<iniKey>
    const char* label;   // UI label
    float basePx;        // 1080p base size (baked at basePx * ui::scale)
    bool  bold;          // use the family's Bold face
};
constexpr RoleDesc kRoles[kRoleCount] = {
    { "menu",      "Menu / panels", kUiPx,        false },  // Role::Menu (== ImGui default)
    { "chat",      "Chat",          kChatPx,      true  },  // Role::Chat
    { "net",       "Net stats",     kUiPx,        false },  // Role::Net
    { "nameplate", "Nameplates",    kNameplatePx, false },  // Role::Nameplate
};

Family FamilyFromToken(const std::string& v, Family fallback) {
    for (int i = 0; i < kFamilyCount; ++i)
        if (v == kFamilies[i].iniValue) return static_cast<Family>(i);
    return fallback;
}

// Read the per-role families once. Each ui.font.<role> defaults to the legacy
// global ui.font (default fixedsys) -- so an existing single-font config, or a
// user who only ever set ui.font, keeps that family on every role.
void ReadRoleFamiliesOnce() {
    if (g_rolesRead) return;
    const std::string global = harness::config::ReadIniValue("ui.font", "fixedsys");
    const Family gfam = FamilyFromToken(global, Family::Fixedsys);
    for (int r = 0; r < kRoleCount; ++r) {
        const std::string key = std::string("ui.font.") + kRoles[r].iniKey;
        const std::string v = harness::config::ReadIniValue(key.c_str(), global.c_str());
        g_roleFamily[r] = FamilyFromToken(v, gfam);
    }
    g_rolesRead = true;
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

// Bake all roles from the embedded families, deduping identical (family, px, weight)
// so the common single-family config adds only ONE regular + ONE bold atlas entry.
// Returns true if at least one role's resource baked. Menu (role 0) is baked first
// -> its font is ImGui's default.
bool BakeEmbeddedRoles(float s, const ImFontConfig& cfg, const ImWchar* ranges) {
    struct Baked { int fam; int pxi; bool bold; ImFont* font; };
    Baked cache[kRoleCount];
    int nCache = 0;
    bool any = false;
    for (int r = 0; r < kRoleCount; ++r) {
        const RoleDesc& rd = kRoles[r];
        const int   fi  = static_cast<int>(g_roleFamily[r]);
        const float px  = rd.basePx * s;
        const int   pxi = static_cast<int>(px * 4.f + 0.5f);   // quantize for dedup
        ImFont* font = nullptr;
        for (int c = 0; c < nCache; ++c)
            if (cache[c].fam == fi && cache[c].pxi == pxi && cache[c].bold == rd.bold) {
                font = cache[c].font; break;
            }
        if (!font) {
            const FamilyDesc& fam = kFamilies[fi];
            font = AddFromResource(rd.bold ? fam.boldId : fam.regularId, px, cfg, ranges);
            if (font) { any = true; cache[nCache++] = { fi, pxi, rd.bold, font }; }
        }
        g_roleFont[r] = font;
        g_rolePx[r]   = px;
    }
    return any;
}

}  // namespace

void Load() {
    ImGuiIO& io = ImGui::GetIO();
    // Re-entrant: a scale/family change re-bakes the whole atlas. Clear drops the
    // previous fonts + pixel data (our TTF pointers survive -- not atlas-owned).
    io.Fonts->Clear();
    for (int r = 0; r < kRoleCount; ++r) { g_roleFont[r] = nullptr; g_rolePx[r] = 0.f; }

    ReadRoleFamiliesOnce();

    // Bake at the REAL pixel size for the live resolution (ui::scale) -- never
    // io.FontGlobalScale (that stretches the 1x bitmap and blurs).
    const float s = ui::scale::Ui();

    ImFontConfig cfg;
    // No OversampleH/V: the freetype builder ignores it and hints properly.
    // Default (Basic Latin + Latin-1) + Cyrillic (all of Russian).
    const ImWchar* ranges = io.Fonts->GetGlyphRangesCyrillic();

    // PRIMARY: the per-role families embedded in the DLL as RCDATA (RULE 3, no
    // loose files). Menu is baked first -> ImGui default.
    if (BakeEmbeddedRoles(s, cfg, ranges)) {
        // Any role whose resource somehow failed reuses the default (first baked).
        ImFont* def = g_roleFont[static_cast<int>(Role::Menu)];
        if (!def) for (int r = 0; r < kRoleCount; ++r) if (g_roleFont[r]) { def = g_roleFont[r]; break; }
        for (int r = 0; r < kRoleCount; ++r) if (!g_roleFont[r]) g_roleFont[r] = def;
        UE_LOGI("fonts: roles menu=%s chat=%s net=%s nameplate=%s (embedded; ui %.0f px, "
                "chat %.0f px, scale %.2f, Cyrillic, freetype)",
                kFamilies[static_cast<int>(g_roleFamily[0])].label,
                kFamilies[static_cast<int>(g_roleFamily[1])].label,
                kFamilies[static_cast<int>(g_roleFamily[2])].label,
                kFamilies[static_cast<int>(g_roleFamily[3])].label,
                g_rolePx[0], g_rolePx[1], s);
        return;
    }

    // FALLBACK: one Windows system font for EVERY role (an unthinkable RCDATA
    // failure). One face, shared across roles + the chat size.
    char windir[MAX_PATH] = {};
    ::GetWindowsDirectoryA(windir, sizeof(windir));
    const std::string win = windir[0] ? std::string(windir) + "\\Fonts\\" : std::string();
    struct Cand { std::string reg; const char* tag; };
    const Cand cands[] = {
        { win + "tahoma.ttf",  "Tahoma (system)" },
        { win + "segoeui.ttf", "Segoe UI (system)" },
    };
    for (const Cand& c : cands) {
        ImFont* menu = AddFromFile(c.reg, kUiPx * s, cfg, ranges);
        if (!menu) continue;
        ImFont* chat = AddFromFile(c.reg, kChatPx * s, cfg, ranges);
        g_roleFont[static_cast<int>(Role::Menu)]      = menu; g_rolePx[0] = kUiPx * s;
        g_roleFont[static_cast<int>(Role::Chat)]      = chat ? chat : menu; g_rolePx[1] = (chat ? kChatPx : kUiPx) * s;
        g_roleFont[static_cast<int>(Role::Net)]       = menu; g_rolePx[2] = kUiPx * s;
        g_roleFont[static_cast<int>(Role::Nameplate)] = menu; g_rolePx[3] = kNameplatePx * s;
        UE_LOGW("fonts: embedded families unavailable -- overlay font = %s (all roles; scale %.2f)",
                c.tag, s);
        return;
    }

    // Last resort: the builtin ProggyClean (ASCII-only) so the overlay still renders.
    ImFont* def = io.Fonts->AddFontDefault();
    for (int r = 0; r < kRoleCount; ++r) { g_roleFont[r] = def; g_rolePx[r] = def ? def->FontSize : kUiPx; }
    UE_LOGW("fonts: no font loaded -- overlay stays on the ImGui default "
            "(ASCII-only; Cyrillic renders as '?')");
}

ImFont* FontFor(Role r) {
    const int i = static_cast<int>(r);
    return (i >= 0 && i < kRoleCount) ? g_roleFont[i] : nullptr;
}

float PxFor(Role r) {
    const int i = static_cast<int>(r);
    return (i >= 0 && i < kRoleCount) ? g_rolePx[i] : (kUiPx * ui::scale::Ui());
}

ImFont* Chat()   { return g_roleFont[static_cast<int>(Role::Chat)]; }
float   ChatPx() { return g_rolePx[static_cast<int>(Role::Chat)]; }

const char* FamilyLabel(Family f) {
    const int i = static_cast<int>(f);
    return (i >= 0 && i < kFamilyCount) ? kFamilies[i].label : "?";
}

const char* RoleLabel(Role r) {
    const int i = static_cast<int>(r);
    return (i >= 0 && i < kRoleCount) ? kRoles[i].label : "?";
}

Family RoleFamily(Role r) {
    ReadRoleFamiliesOnce();
    const int i = static_cast<int>(r);
    return (i >= 0 && i < kRoleCount) ? g_roleFamily[i] : Family::Fixedsys;
}

void SetRoleFamily(Role r, Family f) {
    const int ri = static_cast<int>(r);
    const int fi = static_cast<int>(f);
    if (ri < 0 || ri >= kRoleCount || fi < 0 || fi >= kFamilyCount) return;
    ReadRoleFamiliesOnce();
    if (g_roleFamily[ri] == f) return;
    g_roleFamily[ri] = f;
    g_rolesRead = true;  // the user's live choice wins over the ini read
    const std::string key = std::string("ui.font.") + kRoles[ri].iniKey;
    harness::config::WriteIniValue(key.c_str(), kFamilies[fi].iniValue);
    ui::scale::RequestRebuild();  // atlas re-bakes before the next frame
}

void OnContextDestroyed() {
    for (int r = 0; r < kRoleCount; ++r) g_roleFont[r] = nullptr;
}

}  // namespace ui::fonts
