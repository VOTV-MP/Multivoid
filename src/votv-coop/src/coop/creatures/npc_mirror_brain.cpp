// coop/creatures/npc_mirror_brain.cpp -- see coop/creatures/npc_mirror_brain.h. The mark, the watches, the death
// seam's end of a mirror and the dispatch to a class's presentation setup; the tables of allowed entries are
// npc_mirror_brain_table.cpp.

#include "coop/creatures/npc_mirror_brain.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/death_seam.h"
#include "coop/element/element.h"

#include "ue_wrap/actors/fossilhound.h"
#include "ue_wrap/actors/zombie.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/obj_keyed_cache.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/facts/function_row.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coop::npc_mirror_brain {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;

// One row per class that OWNS an event graph. A class below one of these with no graph of its own runs its parent's
// graph, which that parent's row matches (the gate compares the class that owns the function).
UE_FACT_FN(kRowZombie, "npc_mirror_brain", L"npc_zombie_C", L"ExecuteUbergraph_npc_zombie", Declared);
UE_FACT_FN(kRowSummoner, "npc_mirror_brain", L"npc_zombie_alienSummoner_C",
           L"ExecuteUbergraph_npc_zombie_alienSummoner", Declared);
UE_FACT_FN(kRowCorpse, "npc_mirror_brain", L"npc_zombie_corpse_C", L"ExecuteUbergraph_npc_zombie_corpse", Declared);
UE_FACT_FN(kRowSkerfuro, "npc_mirror_brain", L"npc_zombie_skerfuro_C", L"ExecuteUbergraph_npc_zombie_skerfuro",
           Declared);
UE_FACT_FN(kRowHound, "npc_mirror_brain", L"fossilhound_C", L"ExecuteUbergraph_fossilhound", Declared);
UE_FACT_FN(kRowGeomOcta, "npc_mirror_brain", L"geomOcta_C", L"ExecuteUbergraph_geomOcta", Declared);

constexpr int kTagZombie   = 0x4D505A30;  // 'MPZ0'
constexpr int kTagSummoner = 0x4D505A41;  // 'MPZA'
constexpr int kTagCorpse   = 0x4D505A43;  // 'MPZC'
constexpr int kTagSkerfuro = 0x4D505A53;  // 'MPZS'
constexpr int kTagHound    = 0x4D504630;  // 'MPF0'
constexpr int kTagGeomOcta = 0x4D504730;  // 'MPG0'

// ---- The mark ----

// A fixed open-addressed table, linear probing, no tombstones (a removal shifts the chain back). A slot's `index` is
// the actor's object index (-1 empty) and `serial` the slot's serial when it was marked. A mark of a dead world matches
// nothing, its slot holding another serial, so no world change empties the table: a live mirror stays marked.
constexpr size_t kMarkSlots = 256;  // a power of two: Home takes the hash's top eight bits
struct Mark {
    int32_t index  = -1;
    int32_t serial = 0;
};
Mark g_marks[kMarkSlots];

size_t Home(int32_t index) { return (static_cast<uint32_t>(index) * 2654435761u) >> 24; }

void ClearMarks() {
    for (Mark& m : g_marks) m = Mark{};
}

Mark* Find(int32_t index) {
    if (index < 0) return nullptr;
    size_t i = Home(index);
    for (size_t n = 0; n < kMarkSlots; ++n, i = (i + 1) & (kMarkSlots - 1)) {
        if (g_marks[i].index == index) return &g_marks[i];
        if (g_marks[i].index < 0) return nullptr;
    }
    return nullptr;
}

// False when every slot holds another actor.
bool Insert(int32_t index, int32_t serial) {
    size_t i = Home(index);
    for (size_t n = 0; n < kMarkSlots; ++n, i = (i + 1) & (kMarkSlots - 1)) {
        Mark& m = g_marks[i];
        if (m.index == index) {  // a recycled slot's old mark
            m.serial = serial;
            return true;
        }
        if (m.index < 0) {
            m = Mark{index, serial};
            return true;
        }
    }
    return false;
}

