// coop/net/http_client.cpp -- see coop/net/http_client.h.

#include "coop/net/http_client.h"

#include "ue_wrap/core/log.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <string>

#pragma comment(lib, "winhttp.lib")

namespace coop::net::http {
namespace {

// RAII for the three WinHTTP handle kinds (all close via WinHttpCloseHandle).
struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET hh) : h(hh) {}
    ~WinHttpHandle() { if (h) ::WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// Split "host:port" into host + numeric port, and decide TLS.
//
// URL grammar (Tier B): SCHEMELESS = SECURE. A bare "host:port" is TLS, so the
// compiled-in official endpoint gets TLS without carrying a scheme (which also
// keeps the exact-string "DEFAULT" display mask intact). "https://" is explicit
// TLS; "http://" is an explicit cleartext DOWNGRADE, which exists only for a
// self-hoster running their own master without a certificate -- it is never used
// against the official endpoint.
//
// Returns false if the port is missing/out of range (the master URL must carry an
// explicit port -- it lives on a spare port, never the default :80/:443).
bool SplitHostPort(const std::string& in, std::string& host, uint16_t& port, bool& secure) {
    std::string s = in;
    secure = true;  // schemeless default
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) {
        std::string proto = s.substr(0, scheme);
        for (char& c : proto) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (proto == "http") secure = false;
        else if (proto != "https") {
            UE_LOGW("http: unknown scheme '%s://' -- treating as secure", proto.c_str());
        }
        s = s.substr(scheme + 3);
    }
    // strip any trailing path the caller accidentally included
    const size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) return false;
    host = s.substr(0, colon);
    const unsigned long raw = std::strtoul(s.c_str() + colon + 1, nullptr, 10);
    if (raw == 0 || raw > 65535) return false;
    port = static_cast<uint16_t>(raw);
    return true;
}

}  // namespace

Response Request(const std::string& hostPort, const std::string& path,
                 const char* method, const std::string& body, int timeoutMs) {
    Response out;

    std::string host;
    uint16_t port = 0;
    bool secure = true;
    if (!SplitHostPort(hostPort, host, port, secure)) {
        UE_LOGW("http: bad host:port '%s'", hostPort.c_str());
        return out;
    }

    WinHttpHandle session(::WinHttpOpen(L"multivoid/0.9.0n",
                                        WINHTTP_ACCESS_TYPE_NO_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return out;

    if (timeoutMs > 0) {
        ::WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    }

    const std::wstring whost = Widen(host);
    WinHttpHandle conn(::WinHttpConnect(session.h, whost.c_str(), port, 0));
    if (!conn) return out;

    const std::wstring wpath = Widen(path.empty() ? "/" : path);
    const std::wstring wmethod = Widen(method);
    // WINHTTP_FLAG_SECURE turns on TLS with WinHTTP's DEFAULT validation: the
    // server chain is verified against the Windows root store and the hostname is
    // checked. There is deliberately NO certificate pinning and no
    // SECURITY_FLAG_IGNORE_* relaxation anywhere -- a Let's Encrypt rotation is
    // then a non-event, and a MITM or a wrong-name endpoint fails the request
    // rather than silently succeeding.
    WinHttpHandle req(::WinHttpOpenRequest(conn.h, wmethod.c_str(), wpath.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req) return out;

    LPCWSTR headers = WINHTTP_NO_ADDITIONAL_HEADERS;
    DWORD headersLen = 0;
    static const wchar_t kJsonHdr[] = L"Content-Type: application/json\r\n";
    if (body.size()) { headers = kJsonHdr; headersLen = static_cast<DWORD>(-1L); }

    if (!::WinHttpSendRequest(req.h, headers, headersLen,
                              body.size() ? const_cast<char*>(body.data()) : WINHTTP_NO_REQUEST_DATA,
                              static_cast<DWORD>(body.size()),
                              static_cast<DWORD>(body.size()), 0)) {
        // Name a TLS failure explicitly. Without this a certificate problem is
        // indistinguishable from "server down" in the log, and the operator
        // chases the wrong thing (the endpoint answers fine over plaintext).
        const DWORD err = ::GetLastError();
        if (err == ERROR_WINHTTP_SECURE_FAILURE) {
            UE_LOGW("http: TLS validation FAILED for %s (untrusted/expired cert, or the "
                    "name does not match -- an IP address never matches a certificate)",
                    host.c_str());
        } else if (secure && err == ERROR_WINHTTP_CANNOT_CONNECT) {
            UE_LOGW("http: cannot connect to %s:%u -- is the TLS port right?",
                    host.c_str(), static_cast<unsigned>(port));
        }
        return out;
    }
    if (!::WinHttpReceiveResponse(req.h, nullptr)) return out;

    // Status code.
    DWORD status = 0, statusLen = sizeof(status);
    if (!::WinHttpQueryHeaders(req.h,
                               WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                               WINHTTP_NO_HEADER_INDEX)) {
        return out;
    }
    out.status = static_cast<int>(status);

    // Body (bounded read loop). A master response is small; cap at 1 MiB so a rogue
    // endpoint can't grow our buffer without bound.
    constexpr size_t kBodyCap = 1u << 20;
    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(req.h, &avail)) return out;  // partial -> !ok
        if (avail == 0) break;
        size_t chunk = avail;
        if (out.body.size() + chunk > kBodyCap) chunk = kBodyCap - out.body.size();
        if (chunk == 0) { UE_LOGW("http: response body exceeded 1 MiB cap"); break; }
        const size_t base = out.body.size();
        out.body.resize(base + chunk);
        DWORD read = 0;
        if (!::WinHttpReadData(req.h, &out.body[base], static_cast<DWORD>(chunk), &read)) {
            return out;
        }
        out.body.resize(base + read);
        if (read == 0) break;
    }

    out.ok = true;
    return out;
}

}  // namespace coop::net::http
