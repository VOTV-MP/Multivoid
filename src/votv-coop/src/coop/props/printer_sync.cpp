// coop/props/printer_sync.cpp -- a 3D print and the object it prints, the host's. See the header.

#include "coop/props/printer_sync.h"

#include "coop/comms/peer_action_feed.h"
#include "coop/element/identity_create.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/slot_count.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/printer_models.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::printer_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace N  = coop::net;
namespace OI = ue_wrap::object_index;
namespace PM = coop::printer_models;
namespace PT = coop::prop_element_tracker;
namespace sg = ue_wrap::script_gate;
using coop::element::ElementId;
using ue_wrap::FRotator;
using ue_wrap::FVector;

std::atomic<N::Session*> g_session{nullptr};

UE_FACT_FN(kRowPrint, "printer_sync", L"3dPrinterAnim_C", L"print", Declared);
UE_FACT_FN(kRowAction, "printer_sync", L"printedObject_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowLoad, "printer_sync", L"printedObject_C", L"load", Declared);
UE_FACT_FN(kRowFromPrinter, "printer_sync", L"printedObject_C", L"loadFromPrinter", Declared);
constexpr int kTag = 0x50520001;  // 'PR' 1

constexpr uint64_t kCheckMs = 5000;    // a client that has not answered by then is taken as lacking the model
constexpr uint64_t kScanMs = 250;      // the host's walk of the printed objects
constexpr uint64_t kRestReadMs = 500;  // and of where each rests
constexpr float kReachCm = 800.f;
constexpr float kMovedCm = 2.f, kTurnedDeg = 3.f;
constexpr uint8_t kActionEat = 4, kActionLight = 5;

uint64_t Now() { return ::GetTickCount64(); }

// This lane's own calls of the watched functions, which its gates let run. (Not the gate's fromOurCode: any other mod
// call, a probe's press included, is judged as the game's.)
bool g_ours = false;
struct Ours {
    bool was;
    Ours() : was(g_ours) { g_ours = true; }
    ~Ours() { g_ours = was; }
};

N::Session* Live() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected()) ? s : nullptr;
}
bool IsHost() { N::Session* s = Live(); return s && s->role() == N::Role::Host; }
bool IsClient() { N::Session* s = Live(); return s && s->role() == N::Role::Client; }

void* g_printedClass = nullptr;
void* PrintedClass() {
    if (!g_printedClass) g_printedClass = OI::ClassByName(L"printedObject_C");
    return g_printedClass;
}
bool IsPrinted(void* a) { return a && PrintedClass() && R::ClassOf(a) == g_printedClass; }

// The printer and its subclasses: 3dPrinter_desktop_C declares nothing of its own and runs this graph.
bool IsPrinter(void* a) {
    void* const base = a ? OI::ClassByName(L"3dPrinterAnim_C") : nullptr;
    return base && R::IsDescendantOfAny(R::ClassOf(a), &base, 1, 8);
}

std::vector<void*> AllPrinted() {
    std::vector<void*> out;
    if (void* const c = PrintedClass())
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

int32_t Off(void* obj, const wchar_t* name) { return obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1; }
template <class T>
T Get(void* obj, const wchar_t* name, T dflt) {
    const int32_t o = Off(obj, name);
    return o >= 0 ? *reinterpret_cast<const T*>(static_cast<const char*>(obj) + o) : dflt;
}
template <class T>
void Put(void* obj, const wchar_t* name, T v) {
    const int32_t o = Off(obj, name);
    if (o >= 0) *reinterpret_cast<T*>(static_cast<char*>(obj) + o) = v;
}
std::wstring Str(void* obj, const wchar_t* name) {
    const int32_t o = Off(obj, name);
    return o >= 0 ? ue_wrap::field_io::ReadFStringAt(obj, o) : std::wstring();
}
std::wstring KeyOf(void* obj) {
    const R::FName k = Get<R::FName>(obj, L"key", R::FName{0, 0});
    if (k.ComparisonIndex == 0) return {};
    std::wstring s = R::ToString(k);
    return s == L"None" ? std::wstring() : s;
}

bool CallNoArgs(void* obj, const wchar_t* fn) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && ue_wrap::Call(obj, frame);
}

ElementId EidOf(void* actor) { return actor ? coop::element::Registry::Get().EidForActor(actor) : coop::element::kInvalidId; }
void* ActorOf(uint32_t eid) {
    coop::element::Element* el = coop::element::Registry::Get().Get(static_cast<ElementId>(eid));
    void* const a = el ? el->GetActor() : nullptr;
    return (a && R::IsLiveByIndex(a, el->GetInternalIdx())) ? a : nullptr;
}

template <size_t M>
uint8_t PutText(char (&dst)[M], const std::string& s) {
    const size_t n = s.size() < M ? s.size() : M;
    std::memcpy(dst, s.data(), n);
    return static_cast<uint8_t>(n);
}
std::wstring TextOf(const char* s, uint8_t len, size_t max) { return PM::FromUtf8(s, len <= max ? len : max); }

