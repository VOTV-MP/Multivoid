// harness/autotest/peer_signal.cpp -- see peer_signal.h.

#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstdint>
#include <fstream>
#include <string>

namespace harness::autotest::peer_signal {
namespace {

std::string g_dir;
std::string g_tag;
uint64_t g_start = 0;  // FILETIME of this peer's drill start

bool WrittenSinceStart(const std::string& path) {
    const int n = ::MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, w.data(), n);
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!::GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &a)) return false;
    const uint64_t t = (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return t >= g_start;
}

}  // namespace

bool Begin(const char* logTag) {
    g_tag = logTag;
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    g_start = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("%hs: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset", g_tag.c_str());
        return false;
    }
    return true;
}

void Mark(const char* name) {
    std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << "x";
}

bool Await(const char* name, int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
        const std::string path = g_dir + "\\" + name;
        std::ifstream f(path, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty() && WrittenSinceStart(path)) return true;
        ::Sleep(100);
    }
    UE_LOGW("%hs: VERDICT ABORT -- no '%hs' from the other peer", g_tag.c_str(), name);
    return false;
}

namespace detail {
void SleepStep() { ::Sleep(100); }
void ReportTimeout(const char* what, int boundSeconds) {
    UE_LOGW("%hs: VERDICT ABORT -- %hs not within %ds", g_tag.c_str(), what, boundSeconds);
}
}  // namespace detail

}  // namespace harness::autotest::peer_signal
