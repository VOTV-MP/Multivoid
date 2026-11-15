// coop/dev/erie_drill.cpp -- see coop/dev/erie_drill.h.

#include "coop/dev/erie_drill.h"

#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"
#include "coop/world/erie_sync.h"
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
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

namespace coop::dev::erie_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
using Vec = ue_wrap::FVector;

constexpr const wchar_t* kPlushPath = L"/Game/objects/prop_eriePlush.prop_eriePlush_C";
constexpr const wchar_t* kPaperPath = L"/Game/objects/prop_notebook_paper.prop_notebook_paper_C";
constexpr const wchar_t* kPencilPath = L"/Game/objects/prop_pencil.prop_pencil_C";
constexpr const wchar_t* kLivePath = L"/Game/objects/eriePlushLive.eriePlushLive_C";
constexpr const wchar_t* kDragPath = L"/Game/objects/misc/erieDrag.erieDrag_C";
constexpr const wchar_t* kChopPath = L"/Game/objects/erieChop.erieChop_C";
constexpr const wchar_t* kHitFn =
    L"BndEvt__prop_eriePlush_StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature";

uint64_t g_clientSeenMs = 0, g_lastSayHost = 0, g_lastSayClient = 0, g_hitAt = 0, g_firstCopyAt = 0;
bool g_done[16] = {};
std::set<void*> g_preexisting;   // HOST: the Erie forms standing before the drill made any
void* g_plush = nullptr;
void* g_live = nullptr;

// This world's live instances of a class (the object index's walk, then the liveness and world tests).
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
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__") && ue_wrap::world_identity::WorldOf(obj) == world) out.push_back(obj);
    return out;
}

// Every live Erie of the three kinds, once each (the walk may list a subclass under its parent too).
std::vector<void*> LiveErie() {
    std::set<void*> seen;
    std::vector<void*> out;
    for (const wchar_t* c : {L"eriePlushLive_C", L"eriePlushLive_good_C", L"eriePlushLive_evil_C"})
        for (void* a : Live(c))
            if (seen.insert(a).second) out.push_back(a);
    return out;
}

std::vector<void*> LiveForms() {
    std::set<void*> seen;
    std::vector<void*> out = LiveErie();
    for (void* a : out) seen.insert(a);
    for (const wchar_t* c : {L"erieDrag_C", L"erieDrag_good_C", L"erieDrag_evil_C", L"erieChop_C"})
        for (void* a : Live(c))
            if (seen.insert(a).second) out.push_back(a);
    return out;
}

bool Once(int slot, uint64_t since, uint64_t atMs) {
    if (g_done[slot] || since < atMs) return false;
    g_done[slot] = true;
    return true;
}

bool SayDue(uint64_t since, uint64_t& last) {
    const uint64_t bucket = since / 5000 + 1;
    if (bucket <= last) return false;
    last = bucket;
    return true;
}

bool Bool(void* actor, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return actor && R::FindBoolProperty(R::ClassOf(actor), name, off, mask) && off >= 0 && (*(static_cast<const uint8_t*>(actor) + off) & mask) != 0;
}

float Float(void* actor, const wchar_t* name) {
    const int32_t off = actor ? R::FindPropertyOffset(R::ClassOf(actor), name) : -1;
    return off < 0 ? 0.f : *reinterpret_cast<const float*>(static_cast<const uint8_t*>(actor) + off);
}

void SetFloat(void* actor, const wchar_t* name, float v) {
    const int32_t off = actor ? R::FindPropertyOffset(R::ClassOf(actor), name) : -1;
    if (off >= 0) *reinterpret_cast<float*>(static_cast<uint8_t*>(actor) + off) = v;
}

unsigned Byte(void* actor, const wchar_t* name) {
    const int32_t off = actor ? R::FindPropertyOffset(R::ClassOf(actor), name) : -1;
    return off < 0 ? 0xFFu : *(static_cast<const uint8_t*>(actor) + off);
}

void SetByte(void* actor, const wchar_t* name, uint8_t v) {
    const int32_t off = actor ? R::FindPropertyOffset(R::ClassOf(actor), name) : -1;
    if (off >= 0) *(static_cast<uint8_t*>(actor) + off) = v;
}

std::wstring NameProp(void* actor, const wchar_t* name) {
    const int32_t off = actor ? R::FindPropertyOffset(R::ClassOf(actor), name) : -1;
    return off < 0 ? L"?" : R::ToString(*reinterpret_cast<const R::FName*>(static_cast<const uint8_t*>(actor) + off));
}

