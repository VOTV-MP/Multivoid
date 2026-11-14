// coop/gamefix/disc_eject_fix.cpp -- see coop/gamefix/disc_eject_fix.h.

#include "coop/gamefix/disc_eject_fix.h"

#include "coop/gamefix/body_guard.h"
#include "coop/net/net_clock.h"

#include "ue_wrap/actors/floppy_disc.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/devices/floppy_slot.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace coop::gamefix::disc_eject_fix {
namespace {

namespace sg = ue_wrap::script_gate;
namespace FS = ue_wrap::floppy_slot;
namespace FD = ue_wrap::floppy_disc;
namespace R  = ue_wrap::reflection;

UE_FACT_FN(kRowEject, "disc_eject_fix", L"laptop_C", L"ejectFloppy", Declared);
UE_FACT_FN(kRowEjectEnd, "disc_eject_fix", L"laptop_C", L"Timeline_0__FinishedFunc", Declared);
UE_FACT_FN(kRowFinishSpawn, "disc_eject_fix", L"GameplayStatics", L"FinishSpawningActor", Declared);
constexpr int kTag = 0x47584445;  // 'GXDE'

// laptop_c.timeline_0__finishedfunc on 0.10.0: the eject's end that restores the snapshot and stops.
BodyGuard g_guard{"disc_eject", 0x1b53abe0c710a109ull};

// The slide out takes about a second; a record older than this belongs to an eject that never ended.
constexpr uint64_t kRecordTtlMs = 15000;
constexpr int      kRecords = 4;

struct Record {
    void*                     laptop = nullptr;
    int32_t                   idx = -1;
    int32_t                   serial = 0;
    uint64_t                  atMs = 0;
    int32_t                   readWrites = -1;
    std::vector<std::wstring> rows;
};
Record g_records[kRecords];

std::atomic<bool>     g_enabled{true};
std::atomic<uint64_t> g_applied{0};
bool g_watchedEject = false;
bool g_watchedEnd = false;
bool g_spawnHooked = false;
R::InstanceOffset g_offProcess{L"floppyProcess"};

// The laptop whose eject's end is running, and the actor that body finished spawning.
void* g_open = nullptr;
void* g_spawned = nullptr;

bool Stands(const Record& r) {
    return r.laptop && R::IsLiveByIndex(r.laptop, r.idx) && R::SlotSerial(r.idx) == r.serial &&
           coop::net::NowMs() - r.atMs < kRecordTtlMs;
}

Record* RecordOf(void* laptop) {
    for (Record& r : g_records)
        if (r.laptop == laptop && Stands(r)) return &r;
    return nullptr;
}

Record& RecordFor(void* laptop) {
    Record* oldest = &g_records[0];
    for (Record& r : g_records) {
        if (r.laptop == laptop || !Stands(r)) return r;
        if (r.atMs < oldest->atMs) oldest = &r;
    }
    return *oldest;
}

// Inside the eject's end, between the spawn and the snapshot's restore. Reads only: the write
// waits for the body's end, after the restore.
void OnFinishSpawnPost(void* /*context*/, void* sourceObject, void* spawned) {
    if (g_open && sourceObject == g_open && spawned) g_spawned = spawned;
}

void EnsureSpawnHook() {
    if (g_spawnHooked) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowFinishSpawn);
    if (fn) g_spawnHooked = ue_wrap::ufunction_hook::InstallPostHook(fn, &OnFinishSpawnPost);
}

// The eject as it begins: the game is about to clear the working rows (they ride its own frame to
// the eject's end, where 0.10.0 no longer reads them). Kept only for an eject the game will run.
sg::Verdict OnEjectPre(const sg::Call& c) {
    if (!g_enabled.load(std::memory_order_relaxed) || !c.object) return sg::Verdict::Run;
    EnsureSpawnHook();
    FS::Scalars sc{};
    if (!FS::EnsureResolved(FS::DeviceKind::Laptop) || !FS::ReadScalars(FS::DeviceKind::Laptop, c.object, sc) ||
        sc.floppyType < 0)
        return sg::Verdict::Run;  // an empty slot: the game says so and returns
    const int32_t offBusy = g_offProcess.Of(c.object);
    if (offBusy >= 0 && *(reinterpret_cast<const uint8_t*>(c.object) + offBusy) != 0)
        return sg::Verdict::Run;  // a disc mid-slide: the game returns at once
    FS::Content content;
    if (!FS::ReadContent(FS::DeviceKind::Laptop, c.object, content)) return sg::Verdict::Run;
    Record& r = RecordFor(c.object);
    r.laptop = c.object;
    r.idx = R::InternalIndexOf(c.object);
    r.serial = R::AllocateSlotSerial(r.idx);
    r.atMs = coop::net::NowMs();
    r.readWrites = sc.readWrites;
    r.rows = std::move(content.data);
    return sg::Verdict::Run;
}

sg::Verdict OnEjectEndPre(const sg::Call& c) {
    g_open = nullptr;
    g_spawned = nullptr;
    if (!g_enabled.load(std::memory_order_relaxed) || !c.object || !g_guard.Holds(c.function))
        return sg::Verdict::Run;
    if (RecordOf(c.object)) g_open = c.object;
    return sg::Verdict::Run;
}

void OnEjectEndPost(const sg::Call& c) {
    void* laptop = g_open;
    void* disc = g_spawned;
    g_open = nullptr;
    g_spawned = nullptr;
    if (!laptop || laptop != c.object) return;
    Record* r = RecordOf(laptop);
    if (!r) return;
    FD::DiscContent content;
    content.readWrites = r->readWrites;
    content.data = std::move(r->rows);
    *r = Record{};
    if (!disc || !R::IsLive(disc) || !FD::EnsureResolved() || !FD::IsDiscClass(R::ClassOf(disc))) {
        UE_LOGW("gamefix[disc_eject]: the eject ended with no disc spawned (%p, hook %d) -- %zu row(s) not written",
                disc, g_spawnHooked ? 1 : 0, content.data.size());
        return;
    }
    const bool ok = FD::WriteDiscContent(disc, content);
    if (ok) g_applied.fetch_add(1, std::memory_order_relaxed);
    UE_LOGI("gamefix[disc_eject]: the ejected disc takes the PC's %zu row(s) and %d write(s) left (%s)",
            content.data.size(), content.readWrites, ok ? "written" : "WRITE FAILED");
}

}  // namespace

void Install() {
    if (!g_watchedEject) g_watchedEject = sg::WatchName(kRowEject, kTag, &OnEjectPre, nullptr);
    if (!g_watchedEnd) g_watchedEnd = sg::WatchName(kRowEjectEnd, kTag, &OnEjectEndPre, &OnEjectEndPost);
}

void SetEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }

uint64_t Applied() { return g_applied.load(std::memory_order_relaxed); }

}  // namespace coop::gamefix::disc_eject_fix
