// ui/fonts.cpp -- see ui/fonts.h.

#include "ui/fonts.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/text/repertoire.h"
#include "ui/scale.h"
#include "ue_wrap/core/log.h"

#include "imgui.h"
#include "misc/freetype/imgui_freetype.h"

#include <windows.h>

#include <string>
#include <vector>

#include "../../resources/font_resource_ids.h"

namespace ui::fonts {
namespace {

// Per-role baked font + the px it was baked at + the chosen family. Menu is baked
// FIRST so its font is ImGui's default (every un-pushed panel follows it).
ImFont* g_roleFont[kRoleCount]   = {};
float   g_rolePx[kRoleCount]     = {};
// Filled by ReadRoleFamiliesOnce (every consumer path calls it first); the
// per-role DEFAULT assignment lives ONLY in the registry row list (arc 3 --
// config_registry::kFontRoleDefaultFamily; the old per-role default-family
// column of the local RoleDesc table is retired).
Family  g_roleFamily[kRoleCount] = {};
bool    g_rolesRead = false;     // ini read once; SetRoleFamily overrides after

// The ini TOKEN spelling per family lives in the config registry
// (config_registry::kFontFamilyTokens, same Family order) -- the registry owns
// every key/value spelling (T2, arc 2); this table keeps the UI-only columns.
struct FamilyDesc {
    const char* label;     // UI label
    int regularId;         // RCDATA ids
    int boldId;
};
constexpr FamilyDesc kFamilies[kFamilyCount] = {
    { "JetBrains Mono", IDR_FONT_JBMONO_REGULAR,   IDR_FONT_JBMONO_BOLD },
    { "Roboto",         IDR_FONT_ROBOTO_REGULAR,   IDR_FONT_ROBOTO_BOLD },
    { "Cascadia Code",  IDR_FONT_CASCADIA_REGULAR, IDR_FONT_CASCADIA_BOLD },
    // VOTV's own terminal pixel font (FSEX300 -> font_terminal). Single weight,
    // so the chat "bold" face reuses Regular. Covers Cyrillic (cmap-verified, 5992 cp).
    { "Fixedsys (VOTV)", IDR_FONT_FIXEDSYS_REGULAR, IDR_FONT_FIXEDSYS_REGULAR },
};
static_assert(coop::config_registry::kFontFamilyCount ==
                  static_cast<size_t>(kFamilyCount),
              "Family enum and config_registry::kFontFamilyTokens must stay in lockstep");
inline const char* FamilyToken(int fi) {
    return coop::config_registry::kFontFamilyTokens[fi];
}

// The ini key SUFFIX per role lives in the config registry
// (config_registry::kFontRoleKeys, same Role order) -- the composed key family
// "ui.font.<role>" is enumerated there by reference (T2/F41); this table keeps
// the UI-only columns.
struct RoleDesc {
    const char* label;       // UI label
    float  basePx;           // 1080p base size (baked at basePx * ui::scale)
    bool   bold;             // use the family's Bold face
};
constexpr RoleDesc kRoles[kRoleCount] = {
    { "Menu / panels", kUiPx,        false },  // Role::Menu (== ImGui default)
    { "Chat",          kChatPx,      true  },  // Role::Chat
    { "Net stats",     kUiPx,        false },  // Role::Net
    { "Nameplates",    kNameplatePx, false },  // Role::Nameplate
    { "Release toast", kUiPx,        false },  // Role::Toast (our update/version toast)
};
// Per-role default family: the user-2026-07-09 assignment, owned by the
// registry row list since arc 3 (menu/chat/toast=fixedsys, net/nameplate=roboto).
inline Family RoleDefaultFam(int r) {
    return static_cast<Family>(coop::config_registry::kFontRoleDefaultFamily[r]);
}
static_assert(coop::config_registry::kFontRoleCount == static_cast<size_t>(kRoleCount),
              "Role enum and config_registry::kFontRoleKeys must stay in lockstep");

Family FamilyFromToken(const std::string& v, Family fallback) {
    for (int i = 0; i < kFamilyCount; ++i)
        if (v == FamilyToken(i)) return static_cast<Family>(i);
    return fallback;
}

// Read the per-role families once. Each ui.font.<role> defaults to that ROLE's own
// designated default family (user 2026-07-09: menu/chat/toast = Fixedsys, nameplate/
// net = Roboto) -- NOT a single global, so the surfaces differ out of the box.
void ReadRoleFamiliesOnce() {
    if (g_rolesRead) return;
    for (int r = 0; r < kRoleCount; ++r) {
        // The per-role Enum ROW (arc 3): ResolveEnum returns the canonical
        // family token, or the role's own default token on absent/garbage --
        // the same fallback FamilyFromToken applied before, now sweep-visible.
        const std::string v =
            coop::config::ResolveEnum(coop::config_registry::FontRoleRow(static_cast<size_t>(r)));
        g_roleFamily[r] = FamilyFromToken(v, RoleDefaultFam(r));
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

// The repertoire (coop/text/repertoire.h) in ImGui's range form. THE SAME TABLE
// the nickname arbiter folds against -- that is the whole construction: a name
// is unique on screen because what we BAKE and what we FOLD are one generated
// constant rather than two lists that drift.
//
// ImWchar must be 32-bit here: 1,232 of the donor's 1,418 codepoints live above
// the BMP, so on a 16-bit build the ranges could not even express U+1F600 and
// the emoji half of the repertoire would silently not exist.
static_assert(sizeof(ImWchar) == 4,
              "IMGUI_USE_WCHAR32 must be on: the emoji repertoire is astral");
const ImWchar* Repertoire() {
    static std::vector<ImWchar> v;
    if (v.empty()) {
        size_t n = 0;
        const coop::text::CodepointRange* r = coop::text::RepertoireRanges(&n);
        v.reserve(n * 2 + 1);
        for (size_t i = 0; i < n; ++i) {
            v.push_back(static_cast<ImWchar>(r[i].begin));
            v.push_back(static_cast<ImWchar>(r[i].end));
        }
        v.push_back(0);
    }
    return v.data();
}

// Merge the OTHER embedded families, then the colour donor, into the face just
// added. Two defects this closes, both live in b132:
//
//   - JetBrains Mono has no U+0400, U+040D, U+0450 or U+045D. Four Cyrillic
//     letters that the other three families all carry, invisible to every drill
//     because the drills ran on Fixedsys and Roboto. A backstop costs zero DLL
//     bytes and closes it for every family at once.
//   - Six of the seven faces have no U+FFFD, so an absent codepoint fell back to
//     '?' -- indistinguishable from a name that really contains one.
//
// ORDER IS THE POLICY. imgui_freetype.cpp:515 refuses to overwrite a glyph an
// earlier source already provided, so the chosen family wins wherever it HAS the
// glyph and the backstops only fill holes. The donor goes last for the same
// reason: a family that draws its own dingbat keeps drawing it.
void MergeBackstops(int chosenFamily, bool bold, float px, const ImWchar* ranges) {
    ImFontConfig merge;
    merge.MergeMode = true;
    for (int o = 0; o < kFamilyCount; ++o) {
        if (o == chosenFamily) continue;
        AddFromResource(bold ? kFamilies[o].boldId : kFamilies[o].regularId, px, merge, ranges);
    }
    // ImGuiFreeTypeBuilderFlags_LoadColor is not optional: without it the COLR
    // layers are skipped and every emoji bakes with visible=0 -- INVISIBLE, not
    // missing, which is exactly the state a "did the donor load?" check passes.
    ImFontConfig donor = merge;
    donor.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_LoadColor;
    AddFromResource(IDR_FONT_EMOJI_DONOR, px, donor, ranges);
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
            if (font) {
                MergeBackstops(fi, rd.bold, px, ranges);
                any = true;
                cache[nCache++] = { fi, pxi, rd.bold, font };
            }
        }
        g_roleFont[r] = font;
        g_rolePx[r]   = px;
    }
    return any;
}

// ASSERT THE PHENOMENON, NOT THE PRECONDITION. "Did the donor resource load?"
// goes GREEN on a build compiled without ImGuiFreeTypeBuilderFlags_LoadColor,
// where every COLR glyph bakes with Visible == 0 -- invisible, not missing, so
// the atlas is full of emoji nobody can see and every check passes. What is
// actually claimed is that a coloured emoji reached the texture, so that is what
// is measured: the glyph carries Colored, and its box in the atlas contains
// texels whose channels are not all equal (a greyscale mask cannot).
// [[lesson-an-instrument-blind-to-the-phenomenon-always-passes]]
//
// Logged, never a gate. A player whose atlas came out short should still be able
// to join and see everyone; the FOLD is on a build constant either way, so the
// names stay consistent across peers regardless of what baked here.
void RunFontRepertoireSelftestOnce() {
    static bool done = false;
    if (done) return;
    done = true;

    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (!atlas->IsBuilt() && !atlas->Build()) {
        UE_LOGE("font selftest: FAIL -- the atlas did not build");
        return;
    }
    ImFont* f = g_roleFont[static_cast<int>(Role::Nameplate)];
    if (!f) { UE_LOGE("font selftest: FAIL -- no nameplate face"); return; }

    int pass = 0, total = 0;
    auto ok = [&](bool cond, const char* what) {
        ++total;
        if (cond) ++pass;
        else UE_LOGE("font selftest: FAIL -- %s", what);
    };

    const ImFontGlyph* emoji = f->FindGlyphNoFallback(0x1F600);
    ok(emoji != nullptr, "the donor baked U+1F600 (grinning face)");
    ok(emoji && emoji->Colored, "U+1F600 is flagged Colored (COLR layers loaded)");
    ok(emoji && emoji->Visible, "U+1F600 has pixels (LoadColor is on)");

    int nonGrey = 0;
    if (emoji && atlas->TexPixelsRGBA32) {
        const int w = atlas->TexWidth, h = atlas->TexHeight;
        const int x0 = int(emoji->U0 * w), x1 = int(emoji->U1 * w);
        const int y0 = int(emoji->V0 * h), y1 = int(emoji->V1 * h);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const unsigned p = atlas->TexPixelsRGBA32[y * w + x];
                const unsigned r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
                if (r != g || g != b) ++nonGrey;
            }
    }
    ok(nonGrey > 0, "U+1F600's atlas box holds non-greyscale texels (it is COLOURED)");

