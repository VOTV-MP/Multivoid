// ui/fonts.cpp -- the overlay's fonts: one baked face per role (menu, chat, net stats,
// nameplates, the toast), each from an embedded family at the live pixel scale, with the other
// families and the colour emoji donor merged in as backstops, and the generated exclude list
// applied to every source. See ui/fonts.h.

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

// The per-role baked font, its pixel size and its family. Menu is baked first, so its font is
// ImGui's default.
ImFont* g_roleFont[kRoleCount]   = {};
float   g_rolePx[kRoleCount]     = {};
// Filled by ReadRoleFamiliesOnce, which every consumer path calls first; the per-role default
// lives in the config registry's row list.
Family  g_roleFamily[kRoleCount] = {};
bool    g_rolesRead = false;     // the ini is read once; SetRoleFamily overrides after

// The ini token per family lives in the config registry, in the same Family order; this table
// keeps the UI-only columns.
struct FamilyDesc {
    const char* label;     // UI label
    int regularId;         // RCDATA ids
    int boldId;
};
constexpr FamilyDesc kFamilies[kFamilyCount] = {
    { "JetBrains Mono", IDR_FONT_JBMONO_REGULAR,   IDR_FONT_JBMONO_BOLD },
    { "Roboto",         IDR_FONT_ROBOTO_REGULAR,   IDR_FONT_ROBOTO_BOLD },
    { "Cascadia Code",  IDR_FONT_CASCADIA_REGULAR, IDR_FONT_CASCADIA_BOLD },
    // The game's own terminal pixel font, single weight, so the chat's bold face reuses Regular.
    // Covers Cyrillic.
    { "Fixedsys (VOTV)", IDR_FONT_FIXEDSYS_REGULAR, IDR_FONT_FIXEDSYS_REGULAR },
};
static_assert(coop::config_registry::kFontFamilyCount ==
                  static_cast<size_t>(kFamilyCount),
              "Family enum and config_registry::kFontFamilyTokens must stay in lockstep");
inline const char* FamilyToken(int fi) {
    return coop::config_registry::kFontFamilyTokens[fi];
}

// The ini key suffix per role lives in the config registry, in the same Role order; this table
// keeps the UI-only columns.
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
// The per-role default family, owned by the registry's row list: the menu, chat and toast
// default to Fixedsys, the net stats and nameplates to Roboto.
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

// The per-role families, read once. Each ui.font.<role> defaults to that role's own family, not
// one global, so the surfaces differ out of the box.
void ReadRoleFamiliesOnce() {
    if (g_rolesRead) return;
    for (int r = 0; r < kRoleCount; ++r) {
        // ResolveEnum returns the canonical family token, or the role's own default on an absent or
        // garbage value.
        const std::string v =
            coop::config::ResolveEnum(coop::config_registry::FontRoleRow(static_cast<size_t>(r)));
        g_roleFamily[r] = FamilyFromToken(v, RoleDefaultFam(r));
    }
    g_rolesRead = true;
}

// An RCDATA TTF embedded in our own module, not the game's.
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

// The exclude set (coop/text/repertoire.h) in ImGui's range form: a coarse cover of what
// our faces carry, of the table the nickname arbiter folds against. One generator emits both,
// so what is refused for baking and what folds to the sentinel cannot drift. Subtractive
// because the lazy atlas ignores an inclusion list and bakes whatever is drawn; the only lever
// is the per-source exclude list. The list is zero-terminated, so it may not begin with U+0000
// (ImGui's walk would stop at index 0 and exclude nothing, with no symptom); the generator
// refuses to emit one and repertoire.cpp asserts it. ImWchar must be 32-bit: the set reaches
// the astral planes.
static_assert(sizeof(ImWchar) == 4,
              "IMGUI_USE_WCHAR32 must be on: the exclude set is astral");
const ImWchar* ExcludeList() {
    // The drill (dev.atlas_no_exclude_drill): null lets every source bake its entire cmap, the
    // superset the invariant in ui/atlas_watch.cpp exists to catch, so that detector can be shown
    // red without a source edit. It breaks name folding while set.
    if (coop::config::ResolveFlag(coop::config_registry::rows::atlas_no_exclude_drill))
        return nullptr;
    static std::vector<ImWchar> v;
    if (v.empty()) {
        size_t n = 0;
        const coop::text::CodepointRange* r = coop::text::ExcludeRanges(&n);
        v.reserve(n * 2 + 1);
        for (size_t i = 0; i < n; ++i) {
            v.push_back(static_cast<ImWchar>(r[i].begin));
            v.push_back(static_cast<ImWchar>(r[i].end));
        }
        v.push_back(0);
    }
    return v.data();
}

