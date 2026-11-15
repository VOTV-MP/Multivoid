// coop/dev/level_prop_drill.cpp -- see coop/dev/level_prop_drill.h.

#include "coop/dev/level_prop_drill.h"

#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"
#include "coop/world/spawn_authority.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/engine/world_identity.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::level_prop_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr const wchar_t* kShrimpPath = L"/Game/objects/prop_food_shrimp.prop_food_shrimp_C";

uint64_t g_clientSeenMs = 0, g_lastSayHost = 0, g_lastSayClient = 0;
bool g_done[12] = {};

// This world's live instances of a class, found as level_fixture_sync's LiveOf finds them: the object index's walk of the class's
// own instances first, the liveness and world tests after it. Never R::FindObjectsByClass, a walk of every object that touched a
// package still loading (a client's run absorbed 16 faults that way); and never every frame.
std::vector<void*> Live(const wchar_t* cls) {
    std::vector<void*> out;
    void* c = ue_wrap::object_index::ClassByName(cls);
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!c || !world) return out;
    std::vector<void*> found;
    ue_wrap::object_index::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
        if (obj) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &found);
    for (void* obj : found)
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__") && ue_wrap::world_identity::WorldOf(obj) == world)
            out.push_back(obj);
    return out;
}

size_t EmptyPacks();

bool BoolProp(void* actor, const wchar_t* name, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(actor), name, off, mask) || off < 0) return false;
    out = (*(static_cast<const uint8_t*>(actor) + off) & mask) != 0;
    return true;
}

bool FloatProp(void* actor, const wchar_t* name, float& out) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), name);
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(actor) + off, sizeof(out));
    return true;
}

bool Hidden(void* actor) {
    bool h = false;
    return BoolProp(actor, L"bHidden", h) && h;
}

bool Call0(void* actor, const wchar_t* fnName) {
    void* fn = actor ? R::FindDispatchFunctionCached(R::ClassOf(actor), fnName) : nullptr;
    return fn && R::CallFunction(actor, fn, nullptr);
}

bool Once(int slot, uint64_t since, uint64_t atMs) {
    if (g_done[slot] || since < atMs) return false;
    g_done[slot] = true;
    return true;
}

void MoveBy(const wchar_t* cls, float dx, const char* why) {
    // The same actor each time: the first one this drill moved, so a move back undoes it.
    static std::vector<std::pair<std::wstring, void*>> s_moved;
    void* want = nullptr;
    for (const auto& m : s_moved) if (m.first == cls) want = m.second;
    for (void* a : Live(cls)) {
        if (want && a != want) continue;
        if (!want) s_moved.emplace_back(cls, a);
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(a, at)) continue;
        at.X += dx;
        E::SetActorLocation(a, at);
        UE_LOGI("[GAP2] [H] %s: %ls '%ls' written %.0f cm to (%.0f, %.0f, %.0f)", why, cls, R::ToString(R::NameOf(a)).c_str(), dx, at.X,
                at.Y, at.Z);
        return;   // one of each class: the first live one
    }
    UE_LOGI("[GAP2] [H] %s: no live %ls to move", why, cls);
}

void SayActors(const char* who, uint64_t since, const wchar_t* cls) {
    const std::vector<void*> live = Live(cls);
    if (live.empty()) {
        UE_LOGI("[GAP2] [%s] t=%us: no %ls", who, static_cast<unsigned>(since / 1000), cls);
        return;
    }
    size_t n = 0;
    for (void* a : live) {
        if (++n > 3) { UE_LOGI("[GAP2] [%s] t=%us: ... %zu %ls in all", who, static_cast<unsigned>(since / 1000), live.size(), cls); break; }
        ue_wrap::FVector at{};
        E::TryGetActorLocation(a, at);
        bool open = false, closed = false;
        float alpha = -1.f;
        const bool isDoor = BoolProp(a, L"movedir", open);
        if (isDoor) { BoolProp(a, L"closed", closed); FloatProp(a, L"alpha", alpha); }
        if (isDoor)
            UE_LOGI("[GAP2] [%s] t=%us: %ls '%ls' at (%.0f, %.0f, %.0f) %s, %s, alpha %.2f", who, static_cast<unsigned>(since / 1000), cls,
                    R::ToString(R::NameOf(a)).c_str(), at.X, at.Y, at.Z, Hidden(a) ? "hidden" : "shown", open ? "open" : "closed", alpha);
        else
            UE_LOGI("[GAP2] [%s] t=%us: %ls '%ls' key '%ls' at (%.0f, %.0f, %.0f) %s", who, static_cast<unsigned>(since / 1000), cls,
                    R::ToString(R::NameOf(a)).c_str(), ue_wrap::prop::GetInteractableKeyString(a).c_str(), at.X, at.Y, at.Z,
                    Hidden(a) ? "hidden" : "shown");
    }
}

