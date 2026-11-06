// ue_wrap/actors/build_piece.cpp -- see ue_wrap/actors/build_piece.h.

#include "ue_wrap/actors/build_piece.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace ue_wrap::build_piece {
namespace {

namespace R = reflection;
namespace SR = save_record;

struct ClassRow {
    const wchar_t* name;
    Kind kind;
};
constexpr ClassRow kClasses[] = {
    {L"nail_C", Kind::Nail},
    {L"nail_long_C", Kind::Nail},
    {L"nail_ball_C", Kind::Nail},
    {L"nail_ax_C", Kind::Nail},
    {L"nail_pin_C", Kind::Nail},
    {L"nail_super_C", Kind::Nail},
    {L"customWall_unfinished_C", Kind::Unfinished},
    {L"customWall_C", Kind::Wall},
    {L"customWall_rad_C", Kind::Wall},
    {L"customWall_wetConcrete_C", Kind::WetWall},
};

template <class T>
bool ReadField(void* obj, const wchar_t* name, T& out) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(obj) + off, sizeof(out));
    return true;
}

template <class T>
bool WriteField(void* obj, const wchar_t* name, const T& v) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(obj) + off, &v, sizeof(v));
    return true;
}

bool WriteFlag(void* obj, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return false;
    uint8_t& b = static_cast<uint8_t*>(obj)[off];
    b = v ? (b | mask) : (b & ~mask);
    return true;
}

void* LiveObj(void* obj, const wchar_t* name) {
    void* v = nullptr;
    return ReadField(obj, name, v) && v && R::IsLive(v) ? v : nullptr;
}

bool CallNoArgs(void* obj, const wchar_t* name) {
    void* fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), name) : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(obj, f);
}

bool Default(void* obj) { return R::NameStartsWith(R::NameOf(obj), L"Default__"); }

// The tie's profile (PhysicsConstraint.ConstraintInstance.ProfileInstance) and its struct. Null when it cannot be read.
uint8_t* TieProfile(void* nail, void*& profile) {
    static void* ci = R::FindObject(L"ConstraintInstance", L"ScriptStruct");
    static void* prof = R::FindObject(L"ConstraintProfileProperties", L"ScriptStruct");
    profile = prof;
    void* comp = LiveObj(nail, L"PhysicsConstraint");
    const int32_t ciOff = comp ? R::FindPropertyOffset(R::ClassOf(comp), L"ConstraintInstance") : -1;
    const int32_t piOff = ci ? R::FindPropertyOffset(ci, L"ProfileInstance") : -1;
    return ciOff < 0 || piOff < 0 || !prof ? nullptr : static_cast<uint8_t*>(comp) + ciOff + piOff;
}

bool TieFlag(void* nail, const wchar_t* flag, bool& out) {
    void* profile = nullptr;
    const uint8_t* p = TieProfile(nail, profile);
    int32_t off = -1;
    uint8_t mask = 0;
    if (!p || !R::FindBoolProperty(profile, flag, off, mask) || off < 0) return false;
    out = (p[off] & mask) != 0;
    return true;
}

float TieThreshold(void* nail, const wchar_t* name) {
    void* profile = nullptr;
    const uint8_t* p = TieProfile(nail, profile);
    const int32_t off = p ? R::FindPropertyOffset(profile, name) : -1;
    float v = 0.f;
    if (off >= 0) std::memcpy(&v, p + off, sizeof(v));
    return v;
}

// Its threshold kept, so the game's own comes back when the tie is let go.
bool SetTieBreakable(void* nail, const wchar_t* fnName, const wchar_t* flag, const wchar_t* threshold, bool on) {
    void* comp = LiveObj(nail, L"PhysicsConstraint");
    void* fn = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), fnName) : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set(flag, on);
    f.Set(threshold, TieThreshold(nail, threshold));
    return Call(comp, f);
}

// Each nail class's own breakability, as the first of it seen breakable had it.
struct TieRow {
    bool linear = true, angular = true;
};
std::unordered_map<std::wstring, TieRow> g_ties;

