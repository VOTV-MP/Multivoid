// coop/world/cover_sync.cpp -- see coop/world/cover_sync.h.
//
// analogDScreenTest_C's plates, from its bytecode (0.9.0n). `panels` holds the four plate meshes (0 top left, 1 bottom
// left, 2 top right, which "does not move", 3 bottom right), `panelsState` whether each is on and `panelsUnscrew`
// whether each is unbolted. playerUsedOn with a tool on a plate starts a timed unbolt (the ui_linearProgress widget,
// the sawSound timer), whose end runs `unscrewPanel`: the looked-at plate's bolt flipped, `updPanels`, `resetUnscrew`.
// actionOptionIndex on an unbolted plate that is on, while no plate is being set back (`panelsProcess`): that plate
// off, `panelTakeoff`, its prop class spawned at the plate's transform and handed to the player's pickupObjectDirect,
// `updPanels`, `soundDetachPanel`. ReceiveActorBeginOverlap, while neither `panelTakeoff` nor `panelsProcess`: a plate
// prop of the top left, bottom left or bottom right class touching its lid sets that plate on, `updPanels`, destroys
// the prop and slides the lid home (`panelsProcess` while it moves). ReceiveActorEndOverlap ends `panelTakeoff`.
// `updPanels` shows each plate by its state, sets its collision, and tilts an unbolted one 5 degrees. gatherData and
// setData keep both arrays in the desk's save record.

// generator_C's door: actionOptionIndex, action 4 while `lookAtDoor`: `openDoor(!opened)`, which, unless the door
// already `moving`, plays or reverses doorAnim_TL; its end writes `opened` and the repair panel's collision.

#include "coop/world/cover_sync.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/facts/function_row.h"

#include <chrono>
#include <cmath>
#include <cwchar>
#include <vector>