bool InReach(int slot, void* actor) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    FVector p{}, a{};
    if (!rp || !rp->valid() || !rp->TryGetLocation(p) || !E::TryGetActorLocation(actor, a)) return false;
    const float x = p.X - a.X, y = p.Y - a.Y, z = p.Z - a.Z;
    return x * x + y * y + z * z <= kReachCm * kReachCm;
}

void Say(uint8_t subject, uint8_t reason, const std::wstring& name) {
    coop::peer_action_feed::AnnounceDirect(subject, reason == N::kPrintDiffers
                                                        ? coop::peer_action_feed::Action::PrintModelDiffers
                                                        : coop::peer_action_feed::Action::PrintModelMissing,
                                           name);
}

// The object as a hidden holder of its identity: no mesh, no collision, no physics, so the game's own load does not
// destroy it for a missing file and nothing of another model stands in its place.
void MakePlaceholder(void* actor) {
    if (void* const mesh = Get<void*>(actor, L"proceduralMesh", nullptr)) {
        CallNoArgs(mesh, L"ClearAllMeshSections");
        CallNoArgs(mesh, L"ClearCollisionConvexMeshes");
    }
    E::SetActorSimulatePhysics(actor, false);
    E::SetActorRootCollisionEnabled(actor, 0);
    E::SetActorHiddenInGame(actor, true);
}

// ---- the host: prints ----------------------------------------------------------------------------

struct Check {
    uint32_t request = 0;
    void* printer = nullptr;
    int32_t printerIdx = -1;
    std::wstring name;
    int requester = 0;
    uint64_t hostHash = 0;
    uint64_t deadline = 0;
    bool asked[coop::players::kMaxPeers] = {};
    bool answered[coop::players::kMaxPeers] = {};
    uint64_t hash[coop::players::kMaxPeers] = {};
};
std::vector<Check> g_checks;
uint32_t g_nextRequest = 1;
uint64_t g_printsRun = 0, g_printsRefused = 0;

void NoticeTo(int slot, uint8_t subject, uint8_t reason, const std::wstring& name) {
    N::Session* s = Live();
    if (!s || slot <= 0) return;
    N::PrintNoticePayload p{};
    p.subject = subject;
    p.reason = reason;
    p.nameLen = PutText(p.name, PM::ToUtf8(name));
    s->SendReliableToSlot(slot, N::ReliableKind::PrintNotice, &p, sizeof(p));
}

void StartCheck(void* printer, const std::wstring& name, int requester) {
    N::Session* s = Live();
    if (!s) return;
    Check c;
    c.request = g_nextRequest++;
    c.printer = printer;
    c.printerIdx = R::InternalIndexOf(printer);
    c.name = name;
    c.requester = requester;
    c.hostHash = PM::ModelHash(name);
    c.deadline = Now() + kCheckMs;
    N::PrintCheckPayload q{};
    q.request = c.request;
    q.nameLen = PutText(q.name, PM::ToUtf8(name));
    int asked = 0;
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        if (!s->IsSlotConnected(slot) || !s->IsSlotWorldReady(slot)) continue;
        if (s->SendReliableToSlot(slot, N::ReliableKind::PrintCheck, &q, sizeof(q))) {
            c.asked[slot] = true;
            ++asked;
        }
    }
    UE_LOGI("printer_sync: slot %d's print of '%ls' (host hash %016llx) waits on %d player(s), request %u", requester,
            name.c_str(), static_cast<unsigned long long>(c.hostHash), asked, c.request);
    g_checks.push_back(std::move(c));
}

void Finish(Check& c) {
    std::vector<std::pair<int, uint8_t>> lacking;
    if (!c.hostHash) lacking.emplace_back(0, N::kPrintLacks);
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        if (!c.asked[slot]) continue;
        if (!c.answered[slot] || !c.hash[slot]) lacking.emplace_back(slot, N::kPrintLacks);
        else if (c.hash[slot] != c.hostHash) lacking.emplace_back(slot, N::kPrintDiffers);
    }
    if (!R::IsLiveByIndex(c.printer, c.printerIdx)) return;
    if (lacking.empty()) {
        Ours ours;
        const bool ran = CallNoArgs(c.printer, L"print");
        ++g_printsRun;
        UE_LOGI("printer_sync: every player holds '%ls' -- printing (request %u, ran=%d)", c.name.c_str(), c.request,
                ran ? 1 : 0);
        return;
    }
    ++g_printsRefused;
    for (const auto& [slot, reason] : lacking) {
        UE_LOGI("printer_sync: '%ls' refused (request %u): slot %d %s it", c.name.c_str(), c.request, slot,
                reason == N::kPrintDiffers ? "holds another copy of" : "lacks");
        Say(static_cast<uint8_t>(slot), reason, c.name);
        NoticeTo(c.requester, static_cast<uint8_t>(slot), reason, c.name);
        if (slot != c.requester) NoticeTo(slot, static_cast<uint8_t>(slot), reason, c.name);
    }
}

