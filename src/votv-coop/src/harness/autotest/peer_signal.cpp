// harness/autotest/peer_signal.cpp -- see peer_signal.h.

#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/session/rig_verdict.h"
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

// The path in the wide form the file calls take; empty when it does not convert.
std::wstring Widen(const std::string& path) {
    const int n = ::MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, w.data(), n);
    return w;
}

// The first line of the signal file at `path`, read through a handle that shares delete as well as read and write: a peer
// leaving the signal renames its temp file over it, and Windows refuses that rename (error 5, access denied) for as long
// as any other process holds the target open without sharing delete, which an ifstream's handle does not. False when the
// file is not there or holds no line.
bool ReadFirstLine(const std::string& path, std::string& out) {
    const std::wstring w = Widen(path);
    if (w.empty()) return false;
    HANDLE h = ::CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD got = 0;
    const BOOL ok = ::ReadFile(h, buf, sizeof(buf), &got, nullptr);
    ::CloseHandle(h);
    if (!ok || got == 0) return false;
    size_t n = 0;
    while (n < got && buf[n] != '\n') ++n;
    out.assign(buf, n);
    return !out.empty();
}

bool WrittenSinceStart(const std::string& path) {
    const std::wstring w = Widen(path);
    if (w.empty()) return false;
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
        coop::rig_verdict::Say(g_tag.c_str(), coop::rig_verdict::Outcome::Invalid, "VOTVCOOP_CAN_DIR is unset");
        return false;
    }
    return true;
}

const std::string& Dir() { return g_dir; }

void Mark(const char* name) { Mark(name, "x"); }

// Written whole: the text goes to `<name>.tmp` and is renamed over `<name>`, so a reader polling the signal never
// sees a half-written line.
void Mark(const char* name, const std::string& text) {
    const std::string path = g_dir + "\\" + name;
    const std::string tmp = path + ".tmp";
    { std::ofstream(tmp, std::ios::binary | std::ios::trunc) << text; }
    const std::wstring from = Widen(tmp), to = Widen(path);
    if (from.empty() || to.empty() || !::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
        UE_LOGW("%hs: signal '%hs' could not be left (error %lu)", g_tag.c_str(), name, ::GetLastError());
}

bool Await(const char* name, int seconds) {
    std::string unused;
    return AwaitText(name, unused, seconds);
}

bool AwaitText(const char* name, std::string& out, int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
        const std::string path = g_dir + "\\" + name;
        std::string s;
        if (ReadFirstLine(path, s) && WrittenSinceStart(path)) {
            out = s;
            return true;
        }
        ::Sleep(100);
    }
    coop::rig_verdict::Say(g_tag.c_str(), coop::rig_verdict::Outcome::Invalid, "no '%s' from the other peer", name);
    return false;
}

bool Peek(const char* name, std::string& out) {
    const std::string path = g_dir + "\\" + name;
    std::string s;
    if (!ReadFirstLine(path, s) || !WrittenSinceStart(path)) return false;
    out = s;
    return true;
}

namespace detail {
void SleepStep() { ::Sleep(100); }
void ReportTimeout(const char* what, int boundSeconds) {
    coop::rig_verdict::Say(g_tag.c_str(), coop::rig_verdict::Outcome::Invalid, "%s not within %ds", what, boundSeconds);
}
}  // namespace detail

}  // namespace harness::autotest::peer_signal
