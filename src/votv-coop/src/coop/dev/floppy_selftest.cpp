// coop/dev/floppy_selftest.cpp -- see coop/dev/floppy_selftest.h.

#include "coop/dev/floppy_selftest.h"

#include "coop/config/config.h"
#include "coop/net/session.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/laptop.h"
#include "ue_wrap/devices/serverbox.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::dev::floppy_selftest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace SB = ue_wrap::serverbox;
namespace LP = ue_wrap::laptop;  // the disc prop's own content fields live in the laptop wrapper

std::atomic<coop::net::Session*> g_session{nullptr};

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::floppy_selftest);
    return s;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The schedule, measured from the first tick at which this peer is connected. The gaps are wide
// enough that the two peers reaching that tick a second apart cannot reorder two episodes, and
// wider than the box's out-timeline, which is what hands an eject its disc.
constexpr uint64_t kSeedMs   =  8000;
constexpr uint64_t kPickMs   = 16000;
constexpr uint64_t kCensusMs =  6000;
constexpr uint64_t kPostMs   =  4000;   // the after-picture of one episode
constexpr uint64_t kFinalMs  = 98000;

constexpr int kTargets = 3;  // boxes, and discs: one of each per insert episode

// The marker a seeded disc carries in its own data array, so the peer that wrote the content can
// be told from one that only mirrors the actor.
constexpr const wchar_t* kMarker = L"MULTIVOID-FLOPPY-SELFTEST";
constexpr int32_t kMarkerReadWrites = 900;  // + the disc index; the class default is a round 32

// A disc's slot type IS its class: the library keeps one class per colour and answers with the
// index of the disc's class in that list, so the base class -- which is in no list -- is typed -1,
// and a box that swallows one of those can never hand it back. Seed the coloured classes instead,
// one per episode, so the type in the slot also says which disc is in there. The white one is the
// zip drive, which a server refuses outright, so it is not among them.
struct DiscClass { const wchar_t* name; int32_t type; };
const DiscClass kDiscClasses[] = {
    { L"prop_floppyDisc_R_C", 0 },
    { L"prop_floppyDisc_G_C", 1 },
    { L"prop_floppyDisc_Y_C", 2 },
};

enum class Verb { Insert, Eject };

struct Step {
    const char* id;
    uint64_t    atMs;
    bool        onHost;   // the role that fires it; the other role only watches
    int         box;
    int         disc;     // inserts only
    Verb        verb;
    const char* under_test;
};

// One row per episode half. `under_test` is printed with the outcome, so one log says whether what
// happened is the thing the episode was built to catch.
const Step kSteps[] = {
    { "E1-insert", 20000, false, 0,  0, Verb::Insert,
      "a client insert: the disc dies here and the destroy is what crosses" },
    { "E1-eject",  30000, true,  0, -1, Verb::Eject,
      "the host ejecting a client's insert: an empty slot means the transfer never crossed" },
    { "E2-insert", 42000, true,  1,  1, Verb::Insert,
      "a host insert: the slot state is authored on the host" },
    { "E2-eject",  52000, false, 1, -1, Verb::Eject,
      "the client ejecting a host insert: an empty slot means the slot state never crossed" },
    { "E2-reject", 62000, true,  1, -1, Verb::Eject,
      "the host ejecting its own insert: the disc and its content come back here" },
    { "E3-insert", 74000, false, 2,  2, Verb::Insert,
      "a client insert whose eject is the same client's" },
    { "E3-eject",  84000, false, 2, -1, Verb::Eject,
      "a client eject: the disc is born on the client, through its own place seam" },
};
constexpr size_t kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

struct Outcome {
    bool        done    = false;
    bool        fired   = false;  // the verb was dispatched
    std::string note;             // the refusal reason, or what the slot did
};
Outcome g_outcome[kStepCount];

uint64_t g_connectedAtMs = 0;
uint64_t g_nextCensusMs  = 0;
uint64_t g_postAtMs      = 0;
int      g_postStep      = -1;
bool     g_seeded        = false;
bool     g_picked        = false;
bool     g_finalDone     = false;

ue_wrap::CachedObjRef g_box[kTargets];
int                   g_boxSlot[kTargets] = { -1, -1, -1 };  // the servers[] index, for the log
std::wstring          g_boxName[kTargets];
std::wstring          g_discKey[kTargets];

