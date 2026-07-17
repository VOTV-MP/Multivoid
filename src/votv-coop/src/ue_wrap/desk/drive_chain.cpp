// ue_wrap/drive_chain.cpp -- see ue_wrap/desk/drive_chain.h.

#include "ue_wrap/desk/drive_chain.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"

#include <chrono>
#include <cstring>

namespace ue_wrap::drive_chain {
namespace {

namespace R  = ue_wrap::reflection;
namespace SD = ue_wrap::signal_dynamic;
using Clock = std::chrono::steady_clock;

// ---- class-level resolves (persist across level reloads) ----
void*   g_slotCls = nullptr;      // driveSlot_C
void*   g_driveCls = nullptr;     // prop_drive_C
void*   g_rackCls = nullptr;      // prop_driveRack_C
void*   g_eraserCls = nullptr;    // signalDriveEraser_C
void*   g_gamemodeCls = nullptr;  // mainGamemode_C (verb-ctx discrimination)
void*   g_primCompCls = nullptr;  // PrimitiveComponent (IsOverlappingActor owner)

int32_t g_offSlotDrive = -1;        // driveSlot_C::drive
int32_t g_offSlotDetached = -1;     // driveSlot_C::isRecentlyDetached (plain EX_Let bool)
int32_t g_offSlotPort = -1;         // driveSlot_C::drivePort (component ptr)
int32_t g_offDriveData = -1;        // prop_drive_C::data_0 (@0x550, Fstruct_signalDataDynamic)
int32_t g_offDeskSlotPlay = -1;     // analogDScreenTest_C::obj_driveSlot_play (@0x0648)
int32_t g_offDeskSlotComp = -1;     // analogDScreenTest_C::obj_driveSlot_comp (@0x0A88)
int32_t g_offEraserSlot = -1;       // signalDriveEraser_C::driveSLot_obj (typo'd in the BP)
int32_t g_offRackData = -1;         // prop_driveRack_C::data (TArray<row>, stride 0x70)
int32_t g_offRackHas = -1;          // prop_driveRack_C::has  (TArray<bool>)

void* g_putDriveInFn = nullptr;     // driveSlot_C::putDriveIn(overlapped)
void* g_pulledOutFn = nullptr;      // driveSlot_C::drivePulledOut()
void* g_driveUpdFn = nullptr;       // prop_drive_C::upd()
void* g_rackGenFn = nullptr;        // prop_driveRack_C::gen()
void* g_isOverlappingFn = nullptr;  // PrimitiveComponent::IsOverlappingActor(Other)

bool g_resolved = false;
Clock::time_point g_nextTry{};

// ---- instance caches (liveness-revalidated per access) ----
struct CachedActor {
    void* ptr = nullptr;
    int32_t idx = -1;
};
CachedActor g_slots[kRoleCount];   // resolved slot ACTORS per role
CachedActor g_eraser;              // the signalDriveEraser_C instance

void* Revalidate(CachedActor& c) {
    if (c.ptr && R::IsLiveByIndex(c.ptr, c.idx)) return c.ptr;
    c.ptr = nullptr;
    c.idx = -1;
    return nullptr;
}

void Cache(CachedActor& c, void* p) {
    c.ptr = p;
    c.idx = p ? R::InternalIndexOf(p) : -1;
}

// UE TArray view {data @0, Num @8}.
uint8_t* ArrayAt(void* obj, int32_t off, int32_t& num) {
    auto* base = reinterpret_cast<uint8_t*>(obj) + off;
    num = *reinterpret_cast<int32_t*>(base + 8);
    return *reinterpret_cast<uint8_t**>(base);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved) return true;
    const auto now = Clock::now();
    if (now < g_nextTry) return false;
    g_nextTry = now + std::chrono::seconds(1);

    if (!g_slotCls)     g_slotCls = R::FindClass(L"driveSlot_C");
    if (!g_driveCls)    g_driveCls = R::FindClass(L"prop_drive_C");
    if (!g_rackCls)     g_rackCls = R::FindClass(L"prop_driveRack_C");
    if (!g_eraserCls)   g_eraserCls = R::FindClass(L"signalDriveEraser_C");
    if (!g_gamemodeCls) g_gamemodeCls = R::FindClass(L"mainGamemode_C");
    if (!g_primCompCls) g_primCompCls = R::FindClass(L"PrimitiveComponent");
    if (!g_slotCls || !g_driveCls) return false;

    if (g_offSlotDrive < 0)    g_offSlotDrive = R::FindPropertyOffset(g_slotCls, L"drive");
    if (g_offSlotDetached < 0) g_offSlotDetached = R::FindPropertyOffset(g_slotCls, L"isRecentlyDetached");
    if (g_offSlotPort < 0)     g_offSlotPort = R::FindPropertyOffset(g_slotCls, L"drivePort");
    if (g_offDriveData < 0)    g_offDriveData = R::FindPropertyOffset(g_driveCls, L"data_0");
    if (g_offEraserSlot < 0 && g_eraserCls)
        g_offEraserSlot = R::FindPropertyOffset(g_eraserCls, L"driveSLot_obj");
    if (g_offRackData < 0 && g_rackCls) g_offRackData = R::FindPropertyOffset(g_rackCls, L"data");
    if (g_offRackHas < 0 && g_rackCls)  g_offRackHas = R::FindPropertyOffset(g_rackCls, L"has");

