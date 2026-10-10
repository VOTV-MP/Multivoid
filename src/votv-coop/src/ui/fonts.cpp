// ui/fonts.cpp -- the overlay's fonts: one baked face per role (menu, chat, net stats,
// nameplates, the toast), each from an embedded family at the live pixel scale, with the other
// families and the colour emoji donor merged in as backstops, and the generated exclude list
// applied to every source. See ui/fonts.h.

#include "ui/fonts.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/text/repertoire.h"
#include "l10n/l10n.h"
#include "ui/atlas_watch.h"
#include "ui/scale.h"
#include "ue_wrap/core/log.h"

#include "imgui.h"
#include "misc/freetype/imgui_freetype.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <atomic>
#include <cstdint>
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
bool    g_rolesRead = false;     // the config is read once; ApplyRowsIfChanged updates after

// The two atomics are the exception to the render-thread-only data above. g_rowsChanged is handed
// from the game thread (the rows' subscriber stores true) to the render thread (the frame consumes
// it); g_rowsApplies is incremented by the render thread's apply and read on any thread by
// RowsApplies(). The render-thread data above is never written from the game thread.
std::atomic<bool>     g_rowsChanged{false};
std::atomic<uint32_t> g_rowsApplies{0};

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

// The ini key suffix per role lives in the config registry, in the same Role order; this table
// keeps the UI-only columns.
struct RoleDesc {
    const char* label;       // UI label
    float  basePx;           // 1080p base size (baked at basePx * ui::scale)
    bool   bold;             // use the family's Bold face
};
constexpr RoleDesc kRoles[kRoleCount] = {
    { L10N_MARK("Menu / panels"), kUiPx,        false },  // Role::Menu (== ImGui default)
    { L10N_MARK("Chat"),          kChatPx,      true  },  // Role::Chat
    { L10N_MARK("Net stats"),     kUiPx,        false },  // Role::Net
    { L10N_MARK("Nameplates"),    kNameplatePx, false },  // Role::Nameplate
    { L10N_MARK("Release toast"), kUiPx,        false },  // Role::Toast (our update/version toast)
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
        if (v == FamilyToken(static_cast<Family>(i))) return static_cast<Family>(i);
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

// Game thread, through the config notifier: only the flag is stored.
void OnFontRowChanged() { g_rowsChanged.store(true, std::memory_order_release); }

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
// so what is refused for baking and what folds to the sentinel cannot drift -- for every face but
// the system face merged for the active language's script (MergeSystemFace below), which bakes that
// script on purpose while the fold still reads it as the sentinel. Subtractive
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
    // Read once: this runs for every font source on each atlas rebuild, on the render thread.
    static const bool s_noExclude =
        coop::config::ResolveFlag(coop::config_registry::rows::atlas_no_exclude_drill);
    if (s_noExclude) return nullptr;
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

// ---- the script a pack needs and our faces do not carry ---------------------------------------
// The active language's script and the Windows face that draws it -- #41's merge, restricted. The
// face is not ours, so it is held to its script: its own exclude list is the generated table united
// with everything outside the script's ranges, and it is merged last, so it supplies only what every
// embedded face lacked inside its script and Latin, Cyrillic and every symbol still come from ours.
// The allowance leaves out what looks like a Latin character while folding apart from it (U+3007, the
// fullwidth digits and letters), and the generated table keeps U+3000 and U+FFA0 refused here too.
// Only Simplified Chinese has a row: the one pack that ships; another script is one row.
//
// Each row carries its own state, the face's view and the face's exclude list: the language is
// fixed for the process (l10n::Init runs once), so a row's state is built once and held while the
// process lives -- the atlas reads the view in place on every rebuild -- and never another row's.
struct SystemFace {
    bool tried = false;
    const char* data = nullptr;   // a read-only view of the file, never unmapped
    size_t size = 0;
    std::string name;
};
struct ScriptState {
    SystemFace face;
    std::vector<ImWchar> exclude;
};
struct ScriptDesc {
    const char* id;
    const coop::text::CodepointRange* ranges;   // sorted and disjoint (SortedDisjoint)
    size_t count;
    const char* const* faces;                   // first found wins; null-terminated
    ScriptState* state;
};
// Both exclude-list derivations (here and ui/atlas_watch.cpp) walk the ranges in order, and their
// equality check cannot see an input both read wrong, so the order is asserted where the row is.
template <size_t N>
constexpr bool SortedDisjoint(const coop::text::CodepointRange (&r)[N]) {
    for (size_t i = 0; i < N; ++i) {
        if (r[i].begin < 1 || r[i].begin > r[i].end || r[i].end > 0x10FFFF) return false;
        if (i > 0 && r[i].begin <= r[i - 1].end) return false;
    }
    return true;
}
constexpr coop::text::CodepointRange kHansRanges[] = {
    {0x3001, 0x3006}, {0x3008, 0x303F}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xF900, 0xFAFF},
    {0xFF01, 0xFF0F}, {0xFF1A, 0xFF20}, {0xFF3B, 0xFF40}, {0xFF5B, 0xFF65},
};
static_assert(SortedDisjoint(kHansRanges), "a script's ranges must be sorted, disjoint and inside U+0001..U+10FFFF");
constexpr const char* kHansFaces[] = {"msyh.ttc", "msyhl.ttc", "simhei.ttf", "simsun.ttc", nullptr};
ScriptState g_hansState;
constexpr ScriptDesc kHans = {"Hans", kHansRanges, sizeof(kHansRanges) / sizeof(kHansRanges[0]), kHansFaces,
                              &g_hansState};