void TickChecks() {
    const uint64_t now = Now();
    for (auto it = g_checks.begin(); it != g_checks.end();) {
        bool all = true;
        for (int slot = 1; slot < coop::players::kMaxPeers; ++slot)
            if (it->asked[slot] && !it->answered[slot]) all = false;
        if (all || now >= it->deadline) {
            Finish(*it);
            it = g_checks.erase(it);
        } else {
            ++it;
        }
    }
}

// The script gate's print. The host's own is held for the check while a client is connected; a client's is asked of the
// host. Our own call (the check's end) runs.
sg::Verdict OnPrintPre(const sg::Call& c) {
    if (!c.object || g_ours) return sg::Verdict::Run;
    N::Session* s = Live();
    if (!s) return sg::Verdict::Run;
    if (Get<uint8_t>(c.object, L"active", 0)) return sg::Verdict::Run;  // the game's own no-op
    const std::wstring name = Str(c.object, L"meshName");
    if (name.empty()) return sg::Verdict::Run;
    if (s->role() == N::Role::Host) {
        if (s->connectedPeerCount() <= 0) return sg::Verdict::Run;
        for (const Check& k : g_checks)
            if (k.printer == c.object) return sg::Verdict::Cancel;  // one check at a time per printer
        StartCheck(c.object, name, 0);
        return sg::Verdict::Cancel;
    }
    const ElementId eid = EidOf(c.object);
    if (eid == coop::element::kInvalidId) return sg::Verdict::Run;  // a printer the host does not know is this peer's
    N::PrintAskPayload p{};
    p.printerEid = static_cast<uint32_t>(eid);
    p.nameLen = PutText(p.name, PM::ToUtf8(name));
    s->SendReliable(N::ReliableKind::PrintAsk, &p, sizeof(p));
    UE_LOGI("printer_sync: this client's print of '%ls' on eid=%u asked of the host", name.c_str(), p.printerEid);
    return sg::Verdict::Cancel;
}

// ---- the host: printed objects -------------------------------------------------------------------

struct Tracked {
    void* actor = nullptr;
    int32_t idx = -1;
    std::wstring key, name;
    uint64_t hash = 0;
    FVector loc{};
    FRotator rot{};
    uint8_t light = 0;
    int32_t bites = 0;
};
std::unordered_map<uint32_t, Tracked> g_tracked;  // host, by element id
uint64_t g_scanAt = 0, g_restAt = 0;
std::set<std::pair<int, std::wstring>> g_told;  // host: which slot was told it lacks which model

bool MakeState(uint32_t eid, Tracked& t, N::PrintedStatePayload& p) {
    p = N::PrintedStatePayload{};
    p.eid = eid;
    p.op = N::kPrintedBorn;
    p.light = t.light;
    p.bites = t.bites;
    p.hash = t.hash;
    p.loc[0] = t.loc.X; p.loc[1] = t.loc.Y; p.loc[2] = t.loc.Z;
    p.rot[0] = t.rot.Pitch; p.rot[1] = t.rot.Yaw; p.rot[2] = t.rot.Roll;
    p.keyLen = PutText(p.key, PM::ToUtf8(t.key));
    p.nameLen = PutText(p.name, PM::ToUtf8(t.name));
    return true;
}

void SendState(int slot, uint32_t eid, Tracked& t) {
    N::Session* s = Live();
    if (!s) return;
    N::PrintedStatePayload p{};
    MakeState(eid, t, p);
    if (slot < 0) s->SendReliable(N::ReliableKind::PrintedState, &p, sizeof(p));
    else s->SendReliableToSlot(slot, N::ReliableKind::PrintedState, &p, sizeof(p));
}

void ReadPose(Tracked& t) {
    E::TryGetActorLocation(t.actor, t.loc);
    E::TryGetActorRotation(t.actor, t.rot);
}