namespace coop::cover_sync {
namespace {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace OI  = ue_wrap::object_index;
namespace sg  = ue_wrap::script_gate;
namespace CC  = ue_wrap::component_calls;
namespace GEN = ue_wrap::generator;
namespace EL  = coop::element;
namespace N   = coop::net;

UE_FACT_FN(kRowUnscrew, "cover_sync", L"analogDScreenTest_C", L"unscrewPanel", Declared);
UE_FACT_FN(kRowDeskAction, "cover_sync", L"analogDScreenTest_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowDeskOverlap, "cover_sync", L"analogDScreenTest_C", L"ReceiveActorBeginOverlap", Declared);
UE_FACT_FN(kRowStationAction, "cover_sync", L"generator_C", L"actionOptionIndex", Declared);
constexpr int kTagUnscrew = 0x43565201, kTagDeskAction = 0x43565202, kTagDeskOverlap = 0x43565203,
              kTagStationAction = 0x43565204;  // 'CVR' 1..4

constexpr const wchar_t* kDeskClass = L"analogDScreenTest_C";
// The plates' prop classes, by plate.
const wchar_t* const kPlateClass[N::kCoverDeskPanels] = {L"prop_computerBackPanel_tl_C", L"prop_computerBackPanel_bl_C",
                                                         L"prop_computerBackPanel_tr_C", L"prop_computerBackPanel_br_C"};
// Each plate's lid, the component a plate prop must touch to be set back (the top right has none that moves).
const wchar_t* const kLid[N::kCoverDeskPanels] = {L"lid1", L"lid2", L"lid3_no", L"lid4"};
constexpr uint8_t  kFixedPlate = 2;       // the top right plate, which the game never lets move
constexpr uint8_t  kActionUse = 4;        // EInteractionActions: the E press
constexpr float    kDeskReachCm = 400.f;  // a client's ask, from its eye to the plate
constexpr float    kPlateNearCm = 300.f;  // a plate set back, from the prop to its place
constexpr float    kDoorReachCm = 600.f;  // a client's door press, from its eye to the station
constexpr uint64_t kAskEveryMs = 250;     // a slot's asks, one per this
constexpr uint64_t kScanEveryMs = 100;    // the host's look at the covers
constexpr uint64_t kPickupWaitMs = 3000;  // a granted plate's copy, waited for this long
constexpr uint64_t kBodyWaitMs = 5000;    // an ask waits this long for its sender's body here

coop::net::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;
bool g_applying = false;  // this lane's own calls into the desk or a station: the gate lets them run

bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->running() && g_session->role() == N::Role::Client; }

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

float Dist2(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

bool ReadBool(void* obj, const wchar_t* name, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return false;
    out = (static_cast<const uint8_t*>(obj)[off] & mask) != 0;
    return true;
}

bool BoolOr(void* obj, const wchar_t* name, bool fallback) {
    bool v = fallback;
    return ReadBool(obj, name, v) ? v : fallback;
}

void WriteBool(void* obj, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return;
    uint8_t& b = static_cast<uint8_t*>(obj)[off];
    b = v ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

int32_t IntOr(void* obj, const wchar_t* name, int32_t fallback) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<const int32_t*>(static_cast<const char*>(obj) + off) : fallback;
}

const ue_wrap::field_io::TArrayView* ArrayOf(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? reinterpret_cast<const ue_wrap::field_io::TArrayView*>(static_cast<const char*>(obj) + off)
                    : nullptr;
}

// ---- the desk ------------------------------------------------------------------------------------------------------

void* Desk() {
    void* const cls = OI::ClassByName(kDeskClass);
    if (!cls) return nullptr;
    void* found = nullptr;
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        void*& f = *static_cast<void**>(ctx);
        if (!f && obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) f = obj;
    }, &found);
    return found;
}

// The plates as one byte: on in bits 0-3, unbolted in bits 4-7. False when either array is short.
bool ReadPlates(void* desk, uint8_t& bits) {
    const auto* on = ArrayOf(desk, L"panelsState");
    const auto* loose = ArrayOf(desk, L"panelsUnscrew");
    if (!on || !loose || !on->data || !loose->data || on->num < N::kCoverDeskPanels || loose->num < N::kCoverDeskPanels)
        return false;
    bits = 0;
    for (int i = 0; i < N::kCoverDeskPanels; ++i) {
        if (on->data[i]) bits |= static_cast<uint8_t>(1u << i);
        if (loose->data[i]) bits |= static_cast<uint8_t>(1u << (4 + i));
    }
    return true;
}

bool PlateOn(uint8_t bits, int i) { return (bits >> i) & 1u; }
bool PlateLoose(uint8_t bits, int i) { return (bits >> (4 + i)) & 1u; }

// The plates written and shown through the desk's own updPanels.
void WritePlates(void* desk, uint8_t bits) {
    auto* on = const_cast<ue_wrap::field_io::TArrayView*>(ArrayOf(desk, L"panelsState"));
    auto* loose = const_cast<ue_wrap::field_io::TArrayView*>(ArrayOf(desk, L"panelsUnscrew"));
    if (!on || !loose || !on->data || !loose->data || on->num < N::kCoverDeskPanels || loose->num < N::kCoverDeskPanels)
        return;
    for (int i = 0; i < N::kCoverDeskPanels; ++i) {
        on->data[i] = PlateOn(bits, i) ? 1 : 0;
        loose->data[i] = PlateLoose(bits, i) ? 1 : 0;
    }
    g_applying = true;
    CC::CallParamlessNamed(desk, L"updPanels");
    g_applying = false;
}

void* PlateMesh(void* desk, int i) {
    const auto* panels = ArrayOf(desk, L"panels");
    if (!panels || !panels->data || i < 0 || i >= panels->num) return nullptr;
    void* const mesh = reinterpret_cast<void* const*>(panels->data)[i];
    return mesh && R::IsLive(mesh) ? mesh : nullptr;
}

int PlateOfClass(void* actor) {
    if (!actor) return -1;
    const std::wstring cls = R::ClassNameOf(actor);
    for (int i = 0; i < N::kCoverDeskPanels; ++i)
        if (_wcsicmp(cls.c_str(), kPlateClass[i]) == 0) return i;
    return -1;
}

void SoundDetach(void* desk, const ue_wrap::FVector& at) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(desk), L"soundDetachPanel"));
    if (f.valid() && f.Set<ue_wrap::FVector>(L"location", at)) {
        g_applying = true;
        ue_wrap::Call(desk, f);
        g_applying = false;
    }
}

// ---- the stations --------------------------------------------------------------------------------------------------

void* Station(int index) {
    std::vector<void*> gens;
    GEN::ReadGenerators(gens);
    return index >= 0 && index < static_cast<int>(gens.size()) ? gens[static_cast<size_t>(index)] : nullptr;
}

