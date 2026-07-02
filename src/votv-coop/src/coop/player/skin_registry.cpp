// coop/player/skin_registry.cpp -- see coop/player/skin_registry.h.

#include "coop/player/skin_registry.h"

#include "harness/config.h"  // ModuleDir -- the pak folder is derived from the DLL location
#include "ue_wrap/log.h"

#include <cwctype>
#include <filesystem>

namespace coop::skins {

namespace {
namespace fs = std::filesystem;

std::vector<SkinEntry> g_entries;
bool g_scanned = false;

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

std::wstring PakDir() {
    // ModuleDir = <game>/VotV/Binaries/Win64 (the proxy DLL's folder). The model
    // paks auto-mount from <game>/VotV/Content/Paks/LogicMods/votv-coop.
    const std::wstring base = harness::config::ModuleDir();
    if (base.empty()) return {};
    std::error_code ec;
    fs::path p = fs::path(base).parent_path().parent_path()
                 / L"Content" / L"Paks" / L"LogicMods" / L"votv-coop";
    if (!fs::is_directory(p, ec)) return {};
    return p.wstring();
}

const std::vector<SkinEntry>& Entries(bool rescan) {
    if (g_scanned && !rescan) return g_entries;
    g_scanned = true;
    g_entries.clear();
    g_entries.push_back({kNativeSkinName, L""});  // the stock body is always offered

    const std::wstring dirW = PakDir();
    if (dirW.empty()) {
        UE_LOGW("skin_registry: LogicMods/votv-coop pak dir not found -- only dr_kel offered");
        return g_entries;
    }
    std::error_code ec;
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
        // Preview tile = sibling <stem>.png or <stem>.bmp (user convention).
        std::wstring preview;
        for (const wchar_t* pext : {L".png", L".bmp"}) {
            fs::path cand = p; cand.replace_extension(pext);
            if (fs::is_regular_file(cand, ec)) { preview = cand.wstring(); break; }
        }
        g_entries.push_back({std::move(stem), std::move(preview)});
    }
    UE_LOGI("skin_registry: %zu skin(s) catalogued (incl. dr_kel) from %ls",
            g_entries.size(), dirW.c_str());
    return g_entries;
}

}  // namespace coop::skins
