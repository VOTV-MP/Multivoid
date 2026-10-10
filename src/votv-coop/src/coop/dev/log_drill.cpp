// coop/dev/log_drill.cpp -- see coop/dev/log_drill.h.

#include "coop/dev/log_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace coop::dev::log_drill {
namespace {

// The usual file's process line ("process <PID>", line 3 of every log), read while its writer holds it: the
// writer shares reads, so a reader asks for write sharing too. 0 when it does not read.
unsigned long LiveWriter(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[512] = {};
    DWORD got = 0;
    const BOOL ok = ::ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    ::CloseHandle(h);
    if (!ok) return 0;
    buf[got] = '\0';
    const char* line = std::strstr(buf, "\nprocess ");
    return line ? std::strtoul(line + 9, nullptr, 10) : 0;
}

// Whether `pid` names a running process: a log left by an exited one, still held open by a viewer, is not another
// writer.
bool Running(unsigned long pid) {
    HANDLE p = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    DWORD code = 0;
    const bool live = ::GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    ::CloseHandle(p);
    return live;
}

}  // namespace

void RunOnce() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::log_drill);
    if (!on) return;
    const unsigned long me = ::GetCurrentProcessId();
    const std::wstring mine = ue_wrap::log::CurrentPath();
    const std::wstring base = ue_wrap::log::BasePath();
    if (_wcsicmp(mine.c_str(), base.c_str()) == 0) {
        UE_LOGI("[LOG-DRILL] process %lu writes the install's usual log '%ls'", me, mine.c_str());
        return;
    }
    std::wstring expected = base;
    if (expected.size() > 4 && _wcsicmp(expected.c_str() + expected.size() - 4, L".log") == 0) expected.resize(expected.size() - 4);
    expected += L"." + std::to_wstring(me) + L".log";
    const unsigned long other = LiveWriter(base);
    const bool running = other != 0 && other != me && Running(other);
    if (_wcsicmp(mine.c_str(), expected.c_str()) != 0 || !running) {
        UE_LOGW("[LOG-DRILL] FAIL: process %lu writes '%ls' (expected '%ls'); the usual log names process %lu "
                "(running=%d; 0 = its process line unread)", me, mine.c_str(), expected.c_str(), other, running ? 1 : 0);
        return;
    }
    UE_LOGI("[LOG-DRILL] DONE: process %lu writes its own '%ls'; the usual log is process %lu's -- PASS", me,
            mine.c_str(), other);
}

}  // namespace coop::dev::log_drill
