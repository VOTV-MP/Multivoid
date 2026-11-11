// coop/save/save_scratch.cpp -- see coop/save/save_scratch.h.

#include "coop/save/save_scratch.h"

#include "coop/save/save_guard.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <chrono>
#include <cwchar>
#include <cwctype>
#include <vector>

namespace fs = std::filesystem;

namespace coop::save_scratch {
namespace {

constexpr const wchar_t* kDirName = L"multivoid_session";
constexpr const wchar_t* kJoinerPrefix = L"zcoop_";
constexpr const wchar_t* kCaptureLeaf = L"zcoop_hostxfer";
constexpr auto kCaptureMaxAge = std::chrono::minutes(1);

// Is process `pid` running? A process that cannot be opened for a reason other than "no such process"
// (a sibling started by another user, a protected process) counts as running: a folder is never taken
// from under a process that may still own it.
bool ProcessRuns(DWORD pid) {
    HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return ::GetLastError() != ERROR_INVALID_PARAMETER;
    const bool running = ::WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    ::CloseHandle(h);
    return running;
}

// The pid in a joiner's folder name "zcoop_<pid>"; 0 when `leaf` is not one.
DWORD JoinerPid(const std::wstring& leaf) {
    const size_t prefix = std::wcslen(kJoinerPrefix);
    if (leaf.size() <= prefix || leaf.compare(0, prefix, kJoinerPrefix) != 0) return 0;
    DWORD pid = 0;
    for (size_t i = prefix; i < leaf.size(); ++i) {
        if (leaf[i] < L'0' || leaf[i] > L'9') return 0;
        pid = pid * 10 + static_cast<DWORD>(leaf[i] - L'0');
    }
    return pid;
}

}  // namespace

fs::path Root() {
    const fs::path saves = coop::save_guard::SaveGamesDir();
    return saves.empty() ? fs::path{} : saves.parent_path() / kDirName;
}

fs::path Dir(const std::wstring& leaf) {
    const fs::path root = Root();
    return root.empty() ? fs::path{} : root / leaf;
}

std::wstring FolderName(const std::wstring& leaf) { return std::wstring(L"../") + kDirName + L"/" + leaf; }

bool IsScratch(const wchar_t* folderPath) {
    if (!folderPath) return false;
    std::wstring path(folderPath);
    for (wchar_t& c : path) c = c == L'\\' ? L'/' : static_cast<wchar_t>(std::towlower(c));
    const std::wstring part = std::wstring(L"/") + kDirName + L"/";
    return path.find(part) != std::wstring::npos;
}

void Remove(const std::wstring& leaf) {
    const fs::path dir = Dir(leaf);
    if (dir.empty()) return;
    std::error_code ec;
    if (fs::remove_all(dir, ec) > 0) UE_LOGI("save_scratch: deleted '%ls'", dir.c_str());
}

void SweepStale() {
    const fs::path root = Root();
    if (root.empty()) return;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    std::vector<std::wstring> stale;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        const std::wstring leaf = e.path().filename().wstring();
        bool isStale = false;
        if (const DWORD pid = JoinerPid(leaf)) {
            isStale = !ProcessRuns(pid);
        } else if (leaf == kCaptureLeaf) {
            const auto written = fs::last_write_time(e.path(), ec);
            isStale = !ec && now - written > kCaptureMaxAge;
        }
        if (isStale) stale.push_back(leaf);
    }
    for (const std::wstring& leaf : stale) Remove(leaf);
}

}  // namespace coop::save_scratch
