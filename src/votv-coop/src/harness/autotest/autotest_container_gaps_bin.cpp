// harness/autotest/autotest_container_gaps_bin.cpp -- the container gaps drill's bin case (autotest_container_gaps.cpp):
// the host drops a disc into a garbage container, tips it out and destroys it; the client's copy must read
// the host's list and flags after each, the listed disc must be glued to it, and the disc must be free at the end.

#include "container_gaps_shared.h"   // co-located private header (src tree, not include/)

#include "coop/interactables/garbage_bin_sync.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace harness::autotest::container_gaps {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace SR = ue_wrap::save_record;

int32_t g_offItems = -1, g_offPropData = -1, g_offCanHold = -1, g_offCanCollect = -1;

// The bin's itemsInside count and its two flags, "n h c".
std::string BinState() {
    if (!Live(g.bin)) return "-1 -1 -1";
    void* cls = R::ClassOf(g.bin);
    if (g_offItems < 0) g_offItems = R::FindPropertyOffset(cls, L"itemsInside");
    if (g_offPropData < 0) {
        g_offPropData = R::FindPropertyOffset(cls, L"propData");
        if (void* st = R::PropertyInnerStruct(cls, L"propData")) {
            g_offCanHold = R::FindPropertyOffsetByPrefix(st, L"canHold_");
            g_offCanCollect = R::FindPropertyOffsetByPrefix(st, L"canCollect_");
        }
    }
    int32_t items = -1;
    if (g_offItems >= 0) items = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(g.bin) + g_offItems + 8);
    const uint8_t* pd = g_offPropData >= 0 ? static_cast<const uint8_t*>(g.bin) + g_offPropData : nullptr;
    const int hold = pd && g_offCanHold >= 0 ? (pd[g_offCanHold] ? 1 : 0) : -1;
    const int collect = pd && g_offCanCollect >= 0 ? (pd[g_offCanCollect] ? 1 : 0) : -1;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d %d %d", items, hold, collect);
    return buf;
}

std::string ReadBin() {
    std::string s;
    OnGameThread([&] { s = BinState(); });
    return s;
}

// A disc above the bin's mouth, and what was done, said: the bin's place, the disc's, and what the spawn returned.
void DropDiscIntoBin() {
    OnGameThread([] {
        ue_wrap::FVector binAt{};
        if (!E::TryGetActorLocation(g.bin, binAt)) {
            UE_LOGW("container-gaps: bin: the bin's place did not read -- no disc dropped");
            return;
        }
        const ue_wrap::FVector at{binAt.X, binAt.Y, binAt.Z + 70.f};
        void* disc = SpawnPath(kDiscPath, at);
        UE_LOGI("container-gaps: bin: the host drops a disc above the bin at %.0f,%.0f,%.0f (the bin is at %.0f,%.0f,%.0f): "
                "spawned %p", at.X, at.Y, at.Z, binAt.X, binAt.Y, binAt.Z, disc);
    });
}

// The client's copy has resolved the host's state number `n` (counted from `base`) and must hold the host's list and flags.
// A leg can make two host states (the flags and the list change on different ticks), so a reading that differs from an
// applied state is read again at the next one; a state resolved and not applied (garbage_state_off) is a decided failure.
bool JudgeBin(const char* leg, uint32_t base, uint32_t n, const std::string& host) {
    namespace GB = coop::garbage_bin_sync;
    if (!Wait("the host's garbage state resolved", kStepSec, [&] { return GB::ResolvedCount() >= base + n; })) return false;
    std::string mine = ReadBin();
    if (mine != host && GB::AppliedCount() >= GB::ResolvedCount()) {
        const uint32_t seen = GB::ResolvedCount();
        if (!Wait("the next state of the host's container", kStepSec, [&] {
                mine = ReadBin();
                return mine == host || GB::ResolvedCount() > seen;
            }))
            return false;
        mine = ReadBin();
    }
    if (mine != host) {
        Fail("bin %s: this client's container reads '%s' (items, canHold, canCollect), the host's '%s'", leg, mine.c_str(),
             host.c_str());
        return false;
    }
    UE_LOGI("container-gaps: bin %s -- the client's container reads the host's '%s'", leg, host.c_str());
    return true;
}

// The props the bin's own itemsInside holds, the list the host's state wrote, each with the parent it is attached to.
struct BinItem {
    void* actor;
    void* parent;
};
std::vector<BinItem> BinItems() {
    std::vector<BinItem> out;
    if (!Live(g.bin)) return out;
    if (g_offItems < 0) g_offItems = R::FindPropertyOffset(R::ClassOf(g.bin), L"itemsInside");
    if (g_offItems < 0) return out;
    const SR::Arr arr = SR::ReadArr(g.bin, g_offItems);
    for (int32_t i = 0; arr.data && i < arr.num; ++i) {
        void* a = reinterpret_cast<void* const*>(arr.data)[i];
        void* parent = nullptr;
        if (a && Live(a)) {
            E::GetAttachParentActor(a, parent);
            out.push_back({a, parent});
        }
    }
    return out;
}

