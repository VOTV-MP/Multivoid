// ue_wrap/core/paths.cpp -- see ue_wrap/core/paths.h.

#include "ue_wrap/core/paths.h"

#include <windows.h>

namespace ue_wrap::paths {

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring p(path);
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? std::wstring{} : p.substr(0, sep);
}

}  // namespace ue_wrap::paths
