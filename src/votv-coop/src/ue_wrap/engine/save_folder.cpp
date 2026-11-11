// ue_wrap/engine/save_folder.cpp -- see ue_wrap/engine/save_folder.h.
//
// CompressedSaveLoadLib is the game's own native library, so its functions carry no Blueprint body
// to read their parameter names from; the blueprints call them positionally. The frame is bound the
// same way: its parameters in declaration order, checked by size, so the binding holds whatever the
// native names its folder parameter.

#include "ue_wrap/engine/save_folder.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <wchar.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ue_wrap::save_folder {

namespace R = reflection;
namespace P = profile;

namespace {

void* g_libCdo = nullptr;  // the CompressedSaveLoadLib CDO: native, rooted
void* g_loadFn = nullptr;
void* g_saveFn = nullptr;

bool ResolveLib() {
    if (!g_libCdo) g_libCdo = R::FindClassDefaultObject(P::name::CompressedSaveLoadLibClass);
    if (g_libCdo) {
        void* cls = R::ClassOf(g_libCdo);
        if (cls && !g_loadFn) g_loadFn = R::FindFunction(cls, P::name::LoadGameCompressedFn);
        if (cls && !g_saveFn) g_saveFn = R::FindFunction(cls, P::name::SaveGameCompressedFn);
    }
    return g_libCdo && g_loadFn && g_saveFn;
}

// An FString aliasing `buf` for the duration of one call; the native copies what it keeps.
R::FString Alias(std::wstring& buf) {
    R::FString fs{};
    fs.Data = buf.data();
    fs.Num = static_cast<int32_t>(buf.size()) + 1;  // FString::Num counts the null
    fs.Max = fs.Num;
    return fs;
}

// The frame's parameters split by shape, in declaration order: the FStrings (16 bytes), the
// object pointer, the int32 user index and the return value. False when the shape is not the
// one `fn` is expected to have, named once per function.
struct Shape {
    std::vector<int32_t> strings;
    int32_t object = -1;
    int32_t userIndex = -1;
    int32_t ret = -1;
};
bool ShapeOf(void* fn, size_t wantStrings, bool wantObject, Shape& out) {
    out = Shape{};
    for (const R::ParamInfo& p : R::FunctionParams(fn)) {
        if (p.flags & P::cpf::ReturnParm) out.ret = p.offset;
        else if (p.size == static_cast<int32_t>(sizeof(R::FString))) out.strings.push_back(p.offset);
        else if (p.size == 8) out.object = p.offset;
        else if (p.size == 4) out.userIndex = p.offset;
    }
    const bool ok = out.strings.size() == wantStrings && (out.object >= 0) == wantObject &&
                    out.userIndex >= 0 && out.ret >= 0;
    if (!ok) {
        static void* sNamed = nullptr;
        if (sNamed != fn) {
            sNamed = fn;
            std::wstring names;
            for (const R::ParamInfo& p : R::FunctionParams(fn))
                names += p.name + L"(" + std::to_wstring(p.size) + L") ";
            UE_LOGE("save_folder: %ls has an unexpected frame: %ls", R::ToString(R::NameOf(fn)).c_str(),
                    names.c_str());
        }
    }
    return ok;
}

std::wstring g_savedDir;  // GetProjectSavedDirectory, with a trailing '/'

}  // namespace

std::wstring FolderPath(const std::wstring& name) {
    if (g_savedDir.empty()) {
        void* ksl = R::FindClassDefaultObject(L"KismetSystemLibrary");
        void* fn = ksl ? R::FindFunction(R::ClassOf(ksl), L"GetProjectSavedDirectory") : nullptr;
        if (!fn) return {};
        ParamFrame f(fn);
        if (!Call(ksl, f)) return {};
        R::FString ret{};
        f.GetRaw(L"ReturnValue", &ret, static_cast<int32_t>(sizeof(ret)));
        if (!ret.Data || ret.Num <= 1) return {};
        g_savedDir.assign(ret.Data, ret.Data + (ret.Num - 1));
        R::EngineFree(ret.Data);  // the engine minted it for this call; the copy is kept
        if (g_savedDir.back() != L'/' && g_savedDir.back() != L'\\') g_savedDir += L'/';
        UE_LOGI("save_folder: the saved directory is '%ls'", g_savedDir.c_str());
    }
    return g_savedDir + L"SaveGames/" + name;
}