// Empty the slot `hole` and move back each later entry of the chain whose home lies at or before the hole.
void EraseAt(size_t hole) {
    constexpr size_t kMask = kMarkSlots - 1;
    size_t j = hole;
    for (size_t n = 1; n < kMarkSlots; ++n) {
        j = (j + 1) & kMask;
        if (g_marks[j].index < 0) break;
        const size_t home = Home(g_marks[j].index);
        if (((j - home) & kMask) >= ((j - hole) & kMask)) {
            g_marks[hole] = g_marks[j];
            hole = j;
        }
    }
    g_marks[hole] = Mark{};
}

// Drop every mark whose slot no longer holds its serial.
void DropStale() {
    Mark live[kMarkSlots];
    size_t kept = 0;
    for (const Mark& m : g_marks)
        if (m.index >= 0 && R::SlotSerial(m.index) == m.serial) live[kept++] = m;
    ClearMarks();
    for (size_t i = 0; i < kept; ++i) Insert(live[i].index, live[i].serial);
}

// ---- The watches ----

struct Watch {
    const ue_wrap::facts::Function* row;
    int                             tag;
    R::ParamOffset                  entryParam;  // EntryPoint in this graph's frame
    bool                            registered = false;
    bool                            liveSeen = false;    // the gate has the name resolved and the watch enabled
    bool                            liveWarned = false;
    bool                            tableResolved = false;
    const EntryTable*               table = nullptr;
    const EntryTable*               classTable = nullptr;  // the table of the row's own class, found once
};
Watch g_watches[] = {
    {&kRowZombie, kTagZombie, R::ParamOffset{L"EntryPoint"}},
    {&kRowSummoner, kTagSummoner, R::ParamOffset{L"EntryPoint"}},
    {&kRowCorpse, kTagCorpse, R::ParamOffset{L"EntryPoint"}},
    {&kRowSkerfuro, kTagSkerfuro, R::ParamOffset{L"EntryPoint"}},
    {&kRowHound, kTagHound, R::ParamOffset{L"EntryPoint"}},
    {&kRowGeomOcta, kTagGeomOcta, R::ParamOffset{L"EntryPoint"}},
};
constexpr size_t kWatches = sizeof(g_watches) / sizeof(g_watches[0]);

Watch* WatchOf(int tag) {
    for (Watch& w : g_watches)
        if (w.tag == tag) return &w;
    return nullptr;
}

// Is `cls`, or a class above it, the class the watch's row names? Told by FName index through the class's table.
bool ChainHasWatchClass(void* cls, Watch& w) {
    if (!w.classTable) w.classTable = TableNamed(w.row->Class());
    return w.classTable && ChainHasTable(cls, *w.classTable);
}

// What the watches refused: each (graph, entry) pair counted, and said once.
struct Refusal {
    size_t   watch;
    int32_t  entry;
    uint32_t count;
};
constexpr size_t kMaxRefusals = 64;
Refusal  g_refusals[kMaxRefusals];
size_t   g_refusalCount = 0;
uint32_t g_refusalsUnlisted = 0;
bool     g_unlistedSaid = false;

void NoteRefused(const Watch& w, int32_t entry) {
    const size_t which = static_cast<size_t>(&w - g_watches);
    for (size_t i = 0; i < g_refusalCount; ++i) {
        if (g_refusals[i].watch == which && g_refusals[i].entry == entry) {
            ++g_refusals[i].count;
            return;
        }
    }
    if (g_refusalCount < kMaxRefusals) {
        g_refusals[g_refusalCount++] = Refusal{which, entry, 1};
        UE_LOGI("npc_mirror_brain: %ls mirror entry %d refused", w.row->Class(), entry);
        return;
    }
    ++g_refusalsUnlisted;
    if (!g_unlistedSaid) {
        g_unlistedSaid = true;
        UE_LOGW("npc_mirror_brain: more than %zu distinct refused entries -- the rest are counted, not listed",
                kMaxRefusals);
    }
}