    // The cross-merge's own two claims, each a defect that shipped in b132.
    ok(f->FindGlyphNoFallback(0x0400) != nullptr,
       "U+0400 is present (JetBrains Mono lacks it; a backstop must supply it)");
    ok(f->FindGlyphNoFallback(0xFFFD) != nullptr,
       "U+FFFD is present (six of seven faces lack it; absent text fell to '?')");

    UE_LOGI("font selftest: %s (%d/%d) -- atlas %dx%d %s, %d colour texels in one emoji",
            pass == total ? "PASS" : "FAIL", pass, total, atlas->TexWidth, atlas->TexHeight,
            atlas->TexPixelsRGBA32 ? "RGBA32" : "Alpha8", nonGrey);
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
    // The REPERTOIRE, not GetGlyphRangesCyrillic(): Latin-1 + Cyrillic still,
    // plus every single-codepoint emoji, and minus the codepoints no embedded
    // face can draw. Passing a superset costs nothing -- the freetype builder
    // skips a codepoint whose face has no glyph for it.
    const ImWchar* ranges = Repertoire();

    // PRIMARY: the per-role families embedded in the DLL as RCDATA (RULE 3, no
    // loose files). Menu is baked first -> ImGui default.
    if (BakeEmbeddedRoles(s, cfg, ranges)) {
        // Any role whose resource somehow failed reuses the default (first baked).
        ImFont* def = g_roleFont[static_cast<int>(Role::Menu)];
        if (!def) for (int r = 0; r < kRoleCount; ++r) if (g_roleFont[r]) { def = g_roleFont[r]; break; }
        for (int r = 0; r < kRoleCount; ++r) if (!g_roleFont[r]) g_roleFont[r] = def;
        UE_LOGI("fonts: roles menu=%s chat=%s net=%s nameplate=%s toast=%s (embedded; ui %.0f px, "
                "chat %.0f px, scale %.2f, repertoire+emoji, cross-merged, freetype)",
                kFamilies[static_cast<int>(g_roleFamily[0])].label,
                kFamilies[static_cast<int>(g_roleFamily[1])].label,
                kFamilies[static_cast<int>(g_roleFamily[2])].label,
                kFamilies[static_cast<int>(g_roleFamily[3])].label,
                kFamilies[static_cast<int>(g_roleFamily[4])].label,
                g_rolePx[0], g_rolePx[1], s);
        // THE ACCEPTED COST, MEASURED WHERE IT IS PAID. Adding the donor took the
        // offline bake from ~5-16 ms to ~28-87 ms, and ui::scale::NoteViewport
        // re-bakes on every quantized-sixth crossing -- its own comment concedes
        // a windowed drag crosses several. An offline probe is not that path (it
        // never uploads a texture), so Build() is called HERE, timed, and logged:
        // the number in a real session's log is the only honest version of it.
        // The backend's own lazy Build() then becomes a no-op (IsBuilt latches).
        {
            LARGE_INTEGER f{}, a{}, b{};
            ::QueryPerformanceFrequency(&f);
            ::QueryPerformanceCounter(&a);
            const bool built = io.Fonts->Build();
            ::QueryPerformanceCounter(&b);
            const double ms = f.QuadPart ? (double(b.QuadPart - a.QuadPart) * 1000.0 /
                                            double(f.QuadPart)) : 0.0;
            UE_LOGI("fonts: atlas %s in %.1f ms (%dx%d %s)", built ? "baked" : "FAILED TO BAKE",
                    ms, io.Fonts->TexWidth, io.Fonts->TexHeight,
                    io.Fonts->TexPixelsRGBA32 ? "RGBA32" : "Alpha8");
        }
        RunFontRepertoireSelftestOnce();
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
        // The donor still merges here -- it is RCDATA in the same DLL, so the
        // only way this path runs with no emoji is a resource table that lost
        // one entry and not the others. What the fold does NOT do is follow:
        // FoldKey stays on the compile-time table whatever baked, so peers agree
        // about names even on a machine whose atlas came out short.
        MergeBackstops(-1, false, kUiPx * s, ranges);
        ImFont* chat = AddFromFile(c.reg, kChatPx * s, cfg, ranges);
        if (chat) MergeBackstops(-1, false, kChatPx * s, ranges);
        g_roleFont[static_cast<int>(Role::Menu)]      = menu; g_rolePx[0] = kUiPx * s;
        g_roleFont[static_cast<int>(Role::Chat)]      = chat ? chat : menu; g_rolePx[1] = (chat ? kChatPx : kUiPx) * s;
        g_roleFont[static_cast<int>(Role::Net)]       = menu; g_rolePx[2] = kUiPx * s;
        g_roleFont[static_cast<int>(Role::Nameplate)] = menu; g_rolePx[3] = kNameplatePx * s;
        g_roleFont[static_cast<int>(Role::Toast)]     = menu; g_rolePx[4] = kUiPx * s;
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
    coop::config::WriteIniValue(coop::config_registry::FontRoleRow(static_cast<size_t>(ri)),
                                FamilyToken(fi));
    ui::scale::RequestRebuild();  // atlas re-bakes before the next frame
}

void OnContextDestroyed() {
    for (int r = 0; r < kRoleCount; ++r) g_roleFont[r] = nullptr;
}

}  // namespace ui::fonts
