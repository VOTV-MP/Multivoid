// coop/dev/atv_riders_probe_rows.cpp -- the gate rows of the ATV riders probe; see coop/dev/atv_riders_probe.h. Log only, every
// one runs the body: M1 on the host (the kerfur's own `sitOnCar` POST and `getOffCar` PRE), M3 on a client (the `tryToOccupyCar`
// PRE the driver's own sit makes on every kerfur of its world).

#include "atv_riders_probe_detail.h"

#include "coop/element/mirror_managers.h"
#include "coop/element/registry.h"
#include "harness/autotest/peer_signal.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstddef>
#include <string>

namespace coop::dev::atv_riders_probe::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace PS = harness::autotest::peer_signal;

UE_FACT_FN(kRowSitOnCar, "dev.atv_riders_probe", L"kerfurOmega_C", L"sitOnCar", Declared);
UE_FACT_FN(kRowGetOffCar, "dev.atv_riders_probe", L"kerfurOmega_C", L"getOffCar", Declared);
UE_FACT_FN(kRowTryToOccupyCar, "dev.atv_riders_probe", L"kerfurOmega_C", L"tryToOccupyCar", Declared);

constexpr int kTagSit = 0x41525001;   // 'ARP' 1
constexpr int kTagGetOff = 0x41525002;
constexpr int kTagOccupy = 0x41525003;

std::atomic<bool>     g_sat{false};
std::atomic<uint32_t> g_getOff{0};
R::ParamOffset        g_killOff{L"kill"};

uint32_t EidOf(void* kerfur) { return coop::element::Registry::Get().EidForActor(kerfur); }

void OnSitPost(const sg::Call& c) {
    void* parent = nullptr;
    ue_wrap::engine::GetAttachParentActor(c.object, parent);
    const uint32_t eid = EidOf(c.object);
    UE_LOGI("atv-probe: M1 sitOnCar POST kerfur=%p eid=%u occupyCar=%p parent=%p", c.object, eid,
            ue_wrap::kerfur::GetOccupyCar(c.object), parent);
    if (!g_sat.exchange(true)) PS::Mark("rider-sat", std::to_string(eid));
}

sg::Verdict OnGetOffPre(const sg::Call& c) {
    ++g_getOff;
    const int32_t off = g_killOff.Of(c.function);
    if (off >= 0 && c.locals)
        UE_LOGI("atv-probe: M1 getOffCar PRE kerfur=%p eid=%u kill=%d", c.object, EidOf(c.object), c.locals[off] != 0);
    else
        UE_LOGI("atv-probe: M1 getOffCar PRE kerfur=%p eid=%u kill=?", c.object, EidOf(c.object));
    return sg::Verdict::Run;
}

sg::Verdict OnOccupyPre(const sg::Call& c) {
    const uint32_t eid = EidOf(c.object);
    UE_LOGI("atv-probe: M3 tryToOccupyCar kerfur=%p mirror=%d", c.object,
            eid != coop::element::kInvalidId && coop::element::NpcMirrors().Get(eid) ? 1 : 0);
    return sg::Verdict::Run;
}

struct Watch {
    const ue_wrap::facts::Function& row;
    int                             tag;
    sg::PreFn                       pre;
    sg::PostFn                      post;
    const char*                     label;
};

const Watch kHost[] = {
    {kRowSitOnCar, kTagSit, nullptr, &OnSitPost, "kerfurOmega_C.sitOnCar POST"},
    {kRowGetOffCar, kTagGetOff, &OnGetOffPre, nullptr, "kerfurOmega_C.getOffCar PRE"},
};
const Watch kClient[] = {
    {kRowTryToOccupyCar, kTagOccupy, &OnOccupyPre, nullptr, "kerfurOmega_C.tryToOccupyCar PRE"},
};

template <size_t N>
bool RegisterAll(const Watch (&ws)[N]) {
    bool all = true;
    for (const Watch& w : ws) all = sg::WatchName(w.row, w.tag, w.pre, w.post) && all;
    return all;
}

template <size_t N>
int LiveAll(const Watch (&ws)[N]) {
    sg::ResolvePendingNames();
    int state = 1;
    for (const Watch& w : ws) {
        if (sg::NameWatchLive(w.row, w.tag)) continue;
        if (sg::NameWatchSettled(w.row, w.tag)) {
            UE_LOGE("atv-probe: watch '%s' is NOT live: its name resolved dead", w.label);
            return -1;
        }
        state = 0;
    }
    return state;
}

}  // namespace

bool RegisterRows(bool host) { return host ? RegisterAll(kHost) : RegisterAll(kClient); }

int RowsLive(bool host) { return host ? LiveAll(kHost) : LiveAll(kClient); }

uint32_t GetOffSeen() { return g_getOff.load(); }

}  // namespace coop::dev::atv_riders_probe::detail
