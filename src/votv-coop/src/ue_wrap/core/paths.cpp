// ue_wrap/core/paths.cpp -- see ue_wrap/core/paths.h.

#include "ue_wrap/core/paths.h"

#include "ue_wrap/core/log.h"

#include <windows.h>
#include <wchar.h>

#include <utility>

namespace ue_wrap::paths {

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring p(path);
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? std::wstring{} : p.substr(0, sep);
}

bool ExistsBesideExe(const wchar_t* name, std::wstring* pathOut) {
    if (!name || !*name) return false;
    const std::wstring exe = ExeDir();
    if (exe.empty()) return false;
    // Where the data folder is the exe folder, a file of that name is the mod's own, not a leftover.
    if (_wcsicmp(exe.c_str(), ModDataDir().c_str()) == 0) return false;
    std::wstring path = exe + L"\\" + name;
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    if (pathOut) *pathOut = std::move(path);
    return true;
}

namespace {

constexpr DWORD kPathCap = 32768;  // the long-path ceiling: a module or profile path is never cut

std::wstring LeafOf(const std::wstring& p) {
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? p : p.substr(sep + 1);
}

std::wstring ParentOf(const std::wstring& p) {
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? std::wstring{} : p.substr(0, sep);
}

bool IsDirectory(const std::wstring& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// This module's own path, by the address of one of its own functions (the module is not the exe).
std::wstring OwnModulePath();

std::wstring ResolveModDataDir() {
    const std::wstring module = OwnModulePath();
    if (module.empty()) {
        UE_LOGW("paths: this module's own path could not be read -- the mod has no data folder");
        return {};
    }
    // <pkg>\dlls\main.dll: the data folder is <pkg>, the folder above dlls\.
    const std::wstring dllsDir = ParentOf(module);
    const std::wstring pkgDir = ParentOf(dllsDir);
    if (_wcsicmp(LeafOf(dllsDir).c_str(), L"dlls") != 0 || pkgDir.empty()) {
        UE_LOGW("paths: this module (%ls) is not under a dlls folder -- the mod has no data folder",
                module.c_str());
        return {};
    }
    std::wstring profile(kPathCap, L'\0');
    const DWORD n = ::GetEnvironmentVariableW(L"SHIMLOADER_MOD_DIR", profile.data(), kPathCap);
    if (n == 0) {
        UE_LOGI("paths: the mod's data folder is %ls", pkgDir.c_str());
        return pkgDir;
    }
    if (n >= kPathCap) {
        UE_LOGW("paths: SHIMLOADER_MOD_DIR is longer than %lu characters -- the mod has no data "
                "folder", static_cast<unsigned long>(kPathCap));
        return {};
    }
    profile.resize(n);
    while (!profile.empty() && (profile.back() == L'\\' ||profile.back() == L'/')) profile.pop_back();
    const std::wstring dir = profile + L"\\" + LeafOf(pkgDir);
    if (!IsDirectory(dir)) {
        UE_LOGW("paths: SHIMLOADER_MOD_DIR is set but its package folder %ls does not exist -- the "
                "mod has no data folder this session", dir.c_str());
        return {};
    }
    UE_LOGI("paths: the mod's data folder is %ls (the manager's profile)", dir.c_str());
    return dir;
}

std::wstring OwnModulePath() {
    HMODULE mod = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&ResolveModDataDir), &mod) ||
        mod == nullptr)
        return {};
    std::wstring buf(kPathCap, L'\0');
    const DWORD n = ::GetModuleFileNameW(mod, buf.data(), kPathCap);
    if (n == 0 || n >= kPathCap) return {};
    buf.resize(n);
    return buf;
}

}  // namespace

std::wstring ModDataDir() {
    static const std::wstring dir = ResolveModDataDir();
    return dir;
}

std::wstring Ue4ssModulePath() {
    const HMODULE h = ::GetModuleHandleW(L"UE4SS.dll");
    if (!h) return {};
    wchar_t path[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(h, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return path;
}

std::wstring Ue4ssDir() {
    const std::wstring p = Ue4ssModulePath();
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? std::wstring{} : p.substr(0, sep);
}

}  // namespace ue_wrap::paths
