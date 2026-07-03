// coop/dev/event_force.cpp -- see coop/dev/event_force.h.

#include "coop/dev/event_force.h"

#include "coop/dev/dev_gate.h"
#include "coop/world/event_fire_sync.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace coop::dev::event_force {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

// menu event name -> its level trigger volume (map census _map_untitled_1.json
// 2026-07-03: eventer property -> trigger_TBoxActivator_C -> trigger_box_N_C).
// All 14 volume-gated events; every box is a trigger_box_N_C with N=1.
struct BoxRow { const char* event; const wchar_t* box; const char* boxNarrow; };
constexpr BoxRow kBoxes[] = {
    { "obelisk",    L"TB_event_obelisk",      "TB_event_obelisk"      },
    { "arirShip",   L"TB_event_arirAppear",   "TB_event_arirAppear"   },
    { "falseEnter", L"TB_event_arirPasslock", "TB_event_arirPasslock" },
    { "mann",       L"TB_event_mann",         "TB_event_mann"         },
    { "vent",       L"TB_event_vent",         "TB_event_vent"         },
    { "crys",       L"TB_event_crys",         "TB_event_crys"         },
    { "fakeGrays",  L"TB_event_fakeGraysInit","TB_event_fakeGraysInit"},
    { "toeStab",    L"TB_event_arirToe",      "TB_event_arirToe"      },
    { "cookier",    L"TB_event_cookier",      "TB_event_cookier"      },
    { "susArir",    L"TB_event_susArir",      "TB_event_susArir"      },
    { "arirEgg",    L"TB_event_arirEgg",      "TB_event_arirEgg"      },
    { "wisps",      L"TB_event_wispSwarm",    "TB_event_wispSwarm"    },
    { "piramid",    L"TB_event_piramid",      "TB_event_piramid"      },
    { "call0",      L"TB_event_bigmRoar",     "TB_event_bigmRoar"     },
};
constexpr int kBoxCount = static_cast<int>(std::size(kBoxes));

const BoxRow* RowFor(const char* eventName) {
    for (const auto& r : kBoxes)
        if (std::strcmp(r.event, eventName) == 0) return &r;
    return nullptr;
}

// Native-gate notes for rows WITHOUT a volume (mined from the eventer RE docs;
// static truth, not live state).
struct NoteRow { const char* event; const char* note; };
constexpr NoteRow kNotes[] = {
    { "agrav",         "no-op unless lib.isPhysicalEvents (physical-events setting ON)" },
    { "bedEvent",      "arms the dream system -- fires on your NEXT sleep" },
    { "treehouseSleep","fires on your NEXT sleep (kidnap teleport)" },
    { "earthTp",       "teleports THE TRIGGERING PLAYER immediately (skysphere.tp)" },
    { "peace",         "signal joins the SETI pool NOW -- see it in the dish downloads" },
    { "arirSignal",    "signal joins the SETI pool NOW -- see it in the dish downloads" },
    { "arirSpk",       "signal joins the SETI pool NOW -- see it in the dish downloads" },
    { "picSignal",     "signal joins the SETI pool NOW -- see it in the dish downloads" },
    { "arirSat_0",     "signal joins the SETI pool NOW -- see it in the dish downloads" },
    { "arirSat_1",     "signal joins the SETI pool NOW -- see it in the dish downloads" },
    { "arirSat_2",     "signal joins the SETI pool NOW -- see it in the dish downloads" },
    { "piramid_sig",   "signal joins the SETI pool NOW -- see it in the dish downloads" },
    { "treehouse_0",   "instant build step (visible at the radio-tower treehouse site)" },
    { "treehouse_1",   "instant build step (visible at the radio-tower treehouse site)" },
    { "treehouse_2",   "instant build step (visible at the radio-tower treehouse site)" },
    { "treehouse_3",   "instant build step (visible at the radio-tower treehouse site)" },
    { "treehouse_4",   "instant build step (visible at the radio-tower treehouse site)" },
    { "treehouse_5",   "instant build step (visible at the radio-tower treehouse site)" },
    { "break_RomeoSierra", "instant: the server rack breaks NOW (check the server room)" },
    { "break_Victor",  "instant: the server rack breaks NOW (check the server room)" },
    { "break_Victor2", "instant: the server rack breaks NOW (check the server room)" },
    { "solar",         "instant sky flash + boom (step outside / look at the sky)" },
    { "picnic",        "instant: picnic props appear at the forest picnic site" },
    { "destroyPicnic", "instant: the picnic site props are removed" },
};

struct Snap { bool resolved; bool armed; int shots; };
Snap g_snap[kBoxCount];
std::mutex g_snapMx;
std::atomic<int64_t> g_lastRefreshMs{0};

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Find a mapped live box actor by its level name. One GUObjectArray class walk
// per call -- refresh/force paths only (never per-frame).
void* FindBox(const wchar_t* name) {
    for (void* obj : R::FindObjectsByClass(L"trigger_box_N_C")) {
        if (obj && R::IsLive(obj) && R::NameStartsWith(R::NameOf(obj), name))
            return obj;
    }
    return nullptr;
}