// ---- the world's discs ---------------------------------------------------------------------------

struct DiscRow {
    void*        actor = nullptr;
    std::wstring key;
    std::wstring cls;              // the class is the disc's slot type, so it belongs in the census
    bool         typed = false;    // ... and a class the library types is the only insertable one
    int32_t      readWrites = -1;
    int32_t      rows = 0;
};

bool IsTypedDiscClass(const std::wstring& cls) {
    for (const DiscClass& dc : kDiscClasses)
        if (cls == dc.name) return true;
    return false;
}

// Every live disc actor of any disc class, sorted by key so both peers index the same disc with
// the same number. One object-array walk per call on the census period, with the class verdict
// cached so the descendant test runs once per distinct class instead of once per object.
std::vector<DiscRow> DiscCensus() {
    static std::unordered_map<void*, bool> verdict;
    std::vector<DiscRow> out;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        auto it = verdict.find(cls);
        if (it == verdict.end()) it = verdict.emplace(cls, LP::IsDiscClass(cls)).first;
        if (!it->second) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // the class defaults
        if (!R::IsLive(obj)) continue;
        DiscRow row;
        row.actor = obj;
        row.key = PR::GetKeyString(obj);
        row.cls = R::ToString(R::NameOf(cls));
        row.typed = IsTypedDiscClass(row.cls);
        LP::DiscContent c;
        if (LP::ReadDiscContent(obj, c)) {
            row.readWrites = c.readWrites;
            row.rows = static_cast<int32_t>(c.data.size());
        }
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end(),
              [](const DiscRow& a, const DiscRow& b) { return a.key < b.key; });
    return out;
}

std::wstring DescribeDiscs(const std::vector<DiscRow>& discs) {
    std::wstring s;
    for (size_t i = 0; i < discs.size() && i < 8; ++i) {
        if (!s.empty()) s += L"; ";
        s += L"key='" + discs[i].key + L"' cls='" + discs[i].cls + L"'" +
             (discs[i].typed ? L"" : L" UNTYPED") +
             L" rw=" + std::to_wstring(discs[i].readWrites) +
             L" rows=" + std::to_wstring(discs[i].rows);
    }
    if (discs.size() > 8) s += L"; ...";
    return s;
}

std::wstring DescribeBoxes() {
    std::wstring s;
    for (int t = 0; t < kTargets; ++t) {
        void* box = g_box[t].Get();
        if (!s.empty()) s += L"; ";
        s += std::to_wstring(t) + L" '" + g_boxName[t] + L"'";
        SB::SlotState st{};
        if (!box || !SB::ReadSlot(box, st)) { s += L" GONE"; continue; }
        s += L" type=" + std::to_wstring(st.floppyType) + L" rw=" + std::to_wstring(st.readWrites) +
             L" rows=" + std::to_wstring(st.dataNum) + L" json=" + std::to_wstring(st.objectDataLen);
    }
    return s;
}

void Census(const char* tag, bool isHost) {
    const std::vector<DiscRow> discs = DiscCensus();
    UE_LOGI("floppy_selftest: CENSUS %s role=%s t=+%llus discs=%zu [%ls] boxes=[%ls]",
            tag, isHost ? "HOST" : "CLIENT",
            static_cast<unsigned long long>((NowMs() - g_connectedAtMs) / 1000),
            discs.size(), DescribeDiscs(discs).c_str(), DescribeBoxes().c_str());
}

// ---- target resolution --------------------------------------------------------------------------