void Enroll(void* actor) {
    const std::wstring name = Str(actor, L"name");
    if (name.empty()) return;  // its print has not handed it its model yet
    // GetKey mints the save key (lib_C's assignKey) as the game's own save would.
    std::wstring key;
    if (void* const fn = R::FindDispatchFunctionCached(R::ClassOf(actor), L"GetKey")) {
        ue_wrap::ParamFrame f(fn);
        if (f.valid() && ue_wrap::Call(actor, f)) {
            const R::FName k = f.Get<R::FName>(L"key");
            if (k.ComparisonIndex) key = R::ToString(k);
        }
    }
    // A host element it already holds (a session before this one) is kept.
    ElementId eid = EidOf(actor);
    if (eid == coop::element::kInvalidId) {
        PT::MarkPropElement(actor, L"", R::ClassNameOf(actor), PT::EnrollSource::kExpressSeam);
        eid = PT::GetPropElementIdForActor(actor);
    }
    if (eid == coop::element::kInvalidId) {
        UE_LOGW("printer_sync: no element for printed '%ls' -- it stays the host's alone", name.c_str());
        return;
    }
    Tracked t;
    t.actor = actor;
    t.idx = R::InternalIndexOf(actor);
    t.key = key;
    t.name = name;
    t.hash = PM::ModelHash(name);
    t.light = Get<uint8_t>(actor, L"lightActive", 0);
    t.bites = Get<int32_t>(actor, L"bites", 0);
    ReadPose(t);
    auto& slot = g_tracked[static_cast<uint32_t>(eid)] = t;
    UE_LOGI("printer_sync: printed '%ls' key '%ls' is eid=%u (hash %016llx), sent to every peer", name.c_str(),
            key.c_str(), static_cast<unsigned>(eid), static_cast<unsigned long long>(t.hash));
    SendState(-1, static_cast<uint32_t>(eid), slot);
}

void HostTick() {
    const uint64_t now = Now();
    if (now >= g_scanAt) {
        g_scanAt = now + kScanMs;
        for (void* a : AllPrinted()) {
            const ElementId e = EidOf(a);
            if (e == coop::element::kInvalidId || !g_tracked.count(static_cast<uint32_t>(e))) Enroll(a);
        }
        for (auto it = g_tracked.begin(); it != g_tracked.end();) {
            if (R::IsLiveByIndex(it->second.actor, it->second.idx)) { ++it; continue; }
            N::PrintedStatePayload p{};
            p.eid = it->first;
            p.op = N::kPrintedGone;
            if (N::Session* s = Live()) s->SendReliable(N::ReliableKind::PrintedState, &p, sizeof(p));
            UE_LOGI("printer_sync: printed eid=%u is gone on the host", it->first);
            it = g_tracked.erase(it);
        }
    }
    if (now < g_restAt) return;
    g_restAt = now + kRestReadMs;
    for (auto& [eid, t] : g_tracked) {
        const uint8_t light = Get<uint8_t>(t.actor, L"lightActive", 0);
        const int32_t bites = Get<int32_t>(t.actor, L"bites", 0);
        bool changed = light != t.light || bites != t.bites;
        t.light = light;
        t.bites = bites;
        // Where it rests: sent once it is at rest and moved, since each peer's own physics carries it in between
        // (and the held-prop stream while a player holds it).
        FVector at{};
        FRotator rot{};
        if (E::TryGetActorLocation(t.actor, at) && E::TryGetActorRotation(t.actor, rot) &&
            E::IsActorRootSimulatingPhysics(t.actor) && E::IsActorRootBodyAtRest(t.actor)) {
            const float dx = at.X - t.loc.X, dy = at.Y - t.loc.Y, dz = at.Z - t.loc.Z;
            const float turn = std::fabs(ue_wrap::NormalizeAxis(rot.Pitch - t.rot.Pitch)) +
                               std::fabs(ue_wrap::NormalizeAxis(rot.Yaw - t.rot.Yaw)) +
                               std::fabs(ue_wrap::NormalizeAxis(rot.Roll - t.rot.Roll));
            if (dx * dx + dy * dy + dz * dz > kMovedCm * kMovedCm || turn > kTurnedDeg) {
                t.loc = at;
                t.rot = rot;
                changed = true;
            }
        }
        if (changed) SendState(-1, eid, t);
    }
}

// ---- the client ----------------------------------------------------------------------------------

std::set<uint32_t> g_reported;  // client: the objects it told the host it lacks
uint64_t g_spawned = 0, g_adopted = 0, g_placeholders = 0;

void ReportLacking(uint32_t eid, uint8_t reason) {
    N::Session* s = Live();
    if (!s || !g_reported.insert(eid).second) return;
    N::PrintedIntentPayload p{};
    p.eid = eid;
    p.op = N::kPrintedLacking;
    p.arg = reason;
    s->SendReliable(N::ReliableKind::PrintedIntent, &p, sizeof(p));
}

void* FindByKey(const std::wstring& key) {
    if (key.empty()) return nullptr;
    for (void* a : AllPrinted())
        if (EidOf(a) == coop::element::kInvalidId && KeyOf(a) == key) return a;
    return nullptr;
}

void* SpawnCopy(const N::PrintedStatePayload& p, const std::wstring& key, const std::wstring& name) {
    void* const cls = PrintedClass();
    if (!cls) return nullptr;
    const FVector at{p.loc[0], p.loc[1], p.loc[2]};
    const FRotator rot{p.rot[0], p.rot[1], p.rot[2]};
    void* const a = E::BeginDeferredSpawn(cls, at, rot);
    if (!a) return nullptr;
    const int32_t nameOff = Off(a, L"name");
    if (nameOff >= 0) ue_wrap::field_io::WriteFStringField(a, nameOff, name);
    if (!key.empty()) Put<R::FName>(a, L"key", ue_wrap::fname_utils::StringToFName(key));
    if (!E::FinishDeferredSpawn(a, at, rot)) return nullptr;
    return a;
}

