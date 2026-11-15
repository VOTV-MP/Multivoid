// coop/interactables/tv_link_resolver.cpp -- see coop/interactables/tv_link_resolver.h.

#include "coop/interactables/tv_link_resolver.h"

#include "coop/net/protocol.h"
#include "coop/session/shutdown.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <windows.h>

#include <atomic>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace coop::tv_link_resolver {
namespace {

constexpr DWORD kToolBudgetMs = 45000;
constexpr int   kMaxAtOnce = 2;
constexpr size_t kMaxLinkChars = 2000;
std::atomic<int> g_running{0};

// What the tool answered lately. A stream's address stays good for hours; twenty minutes spares a set's
// replay, a joiner's and a second set's the several seconds a run takes.
constexpr ULONGLONG kCacheMs = 20 * 60 * 1000;
constexpr size_t kMaxCached = 16;
struct Cached {
    std::wstring link;
    std::wstring direct;
    ULONGLONG atMs;
};
std::mutex g_cacheMutex;
std::vector<Cached> g_cache;

bool FromCache(const std::wstring& link, std::wstring& direct) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    const ULONGLONG now = ::GetTickCount64();
    for (size_t i = g_cache.size(); i-- > 0;) {
        if (now - g_cache[i].atMs > kCacheMs) g_cache.erase(g_cache.begin() + static_cast<std::ptrdiff_t>(i));
        else if (g_cache[i].link == link) { direct = g_cache[i].direct; return true; }
    }
    return false;
}

void ToCache(const std::wstring& link, const std::wstring& direct) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    if (g_cache.size() >= kMaxCached) g_cache.erase(g_cache.begin());
    g_cache.push_back({link, direct, ::GetTickCount64()});
}

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    size_t i = 0;
    for (; prefix[i]; ++i)
        if (i >= s.size() || std::towlower(s[i]) != prefix[i]) return false;
    return true;
}

// The host of an http(s) address, lowercased, without a port.
std::wstring HostOf(const std::wstring& link) {
    const size_t start = link.find(L"://");
    if (start == std::wstring::npos) return {};
    size_t end = link.find_first_of(L"/?#", start + 3);
    if (end == std::wstring::npos) end = link.size();
    std::wstring host = link.substr(start + 3, end - (start + 3));
    const size_t at = host.rfind(L'@');
    if (at != std::wstring::npos) host.erase(0, at + 1);
    const size_t colon = host.find(L':');
    if (colon != std::wstring::npos) host.erase(colon);
    for (auto& c : host) c = static_cast<wchar_t>(std::towlower(c));
    return host;
}

bool HostIs(const std::wstring& host, const wchar_t* domain) {
    const std::wstring d(domain);
    if (host == d) return true;
    return host.size() > d.size() + 1 && host.compare(host.size() - d.size(), d.size(), d) == 0 &&
           host[host.size() - d.size() - 1] == L'.';
}

// tools\yt-dlp.exe in the mod's own folder. Under a mod manager the path a module reports for itself is one the
// loader maps for file reads only: a program cannot be started from it. The data folder is the real one
// (ue_wrap/core/paths), so the tool is looked for there first, then beside the folder this module was loaded from.
std::wstring ToolPath() {
    static const std::wstring path = [] {
        const std::wstring data = ue_wrap::paths::ModDataDir();
        if (!data.empty()) {
            const std::wstring real = data + L"\\tools\\yt-dlp.exe";
            if (::GetFileAttributesW(real.c_str()) != INVALID_FILE_ATTRIBUTES) return real;
        }
        HMODULE self = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(&ToolPresent), &self))
            return std::wstring();
        wchar_t buf[1024] = {};
        const DWORD n = ::GetModuleFileNameW(self, buf, 1024);
        if (n == 0 || n >= 1024) return std::wstring();
        std::wstring dir(buf, n);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return std::wstring();
        dir.erase(slash);                       // ...\dlls
        const size_t up = dir.find_last_of(L"\\/");
        if (up == std::wstring::npos) return std::wstring();
        return dir.substr(0, up) + L"\\tools\\yt-dlp.exe";
    }();
    return path;
}