// The first boxes in the gamemode's own order whose slot is empty: one rule, run on both peers. A
// busy slot refuses an insert, which is why the rule skips one, and every pick is logged with its
// index, label and location so a divergent pick is visible instead of silent.
bool ResolveBoxes() {
    if (g_box[kTargets - 1].Raw()) return true;
    if (!SB::EnsureResolved()) return false;
    std::vector<void*> servers;
    const size_t n = SB::ReadServers(servers);
    if (n == 0) return false;
    int taken = 0;
    for (size_t i = 0; i < servers.size() && taken < kTargets; ++i) {
        void* box = servers[i];
        SB::SlotState st{};
        if (!box || !R::IsLive(box) || !SB::ReadSlot(box, st) || st.floppyType >= 0) continue;
        g_box[taken].Set(box);
        g_boxSlot[taken] = static_cast<int>(i);
        g_boxName[taken] = SB::ReadName(box);
        ++taken;
    }
    if (taken < kTargets) {
        UE_LOGW("floppy_selftest: only %d of %d empty-slot boxes among %zu servers -- not armed",
                taken, kTargets, n);
        for (int t = 0; t < kTargets; ++t) { g_box[t].Reset(); g_boxSlot[t] = -1; }
        return false;
    }
    for (int t = 0; t < kTargets; ++t) {
        const auto loc = E::GetActorLocation(g_box[t].Get());
        UE_LOGI("floppy_selftest: TARGET box %d = servers[%d] '%ls' at (%.0f,%.0f,%.0f) of %zu",
                t, g_boxSlot[t], g_boxName[t].c_str(), loc.X, loc.Y, loc.Z, n);
    }
    return true;
}

// HOST only, once: make sure the world holds enough discs and that each carries a payload a reader
// can attribute, so "the content is on this peer only" is a value in two logs rather than an
// inference. A spawned disc is keyed through the game's own getKey, which is what mints one.
void SeedAndStamp() {
    std::vector<DiscRow> discs = DiscCensus();
    int typed = 0;
    for (const DiscRow& d : discs) if (d.typed) ++typed;
    for (int i = typed; i < kTargets; ++i) {
        const DiscClass& dc = kDiscClasses[i % (sizeof(kDiscClasses) / sizeof(kDiscClasses[0]))];
        void* cls = R::FindClass(dc.name);
        void* anchor = g_box[i % kTargets].Get();
        if (!cls || !anchor) {
            UE_LOGW("floppy_selftest: disc seed %d NOT spawned (class '%ls'=%p anchor=%p)", i,
                    dc.name, cls, anchor);
            continue;
        }
        const auto base = E::GetActorLocation(anchor);
        void* disc = E::SpawnActor(cls, { base.X, base.Y + static_cast<float>(30 * i),
                                          base.Z + 80.f });
        if (!disc) { UE_LOGW("floppy_selftest: disc seed %d SPAWN FAILED", i); continue; }
        // No key verb here: reading the key through the wrapper leaves the disc keyed, which the
        // line below both uses and shows.
        UE_LOGI("floppy_selftest: seeded disc %d class='%ls' (slot type %d) key='%ls'", i, dc.name,
                dc.type, PR::GetKeyString(disc).c_str());
    }
    discs = DiscCensus();
    int stamped = 0;
    for (const DiscRow& d : discs) {
        if (!d.typed || stamped >= kTargets) continue;
        LP::DiscContent c;
        c.readWrites = kMarkerReadWrites + stamped;
        c.data.push_back(std::wstring(kMarker) + L"-" + std::to_wstring(stamped));
        const bool ok = LP::WriteDiscContent(d.actor, c);
        UE_LOGI("floppy_selftest: stamped disc %d key='%ls' cls='%ls' rw=%d rows=1 write=%d",
                stamped, d.key.c_str(), d.cls.c_str(), c.readWrites, ok ? 1 : 0);
        ++stamped;
    }
}

// Both peers name their three discs by the same rule -- the lowest three keys in the world -- and
// print them, so a set that is not shared shows up as two different lists rather than as an
// episode that quietly used another disc.
void PickDiscs(bool isHost) {
    const std::vector<DiscRow> discs = DiscCensus();
    std::wstring picked;
    int taken = 0, marked = 0;
    for (const DiscRow& d : discs) {
        if (!d.typed || taken >= kTargets) continue;   // an untyped disc is refused by the server
        g_discKey[taken] = d.key;
        if (d.readWrites >= kMarkerReadWrites) ++marked;
        picked += (taken ? L", " : L"") + std::to_wstring(taken) + L"='" + d.key + L"' cls='" +
                  d.cls + L"'";
        ++taken;
    }
    UE_LOGI("floppy_selftest: DISCS role=%s of %zu in world, %d typed and named, %d of them "
            "marked: %ls", isHost ? "HOST" : "CLIENT", discs.size(), taken, marked, picked.c_str());
    if (taken < kTargets)
        UE_LOGW("floppy_selftest: only %d of %d episodes have a disc the server can type -- the "
                "rest cannot fire, and their rows below say so", taken, kTargets);
    else if (marked < kTargets)
        UE_LOGW("floppy_selftest: %d of %d named discs carry no marker -- the episodes are about "
                "to use a disc this instrument did not prepare", kTargets - marked, kTargets);
}