void ApplyState(const N::PrintedStatePayload& p) {
    const std::wstring key = TextOf(p.key, p.keyLen, N::kPrintKeyMax);
    const std::wstring name = TextOf(p.name, p.nameLen, N::kPrintNameMax);
    void* actor = ActorOf(p.eid);
    bool fresh = false;
    if (!actor) {
        if ((actor = FindByKey(key))) {
            ++g_adopted;
        } else if ((actor = SpawnCopy(p, key, name))) {
            ++g_spawned;
            fresh = true;
        } else {
            UE_LOGW("printer_sync: could not make printed eid=%u ('%ls')", p.eid, name.c_str());
            return;
        }
        coop::element::CreateOrAdoptPropMirror(static_cast<ElementId>(p.eid), actor, L"", R::ClassNameOf(actor), 0,
                                               false);
        const uint64_t mine = PM::ModelHash(name);
        const bool same = mine && mine == p.hash;
        if (same) {
            if (!Get<uint8_t>(actor, L"loaded", 0)) {
                void* const fn = R::FindDispatchFunctionCached(R::ClassOf(actor), L"load");
                ue_wrap::ParamFrame f(fn);
                if (fn && f.valid()) { Ours ours; ue_wrap::Call(actor, f); }  // configInfo false: no printer widget is read
            }
        } else {
            MakePlaceholder(actor);
            ++g_placeholders;
            ReportLacking(p.eid, mine ? N::kPrintDiffers : N::kPrintLacks);
        }
        UE_LOGI("printer_sync: printed eid=%u '%ls' key '%ls' %s here, %s (mine %016llx, host %016llx)", p.eid,
                name.c_str(), key.c_str(), fresh ? "spawned" : "adopted",
                same ? "built from this player's file" : "a placeholder", static_cast<unsigned long long>(mine),
                static_cast<unsigned long long>(p.hash));
    }
    if (!R::IsLive(actor)) return;
    Put<int32_t>(actor, L"bites", p.bites);
    if (Get<uint8_t>(actor, L"lightActive", 0) != p.light) {
        Put<uint8_t>(actor, L"lightActive", p.light);
        CallNoArgs(actor, L"updLight");
    }
    // Where the host's copy rests, unless this player holds this one.
    if (fresh) return;
    void* const me = coop::players::Registry::Get().Local();
    if (me && E::IsMainPlayerGrabbing(me, actor)) return;
    FVector at{};
    FRotator rot{};
    if (!E::TryGetActorLocation(actor, at) || !E::TryGetActorRotation(actor, rot)) return;
    const FVector to{p.loc[0], p.loc[1], p.loc[2]};
    const FRotator tr{p.rot[0], p.rot[1], p.rot[2]};
    const float dx = at.X - to.X, dy = at.Y - to.Y, dz = at.Z - to.Z;
    const float turn = std::fabs(ue_wrap::NormalizeAxis(rot.Pitch - tr.Pitch)) +
                       std::fabs(ue_wrap::NormalizeAxis(rot.Yaw - tr.Yaw)) +
                       std::fabs(ue_wrap::NormalizeAxis(rot.Roll - tr.Roll));
    if (dx * dx + dy * dy + dz * dz <= kMovedCm * kMovedCm && turn <= kTurnedDeg) return;
    E::SetActorLocation(actor, to);
    E::SetActorRotation(actor, tr);
    E::SetActorRootPhysicsVelocity(actor, FVector{}, FVector{});
}

// The game's load of a printed object, from the save a joiner loaded: without the model here it would destroy the object
// ("Error loading a model"), so it is kept as a placeholder until the host's state names it.
sg::Verdict OnLoadPre(const sg::Call& c) {
    if (!IsClient() || !c.object || g_ours) return sg::Verdict::Run;
    if (PM::ModelHash(Str(c.object, L"name"))) return sg::Verdict::Run;
    MakePlaceholder(c.object);
    UE_LOGI("printer_sync: a loaded printed object's model '%ls' is not on this machine -- kept as a placeholder",
            Str(c.object, L"name").c_str());
    return sg::Verdict::Cancel;
}

