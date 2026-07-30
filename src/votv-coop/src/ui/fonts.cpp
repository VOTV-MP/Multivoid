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

// The EXCLUDE set (coop/text/repertoire.h) in ImGui's range form -- the exact
// complement, within what our faces carry, of the table the nickname arbiter
// folds against. One generator emits both, so what we refuse to BAKE and what we
// fold to the sentinel cannot drift.
//
// SUBTRACTIVE, BECAUSE 1.92 LEFT US NOTHING ELSE. The eager builder took an
// inclusion list and `fold == bake` held by construction. The lazy atlas ignores
// ImFontConfig::GlyphRanges entirely and bakes whatever gets drawn, so the only
// lever is ImFontConfig::GlyphExcludeRanges, consulted per source by
// ImFontAtlasBuildAcceptCodepointForSource on the on-demand path.
//
// THE LIST IS ZERO-TERMINATED, so it may not begin with U+0000 -- ImGui's walk
// would stop at index 0 and exclude nothing at all, with no symptom anywhere:
// its own size asserts would pass (0 is even and <= 64) and NDEBUG strips them.
// The generator hard-fails rather than emit one, coop/text/repertoire.cpp
// static_asserts it, and the loop below would still be correct if both failed.
//
// ImWchar must be 32-bit: the exclude set reaches U+10FFFD (private use) and the
// repertoire reaches U+1FBF9, so on a 16-bit build neither table could even be
// expressed and the emoji half of the repertoire would silently not exist.
static_assert(sizeof(ImWchar) == 4,
              "IMGUI_USE_WCHAR32 must be on: the exclude set is astral");
const ImWchar* ExcludeList() {
    // DRILL ONLY (dev.atlas_no_exclude_drill). Returning nullptr lets every source
    // bake its entire cmap, which is precisely the superset the invariant in
    // ui/atlas_watch.cpp exists to catch -- so this is how that detector is shown
    // RED without a source edit. It breaks name folding while set, which is why
    // it is a dev row with no UI and an explicit warning in its catalog text.
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

// EVERY add goes through this funnel or AddFromFile below, and the exclude list
// is applied HERE rather than at each ImFontConfig declaration. The field lives
// on the config, and we build four of them (cfg / merge / donor / the OS
// fallbacks), so setting it at the declarations is a site list -- a fifth config
// added later silently bakes the whole cmap and the superset invariant in
// ui/atlas_watch.cpp is what would notice, after the fact. Two funnels, no list.
ImFont* AddFromResource(int id, float px, const ImFontConfig& baseCfg) {
    int sz = 0;
    const void* data = ResourceTtf(id, &sz);
    if (!data) return nullptr;
    // The resource lives in the mapped DLL image for the process lifetime, so the
    // atlas must NOT take ownership (it would FREE a resource pointer on rebuild --
    // and Load() rebuilds on every scale/family change).
    ImFontConfig cfg = baseCfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.GlyphExcludeRanges = ExcludeList();
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        const_cast<void*>(data), sz, px, &cfg, nullptr);
}

