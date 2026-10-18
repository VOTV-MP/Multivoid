// coop/atomic_file/atomic_file.h -- one writer for every file the mod stores: the bytes go to a
// temporary beside the target, are optionally flushed, and are moved into place in one step, so a
// kill or a full disk leaves the target as the old file or the new one and never a cut one.
//
// MTA writes a `_new_` file and swaps through an `_old_` one with a recovery flag
// (reference/mtasa-blue/Shared/XML/CXMLFileImpl.cpp:138-180); its rename is the CRT's _wrename
// (reference/mtasa-blue/Shared/sdk/SharedUtil.File.hpp:1678), which does not replace an
// existing file. MoveFileExW with MOVEFILE_REPLACE_EXISTING replaces in one step, so on NTFS no
// journal is needed -- the target is the old file or the new one -- and the flush is ours.
//
// Engine-free (Win32 only) and silent: each caller keeps its own log line and its own failure
// policy, since "the ban holds for this session only" and "the ini left unchanged" are the
// caller's facts. Any thread; no state and no lock (each site already serialises its own target).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

struct _SECURITY_ATTRIBUTES;

namespace coop::atomic_file {

enum class Mode : uint8_t { Replace, CreateOnly };
enum class Sync : uint8_t { ToDisk, Cached };
enum class Step : uint8_t { None, Open, Write, Flush, Close, Move };

struct Result {
    Step failedAt = Step::None;
    unsigned long error = 0;  // GetLastError() at the failed step
    bool ok() const { return failedAt == Step::None; }
};

// <target>.<this process id>.tmp -- the process id because two copies of the game may run from
// one folder.
std::filesystem::path TempPathFor(const std::filesystem::path& target);

// Whether `pid` names a running process: false only when OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)
// fails with ERROR_INVALID_PARAMETER; a process this one may not open is running.
bool ProcessRunning(unsigned long pid);

// An empty target is refused first (failedAt == Step::Open, ERROR_PATH_NOT_FOUND), touching nothing.
// Deletes the target's leftovers (RemoveLeftovers over its folder and its own file name), then
// writes `bytes` to TempPathFor(target) (CreateFileW CREATE_ALWAYS, GENERIC_WRITE, no sharing,
// `security` as given), WriteFile in chunks of at most 1 MiB, FlushFileBuffers when ToDisk,
// CloseHandle checked, then MoveFileExW with REPLACE_EXISTING for Replace and WRITE_THROUGH for
// ToDisk. Any failure deletes the temporary. Never creates a folder.
Result Write(const std::filesystem::path& target, std::string_view bytes, Mode mode, Sync sync,
             const _SECURITY_ATTRIBUTES* security = nullptr);

// For a writer that streamed into TempPathFor(target) itself and closed it (the bug report's
// zip, its only user): the move as Write's, never flushed. A failure leaves the temporary in
// place (its owner retries or removes it).
Result Commit(const std::filesystem::path& temp, const std::filesystem::path& target, Mode mode);

// failedAt == Step::Move and the error is ERROR_ALREADY_EXISTS or ERROR_FILE_EXISTS.
bool TargetExisted(const Result& r);

// Deletes each `<dir>\<name>` that FindFirstFileW returns for `targetPattern + L".*.tmp"` (the
// pattern may hold `*`: `report-*.zip`) whose long name ends in `.<1-10 ASCII digits>.tmp` (a
// digit run past a DWORD is not ours and is skipped) and whose process is not running
// (ProcessRunning false); returns how many it DELETED.
int RemoveLeftovers(const std::filesystem::path& dir, std::wstring_view targetPattern);

// Whether the volume holding `path` keeps ACLs: GetVolumePathNameW, then GetVolumeInformationW's
// FILE_PERSISTENT_ACLS. For the selftests that read a DACL back.
bool VolumeKeepsAcls(const std::filesystem::path& path);

// "<step> failed (Win32 <error>)", the step in lower case: "open", "write", "flush", "close",
// "move".
std::string Describe(const Result& r);

// The un-gated boot selftest, once per process (a later call returns the first verdict, silently):
// `atomic_file selftest: ALL PASS (N checks)`, or one `atomic_file selftest FAIL: <case>` per
// failure and `atomic_file selftest: M/N checks passed`.
bool RunSelftest();

}  // namespace coop::atomic_file
