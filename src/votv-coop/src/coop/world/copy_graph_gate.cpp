// coop/world/copy_graph_gate.cpp -- see coop/world/copy_graph_gate.h. The rows rest on these bytecode facts:
//   grayboar_C: the begin play runs the prop's own, spawns and attaches the nav pawn, arms four looping timers
//     (senseUpdate, setWalking, noMoveMove, checkIfOnPath), hands itself to the animation blueprint, calls move()
//     and adds itself to the gamemode's allGrayBoars. The tick is the hover: a trace down, a force up, the
//     movement force and the turn of its two arrows. The destroy takes the pawn with it. Every decision (the wander
//     point, the path, the bite, the damage and the death) is in the entries the timers, overlaps and hits reach.
//   grayboar_overcharged_C: its own graph holds its begin play (the parent's, a spark timer, a spin), the death
//     explosion and the punch of a player it hits. firetank_C: the begin play builds the wheels, arms update,
//     senseUpdate, fire, speak and the health noise and calls activate(), a graph entry; the destroy ends its event.
//   kavotia_C: the begin play attaches the mesh to its spring arm and holds the animation instance, then arms the
//     walk, the lie-down, the sense, the charge sound, the talk and the door timer. The tick aims the animation at
//     the actor it looks at.
//   igetis_C: the begin play arms its hour-long destroy timer and the per-second visibility check and makes the
//     three beam materials. The tick places the two hands and the beams. Its overlaps start the montage whose
//     notifies throw a container (a prop mint) and calm the kavotias.

#include "coop/world/copy_graph_gate.h"

#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <iterator>

namespace coop::copy_graph_gate {
namespace {

namespace SG = ue_wrap::script_gate;

UE_FACT_FN(kBoarGraph, "copy_graph_gate", L"grayboar_C", L"ExecuteUbergraph_grayboar", Declared);
UE_FACT_FN(kBoarBegin, "copy_graph_gate", L"grayboar_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kBoarTick, "copy_graph_gate", L"grayboar_C", L"ReceiveTick", Declared);
UE_FACT_FN(kBoarDestroyed, "copy_graph_gate", L"grayboar_C", L"ReceiveDestroyed", Declared);
UE_FACT_FN(kOverGraph, "copy_graph_gate", L"grayboar_overcharged_C", L"ExecuteUbergraph_grayboar_overcharged",
           Declared);
UE_FACT_FN(kOverBegin, "copy_graph_gate", L"grayboar_overcharged_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kTankGraph, "copy_graph_gate", L"firetank_C", L"ExecuteUbergraph_firetank", Declared);
UE_FACT_FN(kTankBegin, "copy_graph_gate", L"firetank_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kTankDestroyed, "copy_graph_gate", L"firetank_C", L"ReceiveDestroyed", Declared);
UE_FACT_FN(kKavotiaGraph, "copy_graph_gate", L"kavotia_C", L"ExecuteUbergraph_kavotia", Declared);
UE_FACT_FN(kKavotiaBegin, "copy_graph_gate", L"kavotia_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kKavotiaTick, "copy_graph_gate", L"kavotia_C", L"ReceiveTick", Declared);
UE_FACT_FN(kIgetisGraph, "copy_graph_gate", L"igetis_C", L"ExecuteUbergraph_igetis", Declared);
UE_FACT_FN(kIgetisBegin, "copy_graph_gate", L"igetis_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kIgetisTick, "copy_graph_gate", L"igetis_C", L"ReceiveTick", Declared);

// One class: its graph, and the events whose entry a copy runs.
struct Gate {
    const char*                     name;
    const ue_wrap::facts::Function* graph;
    const ue_wrap::facts::Function* runs[3];
};
constexpr Gate kGates[] = {
    {"grayboar", &kBoarGraph, {&kBoarBegin, &kBoarTick, &kBoarDestroyed}},
    {"grayboar_overcharged", &kOverGraph, {&kOverBegin, nullptr, nullptr}},
    {"firetank", &kTankGraph, {&kTankBegin, &kTankDestroyed, nullptr}},
    {"kavotia", &kKavotiaGraph, {&kKavotiaBegin, &kKavotiaTick, nullptr}},
    {"igetis", &kIgetisGraph, {&kIgetisBegin, &kIgetisTick, nullptr}},
};
constexpr int kGateCount = static_cast<int>(std::size(kGates));
constexpr int kTagGraph = 0x43474700;  // 'CGG', then the gate
constexpr int kTagEvent = 0x43474500;  // 'CGE', then the gate

// The object whose listed event started last, per gate: the graph call that event makes passes, once. The event's
// body is the store of its parameters and that one call, so nothing of the same class runs between the two.
void* g_pass[kGateCount] = {};
uint64_t g_refused[kGateCount] = {};
bool g_installed = false;

SG::Verdict OnEventPre(const SG::Call& call) {
    const int gate = call.tag - kTagEvent;
    if (gate >= 0 && gate < kGateCount && coop::spawn_authority::IsActiveClientSession()) g_pass[gate] = call.object;
    return SG::Verdict::Run;
}

SG::Verdict OnGraphPre(const SG::Call& call) {
    const int gate = call.tag - kTagGraph;
    if (gate < 0 || gate >= kGateCount || !coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    if (g_pass[gate] == call.object && call.object) {
        g_pass[gate] = nullptr;
        return SG::Verdict::Run;
    }
    const uint64_t n = ++g_refused[gate];
    if ((n & (n - 1)) == 0)
        UE_LOGI("copy_graph_gate[%s]: a copy's graph entry refused on %p (call #%llu)", kGates[gate].name,
                call.object, static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

}  // namespace

void Install() {
    if (g_installed) return;
    g_installed = true;
    int watched = 0, wanted = 0;
    for (int i = 0; i < kGateCount; ++i) {
        const Gate& g = kGates[i];
        // The events first: a graph watched with no event to pass would refuse the begin play too.
        bool events = true;
        for (const ue_wrap::facts::Function* run : g.runs) {
            if (!run) continue;
            ++wanted;
            if (SG::WatchName(*run, kTagEvent + i, &OnEventPre, nullptr)) ++watched;
            else events = false;
        }
        ++wanted;
        if (events && SG::WatchName(*g.graph, kTagGraph + i, &OnGraphPre, nullptr)) {
            ++watched;
        } else {
            UE_LOGE("copy_graph_gate: the watches on %s did not all register -- a client's copy of it thinks",
                    g.name);
        }
    }
    UE_LOGI("copy_graph_gate: %d/%d watches registered over %d classes; a client's copy runs its begin play, and "
            "its tick or destroy where listed, and nothing else of its graph", watched, wanted, kGateCount);
}

bool IsGatedClass(void* cls) {
    if (!cls) return false;
    static const wchar_t* const kNames[] = {L"grayboar_C", L"grayboar_overcharged_C", L"firetank_C", L"kavotia_C",
                                            L"igetis_C"};
    void* bases[std::size(kNames)] = {};
    size_t n = 0;
    for (const wchar_t* name : kNames)
        if (void* base = ue_wrap::object_index::ClassByName(name)) bases[n++] = base;
    return n > 0 && ue_wrap::reflection::IsDescendantOfAny(cls, bases, n);
}

void OnDisconnect() {
    for (int i = 0; i < kGateCount; ++i) {
        if (g_refused[i] > 0)
            UE_LOGI("copy_graph_gate[%s]: %llu graph calls refused over the session", kGates[i].name,
                    static_cast<unsigned long long>(g_refused[i]));
        g_refused[i] = 0;
        g_pass[i] = nullptr;
    }
}

}  // namespace coop::copy_graph_gate
