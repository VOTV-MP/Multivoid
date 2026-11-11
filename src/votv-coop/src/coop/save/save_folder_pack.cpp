// coop/save/save_folder_pack.cpp -- see coop/save/save_folder_pack.h.

#include "coop/save/save_folder_pack.h"

#include "coop/atomic_file/atomic_file.h"
#include "coop/save/slot_file_read.h"
#include "ue_wrap/core/log.h"

#include <cstring>
#include <string>
#include <string_view>

namespace coop::save_folder_pack {

namespace {

constexpr char kMagic[4] = {'M', 'V', 'S', 'F'};
constexpr const char* kFiles[] = {"main.sav", "persistent.sav", "main.meta"};
constexpr size_t kFileCount = sizeof(kFiles) / sizeof(kFiles[0]);
constexpr std::string_view kAreaDir = "areas/";
constexpr std::string_view kAreaExt = ".sav";

void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

// The index of `name` among the three fixed files, or -1.
int IndexOf(std::string_view name) {
    for (size_t i = 0; i < kFileCount; ++i)
        if (name == kFiles[i]) return static_cast<int>(i);
    return -1;
}

// Can `area` be one file's name: printable ASCII with no path separator or drive colon, not a dot name,
// short enough that the packed name fits its length byte.
bool IsFileName(std::string_view area) {
    if (area.empty() || area.size() > 64 || area == "." || area == "..") return false;
    for (const char c : area)
        if (c < 0x20 || c > 0x7E || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            return false;
    return true;
}

// The area named by a packed "areas/<area>.sav", or empty when `name` is not that shape.
std::string_view AreaOfPackedName(std::string_view name) {
    if (name.size() <= kAreaDir.size() + kAreaExt.size() || name.substr(0, kAreaDir.size()) != kAreaDir ||
        name.substr(name.size() - kAreaExt.size()) != kAreaExt)
        return {};
    const std::string_view area = name.substr(kAreaDir.size(), name.size() - kAreaDir.size() - kAreaExt.size());
    return IsFileName(area) ? area : std::string_view{};
}

void PutFile(std::vector<uint8_t>& out, const std::string& name, const std::vector<uint8_t>& bytes) {
    out.push_back(static_cast<uint8_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    PutU32(out, static_cast<uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

}  // namespace

bool Pack(const std::filesystem::path& dir, const std::wstring& area, std::vector<uint8_t>& out) {
    out.clear();
    std::string areaName;
    for (const wchar_t c : area) areaName.push_back(c < 0x80 ? static_cast<char>(c) : '\x7F');
    if (!area.empty() && !IsFileName(areaName)) {
        UE_LOGE("save_folder_pack: the area '%ls' is not a name one file can have", area.c_str());
        return false;
    }
    out.insert(out.end(), kMagic, kMagic + 4);
    PutU32(out, static_cast<uint32_t>(kFileCount + (area.empty() ? 0 : 1)));
    for (const char* name : kFiles) {
        std::vector<uint8_t> bytes;
        if (!coop::slot_file_read::ReadWholeFile(dir / name, bytes)) {
            UE_LOGW("save_folder_pack: '%ls' has no readable %s", dir.c_str(), name);
            out.clear();
            return false;
        }
        PutFile(out, name, bytes);
    }
    if (!area.empty()) {
        std::vector<uint8_t> bytes;
        const std::filesystem::path file = dir / L"areas" / (area + L".sav");
        if (!coop::slot_file_read::ReadWholeFile(file, bytes)) {
            UE_LOGW("save_folder_pack: '%ls' is unreadable", file.c_str());
            out.clear();
            return false;
        }
        PutFile(out, std::string(kAreaDir) + areaName + std::string(kAreaExt), bytes);
    }
    return true;
}

bool Unpack(const uint8_t* data, size_t len, const std::filesystem::path& dir) {
    if (!data || len < 8 || std::memcmp(data, kMagic, 4) != 0) {
        UE_LOGE("save_folder_pack: not a packed save folder (%zu bytes)", len);
        return false;
    }
    uint32_t count = 0;
    std::memcpy(&count, data + 4, 4);
    if (count != kFileCount && count != kFileCount + 1) {
        UE_LOGE("save_folder_pack: %u files packed, %zu or %zu expected", count, kFileCount, kFileCount + 1);
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const uint8_t* p = data + 8;
    const uint8_t* const end = data + len;
    bool seen[kFileCount] = {};
    bool seenArea = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (end - p < 1) return false;
        const size_t nameLen = *p++;
        if (static_cast<size_t>(end - p) < nameLen + 4) return false;
        const std::string name(reinterpret_cast<const char*>(p), nameLen);
        p += nameLen;
        uint32_t size = 0;
        std::memcpy(&size, p, 4);
        p += 4;
        if (static_cast<size_t>(end - p) < size) return false;
        const int idx = IndexOf(name);
        const bool isArea = idx < 0 && !AreaOfPackedName(name).empty();
        if ((idx < 0 && !isArea) || (isArea ? seenArea : seen[idx])) {
            UE_LOGE("save_folder_pack: refusing a packed file named '%s'", name.c_str());
            return false;
        }
        (isArea ? seenArea : seen[idx]) = true;
        const std::filesystem::path file = dir / name;   // "areas/<area>.sav" lands in the areas folder
        if (isArea) std::filesystem::create_directories(file.parent_path(), ec);
        const atomic_file::Result wr =
            atomic_file::Write(file, std::string_view(reinterpret_cast<const char*>(p), size),
                               atomic_file::Mode::Replace, atomic_file::Sync::Cached);
        if (!wr.ok()) {
            UE_LOGE("save_folder_pack: writing '%ls' failed (%s)", file.c_str(),
                    atomic_file::Describe(wr).c_str());
            return false;
        }
        p += size;
    }
    // The count matched and no name repeated, so a count of three or four with all three fixed names
    // present has written everything: the fourth, if any, was the area.
    for (const bool s : seen)
        if (!s) return false;
    return p == end;
}

}  // namespace coop::save_folder_pack