void OpenDoor(void* gen, bool open) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(gen), L"openDoor"));
    if (!f.valid() || !f.Set<bool>(L"open", open)) return;
    g_applying = true;
    ue_wrap::Call(gen, f);
    g_applying = false;
}

// ---- the host ------------------------------------------------------------------------------------------------------
struct Ask { int slot; N::CoverPayload p; uint64_t since; };
std::vector<Ask> g_asks;
uint64_t g_lastAskMs[N::kMaxPeers] = {};
uint64_t g_nextScanMs = 0;
bool g_deskSaid = false;
uint8_t g_deskSent = 0;
std::vector<int8_t> g_doorSent;  // by station: -1 not said, else opened

void Send(const N::CoverPayload& p, int slot) {
    if (slot >= 0) g_session->SendReliableToSlot(slot, N::ReliableKind::CoverState, &p, sizeof(p));
    else g_session->SendReliable(N::ReliableKind::CoverState, &p, sizeof(p));
}

void SendDesk(uint8_t bits, int slot) {
    N::CoverPayload p{};
    p.op = N::kCoverOpState;
    p.what = N::kCoverDesk;
    p.bits = bits;
    Send(p, slot);
}

void SendDoor(int index, bool open, int slot) {
    N::CoverPayload p{};
    p.op = N::kCoverOpState;
    p.what = N::kCoverStation;
    p.index = static_cast<uint8_t>(index);
    p.bits = open ? 1 : 0;
    Send(p, slot);
}

void AnswerTakeOff(int slot, uint8_t plate, uint32_t eid) {
    N::CoverPayload p{};
    p.op = N::kCoverOpAnswer;
    p.what = N::kCoverDesk;
    p.index = plate;
    p.bits = eid ? 1 : 0;
    p.eid = eid;
    Send(p, slot);
}

bool EyeOf(int slot, ue_wrap::FVector& eye) {
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(static_cast<uint8_t>(slot));
    return rp.valid() && rp.TryGetEyePosition(eye);
}

// A granted take-off's answer waits for its prop's element id, which the spawn seam gives it after the spawn returns.
struct Owed {
    int slot = -1;
    uint8_t plate = 0;
    ue_wrap::CachedObjRef prop;
    uint64_t since = 0;
};
std::vector<Owed> g_owed;
// A granted take-off's prop moves from the plate to its asker's hand over the next frames, leaving the lid and
// brushing it again: the host's desk holds panelTakeoff until this, so its own set-back does not take the plate back
// on the way.
constexpr uint64_t kTakeOffSettleMs = 1500;
uint64_t g_holdTakeOffUntilMs = 0;