// A client's E-menu on a printed object: its light is asked of the host; its bite is its own player's, told to the host.
std::unordered_map<void*, int32_t> g_bitesBefore;
sg::Verdict OnActionPre(const sg::Call& c) {
    if (!IsClient() || !c.object || g_ours) return sg::Verdict::Run;
    const int32_t off = c.locals ? R::FindParamOffset(c.function, L"action") : -1;
    if (off < 0) return sg::Verdict::Run;
    const uint8_t action = c.locals[off];
    const ElementId eid = EidOf(c.object);
    if (eid == coop::element::kInvalidId) return sg::Verdict::Run;
    if (action == kActionLight) {
        N::PrintedIntentPayload p{};
        p.eid = static_cast<uint32_t>(eid);
        p.op = N::kPrintedLight;
        if (N::Session* s = Live()) s->SendReliable(N::ReliableKind::PrintedIntent, &p, sizeof(p));
        return sg::Verdict::Cancel;
    }
    if (action == kActionEat) g_bitesBefore[c.object] = Get<int32_t>(c.object, L"bites", 0);
    return sg::Verdict::Run;
}
void OnActionPost(const sg::Call& c) {
    auto it = c.object ? g_bitesBefore.find(c.object) : g_bitesBefore.end();
    if (it == g_bitesBefore.end()) return;
    const int32_t before = it->second;
    g_bitesBefore.erase(it);
    const ElementId eid = EidOf(c.object);
    const bool bitten = !R::IsLive(c.object) || Get<int32_t>(c.object, L"bites", before) < before;
    if (!bitten || eid == coop::element::kInvalidId) return;
    N::PrintedIntentPayload p{};
    p.eid = static_cast<uint32_t>(eid);
    p.op = N::kPrintedBite;
    if (N::Session* s = Live()) s->SendReliable(N::ReliableKind::PrintedIntent, &p, sizeof(p));
}

// ---- the printer's run on every peer ------------------------------------------------------------
//
// A print's run is the printer's own tick, so a client's copy runs it from the host's start and the host's end stops it:
// two messages, no stream. A client's copy at its run's end spawns an object of its own, which is refused (the host's
// arrives as PrintedState).

struct Run {
    void* printer = nullptr;
    int32_t idx = -1;
    std::wstring name;
};
std::unordered_map<uint32_t, Run> g_runs;  // host: the printers printing, by element id
std::vector<void*> g_ownBirths;              // client: objects its copy of a printer made, destroyed at the next tick
uint64_t g_runsMirrored = 0, g_ownBirthsRefused = 0;

void SendRun(int slot, uint32_t eid, uint8_t op, const std::wstring& name) {
    N::Session* s = Live();
    if (!s) return;
    N::PrinterStatePayload p{};
    p.printerEid = eid;
    p.op = op;
    p.nameLen = PutText(p.name, PM::ToUtf8(name));
    if (slot < 0) s->SendReliable(N::ReliableKind::PrinterState, &p, sizeof(p));
    else s->SendReliableToSlot(slot, N::ReliableKind::PrinterState, &p, sizeof(p));
}

// The host's print that ran: its printer is printing now.
void OnPrintPost(const sg::Call& c) {
    if (!IsHost() || !c.object || !Get<uint8_t>(c.object, L"active", 0)) return;
    const ElementId eid = EidOf(c.object);
    if (eid == coop::element::kInvalidId) return;
    Run& r = g_runs[static_cast<uint32_t>(eid)];
    r.printer = c.object;
    r.idx = R::InternalIndexOf(c.object);
    r.name = Str(c.object, L"meshName");
    SendRun(-1, static_cast<uint32_t>(eid), N::kPrinterRunStart, r.name);
    UE_LOGI("printer_sync: printer eid=%u prints '%ls' -- its run sent to every peer", static_cast<unsigned>(eid),
            r.name.c_str());
}

void HostTickRuns() {
    for (auto it = g_runs.begin(); it != g_runs.end();) {
        const bool live = R::IsLiveByIndex(it->second.printer, it->second.idx);
        if (live && Get<uint8_t>(it->second.printer, L"active", 0)) { ++it; continue; }
        SendRun(-1, it->first, N::kPrinterRunEnd, it->second.name);
        UE_LOGI("printer_sync: printer eid=%u's run ended on the host", it->first);
        it = g_runs.erase(it);
    }
}

// A client's copy at the host's end: the run's own end, without the object its tick would spawn.
void EndRunHere(void* printer) {
    if (!Get<uint8_t>(printer, L"active", 0)) return;
    Put<uint8_t>(printer, L"active", 0);
    Put<uint8_t>(printer, L"canPrint", 0);
    if (void* const loop = Get<void*>(printer, L"loop", nullptr)) CallNoArgs(loop, L"Deactivate");
    if (void* const mesh = Get<void*>(printer, L"mesh", nullptr)) CallNoArgs(mesh, L"ClearAllMeshSections");
}

// A client's printer copy handing its print to the object it spawned: refused, and that object goes.
sg::Verdict OnFromPrinterPre(const sg::Call& c) {
    if (!IsClient() || !c.object || g_ours) return sg::Verdict::Run;
    if (!IsPrinter(c.callerObject)) return sg::Verdict::Run;
    g_ownBirths.push_back(c.object);
    ++g_ownBirthsRefused;
    return sg::Verdict::Cancel;
}