std::wstring KeyOf(void* a) {
    std::wstring k = ue_wrap::prop::GetInteractableKeyString(a);
    return k.empty() ? L"None" : k;
}

bool Pos(void* a, Vec& out) { return a && E::TryGetActorLocation(a, out); }

bool PawnPos(Vec& out) { return Pos(coop::players::Registry::Get().Local(), out); }

float Dist(const Vec& a, const Vec& b) {
    return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
}

// The instance of `cls` nearest `from`, within `within` cm.
void* Nearest(const wchar_t* cls, const Vec& from, float within) {
    void* best = nullptr;
    float bd = within;
    for (void* a : Live(cls)) {
        Vec p{};
        if (!Pos(a, p)) continue;
        const float d = Dist(p, from);
        if (d <= bd) { bd = d; best = a; }
    }
    return best;
}

void LoadAll(const char* who, std::initializer_list<const wchar_t*> paths) {
    int n = 0;
    for (const wchar_t* p : paths) n += ue_wrap::asset_load::LoadObjectByPath(p) ? 1 : 0;
    UE_LOGI("[GAP4] [%s] classes loaded: %d of %zu", who, n, paths.size());
}

void* SpawnAt(const wchar_t* path, const Vec& at) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(path);
    return cls ? E::SpawnActor(cls, at) : nullptr;
}

bool Call0(void* actor, const wchar_t* fnName) {
    void* fn = actor ? R::FindDispatchFunctionCached(R::ClassOf(actor), fnName) : nullptr;
    return fn && R::CallFunction(actor, fn, nullptr);
}

// ---- the quest ----

// The CLIENT's physical hit, as a reflected call of the plush copy's own handler with `other` as the actor it struck.
bool ClientHit(void* plush, void* other) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(plush), kHitFn);
    ue_wrap::ParamFrame f(fn);
    const bool ok = fn && f.valid() && f.Set<void*>(L"OtherActor", other);
    coop::erie_sync::SetDrillCallsCount(true);
    const bool ran = ok && ue_wrap::Call(plush, f);
    coop::erie_sync::SetDrillCallsCount(false);
    return ran;
}

void SayQuest(const char* who, uint64_t since) {
    Vec at{};
    for (void* p : Live(L"prop_eriePlush_C")) {
        Pos(p, at);
        UE_LOGI("[GAP4] [%s] t=%us: plush key '%ls' at (%.0f, %.0f, %.0f): paper %d pencil %d food '%ls' fullness %.2f", who, static_cast<unsigned>(since / 1000),
                KeyOf(p).c_str(), at.X, at.Y, at.Z, Bool(p, L"haspaper") ? 1 : 0, Bool(p, L"haspencil") ? 1 : 0, NameProp(p, L"food").c_str(), Float(p, L"fullness"));
    }
    UE_LOGI("[GAP4] [%s] t=%us: %zu plush(es), %zu paper(s), %zu pencil(s) held; hits refused %llu asked of the host %llu; lonetimer refused %llu, eatcheesetimer %llu, "
            "fuck %llu, anger %llu", who, static_cast<unsigned>(since / 1000), Live(L"prop_eriePlush_C").size(), Live(L"prop_notebook_paper_C").size(),
            Live(L"prop_pencil_C").size(), static_cast<unsigned long long>(coop::erie_sync::RefusedHits()),
            static_cast<unsigned long long>(coop::erie_sync::ForwardedHits()),
            static_cast<unsigned long long>(coop::spawn_authority::RefusalCount("eriePlush.lonetimer")),
            static_cast<unsigned long long>(coop::spawn_authority::RefusalCount("eriePlush.eatcheesetimer")),
            static_cast<unsigned long long>(coop::spawn_authority::RefusalCount("eriePlush.fuck")),
            static_cast<unsigned long long>(coop::spawn_authority::RefusalCount("eriePlush.anger")));
}