void SayRefusals(const char* who, uint64_t since, const char* const* tags, size_t n) {
    std::string line;
    for (size_t i = 0; i < n; ++i)
        line += std::string(tags[i]) + "=" + std::to_string(static_cast<unsigned long long>(coop::spawn_authority::RefusalCount(tags[i]))) + " ";
    UE_LOGI("[GAP2] [%s] t=%us: refusals %s", who, static_cast<unsigned>(since / 1000), line.c_str());
}

bool SayDue(uint64_t since, uint64_t& last) {
    const uint64_t bucket = since / 5000 + 1;
    if (bucket <= last) return false;
    last = bucket;
    return true;
}

void Say(const char* who, const std::string& name, uint64_t since) {
    if (name == "gapDoor") {
        SayActors(who, since, L"TheDoor_C");
        static const char* const tags[] = {"theDoor.tp", "theDoor.open", "theDoor.close"};
        SayRefusals(who, since, tags, 3);
    } else if (name == "gapWander") {
        SayActors(who, since, L"rorGraf_C");
        SayActors(who, since, L"log_C");
        SayActors(who, since, L"prop_duc_C");
        SayActors(who, since, L"prop_mannequin_C");
        static const char* const tags[] = {"rorGraf.move", "log.ReceiveTick", "log.check", "propDuc.chain", "propDuc.ReceiveTick",
                                           "mannequinTp.ReceiveTick"};
        SayRefusals(who, since, tags, 6);
    } else if (name == "gapShrimp") {
        const std::vector<void*> eaters = Live(L"shrimpEaterMaster_C");
        UE_LOGI("[GAP2] [%s] t=%us: %zu shrimp prop(s), %zu empty pack(s) (prop_C named shrimp_e), %zu eater(s)", who, static_cast<unsigned>(since / 1000),
                Live(L"prop_food_shrimp_C").size(), EmptyPacks(), eaters.size());
        static const char* const tags[] = {"shrimpEater.eat"};
        SayRefusals(who, since, tags, 1);
    } else if (name == "gapPrints") {
        SayActors(who, since, L"fprints_C");
        static const char* const tags[] = {"fprints.ReceiveTick"};
        SayRefusals(who, since, tags, 1);
    }
}

void StandShrimp() {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kShrimpPath);
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    void* shrimp = cls && player && E::TryGetActorLocation(player, at) ? E::SpawnActor(cls, {at.X + 250.f, at.Y, at.Z + 20000.f}) : nullptr;
    UE_LOGI("[GAP2] [H] a shrimp prop stood %p (class %p) 200 m above the host's player", shrimp, cls);
}

// Actor.WasRecentlyRendered(tolerance); false when it cannot be asked.
bool Rendered(void* actor, float tol) {
    static void* s_fn = nullptr;
    if (!s_fn)
        if (void* cls = R::FindClass(L"Actor")) s_fn = R::FindFunction(cls, L"WasRecentlyRendered");
    if (!s_fn) return false;
    ue_wrap::ParamFrame f(s_fn);
    return f.valid() && f.Set<float>(L"Tolerance", tol) && ue_wrap::Call(actor, f) && f.Get<bool>(L"ReturnValue");
}

