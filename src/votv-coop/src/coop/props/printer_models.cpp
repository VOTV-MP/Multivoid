// coop/props/printer_models.cpp -- see coop/props/printer_models.h.

#include "coop/props/printer_models.h"

#include "coop/config/config.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>
#include <vector>

namespace coop::printer_models {
namespace {

namespace R  = ue_wrap::reflection;
namespace fs = std::filesystem;

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull, kFnvPrime = 0x100000001b3ull;
void Mix(uint64_t& h, const void* data, size_t n) {
    const auto* b = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * kFnvPrime;
}

// lib_C's GetAssetFolder: the gamemode's `assetFolder`, or the player's local app data.
fs::path AssetFolder() {
    void* const gm = R::FindObjectByClass(L"mainGamemode_C");
    const int32_t off = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"assetFolder") : -1;
    std::wstring set = off >= 0 ? ue_wrap::field_io::ReadFStringAt(gm, off) : std::wstring();
    if (!set.empty()) return fs::path(set);
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return fs::path(buf) / L"VotV" / L"Assets";
}

struct FileRow {
    std::wstring rel;  // lower case, '/' separated
    fs::path path;
    uint64_t size;
    int64_t time;
};

struct Cached {
    uint64_t signature = 0;
    uint64_t hash = 0;
};
std::map<std::wstring, Cached> g_cache;

uint64_t ModelHashOnDisc(const std::wstring& name);

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

}  // namespace

bool IsPlainName(const std::wstring& name) {
    if (name.empty() || name == L"." || name.find(L"..") != std::wstring::npos) return false;
    for (wchar_t c : name)
        if (c < 0x20 || c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' ||
            c == L'>' || c == L'|')
            return false;
    const std::string u = ToUtf8(name);
    return !u.empty() && u.size() <= 95;
}

uint64_t ModelHash(const std::wstring& name) {
    if (!IsPlainName(name)) return 0;
    // The rig's seam: its game copies share one asset folder, so a peer is told to read as lacking every model
    // (VOTVCOOP_PRINTER_MODEL_FAKE=missing) or as holding another copy of each (=differs).
    static const std::string fake = coop::config::ReadEnv("VOTVCOOP_PRINTER_MODEL_FAKE");
    if (fake == "missing") return 0;
    if (fake == "differs") return ModelHashOnDisc(name) ^ 0x5a5a5a5a5a5a5a5aull;
    return ModelHashOnDisc(name);
}

namespace {
uint64_t ModelHashOnDisc(const std::wstring& name) {
    if (name.empty()) return 0;
    const fs::path root = AssetFolder();
    if (root.empty()) return 0;
    const fs::path dir = root / L"meshes" / L"printer" / name;
    std::error_code ec;
    if (!fs::is_regular_file(dir / (name + L".obj"), ec)) return 0;
    std::vector<FileRow> files;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        FileRow f;
        f.path = it->path();
        std::wstring rel = fs::relative(f.path, dir, ec).generic_wstring();
        if (ec) continue;
        f.rel = Lower(rel);
        f.size = static_cast<uint64_t>(it->file_size(ec));
        f.time = static_cast<int64_t>(it->last_write_time(ec).time_since_epoch().count());
        files.push_back(std::move(f));
    }
    if (ec) return 0;
    std::sort(files.begin(), files.end(), [](const FileRow& a, const FileRow& b) { return a.rel < b.rel; });
    uint64_t sig = kFnvBasis;
    for (const FileRow& f : files) {
        Mix(sig, f.rel.data(), f.rel.size() * sizeof(wchar_t));
        Mix(sig, &f.size, sizeof(f.size));
        Mix(sig, &f.time, sizeof(f.time));
    }
    Cached& c = g_cache[Lower(name)];
    if (c.hash && c.signature == sig) return c.hash;
    // The bytes: each file's path inside the folder (not its time, which a copy changes), its size and its contents.
    uint64_t h = kFnvBasis;
    std::vector<char> buf(1 << 16);
    for (const FileRow& f : files) {
        const std::string rel = ToUtf8(f.rel);
        Mix(h, rel.data(), rel.size());
        Mix(h, &f.size, sizeof(f.size));
        std::ifstream in(f.path, std::ios::binary);
        if (!in) return 0;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            Mix(h, buf.data(), static_cast<size_t>(in.gcount()));
        }
    }
    if (!h) h = 1;
    c.signature = sig;
    c.hash = h;
    UE_LOGI("printer_models: '%ls' is %zu file(s), hash %016llx", name.c_str(), files.size(),
            static_cast<unsigned long long>(h));
    return h;
}
}  // namespace

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const char* s, size_t len) {
    if (!s || !len) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(len), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(len), out.data(), n);
    return out;
}

}  // namespace coop::printer_models