void HostQuest(uint64_t since) {
    Vec p{};
    if (!PawnPos(p)) return;
    if (Once(0, since, 6000)) {
        g_plush = SpawnAt(kPlushPath, {p.X + 120.f, p.Y, p.Z + 40.f});
        SetFloat(g_plush, L"fullness", -100.f);   // a food eaten by accident must not roll the morph
        UE_LOGI("[GAP4] [H] a plush stood %p, key '%ls', fullness written %.0f", g_plush, g_plush ? KeyOf(g_plush).c_str() : L"-", g_plush ? Float(g_plush, L"fullness") : 0.f);
    }
    if (Once(1, since, 8000)) {
        void* paper = SpawnAt(kPaperPath, {p.X + 100.f, p.Y + 130.f, p.Z + 40.f});
        void* pencil = SpawnAt(kPencilPath, {p.X + 100.f, p.Y - 130.f, p.Z + 40.f});
        UE_LOGI("[GAP4] [H] a paper stood %p and a pencil %p", paper, pencil);
    }
    if (Once(2, since, 85000)) {
        const bool live = g_plush && R::IsLive(g_plush);
        UE_LOGI("[GAP4] [H] the drill's plush ends (it stood: %d); %zu plush(es) are held", live ? 1 : 0, Live(L"prop_eriePlush_C").size());
        if (live) E::DestroyActor(g_plush);
        g_plush = nullptr;
    }
}

void ClientQuest(uint64_t since) {
    if (Once(0, since, 1500)) LoadAll("C", {kPlushPath, kPaperPath, kPencilPath});
    Vec p{};
    if (!PawnPos(p)) return;
    const bool wantPaper = !g_done[1] && since >= 18000;
    const bool wantPencil = g_done[1] && !g_done[2] && since >= g_hitAt + 12000;
    if (!wantPaper && !wantPencil) return;
    void* plush = Nearest(L"prop_eriePlush_C", p, 1500.f);
    if (!plush || KeyOf(plush) == L"None") return;
    Vec pp{};
    Pos(plush, pp);
    void* other = Nearest(wantPaper ? L"prop_notebook_paper_C" : L"prop_pencil_C", pp, 400.f);
    if (!other) return;
    g_done[wantPaper ? 1 : 2] = true;
    g_hitAt = since;
    const bool ran = ClientHit(plush, other);
    UE_LOGI("[GAP4] [C] this peer's copy of plush '%ls' was struck by its copy of the %s %p: handler call %d (refused here, asked of the host); copy's flags right after: paper %d pencil %d",
            KeyOf(plush).c_str(), wantPaper ? "paper" : "pencil", other, ran ? 1 : 0, Bool(plush, L"haspaper") ? 1 : 0, Bool(plush, L"haspencil") ? 1 : 0);
}

// ---- the forms ----

void SayForms(const char* who, uint64_t since) {
    size_t live = LiveErie().size();
    size_t drag = Live(L"erieDrag_C").size() + Live(L"erieDrag_good_C").size() + Live(L"erieDrag_evil_C").size();
    UE_LOGI("[GAP4] [%s] t=%us: %zu live Erie, %zu drag form(s), %zu chop(s), %zu angry; client refusals: erieDrag tick %llu, erieChop clothes %llu, erieHold begin play %llu", who,
            static_cast<unsigned>(since / 1000), live, drag, Live(L"erieChop_C").size(), Live(L"npc_angryErieFlesh_C").size(),
            static_cast<unsigned long long>(coop::spawn_authority::RefusalCount("erieDrag.ReceiveTick")),
            static_cast<unsigned long long>(coop::spawn_authority::RefusalCount("erieChop.anim_clothes_finished")),
            static_cast<unsigned long long>(coop::spawn_authority::RefusalCount("erieHold.ReceiveBeginPlay")));
    int n = 0;
    for (void* a : LiveErie()) {
        Vec at{};
        Pos(a, at);
        if (++n > 4) break;
        UE_LOGI("[GAP4] [%s] t=%us:   live Erie %p class %ls at (%.0f, %.0f, %.0f), rep %.2f state %u", who, static_cast<unsigned>(since / 1000), a, R::ClassNameOf(a).c_str(), at.X,
                at.Y, at.Z, Float(a, L"rep"), Byte(a, L"state"));
    }
}