// ---- the episodes ------------------------------------------------------------------------------

void Fire(size_t i) {
    const Step& s = kSteps[i];
    Outcome& o = g_outcome[i];
    o.done = true;
    void* box = g_box[s.box].Get();
    if (!box) {
        o.note = "the target box no longer resolves";
        UE_LOGW("floppy_selftest: %s NOT FIRED -- %s", s.id, o.note.c_str());
        return;
    }
    SB::SlotState before{};
    SB::ReadSlot(box, before);

    if (s.verb == Verb::Insert) {
        if (g_discKey[s.disc].empty()) {
            o.note = "no disc was named for this episode";
            UE_LOGW("floppy_selftest: %s NOT FIRED -- %s", s.id, o.note.c_str());
            return;
        }
        void* disc = PR::FindByKeyString(g_discKey[s.disc]);
        if (!disc) {
            o.note = "the named disc is not in this peer's world";
            UE_LOGW("floppy_selftest: %s NOT FIRED -- %s (key='%ls')", s.id, o.note.c_str(),
                    g_discKey[s.disc].c_str());
            return;
        }
        LP::DiscContent dc;
        LP::ReadDiscContent(disc, dc);
        const bool called = SB::CallProcessFloppy(box, disc);
        SB::SlotState after{};
        SB::ReadSlot(box, after);
        const bool discGone = !R::IsLive(disc);
        o.fired = called;
        UE_LOGI("floppy_selftest: %s %s box=%d '%ls' disc key='%ls' rw=%d rows=%zu -- slot type "
                "%d -> %d rw %d -> %d rows %d -> %d json %d -> %d, disc destroyed=%d (%s)",
                s.id, called ? "FIRED" : "CALL REFUSED", s.box, g_boxName[s.box].c_str(),
                g_discKey[s.disc].c_str(), dc.readWrites, dc.data.size(), before.floppyType,
                after.floppyType, before.readWrites, after.readWrites, before.dataNum,
                after.dataNum, before.objectDataLen, after.objectDataLen, discGone ? 1 : 0,
                s.under_test);
        const bool tookContent = after.objectDataLen > before.objectDataLen ||
                                 after.dataNum > before.dataNum ||
                                 after.readWrites != before.readWrites;
        if (after.floppyType >= 0) {
            o.note = "slot filled";
        } else if (tookContent) {
            o.note = "the slot took the content but no type";
            UE_LOGW("floppy_selftest: %s left the slot TYPELESS -- the box holds the disc's data "
                    "and its type is still %d, and every eject gates on a type, so this disc "
                    "cannot come back out on any peer", s.id, after.floppyType);
        } else {
            o.note = "the slot did not change";
            UE_LOGW("floppy_selftest: %s changed NOTHING in the slot -- the TRIGGER is inert (a "
                    "busy slot or a zip disc refuses the insert), so nothing downstream of it can "
                    "be read from this run", s.id);
        }
        return;
    }

    // Eject. An empty slot here is not a failure of the instrument: it is the outcome the episode
    // exists to record, and the game answers it with its own refusal hint.
    if (before.floppyType < 0) {
        o.fired = false;
        o.note = "the slot was EMPTY at eject time";
        UE_LOGW("floppy_selftest: %s SLOT EMPTY box=%d '%ls' -- nothing to eject, the game answers "
                "its no-disc hint (%s)", s.id, s.box, g_boxName[s.box].c_str(), s.under_test);
        return;
    }
    const bool called = SB::CallEjectFloppy(box);
    SB::SlotState after{};
    SB::ReadSlot(box, after);
    o.fired = called;
    o.note = "slot drained";
    UE_LOGI("floppy_selftest: %s %s box=%d '%ls' -- slot type %d -> %d rw %d -> %d rows %d -> %d "
            "json %d -> %d; the disc arrives on the out-timeline (%s)",
            s.id, called ? "FIRED" : "CALL REFUSED", s.box, g_boxName[s.box].c_str(),
            before.floppyType, after.floppyType, before.readWrites, after.readWrites,
            before.dataNum, after.dataNum, before.objectDataLen, after.objectDataLen,
            s.under_test);
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!Enabled()) return;
    g_session.store(session, std::memory_order_release);
}