int32_t EntryOf(Watch& w, const sg::Call& call) {
    const int32_t off = w.entryParam.Of(call.function);
    int32_t entry = -1;
    if (off >= 0 && call.locals) std::memcpy(&entry, call.locals + off, sizeof(entry));
    return entry;
}

bool Allowed(const EntryTable& t, int32_t entry) {
    for (size_t i = 0; i < t.count; ++i)
        if (t.allowed[i] == entry) return true;
    return false;
}

// ---- The one-shot pass (ScopedEntryPass) ----

constexpr size_t kPassEntries = 8;
struct Pass {
    bool           open = false;
    void*          actor = nullptr;
    int32_t        entries[kPassEntries] = {};
    bool           used[kPassEntries] = {};
    size_t         count = 0;
    const uint8_t* frame = nullptr;
};
thread_local Pass t_pass;

// Does the open pass let this entry of `actor`'s graph run? Each named entry runs once, and the frame it ran with is kept.
bool PassTakes(void* actor, int32_t entry, const uint8_t* locals) {
    Pass& p = t_pass;
    if (!p.open || p.actor != actor) return false;
    for (size_t i = 0; i < p.count; ++i) {
        if (p.entries[i] == entry && !p.used[i]) {
            p.used[i] = true;
            p.frame = locals;
            return true;
        }
    }
    return false;
}

// Before a parked class's graph runs: a marked mirror's entry runs when its graph's table names it, and is refused
// otherwise. No session test: a mirror world can outlive its session, and the mark alone says what the body is.
sg::Verdict OnGraphPre(const sg::Call& call) {
    if (!IsMarkedMirror(call.object)) return sg::Verdict::Run;
    Watch* w = WatchOf(call.tag);
    if (!w) return sg::Verdict::Run;
    if (t_pass.open && PassTakes(call.object, EntryOf(*w, call), call.locals)) return sg::Verdict::Run;
    if (!w->tableResolved) {
        // A table not found before the class names resolve is not latched: the next call asks again.
        const EntryTable* found = TableFor(R::OuterOf(call.function));
        if (found || ResolveClassNames()) {
            w->tableResolved = true;
            w->table = found;
            if (!w->table)
                UE_LOGE("npc_mirror_brain: %ls has a watch and no table -- every entry of its mirrors is refused",
                        w->row->Class());
        }
    }
    const int32_t entry = EntryOf(*w, call);
    if (w->table && Allowed(*w->table, entry)) return sg::Verdict::Run;
    NoteRefused(*w, entry);
    return sg::Verdict::Cancel;
}

// ---- The death seam ----

bool g_subscribed = false;
bool g_announced = false;
int  g_subscribeRetry = 0;
bool g_subscribeWarned = false;
bool g_notLiveSaid = false;                         // a mirror that was not live has been said
uint32_t g_adoptedKickSkips = 0;                    // the per-viewer kicks left out for a twin whose BeginPlay ran
ue_wrap::ObjKeyedCache<bool, 16> g_namesPendingSaid;   // the classes born before the parked names resolved, said

// An Npc mirror's end, after the engine's own: its mark goes by the record's own slot and serial.
void OnNpcEnd(const EL::death_seam::Death& death) {
    UE_ASSERT_GAME_THREAD("npc_mirror_brain::OnNpcEnd");
    if (death.mirror) UnmarkMirror(death.actorIndex, death.actorSerial);
}

// The rows whose class `cls` is or derives from must have their name resolved and enabled by the time a mirror is
// born: a Declared watch resolves once its class has loaded, and the first spawn is what loads it. The gate is asked
// to resolve now; a watch seen live is never asked again (a watch matches every incarnation of its class).
void CheckWatchesLive(void* cls) {
    bool unproven = false;
    for (Watch& w : g_watches)
        if (!w.liveSeen && ChainHasWatchClass(cls, w)) unproven = true;
    if (!unproven) return;
    sg::ResolvePendingNames();
    for (Watch& w : g_watches) {
        if (w.liveSeen || !ChainHasWatchClass(cls, w)) continue;
        if (sg::NameWatchSettled(*w.row, w.tag) && sg::NameWatchLive(*w.row, w.tag)) {
            w.liveSeen = true;
        } else if (!w.liveWarned) {
            w.liveWarned = true;
            UE_LOGW("npc_mirror_brain: %ls watch not live at a mirror's birth -- its BeginPlay runs", w.row->Class());
        }
    }
}

}  // namespace

