// votv-coop standalone loader -- xinput1_3.dll proxy.
//
// Shipping auto-loader (RULE No.3 -- no UE4SS at runtime). VOTV imports
// XInputGetState/XInputSetState from xinput1_3.dll, and the Windows loader
// searches the exe's own directory before System32. So a xinput1_3.dll dropped
// next to VotV-Win64-Shipping.exe is loaded in place of the system one. The
// two imported exports are satisfied by STATIC forwarders to System32's
// xinput1_4.dll (see xinput1_3.def) -- resolved by the loader, independent of
// the code below.
//
// This proxy's only RUNTIME job is to load the mod payload from its own
// directory. That is exactly what the dev-only inject.ps1 did via
// CreateRemoteThread; the proxy makes it automatic, with no injection step and
// no UE4SS. LoadLibrary is done off the loader lock (worker thread), never
// inside DllMain.
//
// v122 (2026-07-19, the Paper-style versioned artifact): the payload is
// multivoid-<game>-<build>.dll (e.g. multivoid-0.9.0n-122.dll). The proxy
// SCANS for multivoid-*.dll, loads the HIGHEST build number, and -- when more
// than one version file is present (a botched manual update) -- hands the full
// list to the payload via the MULTIVOID_DUP_FILES process env var; the mod
// raises an in-game popup telling the user which file it loaded and what to
// delete. A stray legacy votv-coop.dll counts as a duplicate too (never
// loaded). No fixed-name fallback (RULE 2: the old name is retired whole).

#include <windows.h>

#include <string>
#include <vector>

// Static export forwarders for the two symbols VOTV imports from xinput1_3.dll
// (verified by name in the shipping exe import table). Both are forwarded to
// System32's xinput1_4.dll (guaranteed present on Win8+), which exports the
// same functions with identical signatures. We forward ONLY what VOTV imports
// (RULE No.2 -- no speculative baggage); the ordinals mirror the real
// xinput1_3 so an ordinal importer would also resolve. The loader resolves
// these at bind time, independent of the LoadPayload code below.
#pragma comment(linker, "/export:XInputGetState=xinput1_4.XInputGetState,@2")
#pragma comment(linker, "/export:XInputSetState=xinput1_4.XInputSetState,@3")

namespace {

// Parse the trailing build number out of "multivoid-<game>-<build>.dll".
// Returns -1 when the name does not end in "-<digits>.dll" (never loaded --
// an unparseable name cannot be ranked, and fabricating rank 0 could load an
// older build over a newer one).
int ParseBuildNumber(const std::wstring& name) {
    const size_t ext = name.size() >= 4 ? name.size() - 4 : 0;  // ".dll"
    size_t i = ext;
    while (i > 0 && name[i - 1] >= L'0' && name[i - 1] <= L'9') --i;
    if (i == ext || i == 0 || name[i - 1] != L'-') return -1;
    int v = 0;
    for (size_t k = i; k < ext; ++k) {
        v = v * 10 + (name[k] - L'0');
        if (v > 1000000) return -1;
    }
    return v;
}

DWORD WINAPI LoadPayload(LPVOID) {
    // Resolve this proxy's own path so we load the payload sitting beside it,
    // not whatever a relative search order might find first.
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LoadPayload), &self);

    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(self, path, MAX_PATH);

    // Trim to the directory (keep the trailing separator).
    wchar_t* lastSep = nullptr;
    for (wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') lastSep = p;
    }
    if (lastSep) {
        lastSep[1] = L'\0';
    } else {
        path[0] = L'\0';
    }
    const std::wstring dir(path);

    // Scan for every payload-version file present. A stray legacy votv-coop.dll
    // is collected as a duplicate (listed in the popup, never loaded).
    std::vector<std::wstring> found;
    {
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((dir + L"multivoid-*.dll").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    found.push_back(fd.cFileName);
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
    }
    const bool legacyPresent =
        ::GetFileAttributesW((dir + L"votv-coop.dll").c_str()) != INVALID_FILE_ATTRIBUTES;

    // Pick the HIGHEST parseable build number (a manual update usually ADDS the
    // new file next to the old one -- newest wins, the popup names the rest).
    int bestBuild = -1;
    std::wstring best;
    for (const std::wstring& f : found) {
        const int b = ParseBuildNumber(f);
        if (b > bestBuild) {
            bestBuild = b;
            best = f;
        }
    }
    if (best.empty()) return 0;  // no payload beside the proxy -- nothing to load

    // More than one version file (or a legacy leftover): hand the list to the
    // payload BEFORE loading it -- the mod reads MULTIVOID_DUP_FILES at boot and
    // raises the in-game popup. Process-env is the one channel that needs no
    // file/IPC and is inherited by nothing else (we set it on our own process).
    if (found.size() > 1 || legacyPresent) {
        std::wstring list;
        for (const std::wstring& f : found) {
            if (f == best) continue;
            if (!list.empty()) list += L";";
            list += f;
        }
        if (legacyPresent) {
            if (!list.empty()) list += L";";
            list += L"votv-coop.dll";
        }
        ::SetEnvironmentVariableW(L"MULTIVOID_DUP_FILES", list.c_str());
        ::SetEnvironmentVariableW(L"MULTIVOID_LOADED", best.c_str());
    }

    // The payload bootstraps itself from its own DllMain.
    ::LoadLibraryW((dir + best).c_str());
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(module);
        if (HANDLE t = ::CreateThread(nullptr, 0, LoadPayload, nullptr, 0, nullptr)) {
            ::CloseHandle(t);
        }
    }
    return TRUE;
}
