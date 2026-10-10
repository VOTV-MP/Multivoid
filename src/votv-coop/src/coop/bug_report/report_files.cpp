// coop/bug_report/report_files.cpp -- the files a bundle holds and how one is read: ListEntries,
// ReadThisMachine and the chunked line reader. See coop/bug_report/report_bundle.h.

#include "coop/bug_report/report_bundle.h"
#include "report_stream.h"

#include "l10n/mark.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <share.h>

namespace coop::bug_report {
namespace {

constexpr size_t kNone = std::string_view::npos;
static_assert(kUe4ssTailBytes < kChunkBytes, "the tail's window is read in one chunk");

Entry MakeEntry(const char* name, std::wstring path, bool tailOnly) {
    Entry e{name, std::move(path), tailOnly, std::string(), 0};
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (e.path.empty() || !::GetFileAttributesExW(e.path.c_str(), GetFileExInfoStandard, &d) ||
        (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        e.leftOut = L10N_MARK("not present");
        return e;
    }
    e.bytesOnDisk = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return e;
}

// Whether line 2 of a log is this build's format line.
enum class LogFormat { Same, Other, Unreadable };

LogFormat CheckLogFormat(const std::wstring& path) {
    FILE* f = ::_wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return LogFormat::Unreadable;
    char buf[256];
    const size_t n = std::fread(buf, 1, sizeof(buf), f);
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) return LogFormat::Unreadable;
    std::string_view v(buf, n);
    const size_t first = v.find('\n');
    if (first == kNone) return LogFormat::Other;
    v.remove_prefix(first + 1);
    const size_t second = v.find('\n');
    if (second == kNone) return LogFormat::Other;
    return IsReadableLogFormat(v.substr(0, second)) ? LogFormat::Same : LogFormat::Other;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    if (p) ::CoTaskMemFree(p);
    return out;
}

void StripTrailingSlash(std::wstring& w) {
    while (w.size() > 3 && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
}

// `w` in a code page, false when the call fails or the page cannot hold it (a lossy form would
// carry a '?' no log line holds).
bool Narrow(UINT codePage, const std::wstring& w, std::string& out) {
    out.clear();
    if (w.empty()) return false;
    const bool utf8 = codePage == CP_UTF8;
    BOOL usedDefault = FALSE;
    const int len = static_cast<int>(w.size());
    const int n = ::WideCharToMultiByte(codePage, 0, w.data(), len, nullptr, 0, nullptr,
                                        utf8 ? nullptr : &usedDefault);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    if (::WideCharToMultiByte(codePage, 0, w.data(), len, out.data(), n, nullptr,
                              utf8 ? nullptr : &usedDefault) != n || usedDefault) {
        out.clear();
        return false;
    }
    return true;
}

void AddUnique(std::vector<std::string>& forms, const std::string& s) {
    if (!s.empty() && std::find(forms.begin(), forms.end(), s) == forms.end()) forms.push_back(s);
}

// A form, and the same with every `\` doubled (the spelling a JSON or Lua string writes).
void AddWithDoubled(std::vector<std::string>& forms, const std::string& s) {
    AddUnique(forms, s);
    std::string doubled;
    for (const char c : s) {
        doubled += c;
        if (c == '\\') doubled += '\\';
    }
    AddUnique(forms, doubled);
}

// The forms of one folder: UTF-8 first, then the ANSI code page (a third-party Lua mod writes
// paths in it), then the 8.3 short name when it differs.
void AddFolderForms(std::vector<std::string>& forms, const std::wstring& folder) {
    if (folder.empty()) return;
    std::string s;
    if (Narrow(CP_UTF8, folder, s)) AddWithDoubled(forms, s);
    if (Narrow(CP_ACP, folder, s)) AddWithDoubled(forms, s);
    wchar_t shortPath[MAX_PATH] = {};
    const DWORD n = ::GetShortPathNameW(folder.c_str(), shortPath, MAX_PATH);
    if (n != 0 && n < MAX_PATH) {
        const std::wstring shortForm(shortPath, n);
        if (shortForm != folder && Narrow(CP_UTF8, shortForm, s)) AddWithDoubled(forms, s);
    }
}

// Two folder spellings that name one place: equal after case-folding with `/` made `\`.
std::wstring Folded(std::wstring w) {
    for (wchar_t& c : w) {
        if (c == L'/') c = L'\\';
    }
    if (!w.empty()) ::CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
    return w;
}

}  // namespace

std::vector<Entry> ListEntries() {
    std::vector<Entry> out;
    const std::wstring exeDir = ue_wrap::paths::ExeDir();
    const std::wstring ue4ssDir = ue_wrap::paths::Ue4ssDir();

    out.push_back(MakeEntry("multivoid.log", ue_wrap::log::CurrentPath(), false));

    Entry prev = MakeEntry("multivoid.prev.log", ue_wrap::log::PreviousPath(), false);
    if (prev.leftOut.empty()) {
        switch (CheckLogFormat(prev.path)) {
            case LogFormat::Same: break;
            case LogFormat::Other: prev.leftOut = L10N_MARK("written by a build with another log format"); break;
            case LogFormat::Unreadable: prev.leftOut = L10N_MARK("could not be read"); break;
        }
    }
    out.push_back(std::move(prev));

    out.push_back(MakeEntry("multivoid-compat-report.txt",
                            exeDir.empty() ? std::wstring() : exeDir + L"\\multivoid-compat-report.txt",
                            false));
    out.push_back(MakeEntry("UE4SS.log", ue4ssDir.empty() ? std::wstring() : ue4ssDir + L"\\UE4SS.log",
                            true));
    return out;
}

RedactContext ReadThisMachine(std::string selfPlayerId, std::string selfKey) {
    RedactContext ctx;
    ctx.selfPlayerId = std::move(selfPlayerId);
    ctx.selfKey = std::move(selfKey);

    std::wstring localAppData = KnownFolder(FOLDERID_LocalAppData);
    std::wstring profile = KnownFolder(FOLDERID_Profile);
    StripTrailingSlash(localAppData);
    StripTrailingSlash(profile);

    // The environment value our save code builds its paths from, a second spelling of the local
    // app data when it names another place.
    std::wstring fromEnv;
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n != 0 && n < MAX_PATH) fromEnv.assign(buf, n);
    StripTrailingSlash(fromEnv);