// trigger_box_C property offsets (resolved once against the live class).
int32_t g_offIsActiveByte = -1;
uint8_t g_maskIsActive = 0;
int32_t g_offN = -1;
int32_t g_offBoxComp = -1;
bool ResolveOffsets(void* boxObj) {
    if (g_offIsActiveByte >= 0 && g_offN >= 0 && g_offBoxComp >= 0) return true;
    void* cls = R::ClassOf(boxObj);
    if (!cls) return false;
    if (!R::FindBoolProperty(cls, L"IsActive", g_offIsActiveByte, g_maskIsActive)) {
        UE_LOGW("event_force: trigger_box_C.IsActive bool property unresolved");
        return false;
    }
    g_offN = R::FindPropertyOffset(cls, L"N");
    g_offBoxComp = R::FindPropertyOffset(cls, L"Box");
    if (g_offN < 0 || g_offBoxComp < 0) {
        UE_LOGW("event_force: trigger_box_N offsets unresolved (N=%d Box=%d)", g_offN, g_offBoxComp);
        return false;
    }
    return true;
}

bool ReadArmed(void* box) {
    return (*(reinterpret_cast<const uint8_t*>(box) + g_offIsActiveByte) & g_maskIsActive) != 0;
}

// The local player pawn (controller present = local, the project-wide
// discriminator; puppets are unpossessed).
void* LocalPawn() {
    for (void* p : R::FindObjectsByClass(L"mainPlayer_C")) {
        if (p && R::IsLive(p) && E::GetController(p)) return p;
    }
    return nullptr;
}

constexpr wchar_t kOverlapFn[] =
    L"BndEvt__Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature";

}  // namespace

BoxStatus StatusFor(const char* eventName) {
    BoxStatus st;
    const BoxRow* row = RowFor(eventName);
    if (!row) return st;
    st.hasBox = true;
    st.boxName = row->boxNarrow;
    const int idx = static_cast<int>(row - kBoxes);
    std::lock_guard<std::mutex> lk(g_snapMx);
    st.resolved = g_snap[idx].resolved;
    st.armed = g_snap[idx].armed;
    st.shots = g_snap[idx].shots;
    return st;
}

void RequestRefresh() {
    const int64_t now = NowMs();
    int64_t last = g_lastRefreshMs.load(std::memory_order_relaxed);
    if (now - last < 1000) return;
    if (!g_lastRefreshMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    GT::Post([] {
        Snap fresh[kBoxCount] = {};
        // one class walk for the whole table, then per-box name match
        std::vector<void*> boxes = R::FindObjectsByClass(L"trigger_box_N_C");
        for (int i = 0; i < kBoxCount; ++i) {
            void* hit = nullptr;
            for (void* obj : boxes)
                if (obj && R::IsLive(obj) && R::NameStartsWith(R::NameOf(obj), kBoxes[i].box)) { hit = obj; break; }
            if (!hit || !ResolveOffsets(hit)) continue;
            fresh[i].resolved = true;
            fresh[i].armed = ReadArmed(hit);
            fresh[i].shots = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(hit) + g_offN);
        }
        std::lock_guard<std::mutex> lk(g_snapMx);
        std::memcpy(g_snap, fresh, sizeof(g_snap));
    });
}

const char* GateNote(const char* eventName) {
    for (const auto& n : kNotes)
        if (std::strcmp(n.event, eventName) == 0) return n.note;
    return nullptr;
}

bool ForceNow(const char* eventName) {
    if (!::coop::dev_gate::Allowed()) {
        UE_LOGW("event_force: REFUSED -- dev features are disabled while connected as a client");
        return false;
    }
    const BoxRow* row = RowFor(eventName);
    if (!row) return false;
    // 1) ARM through the shared fire seam (native eventer dispatch + the v95
    //    EventFire broadcast -> clients replay the arm per policy). Re-arming an
    //    already-active box is a no-op (isActive=true twice).
    namespace efs = coop::event_fire_sync;
    const std::string narrow(row->event);
    std::wstring wname(narrow.begin(), narrow.end());
    efs::HostFire(efs::FireKind::RunEvent, wname, L"None");
    // 2) COMPLETE: drive the box's own BeginOverlap handler with the local pawn.
    //    Posted after HostFire's task (FIFO pump) so the arm lands first.
    const wchar_t* boxName = row->box;
    const char* boxNarrow = row->boxNarrow;
    GT::Post([boxName, boxNarrow] {
        void* box = FindBox(boxName);
        if (!box) { UE_LOGW("event_force: box %s not found (world up?)", boxNarrow); return; }
        if (!ResolveOffsets(box)) return;
        const int32_t n = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(box) + g_offN);
        if (n <= 0) {
            UE_LOGI("event_force: %s already consumed (N=%d) -- nothing to force", boxNarrow, n);
            return;
        }
        void* pawn = LocalPawn();
        if (!pawn) { UE_LOGW("event_force: no local player pawn -- cannot emulate the walk-in"); return; }
        void* fn = R::FindFunction(R::ClassOf(box), kOverlapFn);
        if (!fn) { UE_LOGW("event_force: %s overlap handler unresolved", boxNarrow); return; }
        ue_wrap::ParamFrame f(fn);
        if (!f.valid()) return;
        void* boxComp = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(box) + g_offBoxComp);
        f.Set<void*>(L"OverlappedComponent", boxComp);
        f.Set<void*>(L"OtherActor", pawn);
        // OtherComp/OtherBodyIndex/bFromSweep/SweepResult stay zeroed -- the
        // handler's bytecode only reads OtherActor (class filter) [trigger_box_N RE].
        if (ue_wrap::Call(box, f))
            UE_LOGI("event_force: '%s' FORCED -- %s overlap dispatched with the local pawn "
                    "(native filter/N/collision bookkeeping ran)", boxNarrow, boxNarrow);
        else
            UE_LOGW("event_force: %s overlap dispatch FAILED", boxNarrow);
    });
    return true;
}

}  // namespace coop::dev::event_force