// The atlas ceilings (Load). A script face's text is not bounded as the repertoire is: it bakes each
// ideograph the first time something draws it, and a baked drawn every frame (the chat) keeps every
// glyph it took, so peer text accumulates. Measured by the l10n drill's atlas arm: the whole CJK block,
// 20,992 ideographs, packs into 67% of 2048 squared at a 15 px chat, about 134 px squared each; by area,
// at full fill, about 3,500 at 4K's 45 px and 1,400 at the scale cap's 72, against the few thousand
// everyday Chinese uses. So under a script face the ceiling is 4096, reached only by a session that draws that many,
// which alone pays the larger upload and repack peak. The pack-failure detector says when it fills.
constexpr int kAtlasMax       = 2048;
constexpr int kAtlasMaxScript = 4096;

// The script of the language whose catalogue loaded.
const ScriptDesc* ActiveScript() {
    const std::string loc = l10n::ActiveLocale();
    if (loc == "zh" || loc == "zh_CN" || loc == "zh_SG") return &kHans;
    return nullptr;
}

// The face, mapped once per process and the view held while it lives: every atlas rebuild adds it from
// memory, not owned by the atlas, as the embedded faces are added (a file add would read it once per
// role on every rebuild), and FreeType reads only the pages it touches -- file-backed, so neither a
// 20 MB private copy nor a whole-file read on the render thread. The trade: a disk error under a held
// view faults inside FreeType where a read would have failed softly, on the local Windows directory.
// A candidate that exists and does not open or map is named with its reason; a missing face is said
// once and its script draws as boxes.
const SystemFace& LoadSystemFace(const ScriptDesc& script) {
    SystemFace& face = script.state->face;
    if (face.tried) return face;
    face.tried = true;
    char windir[MAX_PATH] = {};
    ::GetWindowsDirectoryA(windir, sizeof(windir));
    const std::string dir = windir[0] ? std::string(windir) + "\\Fonts\\" : std::string();
    for (const char* const* f = script.faces; *f && !dir.empty(); ++f) {
        HANDLE h = ::CreateFileA((dir + *f).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
                UE_LOGW("fonts: the %s script's face '%s' is skipped -- it does not open (error %lu)", script.id, *f,
                        static_cast<unsigned long>(err));
            continue;
        }
        LARGE_INTEGER size{};
        const void* view = nullptr;
        const char* why = nullptr;
        if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
            why = "its size does not read";
        } else if (size.QuadPart > INT_MAX) {
            why = "larger than the atlas takes (an int size)";
        } else if (HANDLE m = ::CreateFileMappingA(h, nullptr, PAGE_READONLY, 0, 0, nullptr)) {
            view = ::MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
            ::CloseHandle(m);   // the view keeps the mapping, and the file, open
            if (!view) why = "its view does not map";
        } else {
            why = "it does not map";
        }
        ::CloseHandle(h);
        if (why) {
            UE_LOGW("fonts: the %s script's face '%s' is skipped -- %s", script.id, *f, why);
            continue;
        }
        face.data = static_cast<const char*>(view);
        face.size = static_cast<size_t>(size.QuadPart);
        face.name = *f;
        break;
    }
    if (!face.data)
        UE_LOGW("fonts: the %s script needs a Windows face and none of its candidates mapped from '%s' -- "
                "its characters draw as boxes", script.id, dir.empty() ? "(no Windows directory)" : dir.c_str());
    else
        UE_LOGI("fonts: %s script -- the Windows face '%s' (%zu bytes, mapped) merged into every role, "
                "restricted to the script", script.id, face.name.c_str(), face.size);
    return face;
}