// Every add goes through this funnel or AddFromFile, and the exclude list is applied here
// rather than at each config declaration: four configs are built, and a fifth added later
// would otherwise silently bake the whole cmap.
ImFont* AddFromResource(int id, float px, const ImFontConfig& baseCfg) {
    int sz = 0;
    const void* data = ResourceTtf(id, &sz);
    if (!data) return nullptr;
    // The resource lives in the mapped DLL image for the process lifetime, so the atlas must not
    // take ownership; it would free the pointer on a rebuild, and Load rebuilds on every scale or
    // family change.
    ImFontConfig cfg = baseCfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.GlyphExcludeRanges = ExcludeList();
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        const_cast<void*>(data), sz, px, &cfg, nullptr);
}

// Merge the other embedded families, then the colour donor, into the face just added. A
// backstop costs no DLL bytes and closes every hole at once (one family lacks four Cyrillic
// letters the others carry). Order is the policy: the chosen family wins wherever it has the
// glyph, the backstops fill holes, and the donor goes last so a family that draws its own
// dingbat keeps drawing it; ImGui's glyph load walks a font's sources in order and returns on
// the first that produces the glyph. The fallback glyph U+FFFD is baked because the repertoire
// table contains it, not because of this merge.
void MergeBackstops(int chosenFamily, bool bold, float px) {
    ImFontConfig merge;
    merge.MergeMode = true;
    for (int o = 0; o < kFamilyCount; ++o) {
        if (o == chosenFamily) continue;
        AddFromResource(bold ? kFamilies[o].boldId : kFamilies[o].regularId, px, merge);
    }
    // The colour flag is not optional: without it the COLR layers are skipped and every emoji bakes
    // invisible rather than missing, a state a "did the donor load?" check passes.
    ImFontConfig donor = merge;
    // The flag still does the job under per-size baking.
    donor.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LoadColor;
    AddFromResource(IDR_FONT_EMOJI_DONOR, px, donor);
}

ImFont* AddFromFile(const std::string& path, float px, const ImFontConfig& baseCfg) {
    const DWORD attrs = ::GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return nullptr;
    ImFontConfig cfg = baseCfg;
    cfg.GlyphExcludeRanges = ExcludeList();
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg, nullptr);
}

// Bake every role from the embedded families, deduping an identical (family, size, weight), so
// a single-family configuration adds one regular and one bold entry. True if at least one role
// baked. Menu is baked first.
bool BakeEmbeddedRoles(float s, const ImFontConfig& cfg) {
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
            font = AddFromResource(rd.bold ? fam.boldId : fam.regularId, px, cfg);
            if (font) {
                MergeBackstops(fi, rd.bold, px);
                any = true;
                cache[nCache++] = { fi, pxi, rd.bold, font };
            }
        }
        g_roleFont[r] = font;
        g_rolePx[r]   = px;
    }
    return any;
}

}  // namespace

