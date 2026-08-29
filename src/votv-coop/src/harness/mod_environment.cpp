// harness/mod_environment.cpp -- see harness/mod_environment.h for WHY.

#include "harness/mod_environment.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"
#include "ui/boot_warning_dialog.h"

#include <windows.h>

#include <string>
#include <vector>

namespace harness::mod_environment {
namespace {

namespace cfg = coop::config;

// UE4SS 3.0.1 enable list: one "Name : 0|1" per line, ';' comments. The newer
// shimloader lane has no mods.txt at all -- that is the CONFIGURATION THAT WAS
// MEASURED AT 120 fps, so its absence is the good case, never an error.
std::vector<std::string> EnabledLuaMods(const std::wstring& exeDir) {
    std::vector<std::string> out;
    const std::wstring path = exeDir + L"\\Mods\\mods.txt";
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;   // no list -> nothing enabled here
    std::string text;
    char buf[4096];
    DWORD got = 0;
    while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) text.append(buf, got);
    ::CloseHandle(h);
    // UE4SS ships mods.txt with a UTF-8 BOM. Without this the first entry reads
    // "\xEF\xBB\xBFCheatManagerEnablerMod" and lands in a player-facing dialog with
    // a stray glyph in front of it -- caught by actually reading the first run's
    // output rather than by trusting the parse.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
                         && static_cast<unsigned char>(text[1]) == 0xBB
                         && static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);

    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;
        // strip CR + leading blanks, drop comments and blank lines
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        line = line.substr(s);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::string val  = line.substr(colon + 1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        const size_t vs = val.find_first_not_of(" \t");
        if (vs == std::string::npos) continue;
        if (val[vs] != '1') continue;            // 0 / anything else = off
        if (!name.empty()) out.push_back(name);
    }
    return out;
}

// Foreign Blueprint mods: a .pak sitting DIRECTLY in Content/Paks/LogicMods.
// Ours live one level down in LogicMods\multivoid\, so the flat listing is the
// discriminator and no name matching is needed (a name test would break the
// moment someone renames a pak, which is the thing a player is most likely to do).
std::vector<std::string> ForeignBpPaks(const std::wstring& exeDir) {
    std::vector<std::string> out;
    // exeDir = ...\VotV\Binaries\Win64  ->  ...\VotV\Content\Paks\LogicMods
    const std::wstring glob = exeDir + L"\\..\\..\\Content\\Paks\\LogicMods\\*.pak";
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(glob.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char nb[MAX_PATH]{};
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nb, sizeof(nb) - 1,
                                            nullptr, nullptr);
        if (n > 0) out.push_back(std::string(nb));
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return out;
}

std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ", "; s += v[i]; }
    return s;
}

bool g_ran = false;

}  // namespace

void Run() {
    if (g_ran) return;
    g_ran = true;
    if (!cfg::ResolveFlag(::coop::config_registry::rows::warn_perf_mods)) return;

    const std::wstring exeDir = ue_wrap::paths::ExeDir();
    const std::vector<std::string> lua  = EnabledLuaMods(exeDir);
    const std::vector<std::string> paks = ForeignBpPaks(exeDir);

    if (lua.empty() && paks.empty()) {
        UE_LOGI("mod_env: no UE4SS Lua mods enabled and no foreign LogicMods paks -- "
                "this is the configuration measured at full frame rate (2026-08-29).");
        return;
    }

    UE_LOGW("mod_env: UE4SS Lua mods enabled: [%s] | foreign LogicMods paks: [%s]",
            lua.empty()  ? "none" : Join(lua).c_str(),
            paks.empty() ? "none" : Join(paks).c_str());

    // The wording states the SET figure and never a per-mod one -- see the header's
    // HONESTY BOUND. It also says plainly that this is not Multivoid, because the
    // player's next move otherwise is to blame the mod they just installed.
    std::string msg =
        "FRAME RATE NOTICE\n\n"
        "Other mods are loaded in this game besides Multivoid, and on the machine "
        "where this was measured they were expensive.\n\n";
    if (!lua.empty())
        msg += "UE4SS Lua mods enabled (Binaries\\Win64\\Mods\\mods.txt):\n  " + Join(lua) + "\n\n";
    if (!paks.empty())
        msg += "Blueprint mods in Content\\Paks\\LogicMods:\n  " + Join(paks) + "\n\n";
    msg +=
        "Measured 2026-08-29, same save and same Multivoid build: turning the whole "
        "set off took the game from about 75 fps to about 120. Multivoid itself "
        "measured at no detectable cost in the same test.\n\n"
        "That figure is for the SET, not for any one mod -- BPModLoaderMod is what "
        "loads the Blueprint paks, so their costs are not separable. If you want the "
        "frames back, set the entries in mods.txt to 0. If you want these mods, keep "
        "them; nothing here is broken.\n\n"
        "Silence this notice with warn.perf_mods=0 in multivoid.ini.";
    ui::boot_warning_dialog::Arm(msg);
}

}  // namespace harness::mod_environment
