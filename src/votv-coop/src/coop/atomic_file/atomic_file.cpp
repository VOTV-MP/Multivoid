// coop/atomic_file/atomic_file.cpp -- see coop/atomic_file/atomic_file.h for WHY.

#include "coop/atomic_file/atomic_file.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>

namespace coop::atomic_file {

namespace {

constexpr size_t kChunkBytes = 1024 * 1024;

Result Fail(Step step, DWORD error) {
    Result r;
    r.failedAt = step;
    r.error = error;
    return r;
}

DWORD MoveFlags(Mode mode, Sync sync) {
    DWORD flags = 0;
    if (mode == Mode::Replace) flags |= MOVEFILE_REPLACE_EXISTING;
    if (sync == Sync::ToDisk) flags |= MOVEFILE_WRITE_THROUGH;
    return flags;
}

// `.<1-10 digits>.tmp` at the end of `name`: the process id, when it fits a DWORD.
bool LeftoverPid(const std::wstring& name, unsigned long& pidOut) {
    constexpr wchar_t kTail[] = L".tmp";
    constexpr size_t kTailLen = 4;
    if (name.size() < kTailLen + 2) return false;
    for (size_t i = 0; i < kTailLen; ++i)
        if (std::towlower(name[name.size() - kTailLen + i]) != kTail[i]) return false;
    const size_t end = name.size() - kTailLen;
    size_t begin = end;
    while (begin > 0 && name[begin - 1] >= L'0' && name[begin - 1] <= L'9') --begin;
    const size_t digits = end - begin;
    if (digits < 1 || digits > 10 || begin == 0 || name[begin - 1] != L'.') return false;
    unsigned long long v = 0;
    for (size_t i = begin; i < end; ++i) v = v * 10 + static_cast<unsigned>(name[i] - L'0');
    if (v > 0xFFFFFFFFull) return false;
    pidOut = static_cast<unsigned long>(v);
    return true;
}

}  // namespace

std::filesystem::path TempPathFor(const std::filesystem::path& target) {
    return std::filesystem::path(target.native() + L"." +
                                 std::to_wstring(::GetCurrentProcessId()) + L".tmp");
}

bool ProcessRunning(unsigned long pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h) {
        ::CloseHandle(h);
        return true;
    }
    return ::GetLastError() != ERROR_INVALID_PARAMETER;
}

int RemoveLeftovers(const std::filesystem::path& dir, std::wstring_view targetPattern) {
    std::wstring folder = dir.native();
    if (folder.empty()) folder = L".";
    while (folder.size() > 1 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
    std::wstring search = folder + L"\\";
    search.append(targetPattern);
    search += L".*.tmp";

    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int deleted = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = fd.cFileName;
        unsigned long pid = 0;
        if (!LeftoverPid(name, pid)) continue;
        if (ProcessRunning(pid)) continue;
        if (::DeleteFileW((folder + L"\\" + name).c_str())) ++deleted;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return deleted;
}

Result Write(const std::filesystem::path& target, std::string_view bytes, Mode mode, Sync sync,
             const _SECURITY_ATTRIBUTES* security) {
    // An empty target is a caller whose folder is unknown: it would otherwise leave its temporary in
    // the working directory and sweep dead-pid temporaries there.
    if (target.empty()) return Fail(Step::Open, ERROR_PATH_NOT_FOUND);
    RemoveLeftovers(target.parent_path(), target.filename().native());
    const std::filesystem::path temp = TempPathFor(target);
    HANDLE h = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0,
                             const_cast<LPSECURITY_ATTRIBUTES>(security), CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Fail(Step::Open, ::GetLastError());

    Step failed = Step::None;
    DWORD error = 0;
    size_t off = 0;
    while (off < bytes.size()) {
        const DWORD n = static_cast<DWORD>((std::min)(kChunkBytes, bytes.size() - off));
        DWORD wrote = 0;
        if (!::WriteFile(h, bytes.data() + off, n, &wrote, nullptr) || wrote != n) {
            failed = Step::Write;
            error = ::GetLastError();
            break;
        }
        off += n;
    }
    if (failed == Step::None && sync == Sync::ToDisk && !::FlushFileBuffers(h)) {
        failed = Step::Flush;
        error = ::GetLastError();
    }
    if (!::CloseHandle(h) && failed == Step::None) {
        failed = Step::Close;
        error = ::GetLastError();
    }
    // No fallback on a failed move. MTA's CXMLFile::Write falls back to an in-place SaveFile
    // when WriteSafer fails (reference/mtasa-blue/Shared/XML/CXMLFileImpl.cpp:125-132); a copy
    // or an in-place write here would give up the atomicity this module exists for.
    if (failed == Step::None && !::MoveFileExW(temp.c_str(), target.c_str(), MoveFlags(mode, sync))) {
        failed = Step::Move;
        error = ::GetLastError();
    }
    if (failed != Step::None) {
        ::DeleteFileW(temp.c_str());
        return Fail(failed, error);
    }
    return {};
}

Result Commit(const std::filesystem::path& temp, const std::filesystem::path& target, Mode mode) {
    if (!::MoveFileExW(temp.c_str(), target.c_str(), MoveFlags(mode, Sync::Cached)))
        return Fail(Step::Move, ::GetLastError());
    return {};
}

bool TargetExisted(const Result& r) {
    return r.failedAt == Step::Move &&
           (r.error == ERROR_ALREADY_EXISTS || r.error == ERROR_FILE_EXISTS);
}

bool VolumeKeepsAcls(const std::filesystem::path& path) {
    std::wstring full = path.native();
    wchar_t root[MAX_PATH + 1] = {};
    if (!::GetVolumePathNameW(full.c_str(), root, MAX_PATH)) return false;
    DWORD flags = 0;
    if (!::GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0))
        return false;
    return (flags & FILE_PERSISTENT_ACLS) != 0;
}

std::string Describe(const Result& r) {
    const char* step = "none";
    switch (r.failedAt) {
        case Step::None: break;
        case Step::Open: step = "open"; break;
        case Step::Write: step = "write"; break;
        case Step::Flush: step = "flush"; break;
        case Step::Close: step = "close"; break;
        case Step::Move: step = "move"; break;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s failed (Win32 %lu)", step, r.error);
    return buf;
}

}  // namespace coop::atomic_file
