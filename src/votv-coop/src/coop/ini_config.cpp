#include "coop/ini_config.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::ini_config {

namespace {

// DLL's own directory -- votv-coop.ini lives here next to the deployed DLL
// (same convention as freecam / pos_hud individual readers).
std::wstring ModuleDir() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? L"." : p.substr(0, sep);
}

// Strip whitespace + lowercase a line in place (case/space tolerant key matching).
std::string Normalize(const char* line) {
    std::string s(line);
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
            s.end());
    for (auto& c : s) c = static_cast<char>(::tolower(c));
    return s;
}

// Scan votv-coop.ini for `key=...` (the FIRST match wins; later duplicates are
// ignored). Returns Tristate-ish:  +1 = true,  0 = absent,  -1 = false.
//   true  matches: `key=1`  `key=true`
//   false matches: `key=0`  `key=false`
int LookupTriState(const char* key) {
    const std::wstring path = ModuleDir() + L"\\votv-coop.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return 0;
    char line[128];
    std::string trueForm  = std::string(key) + "=1";
    std::string trueAlt   = std::string(key) + "=true";
    std::string falseForm = std::string(key) + "=0";
    std::string falseAlt  = std::string(key) + "=false";
    int verdict = 0;
    while (std::fgets(line, sizeof(line), f)) {
        const std::string s = Normalize(line);
        if (s == trueForm || s == trueAlt) { verdict =  1; break; }
        if (s == falseForm || s == falseAlt) { verdict = -1; break; }
    }
    std::fclose(f);
    return verdict;
}

}  // namespace

bool MasterEnabled() {
    // ABSENT defaults to enabled (= granular switches decide). Only an explicit
    // `enabled=0` forces all dev features off.
    return LookupTriState("enabled") != -1;
}

bool IsIniKeyTrue(const char* key) {
    return LookupTriState(key) == 1;
}

bool IsOurWindowForeground() {
    HWND fg = ::GetForegroundWindow();
    if (!fg) return true;  // defensive: if we can't tell, don't break the hotkey
    DWORD ownerPid = 0;
    ::GetWindowThreadProcessId(fg, &ownerPid);
    return ownerPid == ::GetCurrentProcessId();
}

}  // namespace coop::ini_config