// Merge the OTHER embedded families, then the colour donor, into the face just
// added. Two defects this closes, both live in b132:
//
//   - JetBrains Mono has no U+0400, U+040D, U+0450 or U+045D. Four Cyrillic
//     letters that the other three families all carry, invisible to every drill
//     because the drills ran on Fixedsys and Roboto. A backstop costs zero DLL
//     bytes and closes it for every family at once.
// (The fallback glyph is NOT one of them, and that is worth stating because the
// design claimed it was: all seven faces carry U+FFFD, but no glyph RANGE ever
// asked for it, so nothing baked it and the fallback fell through to '?'. That is
// fixed by the repertoire table above containing U+FFFD, not by this merge.)
//
// ORDER IS THE POLICY: the chosen family wins wherever it HAS the glyph and the
// backstops only fill holes. The donor goes last for the same reason -- a family
// that draws its own dingbat keeps drawing it.
//
// RE-POINTED 2026-07-30. This used to credit misc/freetype/imgui_freetype.cpp:515
// ("refuses to overwrite a glyph an earlier source already provided"). That was
// the 1.91.5 eager-builder mechanism and it is GONE: :515 in 1.92.9 is FreeType
// render-mode selection. (The bare filename never resolved either -- that file
// lives under misc/freetype/, not at the imgui root.) The policy still holds, by
// a different site --
// ImFontBaked_BuildLoadGlyph (imgui_draw.cpp:4590-4602) walks font->Sources in
// order and RETURNS on the first source that produces the glyph, and the legacy
// eager preload routes through the same function. Same outcome, and the 88
// codepoints both the faces and the donor carry (digits, '#', '*', ZWJ, TM, the
// arrows) are decided there in both regimes -- measured, so a uniform
// GlyphExcludeRanges cannot change a winner.
//
// The general trap, worth stating because this file has now been caught twice
// (see also the retired GetTexDataAsRGBA32 note below): a comment citing a LINE
// NUMBER in a vendored dependency is silently invalidated by a submodule bump.
// Nothing in the build complains, and the stale citation reads exactly as
// authoritative as a live one.
void MergeBackstops(int chosenFamily, bool bold, float px) {
    ImFontConfig merge;
    merge.MergeMode = true;
    for (int o = 0; o < kFamilyCount; ++o) {
        if (o == chosenFamily) continue;
        AddFromResource(bold ? kFamilies[o].boldId : kFamilies[o].regularId, px, merge);
    }
    // ImGuiFreeTypeBuilderFlags_LoadColor is not optional: without it the COLR
    // layers are skipped and every emoji bakes with visible=0 -- INVISIBLE, not
    // missing, which is exactly the state a "did the donor load?" check passes.
    ImFontConfig donor = merge;
    // 1.92 renamed FontBuilderFlags -> FontLoaderFlags (imgui.h:3633). MEASURED
    // 2026-07-30 that the flag still does the job under per-size baking:
    // UseColors=1, Colored=3, 2600 non-greyscale texels; without it
    // emoji-visible=0, exactly as this comment claimed on 1.91.5.
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

// Bake all roles from the embedded families, deduping identical (family, px, weight)
// so the common single-family config adds only ONE regular + ONE bold atlas entry.
// Returns true if at least one role's resource baked. Menu (role 0) is baked first
// -> its font is ImGui's default.
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
    // Re-entrant: a scale/family change re-bakes the whole atlas. Clear drops the
    // previous fonts + pixel data (our TTF pointers survive -- not atlas-owned).
    io.Fonts->Clear();
    for (int r = 0; r < kRoleCount; ++r) { g_roleFont[r] = nullptr; g_rolePx[r] = 0.f; }

    ReadRoleFamiliesOnce();

    // Bake at the REAL pixel size for the live resolution (ui::scale) -- never
    // io.FontGlobalScale (that stretches the 1x bitmap and blurs).
    const float s = ui::scale::Ui();

    // THE ATLAS CEILING, SET EXPLICITLY. ImGui defaults TexMaxWidth/Height to
    // 8192 and the lazy atlas grows into whatever it is allowed (doubling height
    // then width, ImFontAtlasTextureGrow). Two reasons this is 2048 and not the
    // default, both measured:
    //
    //   UPLOAD -- the DX12 backend uploads the dirty UpdateRect BOUNDING BOX and
    //   its staging buffer only ever GROWS (ImGui_ImplDX12_UpdateTexture), behind
    //   a WaitForSingleObject(.., INFINITE) that nothing has yet timed. At 4096
    //   the worst single upload is 67 MB; at 2048 it is 16.8 MB, which is exactly
    //   today's geometry, so this commit cannot regress the per-upload ceiling.
    //   (It does raise upload FREQUENCY -- the atlas now grows during play rather
    //   than once at boot -- and that is what the probe in the DX12 backend and
    //   the geometry line in ui/atlas_watch.cpp exist to measure.)
    //
    //   PEAK -- ImFontAtlas keeps the old AND new texture across a repack, so
    //   peak is two: 2048^2 RGBA32 = 16.8 MB each, 33.6 MB. At 4096 it is 134 MB.
    //
    // CAPACITY IS NOT THE BINDING CONSTRAINT, and the arithmetic is worth keeping
    // because it was wrong twice. The pathological demand is every surface that
    // draws remote-authored text asking for the WHOLE repertoire at once: chat
    // feed (18 px) + overhead bubble (14.08 px) + scoreboard (16 px), summed
    // because ONE atlas is shared across sizes and its GC is pressure-triggered
    // rather than periodic. Measured from the font binaries' own bounding boxes:
    // 3,589,892 px2 against 2048^2's 4,194,304 -- 0.856x. It fits, with 17 %
    // headroom rather than the "3x margin" an earlier revision of this reasoning
    // claimed by pricing a single size. That is why the pack-failure detector in
    // ui/atlas_watch.cpp is load-bearing and not diagnostic.
    io.Fonts->TexMaxWidth  = 2048;
    io.Fonts->TexMaxHeight = 2048;
    // DRILL ONLY (dev.atlas_texmax_drill, 0 = off). 256 starves the packer, which
    // is how ui/atlas_watch.cpp's pack-failure detector is shown RED -- the one
    // instrument standing between a remote peer and a permanently-boxed glyph.
    if (const long drill =
            coop::config::ResolveInt(coop::config_registry::rows::atlas_texmax_drill)) {
        io.Fonts->TexMaxWidth = io.Fonts->TexMaxHeight = static_cast<int>(drill);
        // TexMin defaults to 512, so a ceiling below it would be incoherent and the
        // packer would never reach the starved state the drill is for.
        if (io.Fonts->TexMinWidth > io.Fonts->TexMaxWidth)
            io.Fonts->TexMinWidth = io.Fonts->TexMinHeight = io.Fonts->TexMaxWidth;
        UE_LOGW("fonts: ATLAS DRILL -- TexMax forced to %ld (dev.atlas_texmax_drill). Glyphs "
                "WILL fail to pack; that is the point.", drill);
    }

    ImFontConfig cfg;
    // No OversampleH/V: the freetype builder ignores it and hints properly.
    // What each source may bake is decided SUBTRACTIVELY now -- AddFromResource
    // and AddFromFile put the generated exclude list on every config. The old
    // `ranges` inclusion parameter is gone: ImGui 1.92 ignores
    // ImFontConfig::GlyphRanges on the on-demand path, so it was a live-looking
    // knob that no longer decided anything (RULE 2).

    // PRIMARY: the per-role families embedded in the DLL as RCDATA (RULE 3, no
    // loose files). Menu is baked first -> ImGui default.
    if (BakeEmbeddedRoles(s, cfg)) {
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
        // NOTHING IS BAKED HERE ANY MORE, AND THERE IS NO NUMBER TO PRINT. This
        // used to call ImFontAtlas::Build() inside a QueryPerformanceCounter pair
        // and log "atlas baked in %.1f ms (%dx%d)". Both halves retired with the
        // flip (RULE 2 -- retired, not kept behind a condition):
        //
        //   Build() is an OBSOLETE shim (imgui_draw.cpp, inside `#ifndef
        //   IMGUI_DISABLE_OBSOLETE_FUNCTIONS`) that just calls
        //   ImFontAtlasBuildMain and returns true, so its bool was already
        //   unreachable. Worse, calling it HERE samples the capability flag from
        //   the context at that instant -- and Load() runs before the renderer
        //   backend sets the flag, which is precisely how an eager build gets
        //   locked in under a dynamic regime. The first build is now
        //   ImFontAtlasUpdateNewFrame inside the first NewFrame(), which is
        //   unconditionally after InitRenderer.
        //
        //   The ms figure has no meaning under a lazy atlas: rasterisation is
        //   spread across the frames that draw new text, so a single boot number
        //   would be a fiction. The honest replacements ship in the same commit,
        //   in ui/atlas_watch.cpp -- geometry logged when it CHANGES, plus the
        //   per-frame glyph delta when a frame rasterises a lot at once.
        //
        // The selftest moved there too, for the same reason: it now fires on a
        // texture-id edge, so it sees boot, rescale, the F1 family switch AND
        // every grow -- where a call from here would only ever see Load().
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
        ImFont* menu = AddFromFile(c.reg, kUiPx * s, cfg);
        if (!menu) continue;
        // The donor still merges here -- it is RCDATA in the same DLL, so the
        // only way this path runs with no emoji is a resource table that lost
        // one entry and not the others. What the fold does NOT do is follow:
        // FoldKey stays on the compile-time table whatever baked, so peers agree
        // about names even on a machine whose atlas came out short.
        MergeBackstops(-1, false, kUiPx * s);
        ImFont* chat = AddFromFile(c.reg, kChatPx * s, cfg);
        if (chat) MergeBackstops(-1, false, kChatPx * s);
        g_roleFont[static_cast<int>(Role::Menu)]      = menu; g_rolePx[0] = kUiPx * s;
        g_roleFont[static_cast<int>(Role::Chat)]      = chat ? chat : menu; g_rolePx[1] = (chat ? kChatPx : kUiPx) * s;
        g_roleFont[static_cast<int>(Role::Net)]       = menu; g_rolePx[2] = kUiPx * s;
        g_roleFont[static_cast<int>(Role::Nameplate)] = menu; g_rolePx[3] = kNameplatePx * s;
        g_roleFont[static_cast<int>(Role::Toast)]     = menu; g_rolePx[4] = kUiPx * s;
        // THE FAILURE MODE HERE INVERTED WITH THE FLIP, so the warning says both
        // halves now. It used to be that a system face came out SHORT of the
        // repertoire and some names drew as boxes. Under the lazy atlas the
        // exclude list is subtractive, so a system face is asked for whatever it
        // HAS -- and Segoe UI carries Hebrew, Thai and Arabic that our embedded
        // families do not. The atlas therefore comes out a SUPERSET, while
        // FoldKey still maps those codepoints to the sentinel: two legible
        // non-Latin names can both fold to U+FFFD and the arbiter will suffix one
        // of them for a collision the player cannot see. Uniqueness is not
        // guaranteed on this path. It is detected, never prevented -- the
        // alternative to a superset font is no font at all -- so
        // ui/atlas_watch.cpp's superset invariant logs each offender.
        UE_LOGW("fonts: embedded families unavailable -- overlay font = %s (all roles; scale "
                "%.2f). Name uniqueness is NOT guaranteed on this path: a system face may "
                "draw scripts the fold table sentinels, so two legible names can collide "
                "invisibly.", c.tag, s);
        return;
    }

    // Last resort: the builtin ProggyClean (ASCII-only) so the overlay still renders.
    // AddFontDefaultBitmap, not AddFontDefault: 1.92 embeds a SECOND, vector
    // default font and AddFontDefault picks between them by expected size
    // (imgui_draw.cpp:3180-3186, vector at >= 15 px). Naming the one we want is
    // upstream's own advice (:3178) and it is what makes
    // IMGUI_DISABLE_DEFAULT_FONT_VECTOR safe to define -- 14,562 bytes of
    // compressed font data (imgui_draw.cpp:6560) that no path of ours can reach.
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
    g_rolesRead = true;  // the user's live choice wins over the ini read
    coop::config::WriteIniValue(coop::config_registry::FontRoleRow(static_cast<size_t>(ri)),
                                FamilyToken(fi));
    ui::scale::RequestRebuild();  // atlas re-bakes before the next frame
}

void OnContextDestroyed() {
    for (int r = 0; r < kRoleCount; ++r) g_roleFont[r] = nullptr;
}

}  // namespace ui::fonts