void PayOwed() {
    const uint64_t now = NowMs();
    for (size_t i = 0; i < g_owed.size();) {
        Owed& o = g_owed[i];
        void* const prop = o.prop.Get();
        const EL::ElementId eid = prop ? EL::Registry::Get().EidForActor(prop) : EL::kInvalidId;
        if (eid == EL::kInvalidId && prop && now - o.since < 2000) {
            ++i;
            continue;
        }
        AnswerTakeOff(o.slot, o.plate, eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid));
        UE_LOGI("cover_sync: HOST slot %d's plate %u is prop eid %u -- answered", o.slot, o.plate,
                eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid));
        g_owed.erase(g_owed.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// A take-off: the plate off on the host's desk, and its prop the host's, spawned where the plate was, so every peer
// has it. The asker takes it into its hand when its copy arrives.
void* TakeOff(void* desk, uint8_t plate, uint8_t bits) {
    void* const mesh = PlateMesh(desk, plate);
    void* const cls = R::FindClass(kPlateClass[plate]);
    if (!mesh || !cls) {
        UE_LOGW("cover_sync: HOST plate %u's mesh or prop class (%ls) is not here", plate, kPlateClass[plate]);
        return nullptr;
    }
    const ue_wrap::FVector at = E::GetComponentLocation(mesh);
    const ue_wrap::FRotator rot = E::GetComponentWorldRotation(mesh);
    WriteBool(desk, L"panelTakeoff", true);  // the prop born touching its lid is not set back at once
    WritePlates(desk, static_cast<uint8_t>(bits & ~(1u << plate)));
    void* const prop = E::BeginDeferredSpawn(cls, at, rot);
    if (!prop || !E::FinishDeferredSpawn(prop, at, rot)) {  // no plate made: it stays on
        WriteBool(desk, L"panelTakeoff", false);
        WritePlates(desk, bits);
        UE_LOGW("cover_sync: HOST plate %u's prop did not spawn -- the plate stays on", plate);
        return nullptr;
    }
    SoundDetach(desk, at);
    return prop;
}

void JudgeDesk(const Ask& a) {
    void* const desk = Desk();
    uint8_t bits = 0;
    const uint8_t plate = a.p.index;
    if (!desk || !ReadPlates(desk, bits) || plate >= N::kCoverDeskPanels) {
        UE_LOGW("cover_sync: HOST slot %d asked of plate %u with no desk here -- refused", a.slot, plate);
        if (a.p.op == N::kCoverOpTakeOff) AnswerTakeOff(a.slot, plate, 0);
        return;
    }
    ue_wrap::FVector eye{};
    void* const mesh = PlateMesh(desk, plate);
    const bool seen = mesh && EyeOf(a.slot, eye);
    const ue_wrap::FVector plateAt = mesh ? E::GetComponentLocation(mesh) : ue_wrap::FVector{};
    const bool reach = seen && Dist2(eye, plateAt) <= kDeskReachCm * kDeskReachCm;
    if (!reach)
        UE_LOGI("cover_sync: HOST slot %d's eye (%.0f,%.0f,%.0f) is %.0f cm from plate %u at (%.0f,%.0f,%.0f)", a.slot,
                eye.X, eye.Y, eye.Z, seen ? std::sqrt(Dist2(eye, plateAt)) : -1.f, plate, plateAt.X, plateAt.Y, plateAt.Z);
    const bool busy = BoolOr(desk, L"panelsProcess", false);
    if (a.p.op == N::kCoverOpUnscrew) {
        const bool ok = reach && plate != kFixedPlate && PlateOn(bits, plate) && !busy;
        if (ok) WritePlates(desk, static_cast<uint8_t>(bits ^ (1u << (4 + plate))));
        UE_LOGI("cover_sync: HOST slot %d %hs plate %u -- %hs", a.slot, PlateLoose(bits, plate) ? "bolts" : "unbolts",
                plate, ok ? "taken" : reach ? "refused by the desk" : "out of reach, refused");
    } else if (a.p.op == N::kCoverOpTakeOff) {
        const bool ok = reach && PlateLoose(bits, plate) && PlateOn(bits, plate) && !busy;
        void* const prop = ok ? TakeOff(desk, plate, bits) : nullptr;
        if (prop) {
            g_owed.emplace_back();
            g_owed.back().slot = a.slot;
            g_owed.back().plate = plate;
            g_owed.back().prop.Set(prop);
            g_owed.back().since = NowMs();
            g_holdTakeOffUntilMs = NowMs() + kTakeOffSettleMs;
        } else {
            AnswerTakeOff(a.slot, plate, 0);
        }
        UE_LOGI("cover_sync: HOST slot %d takes plate %u off -- %hs", a.slot, plate,
                prop ? "taken" : ok ? "the plate's prop was not made" : reach ? "refused by the desk" : "out of reach, refused");
    } else {  // a plate set back
        void* const prop = EL::LiveActorOfType(static_cast<EL::ElementId>(a.p.eid), EL::ElementType::Prop);
        ue_wrap::FVector at{};
        const bool near = prop && mesh && E::TryGetActorLocation(prop, at) &&
                          Dist2(at, E::GetComponentLocation(mesh)) <= kPlateNearCm * kPlateNearCm;
        const bool ok = near && PlateOfClass(prop) == plate && !PlateOn(bits, plate);
        if (ok) {
            WritePlates(desk, static_cast<uint8_t>(bits | (1u << plate)));
            CC::CallParamlessNamed(prop, L"K2_DestroyActor");
            SoundDetach(desk, at);
        }
        UE_LOGI("cover_sync: HOST slot %d sets plate %u back (prop eid %u) -- %hs", a.slot, plate, a.p.eid,
                ok ? "taken" : PlateOn(bits, plate) ? "already on" : "refused");
    }
    g_nextScanMs = 0;  // the change goes out at once
}

void JudgeDoor(const Ask& a) {
    void* const gen = Station(a.p.index);
    ue_wrap::FVector eye{}, at{};
    const bool seen = gen && EyeOf(a.slot, eye) && E::TryGetActorLocation(gen, at);
    const bool reach = seen && Dist2(eye, at) <= kDoorReachCm * kDoorReachCm;
    if (!reach)
        UE_LOGI("cover_sync: HOST slot %d's eye (%.0f,%.0f,%.0f) is %.0f cm from station %u at (%.0f,%.0f,%.0f)", a.slot,
                eye.X, eye.Y, eye.Z, seen ? std::sqrt(Dist2(eye, at)) : -1.f, a.p.index, at.X, at.Y, at.Z);
    const bool want = (a.p.bits & 1u) != 0;
    const bool ok = reach && !BoolOr(gen, L"moving", true) && BoolOr(gen, L"opened", !want) != want;
    if (ok) OpenDoor(gen, want);
    UE_LOGI("cover_sync: HOST slot %d %hs station %u's door -- %hs", a.slot, want ? "opens" : "shuts", a.p.index,
            ok ? "taken" : reach ? "refused by the station" : "out of reach, refused");
}

void TickHost() {
    PayOwed();
    if (g_holdTakeOffUntilMs && NowMs() < g_holdTakeOffUntilMs) {
        if (void* desk = Desk()) WriteBool(desk, L"panelTakeoff", true);
    } else {
        g_holdTakeOffUntilMs = 0;
    }
    std::vector<Ask> asks;
    asks.swap(g_asks);
    for (const Ask& a : asks) {
        ue_wrap::FVector eye{};
        if (!EyeOf(a.slot, eye) && NowMs() - a.since < kBodyWaitMs) {  // a joiner's first pose follows its ready edge
            g_asks.push_back(a);
            continue;
        }
        if (a.p.what == N::kCoverDesk) JudgeDesk(a);
        else JudgeDoor(a);
    }
    const uint64_t now = NowMs();
    if (now < g_nextScanMs) return;
    g_nextScanMs = now + kScanEveryMs;
    uint8_t bits = 0;
    if (void* desk = Desk(); desk && ReadPlates(desk, bits) && (!g_deskSaid || bits != g_deskSent)) {
        if (g_deskSaid) SendDesk(bits, -1);
        if (g_deskSaid) UE_LOGI("cover_sync: HOST the desk's plates on=0x%x unbolted=0x%x -- sent", bits & 0xF, bits >> 4);
        g_deskSaid = true;
        g_deskSent = bits;
    }
    std::vector<void*> gens;
    GEN::ReadGenerators(gens);
    if (g_doorSent.size() < gens.size()) g_doorSent.resize(gens.size(), -1);
    for (size_t i = 0; i < gens.size(); ++i) {
        bool open = false;
        if (!gens[i] || !ReadBool(gens[i], L"opened", open)) continue;
        if (g_doorSent[i] == (open ? 1 : 0)) continue;
        if (g_doorSent[i] >= 0) {
            SendDoor(static_cast<int>(i), open, -1);
            UE_LOGI("cover_sync: HOST station %zu's door %hs -- sent", i, open ? "opened" : "shut");
        }
        g_doorSent[i] = open ? 1 : 0;
    }
}

// ---- the client ----------------------------------------------------------------------------------------------------
void Ask(uint8_t op, uint8_t what, uint8_t index, uint8_t bits, uint32_t eid);

void* MemberObj(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(obj) + off) : nullptr;
}

