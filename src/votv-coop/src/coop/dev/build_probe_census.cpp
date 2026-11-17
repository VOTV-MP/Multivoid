// coop/dev/build_probe_census.cpp -- the building probe's census of nails, walls and posters; see coop/dev/build_probe.h.

#include "build_probe_detail.h"

#include "coop/net/net_clock.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_key_index.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::build_probe::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace KI = coop::prop_key_index;

// The four lineages the census reads, each a base class: a piece is any instance of a class that walks to one.
constexpr const wchar_t* kBases[] = {L"nail_C", L"customWall_C", L"customWall_unfinished_C", L"poster_C"};
constexpr int            kBaseCount = static_cast<int>(sizeof(kBases) / sizeof(kBases[0]));

struct Piece {
    ue_wrap::CachedObjRef actor;
    std::wstring          key;
    std::string           cls;
    std::string           level;
    ue_wrap::FVector      at{};
    bool                  persistent = false;
    bool                  indexed = false;
    uint64_t              indexedAfterMs = 0;
};

// This world's pieces, and the world they were read in (a new world drops them: the earlier census is kept for the diff).
std::vector<Piece> g_pieces;
std::vector<Piece> g_before;
uint64_t           g_edgeMs = 0;
uint64_t           g_indexVersion = 0;
bool               g_endSaid = false;

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

void OnInstance(void* ctx, void* obj, int32_t) {
    auto* out = static_cast<std::vector<Piece>*>(ctx);
    if (R::NameStartsWith(R::NameOf(obj), L"Default__") || !R::IsLive(obj)) return;
    Piece p;
    p.actor.Set(obj);
    p.cls = Narrow(R::ClassNameOf(obj));
    p.key = ue_wrap::prop::GetActorSaveKeyString(obj);
    void* level = R::OuterOf(obj);
    void* owner = level ? R::OuterOf(level) : nullptr;
    p.level = owner ? Narrow(R::ToString(R::NameOf(owner))) : std::string("unread");
    p.persistent = owner && owner == ue_wrap::world_identity::CurrentWorld();
    E::TryGetActorLocation(obj, p.at);
    out->push_back(std::move(p));
}

// Whether the key index holds this piece's key now.
bool Held(const Piece& p) {
    if (p.key.empty() || p.key == L"None") return false;
    return KI::Find(p.key) != nullptr;
}

void SayPiece(Role role, int generation, const Piece& p) {
    void* a = p.actor.Get();
    namespace JS = coop::join_membership_sweep;
    const bool claimed = a && JS::ClaimedActors().count(a) != 0;
    const bool inUniverse = a && ue_wrap::prop::IsClassKeyedInteractable(R::ClassOf(a));
    const bool unclaimed = a && JS::IsInDivergenceUniverseUnclaimed(a);
    float health = -1.f;
    if (a) ReadFloat(a, L"health", health);
    Info("census role=%s world=%d class=%s key=%ls level=%s persistent=%d at=(%.1f,%.1f,%.1f) health=%.1f "
         "keyIndex=%s sweep=%s",
         RoleName(role), generation, p.cls.c_str(), p.key.c_str(), p.level.c_str(), p.persistent ? 1 : 0,
         static_cast<double>(p.at.X), static_cast<double>(p.at.Y), static_cast<double>(p.at.Z),
         static_cast<double>(health), p.indexed ? "held-at-edge" : "not-held",
         claimed ? "claimed" : unclaimed ? "unclaimed" : inUniverse ? "in-universe-spared" : "outside-universe");
}

}  // namespace