// The bin's items that are attached to it.
std::vector<void*> AttachedItems() {
    std::vector<void*> out;
    for (const BinItem& it : BinItems())
        if (it.parent == g.bin) out.push_back(it.actor);
    return out;
}

}  // namespace

bool HostBinLeg() {
    // bin: a disc in, tipped out, the container destroyed.
    if (!WaitMark("bin_base")) return false;
    DropDiscIntoBin();
    std::string st;
    if (!Wait("the disc in the bin", kStepSec, [&] { st = ReadBin(); return st.rfind("1 ", 0) == 0; })) return false;
    UE_LOGI("container-gaps: bin: the host's bin holds the disc ('%s': items, canHold, canCollect)", st.c_str());
    PS::Mark("bin_in", st);
    if (!WaitMark("bin_in_ok")) return false;
    // Tipping a bin in place puts its body into the floor (its origin sits near its foot) and it fell through, so it is lifted
    // by its own height as it is turned.
    OnGameThread([&] {
        ue_wrap::FVector at{}, origin{}, extent{};
        ue_wrap::FRotator rot{};
        E::TryGetActorRotation(g.bin, rot);
        E::TryGetActorLocation(g.bin, at);
        const float lift = E::GetActorBounds(g.bin, true, origin, extent) ? 2.f * extent.Z : 0.f;
        const bool ran = E::TeleportTo(g.bin, {at.X, at.Y, at.Z + lift}, ue_wrap::FRotator{rot.Pitch, rot.Yaw, 180.f});
        UE_LOGI("container-gaps: bin: the host tips the bin over at %.0f,%.0f,%.0f lifted %.0f cm (ran=%d)", at.X, at.Y, at.Z, lift,
                ran ? 1 : 0);
    });
    if (!Wait("the bin tipped out", kStepSec, [&] { st = ReadBin(); return st.rfind("0 ", 0) == 0; })) return false;
    UE_LOGI("container-gaps: bin: the host's bin released the disc ('%s')", st.c_str());
    PS::Mark("bin_out", st);
    if (!WaitMark("bin_out_ok")) return false;
    OnGameThread([] { E::DestroyActor(g.bin); });
    UE_LOGI("container-gaps: bin: the host destroyed the bin");
    PS::Mark("bin_destroyed");
    return WaitMark("bin_free_ok");
}

// The client's half: its copy reads the host's state after each step, the listed disc is glued to it, the tip releases it.
bool ClientBinLeg() {
    std::string text;
    // bin
    const uint32_t base = coop::garbage_bin_sync::ResolvedCount();
    PS::Mark("bin_base");
    if (!WaitMark("bin_in", &text) || !JudgeBin("with a disc", base, 1, text)) return false;
    // Judged by the bin's own state, read after the glue (JudgeBin returned on the applied state): its itemsInside holds the
    // prop the host listed, and that prop is attached to it.
    std::vector<void*> discs;
    OnGameThread([&] { discs = AttachedItems(); });
    if (discs.empty()) {
        OnGameThread([] {
            for (const BinItem& it : BinItems())
                UE_LOGI("container-gaps: the bin holds %p (%ls, key '%ls'), attached to %p (the container is %p)", it.actor,
                        R::ClassNameOf(it.actor).c_str(), ue_wrap::prop::GetInteractableKeyString(it.actor).c_str(), it.parent,
                        g.bin);
        });
        Fail("bin: the prop the host lists in the container is not attached to this client's copy of it");
        return false;
    }
    PS::Mark("bin_in_ok");
    if (!WaitMark("bin_out", &text) || !JudgeBin("tipped", base, 2, text)) return false;
    // The tip releases the disc: it is no longer attached to the container.
    bool held = false;
    OnGameThread([&] {
        for (void* d : discs) {
            void* parent = nullptr;
            if (Live(d) && E::GetAttachParentActor(d, parent) && parent == g.bin) held = true;
        }
    });
    if (held) { Fail("bin: the disc the tip released is still attached to this client's copy of the container"); return false; }
    PS::Mark("bin_out_ok");
    if (!WaitMark("bin_destroyed")) return false;
    if (!Wait("the container gone here", kStepSec, [] { return Dead(&g.bin); })) return false;
    bool stuck = false;
    OnGameThread([&] {
        for (void* d : discs) {
            void* parent = nullptr;
            if (Live(d) && E::GetAttachParentActor(d, parent) && parent) stuck = true;
        }
    });
    if (stuck) { Fail("bin: a disc is still attached to the container that was destroyed"); return false; }
    PS::Mark("bin_free_ok");
    return true;
}

}  // namespace harness::autotest::container_gaps