bool Touches(void* lid, void* actor) {
    ue_wrap::ParamFrame f(lid ? R::FindDispatchFunctionCached(R::ClassOf(lid), L"IsOverlappingActor") : nullptr);
    return f.valid() && f.Set<void*>(L"Other", actor) && ue_wrap::Call(lid, f) && f.Get<bool>(L"ReturnValue");
}

uint64_t g_lastPutBackAskMs = 0;
bool g_armed[N::kCoverDeskPanels] = {};  // a plate's prop has been seen off its lid since the plate came off

// A plate prop touching the lid of a plate that is off, here: asked of the host, which sets it back on its desk. The
// desk's own set-back never runs on a client (panelTakeoff is held), so a copy born or carried there is never eaten.
void AskPutBacks(void* desk, uint8_t bits) {
    const uint64_t now = NowMs();
    for (int i = 0; i < N::kCoverDeskPanels; ++i) {
        if (PlateOn(bits, i) || i == kFixedPlate) {
            g_armed[i] = false;  // as the desk's panelTakeoff: a prop born on its lid is not set back until it has left
            continue;
        }
        void* const lid = MemberObj(desk, kLid[i]);
        void* const cls = R::FindClass(kPlateClass[i]);
        if (!lid || !cls) continue;
        std::vector<void*> props;
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &props);
        bool touching = false;
        for (void* prop : props) {
            const EL::ElementId eid = EL::Registry::Get().EidForActor(prop);
            if (eid == EL::kInvalidId || !Touches(lid, prop)) continue;
            touching = true;
            if (!g_armed[i] || now - g_lastPutBackAskMs < 1000) break;
            g_lastPutBackAskMs = now;
            Ask(N::kCoverOpPutBack, N::kCoverDesk, static_cast<uint8_t>(i), 0, static_cast<uint32_t>(eid));
            return;
        }
        if (!touching && !props.empty()) g_armed[i] = true;  // the prop is here, and off its lid
    }
}
bool g_deskWanted = false;   // the host's plates, held until the desk is read
bool g_haveDesk = false;     // the host's plates are known: the desk is kept at them
uint8_t g_deskBits = 0;
uint64_t g_nextDeskCheckMs = 0;
std::vector<int8_t> g_doorWanted;  // by station: -1 none, else the host's opened
struct Pickup { uint32_t eid = 0; uint64_t since = 0; };
Pickup g_pickup;