ScopedEntryPass::ScopedEntryPass(void* actor, std::initializer_list<int32_t> entries) {
    UE_ASSERT_GAME_THREAD("npc_mirror_brain::ScopedEntryPass");
    if (t_pass.open || entries.size() > kPassEntries) {
        UE_LOGE("npc_mirror_brain: a ScopedEntryPass was %s -- inert, its entries stay refused",
                t_pass.open ? "opened inside another" : "given too many entries");
        return;
    }
    t_pass = Pass{};
    t_pass.open = true;
    t_pass.actor = actor;
    for (int32_t e : entries) t_pass.entries[t_pass.count++] = e;
    owns_ = true;
}

ScopedEntryPass::~ScopedEntryPass() {
    if (owns_) t_pass = Pass{};
}

const uint8_t* ScopedEntryPass::Frame() { return t_pass.open ? t_pass.frame : nullptr; }

void MarkMirror(void* actor) {
    UE_ASSERT_GAME_THREAD("npc_mirror_brain::MarkMirror");
    if (!actor) return;
    if (!R::IsLive(actor)) {
        // No class is read of an actor that is not live, so the line is once, not once per class.
        if (!g_notLiveSaid) {
            g_notLiveSaid = true;
            UE_LOGW("npc_mirror_brain: a mirror was not marked -- it is not live; its Blueprint runs");
        }
        return;
    }
    const int32_t index = R::InternalIndexOf(actor);
    const int32_t serial = index >= 0 ? R::AllocateSlotSerial(index) : 0;
    if (serial == 0) {
        UE_LOGW("npc_mirror_brain: %ls mirror not marked -- its slot gave no serial; its Blueprint runs",
                R::ClassNameOf(actor).c_str());
        return;
    }
    if (Insert(index, serial)) return;
    DropStale();
    if (Insert(index, serial)) return;
    UE_LOGW("npc_mirror_brain: %ls mirror not marked -- the table is full; its Blueprint runs",
            R::ClassNameOf(actor).c_str());
}

bool IsMarkedMirror(void* obj) {
    if (!obj) return false;
    const int32_t index = R::InternalIndexOf(obj);
    const Mark* m = Find(index);
    return m && R::SlotSerial(index) == m->serial;
}

void UnmarkMirror(int32_t index, int32_t serial) {
    UE_ASSERT_GAME_THREAD("npc_mirror_brain::UnmarkMirror");
    Mark* m = Find(index);
    if (m && m->serial == serial) EraseAt(static_cast<size_t>(m - g_marks));
}

bool IsParkedClass(void* cls) {
    int hops = 0;
    for (; cls && hops < 16; cls = R::SuperStructOf(cls), ++hops)
        if (TableFor(cls)) return true;
    return false;
}

void PrepareBirth(void* actor) {
    UE_ASSERT_GAME_THREAD("npc_mirror_brain::PrepareBirth");
    void* const cls = actor ? R::ClassOf(actor) : nullptr;
    // The table's class names first: before they resolve no class reads as parked, so the mirror stays unmarked and
    // its Blueprint runs (fail open), which is said once per class.
    if (!ResolveClassNames()) {
        if (cls && !g_namesPendingSaid.Find(cls)) {
            g_namesPendingSaid.Insert(cls, true);
            UE_LOGW("npc_mirror_brain: %ls born before the parked class names resolved -- not marked, its Blueprint "
                    "runs", R::ClassNameOf(actor).c_str());
        }
        return;
    }
    if (!IsParkedClass(cls)) return;
    MarkMirror(actor);
    CheckWatchesLive(cls);
}

