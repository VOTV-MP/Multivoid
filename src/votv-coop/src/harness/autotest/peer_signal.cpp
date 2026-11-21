// harness/autotest/peer_signal.cpp -- see peer_signal.h.

#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace harness::autotest::peer_signal {
namespace {

constexpr size_t kStampDigits = 16;  // a mark's file is `<name>.<stamp as 16 hex digits>`

std::string g_dir;
std::string g_tag;
uint64_t g_start = 0;  // FILETIME of this peer's drill start

struct OwnFile {
    std::wstring path;
    bool refused = false;  // an outside holder refused its delete (counted once)
};

std::mutex g_markLock;                                           // guards the three below
uint64_t g_lastStamp = 0;                                        // the newest stamp this process gave a mark
std::unordered_map<std::string, std::vector<OwnFile>> g_ownFiles;  // per name: the files this process left, oldest first
uint32_t g_deleteRefused = 0;                                    // files an outside holder refused a delete, all names

// The path in the wide form the file calls take; empty when it does not convert.
std::wstring Widen(const std::string& path) {
    const int n = ::MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, w.data(), n);
    w.resize(static_cast<size_t>(n) - 1);  // MultiByteToWideChar counted the terminator
    return w;
}

uint64_t Ticks(const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

// The first line of the signal file at `path`, read through a handle that shares delete as well as read and write, so a
// writer cleaning up its older marks is never refused by this read. A file counts only when it is COMPLETE: a writer
// creates it in place and ends it with '\n' in one write, so a file read while its writer is mid-create is empty or lacks
// the newline and is skipped. False when the file is not there, is incomplete, or its first line is empty.
bool ReadFirstLine(const std::wstring& path, std::string& out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string all;
    bool ok = ::GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= (1 << 20);
    if (ok) {
        all.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ::ReadFile(h, all.data(), static_cast<DWORD>(all.size()), &got, nullptr) && got == all.size();
    }
    ::CloseHandle(h);
    if (!ok || all.back() != '\n') return false;
    out.assign(all, 0, all.find('\n'));
    return !out.empty();
}

// Creates the file at `path` holding `text` and one '\n', in one write. `exists` is set when CREATE_NEW refused because
// the name is taken. Read and delete are shared, so a reader and the cleanup of an older mark meet no refusal.
bool CreateWhole(const std::wstring& path, const std::string& text, bool& exists) {
    exists = false;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        exists = e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS;
        return false;
    }
    const std::string line = text + "\n";
    DWORD put = 0;
    const bool ok = ::WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &put, nullptr) && put == line.size();
    const DWORD e = ::GetLastError();
    ::CloseHandle(h);
    if (!ok) {
        ::DeleteFileW(path.c_str());
        ::SetLastError(e);
    }
    return ok;
}

// The stamp in a file name `<name>.<16 hex digits>`; false for any other name, so a dotted signal name is never taken
// for a mark of `name`.
bool ParseStamp(const wchar_t* file, const std::wstring& prefix, uint64_t& stamp) {
    if (::_wcsnicmp(file, prefix.c_str(), prefix.size()) != 0) return false;
    const wchar_t* d = file + prefix.size();
    if (std::wcslen(d) != kStampDigits) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < kStampDigits; ++i) {
        const wchar_t c = d[i];
        uint64_t digit;
        if (c >= L'0' && c <= L'9') digit = static_cast<uint64_t>(c - L'0');
        else if (c >= L'a' && c <= L'f') digit = static_cast<uint64_t>(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') digit = static_cast<uint64_t>(c - L'A' + 10);
        else return false;
        v = (v << 4) | digit;
    }
    stamp = v;
    return true;
}

