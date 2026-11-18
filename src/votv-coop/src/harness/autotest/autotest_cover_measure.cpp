// harness/autotest/autotest_cover_measure.cpp -- see harness/autotest/cover_measure.h.

#include "harness/autotest/cover_measure.h"

#include "coop/player/players_registry.h"
#include "coop/props/prop_hold.h"
#include "coop/world/cover_shared.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/analog_desk.h"
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace harness::autotest::cover_measure {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;
namespace AD = ue_wrap::analog_desk;
namespace GEN = ue_wrap::generator;

UE_FACT_FN(kRowUpdPanels, "cover_measure", L"analogDScreenTest_C", L"updPanels", Declared);
UE_FACT_FN(kRowDeskOverlap, "cover_measure", L"analogDScreenTest_C", L"ReceiveActorBeginOverlap", Declared);
UE_FACT_FN(kRowResetUnscrew, "cover_measure", L"analogDScreenTest_C", L"resetUnscrew", Declared);
UE_FACT_FN(kRowSawSound, "cover_measure", L"analogDScreenTest_C", L"sawSound", Declared);
UE_FACT_FN(kRowDoorEnd, "cover_measure", L"generator_C", L"doorAnim_TL__FinishedFunc", Declared);
UE_FACT_FN(kRowOpenDoor, "cover_measure", L"generator_C", L"openDoor", Declared);
constexpr int kTagUpd = 0x434D5201, kTagOverlap = 0x434D5202, kTagReset = 0x434D5203, kTagSaw = 0x434D5204,
              kTagDoorEnd = 0x434D5205, kTagOpenDoor = 0x434D5206;  // 'CMR' 1..6

uint64_t Now() { return ::GetTickCount64(); }

std::wstring CallerOf(const sg::Call& c) {
    return c.callerFunction ? R::ToString(R::NameOf(c.callerFunction)) : std::wstring(L"none");
}

unsigned Bits(void* desk) {
    uint8_t b = 0;
    return AD::ReadPlates(desk, b) ? b : 0xFFu;
}

unsigned g_before = 0;

sg::Verdict UpdPre(const sg::Call& c) {
    g_before = Bits(c.object);
    return sg::Verdict::Run;
}

void UpdPost(const sg::Call& c) {
    UE_LOGI("[COVER-MEASURE] t=%llu updPanels caller=%ls applying=%d plates before on=0x%x unbolted=0x%x after on=0x%x "
            "unbolted=0x%x", static_cast<unsigned long long>(Now()), CallerOf(c).c_str(),
            coop::cover_sync::Applying() ? 1 : 0, g_before & 0xF, g_before >> 4, Bits(c.object) & 0xF, Bits(c.object) >> 4);
}

sg::Verdict OverlapPre(const sg::Call& c) {
    const int32_t off = c.function ? R::FindParamOffset(c.function, L"OtherActor") : -1;
    void* const other = off >= 0 && c.locals ? *reinterpret_cast<void* const*>(c.locals + off) : nullptr;
    const int plate = AD::PlateOfProp(other);
    ue_wrap::engine::MainPlayerGrabState gs{};
    const bool haveGrab = ue_wrap::engine::ReadMainPlayerGrabState(coop::players::Registry::Get().Local(), gs);
    const int heldHere = haveGrab && other ? ((other == gs.grabbingActor || other == gs.holdingActor) ? 1 : 0) : -1;
    void* const lid = plate >= 0 ? AD::Lid(c.object, plate) : nullptr;
    UE_LOGI("[COVER-MEASURE] t=%llu desk overlap with %ls (plate %d) heldHere=%d holder=%d lidOverlapsIt=%d applying=%d",
            static_cast<unsigned long long>(Now()), other ? R::ClassNameOf(other).c_str() : L"none", plate,
            heldHere, other ? static_cast<int>(coop::prop_hold::WhoHolds(other, 0)) : -1,
            lid && other ? (E::IsOverlappingActor(lid, other) ? 1 : 0) : -1, coop::cover_sync::Applying() ? 1 : 0);
    return sg::Verdict::Run;
}

sg::Verdict ResetPre(const sg::Call& c) {
    UE_LOGI("[COVER-MEASURE] t=%llu resetUnscrew caller=%ls applying=%d", static_cast<unsigned long long>(Now()),
            CallerOf(c).c_str(), coop::cover_sync::Applying() ? 1 : 0);
    return sg::Verdict::Run;
}

sg::Verdict SawPre(const sg::Call& c) {
    UE_LOGI("[COVER-MEASURE] t=%llu sawSound (the unbolt's sound timer) caller=%ls", static_cast<unsigned long long>(Now()),
            CallerOf(c).c_str());
    return sg::Verdict::Run;
}

int Flag(void* gen, bool (*read)(void*, bool&)) {
    bool v = false;
    return read(gen, v) ? (v ? 1 : 0) : -1;
}

void DoorEndPost(const sg::Call& c) {
    UE_LOGI("[COVER-MEASURE] t=%llu doorAnim_TL__FinishedFunc station=%d opened=%d moving=%d", static_cast<unsigned long long>(Now()),
            GEN::IndexOf(c.object), Flag(c.object, &GEN::ReadOpened), Flag(c.object, &GEN::ReadMoving));
}

int g_openBefore = -1, g_movingBefore = -1;

sg::Verdict OpenDoorPre(const sg::Call& c) {
    g_openBefore = Flag(c.object, &GEN::ReadOpened);
    g_movingBefore = Flag(c.object, &GEN::ReadMoving);
    return sg::Verdict::Run;
}

void OpenDoorPost(const sg::Call& c) {
    const int32_t off = c.function ? R::FindParamOffset(c.function, L"open") : -1;
    const int want = off >= 0 && c.locals ? (c.locals[off] ? 1 : 0) : -1;
    UE_LOGI("[COVER-MEASURE] t=%llu openDoor(%d) station=%d caller=%ls applying=%d opened %d -> %d moving %d -> %d",
            static_cast<unsigned long long>(Now()), want, GEN::IndexOf(c.object), CallerOf(c).c_str(),
            coop::cover_sync::Applying() ? 1 : 0, g_openBefore, Flag(c.object, &GEN::ReadOpened), g_movingBefore,
            Flag(c.object, &GEN::ReadMoving));
}

}  // namespace

