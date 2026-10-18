// coop/atomic_file/atomic_file_selftest.cpp -- the un-gated boot selftest of the one file writer,
// run once per process on both peers (shape: coop/moderation/ban_list_selftest.cpp). It
// writes real files in a scratch folder in the mod's folder: a writer that leaves a cut file or
// a stray temporary loses an ini, a ban or a key, and nothing else would notice.
//
// Red arm: with the dev row selftest_break_atomic_file the first case expects the wrong bytes, so
// the run must fail.

#include "coop/atomic_file/atomic_file.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <tlhelp32.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace coop::atomic_file {
namespace {

namespace fs = std::filesystem;

constexpr const wchar_t* kScratchPrefix = L"multivoid.selftest-atomic.";

bool ReadAll(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool Reads(const fs::path& p, const std::string& want) {
    std::string got;
    return ReadAll(p, got) && got == want;
}

bool HandWrite(const fs::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

int CountTmp(const fs::path& dir) {
    int n = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((dir.native() + L"\\*.tmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        ++n;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return n;
}

// Whether the process list holds `id`: the live-other-process arm of case 8 needs a process this
// one may not open, and only Windows NT's System (id 4) is one by name; Wine need not list it.
bool ProcessListed(DWORD id) {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL more = ::Process32FirstW(snap, &pe); more; more = ::Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == id) {
            found = true;
            break;
        }
    }
    ::CloseHandle(snap);
    return found;
}

bool AllDigits(const std::wstring& s) {
    if (s.empty() || s.size() > 10) return false;
    for (wchar_t c : s)
        if (c < L'0' || c > L'9') return false;
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
        if (ProcessRunning(static_cast<unsigned long>(id))) continue;
        std::error_code ec;
        fs::remove_all(fs::path(dir) / name, ec);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

// The key file's access list shape (coop/net/identity_crypto KeyFileAcl): this account and the
// system, full control, nothing inherited, protected. Built here because this module may not
// include coop/net.
struct ProtectedDacl {
    std::vector<uint8_t> user;
    uint8_t system[SECURITY_MAX_SID_SIZE]{};
    PACL acl = nullptr;
    SECURITY_DESCRIPTOR sd{};
    SECURITY_ATTRIBUTES sa{};
    ~ProtectedDacl() {
        if (acl) ::LocalFree(acl);
    }
};

bool BuildProtectedDacl(ProtectedDacl& d) {
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    DWORD need = 0;
    ::GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
    std::vector<uint8_t> buf(need ? need : 1);
    const bool got = need != 0 && ::GetTokenInformation(tok, TokenUser, buf.data(), need, &need);
    ::CloseHandle(tok);
    if (!got) return false;
    PSID user = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    const auto* bytes = static_cast<const uint8_t*>(user);
    d.user.assign(bytes, bytes + ::GetLengthSid(user));
    DWORD cb = sizeof(d.system);
    if (!::CreateWellKnownSid(WinLocalSystemSid, nullptr, d.system, &cb)) return false;
    EXPLICIT_ACCESS_W ea[2] = {};
    for (auto& e : ea) {
        e.grfAccessPermissions = FILE_ALL_ACCESS;
        e.grfAccessMode = SET_ACCESS;
        e.grfInheritance = NO_INHERITANCE;
        e.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    }
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(d.user.data());
    ea[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(d.system);
    if (::SetEntriesInAclW(2, ea, nullptr, &d.acl) != ERROR_SUCCESS) {
        d.acl = nullptr;
        return false;
    }
    if (!::InitializeSecurityDescriptor(&d.sd, SECURITY_DESCRIPTOR_REVISION) ||
        !::SetSecurityDescriptorDacl(&d.sd, TRUE, d.acl, FALSE) ||
        !::SetSecurityDescriptorControl(&d.sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        return false;
    d.sa.nLength = sizeof(d.sa);
    d.sa.lpSecurityDescriptor = &d.sd;
    d.sa.bInheritHandle = FALSE;
    return true;
}

// The DACL of `path` as read back: whether it is protected, and how many entries it holds.
bool ReadDacl(const fs::path& path, bool& protectedOut, DWORD& aceCountOut) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR psd = nullptr;
    if (::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                nullptr, &dacl, nullptr, &psd) != ERROR_SUCCESS)
        return false;
    SECURITY_DESCRIPTOR_CONTROL ctrl = 0;
    DWORD rev = 0;
    ACL_SIZE_INFORMATION info{};
    const bool ok = dacl != nullptr && ::GetSecurityDescriptorControl(psd, &ctrl, &rev) != 0 &&
                    ::GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation) != 0;
    if (ok) {
        protectedOut = (ctrl & SE_DACL_PROTECTED) != 0;
        aceCountOut = info.AceCount;
    }
    ::LocalFree(psd);
    return ok;
}

}  // namespace

static bool RunSelftestBody() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("atomic_file selftest FAIL: %s", what);
    };
    // A seed write (the setup before a case's assertion) names its own failure first, and adds a
    // check only when it fails; checks that depend on the seed may still fail after it.
    auto seeded = [&](bool ok, const char* what) {
        if (!ok) check(false, what);
        return ok;
    };
    const bool breakIt =
        coop::config::ResolveFlag(coop::config_registry::rows::selftest_break_atomic_file);

    {
        const Result r = Write(fs::path{}, "x", Mode::Replace, Sync::Cached);
        check(!r.ok() && r.failedAt == Step::Open, "case 0: an empty target is refused");
    }

    const std::wstring dataDir = ue_wrap::paths::ModDataDir();
    if (dataDir.empty()) {
        if (pass != total) {
            UE_LOGE("atomic_file selftest: %d/%d checks passed", pass, total);
            return false;
        }
        if (breakIt) {
            UE_LOGE("atomic_file selftest: the red arm cannot run -- no data folder, so the disk cases "
                    "it breaks are skipped");
            return false;
        }
        UE_LOGI("atomic_file selftest: disk cases skipped, the mod has no data folder (%d checks passed)",
                total);
        return true;
    }
    SweepDeadScratch(dataDir);
    const DWORD pid = ::GetCurrentProcessId();
    const fs::path scratch = fs::path(dataDir) / (std::wstring(kScratchPrefix) + std::to_wstring(pid));
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    check(fs::is_directory(scratch), "the scratch folder is created");

    const fs::path y = scratch / L"y.json";
    const std::wstring pidText = std::to_wstring(pid);

    // 1. A new file, Replace/ToDisk: whole, and no temporary left.
    {
        const Result r = Write(y, "a", Mode::Replace, Sync::ToDisk);
        check(r.ok() && Reads(y, breakIt ? "b" : "a") && CountTmp(scratch) == 0,
              "case 1: Replace/ToDisk to a new file reads back and leaves no temporary");
    }
    // 2. Replace/Cached over it.
    {
        const Result r = Write(y, "bb", Mode::Replace, Sync::Cached);
        check(r.ok() && Reads(y, "bb"), "case 2: Replace/Cached over an existing file");
    }
    // 3. CreateOnly over it: refused, the file kept, no temporary.
    {
        const Result r = Write(y, "ccc", Mode::CreateOnly, Sync::Cached);
        check(TargetExisted(r) && Reads(y, "bb") && CountTmp(scratch) == 0,
              "case 3: CreateOnly over an existing file is refused and keeps it");
    }
    // 4. CreateOnly to a new name.
    {
        const Result r = Write(scratch / L"n.json", "d", Mode::CreateOnly, Sync::ToDisk);
        check(r.ok() && Reads(scratch / L"n.json", "d"), "case 4: CreateOnly to a new name");
    }
    // 5. A folder that does not exist is not created.
    {
        const fs::path missing = scratch / L"nope";
        const Result r = Write(missing / L"x.json", "e", Mode::Replace, Sync::Cached);
        check(r.failedAt == Step::Open && !fs::exists(missing),
              "case 5: a missing folder fails at open and is not created");
    }
    // 6. The temporary's name and the failure text.
    {
        const bool name = TempPathFor(y).native() == y.native() + L"." + pidText + L".tmp";
        Result moved;
        moved.failedAt = Step::Move;
        moved.error = 5;
        check(name && Describe(moved) == "move failed (Win32 5)",
              "case 6: TempPathFor names <target>.<pid>.tmp and Describe prints the step");
    }
    // 7. Commit: a streamed temporary moved in, CreateOnly refusing, the probe name free.
    {
        const fs::path z = scratch / L"z.zip";
        const fs::path z2 = scratch / L"z-2.zip";
        const fs::path tmp = TempPathFor(z);
        seeded(HandWrite(tmp, "zip1"), "case 7: its first hand-written temporary was not written");
        const Result r1 = Commit(tmp, z, Mode::CreateOnly);
        seeded(HandWrite(tmp, "zip2"), "case 7: its second hand-written temporary was not written");
        const Result r2 = Commit(tmp, z, Mode::CreateOnly);
        const bool stayed = fs::exists(tmp) && Reads(z, "zip1");
        const Result r3 = Commit(tmp, z2, Mode::CreateOnly);
        check(r1.ok() && TargetExisted(r2) && stayed && r3.ok() && Reads(z2, "zip2") &&
                  !fs::exists(tmp),
              "case 7: Commit moves a temporary in, a refusal leaves it, the next name takes it");
    }
    // 8. Leftovers: a dead process's goes, a running one's and ours stay; a pattern with `*`.
    // The live-other arm needs a process this one may not open: System (id 4) on Windows NT,
    // which Wine/Proton need not have, so that arm runs only when the process list holds it.
    {
        const fs::path dead = scratch / L"y.json.1.tmp";
        const fs::path system = scratch / L"y.json.4.tmp";
        const fs::path ours = scratch / (L"y.json." + pidText + L".tmp");
        const bool liveOther = ProcessListed(4);
        if (!liveOther)
            UE_LOGI("atomic_file selftest: case 8 live-other arm not run (no process 4 here)");
        seeded(HandWrite(dead, "x"), "case 8a: the dead process's temporary was not written");
        if (liveOther)
            seeded(HandWrite(system, "x"), "case 8a: process 4's temporary was not written");
        seeded(HandWrite(ours, "x"), "case 8a: our own temporary was not written");
        const int removed = RemoveLeftovers(scratch, L"y.json");
        const bool kept = !fs::exists(dead) && fs::exists(ours) && (!liveOther || fs::exists(system));
        check(removed == 1 && kept && !ProcessRunning(1) && (!liveOther || ProcessRunning(4)),
              "case 8a: RemoveLeftovers deletes only a dead process's temporary");
        seeded(HandWrite(dead, "x"), "case 8b: the dead process's temporary was not written");
        const Result r = Write(y, "ff", Mode::Replace, Sync::Cached);
        check(r.ok() && !fs::exists(dead) && Reads(y, "ff"),
              "case 8b: a write sweeps its own target's dead leftover");
        const fs::path report = scratch / L"report-x.zip.1.tmp";
        seeded(HandWrite(report, "x"), "case 8c: the report's dead temporary was not written");
        check(RemoveLeftovers(scratch, L"report-*.zip") == 1 && !fs::exists(report),
              "case 8c: RemoveLeftovers takes a pattern with a wildcard");
        fs::remove(system, ec);
    }
    // 9. More than one chunk.
    {
        std::string big(3u * 1024 * 1024, '\0');
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + (i >> 11));
        const Result r = Write(scratch / L"big.bin", big, Mode::Replace, Sync::Cached);
        check(r.ok() && Reads(scratch / L"big.bin", big), "case 9: a 3 MiB write reads back equal");
    }
    // 10. The access list rides the write, and survives the move over an inherited-list file.
    if (VolumeKeepsAcls(scratch)) {
        ProtectedDacl d;
        bool isProtected = false;
        DWORD aces = 0;
        bool ok = BuildProtectedDacl(d);
        if (ok) {
            const Result r = Write(y, "key", Mode::Replace, Sync::ToDisk, &d.sa);
            ok = r.ok() && ReadDacl(y, isProtected, aces);
        }
        check(ok && isProtected && aces == 2,
              "case 10: the written file's access list is protected with two entries");
    } else {
        UE_LOGI("atomic_file selftest: case 10 not run (the volume keeps no ACLs)");
    }

    fs::remove_all(scratch, ec);
    if (pass == total) {
        UE_LOGI("atomic_file selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("atomic_file selftest: %d/%d checks passed", pass, total);
    return false;
}

// Once per process: the cases are ~150 file operations and several flushes, and a session
// Stop/Start would redo them; a later call returns the first run's verdict without output.
bool RunSelftest() {
    static std::once_flag ran;
    static bool verdict = false;
    std::call_once(ran, [] { verdict = RunSelftestBody(); });
    return verdict;
}

}  // namespace coop::atomic_file
