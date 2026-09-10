// coop/dev/floppy_selftest_world.cpp -- see coop/dev/floppy_selftest_world.h.

#include "floppy_selftest_world.h"

#include "ue_wrap/actors/floppy_disc.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/floppy_slot.h"
#include "ue_wrap/devices/serverbox.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace coop::dev::floppy_selftest::world {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace SB = ue_wrap::serverbox;
namespace FS = ue_wrap::floppy_slot;
namespace FD = ue_wrap::floppy_disc;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A disc's slot type IS its class: the library keeps one class per colour and answers with the
// index of the disc's class in that list, so the base class -- which is in no list -- is typed -1,
// and a box that swallows one of those can never hand it back. Seeding the coloured classes means
// the type in the slot also says which disc is in there. The white one is the zip drive, which a
// server refuses outright.
struct DiscClass { const wchar_t* name; int32_t type; bool zip; };
const DiscClass kDiscClasses[] = {
    { L"prop_floppyDisc_R_C",  0, false },
    { L"prop_floppyDisc_G_C",  1, false },
    { L"prop_floppyDisc_Y_C",  2, false },
    { L"prop_floppyDisc_Bl_C", 3, false },
    { L"prop_floppyDisc_Wh_C", 4, true  },  // the zip drive, which a server refuses outright
    { L"prop_floppyDisc_B_C",  5, false },
    { L"prop_floppyDisc_O_C",  6, false },
};

// The seeder walks colours, and the zip drive is not one it may use: a disc seeded from it would
// make an episode measure the refusal instead of the transfer. Wraps, since there are more discs
// than colours.
const DiscClass& NonZipClass(int i) {
    constexpr int n = static_cast<int>(sizeof(kDiscClasses) / sizeof(kDiscClasses[0]));
    int seen = 0;
    for (int pass = 0; pass < 2 * n; ++pass) {
        const DiscClass& dc = kDiscClasses[pass % n];
        if (dc.zip) continue;
        if (seen++ == i % (n - 1)) return dc;
    }
    return kDiscClasses[0];
}

ue_wrap::CachedObjRef g_box[kTargets];
int                   g_boxSlot[kTargets] = { -1, -1, -1 };  // the servers[] index, for the log
std::wstring          g_boxName[kTargets];
std::wstring          g_discKey[kDiscs];

struct DiscRow {
    void*        actor = nullptr;
    std::wstring key;
    std::wstring cls;            // the class is the disc's slot type, so it belongs in the census
    int32_t      type = -1;      // as the library types it; -1 = a class it does not know at all
    bool         zip = false;    // the one class a server refuses rather than swallows
    int32_t      readWrites = -1;
    int32_t      rows = 0;
    bool insertable() const { return type >= 0 && !zip; }
};

const DiscClass* DiscClassOf(const std::wstring& cls) {
    for (const DiscClass& dc : kDiscClasses)
        if (cls == dc.name) return &dc;
    return nullptr;
}

// Every live disc actor of any disc class, sorted by key so both peers index the same disc with
// the same number. One object-array walk per call on the census period, with the class verdict
// cached so the descendant test runs once per distinct class instead of once per object.
std::unordered_map<void*, bool> g_discVerdict;  // UClass* -> is a disc; dropped with the world

void ResetClassVerdicts() { g_discVerdict.clear(); }

std::vector<DiscRow> DiscCensus() {
    auto& verdict = g_discVerdict;
    std::vector<DiscRow> out;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        auto it = verdict.find(cls);
        if (it == verdict.end()) it = verdict.emplace(cls, FD::IsDiscClass(cls)).first;
        if (!it->second) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // the class defaults
        if (!R::IsLive(obj)) continue;
        DiscRow row;
        row.actor = obj;
        row.key = PR::GetKeyString(obj);
        row.cls = R::ToString(R::NameOf(cls));
        if (const DiscClass* dc = DiscClassOf(row.cls)) { row.type = dc->type; row.zip = dc->zip; }
        FD::DiscContent c;
        if (FD::ReadDiscContent(obj, c)) {
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
        s += L"key='" + discs[i].key + L"' cls='" + discs[i].cls + L"' type=" +
             std::to_wstring(discs[i].type) + (discs[i].zip ? L" ZIP" : L"") +
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
        BoxSlot st{};
        if (!box || !ReadBoxSlot(box, st)) { s += L" GONE"; continue; }
        s += L" type=" + std::to_wstring(st.floppyType) + L" rw=" + std::to_wstring(st.readWrites) +
             L" rows=" + std::to_wstring(st.dataNum) + L" json=" + std::to_wstring(st.objectDataLen);
    }
    return s;
}

}  // namespace

void Census(const char* tag, bool isHost, uint64_t sinceMs) {
    const std::vector<DiscRow> discs = DiscCensus();
    UE_LOGI("floppy_selftest: CENSUS %s role=%s t=+%llus discs=%zu [%ls] boxes=[%ls]",
            tag, isHost ? "HOST" : "CLIENT",
            static_cast<unsigned long long>(sinceMs / 1000),
            discs.size(), DescribeDiscs(discs).c_str(), DescribeBoxes().c_str());
}

// ---- target resolution --------------------------------------------------------------------------

namespace {
// The first boxes in the gamemode's own order whose slot is empty: one rule, run on both peers. A
// busy slot refuses an insert, which is why the rule skips one, and every pick is logged with its
// index, label and location so a divergent pick is visible instead of silent.
constexpr uint64_t kResolveRetryMs = 2000;
constexpr int      kMaxResolvePasses = 15;  // ~30 s of world load, then say so and stop
uint64_t g_nextResolveMs = 0;
int      g_resolvePasses = 0;
bool     g_resolveLatchedOff = false;
}  // namespace

