// ue_wrap/game_rules.cpp -- see ue_wrap/game_rules.h.

#include "ue_wrap/world/game_rules.h"

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <cstdint>
#include <cwctype>

namespace ue_wrap::game_rules {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// enum_gamemode ordinals -> friendly names. VOTV strips enum display names in
// the cook, but the ordinal->mode map is EMPIRICALLY verified in the save-picker
// RE (research/findings/saves/votv-save-picker-create-new-RE-2026-06-06.md, the
// getSavePrefix table). 2 and 3 are unused sentinels ("-").
const char* GameModeName(int ord) {
    switch (ord) {
        case 0: return "Story";
        case 1: return "Infinite";
        case 4: return "Sandbox";
        case 5: return "Halloween";
        case 6: return "Ambience";
        case 7: return "Solar";
        default: return nullptr;  // 2/3/unknown -> caller renders "#N"
    }
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

}  // namespace

bool ReadLocal(Snapshot& out) {
    out.valid = false;
    out.gamemode = -1;
    out.gamemodeName.clear();
    out.fields.clear();

    // Re-resolve the GameInstance FRESH every snapshot (game thread): never cache
    // a pointer a mid-flight teardown / rehost could free. Null until it boots.
    void* gi = R::FindObjectByClass(P::name::GameInstanceClass);
    if (!gi) return false;
    void* giClass = R::ClassOf(gi);
    if (!giClass) return false;

    const int32_t grOff = R::FindPropertyOffset(giClass, L"gameRules");
    void* structObj = R::PropertyInnerStruct(giClass, L"gameRules");
    if (grOff < 0 || !structObj) return false;
    uint8_t* base = reinterpret_cast<uint8_t*>(gi) + grOff;

    // gamemode (a GI member, NOT inside gameRules).
    const int32_t gmOff = R::FindPropertyOffset(giClass, L"GameMode");
    if (gmOff >= 0) {
        out.gamemode = *(reinterpret_cast<uint8_t*>(gi) + gmOff);
        const char* nm = GameModeName(out.gamemode);
        if (nm) out.gamemodeName = nm;
        else    out.gamemodeName = "#" + std::to_string(out.gamemode);
    }

    // Enumerate the gameRules members by reflection and read each by type. The
    // struct is all bool + TEnumAsByte(1) + float(4) (no int32/FName), so:
    // FindBoolProperty -> Bool; else size==4 -> Float; else 1-byte -> Enum. A
    // future recook that adds an int32 member would render as Float (harmless
    // debug degradation, caught at the next struct re-RE).
    for (const R::StructFieldInfo& fi : R::EnumerateStructFields(structObj)) {
        RuleField rf;
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
        }
        out.fields.push_back(std::move(rf));
    }

    out.valid = !out.fields.empty();
    return out.valid;
}

}  // namespace ue_wrap::game_rules
