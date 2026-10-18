// coop/net/identity_crypto.cpp -- see coop/net/identity_crypto.h.

#include "identity_crypto.h"

#include "coop/atomic_file/atomic_file.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>   // SetEntriesInAclW, GetNamedSecurityInfoW
#include <bcrypt.h>

namespace coop::net::identity_crypto {

// bcrypt is already linked (GNS's USE_CRYPTO=BCrypt backend), so this costs no new dependency and
// -- unlike GNS's own CCrypto -- needs no header from the vendored tree's internal `src/`.
bool Sha256(const void* data, size_t len, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    if (ok) ok = ::BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)),
                                  static_cast<ULONG>(len), 0) == 0;
    if (ok) ok = ::BCryptFinishHash(hash, out, 32, 0) == 0;
    if (hash) ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string ToHex(const uint8_t* p, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(kHex[p[i] >> 4]);
        s.push_back(kHex[p[i] & 0xF]);
    }
    return s;
}

bool FromHex(const std::string& hex, uint8_t* out, size_t n) {
    if (hex.size() != n * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::string BindingValue(const std::string& machineGuid) {
    if (machineGuid.empty()) return "none";
    std::string bytes = "multivoid-identity-binding-v1\n";
    for (const char c : machineGuid)
        bytes.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    uint8_t digest[32];
    if (!Sha256(bytes.data(), bytes.size(), digest)) return "none";
    return ToHex(digest, sizeof(digest));
}

// A key file under a restrictive access list is on the list of edge protections Relay, Moddy's
// VOTV mod, publishes in its README (docs/credits.md).
const PrivateAcl& KeyFileAcl() {
    static const PrivateAcl acl = [] {
        PrivateAcl a;
        HANDLE tok = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) return a;
        DWORD need = 0;
        ::GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
        std::vector<uint8_t> buf(need ? need : 1);
        const bool gotUser =
            need != 0 && ::GetTokenInformation(tok, TokenUser, buf.data(), need, &need);
        ::CloseHandle(tok);
        if (!gotUser) return a;
        PSID user = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
        const auto* userBytes = static_cast<const uint8_t*>(user);
        a.userSid.assign(userBytes, userBytes + ::GetLengthSid(user));
        uint8_t systemSid[SECURITY_MAX_SID_SIZE]{};
        DWORD cb = sizeof(systemSid);
        if (!::CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid, &cb)) return a;
        EXPLICIT_ACCESS_W ea[2] = {};
        for (auto& e : ea) {
            e.grfAccessPermissions = FILE_ALL_ACCESS;
            e.grfAccessMode = SET_ACCESS;
            e.grfInheritance = NO_INHERITANCE;
            e.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        }
        ea[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(a.userSid.data());
        ea[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(systemSid);
        if (::SetEntriesInAclW(2, ea, nullptr, &a.dacl) != ERROR_SUCCESS) {
            a.dacl = nullptr;
            return a;
        }
        a.ok = true;
        return a;
    }();
    return acl;
}

bool ReadKeyDacl(const std::wstring& path, bool& isProtected, unsigned long& aceCount) {
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
        isProtected = (ctrl & SE_DACL_PROTECTED) != 0;
        aceCount = info.AceCount;
    }
    ::LocalFree(psd);
    return ok;
}

void SweepDeadScratchKeys(const std::wstring& exeDir) {
    const std::wstring prefix = kScratchKeyPrefix;
    const std::wstring suffix = L".key";
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((exeDir + L"\\" + prefix + L"*" + suffix).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring name = fd.cFileName;
            if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            const std::wstring digits =
                name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
            if (digits.size() > 10) continue;
            unsigned long long id = 0;
            bool numeric = true;
            for (wchar_t c : digits) {
                if (c < L'0' || c > L'9') { numeric = false; break; }
                id = id * 10 + static_cast<unsigned>(c - L'0');
            }
            if (!numeric || id > 0xFFFFFFFFull) continue;
            if (coop::atomic_file::ProcessRunning(static_cast<unsigned long>(id))) continue;
            ::DeleteFileW((exeDir + L"\\" + name).c_str());
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    coop::atomic_file::RemoveLeftovers(exeDir, std::wstring(prefix) + L"*" + suffix);
}

}  // namespace coop::net::identity_crypto
