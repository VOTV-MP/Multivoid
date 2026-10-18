// coop/permissions/permissions_selftest.cpp -- the boot runner of the permission model's selftest;
// see coop/permissions/permissions_selftest.h. This runner and `permission_host` (the in-game glue)
// are the only files of the folder that log or read config; every other file is engine-free.

#include "coop/permissions/permissions_selftest.h"

#include "coop/atomic_file/atomic_file.h"
#include "coop/config/config.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <mutex>
#include <string>
#include <system_error>

namespace coop::permissions {
namespace {

namespace fs = std::filesystem;

constexpr const wchar_t* kScratchPrefix = L"multivoid.selftest-permissions.";

void ReportFail(const char* what) { UE_LOGE("permissions selftest FAIL: %s", what); }

bool AllDigits(const std::wstring& s) {
    if (s.empty() || s.size() > 10) return false;
    for (const wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;
}

// Scratch folders of runs that are gone (a kill inside a selftest) are removed whole.
void SweepDeadScratch(const std::wstring& dir) {
    const std::wstring prefix = kScratchPrefix;
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((dir + L"\\" + prefix + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring name = fd.cFileName;
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::wstring digits = name.substr(prefix.size());
        if (!AllDigits(digits)) continue;
        const unsigned long long id = std::stoull(digits);
        if (id > 0xFFFFFFFFull) continue;
        if (atomic_file::ProcessRunning(static_cast<unsigned long>(id))) continue;
        std::error_code ec;
        fs::remove_all(fs::path(dir) / name, ec);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

}  // namespace

static bool RunSelftestBody() {
    CheckSink sink;
    sink.breakFirst = coop::config::ResolveFlag(::coop::config_registry::rows::selftest_break_permissions);
    sink.onFail = &ReportFail;

    // The files cases' disk case writes under a folder in the mod's data folder, named by this
    // process; an empty path tells them there is none.
    const std::wstring dataDir = ue_wrap::paths::ModDataDir();
    fs::path scratch;
    std::error_code ec;
    if (dataDir.empty()) {
        UE_LOGI("permissions selftest: disk cases skipped -- no data folder");
    } else {
        SweepDeadScratch(dataDir);
        scratch = fs::path(dataDir) / (std::wstring(kScratchPrefix) + std::to_wstring(::GetCurrentProcessId()));
        fs::remove_all(scratch, ec);
    }

    RunContextCases(sink);
    RunStoreCases(sink);
    RunInheritanceCases(sink);
    RunResolutionCases(sink);
    RunFilesCases(sink, scratch);
    RunActionLogCases(sink, scratch);
    RunEditCases(sink);
    RunGrantsCases(sink);

    if (!scratch.empty()) fs::remove_all(scratch, ec);

    if (sink.passed == sink.total) {
        UE_LOGI("permissions selftest: ALL PASS (%d checks)", sink.total);
        return true;
    }
    UE_LOGE("permissions selftest: %d/%d checks passed", sink.passed, sink.total);
    return false;
}

// Once per process: the disk case writes and sweeps a scratch folder, and a session Stop/Start
// would redo it; a later call returns the first run's verdict without output.
bool RunSelftest() {
    static std::once_flag ran;
    static bool verdict = false;
    std::call_once(ran, [] { verdict = RunSelftestBody(); });
    return verdict;
}

}  // namespace coop::permissions