void* ClassFor(const std::wstring& className) {
    if (KindOfClass(className) == Kind::None) return nullptr;
    if (void* cls = object_index::ClassByName(className.c_str())) return cls;
    const std::wstring leaf = className.substr(0, className.size() - 2);   // without _C
    return asset_load::LoadObjectByPath((L"/Game/objects/" + leaf + L"." + className).c_str());
}

}  // namespace

Kind KindOfClass(const std::wstring& className) {
    for (const ClassRow& c : kClasses)
        if (className == c.name) return c.kind;
    return Kind::None;
}

Kind KindOf(void* actor) {
    if (!actor || !R::IsLive(actor)) return Kind::None;
    return KindOfClass(R::ToString(R::NameOf(R::ClassOf(actor))));
}

void List(std::vector<void*>& out) {
    out.clear();
    for (const ClassRow& c : kClasses) {
        void* cls = object_index::ClassByName(c.name);
        if (!cls) continue;
        object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !Default(obj)) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    }
}

std::wstring KeyOf(void* piece) {
    R::FName key{};
    return ReadField(piece, L"Key", key) ? R::ToString(key) : std::wstring();
}

bool Capture(void* piece, SR::SaveRecord& out) {
    void* fn = piece ? R::FindDispatchFunctionCached(R::ClassOf(piece), L"getData") : nullptr;
    return fn && SR::CaptureRecordVia(piece, fn, out);
}

void* Spawn(const SR::SaveRecord& r) {
    void* cls = ClassFor(r.className);
    if (!cls) return nullptr;
    const FVector loc{r.xform[4], r.xform[5], r.xform[6]};
    const FRotator rot = engine::QuatToRotator(r.xform[0], r.xform[1], r.xform[2], r.xform[3]);
    void* actor = engine::BeginDeferredSpawn(cls, loc, rot);
    if (!actor) return nullptr;
    if (!engine::FinishDeferredSpawn(actor, loc, rot)) {
        engine::DestroyActor(actor);
        return nullptr;
    }
    engine::SetActorScale3D(actor, {r.xform[7], r.xform[8], r.xform[9]});
    return actor;
}

bool Apply(void* piece, const SR::SaveRecord& r, void* endA, void* endB, SR::ClassResolver resolve) {
    void* load = piece ? R::FindDispatchFunctionCached(R::ClassOf(piece), L"loadData") : nullptr;
    if (!load || !SR::ApplyRecordVia(piece, load, r, resolve)) return false;
    if (KindOf(piece) != Kind::Nail) return true;
    // A broken record's aea only attaches the nail, so a tie this copy still has is broken here.
    if (NailBroken(piece))
        if (void* comp = LiveObj(piece, L"PhysicsConstraint")) CallNoArgs(comp, L"BreakConstraint");
    WriteField(piece, L"actor_a", endA);
    WriteField(piece, L"actor_b", endB);
    return !endA || CallNoArgs(piece, L"aea");   // a nail with no first end stays as the record left it
}

bool NailKeys(const SR::SaveRecord& r, std::wstring& a, std::wstring& b) {
    if (r.names.empty() || r.names[0].size() < 2) return false;
    a = r.names[0][0];
    b = r.names[0][1];
    return true;
}

void* NailEndA(void* nail) { return LiveObj(nail, L"actor_a"); }
void* NailEndB(void* nail) { return LiveObj(nail, L"actor_b"); }

void Park(void* piece, bool tieHeld) {
    switch (KindOf(piece)) {
    case Kind::Nail:
        WriteField(piece, L"nailDrop", static_cast<void*>(nullptr));
        if (tieHeld && NailBreakable(piece)) HoldNail(piece);
        break;
    case Kind::Unfinished:
        if (void* overlap = LiveObj(piece, L"Overlap")) engine::SetComponentCollisionEnabled(overlap, 0);
        break;
    case Kind::WetWall:
        if (engine::IsActorTickEnabled(piece)) engine::SetActorTickEnabled(piece, false);
        break;
    default:
        break;
    }
}