// What the shrimp eater's `eat` reads (C:325549-325620): the Ariral reputation tier (calcRep: 0 to 3 may eat, 4 never), a shrimp in
// the gamemode's `shrimps`, the nearest one to `lastloc` warm enough (above 0 and below 60 C), the player 10000 cm or more from
// `lastloc`, the shrimp unseen for 5 s, and the day index above 10. Said on the host before each call; the one that holds is named.
void LogEatGates(int n, void* eater, const float* lastloc) {
    void* gm = ue_wrap::world_singleton::Gamemode();
    int rep = -1;
    if (void* h = [&]() -> void* {
            const int32_t o = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"ariralrephandler") : -1;
            return o >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(gm) + o) : nullptr; }()) {
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(h), L"calcRep");
        ue_wrap::ParamFrame f(fn);
        if (fn && f.valid() && ue_wrap::Call(h, f)) rep = f.Get<int32_t>(L"ReturnValue");
    }
    int count = 0;
    void* nearest = nullptr;
    float best = 1e30f;
    const int32_t so = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"shrimps") : -1;
    if (so >= 0) {
        void* const* data = *reinterpret_cast<void* const* const*>(static_cast<const uint8_t*>(gm) + so);
        count = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(gm) + so + 8);
        for (int i = 0; data && i < count; ++i) {
            ue_wrap::FVector at{};
            if (!data[i] || !R::IsLive(data[i]) || !E::TryGetActorLocation(data[i], at)) continue;
            const float d = (at.X - lastloc[0]) * (at.X - lastloc[0]) + (at.Y - lastloc[1]) * (at.Y - lastloc[1]) +
                            (at.Z - lastloc[2]) * (at.Z - lastloc[2]);
            if (d < best) { best = d; nearest = data[i]; }
        }
    }
    float celsius = -999.f;
    if (nearest) FloatProp(nearest, L"celsius", celsius);
    // Every registered shrimp, with the third reading the body takes (lib_C::isUnderwater).
    if (so >= 0) {
        void* const* data = *reinterpret_cast<void* const* const*>(static_cast<const uint8_t*>(gm) + so);
        void* libCdo = R::FindClassDefaultObject(L"lib_C");
        void* uwFn = libCdo ? R::FindDispatchFunctionCached(R::ClassOf(libCdo), L"isUnderwater") : nullptr;
        for (int i = 0; data && i < count; ++i) {
            if (!data[i] || !R::IsLive(data[i])) continue;
            bool uw = false, haveUw = false;
            ue_wrap::ParamFrame f(uwFn);
            if (uwFn && f.valid() && f.Set<void*>(L"itemToFind", data[i]) && f.Set<void*>(L"__WorldContext", gm) && ue_wrap::Call(libCdo, f)) {
                uw = f.Get<bool>(L"ReturnValue");
                haveUw = true;
            }
            float c = -999.f;
            FloatProp(data[i], L"celsius", c);
            ue_wrap::FVector at{};
            E::TryGetActorLocation(data[i], at);
            UE_LOGI("[GAP2] [H] eat #%d shrimp %p '%ls' at (%.0f, %.0f, %.0f): %.1f C, underwater %d (asked: %d), rendered within 5 s %d", n, data[i],
                    R::ClassNameOf(data[i]).c_str(), at.X, at.Y, at.Z, c, uw ? 1 : 0, haveUw ? 1 : 0, Rendered(data[i], 5.f) ? 1 : 0);
        }
    }
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::FVector pp{};
    const bool havePlayer = player && E::TryGetActorLocation(player, pp);
    const float far2 = (pp.X - lastloc[0]) * (pp.X - lastloc[0]) + (pp.Y - lastloc[1]) * (pp.Y - lastloc[1]) +
                       (pp.Z - lastloc[2]) * (pp.Z - lastloc[2]);
    void* cycle = Live(L"daynightCycle_C").empty() ? nullptr : Live(L"daynightCycle_C").front();
    const int32_t to = cycle ? R::FindPropertyOffset(R::ClassOf(cycle), L"timeZ") : -1;
    const int day = to >= 0 ? reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(cycle) + to)[2] : -1;
    const bool seen = nearest && Rendered(nearest, 5.f);
    UE_LOGI("[GAP2] [H] eat #%d gates: rep tier %d (%s), %d shrimp(s) registered (%s), nearest %p warm %.1f C (%s), the player %.0f cm from lastloc "
            "(%s), shrimp rendered within 5 s: %d (%s), day index %d (%s)", n, rep, rep >= 0 && rep < 4 ? "holds" : "BLOCKS", count,
            count > 0 ? "holds" : "BLOCKS", nearest, celsius, celsius > 0.f && celsius < 60.f ? "holds" : "BLOCKS",
            havePlayer ? std::sqrt(far2) : -1.f, havePlayer && far2 >= 1.0e8f ? "holds" : "BLOCKS", seen ? 1 : 0, seen ? "BLOCKS" : "holds",
            day, day > 10 ? "holds" : "BLOCKS");
}

