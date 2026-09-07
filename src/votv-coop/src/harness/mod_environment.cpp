// harness/mod_environment.cpp -- see harness/mod_environment.h for WHY.

#include "harness/mod_environment.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <windows.h>

#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace harness::mod_environment {
namespace {

namespace fs  = std::filesystem;

// UE4SS 3.0.1 enable list: one "Name : 0|1" per line, ';' comments. The newer shimloader build has
// no mods.txt at all, and that is the faster configuration, so its absence is the good case and
// never an error.
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

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Is `dirName` the directory OUR OWN paks land in? Both install lanes are covered
// by one rule because both derive the name from the package name we choose:
// the manual lane uses LogicMods\multivoid\ (INSTALL.md), and shimloader VFS-maps
// each package to LogicMods\<Team>-<Name>\, which for us is Pelmentor-Multivoid.
bool IsOurPakDir(const std::wstring& dirName) {
    std::wstring lower = dirName;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(::towlower(c));
    if (lower == L"multivoid") return true;
    const std::wstring suffix = L"-multivoid";
    return lower.size() > suffix.size()
        && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Foreign Blueprint mods: any .pak under Content/Paks/LogicMods that is not ours.
//
// BOTH depths are listed, and the containing DIRECTORY is the discriminator. On a
// hand-install foreign paks sit at the top level; on the r2modman lane shimloader
// maps EVERY package's paks into LogicMods\<Team>-<Name>\, so a foreign pak sits
// one level down exactly where ours does, and a top-level-only listing reports
// "none" while the loader is demonstrably mounting one. Naming the directory is
// not the fragile name test that argument rejected: a pak FILEname is a player's
// to rename, while the containing directory is derived from the package identity.
// skin_registry.cpp reads the same directory on the same assumption.
std::vector<std::string> ForeignBpPaks(const std::wstring& exeDir) {
    std::vector<std::string> out;
    std::error_code ec;
    // exeDir = ...\VotV\Binaries\Win64  ->  ...\VotV\Content\Paks\LogicMods
    const fs::path logic = fs::path(exeDir).parent_path().parent_path()
                           / L"Content" / L"Paks" / L"LogicMods";
    if (!fs::is_directory(logic, ec)) return out;

    for (const auto& de : fs::directory_iterator(logic, ec)) {
        if (de.is_regular_file(ec)) {
            // Top level: the hand-install lane's home for foreign BP mods.
            if (de.path().extension() == L".pak") out.push_back(Narrow(de.path().filename()));
            continue;
        }
        if (!de.is_directory(ec)) continue;
        const std::wstring dirName = de.path().filename().wstring();
        if (IsOurPakDir(dirName)) continue;
        // One level down: the managed lane. Report "<dir>\<pak>" -- the player is
        // being asked to find these, and the bare filename does not locate them.
        for (const auto& f : fs::directory_iterator(de.path(), ec)) {
            if (!f.is_regular_file(ec)) continue;
            if (f.path().extension() != L".pak") continue;
            out.push_back(Narrow(dirName + L"\\" + f.path().filename().wstring()));
        }
    }
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

    const std::wstring exeDir = ue_wrap::paths::ExeDir();
    const std::vector<std::string> lua  = EnabledLuaMods(exeDir);
    const std::vector<std::string> paks = ForeignBpPaks(exeDir);

    if (lua.empty() && paks.empty()) {
        UE_LOGI("mod_env: no UE4SS Lua mods enabled and no foreign LogicMods paks -- "
                "nothing else is loaded beside Multivoid in this install.");
        return;
    }

    UE_LOGW("mod_env: UE4SS Lua mods enabled: [%s] | foreign LogicMods paks: [%s]",
            lua.empty()  ? "none" : Join(lua).c_str(),
            paks.empty() ? "none" : Join(paks).c_str());

    // NO PLAYER-FACING NOTICE: the census logs and tells the player nothing, for two independent
    // reasons.
    //
    // The advice it once carried is refuted. It said the foreign mod set costs frames; a controlled
    // measurement then moved the same rig from ~70 to ~118 fps by swapping the UE4SS build alone,
    // and found the whole enabled Lua set worth about 5 of those. The frames belong to the loader,
    // which the installer now pins, and telling a player to disable their other mods would buy them
    // almost nothing.
    //
    // And it was the wrong surface: boot_warning_dialog is, by its own header, the mod-install
    // problem modal -- a fixed MOD INSTALL PROBLEM title over a frame-rate notice, blocking the
    // menu until dismissed.
    //
    // The CENSUS stays and always logs: one directory read at boot, and it is what a bug report
    // needs.
}

}  // namespace harness::mod_environment