// The first line of the newest COMPLETE mark of `name` written since this peer's drill began: the `<name>.<stamp>` files
// whose last write is not before the start, highest stamp first, until one reads complete. A newer one still being
// created is skipped this poll.
bool ReadNewest(const char* name, std::string& out) {
    const std::wstring dir = Widen(g_dir), base = Widen(name);
    if (dir.empty() || base.empty()) return false;
    const std::wstring prefix = base + L".";
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((dir + L"\\" + prefix + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<std::pair<uint64_t, std::wstring>> marks;
    do {
        uint64_t stamp = 0;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !ParseStamp(fd.cFileName, prefix, stamp)) continue;
        if (Ticks(fd.ftLastWriteTime) < g_start) continue;
        marks.emplace_back(stamp, fd.cFileName);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    std::sort(marks.begin(), marks.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& m : marks)
        if (ReadFirstLine(dir + L"\\" + m.second, out)) return true;
    return false;
}

// A time-ordered name for the next mark: the system time in 100 ns units, never the same twice in this process.
uint64_t NextStamp() {
    FILETIME now{};
    ::GetSystemTimePreciseAsFileTime(&now);
    uint64_t t = Ticks(now);
    if (t <= g_lastStamp) t = g_lastStamp + 1;
    g_lastStamp = t;
    return t;
}

// Deletes this process's files of `name` older than its TWO newest (the last two of `files`). Keeping two removes the
// race by construction: a reader lists the newest mark and opens it one poll later, and the mark it listed is still
// there, since only a mark two newer than it is ever deleted. An outside holder (a scanner) can refuse a delete; the
// file stays on the list for the next mark of that name, and a leftover is harmless because a reader takes the newest.
// A file counts once, when first refused; the count is said every 256th file, never one by one.
void DeleteOlder(const std::string& name, std::vector<OwnFile>& files) {
    if (files.size() <= 2) return;
    const size_t doomed = files.size() - 2;
    std::vector<OwnFile> kept;
    for (size_t i = 0; i < files.size(); ++i) {
        OwnFile& f = files[i];
        if (i < doomed && (::DeleteFileW(f.path.c_str()) || ::GetLastError() == ERROR_FILE_NOT_FOUND)) continue;
        if (i < doomed && !f.refused) {
            f.refused = true;
            if (++g_deleteRefused % 256 == 0)
                UE_LOGI("%hs: %u older signal files were refused a delete by an outside holder (last: '%hs')", g_tag.c_str(),
                        g_deleteRefused, name.c_str());
        }
        kept.push_back(std::move(f));
    }
    files.swap(kept);
}

}  // namespace

bool Begin(const char* logTag) {
    g_tag = logTag;
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    g_start = Ticks(now);
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        coop::rig_verdict::Say(g_tag.c_str(), coop::rig_verdict::Outcome::Invalid, "VOTVCOOP_CAN_DIR is unset");
        return false;
    }
    return true;
}

const std::string& Dir() { return g_dir; }

void Mark(const char* name) { Mark(name, "x"); }

// Created in place, never over a file and never renamed: `<name>.<stamp>` is a name that does not exist yet (CREATE_NEW;
// a taken name takes the next stamp), so no holder of an older mark can refuse it. The text and its closing '\n' go
// in one write; a reader takes a file only once it ends with '\n'.
void Mark(const char* name, const std::string& text) {
    std::lock_guard<std::mutex> lock(g_markLock);
    const std::wstring base = Widen(g_dir + "\\" + name);
    if (base.empty()) {
        UE_LOGW("%hs: signal '%hs' could not be left (the path does not convert)", g_tag.c_str(), name);
        return;
    }
    std::wstring to;
    bool exists = true;
    for (int attempt = 0; exists && attempt < 64; ++attempt) {
        wchar_t stampText[kStampDigits + 1];
        std::swprintf(stampText, kStampDigits + 1, L"%016llx", static_cast<unsigned long long>(NextStamp()));
        to = base + L"." + stampText;
        if (CreateWhole(to, text, exists)) {
            exists = false;
            break;
        }
        if (!exists) {
            UE_LOGW("%hs: signal '%hs' could not be left (error %lu)", g_tag.c_str(), name, ::GetLastError());
            return;
        }
    }
    if (exists) {
        UE_LOGW("%hs: signal '%hs' could not be left (every stamp tried was taken)", g_tag.c_str(), name);
        return;
    }
    std::vector<OwnFile>& files = g_ownFiles[name];
    files.push_back(OwnFile{to, false});
    DeleteOlder(name, files);
}

bool Await(const char* name, int seconds) {
    std::string unused;
    return AwaitText(name, unused, seconds);
}

bool AwaitText(const char* name, std::string& out, int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::string s;
        if (ReadNewest(name, s)) {
            out = s;
            return true;
        }
        ::Sleep(100);
    }
    coop::rig_verdict::Say(g_tag.c_str(), coop::rig_verdict::Outcome::Invalid, "no '%s' from the other peer", name);
    return false;
}

bool Peek(const char* name, std::string& out) {
    std::string s;
    if (!ReadNewest(name, s)) return false;
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