    if (void* deskCls = R::FindClass(L"analogDScreenTest_C")) {
        if (g_offDeskSlotPlay < 0) g_offDeskSlotPlay = R::FindPropertyOffset(deskCls, L"obj_driveSlot_play");
        if (g_offDeskSlotComp < 0) g_offDeskSlotComp = R::FindPropertyOffset(deskCls, L"obj_driveSlot_comp");
    }

    if (!g_putDriveInFn) g_putDriveInFn = R::FindFunction(g_slotCls, L"putDriveIn");
    if (!g_pulledOutFn)  g_pulledOutFn = R::FindFunction(g_slotCls, L"drivePulledOut");
    if (!g_driveUpdFn)   g_driveUpdFn = R::FindFunction(g_driveCls, L"upd");
    if (!g_rackGenFn && g_rackCls) g_rackGenFn = R::FindFunction(g_rackCls, L"gen");
    if (!g_isOverlappingFn && g_primCompCls)
        g_isOverlappingFn = R::FindFunction(g_primCompCls, L"IsOverlappingActor");

    const bool core = g_offSlotDrive >= 0 && g_offSlotDetached >= 0 && g_offSlotPort >= 0 &&
                      g_offDriveData >= 0 && g_offDeskSlotPlay >= 0 && g_offDeskSlotComp >= 0 &&
                      g_putDriveInFn && g_pulledOutFn && g_driveUpdFn;
    if (!core) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGW("drive_chain: resolve incomplete (drive=%d det=%d port=%d data=%d "
                    "deskP=%d deskC=%d putIn=%d pull=%d upd=%d) -- backoff retry (log-once)",
                    g_offSlotDrive, g_offSlotDetached, g_offSlotPort, g_offDriveData,
                    g_offDeskSlotPlay, g_offDeskSlotComp, g_putDriveInFn ? 1 : 0,
                    g_pulledOutFn ? 1 : 0, g_driveUpdFn ? 1 : 0);
        }
        return false;
    }
    g_resolved = true;
    UE_LOGI("drive_chain: resolved (slot.drive=0x%X det=0x%X port=0x%X data_0=0x%X "
            "deskPlay=0x%X deskComp=0x%X eraserSlot=0x%X rackData=0x%X rackHas=0x%X "
            "overlapFn=%d rackGen=%d)",
            g_offSlotDrive, g_offSlotDetached, g_offSlotPort, g_offDriveData,
            g_offDeskSlotPlay, g_offDeskSlotComp, g_offEraserSlot, g_offRackData,
            g_offRackHas, g_isOverlappingFn ? 1 : 0, g_rackGenFn ? 1 : 0);
    return true;
}

bool IsDriveClass(void* cls) {
    if (!cls || !g_driveCls) return false;
    if (cls == g_driveCls) return true;
    void* base[1] = { g_driveCls };
    return R::IsDescendantOfAny(cls, base, 1);
}
bool IsRackClass(void* cls) { return cls && g_rackCls && cls == g_rackCls; }
void* DriveClass() { return g_driveCls; }
bool IsSlotClass(void* cls) { return cls && g_slotCls && cls == g_slotCls; }
bool IsGamemodeClass(void* cls) { return cls && g_gamemodeCls && cls == g_gamemodeCls; }

void* SlotActor(int role) {
    if (!g_resolved || role < 0 || role >= kRoleCount) return nullptr;
    if (void* live = Revalidate(g_slots[role])) return live;

    void* slot = nullptr;
    if (role == kRoleDeskPlay || role == kRoleDeskComp) {
        void* desk = ue_wrap::console_desk::Instance();
        if (desk) {
            const int32_t off = (role == kRoleDeskPlay) ? g_offDeskSlotPlay : g_offDeskSlotComp;
            slot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(desk) + off);
            if (slot && !R::IsLive(slot)) slot = nullptr;
        }
    } else {  // eraser
        if (g_offEraserSlot >= 0) {
            void* eraser = Revalidate(g_eraser);
            if (!eraser) {
                // Perf-audit F-3: the census is a full GUObjectArray walk --
                // negative-cache the miss (5 s) or an eraser-less world pays
                // a 1 Hz multi-ms hitch forever.
                static Clock::time_point s_nextCensus{};
                const auto now = Clock::now();
                if (now < s_nextCensus) return nullptr;
                for (void* obj : R::FindObjectsByClass(L"signalDriveEraser_C")) {
                    if (obj && R::IsLive(obj)) { Cache(g_eraser, obj); eraser = obj; break; }
                }
                if (!eraser) s_nextCensus = now + std::chrono::seconds(5);
            }
            if (eraser) {
                slot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(eraser) + g_offEraserSlot);
                if (slot && !R::IsLive(slot)) slot = nullptr;
            }
        }
    }
    if (slot) Cache(g_slots[role], slot);
    return slot;
}