void EmitVerdict() {
    if (!Enabled() || !g_connectedAtMs) return;
    auto* s = g_session.load(std::memory_order_acquire);
    const bool isHost = s && s->role() == coop::net::Role::Host;
    int fired = 0, refused = 0, notReached = 0;
    for (size_t i = 0; i < kStepCount; ++i) {
        const Outcome& o = g_outcome[i];
        const char* state = !o.done ? "NOT REACHED" : (o.fired ? "fired" : "did not fire");
        if (!o.done) ++notReached;
        else if (o.fired) ++fired;
        else ++refused;
        // Only the role that owns an episode can say anything about it; the other peer prints the
        // row as not its own rather than as a gap.
        const bool mine = kSteps[i].onHost == isHost;
        UE_LOGI("floppy_selftest: VERDICT %-10s %-12s %s%s", kSteps[i].id,
                mine ? state : "not this role", mine && !o.note.empty() ? "-- " : "",
                mine ? o.note.c_str() : "");
    }
    UE_LOGI("floppy_selftest: VERDICT role=%s fired=%d did-not-fire=%d not-reached=%d of %zu "
            "episodes (a not-reached row measured NOTHING)", isHost ? "HOST" : "CLIENT", fired,
            refused, notReached, kStepCount);
}

void Tick() {
    if (!Enabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const bool isHost = s->role() == coop::net::Role::Host;
    const uint64_t now = NowMs();
    if (!g_connectedAtMs) {
        g_connectedAtMs = now;
        g_nextCensusMs = now + kCensusMs;
        UE_LOGI("floppy_selftest: ARMED role=%s (seed +%llus, discs named +%llus, first episode "
                "+%llus)", isHost ? "HOST" : "CLIENT",
                static_cast<unsigned long long>(kSeedMs / 1000),
                static_cast<unsigned long long>(kPickMs / 1000),
                static_cast<unsigned long long>(kSteps[0].atMs / 1000));
    }
    const uint64_t since = now - g_connectedAtMs;
    if (!LP::EnsureResolved()) return;  // the disc fields come from there
    if (!ResolveBoxes()) return;

    if (!g_seeded && since >= kSeedMs) {
        g_seeded = true;
        if (isHost) SeedAndStamp();
        Census("seeded", isHost);
    }
    if (g_seeded && !g_picked && since >= kPickMs) {
        g_picked = true;
        PickDiscs(isHost);
    }
    if (g_picked) {
        for (size_t i = 0; i < kStepCount; ++i) {
            if (g_outcome[i].done || kSteps[i].onHost != isHost || since < kSteps[i].atMs) continue;
            Census("before", isHost);
            Fire(i);
            g_postAtMs = now + kPostMs;
            g_postStep = static_cast<int>(i);
            break;  // one verb per tick, so two episodes can never share an after-picture
        }
    }
    if (g_postStep >= 0 && now >= g_postAtMs) {
        Census(kSteps[g_postStep].id, isHost);
        g_postStep = -1;
    }
    if (now >= g_nextCensusMs) {
        g_nextCensusMs = now + kCensusMs;
        Census("tick", isHost);
    }
    if (!g_finalDone && since >= kFinalMs) {
        g_finalDone = true;
        Census("final", isHost);
        EmitVerdict();
    }
}

void OnDisconnect() {
    if (!Enabled()) return;
    g_connectedAtMs = 0;
    g_nextCensusMs = 0;
    g_postAtMs = 0;
    g_postStep = -1;
    g_seeded = g_picked = g_finalDone = false;
    for (size_t i = 0; i < kStepCount; ++i) g_outcome[i] = Outcome{};
    for (int t = 0; t < kTargets; ++t) {
        g_box[t].Reset();
        g_boxSlot[t] = -1;
        g_boxName[t].clear();
        g_discKey[t].clear();
    }
}

}  // namespace coop::dev::floppy_selftest
