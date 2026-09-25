// ue_wrap/world/game_rules.cpp -- see ue_wrap/world/game_rules.h.

#include "ue_wrap/world/game_rules.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/world/game_mode.h"  // the mode byte: a GI member, not one of the rules

#include <cstdint>
#include <cstring>
#include <cwctype>

namespace ue_wrap::game_rules {
namespace {

namespace R = ue_wrap::reflection;

// The member name with its blueprint tail cut: "fallDamage_8_AEEA..." -> "fallDamage". The cut is
// at the first "_<digit>", which is where the tail always starts; a human sub-word such as
// "enableMG_math" keeps its "_". Unique within the struct, so it is the key a rule is known by.
std::string TrimmedKey(const std::wstring& name) {
    size_t cut = name.size();
    for (size_t i = 0; i + 1 < name.size(); ++i) {
        if (name[i] == L'_' && std::iswdigit(name[i + 1])) { cut = i; break; }
    }
    std::string out;
    out.reserve(cut);
    for (size_t i = 0; i < cut; ++i) out.push_back(static_cast<char>(name[i]));
    return out;
}

// Trim a GUID-mangled BP member name to its stable human prefix + light prettify:
// "fallDamage_8_AEEA..." -> "Fall damage". The member name is unique within the
// struct (BP enforces it), so the trimmed prefix is a unique key; prettify is
// cosmetic. Cut at the first "_<digit>" (the BP index/GUID tail always starts
// that way; a human sub-word like "enableMG_math" keeps its non-digit "_").
std::string PrettyLabel(const std::wstring& name) {
    size_t cut = name.size();
    for (size_t i = 0; i + 1 < name.size(); ++i) {
        if (name[i] == L'_' && std::iswdigit(name[i + 1])) { cut = i; break; }
    }
    std::string out;
    out.reserve(cut + 4);
    bool first = true;
    for (size_t i = 0; i < cut; ++i) {
        wchar_t c = name[i];
        if (c == L'_') { out.push_back(' '); continue; }
        if (first) {
            out.push_back(static_cast<char>(std::towupper(c)));
            first = false;
        } else if (std::iswupper(c)) {
            // camelCase word break -> " lowercased".
            out.push_back(' ');
            out.push_back(static_cast<char>(std::towlower(c)));
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    if (out.empty()) out = "(rule)";
    return out;
}

// The game's display name of one enumerator, through the same library function its rules pane
// fills its combo boxes with. Empty when the library or the enum is not there.
std::string EnumValueName(void* enumObj, uint8_t value) {
    static void* sCdo = nullptr;
    static void* sFn = nullptr;
    if (!enumObj) return {};
    if (!sCdo) sCdo = R::FindClassDefaultObject(L"KismetNodeHelperLibrary");
    if (sCdo && !sFn) sFn = R::FindFunction(R::ClassOf(sCdo), L"GetEnumeratorUserFriendlyName");
    if (!sCdo || !sFn) return {};
    ue_wrap::ParamFrame f(sFn);
    if (!f.valid() || !f.Set<void*>(L"Enum", enumObj) || !f.Set<uint8_t>(L"EnumeratorValue", value)) return {};
    if (!ue_wrap::Call(sCdo, f)) return {};
    R::FString ret{};
    if (!f.GetRaw(L"ReturnValue", &ret, sizeof(ret)) || !ret.Data) return {};
    std::string out;
    for (int32_t i = 0; i + 1 < ret.Num; ++i) out.push_back(static_cast<char>(ret.Data[i]));
    R::EngineFree(ret.Data);  // the returned string is ours: the frame is freed raw, not destructed
    return out;
}

// The rules struct at `owner.<prop>`, member by member. The struct is all bool + TEnumAsByte(1) +
// float(4) (no int32/FName), so: FindBoolProperty -> Bool; else size==4 -> Float; else 1-byte ->
// Enum. A future recook that adds an int32 member would render as Float, caught at the next
// struct re-RE. `withNames` also asks the game for each enum value's display name, one UFunction
// call per enum member: for a reader that shows the rules, not for the boot path's compare.
bool ReadRulesAt(void* owner, const wchar_t* prop, std::vector<RuleField>& out, bool withNames) {
    out.clear();
    void* cls = owner ? R::ClassOf(owner) : nullptr;
    if (!cls) return false;
    const int32_t off = R::FindPropertyOffset(cls, prop);
    void* structObj = R::PropertyInnerStruct(cls, prop);
    if (off < 0 || !structObj) return false;
    uint8_t* base = reinterpret_cast<uint8_t*>(owner) + off;

    for (const R::StructFieldInfo& fi : R::EnumerateStructFields(structObj)) {
        RuleField rf;
        rf.key = TrimmedKey(fi.name);
        rf.label = PrettyLabel(fi.name);

        int32_t bOff = -1;
        uint8_t bMask = 0;
        if (R::FindBoolProperty(structObj, fi.name.c_str(), bOff, bMask) && bOff >= 0) {
            rf.kind = Kind::Bool;
            rf.bval = (base[bOff] & bMask) != 0;
        } else if (fi.size == 4) {
            rf.kind = Kind::Float;
            rf.fval = *reinterpret_cast<float*>(base + fi.offset);
        } else {
            rf.kind = Kind::Enum;
            rf.ival = base[fi.offset];
            if (withNames) {
                rf.valueName = EnumValueName(R::PropertyEnum(structObj, fi.name.c_str()),
                                             static_cast<uint8_t>(rf.ival));
            }
        }
        out.push_back(std::move(rf));
    }
    return !out.empty();
}

}  // namespace

bool ReadLocal(Snapshot& out) {
    out.valid = false;
    out.savedValid = false;
    out.gamemode = -1;
    out.gamemodeName.clear();
    out.fields.clear();
    out.saved.clear();

    // The GameInstance through the world singleton, whose hold is revalidated by slot and serial,
    // never by reading the object: a teardown or rehost that freed it reads as gone. Null until it
    // boots.
    void* gi = world_singleton::GameInstance();
    if (!gi) return false;
    void* giClass = R::ClassOf(gi);
    if (!giClass) return false;

    out.gamemode = ue_wrap::game_mode::ReadFrom(gi);
    if (out.gamemode >= 0) out.gamemodeName = ue_wrap::game_mode::NameOrOrdinal(out.gamemode);

    out.valid = ReadRulesAt(gi, L"gameRules", out.fields, /*withNames=*/true);

    // The saved copy, off the save object the GameInstance holds for this world.
    const int32_t saveOff = R::FindPropertyOffset(giClass, L"save_gameInst");
    if (saveOff >= 0) {
        void* save = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gi) + saveOff);
        if (save && R::IsLive(save)) out.savedValid = ReadRulesAt(save, L"localGameRules", out.saved, /*withNames=*/true);
    }
    return out.valid;
}

const RuleField* NthOfKind(const std::vector<RuleField>& rules, Kind kind, int n) {
    for (const RuleField& f : rules) {
        if (f.kind != kind) continue;
        if (n-- == 0) return &f;
    }
    return nullptr;
}

int ApplySavedToProcess(void* gameInstance, void* save) {
    if (!gameInstance || !save || !R::IsLive(save)) return -1;
    void* giClass = R::ClassOf(gameInstance);
    void* saveClass = R::ClassOf(save);
    if (!giClass || !saveClass) return -1;
    const int32_t dstOff = R::FindPropertyOffset(giClass, L"gameRules");
    const int32_t srcOff = R::FindPropertyOffset(saveClass, L"localGameRules");
    void* dstStruct = R::PropertyInnerStruct(giClass, L"gameRules");
    void* srcStruct = R::PropertyInnerStruct(saveClass, L"localGameRules");
    const int32_t size = R::StructSize(dstStruct);
    // One struct type on both sides, or the bytes do not mean the same thing.
    if (dstOff < 0 || srcOff < 0 || !dstStruct || dstStruct != srcStruct || size <= 0) return -1;

    uint8_t* dst = reinterpret_cast<uint8_t*>(gameInstance) + dstOff;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(save) + srcOff;
    // The boot loop re-asserts this on every poll: equal bytes are the common case and cost nothing.
    if (std::memcmp(dst, src, static_cast<size_t>(size)) == 0) return 0;

    std::vector<RuleField> before, saved;
    ReadRulesAt(gameInstance, L"gameRules", before, /*withNames=*/false);
    ReadRulesAt(save, L"localGameRules", saved, /*withNames=*/false);
    int changed = 0;
    for (size_t i = 0; i < before.size() && i < saved.size(); ++i) {
        const RuleField& a = before[i];
        const RuleField& b = saved[i];
        if (a.bval != b.bval || a.ival != b.ival || a.fval != b.fval) ++changed;
    }
    // The members are bools, bytes and one float: a plain value copy is the whole assignment.
    std::memcpy(dst, src, static_cast<size_t>(size));
    return changed;
}

}  // namespace ue_wrap::game_rules