// The system face's exclude list: the generated table united with the complement of the script's
// ranges over U+0001..U+10FFFF (never beginning at U+0000: the list is zero-terminated), merged and
// sorted. Built once per row; under the no-exclude drill it is null, as every source's is.
const ImWchar* SystemExcludeList(const ScriptDesc& script) {
    if (!ExcludeList()) return nullptr;
    std::vector<ImWchar>& v = script.state->exclude;
    if (!v.empty()) return v.data();
    size_t n = 0;
    const coop::text::CodepointRange* gen = coop::text::ExcludeRanges(&n);
    std::vector<coop::text::CodepointRange> all(gen, gen + n);
    uint32_t next = 1;
    for (size_t i = 0; i < script.count; ++i) {
        if (script.ranges[i].begin > next) all.push_back({next, script.ranges[i].begin - 1});
        next = script.ranges[i].end + 1;
    }
    if (next <= 0x10FFFF) all.push_back({next, 0x10FFFF});
    std::sort(all.begin(), all.end(), [](const auto& x, const auto& y) { return x.begin < y.begin; });
    std::vector<coop::text::CodepointRange> merged;
    for (const auto& r : all) {
        if (!merged.empty() && r.begin <= merged.back().end + 1) merged.back().end = (std::max)(merged.back().end, r.end);
        else merged.push_back(r);
    }
    for (const auto& r : merged) {
        v.push_back(static_cast<ImWchar>(r.begin));
        v.push_back(static_cast<ImWchar>(r.end));
    }
    v.push_back(0);
    return v.data();
}

// Merged into the face just added, after the colour donor, so ImGui's walk of the sources reaches it
// only for what every embedded face lacked. Named, so the watcher's exclude check knows it.
void MergeSystemFace(float px) {
    const ScriptDesc* script = ActiveScript();
    if (!script) return;
    const SystemFace& face = LoadSystemFace(*script);
    if (!face.data) return;
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.FontDataOwnedByAtlas = false;
    cfg.GlyphExcludeRanges = SystemExcludeList(*script);
    std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", ui::atlas_watch::kSystemSourceName);
    // Read-only: with FontDataOwnedByAtlas off neither ImGui nor FreeType writes the data or frees it.
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<char*>(face.data), static_cast<int>(face.size), px,
                                               &cfg, nullptr);
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
    MergeSystemFace(px);
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

bool ScriptFaceActive() { return ActiveScript() != nullptr; }