void Load() {
    ImGuiIO& io = ImGui::GetIO();
    // Re-entrant: a scale or family change re-bakes the whole atlas. Clear drops the previous fonts
    // and pixels; our TTF pointers are not atlas-owned and survive.
    io.Fonts->Clear();
    for (int r = 0; r < kRoleCount; ++r) { g_roleFont[r] = nullptr; g_rolePx[r] = 0.f; }

    ReadRoleFamiliesOnce();

    // Baked at the real pixel size for the live resolution, never through the global font scale,
    // which stretches the bitmap and blurs.
    const float s = ui::scale::Ui();

    // The atlas ceiling, explicit. ImGui defaults to 8192 and the lazy atlas grows into whatever it
    // is allowed. Two reasons for 2048: the DX12 backend uploads the dirty bounding box through a
    // staging buffer that only grows, behind an untimed wait, and at 2048 the worst upload is 16.8
    // MB (67 MB at 4096); and the atlas keeps the old and new texture across a repack, so the peak
    // is two textures, 33.6 MB here against 134 MB. Capacity is not the binding constraint: the
    // pathological demand, every remote-text surface asking for the whole repertoire at its own
    // size in one shared atlas, measures to about 86% of 2048 squared, which fits with 17%
    // headroom. That is why the pack-failure detector in ui/atlas_watch.cpp is load-bearing.
    io.Fonts->TexMaxWidth  = 2048;
    io.Fonts->TexMaxHeight = 2048;
    // The drill (dev.atlas_texmax_drill, 0 off): 256 starves the packer, which is how the
    // pack-failure detector is shown red.
    if (const long drill =
            coop::config::ResolveInt(coop::config_registry::rows::atlas_texmax_drill)) {
        io.Fonts->TexMaxWidth = io.Fonts->TexMaxHeight = static_cast<int>(drill);
        // The minimum defaults to 512, so a ceiling below it would never reach the starved state.
        if (io.Fonts->TexMinWidth > io.Fonts->TexMaxWidth)
            io.Fonts->TexMinWidth = io.Fonts->TexMinHeight = io.Fonts->TexMaxWidth;
        UE_LOGW("fonts: ATLAS DRILL -- TexMax forced to %ld (dev.atlas_texmax_drill). Glyphs "
                "WILL fail to pack; that is the point.", drill);
    }

    ImFontConfig cfg;
    // No oversampling: the FreeType builder ignores it and hints properly. What each source may
    // bake is decided subtractively by the exclude list on every config; the inclusion ranges are
    // ignored on the on-demand path.

    // Primary: the per-role families embedded in the DLL as RCDATA, no loose files. Menu first.
    if (BakeEmbeddedRoles(s, cfg)) {
        // A role whose resource failed reuses the default.
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
        // Nothing is baked here: the lazy atlas rasterises across the frames that draw new text,
        // and the first build happens inside the first NewFrame, after the renderer backend has set
        // its capability flag (an eager build here would sample the flag too early). The geometry
        // and the per-frame glyph delta are logged in ui/atlas_watch.cpp, which also runs the
        // self-test on every texture-id edge: boot, a rescale, a family switch and every grow.
        return;
    }

    // The fallback: one Windows system font for every role, shared across the roles and the chat
    // size.
    char windir[MAX_PATH] = {};
    ::GetWindowsDirectoryA(windir, sizeof(windir));
    const std::string win = windir[0] ? std::string(windir) + "\\Fonts\\" : std::string();
    struct Cand { std::string reg; const char* tag; };
    const Cand cands[] = {
        { win + "tahoma.ttf",  "Tahoma (system)" },
        { win + "segoeui.ttf", "Segoe UI (system)" },
    };
    for (const Cand& c : cands) {
        ImFont* menu = AddFromFile(c.reg, kUiPx * s, cfg);
        if (!menu) continue;
        // The donor still merges here (RCDATA in the same DLL). The fold does not follow: the fold
        // key stays on the compile-time table whatever baked, so peers agree about names on a
        // machine whose atlas came out short.
        MergeBackstops(-1, false, kUiPx * s);
        ImFont* chat = AddFromFile(c.reg, kChatPx * s, cfg);
        if (chat) MergeBackstops(-1, false, kChatPx * s);
        g_roleFont[static_cast<int>(Role::Menu)]      = menu; g_rolePx[0] = kUiPx * s;
        g_roleFont[static_cast<int>(Role::Chat)]      = chat ? chat : menu; g_rolePx[1] = (chat ? kChatPx : kUiPx) * s;
        g_roleFont[static_cast<int>(Role::Net)]       = menu; g_rolePx[2] = kUiPx * s;
        g_roleFont[static_cast<int>(Role::Nameplate)] = menu; g_rolePx[3] = kNameplatePx * s;
        g_roleFont[static_cast<int>(Role::Toast)]     = menu; g_rolePx[4] = kUiPx * s;
        // The warning says both halves. Under the lazy atlas the exclude list is subtractive, so a
        // system face is asked for whatever it has, and one carrying scripts our families do not
        // makes the atlas a superset while the fold key still maps those codepoints to the
        // sentinel: two legible non-Latin names can fold to the same key, and the arbiter suffixes
        // one for a collision the player cannot see. Detected by ui/atlas_watch.cpp's superset
        // invariant, never prevented, since the alternative to a superset font is no font.
        UE_LOGW("fonts: embedded families unavailable -- overlay font = %s (all roles; scale "
                "%.2f). Name uniqueness is NOT guaranteed on this path: a system face may "
                "draw scripts the fold table sentinels, so two legible names can collide "
                "invisibly.", c.tag, s);
        return;
    }

    // The last resort: ImGui's built-in bitmap font, so the overlay still renders. The bitmap one
    // by name: ImGui also embeds a vector default and would pick between them by size, and naming
    // the one wanted is what lets the vector font be compiled out.
    ImFont* def = io.Fonts->AddFontDefaultBitmap();
    for (int r = 0; r < kRoleCount; ++r) { g_roleFont[r] = def; g_rolePx[r] = def ? def->LegacySize : kUiPx; }
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
    g_rolesRead = true;  // the live choice wins over the ini read
    coop::config::WriteIniValue(coop::config_registry::FontRoleRow(static_cast<size_t>(ri)),
                                FamilyToken(fi));
    ui::scale::RequestRebuild();  // atlas re-bakes before the next frame
}

void OnContextDestroyed() {
    for (int r = 0; r < kRoleCount; ++r) g_roleFont[r] = nullptr;
}

}  // namespace ui::fonts