void RunCensus(Role role, int generation, uint64_t edgeMs) {
    g_before = std::move(g_pieces);
    g_pieces.clear();
    g_edgeMs = edgeMs;
    g_endSaid = false;
    g_indexVersion = KI::Version();
    Walk w;
    for (int i = 0; i < kBaseCount; ++i) w.bases[i] = ue_wrap::object_index::ClassByName(kBases[i]);
    ue_wrap::object_index::ForEachClass(&OnClass, &w);
    for (void* cls : w.classes) ue_wrap::object_index::ForEachInstance(cls, &OnInstance, &g_pieces);
    int nails = 0, walls = 0, unfinished = 0, posters = 0, held = 0, streamed = 0;
    for (Piece& p : g_pieces) {
        p.indexed = Held(p);
        if (p.indexed) ++held;
        if (!p.persistent) ++streamed;
        if (p.cls.rfind("nail", 0) == 0) ++nails;
        else if (p.cls.find("unfinished") != std::string::npos) ++unfinished;
        else if (p.cls.rfind("customWall", 0) == 0) ++walls;
        else ++posters;
        SayPiece(role, generation, p);
    }
    Info("census-summary role=%s world=%d nails=%d walls=%d unfinished=%d posters=%d heldByKeyIndex=%d "
         "notInPersistentLevel=%d indexCanAnswer=%d indexSize=%zu",
         RoleName(role), generation, nails, walls, unfinished, posters, held, streamed, KI::CanAnswer() ? 1 : 0,
         KI::Size());
}

void PollKeyIndex(uint64_t nowMs) {
    if (g_pieces.empty()) return;
    const uint64_t v = KI::Version();
    if (v == g_indexVersion) return;
    g_indexVersion = v;
    for (Piece& p : g_pieces) {
        if (p.indexed || !p.actor.Alive() || !Held(p)) continue;
        p.indexed = true;
        p.indexedAfterMs = nowMs - g_edgeMs;
        Info("key-index: %s key=%ls is held %llu ms after the world-ready edge", p.cls.c_str(), p.key.c_str(),
             static_cast<unsigned long long>(p.indexedAfterMs));
    }
}

void SayKeyDiff(int generation) {
    if (g_before.empty()) return;
    int same = 0, changed = 0, gone = 0;
    for (const Piece& b : g_before) {
        const Piece* match = nullptr;
        for (const Piece& n : g_pieces)
            if (n.cls == b.cls && std::abs(n.at.X - b.at.X) < 5.f && std::abs(n.at.Y - b.at.Y) < 5.f &&
                std::abs(n.at.Z - b.at.Z) < 5.f) {
                match = &n;
                break;
            }
        if (!match) {
            ++gone;
            Info("keys-diff world=%d: %s at (%.1f,%.1f,%.1f) key=%ls has no piece of its class at that place now",
                 generation, b.cls.c_str(), static_cast<double>(b.at.X), static_cast<double>(b.at.Y),
                 static_cast<double>(b.at.Z), b.key.c_str());
        } else if (match->key != b.key) {
            ++changed;
            Info("keys-diff world=%d: %s at (%.1f,%.1f,%.1f) key %ls -> %ls", generation, b.cls.c_str(),
                 static_cast<double>(b.at.X), static_cast<double>(b.at.Y), static_cast<double>(b.at.Z), b.key.c_str(),
                 match->key.c_str());
        } else {
            ++same;
        }
    }
    Info("keys-diff-summary world=%d: kept=%d changed=%d gone=%d now=%zu before=%zu", generation, same, changed, gone,
         g_pieces.size(), g_before.size());
}

void SayKeyIndexEnd() {
    if (g_endSaid || g_pieces.empty()) return;
    g_endSaid = true;
    int never = 0;
    for (const Piece& p : g_pieces)
        if (!p.indexed && p.actor.Alive()) ++never;
    Info("key-index-end: %d of %zu pieces never held by the prop key index in this world", never, g_pieces.size());
}

void* LevelWall() {
    for (const Piece& p : g_pieces) {
        if (p.cls != "customWall_C" || !p.persistent) continue;
        void* a = p.actor.Get();
        if (a && R::IsLive(a)) return a;
    }
    return nullptr;
}

}  // namespace coop::dev::build_probe::detail