void TickClient() {
    // The desk's own set-back returns at once while panelTakeoff is set: held every frame on a client, so a plate
    // prop born or carried onto its lid here is never eaten by this copy.
    static ue_wrap::CachedObjRef s_desk;
    void* held = s_desk.Get();
    if (!held) {
        held = Desk();
        s_desk.Set(held);
    }
    if (held) WriteBool(held, L"panelTakeoff", true);
    // The desk is kept at the host's plates: a write when they arrive, and a check every 100 ms after, which rewrites
    // anything that moved them here.
    const uint64_t now = NowMs();
    if ((g_deskWanted || (g_haveDesk && now >= g_nextDeskCheckMs))) {
        g_nextDeskCheckMs = now + kScanEveryMs;
        if (void* desk = Desk()) {
            uint8_t bits = 0;
            const bool read = ReadPlates(desk, bits);
            if (!read || bits != g_deskBits) {
                WritePlates(desk, g_deskBits);
                if (g_deskWanted)
                    UE_LOGI("cover_sync: CLIENT the desk's plates on=0x%x unbolted=0x%x, as the host's", g_deskBits & 0xF,
                            g_deskBits >> 4);
                else
                    UE_LOGW("cover_sync: CLIENT the desk's plates moved here to on=0x%x unbolted=0x%x -- set back to the "
                            "host's on=0x%x unbolted=0x%x", bits & 0xF, bits >> 4, g_deskBits & 0xF, g_deskBits >> 4);
            }
            g_deskWanted = false;
            AskPutBacks(desk, g_deskBits);
        }
    }
    for (size_t i = 0; i < g_doorWanted.size(); ++i) {
        if (g_doorWanted[i] < 0) continue;
        void* const gen = Station(static_cast<int>(i));
        bool open = false;
        if (!gen || !ReadBool(gen, L"opened", open) || BoolOr(gen, L"moving", true)) continue;
        const bool want = g_doorWanted[i] == 1;
        if (open != want) {
            OpenDoor(gen, want);
            UE_LOGI("cover_sync: CLIENT station %zu's door %hs, as the host's", i, want ? "opens" : "shuts");
        }
        g_doorWanted[i] = -1;
    }
    if (g_pickup.eid) {
        void* const prop = EL::LiveActorOfType(static_cast<EL::ElementId>(g_pickup.eid), EL::ElementType::Prop);
        void* const me = coop::players::Registry::Get().Local();
        if (prop && me) {
            ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(me), L"pickupObjectDirect"));
            const bool ok = f.valid() && f.Set<void*>(L"actor", prop) && ue_wrap::Call(me, f);
            UE_LOGI("cover_sync: CLIENT the host's plate eid %u taken into the hand (ok=%d)", g_pickup.eid, ok ? 1 : 0);
            g_pickup = Pickup{};
        } else if (NowMs() - g_pickup.since > kPickupWaitMs) {
            UE_LOGW("cover_sync: CLIENT the host's plate eid %u did not arrive in 3 s -- left where it was made", g_pickup.eid);
            g_pickup = Pickup{};
        }
    }
}