size_t EmptyPacks() {
    size_t n = 0;
    for (void* a : Live(L"prop_C")) if (R::ClassNameOf(a) == L"prop_C" && ue_wrap::prop::GetPropNameString(a) == L"shrimp_e") ++n;
    return n;
}

// One call of the eater's `eat` with `lastloc` put far from every player and the day clock at day 15 for its length: written, called
// and restored, so no sample of either is sent. The gates are said first.
void FireEater(int n) {
    const std::vector<void*> eaters = Live(L"shrimpEaterMaster_C");
    if (eaters.empty()) { UE_LOGI("[GAP2] [H] eat #%d: the world holds no shrimpEaterMaster_C", n); return; }
    void* eater = eaters.front();
    const int32_t off = R::FindPropertyOffset(R::ClassOf(eater), L"lastloc");
    float saved[3] = {0, 0, 0};
    // 3D, as the eater picks the shrimp nearest to it: the level's own shrimp is frozen (-54 C, a gate), so the stood one must be nearer.
    const float far3[3] = {400000.f, 400000.f, 18600.f};
    if (off >= 0) {
        std::memcpy(saved, static_cast<uint8_t*>(eater) + off, sizeof(saved));
        std::memcpy(static_cast<uint8_t*>(eater) + off, far3, sizeof(far3));
    }
    LogEatGates(n, eater, far3);
    // The clock the eater reads is its gamemode's daynightcycle, not necessarily the first instance of the class found.
    void* gm = ue_wrap::world_singleton::Gamemode();
    const int32_t co = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"daynightcycle") : -1;
    void* cycle = co >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(gm) + co) : nullptr;
    const int32_t to = cycle ? R::FindPropertyOffset(R::ClassOf(cycle), L"timeZ") : -1;
    int32_t* clock = to >= 0 ? reinterpret_cast<int32_t*>(static_cast<uint8_t*>(cycle) + to) : nullptr;
    int32_t savedDay = 0;
    if (clock) { savedDay = clock[2]; clock[2] = 15; }
    const size_t before = Live(L"prop_food_shrimp_C").size(), packsBefore = EmptyPacks();
    const bool ran = Call0(eater, L"eat");
    if (clock) clock[2] = savedDay;
    float after[3] = {0, 0, 0};
    if (off >= 0) std::memcpy(after, static_cast<uint8_t*>(eater) + off, sizeof(after));
    const bool reached = off >= 0 && (after[0] != far3[0] || after[1] != far3[1] || after[2] != far3[2]);
    UE_LOGI("[GAP2] [H] eat #%d: lastloc after the call (%.0f, %.0f, %.0f): %s", n, after[0], after[1], after[2],
            reached ? "the body passed every gate and took the shrimp's place" : "the body returned at a gate");
    if (off >= 0) std::memcpy(static_cast<uint8_t*>(eater) + off, saved, sizeof(saved));
    UE_LOGI("[GAP2] [H] eat #%d: eat() ran: %d (day clock %d put at 15 and back); shrimp props before %zu, right after %zu; empty packs before %zu, "
            "right after %zu", n, ran ? 1 : 0, savedDay, before, Live(L"prop_food_shrimp_C").size(), packsBefore, EmptyPacks());
}

void HidePrints(const char* why) {
    for (void* a : Live(L"fprints_C")) {
        E::SetActorHiddenInGame(a, true);
        UE_LOGI("[GAP2] [H] %s: the footprints '%ls' written hidden", why, R::ToString(R::NameOf(a)).c_str());
    }
}

}  // namespace

bool IsDrill(const std::string& n) { return n == "gapDoor" || n == "gapWander" || n == "gapShrimp" || n == "gapPrints"; }

