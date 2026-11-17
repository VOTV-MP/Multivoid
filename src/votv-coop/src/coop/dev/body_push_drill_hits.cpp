// coop/dev/body_push_drill_hits.cpp -- the body-push drill's measure: does the engine say who touched the pushed prop?
//
// A poll of a body's push (coop/props/body_push) is a crutch if the engine announces the push. The probe that kept the poll
// counted hits on a lamp or a toolbox that never moved (row 10's land lens, C1), so this counts them on the PUSHED woodchipper
// itself, on both peers, per push, at the seams below (each commented at its row), and first checks the instrument with a
// known hit: before the pushes the host drops a box on the sleeping woodchipper (the client's copy takes its own box's
// landing), with the prop's body flag bNotifyRigidBodyCollision read and set on. A seam that counts nothing for the control
// cannot be trusted to count nothing for a push. OnComponentHit, a multicast delegate, enters ProcessEvent only through a
// UFunction bound to it and the woodchipper's graph binds none, so no seam of ours is on it; OnComponentWake carries no other
// actor and no Blueprint of the class binds it: neither is counted.

#include "body_push_drill_detail.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::dev::body_push_drill::detail::hits {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace P  = ue_wrap::profile;
namespace sg = ue_wrap::script_gate;

// Seam 1: AActor::ReceiveHit, by a ProcessEvent pre-observer. docs/coop-dispatch-visibility.md says an actor's event that its
// class does not implement never reaches the detour (the event's thunk returns before it), so this is expected to count zero
// for the woodchipper and the pawns, and the count says so.
UE_FACT_FN(kRowReceiveHit, "body_push_drill", nullptr, L"ReceiveHit", RuntimeClass);
// Seam 2: prop_C::hittedd123, the prop's own handler of OnActorHit, a script function the game's Blueprint BINDS while the
// prop sleeps (a sleeping prop that is hit wakes itself with the hit's impulse) and unbinds when it fires. The script gate sees
// every call of a script body, so a hit on a sleeping copy is seen with `OtherActor`, the toucher; once per sleep.
UE_FACT_FN(kRowHandler, "body_push_drill", L"prop_C", L"hittedd123", Declared);

constexpr int kTagHandler = 0x42504831;   // 'BPH1'
constexpr int kMaxLegs = kLegs + 1;       // bucket 0 is the control
constexpr int kMaxOtherClasses = 4;

struct Count {
    int body[2] = {};   // by the client's body (its puppet on the host), by the host's own pawn
    int other = 0;
    char otherClass[kMaxOtherClasses][48] = {};
    int  otherClassN[kMaxOtherClasses] = {};
};
struct Bucket {
    Count propHit;       // seam 1, ReceiveHit on the tracked copy
    int pawnSide[2] = {};   // seam 1, ReceiveHit on a pawn naming the copy
    Count handler;       // seam 2, the sleeping prop's own handler
};

ue_wrap::CachedObjRef g_prop;
ue_wrap::CachedObjRef g_pawn[2];
Bucket g_b[kMaxLegs];
int  g_bucket = 0;
bool g_installed = false;
bool g_liveReceive = false;
uint64_t g_calls = 0;   // every ReceiveHit the observer saw, whatever its actors

void* ObjParam(const uint8_t* base, int32_t off) {
    void* p = nullptr;
    if (off >= 0 && base) std::memcpy(&p, base + off, sizeof(p));
    return p;
}

int PawnIndex(void* a) {
    if (!a) return -1;
    for (int i = 0; i < 2; ++i)
        if (g_pawn[i].Raw() == a) return i;
    return -1;
}

void NoteOther(Count& c, void* other) {
    char name[48] = "(none)";
    if (other && R::IsLive(other)) {
        const std::wstring cls = R::ClassNameOf(other);
        std::snprintf(name, sizeof(name), "%ls", cls.c_str());
    }
    for (int i = 0; i < kMaxOtherClasses; ++i) {
        if (c.otherClass[i][0] && std::strcmp(c.otherClass[i], name) == 0) { ++c.otherClassN[i]; return; }
        if (!c.otherClass[i][0]) {
            std::snprintf(c.otherClass[i], sizeof(c.otherClass[i]), "%s", name);
            c.otherClassN[i] = 1;
            return;
        }
    }
}

void Attribute(Count& c, void* other) {
    const int pawn = PawnIndex(other);
    if (pawn >= 0) ++c.body[pawn];
    else { ++c.other; NoteOther(c, other); }
}

// AActor::ReceiveHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal, NormalImpulse, Hit), before it runs.
void ReceiveHitPre(void* self, void* function, void* params) {
    ++g_calls;
    static R::ParamOffset sOther(L"Other");
    void* tracked = g_prop.Raw();
    if (!tracked || !self) return;
    void* other = ObjParam(static_cast<const uint8_t*>(params), sOther.Of(function));
    Bucket& b = g_b[g_bucket];
    if (self == tracked) {
        Attribute(b.propHit, other);
    } else if (other == tracked) {
        const int pawn = PawnIndex(self);
        if (pawn >= 0) ++b.pawnSide[pawn];
    }
}

