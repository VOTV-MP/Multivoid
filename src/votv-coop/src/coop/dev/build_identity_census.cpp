// coop/dev/build_identity_census.cpp -- the building identity drill's census of nails, walls and posters; see
// coop/dev/build_identity_drill.h.

#include "build_identity_detail.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::build_identity {
namespace {

namespace R = ue_wrap::reflection;

// The four lineages the census reads, each a base class: a piece is any instance of a class that walks to one.
constexpr const wchar_t* kBases[] = {L"nail_C", L"customWall_C", L"customWall_unfinished_C", L"poster_C"};
constexpr int            kBaseCount = static_cast<int>(sizeof(kBases) / sizeof(kBases[0]));

struct Walk {
    void*              bases[kBaseCount] = {};
    std::vector<void*> classes;
};

void OnClass(void* ctx, void* cls, void*) {
    auto* w = static_cast<Walk*>(ctx);
    for (void* b : w->bases)
        if (b && ue_wrap::prop::WalksToBase(cls, b)) {
            w->classes.push_back(cls);
            return;
        }
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

void OnInstance(void* ctx, void* obj, int32_t) {
    auto* out = static_cast<std::vector<Piece>*>(ctx);
    if (R::NameStartsWith(R::NameOf(obj), L"Default__") || !R::IsLive(obj)) return;
    Piece p;
    p.cls = Narrow(R::ClassNameOf(obj));
    p.key = ue_wrap::prop::GetInteractableKeyString(obj);
    void* level = R::OuterOf(obj);
    void* owner = level ? R::OuterOf(level) : nullptr;
    p.level = owner ? Narrow(R::ToString(R::NameOf(owner))) : std::string("unread");
    p.persistent = owner && owner == ue_wrap::world_identity::CurrentWorld();
    out->push_back(std::move(p));
}

}  // namespace

void Info(const char* fmt, ...) {
    char buf[480];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGI("build-identity: %s", buf);
}

void CollectPieces(std::vector<Piece>& out) {
    out.clear();
    Walk w;
    for (int i = 0; i < kBaseCount; ++i) w.bases[i] = ue_wrap::object_index::ClassByName(kBases[i]);
    ue_wrap::object_index::ForEachClass(&OnClass, &w);
    for (void* cls : w.classes) ue_wrap::object_index::ForEachInstance(cls, &OnInstance, &out);
}

LevelCensus SayAndTotal(const char* role, const std::vector<Piece>& pieces) {
    LevelCensus level;
    std::vector<std::string> tokens;
    for (const Piece& p : pieces) {
        Info("census role=%s class=%s key=%ls level=%s persistent=%d", role, p.cls.c_str(), p.key.c_str(),
             p.level.c_str(), p.persistent ? 1 : 0);
        if (!p.persistent) continue;
        if (p.cls == "customWall_C") ++level.walls;
        else if (p.cls.rfind("poster", 0) == 0) ++level.posters;  // poster_C and its children (the level's are poster_c)
        else continue;
        tokens.push_back(p.cls + "=" + Narrow(p.key));
    }
    std::sort(tokens.begin(), tokens.end());
    for (const std::string& t : tokens) {
        if (!level.text.empty()) level.text += ';';
        level.text += t;
    }
    Info("census-summary role=%s pieces=%zu levelWalls=%d levelPosters=%d", role, pieces.size(), level.walls,
         level.posters);
    return level;
}

}  // namespace coop::dev::build_identity