// ---- the gate ------------------------------------------------------------------------------------------------------
bool Paced() {
    static uint64_t s_lastMs = 0;
    const uint64_t now = NowMs();
    if (now - s_lastMs < kAskEveryMs) return false;
    s_lastMs = now;
    return true;
}

void Ask(uint8_t op, uint8_t what, uint8_t index, uint8_t bits, uint32_t eid) {
    N::CoverPayload p{};
    p.op = op;
    p.what = what;
    p.index = index;
    p.bits = bits;
    p.eid = eid;
    const bool ok = g_session->SendReliable(N::ReliableKind::CoverIntent, &p, sizeof(p));
    UE_LOGI("cover_sync: CLIENT asked the host op %u of %hs %u (ok=%d)", op, what == N::kCoverDesk ? "plate" : "station",
            index, ok ? 1 : 0);
}

// The end of a client's timed unbolt: asked of the host, and the unbolt's widget taken down here.
sg::Verdict OnUnscrewPre(const sg::Call& c) {
    if (!IsClient() || g_applying || !c.object) return sg::Verdict::Run;
    const int32_t plate = IntOr(c.object, L"lookingAtPanel", -1);
    if (plate >= 0 && plate < N::kCoverDeskPanels) Ask(N::kCoverOpUnscrew, N::kCoverDesk, static_cast<uint8_t>(plate), 0, 0);
    g_applying = true;
    CC::CallParamlessNamed(c.object, L"resetUnscrew");
    g_applying = false;
    return sg::Verdict::Cancel;
}

// A client's E press on an unbolted plate that is on: a take-off, asked of the host. Every other press runs.
sg::Verdict OnDeskActionPre(const sg::Call& c) {
    if (!IsClient() || g_applying || !c.object) return sg::Verdict::Run;
    const int32_t plate = IntOr(c.object, L"lookingAtPanel", -1);
    uint8_t bits = 0;
    if (plate < 0 || plate >= N::kCoverDeskPanels || BoolOr(c.object, L"panelsProcess", true) ||
        !ReadPlates(c.object, bits) || !PlateLoose(bits, plate) || !PlateOn(bits, plate))
        return sg::Verdict::Run;
    if (Paced()) Ask(N::kCoverOpTakeOff, N::kCoverDesk, static_cast<uint8_t>(plate), 0, 0);
    return sg::Verdict::Cancel;
}

// A plate touching its place on a client's desk: nothing is set back here. The client's put-back is asked of the host by
// AskPutBacks, once the plate's prop has left its lid and comes back to it, as the desk's own panelTakeoff waits.
sg::Verdict OnDeskOverlapPre(const sg::Call&) {
    return IsClient() && !g_applying ? sg::Verdict::Cancel : sg::Verdict::Run;
}

// A client's E press on a station's door: asked of the host. Every other press runs.
sg::Verdict OnStationActionPre(const sg::Call& c) {
    if (!IsClient() || g_applying || !c.object || !c.function || !c.locals) return sg::Verdict::Run;
    const int32_t off = R::FindParamOffset(c.function, L"action");
    if (off < 0 || c.locals[off] != kActionUse || !BoolOr(c.object, L"lookAtDoor", false)) return sg::Verdict::Run;
    const int32_t index = GEN::IndexOf(c.object);
    if (index < 0 || index > 255) return sg::Verdict::Run;
    if (Paced()) Ask(N::kCoverOpDoor, N::kCoverStation, static_cast<uint8_t>(index), BoolOr(c.object, L"opened", false) ? 0 : 1, 0);
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    if (g_watched) return;
    g_watched = true;
    const bool u = sg::WatchName(kRowUnscrew, kTagUnscrew, &OnUnscrewPre, nullptr);
    const bool a = sg::WatchName(kRowDeskAction, kTagDeskAction, &OnDeskActionPre, nullptr);
    const bool o = sg::WatchName(kRowDeskOverlap, kTagDeskOverlap, &OnDeskOverlapPre, nullptr);
    const bool s = sg::WatchName(kRowStationAction, kTagStationAction, &OnStationActionPre, nullptr);
    if (!u || !a || !o || !s)
        UE_LOGE("cover_sync: a watch was refused (unscrew=%d desk=%d overlap=%d station=%d)", u ? 1 : 0, a ? 1 : 0,
                o ? 1 : 0, s ? 1 : 0);
}

