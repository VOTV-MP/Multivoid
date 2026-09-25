// ue_wrap/world/game_mode.cpp -- see ue_wrap/world/game_mode.h.

#include "ue_wrap/world/game_mode.h"

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>

namespace ue_wrap::game_mode {
namespace {

namespace R = ue_wrap::reflection;

// Declaration order, so the index is the ordinal. Tutorial and custom are front-door menu modes
// like the rest; they are the two the slot menu gives no save-name prefix, which is a fact about
// save names and not about the modes.
const wchar_t* const kNames[kCount] = {
    L"Story", L"Infinite", L"Tutorial", L"Custom", L"Sandbox", L"Halloween", L"Ambient", L"Solar",
};

// The byte's offset on the GameInstance class, resolved by name rather than pinned: the property
// is reflected, and a recook that moves it then moves the read and the write together.
int32_t ModeOffset(void* gameInstance) {
    void* cls = gameInstance ? R::ClassOf(gameInstance) : nullptr;
    if (!cls) return -1;
    return R::FindPropertyOffset(cls, L"GameMode");
}

}  // namespace

const wchar_t* Name(int ord) { return IsValid(ord) ? kNames[ord] : nullptr; }

std::string NameOrOrdinal(int ord) {
    const wchar_t* n = Name(ord);
    if (!n) return "#" + std::to_string(ord);
    std::string out;
    for (const wchar_t* p = n; *p; ++p) out.push_back(static_cast<char>(*p));
    return out;
}

int ReadFrom(void* gameInstance) {
    const int32_t off = ModeOffset(gameInstance);
    if (off < 0) return -1;
    return *(reinterpret_cast<uint8_t*>(gameInstance) + off);
}

int ReadLocal() {
    // A null GameInstance is the answer "not booted yet"; the singleton finds it the moment it is.
    return ReadFrom(world_singleton::GameInstance());
}

int WriteTo(void* gameInstance, int mode) {
    if (!IsValid(mode)) return -1;
    const int32_t off = ModeOffset(gameInstance);
    if (off < 0) return -1;
    uint8_t* gm = reinterpret_cast<uint8_t*>(gameInstance) + off;
    const int old = *gm;
    *gm = static_cast<uint8_t>(mode);
    return old;
}

}  // namespace ue_wrap::game_mode
