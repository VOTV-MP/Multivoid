// coop/props/bucket_pin.cpp -- a bucket copy that is a peer's runs its own tick on the host's numbers. See
// coop/props/bucket_sync.h.
//
// A water bucket's tick eases `height_interp` toward `height`, redraws the water and pours when tipped, so a client's copy runs it
// (a refused tick froze the water mesh). The tick also writes the water (a tipped pour, a fill from a volume), the brewing climb
// and `empty`, and the splash handler rolls a random loss of its own, so at the tick's entry the host's last row is written into the
// copy: whatever the copy's own code changed since lasts at most one tick, and the water drawn is the host's. A concrete bucket's tick
// flips `dry` and calls updDry, spawns and replaces props, so a client's copy never runs it. `deleteAlc` destroys the bread and
// sugar inside a brewing bucket: a peer's copy never calls it (the host's own brew destroys them and the destroy lane relays it).
// A copy is a peer's by coop/props/mirror_gate: on a client every world copy, on the host the copy shown in a client's hand.
// MTA eases a vehicle to the server's state (CClientVehicle.cpp:3716); a client's own write here is overwritten whole at the next
// tick, because the water is a number and the game's own `interpFlow` already draws the easing.

#include "bucket_internal.h"

#include "coop/dev/perf_probe.h"
#include "coop/props/bucket_sync.h"
#include "coop/props/mirror_gate.h"

#include "ue_wrap/actors/bucket.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <iterator>

namespace coop::bucket_sync::detail {
namespace {

namespace N  = coop::net;
namespace B  = ue_wrap::bucket;
namespace sg = ue_wrap::script_gate;
namespace M  = coop::mirror_gate;

UE_FACT_FN(kRowPlainTick,    "bucket_pin", L"prop_bucket_C",         L"ReceiveTick", Declared);
UE_FACT_FN(kRowConcreteTick, "bucket_pin", L"prop_concreteBucket_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowDeleteAlc,    "bucket_pin", L"prop_bucket_C",         L"deleteAlc",   Declared);
constexpr int kTagPlain = 0x424B5031, kTagConcrete = 0x424B5032, kTagAlc = 0x424B5033;   // 'BKP1'..'BKP3'

// The host's last row written into the copy.
void PinWrite(void* copy) {
    const N::BucketRow* const row = HostRow(EidOf(copy));
    if (!row || row->type != N::kBucketPlain) return;   // before the first row the copy runs on its own record
    if (B::WritePlain(copy, row->level, row->count, row->progress, (row->flags & N::kBucketAlcohol) != 0,
                      (row->flags & N::kBucketAlcReady) != 0))
        Note(Count::PinWrites);
}

// A water bucket's tick on a peer's copy: the host's last row written into it, the tick then running. The red `tick` refuses
// the tick (the copy's water mesh freezes), the red `pin` writes nothing.
sg::Verdict OnPlainTickPre(const sg::Call& c) {
    if (!c.object || !M::IsPeerCopy(c.object)) return sg::Verdict::Run;
    const dev::Red red = dev::RedOf();
    if (red == dev::Red::Tick) {
        Note(Count::PlainTicksRefused);
        return sg::Verdict::Cancel;
    }
    if (red == dev::Red::Pin || !IsClient()) return sg::Verdict::Run;
    const bool timed = coop::dev::perf_probe::Armed();   // the pin body's cost, counted only under the perf probe
    const unsigned long long t0 = timed ? coop::dev::perf_probe::NowTicks() : 0;
    PinWrite(c.object);
    if (timed) NotePinCost(coop::dev::perf_probe::NowTicks() - t0);
    return sg::Verdict::Run;
}

// The host's own copy ran its tick: its water is told when it moved.
void OnPlainTickPost(const sg::Call& c) {
    if (c.object && IsHost()) NoteHostTick(c.object);
}

sg::Verdict OnConcreteTickPre(const sg::Call& c) {
    if (!c.object || !M::IsPeerCopy(c.object) || dev::RedOf() == dev::Red::Ctick) return sg::Verdict::Run;
    Note(Count::ConcreteTicksRefused);
    return sg::Verdict::Cancel;
}

// The gate skips the post of a cancelled body, so a peer's copy that gets here ran its tick against the rule.
void OnConcreteTickPost(const sg::Call& c) {
    if (!c.object) return;
    if (M::IsPeerCopy(c.object)) Note(Count::ConcreteTicksRan);
    else if (IsHost()) NoteHostTick(c.object);
}

sg::Verdict OnDeleteAlcPre(const sg::Call& c) {
    if (!c.object || !M::IsPeerCopy(c.object) || dev::RedOf() == dev::Red::Alc) return sg::Verdict::Run;
    Note(Count::DeleteAlcRefused);
    return sg::Verdict::Cancel;
}

struct WatchRow {
    const ue_wrap::facts::Function* row;
    int tag;
    sg::PreFn pre;
    sg::PostFn post;
    const char* dead;   // what a dead watch costs
};
const WatchRow kWatches[] = {
    {&kRowPlainTick, kTagPlain, &OnPlainTickPre, &OnPlainTickPost, "a client's bucket copies keep their own numbers"},
    {&kRowConcreteTick, kTagConcrete, &OnConcreteTickPre, &OnConcreteTickPost, "a client's concrete bucket copies run their own tick"},
    {&kRowDeleteAlc, kTagAlc, &OnDeleteAlcPre, nullptr, "a client's bucket copy can destroy what the host's brew destroys"},
};
constexpr size_t kWatchCount = std::size(kWatches);

bool g_registered = false, g_settled = false;

}  // namespace

void SettlePinWatches() {
    if (g_settled) return;
    if (!g_registered) {
        g_registered = true;
        for (const WatchRow& w : kWatches) {
            if (sg::WatchName(*w.row, w.tag, w.pre, w.post)) continue;
            g_settled = true;
            UE_LOGE("bucket_pin: the gate took no watch on %ls::%ls -- %s", w.row->Class(), w.row->Name(), w.dead);
            return;
        }
    }
    sg::ResolvePendingNames();
    for (const WatchRow& w : kWatches)
        if (!sg::NameWatchSettled(*w.row, w.tag)) return;
    g_settled = true;
    for (const WatchRow& w : kWatches) {
        if (sg::NameWatchLive(*w.row, w.tag)) continue;
        UE_LOGE("bucket_pin: the watch on %ls::%ls settled dead -- %s", w.row->Class(), w.row->Name(), w.dead);
        return;
    }
    UE_LOGI("bucket_pin: %zu watches live (the plain tick, the concrete tick, deleteAlc)", kWatchCount);
}

}  // namespace coop::bucket_sync::detail