    AddFolderForms(ctx.localAppDataForms, localAppData);
    if (!fromEnv.empty() && Folded(fromEnv) != Folded(localAppData))
        AddFolderForms(ctx.localAppDataForms, fromEnv);
    AddFolderForms(ctx.profileForms, profile);
    return ctx;
}

namespace detail {

bool StreamLines(const std::wstring& path, const ReadPlan& plan, bool replay, Span& span,
                 const LineFn& fn) {
    FILE* f = ::_wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return false;
    struct Closer {
        FILE* file;
        ~Closer() { std::fclose(file); }
    } closer{f};
    std::vector<char> chunk(kChunkBytes);

    uint64_t limit = UINT64_MAX;
    if (replay) {
        if (::_fseeki64(f, static_cast<__int64>(span.start), SEEK_SET) != 0) return false;
        limit = span.bytes;
    } else if (plan.tailOnly) {
        if (::_fseeki64(f, 0, SEEK_END) != 0) return false;
        const __int64 size = ::_ftelli64(f);
        if (size < 0) return false;
        const uint64_t total = static_cast<uint64_t>(size);
        uint64_t start = total > kUe4ssTailBytes ? total - kUe4ssTailBytes : 0;
        if (start > 0) {
            // From the first '\n' inside the window: the window's first line may be cut.
            if (::_fseeki64(f, static_cast<__int64>(start), SEEK_SET) != 0) return false;
            const size_t got = std::fread(chunk.data(), 1, static_cast<size_t>(total - start), f);
            if (std::ferror(f) != 0) return false;
            const std::string_view window(chunk.data(), got);
            const size_t nl = window.find('\n');
            start = nl == kNone ? total : start + nl + 1;
        }
        span.start = start;
        limit = total - start;
        if (::_fseeki64(f, static_cast<__int64>(start), SEEK_SET) != 0) return false;
    } else {
        span.start = 0;
        if (::_fseeki64(f, 0, SEEK_SET) != 0) return false;
    }

    std::string carry;
    uint64_t readTotal = 0;
    uint64_t remaining = limit;
    while (remaining > 0) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, kChunkBytes));
        const size_t got = std::fread(chunk.data(), 1, want, f);
        if (got == 0) {
            if (std::ferror(f) != 0) return false;
            break;
        }
        remaining -= got;
        readTotal += got;
        std::string_view data(chunk.data(), got);
        while (!data.empty()) {
            const size_t nl = data.find('\n');
            if (nl == kNone) {
                carry.append(data);
                break;
            }
            if (carry.empty()) {
                fn(data.substr(0, nl));
            } else {
                carry.append(data.substr(0, nl));
                fn(carry);
                carry.clear();
            }
            data.remove_prefix(nl + 1);
        }
    }
    if (!carry.empty()) {
        if (plan.completeLinesOnly) readTotal -= carry.size();
        else fn(carry);
    }
    if (!replay) span.bytes = readTotal;
    return true;
}

}  // namespace detail
}  // namespace coop::bug_report