bool ResolveBoxes() {
    if (g_box[kTargets - 1].Raw()) return true;
    if (g_resolveLatchedOff) return false;
    const uint64_t now = NowMs();
    if (now < g_nextResolveMs) return false;   // a per-tick retry would allocate and re-read the
    g_nextResolveMs = now + kResolveRetryMs;   // whole box list for as long as the world says no
    if (!SB::EnsureResolved()) return false;
    std::vector<void*> servers;
    const size_t n = SB::ReadServers(servers);
    if (n == 0) return false;
    int taken = 0;
    for (size_t i = 0; i < servers.size() && taken < kTargets; ++i) {
        void* box = servers[i];
        BoxSlot st{};
        if (!box || !R::IsLive(box) || !ReadBoxSlot(box, st) || st.floppyType >= 0) continue;
        g_box[taken].Set(box);
        g_boxSlot[taken] = static_cast<int>(i);
        g_boxName[taken] = SB::ReadName(box);
        ++taken;
    }
    if (taken < kTargets) {
        for (int t = 0; t < kTargets; ++t) { g_box[t].Reset(); g_boxSlot[t] = -1; }
        if (++g_resolvePasses >= kMaxResolvePasses) {
            g_resolveLatchedOff = true;
            UE_LOGW("floppy_selftest: %d of %d empty-slot boxes among %zu servers after %d passes -- "
                    "NOT ARMED, and no episode below will run; empty three server slots by hand and "
                    "rerun", taken, kTargets, n, g_resolvePasses);
        } else {
            UE_LOGW("floppy_selftest: only %d of %d empty-slot boxes among %zu servers -- retrying "
                    "(pass %d of %d)", taken, kTargets, n, g_resolvePasses, kMaxResolvePasses);
        }
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
    int usable = 0;
    for (const DiscRow& d : discs) if (d.insertable()) ++usable;
    for (int i = usable; i < kDiscs; ++i) {
        const DiscClass& dc = NonZipClass(i);
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
        // No key verb here: the prop's own Init mints the Key inside the spawn, and the line below
        // is a plain field read of it.
        UE_LOGI("floppy_selftest: seeded disc %d class='%ls' (slot type %d) key='%ls'", i, dc.name,
                dc.type, PR::GetKeyString(disc).c_str());
    }
    discs = DiscCensus();
    int stamped = 0;
    for (const DiscRow& d : discs) {
        if (!d.insertable() || stamped >= kDiscs) continue;
        FD::DiscContent c;
        c.readWrites = kMarkerReadWrites + stamped;
        c.data.push_back(std::wstring(kMarker) + L"-" + std::to_wstring(stamped));
        const bool ok = FD::WriteDiscContent(d.actor, c);
        UE_LOGI("floppy_selftest: stamped disc %d key='%ls' cls='%ls' rw %d -> %d rows %d -> 1 "
                "write=%d (a disc the world already held keeps none of its own content)",
                stamped, d.key.c_str(), d.cls.c_str(), d.readWrites, c.readWrites, d.rows,
                ok ? 1 : 0);
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
        // Skip a class the library does not type -- a box swallows one of those and can never hand
        // it back -- and the zip drive, which a server refuses outright.
        if (!d.insertable() || taken >= kDiscs) continue;
        g_discKey[taken] = d.key;
        if (d.readWrites >= kMarkerReadWrites) ++marked;
        picked += (taken ? L", " : L"") + std::to_wstring(taken) + L"='" + d.key + L"' cls='" +
                  d.cls + L"'";
        ++taken;
    }
    UE_LOGI("floppy_selftest: DISCS role=%s of %zu in world, %d typed and named, %d of them "
            "marked: %ls", isHost ? "HOST" : "CLIENT", discs.size(), taken, marked, picked.c_str());
    if (taken < kDiscs)
        UE_LOGW("floppy_selftest: only %d of %d episodes have a disc the server can type -- the "
                "rest cannot fire, and their rows below say so", taken, kDiscs);
    else if (marked < kDiscs)
        UE_LOGW("floppy_selftest: %d of %d named discs carry no marker -- the episodes are about "
                "to use a disc this instrument did not prepare", kDiscs - marked, kDiscs);
}


bool ReadBoxSlot(void* box, BoxSlot& out) {
    FS::Scalars st{};
    FS::Content c;
    if (!FS::EnsureResolved(FS::DeviceKind::ServerBox)) return false;
    if (!FS::ReadScalars(FS::DeviceKind::ServerBox, box, st)) return false;
    FS::ReadContent(FS::DeviceKind::ServerBox, box, c);
    out.floppyType    = st.floppyType;
    out.readWrites    = st.readWrites;
    out.dataNum       = static_cast<int32_t>(c.data.size());
    out.objectDataLen = static_cast<int32_t>(c.objectData.size());
    return true;
}

void* Box(int index) {
    return (index >= 0 && index < kTargets) ? g_box[index].Get() : nullptr;
}

const std::wstring& BoxName(int index) {
    static const std::wstring kNone;
    return (index >= 0 && index < kTargets) ? g_boxName[index] : kNone;
}

const std::wstring& DiscKey(int index) {
    static const std::wstring kNone;
    return (index >= 0 && index < kDiscs) ? g_discKey[index] : kNone;
}

void Reset() {
    ResetClassVerdicts();
    g_nextResolveMs = 0;
    g_resolvePasses = 0;
    g_resolveLatchedOff = false;
    for (int t = 0; t < kTargets; ++t) {
        g_box[t].Reset();
        g_boxSlot[t] = -1;
        g_boxName[t].clear();
    }
    for (int d = 0; d < kDiscs; ++d) g_discKey[d].clear();
}

}  // namespace coop::dev::floppy_selftest::world