void SetUpPresentation(void* actor, Origin origin) {
    UE_ASSERT_GAME_THREAD("npc_mirror_brain::SetUpPresentation");
    if (!actor || !R::IsLive(actor) || !IsMarkedMirror(actor)) return;
    void* const cls = R::ClassOf(actor);
    static const EntryTable* const zombieTable = TableNamed(P::name::NpcClass_Zombie);
    static const EntryTable* const houndTable = TableNamed(P::name::NpcClass_Fossilhound);
    static const EntryTable* const octaTable = TableNamed(P::name::NpcClass_GeomOcta);
    // The Octa's BeginPlay runs whole, so nothing is set up and nothing is said.
    if (octaTable && ChainHasTable(cls, *octaTable)) return;
    bool ok = true;
    if (zombieTable && ChainHasTable(cls, *zombieTable)) {
        ok = ue_wrap::zombie::ApplyBeginPlayPresentation(actor);
    } else if (houndTable && ChainHasTable(cls, *houndTable)) {
        // An adopted twin's own BeginPlay ran as the client's creature and already started the per-viewer loop.
        const bool ownBeginPlayRan = origin == Origin::Adopted;
        ok = ue_wrap::fossilhound::ApplyBeginPlayPresentation(actor, ownBeginPlayRan);
        if (ownBeginPlayRan) ++g_adoptedKickSkips;
    }
    UE_LOGI("npc_mirror_brain: %ls mirror brain off, presentation %s", R::ClassNameOf(actor).c_str(),
            ok ? "set up" : "incomplete");
}

uint32_t AdoptedKickSkips() { return g_adoptedKickSkips; }

void Install() {
    ResolveClassNames();   // a latch; the lookups also ask for it
    // [dev] octa_red=no_parking, the Octa drill's red arm: the Octa's graph watch is never registered, so its mirror
    // keeps every decision and the drill must see it move on its own.
    static const bool noOctaParking =
        coop::config::ResolveEnum(::coop::config_registry::rows::octa_red) == "no_parking";
    bool registered = true;
    for (Watch& w : g_watches) {
        if (noOctaParking && w.tag == kTagGeomOcta) {
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                UE_LOGW("npc_mirror_brain: [dev] octa_red=no_parking -- the Octa's graph watch is left out");
            }
            continue;
        }
        if (!w.registered) w.registered = sg::WatchName(*w.row, w.tag, &OnGraphPre, nullptr);
        registered = registered && w.registered;
    }
    // A refused Subscribe is said once, then retried quietly on every 120th call.
    if (!g_subscribed) {
        if (g_subscribeRetry > 0) {
            --g_subscribeRetry;
        } else if (EL::death_seam::Install()) {
            g_subscribed = EL::death_seam::Subscribe(EL::ElementType::Npc, &OnNpcEnd);
            if (!g_subscribed) {
                if (!g_subscribeWarned) {
                    g_subscribeWarned = true;
                    UE_LOGW("npc_mirror_brain: the death seam took no subscription for creatures -- retried");
                }
                g_subscribeRetry = 120;
            }
        }
    }
    if (registered && g_subscribed && !g_announced) {
        g_announced = true;
        UE_LOGI("npc_mirror_brain: %zu graph watches registered (resolved as their classes load) and every mirror's "
                "end is heard (the death seam)", kWatches);
    }
}

void OnDisconnect() {
    for (size_t i = 0; i < g_refusalCount; ++i) {
        const Refusal& r = g_refusals[i];
        UE_LOGI("npc_mirror_brain: %ls entry %d: %u calls refused over the session",
                g_watches[r.watch].row->Class(), r.entry, r.count);
    }
    if (g_refusalsUnlisted > 0)
        UE_LOGI("npc_mirror_brain: %u calls of entries past the list refused over the session", g_refusalsUnlisted);
    g_refusalCount = 0;
    g_refusalsUnlisted = 0;
    g_unlistedSaid = false;
    g_notLiveSaid = false;
    g_namesPendingSaid.Clear();
    ClearMarks();
}

}  // namespace coop::npc_mirror_brain