void Tick() {
    if (IsHost() && g_session->connected()) TickHost();
    else if (IsClient()) TickClient();
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowUnscrew, kTagUnscrew) || !sg::NameWatchSettled(kRowDeskAction, kTagDeskAction) ||
        !sg::NameWatchSettled(kRowDeskOverlap, kTagDeskOverlap) ||
        !sg::NameWatchSettled(kRowStationAction, kTagStationAction))
        return;
    g_settled = true;
    const bool u = sg::NameWatchLive(kRowUnscrew, kTagUnscrew), a = sg::NameWatchLive(kRowDeskAction, kTagDeskAction),
               o = sg::NameWatchLive(kRowDeskOverlap, kTagDeskOverlap),
               s = sg::NameWatchLive(kRowStationAction, kTagStationAction);
    if (u && a && o && s) UE_LOGI("cover_sync: the watches on the desk's plates and the stations' door press are live");
    else UE_LOGE("cover_sync: a watch is dead (unscrew=%d desk=%d overlap=%d station=%d) -- a client's cover runs its own",
                 u ? 1 : 0, a ? 1 : 0, o ? 1 : 0, s ? 1 : 0);
}

void SendJoinSnapshotForSlot(int slot) {
    if (!IsHost() || slot <= 0) return;
    uint8_t bits = 0;
    if (void* desk = Desk(); desk && ReadPlates(desk, bits)) SendDesk(bits, slot);
    std::vector<void*> gens;
    GEN::ReadGenerators(gens);
    for (size_t i = 0; i < gens.size() && i < 256; ++i) {
        bool open = false;
        if (gens[i] && ReadBool(gens[i], L"opened", open)) SendDoor(static_cast<int>(i), open, slot);
    }
}

void OnIntent(int senderSlot, const coop::net::CoverPayload& p) {
    if (!IsHost() || senderSlot <= 0 || senderSlot >= static_cast<int>(N::kMaxPeers) || g_asks.size() >= 64) return;
    const bool desk = p.what == N::kCoverDesk &&
                      (p.op == N::kCoverOpUnscrew || p.op == N::kCoverOpTakeOff || p.op == N::kCoverOpPutBack) &&
                      p.index < N::kCoverDeskPanels;
    const bool door = p.what == N::kCoverStation && p.op == N::kCoverOpDoor;
    if (!desk && !door) return;
    const uint64_t now = NowMs();
    if (now - g_lastAskMs[senderSlot] < kAskEveryMs) return;
    g_lastAskMs[senderSlot] = now;
    g_asks.push_back({senderSlot, p, now});
}

void OnState(const coop::net::CoverPayload& p) {
    if (!IsClient()) return;
    if (p.op == N::kCoverOpAnswer) {
        if (p.eid && p.index < N::kCoverDeskPanels) g_pickup = Pickup{p.eid, NowMs()};
        else UE_LOGI("cover_sync: CLIENT the host's desk refused this player's take-off of plate %u", p.index);
        return;
    }
    if (p.op != N::kCoverOpState) return;
    if (p.what == N::kCoverDesk) {
        g_deskBits = p.bits;
        g_deskWanted = true;
        g_haveDesk = true;
    } else if (p.what == N::kCoverStation) {
        if (g_doorWanted.size() <= p.index) g_doorWanted.resize(static_cast<size_t>(p.index) + 1, -1);
        g_doorWanted[p.index] = (p.bits & 1u) ? 1 : 0;
    }
}

void OnDisconnect() {
    g_asks.clear();
    g_owed.clear();
    for (uint64_t& t : g_lastAskMs) t = 0;
    g_deskSaid = false;
    g_doorSent.clear();
    g_deskWanted = false;
    g_haveDesk = false;
    for (bool& a : g_armed) a = false;
    g_doorWanted.clear();
    g_pickup = Pickup{};
}

}  // namespace coop::cover_sync