void ClientStep(const std::string& name) {
    const uint64_t now = coop::net::NowMs();
    if (!g_clientSeenMs) g_clientSeenMs = now;
    const uint64_t since = now - g_clientSeenMs;
    if (SayDue(since, g_lastSayClient)) Say("C", name, since);
    if (name == "gapDoor" && Once(1, since, 20000)) {
        // The client's own tp() must be refused (a roll of the 3600 s chain would move its copy); the call from here is that body.
        const std::vector<void*> doors = Live(L"TheDoor_C");
        // tp() has a return value and two outputs: a call with no frame would write through nothing.
        void* fn = doors.empty() ? nullptr : R::FindDispatchFunctionCached(R::ClassOf(doors.front()), L"tp");
        ue_wrap::ParamFrame f(fn);
        const bool ran = fn && f.valid() && ue_wrap::Call(doors.front(), f);
        UE_LOGI("[GAP2] [C] this peer called its door's tp(): %d (refused on a client: theDoor.tp counts it)", ran ? 1 : 0);
    }
    if (name == "gapPrints" && Once(0, since, 12000)) {
        const std::vector<void*> prints = Live(L"fprints_C");
        const bool ran = !prints.empty() && Call0(prints.front(), L"show");
        UE_LOGI("[GAP2] [C] this peer called its footprints show(): %d (it is refused here and asked of the host)", ran ? 1 : 0);
    }
}

void HostStep(const std::string& name, uint64_t since) {
    if (SayDue(since, g_lastSayHost)) Say("H", name, since);
    if (name == "gapDoor") {
        if (Once(0, since, 8000)) { for (void* d : Live(L"TheDoor_C")) UE_LOGI("[GAP2] [H] open() on '%ls': %d", R::ToString(R::NameOf(d)).c_str(), Call0(d, L"open") ? 1 : 0); }
        if (Once(1, since, 16000)) { for (void* d : Live(L"TheDoor_C")) UE_LOGI("[GAP2] [H] close() on '%ls': %d", R::ToString(R::NameOf(d)).c_str(), Call0(d, L"close") ? 1 : 0); }
        if (Once(2, since, 24000)) MoveBy(L"TheDoor_C", 1500.f, "the door moved as its tp() would");
        if (Once(3, since, 30000)) { for (void* d : Live(L"TheDoor_C")) UE_LOGI("[GAP2] [H] open() on '%ls': %d", R::ToString(R::NameOf(d)).c_str(), Call0(d, L"open") ? 1 : 0); }
        if (Once(4, since, 40000)) { for (void* d : Live(L"TheDoor_C")) UE_LOGI("[GAP2] [H] close() on '%ls': %d", R::ToString(R::NameOf(d)).c_str(), Call0(d, L"close") ? 1 : 0); }
    } else if (name == "gapWander") {
        if (Once(0, since, 8000)) MoveBy(L"rorGraf_C", 1500.f, "moved as its move() would");
        if (Once(1, since, 12000)) MoveBy(L"log_C", 1500.f, "moved as its teleport() would");
        if (Once(2, since, 16000)) MoveBy(L"prop_duc_C", 1500.f, "moved as its chain would");
        if (Once(3, since, 20000)) MoveBy(L"prop_mannequin_C", 1500.f, "moved as mannequinTp would");
        if (Once(4, since, 40000)) MoveBy(L"rorGraf_C", -1500.f, "moved back");
        if (Once(5, since, 44000)) MoveBy(L"log_C", -1500.f, "moved back");
        if (Once(6, since, 48000)) MoveBy(L"prop_duc_C", -1500.f, "moved back");
        if (Once(7, since, 52000)) MoveBy(L"prop_mannequin_C", -1500.f, "moved back");
    } else if (name == "gapShrimp") {
        if (Once(0, since, 6000)) StandShrimp();
        if (Once(1, since, 10000)) FireEater(1);
        if (Once(2, since, 20000)) FireEater(2);
        if (Once(3, since, 30000)) FireEater(3);
    } else if (name == "gapPrints") {
        // The footprints stand hidden at the start, so the client's use at its 12 s (its clock starts earlier than the host's fire)
        // is what shows them; the host's write at 30 s hides them again, and the client's copy must follow.
        if (Once(1, since, 30000)) HidePrints("after the client's use, to see the copy follow");
    }
}

void OnDisconnect() {
    g_clientSeenMs = g_lastSayHost = g_lastSayClient = 0;
    for (bool& b : g_done) b = false;
}

}  // namespace coop::dev::level_prop_drill