void ClientTickRuns() {
    for (void* a : g_ownBirths)
        if (R::IsLive(a)) E::DestroyActor(a);
    if (!g_ownBirths.empty())
        UE_LOGI("printer_sync: %zu object(s) this client's printer copy made were destroyed (the host's comes)",
                g_ownBirths.size());
    g_ownBirths.clear();
}

bool g_watchesRegistered = false, g_watchesSettled = false;
void SettleWatches() {
    if (g_watchesSettled) return;
    if (!g_watchesRegistered) {
        g_watchesRegistered = true;
        const bool a = sg::WatchName(kRowPrint, kTag, &OnPrintPre, &OnPrintPost);
        const bool b = sg::WatchName(kRowAction, kTag, &OnActionPre, &OnActionPost);
        const bool c = sg::WatchName(kRowLoad, kTag, &OnLoadPre, nullptr);
        const bool d = sg::WatchName(kRowFromPrinter, kTag, &OnFromPrinterPre, nullptr);
        if (!a || !b || !c || !d) {
            g_watchesSettled = true;
            UE_LOGE("printer_sync: the gate took no watch (print %d, action %d, load %d, from printer %d) -- a print "
                    "stays its machine's", a, b, c, d);
            return;
        }
    }
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowPrint, kTag) || !sg::NameWatchSettled(kRowAction, kTag) ||
        !sg::NameWatchSettled(kRowLoad, kTag) || !sg::NameWatchSettled(kRowFromPrinter, kTag))
        return;
    g_watchesSettled = true;
    UE_LOGI("printer_sync: watches settled (print live=%d, action live=%d, load live=%d, from printer live=%d)",
            sg::NameWatchLive(kRowPrint, kTag) ? 1 : 0, sg::NameWatchLive(kRowAction, kTag) ? 1 : 0,
            sg::NameWatchLive(kRowLoad, kTag) ? 1 : 0, sg::NameWatchLive(kRowFromPrinter, kTag) ? 1 : 0);
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* s = Live();
    if (!s) return;
    SettleWatches();
    if (s->role() == N::Role::Host) {
        TickChecks();
        HostTick();
        HostTickRuns();
    } else {
        ClientTickRuns();
    }
}

void OnPrinterState(const N::PrinterStatePayload& p) {
    if (!IsClient()) return;
    void* const printer = ActorOf(p.printerEid);
    if (!IsPrinter(printer)) return;
    if (p.op == N::kPrinterRunEnd) {
        EndRunHere(printer);
        UE_LOGI("printer_sync: printer eid=%u's run ended by the host", p.printerEid);
        return;
    }
    if (Get<uint8_t>(printer, L"active", 0)) return;
    // The host's model on this copy, from this player's own file, and its own run: the game's selectMesh and print.
    const std::wstring name = TextOf(p.name, p.nameLen, N::kPrintNameMax);
    bool sel = false, ran = false;
    {
        Ours ours;
        if (void* const fn = R::FindDispatchFunctionCached(R::ClassOf(printer), L"selectMesh")) {
            ue_wrap::ParamFrame f(fn);
            uint8_t buf[16] = {};
            sel = f.valid() && ue_wrap::fstring_utils::MintFString(name, buf) &&
                  f.SetRaw(L"meshName", buf, sizeof(buf)) && ue_wrap::Call(printer, f);
        }
        ran = CallNoArgs(printer, L"print");
    }
    ++g_runsMirrored;
    UE_LOGI("printer_sync: printer eid=%u runs '%ls' as the host's (selected %d, print ran %d, active %d)",
            p.printerEid, name.c_str(), sel ? 1 : 0, ran ? 1 : 0, Get<uint8_t>(printer, L"active", 0));
}

void OnAsk(int senderSlot, const N::PrintAskPayload& p) {
    if (!IsHost()) return;
    const std::wstring name = TextOf(p.name, p.nameLen, N::kPrintNameMax);
    void* const printer = coop::element::LivePropActor(static_cast<ElementId>(p.printerEid));
    if (!IsPrinter(printer) || !PM::IsPlainName(name)) {
        UE_LOGI("printer_sync: slot %d's print on eid=%u refused: no such printer or model", senderSlot, p.printerEid);
        return;
    }
    if (!InReach(senderSlot, printer)) {
        UE_LOGI("printer_sync: slot %d's print on eid=%u refused: out of reach", senderSlot, p.printerEid);
        return;
    }
    if (Get<uint8_t>(printer, L"active", 0)) return;
    for (const Check& k : g_checks)
        if (k.printer == printer) return;
    // The host's printer takes the model as the client's did, from the host's own file.
    if (void* const fn = R::FindDispatchFunctionCached(R::ClassOf(printer), L"selectMesh")) {
        ue_wrap::ParamFrame f(fn);
        uint8_t buf[16] = {};
        if (f.valid() && ue_wrap::fstring_utils::MintFString(name, buf) && f.SetRaw(L"meshName", buf, sizeof(buf)))
            ue_wrap::Call(printer, f);
    }
    StartCheck(printer, name, senderSlot);
}