void* LoadCompressed(const std::wstring& slot, const std::wstring& folder, const wchar_t* extension) {
    if (!ResolveLib()) {
        UE_LOGW("save_folder: CompressedSaveLoadLib unresolved (cdo=%p load=%p)", g_libCdo, g_loadFn);
        return nullptr;
    }
    Shape s;
    if (!ShapeOf(g_loadFn, 3, false, s)) return nullptr;
    std::wstring a = slot, b = folder, c = extension ? extension : L"";
    const R::FString fa = Alias(a), fb = Alias(b), fc = Alias(c);
    ParamFrame f(g_loadFn);
    uint8_t* d = static_cast<uint8_t*>(f.data());
    if (!d) return nullptr;
    // LoadGameCompressed(SlotName, folder, SaveFiletype, UserIndex), the order the blueprints pass.
    std::memcpy(d + s.strings[0], &fa, sizeof(fa));
    std::memcpy(d + s.strings[1], &fb, sizeof(fb));
    std::memcpy(d + s.strings[2], &fc, sizeof(fc));
    *reinterpret_cast<int32_t*>(d + s.userIndex) = 0;
    if (!Call(g_libCdo, f)) return nullptr;
    return *reinterpret_cast<void**>(d + s.ret);
}

bool SaveCompressed(void* save, const std::wstring& folder, const std::wstring& slot,
                    const wchar_t* extension) {
    if (!save || !ResolveLib()) return false;
    Shape s;
    if (!ShapeOf(g_saveFn, 3, true, s)) return false;
    std::wstring a = folder, b = slot, c = extension ? extension : L"";
    const R::FString fa = Alias(a), fb = Alias(b), fc = Alias(c);
    ParamFrame f(g_saveFn);
    uint8_t* d = static_cast<uint8_t*>(f.data());
    if (!d) return false;
    // SaveGameCompressed(SaveGameObject, folder, SlotName, SaveFiletype, UserIndex).
    *reinterpret_cast<void**>(d + s.object) = save;
    std::memcpy(d + s.strings[0], &fa, sizeof(fa));
    std::memcpy(d + s.strings[1], &fb, sizeof(fb));
    std::memcpy(d + s.strings[2], &fc, sizeof(fc));
    *reinterpret_cast<int32_t*>(d + s.userIndex) = 0;
    if (!Call(g_libCdo, f)) return false;
    return *reinterpret_cast<bool*>(d + s.ret);
}

std::wstring AreaFolder(const std::wstring& saveFolderPath) { return saveFolderPath + L"/areas"; }

std::wstring AreaOf(const std::wstring& folderPath) {
    void* meta = LoadCompressed(L"main", folderPath, L".meta");
    void* cls = meta ? R::ClassOf(meta) : nullptr;
    const int32_t off = cls ? R::FindPropertyOffset(cls, L"area") : -1;
    if (off < 0) return {};
    const auto& s = *reinterpret_cast<const R::FString*>(static_cast<const uint8_t*>(meta) + off);
    return (s.Data && s.Num > 1) ? std::wstring(s.Data, s.Data + (s.Num - 1)) : std::wstring();
}

std::wstring SaveNameOf(const wchar_t* folderPath) {
    if (!folderPath) return {};
    const std::wstring path(folderPath);
    bool afterSaves = false;
    size_t at = 0;
    while (at <= path.size()) {
        size_t end = path.find_first_of(L"/\\", at);
        if (end == std::wstring::npos) end = path.size();
        const std::wstring part = path.substr(at, end - at);
        if (afterSaves) return part;
        afterSaves = _wcsicmp(part.c_str(), L"SaveGames") == 0;
        at = end + 1;
    }
    return {};
}

bool ResolveSaveFrame(SaveFrame& out) {
    if (!ResolveLib()) return false;
    Shape s;
    if (!ShapeOf(g_saveFn, 3, true, s)) return false;
    out.function = g_saveFn;
    out.object = s.object;
    out.folder = s.strings[0];
    out.slot = s.strings[1];
    out.extension = s.strings[2];
    return true;
}

}  // namespace ue_wrap::save_folder