// prop_C::hittedd123(SelfActor, OtherActor, NormalImpulse, Hit): the sleeping prop's own handler of OnActorHit, before it runs.
sg::Verdict HandlerPre(const sg::Call& c) {
    if (!c.object || c.object != g_prop.Raw() || !c.function || !c.locals) return sg::Verdict::Run;
    static R::ParamOffset sOther(L"OtherActor");
    Attribute(g_b[g_bucket].handler, ObjParam(c.locals, sOther.Of(c.function)));
    return sg::Verdict::Run;
}

void SayCount(char who, const char* what, int leg, const Count& c) {
    UE_LOGI("[BODY-PUSH-DRILL] [%c] HITS %s: %s %d (by the client's body %d, by the host's body %d, by anything else %d)", who,
            leg == 0 ? "control" : "push", what, c.body[0] + c.body[1] + c.other, c.body[0], c.body[1], c.other);
    for (int i = 0; i < kMaxOtherClasses; ++i)
        if (c.otherClass[i][0])
            UE_LOGI("[BODY-PUSH-DRILL] [%c] HITS %s %d: %s other cause %s x%d", who, leg == 0 ? "control" : "push", leg, what,
                    c.otherClass[i], c.otherClassN[i]);
}

}  // namespace

void Install() {
    if (g_installed) return;
    g_installed = true;
    void* actorCls = ue_wrap::object_index::ClassByName(P::name::ActorClassName);
    const ue_wrap::facts::Resolved fn = actorCls ? ue_wrap::facts::ResolveOn(kRowReceiveHit, actorCls) : ue_wrap::facts::Resolved();
    g_liveReceive = fn.Fn() && GT::RegisterPreObserver(fn, &ReceiveHitPre);
    UE_LOGI("[BODY-PUSH-DRILL] seam 1, the ProcessEvent observer on AActor::ReceiveHit: %s",
            g_liveReceive ? "registered" : "REFUSED or unresolved");
    const bool handlerOk = sg::WatchName(kRowHandler, kTagHandler, &HandlerPre, nullptr);
    UE_LOGI("[BODY-PUSH-DRILL] seam 2, the script-gate watch on prop_C::hittedd123: %s", handlerOk ? "registered" : "REFUSED");
}

bool Live() { return g_liveReceive; }

void Track(void* prop, void* clientPawn, void* hostPawn) {
    g_prop.Set(prop);
    g_pawn[0].Set(clientPawn);
    g_pawn[1].Set(hostPawn);
}

void BeginLeg(int leg) { g_bucket = leg < 0 || leg >= kMaxLegs ? 0 : leg; }

// The prop's body flag bNotifyRigidBodyCollision (FBodyInstance, on the static mesh), read by reflection, and set on: a body
// that does not announce its collisions announces none.
void NotifyFlag(char who, void* prop, bool setOn) {
    void* mesh = ue_wrap::prop::GetStaticMesh(prop);
    auto read = [&](bool& out) {
        if (!mesh) return false;
        void* cls = R::ClassOf(mesh);
        void* body = R::PropertyInnerStruct(cls, L"BodyInstance");
        const int32_t bodyOff = R::FindPropertyOffset(cls, L"BodyInstance");
        int32_t off = -1;
        uint8_t mask = 0;
        if (!body || bodyOff < 0 || !R::FindBoolProperty(body, L"bNotifyRigidBodyCollision", off, mask) || off < 0 || !mask)
            return false;
        out = (static_cast<const uint8_t*>(mesh)[bodyOff + off] & mask) != 0;
        return true;
    };
    bool before = false, after = false;
    const bool readBefore = read(before);
    if (setOn) ue_wrap::engine::SetActorRootNotifyRigidBodyCollision(prop, true);
    const bool readAfter = read(after);
    UE_LOGI("[BODY-PUSH-DRILL] [%c] the woodchipper's bNotifyRigidBodyCollision: %s before%s; %s after", who,
            readBefore ? (before ? "ON" : "OFF") : "unreadable", setOn ? " the drill set it on" : "",
            readAfter ? (after ? "ON" : "OFF") : "unreadable");
}

void SayLeg(char who, int leg) {
    if (leg < 0 || leg >= kMaxLegs) return;
    const Bucket& b = g_b[leg];
    SayCount(who, "seam 2, the sleeping prop's own hit handler", leg, b.handler);
    SayCount(who, "seam 1, ReceiveHit on the woodchipper", leg, b.propHit);
    UE_LOGI("[BODY-PUSH-DRILL] [%c] HITS %s %d: seam 1, ReceiveHit on a pawn naming the woodchipper: the client's body %d, the "
            "host's body %d; %llu ReceiveHit calls seen in all (observer %s)", who, leg == 0 ? "control" : "push", leg,
            b.pawnSide[0], b.pawnSide[1], static_cast<unsigned long long>(g_calls), g_liveReceive ? "live" : "NOT LIVE");
}

void Reset() {
    g_prop.Reset();
    g_pawn[0].Reset();
    g_pawn[1].Reset();
    for (Bucket& b : g_b) b = Bucket{};
    g_bucket = 0;
    g_calls = 0;
}

}  // namespace coop::dev::body_push_drill::detail::hits
