// ue_wrap/gvas_meta.cpp -- see ue_wrap/gvas_meta.h.

#include "ue_wrap/engine/gvas_meta.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ue_wrap::gvas_meta {
namespace {

// A bounds-checked little-endian reader over the open file. Every primitive
// read fails closed (ok=false) instead of throwing/misreading -- one malformed
// tag must never turn into an out-of-alignment walk over 20 MB.
struct Reader {
    std::ifstream f;
    bool ok = true;

    explicit Reader(const std::wstring& path)
        : f(std::filesystem::path(path), std::ios::binary) {
        if (!f.is_open()) ok = false;
    }

    bool Bytes(void* out, size_t n) {
        if (!ok) return false;
        f.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
        if (f.gcount() != static_cast<std::streamsize>(n)) ok = false;
        return ok;
    }
    bool Skip(int64_t n) {
        if (!ok || n < 0) { ok = false; return false; }
        f.seekg(n, std::ios::cur);
        if (!f.good()) ok = false;
        return ok;
    }
    template <class T>
    T Read() {
        T v{};
        Bytes(&v, sizeof(v));
        return v;
    }
    // UE FString on disk: int32 len. len>0 = len ANSI bytes (null included);
    // len<0 = -len UTF-16 code units (null included). Top-level tag strings are
    // short; the 4096 cap is a misalignment tripwire, not a real limit.
    std::wstring FStr() {
        const int32_t len = Read<int32_t>();
        if (!ok) return {};
        if (len == 0) return {};
        if (len > 4096 || len < -4096) { ok = false; return {}; }
        std::wstring s;
        if (len > 0) {
            std::vector<char> buf(static_cast<size_t>(len));
            if (!Bytes(buf.data(), buf.size())) return {};
            s.assign(buf.begin(), buf.end() - 1);  // drop the null
        } else {
            std::vector<uint16_t> buf(static_cast<size_t>(-len));
            if (!Bytes(buf.data(), buf.size() * 2)) return {};
            s.reserve(buf.size() - 1);
            for (size_t i = 0; i + 1 < buf.size(); ++i)
                s.push_back(static_cast<wchar_t>(buf[i]));
        }
        return s;
    }
};

}  // namespace

bool ReadSlotMeta(const std::wstring& savPath, GvasSlotMeta& out) {
    out = GvasSlotMeta{};
    Reader r(savPath);
    if (!r.ok) return false;

    // ---- GVAS header ----------------------------------------------------
    char magic[4] = {};
    if (!r.Bytes(magic, 4) || std::memcmp(magic, "GVAS", 4) != 0) return false;
    r.Read<int32_t>();  // SaveGameFileVersion
    r.Read<int32_t>();  // PackageFileUE4Version
    r.Read<uint16_t>(); r.Read<uint16_t>(); r.Read<uint16_t>();  // engine ver maj/min/patch
    r.Read<uint32_t>();                                          // changelist
    r.FStr();                                                    // branch
    r.Read<int32_t>();  // CustomVersionFormat
    const int32_t customCount = r.Read<int32_t>();
    if (!r.ok || customCount < 0 || customCount > 4096) return false;
    if (!r.Skip(static_cast<int64_t>(customCount) * 20)) return false;  // {guid16, ver4} each

    const std::wstring className = r.FStr();
    if (!r.ok) return false;
    out.parsed = true;
    out.isSaveSlotClass = className.find(L"saveSlot_C") != std::wstring::npos;
    if (!out.isSaveSlotClass) return true;  // data.sav etc: parsed, excluded by class

    // ---- top-level tagged-property walk ----------------------------------
    int found = 0;
    while (r.ok && found < 6) {
        const std::wstring name = r.FStr();
        if (!r.ok || name == L"None" || name.empty()) break;
        const std::wstring type = r.FStr();
        const int32_t size = r.Read<int32_t>();
        r.Read<int32_t>();  // ArrayIndex
        if (!r.ok || size < 0) break;

        // Type-specific tag extras (must be consumed to stay tag-aligned).
        std::wstring structName;
        uint8_t boolVal = 0;
        if (type == L"StructProperty") {
            structName = r.FStr();
            r.Skip(16);  // StructGuid
        } else if (type == L"BoolProperty") {
            boolVal = r.Read<uint8_t>();
            (void)boolVal;
        } else if (type == L"ByteProperty" || type == L"EnumProperty" ||
                   type == L"ArrayProperty" || type == L"SetProperty") {
            r.FStr();  // enum / inner type name
        } else if (type == L"MapProperty") {
            r.FStr();  // key type
            r.FStr();  // value type
        }
        const uint8_t hasGuid = r.Read<uint8_t>();
        if (hasGuid) r.Skip(16);
        if (!r.ok) break;

        // Harvest the wanted scalars; seek past everything else.
        bool consumed = false;
        if (name == L"Points" && type == L"IntProperty" && size == 4) {
            out.points = r.Read<int32_t>(); out.hasPoints = true; ++found; consumed = true;
        } else if (name == L"health" && type == L"FloatProperty" && size == 4) {
            out.health = r.Read<float>(); out.hasHealth = true; ++found; consumed = true;
        } else if (name == L"maxHealth" && type == L"FloatProperty" && size == 4) {
            out.maxHealth = r.Read<float>(); out.hasMaxHealth = true; ++found; consumed = true;
        } else if (name == L"Version" && type == L"StrProperty") {
            out.version = r.FStr(); out.hasVersion = true; ++found; consumed = true;
        } else if (name == L"savedtime" && structName == L"IntVector" && size == 12) {
            r.Read<int32_t>(); r.Read<int32_t>();
            out.savedTimeZ = r.Read<int32_t>(); out.hasSavedTimeZ = true; ++found; consumed = true;
        } else if (name == L"lastSavedDate" && structName == L"DateTime" && size == 8) {
            out.lastSavedDateTicks = r.Read<int64_t>();
            out.hasLastSavedDate = true; ++found; consumed = true;
        }
        if (!consumed && !r.Skip(size)) break;
    }
    return out.parsed;
}

}  // namespace ue_wrap::gvas_meta