int RoleOfSlotActor(void* slotActor) {
    if (!slotActor) return -1;
    for (int r = 0; r < kRoleCount; ++r) {
        if (g_slots[r].ptr == slotActor) return r;  // pure cached compare (bracket-safe)
    }
    return -1;
}

void* SlotDrive(void* slotActor) {
    if (!slotActor || g_offSlotDrive < 0) return nullptr;
    void* d = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(slotActor) + g_offSlotDrive);
    return (d && R::IsLive(d)) ? d : nullptr;
}

bool SlotRecentlyDetached(void* slotActor) {
    if (!slotActor || g_offSlotDetached < 0) return false;
    return *(reinterpret_cast<uint8_t*>(slotActor) + g_offSlotDetached) != 0;
}

bool CallPutDriveIn(void* slotActor, void* driveActor) {
    if (!slotActor || !driveActor || !g_putDriveInFn) return false;
    ue_wrap::ParamFrame f(g_putDriveInFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"overlapped", driveActor);
    return ue_wrap::Call(slotActor, f);
}

bool CallDrivePulledOut(void* slotActor) {
    if (!slotActor || !g_pulledOutFn) return false;
    ue_wrap::ParamFrame f(g_pulledOutFn);
    return f.valid() && ue_wrap::Call(slotActor, f);
}

void CompleteEjectLatch(void* slotActor, void* driveActor) {
    if (!slotActor || g_offSlotDetached < 0 || g_offSlotPort < 0) return;
    // Dead/absent drive -> the organic EndOverlap can never come; complete now.
    bool overlapping = false;
    if (driveActor && R::IsLive(driveActor) && g_isOverlappingFn) {
        void* port = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(slotActor) + g_offSlotPort);
        if (port && R::IsLive(port)) {
            ue_wrap::ParamFrame f(g_isOverlappingFn);
            if (f.valid()) {
                f.Set<void*>(L"Other", driveActor);
                if (ue_wrap::Call(port, f)) overlapping = f.Get<bool>(L"ReturnValue");
            }
        }
    }
    if (!overlapping)
        *(reinterpret_cast<uint8_t*>(slotActor) + g_offSlotDetached) = 0;
}

bool ReadDriveRow(void* driveActor, SD::Row& out) {
    if (!driveActor || g_offDriveData < 0) return false;
    return SD::ReadStruct(reinterpret_cast<uint8_t*>(driveActor) + g_offDriveData, out);
}

bool WriteDriveRow(void* driveActor, const SD::Row& in) {
    if (!driveActor || g_offDriveData < 0) return false;
    return SD::WriteStructLive(reinterpret_cast<uint8_t*>(driveActor) + g_offDriveData, in);
}

bool CallDriveUpd(void* driveActor) {
    if (!driveActor || !g_driveUpdFn) return false;
    ue_wrap::ParamFrame f(g_driveUpdFn);
    return f.valid() && ue_wrap::Call(driveActor, f);
}

bool ReadRack(void* rackActor, RackRow out[kRackSlots], int& outNum) {
    outNum = 0;
    if (!rackActor || g_offRackData < 0 || g_offRackHas < 0) return false;
    int32_t nData = 0, nHas = 0;
    uint8_t* data = ArrayAt(rackActor, g_offRackData, nData);
    uint8_t* has  = ArrayAt(rackActor, g_offRackHas, nHas);
    if (!data || !has || nData < 0 || nHas < 0) return false;
    const int n = (nData < nHas ? nData : nHas);
    outNum = n < kRackSlots ? n : kRackSlots;
    for (int i = 0; i < outNum; ++i) {
        out[i].has = has[i] != 0;
        if (!SD::ReadStruct(data + static_cast<size_t>(i) * SD::kStride, out[i].row))
            return false;
    }
    return true;
}

bool WriteRackRow(void* rackActor, int idx, const RackRow& in) {
    if (!rackActor || idx < 0 || idx >= kRackSlots ||
        g_offRackData < 0 || g_offRackHas < 0) return false;
    int32_t nData = 0, nHas = 0;
    uint8_t* data = ArrayAt(rackActor, g_offRackData, nData);
    uint8_t* has  = ArrayAt(rackActor, g_offRackHas, nHas);
    if (!data || !has || idx >= nData || idx >= nHas) return false;
    static const SD::Row kEmpty{};
    if (!SD::WriteStructLive(data + static_cast<size_t>(idx) * SD::kStride,
                             in.has ? in.row : kEmpty))
        return false;
    has[idx] = in.has ? 1 : 0;
    return true;
}

bool CallRackGen(void* rackActor) {
    if (!rackActor || !g_rackGenFn) return false;
    ue_wrap::ParamFrame f(g_rackGenFn);
    return f.valid() && ue_wrap::Call(rackActor, f);
}

}  // namespace ue_wrap::drive_chain