void OnCheck(const N::PrintCheckPayload& p) {
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Client) return;
    N::PrintCheckReplyPayload r{};
    r.request = p.request;
    r.hash = PM::ModelHash(TextOf(p.name, p.nameLen, N::kPrintNameMax));
    s->SendReliable(N::ReliableKind::PrintCheckReply, &r, sizeof(r));
}

void OnCheckReply(int senderSlot, const N::PrintCheckReplyPayload& p) {
    if (!IsHost() || senderSlot <= 0 || senderSlot >= coop::players::kMaxPeers) return;
    for (Check& c : g_checks) {
        if (c.request != p.request || !c.asked[senderSlot]) continue;
        c.answered[senderSlot] = true;
        c.hash[senderSlot] = p.hash;
    }
}

void OnNotice(const N::PrintNoticePayload& p) {
    if (!IsClient()) return;
    Say(p.subject, p.reason, TextOf(p.name, p.nameLen, N::kPrintNameMax));
}

void OnState(const N::PrintedStatePayload& p) {
    if (!IsClient()) return;
    if (p.op == N::kPrintedGone) {
        if (void* const a = ActorOf(p.eid)) E::DestroyActor(a);
        g_reported.erase(p.eid);
        return;
    }
    ApplyState(p);
}

void OnIntent(int senderSlot, const N::PrintedIntentPayload& p) {
    if (!IsHost()) return;
    auto it = g_tracked.find(p.eid);
    if (it == g_tracked.end() || !R::IsLiveByIndex(it->second.actor, it->second.idx)) return;
    Tracked& t = it->second;
    if (p.op == N::kPrintedLacking) {
        if (!g_told.insert({senderSlot, t.name}).second) return;
        const uint8_t reason = p.arg == N::kPrintDiffers ? N::kPrintDiffers : N::kPrintLacks;
        UE_LOGI("printer_sync: slot %d %s '%ls' (eid=%u) -- a placeholder there", senderSlot,
                reason == N::kPrintDiffers ? "holds another copy of" : "lacks", t.name.c_str(), p.eid);
        Say(static_cast<uint8_t>(senderSlot), reason, t.name);
        NoticeTo(senderSlot, static_cast<uint8_t>(senderSlot), reason, t.name);
        return;
    }
    if (!InReach(senderSlot, t.actor)) return;
    if (p.op == N::kPrintedLight) {
        Put<uint8_t>(t.actor, L"lightActive", Get<uint8_t>(t.actor, L"lightActive", 0) ? 0 : 1);
        CallNoArgs(t.actor, L"updLight");
        t.light = Get<uint8_t>(t.actor, L"lightActive", 0);
        SendState(-1, p.eid, t);
    } else if (p.op == N::kPrintedBite) {
        const int32_t bites = Get<int32_t>(t.actor, L"bites", 0) - 1;
        Put<int32_t>(t.actor, L"bites", bites);
        if (bites <= 0) {
            E::DestroyActor(t.actor);  // the walk sends its end
        } else {
            t.bites = bites;
            SendState(-1, p.eid, t);
        }
    }
}

void SendJoin(int slot) {
    if (!IsHost()) return;
    int sent = 0;
    for (auto& [eid, t] : g_tracked) {
        if (!R::IsLiveByIndex(t.actor, t.idx)) continue;
        SendState(slot, eid, t);
        ++sent;
    }
    for (auto& [eid, r] : g_runs)
        if (R::IsLiveByIndex(r.printer, r.idx)) SendRun(slot, eid, N::kPrinterRunStart, r.name);
    if (sent) UE_LOGI("printer_sync: %d printed object(s) sent to joiner slot %d", sent, slot);
}

void OnDisconnect() {
    UE_LOGI("printer_sync: session end (prints run %llu refused %llu; spawned %llu adopted %llu placeholders %llu)",
            static_cast<unsigned long long>(g_printsRun), static_cast<unsigned long long>(g_printsRefused),
            static_cast<unsigned long long>(g_spawned), static_cast<unsigned long long>(g_adopted),
            static_cast<unsigned long long>(g_placeholders));
    g_checks.clear();
    g_tracked.clear();
    g_told.clear();
    g_reported.clear();
    g_bitesBefore.clear();
    g_runs.clear();
    g_ownBirths.clear();
    UE_LOGI("printer_sync: runs mirrored %llu, own births refused %llu", static_cast<unsigned long long>(g_runsMirrored),
            static_cast<unsigned long long>(g_ownBirthsRefused));
    g_runsMirrored = g_ownBirthsRefused = 0;
    g_printsRun = g_printsRefused = g_spawned = g_adopted = g_placeholders = 0;
}

}  // namespace coop::printer_sync