// Run the tool and return what it printed on either stream, at most 4 KB. Empty when it could not be started or
// ran out of time. One line of the log says how the run ended, so a link that does not play has its reason there.
std::string RunTool(const std::wstring& link) {
    const std::wstring tool = ToolPath();
    const ULONGLONG startedAt = ::GetTickCount64();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!::CreatePipe(&readEnd, &writeEnd, &sa, 0)) return {};
    ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    // One stream with picture and sound, an mp4 first. The address follows `--`, so nothing in it is
    // read as an option, and ValidLink has kept quotes and spaces out of it.
    std::wstring cmd = L"\"" + tool +
                       L"\" --no-playlist --no-warnings --quiet --socket-timeout 15 "
                       L"-f \"18/b[ext=mp4][acodec!=none][vcodec!=none]/b[acodec!=none][vcodec!=none]\" -g -- " +
                       link;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    // The tool's errors come down the same pipe as its answer, and its input is the null device: a game has no
    // console to lend it.
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    const BOOL started = ::CreateProcessW(tool.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                          nullptr, &si, &pi);
    const DWORD startError = started ? 0 : ::GetLastError();
    ::CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE) ::CloseHandle(nul);
    if (!started) {
        ::CloseHandle(readEnd);
        UE_LOGW("tv_link: yt-dlp could not be started (Windows error %lu) from %ls", startError, tool.c_str());
        return {};
    }
    std::string out;
    const ULONGLONG deadline = ::GetTickCount64() + kToolBudgetMs;
    for (;;) {
        DWORD avail = 0;
        if (::PeekNamedPipe(readEnd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[1024];
            DWORD got = 0;
            if (::ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0 && out.size() < 4096) out.append(buf, got);
            continue;
        }
        if (::WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            // The last bytes written before the exit.
            while (::PeekNamedPipe(readEnd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                char buf[1024];
                DWORD got = 0;
                if (!::ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) || got == 0) break;
                if (out.size() < 4096) out.append(buf, got);
            }
            break;
        }
        if (::GetTickCount64() > deadline || coop::shutdown::IsShuttingDown()) {
            ::TerminateProcess(pi.hProcess, 1);
            UE_LOGW("tv_link: yt-dlp was stopped after %llu ms without finishing",
                    static_cast<unsigned long long>(::GetTickCount64() - startedAt));
            out.clear();
            break;
        }
    }
    DWORD exitCode = 0;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    // What it said, cut to one short line: its error text when it gave no address.
    // A stream's address is signed for this machine and is not written out.
    std::string said = out.compare(0, 4, "http") == 0 ? std::string("(a stream address)") : out.substr(0, 240);
    for (char& c : said)
        if (c == '\r' || c == '\n') c = ' ';
    UE_LOGI("tv_link: yt-dlp ended with code %lu after %llu ms and %zu bytes: %s", exitCode,
            static_cast<unsigned long long>(::GetTickCount64() - startedAt), out.size(), said.c_str());
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(readEnd);
    return out;
}

}  // namespace

bool ValidLink(const std::wstring& link) {
    if (link.size() < 12 || link.size() > kMaxLinkChars) return false;
    if (!StartsWithI(link, L"https://") && !StartsWithI(link, L"http://")) return false;
    for (wchar_t c : link)
        if (c <= 0x20 || c >= 0x7F || c == L'"' || c == L'\'' || c == L'\\' || c == L'`' || c == L'<' || c == L'>' ||
            c == L'|' || c == L'^')
            return false;
    return !HostOf(link).empty();
}

bool Sendable(const std::wstring& link) {
    return link.size() <= static_cast<size_t>(coop::net::kTvLinkMax) && ValidLink(link);
}

bool NeedsResolve(const std::wstring& link) {
    const std::wstring host = HostOf(link);
    return HostIs(host, L"youtube.com") || HostIs(host, L"youtu.be") || HostIs(host, L"youtube-nocookie.com");
}

bool ToolPresent() {
    const std::wstring tool = ToolPath();
    return !tool.empty() && ::GetFileAttributesW(tool.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void Resolve(const std::wstring& link, std::function<void(std::wstring direct)> done) {
    auto finish = [done = std::move(done)](std::wstring direct) {
        ue_wrap::game_thread::Post([done, direct = std::move(direct)] { done(direct); });
    };
    if (!ValidLink(link) || !ToolPresent()) {
        if (!ToolPresent()) UE_LOGW("tv_link: tools\\yt-dlp.exe is not beside the mod -- a YouTube link cannot be resolved");
        finish({});
        return;
    }
    std::wstring cached;
    if (FromCache(link, cached)) {
        finish(std::move(cached));
        return;
    }
    if (g_running.fetch_add(1) >= kMaxAtOnce) {
        g_running.fetch_sub(1);
        UE_LOGW("tv_link: %d links are being resolved already -- this one is dropped", kMaxAtOnce);
        finish({});
        return;
    }
    std::thread([link, finish = std::move(finish)] {
        const std::string out = RunTool(link);
        g_running.fetch_sub(1);
        // The first line that is an address.
        std::wstring direct;
        size_t at = 0;
        while (at < out.size()) {
            size_t end = out.find_first_of("\r\n", at);
            if (end == std::string::npos) end = out.size();
            if (end - at > 8 && out.compare(at, 4, "http") == 0) {
                direct.assign(out.begin() + static_cast<std::ptrdiff_t>(at), out.begin() + static_cast<std::ptrdiff_t>(end));
                break;
            }
            at = end + 1;
        }
        if (!direct.empty()) ToCache(link, direct);
        finish(std::move(direct));
    }).detach();
}

}  // namespace coop::tv_link_resolver