void HostForms(uint64_t since) {
    if (Once(0, since, 3000)) LoadAll("H", {kLivePath, kDragPath, kChopPath});
    Vec p{};
    if (!PawnPos(p)) return;
    if (Once(1, since, 7000)) {
        for (void* a : LiveForms()) g_preexisting.insert(a);
        UE_LOGI("[GAP4] [H] %zu Erie form(s) stood before the drill made any", g_preexisting.size());
    }
    if (Once(2, since, 8000)) {
        g_live = SpawnAt(kLivePath, {p.X + 200.f, p.Y, p.Z + 60.f});
        SetByte(g_live, L"state", 2);   // not 0 or 1: no wandering, no stealing
        UE_LOGI("[GAP4] [H] a live Erie stood %p (state written 2)", g_live);
    }
    if (Once(3, since, 14000)) {
        void* drag = SpawnAt(kDragPath, {p.X - 150.f, p.Y, p.Z + 200.f});
        UE_LOGI("[GAP4] [H] an erieDrag_C stood %p, to fall and land as a live Erie by its own graph", drag);
    }
    if (Once(4, since, 25000) && g_live && R::IsLive(g_live)) {
        Vec at{};
        Pos(g_live, at);
        UE_LOGI("[GAP4] [H] the live Erie is moved 250 cm: %d", E::SetActorLocation(g_live, {at.X + 250.f, at.Y, at.Z}) ? 1 : 0);
    }
    if (Once(5, since, 32000)) {
        void* chop = SpawnAt(kChopPath, {p.X, p.Y - 250.f, p.Z + 60.f});
        UE_LOGI("[GAP4] [H] an erieChop_C stood %p, to end in two live Erie", chop);
    }
    if (Once(6, since, 80000)) {
        size_t n = 0;
        for (void* a : LiveForms())
            if (!g_preexisting.count(a) && E::DestroyActor(a)) ++n;
        UE_LOGI("[GAP4] [H] the drill's Erie forms end: %zu destroyed", n);
    }
}

void ClientForms(uint64_t since) {
    if (Once(0, since, 1500)) LoadAll("C", {kLivePath, kDragPath, kChopPath});
    Vec p{};
    if (!PawnPos(p)) return;
    // The client's clock starts when its drill step first runs, long before the host's fires: its steps wait for the first copy of a live Erie.
    if (!g_firstCopyAt && !LiveErie().empty()) g_firstCopyAt = since;
    if (!g_firstCopyAt) return;
    // A cancelled spawn makes the engine log an ERROR line (which the runner counts), so the try runs only when asked for (VOTVCOOP_ERIE_DRILL_MINT=1).
    if (Once(1, since, g_firstCopyAt + 2000) && std::getenv("VOTVCOOP_ERIE_DRILL_MINT")) {
        const size_t before = LiveErie().size();
        void* cls = ue_wrap::asset_load::LoadObjectByPath(kLivePath);
        void* mine = cls ? E::SpawnActor(cls, {p.X, p.Y, p.Z + 300.f}) : nullptr;
        UE_LOGI("[GAP4] [C] this peer's own live Erie spawn returned %p (a client's mint is cancelled at the spawn); live Erie held %zu -> %zu", mine, before, LiveErie().size());
    }
    if (Once(2, since, g_firstCopyAt + 4000)) {
        void* copy = nullptr;
        float bd = 4000.f;
        for (void* a : LiveErie()) {
            Vec at{};
            if (Pos(a, at) && Dist(at, p) < bd) { bd = Dist(at, p); copy = a; }
        }
        const float before = Float(copy, L"rep");
        const bool ran = Call0(copy, L"reducerep");
        UE_LOGI("[GAP4] [C] this peer's copy %p ran reducerep: %d (its graph is parked: a run would move rep); rep %.2f -> %.2f", copy, ran ? 1 : 0, before, Float(copy, L"rep"));
    }
}

}  // namespace

bool IsDrill(const std::string& n) { return n == "erieQuest" || n == "erieForms"; }

void ClientStep(const std::string& name) {
    const uint64_t now = coop::net::NowMs();
    if (!g_clientSeenMs) g_clientSeenMs = now;
    const uint64_t since = now - g_clientSeenMs;
    if (SayDue(since, g_lastSayClient)) {
        if (name == "erieQuest") SayQuest("C", since);
        if (name == "erieForms") SayForms("C", since);
    }
    // Object walks at most once a second, never per frame.
    static uint64_t s_lastWork = 0;
    if (now - s_lastWork < 1000) return;
    s_lastWork = now;
    if (name == "erieQuest") ClientQuest(since);
    if (name == "erieForms") ClientForms(since);
}

void HostStep(const std::string& name, uint64_t since) {
    if (SayDue(since, g_lastSayHost)) {
        if (name == "erieQuest") SayQuest("H", since);
        if (name == "erieForms") SayForms("H", since);
    }
    static uint64_t s_lastWork = 0;
    if (since < s_lastWork || since - s_lastWork < 1000) return;
    s_lastWork = since;
    if (name == "erieQuest") HostQuest(since);
    if (name == "erieForms") HostForms(since);
}

void OnDisconnect() {
    g_clientSeenMs = g_lastSayHost = g_lastSayClient = g_hitAt = g_firstCopyAt = 0;
    for (bool& b : g_done) b = false;
    g_preexisting.clear();
    g_plush = g_live = nullptr;
}

}  // namespace coop::dev::erie_drill
