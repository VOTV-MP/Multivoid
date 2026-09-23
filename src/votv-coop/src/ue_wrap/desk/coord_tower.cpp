// ue_wrap/desk/coord_tower.cpp -- see ue_wrap/desk/coord_tower.h.

#include "ue_wrap/desk/coord_tower.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <algorithm>
#include <chrono>

namespace ue_wrap::coord_tower {
namespace {

namespace R = reflection;
using field_io::TArrayView;

constexpr const wchar_t* kClassName = L"coordRadarDish_C";

CachedObjRef g_cls;             // coordRadarDish_C; a class the map loads, so a new world may load a new one
int32_t  g_offId = -1;          // int
int32_t  g_offBroken = -1;      // bool isBroken
uint8_t  g_maskBroken = 0;
int32_t  g_offOpened = -1;      // bool opened (the fuse panel)
uint8_t  g_maskOpened = 0;
int32_t  g_offFuses = -1;       // TArray<uint8>
int32_t  g_offLights = -1;      // TArray<bool> puzzleLights
bool     g_membersResolved = false;
bool     g_latchedOff = false;  // the class loaded without a member: it will not appear later
uint64_t g_missMs = 0;          // a class lookup that misses walks the object array; hold off after one

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Take `cls`, the loaded coordRadarDish_C, as the class. Its members are resolved from the first
// one taken: a class the same map loads again has the same layout. Reads the class's property
// chain only, never the object array.
bool Adopt(void* cls) {
    if (!g_membersResolved) {
        g_offId     = R::FindPropertyOffset(cls, L"id");
        g_offFuses  = R::FindPropertyOffset(cls, L"fuses");
        g_offLights = R::FindPropertyOffset(cls, L"puzzleLights");
        R::FindBoolProperty(cls, L"isBroken", g_offBroken, g_maskBroken);
        R::FindBoolProperty(cls, L"opened", g_offOpened, g_maskOpened);
        if (g_offId < 0 || g_offFuses < 0 || g_offLights < 0 || g_offBroken < 0 || !g_maskBroken ||
            g_offOpened < 0 || !g_maskOpened) {
            g_latchedOff = true;
            UE_LOGW("coord_tower: coordRadarDish_C is loaded but a member is missing (id=%d fuses=%d puzzleLights=%d "
                    "isBroken=%d opened=%d) -- the tower reads stay off; game version mismatch?",
                    g_offId, g_offFuses, g_offLights, g_offBroken, g_offOpened);
            return false;
        }
        g_membersResolved = true;
        UE_LOGI("coord_tower: resolved (id=0x%X isBroken=0x%X opened=0x%X fuses=0x%X puzzleLights=0x%X)", g_offId,
                g_offBroken, g_offOpened, g_offFuses, g_offLights);
    }
    g_cls.Set(cls);
    return true;
}

struct Collect { State* out; int32_t cap; int32_t n; };

void ReadOne(void* ctx, void* obj, int32_t index) {
    auto* c = static_cast<Collect*>(ctx);
    if (c->n >= c->cap || !obj) return;
    // An index member may still be loading, under construction or dying; the slot's flags say so.
    if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
    if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
    State& s = c->out[c->n++];
    s = State{};
    const auto* base = reinterpret_cast<const uint8_t*>(obj);
    s.id       = *reinterpret_cast<const int32_t*>(base + g_offId);
    s.isBroken = (base[g_offBroken] & g_maskBroken) != 0;
    s.opened   = (base[g_offOpened] & g_maskOpened) != 0;
    const auto* f = reinterpret_cast<const TArrayView*>(base + g_offFuses);
    for (int32_t i = 0; f->data && i < f->num && i < 32; ++i)
        s.fuses.push_back(static_cast<wchar_t>(L'0' + f->data[i] % 10));
    const auto* p = reinterpret_cast<const TArrayView*>(base + g_offLights);
    for (int32_t i = 0; p->data && i < p->num && i < 64; ++i) s.puzzleLights.push_back(p->data[i] ? L'1' : L'0');
}

}  // namespace

bool EnsureResolved() {
    if (g_membersResolved && g_cls.Alive()) return true;
    if (g_latchedOff) return false;
    const uint64_t now = NowMs();
    if (g_missMs != 0 && now - g_missMs < 2000) return false;
    void* cls = R::FindClass(kClassName);
    if (!cls) { g_missMs = now; return false; }
    g_missMs = 0;
    return Adopt(cls);
}

int32_t ReadAll(State* out, int32_t cap) {
    if (!out || cap <= 0 || !EnsureResolved()) return -1;
    Collect c{out, cap, 0};
    object_index::ForEachInstance(g_cls.Raw(), &ReadOne, &c);
    std::sort(out, out + c.n, [](const State& a, const State& b) { return a.id < b.id; });
    return c.n;
}

int32_t IdOf(void* tower) {
    if (!tower || g_latchedOff) return -1;
    void* cls = R::ClassOf(tower);
    if (!cls) return -1;
    if (!g_membersResolved || !g_cls.Alive() || cls != g_cls.Raw()) {
        // The tower in hand names its class, so the lookup that would walk the object array is
        // never needed here: a name compare, and the property chain on the first one.
        if (!R::NameEquals(R::NameOf(cls), kClassName) || !Adopt(cls)) return -1;
    }
    return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(tower) + g_offId);
}

}  // namespace ue_wrap::coord_tower