bool ScaleWallHealth(void* wall, float k) {
    const Kind kind = KindOf(wall);
    if (kind != Kind::Wall && kind != Kind::WetWall) return false;
    float health = 0.f, maxHealth = 0.f;
    if (!ReadField(wall, L"health", health) || !ReadField(wall, L"maxHealth", maxHealth)) return false;
    return WriteField(wall, L"health", health * k) && WriteField(wall, L"maxHealth", maxHealth * k);
}

bool WallHealth(void* wall, float& out) {
    const Kind kind = KindOf(wall);
    return (kind == Kind::Wall || kind == Kind::WetWall) && ReadField(wall, L"health", out);
}

bool HitWall(void* wall, float damage) {
    FVector at{};
    float h = 0.f;
    if (!WallHealth(wall, h) || !engine::TryGetActorLocation(wall, at)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(wall), L"Damage");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set(L"damage", damage);
    f.Set(L"location", at);
    return Call(wall, f);
}

bool RepairWall(void* wall, float amount) {
    float h = 0.f, m = 0.f;
    if (!WallHealth(wall, h) || !ReadField(wall, L"maxHealth", m)) return false;
    WriteField(wall, L"health", h + amount < m ? h + amount : m);
    return CallNoArgs(wall, L"setHealthCracks");
}

bool NailBreakable(void* nail) {
    if (KindOf(nail) != Kind::Nail) return false;
    bool linear = true, angular = true;
    const bool read = TieFlag(nail, L"bLinearBreakable", linear) && TieFlag(nail, L"bAngularBreakable", angular);
    return !read || linear || angular;
}

bool HoldNail(void* nail) {
    if (KindOf(nail) != Kind::Nail) return false;
    TieRow row;
    if (TieFlag(nail, L"bLinearBreakable", row.linear) && TieFlag(nail, L"bAngularBreakable", row.angular) &&
        (row.linear || row.angular))
        g_ties.emplace(R::ToString(R::NameOf(R::ClassOf(nail))), row);
    return SetTieBreakable(nail, L"SetLinearBreakable", L"bLinearBreakable", L"LinearBreakThreshold", false) &&
           SetTieBreakable(nail, L"SetAngularBreakable", L"bAngularBreakable", L"AngularBreakThreshold", false);
}

bool FreeNail(void* nail) {
    if (KindOf(nail) != Kind::Nail) return false;
    const auto it = g_ties.find(R::ToString(R::NameOf(R::ClassOf(nail))));
    const TieRow row = it == g_ties.end() ? TieRow{} : it->second;
    return SetTieBreakable(nail, L"SetLinearBreakable", L"bLinearBreakable", L"LinearBreakThreshold", row.linear) &&
           SetTieBreakable(nail, L"SetAngularBreakable", L"bAngularBreakable", L"AngularBreakThreshold", row.angular);
}

bool SnapNail(void* nail) {
    void* comp = KindOf(nail) == Kind::Nail ? LiveObj(nail, L"PhysicsConstraint") : nullptr;
    if (!comp || NailBroken(nail)) return false;
    CallNoArgs(comp, L"BreakConstraint");
    return CallNoArgs(nail,
                      L"BndEvt__PhysicsConstraint_K2Node_ComponentBoundEvent_0_ConstraintBrokenSignature__DelegateSignature");
}

bool SetWallHealth(void* wall, float health) {
    float m = 0.f;
    const Kind kind = KindOf(wall);
    if ((kind != Kind::Wall && kind != Kind::WetWall) || !ReadField(wall, L"maxHealth", m)) return false;
    WriteField(wall, L"health", health < m ? health : m);
    return CallNoArgs(wall, L"setHealthCracks");
}

bool NailBroken(void* nail) {
    int32_t off = -1;
    uint8_t mask = 0;
    return KindOf(nail) == Kind::Nail && R::FindBoolProperty(R::ClassOf(nail), L"broken", off, mask) && off >= 0 &&
           (static_cast<const uint8_t*>(nail)[off] & mask) != 0;
}

void DestroyQuiet(void* piece) {
    if (!piece || !R::IsLive(piece)) return;
    WriteField(piece, L"nailDrop", static_cast<void*>(nullptr));
    WriteFlag(piece, L"wasBroken", false);
    engine::DestroyActor(piece);
}

}  // namespace ue_wrap::build_piece