void Load() {
    ImGuiIO& io = ImGui::GetIO();
    // Re-entrant: a scale or family change re-bakes the whole atlas. Clear drops the previous fonts
    // and pixels; our TTF pointers are not atlas-owned and survive.
    io.Fonts->Clear();
    for (int r = 0; r < kRoleCount; ++r) { g_roleFont[r] = nullptr; g_rolePx[r] = 0.f; }

    // The watcher is told the active language's script before any face is added, on every path below
    // and face or no face: its selftest arm then runs under every script a pack needs, and a face
    // that did not load (or a path that merges none) is a failure there, not a skipped arm.
    {
        const ScriptDesc* script = ActiveScript();
        ui::atlas_watch::AllowScript(script ? script->ranges : nullptr, script ? script->count : 0);
    }
    ReadRoleFamiliesOnce();

    // Baked at the real pixel size for the live resolution, never through the global font scale,
    // which stretches the bitmap and blurs.
    const float s = ui::scale::Ui();

    // The atlas ceiling, explicit. ImGui defaults to 8192 and the lazy atlas grows into whatever it
    // is allowed. Two reasons for 2048: the DX12 backend uploads the dirty bounding box through a
    // staging buffer that only grows, behind an untimed wait, and at 2048 the worst upload is 16.8
    // MB (67 MB at 4096); and the atlas keeps the old and new texture across a repack, so the peak
    // is two textures, 33.6 MB here against 134 MB. The repertoire's pathological demand, every
    // remote-text surface asking for all of it at its own size, measures to about 86% of 2048 squared.
    // Under a script face Load takes the dearer side of both reasons (kAtlasMaxScript, above).
    io.Fonts->TexMaxWidth = io.Fonts->TexMaxHeight = ActiveScript() ? kAtlasMaxScript : kAtlasMax;
    // The drill (dev.atlas_texmax_drill, 0 off): 256 starves the packer, which is how the
    // pack-failure detector is shown red.
    static const long s_texmaxDrill =
        coop::config::ResolveInt(coop::config_registry::rows::atlas_texmax_drill);
    if (const long drill = s_texmaxDrill) {
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
    return (i >= 0 && i < kRoleCount) ? l10n::T(kRoles[i].label) : "?";
}

Family RoleFamily(Role r) {
    ReadRoleFamiliesOnce();
    const int i = static_cast<int>(r);
    return (i >= 0 && i < kRoleCount) ? g_roleFamily[i] : Family::Fixedsys;
}

const char* FamilyToken(Family f) {
    return coop::config_registry::kFontFamilyTokens[static_cast<int>(f)];
}

void SubscribeRows() {
    for (size_t i = 0; i < coop::config_registry::kFontRoleCount; ++i)
        coop::config::Subscribe(coop::config_registry::FontRoleRow(i), &OnFontRowChanged);
}

void ApplyRowsIfChanged() {
    if (!g_rowsChanged.exchange(false, std::memory_order_acquire)) return;
    ReadRoleFamiliesOnce();
    int changed = 0;
    for (int r = 0; r < kRoleCount; ++r) {
        const std::string v =
            coop::config::ResolveEnum(coop::config_registry::FontRoleRow(static_cast<size_t>(r)));
        const Family f = FamilyFromToken(v, RoleDefaultFam(r));
        if (f == g_roleFamily[r]) continue;
        g_roleFamily[r] = f;
        ++changed;
    }
    if (changed == 0) return;
    g_rowsApplies.fetch_add(1, std::memory_order_relaxed);
    ui::scale::RequestRebuild();  // atlas re-bakes once, before this frame's read
    UE_LOGI("ui::fonts: rows applied (%d role(s) changed)", changed);
}

uint32_t RowsApplies() { return g_rowsApplies.load(std::memory_order_relaxed); }

void OnContextDestroyed() {
    for (int r = 0; r < kRoleCount; ++r) g_roleFont[r] = nullptr;
}

}  // namespace ui::fonts
