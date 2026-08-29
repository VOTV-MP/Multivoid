// coop/player/skin_registry.cpp -- see coop/player/skin_registry.h.

#include "coop/player/skin_registry.h"

#include "ue_wrap/core/paths.h"  // ExeDir -- the pak folder is derived from the install dir
#include "ue_wrap/core/log.h"

#include <algorithm>  // std::find -- the cross-subdir stem dedupe
#include <cwctype>
#include <filesystem>
#include <random>   // PickRandomStarterSkin -- one roll per NEW identity (boot path)

namespace coop::skins {

namespace {
namespace fs = std::filesystem;

std::vector<SkinEntry> g_entries;
bool g_scanned = false;

// Builtin skins (v94, census-verified 2026-07-02): the game's own anthro-kerfur
// bodies, loaded by asset path (no pak; materials ship WITH the mesh --
// client_model applies no 'tex' MID override for builtins).
//
// Every entry passed the 4-check census (tools/client_model/builtin_skin_census.py):
// (1) Skeleton import == kerfurOmegaV1_Skeleton (the player-body rig), (2) refskel
// bone 0 == rootKerfur, (3) EVERY refskel bone exists in the skeleton asset,
// (4) exactly one SkeletalMesh export named == stem (the LoadObject path).
// Sharing the Skeleton asset is NOT enough: sk/assbreather points at the same
// skeleton but its root bone is 'rootKerfur_010' (author-side Blender duplicate,
// absent from the skeleton) -- applying it poisons the player AnimInstance and
// locomotion never recovers across later mesh swaps (user-hit 2026-07-02). The
// game itself never loads that asset; its antibreather kerfur wears
// kerfurOmega_antibreatherSkin, which IS in this list. Re-run the census at every
// game-version re-target before trusting this table.
// kerfurOmega_KelSkin itself is intentionally absent: that asset is the kel look
// dr_kel already provides via the pristine native mesh.
struct BuiltinSkin { const char* name; const wchar_t* path; };
constexpr BuiltinSkin kBuiltinSkins[] = {
    { "kerfur_omega",          L"/Game/meshes/kerfurAnthro/sk/kerfurOmegaV1.kerfurOmegaV1" },
    { "kerfur_omega_h",        L"/Game/meshes/kerfurAnthro/sk/kerfurOmegaV1_h.kerfurOmegaV1_h" },
    { "kerfur_omega_m",        L"/Game/meshes/kerfurAnthro/sk/kerfurOmegaV1_m.kerfurOmegaV1_m" },
    { "kerfur_omega_nc",       L"/Game/meshes/kerfurAnthro/sk/kerfurOmegaV1_nc.kerfurOmegaV1_nc" },
    { "kerfur_maid",           L"/Game/meshes/kerfurAnthro/sk/KerfurO_maid.KerfurO_maid" },
    { "kerfur_ariral",         L"/Game/meshes/kerfurAnthro/sk/kerfurOmega_ariralSkin.kerfurOmega_ariralSkin" },
    { "kerfur_ariral_suit",    L"/Game/meshes/kerfurAnthro/sk/kerfurOmega_ariralSuitSkin.kerfurOmega_ariralSuitSkin" },
    { "kerfur_keljoy",         L"/Game/meshes/kerfurAnthro/sk/kerfurOmega_keljoySkin.kerfurOmega_keljoySkin" },
    { "kerfur_mannequin",      L"/Game/meshes/kerfurAnthro/sk/kerfurOmega_mannequinSkin.kerfurOmega_mannequinSkin" },
    { "skerfuro",              L"/Game/meshes/kerfurAnthro/sk/skerfuro.skerfuro" },
    { "scrappy_keith",         L"/Game/meshes/kerfurAnthro/sk/ScrappyKeith.ScrappyKeith" },
    { "kerfur_antibreather",   L"/Game/meshes/antibreather/kerfurOmega_antibreatherSkin.kerfurOmega_antibreatherSkin" },
    { "kerfur_argplush",       L"/Game/meshes/arg/oskin/kerfurOmega_argplushSkin.kerfurOmega_argplushSkin" },
    { "kerfur_alien",          L"/Game/meshes/ayyLmaoFigure/kerfurOmega_alienSkin.kerfurOmega_alienSkin" },
    { "kerfur_fleshly",        L"/Game/meshes/bonerman/kerfurOmega_fleshlySkin.kerfurOmega_fleshlySkin" },
    { "kerfur_skeleton",       L"/Game/meshes/bonerman/kerfurOmega_skeletonSkin.kerfurOmega_skeletonSkin" },
    { "kerfur_vargskeleton",   L"/Game/meshes/bonerman/kerfurOmega_vargskeletonSkin.kerfurOmega_vargskeletonSkin" },
    { "kerfur_maxwell",        L"/Game/meshes/dingus/kerfurOmega_maxwellskin.kerfurOmega_maxwellskin" },
    { "kerfur_erie",           L"/Game/meshes/eriePlush/kerfurOmega_erieSkin.kerfurOmega_erieSkin" },
    { "kerfur_erie_v4",        L"/Game/meshes/eriev4/kerfurOmega_erieV4Skin.kerfurOmega_erieV4Skin" },
    { "kerfur_igetis",         L"/Game/meshes/igetis/kerfurOmega_igetisSkin.kerfurOmega_igetisSkin" },
    { "kerfur_monique",        L"/Game/meshes/kerfurAnthro/kerfurOmega_moniqueSkin.kerfurOmega_moniqueSkin" },
    { "kerfur_krampus",        L"/Game/meshes/krampus/kerfurOmega_krampusSkin.kerfurOmega_krampusSkin" },
    { "kerfur_mynet",          L"/Game/meshes/mynetSkin/kerfurOmega_mynetSkin.kerfurOmega_mynetSkin" },
    { "kerfur_furfur",         L"/Game/meshes/wendussy/kerfurOmega_furfurSkin.kerfurOmega_furfurSkin" },
};

// Bundle paks: one .pak carrying several skins (2026-08-29, user decision --
// the four starter scientists ship as ONE scientists.pak). UE mounts every pak
// under LogicMods and assets resolve by their internal package paths, so the
// only thing the registry needs from a bundle is WHICH skin names its presence
// vouches for. The bundle stem itself is never offered as a skin.
struct SkinBundle { const char* pakStem; const char* members[8]; };
constexpr SkinBundle kSkinBundles[] = {
    { "scientists", { "walter_v1sc", "sci_v1sc", "rvi_scientist_v1sc",
                      "luther_v1sc", nullptr } },
};

const SkinBundle* BundleForStem(const std::string& stem) {
    for (const auto& b : kSkinBundles)
        if (stem == b.pakStem) return &b;
    return nullptr;
}

// Is `skin` available via a pak in `dirW` -- either its own <skin>.pak or a
// bundle pak that lists it as a member?
bool DirProvidesSkin(const std::wstring& dirW, const char* skin) {
    std::error_code ec;
    fs::path own = fs::path(dirW) / skin;
    own += L".pak";
    if (fs::is_regular_file(own, ec)) return true;
    for (const auto& b : kSkinBundles) {
        for (const char* const* m = b.members; *m; ++m) {
            if (std::string(*m) != skin) continue;
            fs::path bp = fs::path(dirW) / b.pakStem;
            bp += L".pak";
            if (fs::is_regular_file(bp, ec)) return true;
        }
    }
    return false;
}

}  // namespace

bool IsValidSkinName(const std::string& name) {
    if (name.empty() || name.size() > 48) return false;
    for (char c : name) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

const wchar_t* BuiltinSkinPath(const std::string& name) {
    for (const auto& b : kBuiltinSkins)
        if (name == b.name) return b.path;
    return nullptr;
}

std::vector<std::wstring> PakDirs();  // defined below (the LogicMods subdir scan)

std::string PickRandomStarterSkin() {
    // v95 (user 2026-07-02): the curated "coolest" converter skins a NEW peer starts
    // with. Names are pak stems; a name only qualifies when its pak is present here,
    // so a fresh install without the bundle still boots with a loadable body.
    // 2026-08-29 (USER): the starter set is EXACTLY these four scientists --
    // "Эти 4 модели становятся дефолтом нашего мода и назначаются случайно
    // новому юзеру именно что-то из них четырех." (twhl_scientist2/3 dropped
    // from the roll; they remain installable skins like any other pak.)
    static constexpr const char* kStarterSkins[] = {
        "walter_v1sc", "sci_v1sc", "rvi_scientist_v1sc", "luther_v1sc",
    };
    std::vector<const char*> present;
    const std::vector<std::wstring> dirs = PakDirs();
    for (const char* s : kStarterSkins) {
        for (const std::wstring& dirW : dirs) {
            if (DirProvidesSkin(dirW, s)) { present.push_back(s); break; }
        }
    }
    if (present.empty()) {
        UE_LOGI("skin_registry: no starter-list pak present -- new identity falls back to '%s'",
                kDefaultSkinName);
        return kDefaultSkinName;
    }
    std::mt19937 rng{std::random_device{}()};
    const char* pick = present[rng() % present.size()];
    UE_LOGI("skin_registry: new identity rolled starter skin '%s' (%zu of %zu list skins provided by installed paks)",
            pick, present.size(), std::size(kStarterSkins));
    return pick;
}

std::vector<std::wstring> PakDirs() {
    // ExeDir = <game>/VotV/Binaries/Win64. Model paks auto-mount from ANY
    // subdirectory of <game>/VotV/Content/Paks/LogicMods (UE4 mounts the whole
    // tree at startup). 2026-08-29 (THUNDERSTORE.md blocker row 5): the scan
    // covers EVERY LogicMods SUBDIRECTORY, not just multivoid/ -- r2modman's
    // shimloader VFS-maps each package's pak dir to LogicMods\<Author>-<Name>\,
    // so a managed install lands the skin paks in a subdir we do not name.
    // The TOP LEVEL of LogicMods is deliberately excluded: that is where
    // foreign UE4SS BP mods live (DebugMod.pak), and listing those as skins
    // would offer unloadable garbage. multivoid/ sorts first (the manual-lane
    // convention + dev deploys) so its entries win the dedupe.
    //
    // `harness/mod_environment.cpp` reads this SAME directory for the opposite
    // question ("what is here that is NOT ours") and got it wrong on 2026-08-29 by
    // assuming the top level was the only place foreign paks sit -- true by hand,
    // false under shimloader. Keep the two in agreement: foreign BP paks live at
    // the top level on the manual lane AND one level down on the managed one.
    const std::wstring base = ue_wrap::paths::ExeDir();
    if (base.empty()) return {};
    std::error_code ec;
    fs::path logic = fs::path(base).parent_path().parent_path()
                     / L"Content" / L"Paks" / L"LogicMods";
    if (!fs::is_directory(logic, ec)) return {};
    std::vector<std::wstring> dirs;
    fs::path multivoid = logic / L"multivoid";
    if (fs::is_directory(multivoid, ec)) dirs.push_back(multivoid.wstring());
    for (const auto& de : fs::directory_iterator(logic, ec)) {
        if (!de.is_directory(ec)) continue;
        if (de.path() == multivoid) continue;
        dirs.push_back(de.path().wstring());
    }
    return dirs;
}

const std::vector<SkinEntry>& Entries(bool rescan) {
    if (g_scanned && !rescan) return g_entries;
    g_scanned = true;
    g_entries.clear();
    g_entries.push_back({kNativeSkinName, L""});  // the stock body is always offered

    const std::vector<std::wstring> dirs = PakDirs();

    // Builtin kerfur skins: always offered (game assets, no pak). Preview tile =
    // the same sidecar convention, looked up across the pak dirs by skin name.
    std::error_code ec;
    for (const auto& b : kBuiltinSkins) {
        std::wstring preview;
        for (const std::wstring& dirW : dirs) {
            for (const wchar_t* pext : {L".png", L".bmp"}) {
                fs::path cand = fs::path(dirW) / b.name;
                cand += pext;
                if (fs::is_regular_file(cand, ec)) { preview = cand.wstring(); break; }
            }
            if (!preview.empty()) break;
        }
        g_entries.push_back({b.name, std::move(preview)});
    }

    if (dirs.empty()) {
        UE_LOGW("skin_registry: no LogicMods pak subdirectory found -- dr_kel + builtins only");
        return g_entries;
    }
    // First dir wins a stem collision (multivoid/ sorts first -- the dev/manual
    // lane's copy beats a managed duplicate of the same model).
    std::vector<std::string> seen;
    for (const std::wstring& dirW : dirs) {
        for (const auto& de : fs::directory_iterator(dirW, ec)) {
            if (!de.is_regular_file(ec)) continue;
            const fs::path& p = de.path();
            std::wstring ext = p.extension().wstring();
            for (wchar_t& c : ext) c = static_cast<wchar_t>(::towlower(c));
            if (ext != L".pak") continue;
            const std::wstring stemW = p.stem().wstring();
            std::string stem;
            stem.reserve(stemW.size());
            bool ascii = true;
            for (wchar_t c : stemW) {
                if (c > 0x7F) { ascii = false; break; }
                stem.push_back(static_cast<char>(c));
            }
            if (!ascii || !IsValidSkinName(stem)) {
                UE_LOGW("skin_registry: pak '%ls' has a non-portable stem -- skipped (allowed: "
                        "[A-Za-z0-9_-], <=48 chars)", p.filename().c_str());
                continue;
            }
            if (stem == kNativeSkinName || BuiltinSkinPath(stem)) {
                UE_LOGW("skin_registry: pak '%ls' shadows a builtin skin name -- skipped "
                        "(rename the pak)", p.filename().c_str());
                continue;
            }
            // A bundle pak contributes its MEMBER skins, never its own stem.
            if (const SkinBundle* bundle = BundleForStem(stem)) {
                for (const char* const* m = bundle->members; *m; ++m) {
                    std::string member(*m);
                    if (std::find(seen.begin(), seen.end(), member) != seen.end())
                        continue;
                    seen.push_back(member);
                    std::wstring preview;
                    for (const wchar_t* pext : {L".png", L".bmp"}) {
                        fs::path cand = p.parent_path() / member;
                        cand += pext;
                        if (fs::is_regular_file(cand, ec)) { preview = cand.wstring(); break; }
                    }
                    g_entries.push_back({std::move(member), std::move(preview)});
                }
                continue;
            }
            if (std::find(seen.begin(), seen.end(), stem) != seen.end()) continue;
            seen.push_back(stem);
            // Preview tile = sibling <stem>.png or <stem>.bmp (user convention).
            std::wstring preview;
            for (const wchar_t* pext : {L".png", L".bmp"}) {
                fs::path cand = p; cand.replace_extension(pext);
                if (fs::is_regular_file(cand, ec)) { preview = cand.wstring(); break; }
            }
            g_entries.push_back({std::move(stem), std::move(preview)});
        }
    }
    UE_LOGI("skin_registry: %zu skin(s) catalogued (incl. dr_kel) across %zu LogicMods subdir(s)",
            g_entries.size(), dirs.size());
    return g_entries;
}

}  // namespace coop::skins