void Start() {
    static bool s_started = false;
    if (s_started) return;
    s_started = true;
    const bool ok = sg::WatchName(kRowUpdPanels, kTagUpd, &UpdPre, &UpdPost) &&
                    sg::WatchName(kRowDeskOverlap, kTagOverlap, &OverlapPre, nullptr) &&
                    sg::WatchName(kRowResetUnscrew, kTagReset, &ResetPre, nullptr) &&
                    sg::WatchName(kRowSawSound, kTagSaw, &SawPre, nullptr) &&
                    sg::WatchName(kRowDoorEnd, kTagDoorEnd, nullptr, &DoorEndPost) &&
                    sg::WatchName(kRowOpenDoor, kTagOpenDoor, &OpenDoorPre, &OpenDoorPost);
    UE_LOGI("[COVER-MEASURE] watches registered (ok=%d)", ok ? 1 : 0);
    for (int i = 0; i < 100 && sg::PendingNameCount() > 0; ++i) {
        ue_wrap::game_thread::RunAndWait([](std::atomic<int>& d) {
            sg::ResolvePendingNames();
            d.store(1);
        });
        ::Sleep(100);
    }
    UE_LOGI("[COVER-MEASURE] watches live: updPanels=%d overlap=%d resetUnscrew=%d sawSound=%d doorEnd=%d openDoor=%d",
            sg::NameWatchLive(kRowUpdPanels, kTagUpd) ? 1 : 0, sg::NameWatchLive(kRowDeskOverlap, kTagOverlap) ? 1 : 0,
            sg::NameWatchLive(kRowResetUnscrew, kTagReset) ? 1 : 0, sg::NameWatchLive(kRowSawSound, kTagSaw) ? 1 : 0,
            sg::NameWatchLive(kRowDoorEnd, kTagDoorEnd) ? 1 : 0, sg::NameWatchLive(kRowOpenDoor, kTagOpenDoor) ? 1 : 0);
}

void Census() {
    ue_wrap::game_thread::RunAndWait([](std::atomic<int>& d) {
        std::vector<void*> gens;
        GEN::ReadGenerators(gens);
        int unread = 0;
        for (size_t i = 0; i < gens.size(); ++i) {
            const int32_t at = gens[i] ? GEN::IndexOf(gens[i]) : -2;
            if (at != static_cast<int32_t>(i)) ++unread;
            UE_LOGI("[COVER-MEASURE] station slot %zu: %hs, IndexOf=%d", i, gens[i] ? "present" : "empty", at);
        }
        UE_LOGI("[COVER-MEASURE] station census: %zu slot(s), %d whose index does not read back as its slot", gens.size(), unread);
        d.store(1);
    });
}

}  // namespace harness::autotest::cover_measure
