// harness/autotest/glowstick_drill_common.cpp -- the glowstick drill's waits, reads of a glowstick and the host's spawns.

#include "glowstick_drill.h"   // co-located private header (src tree, not include/)

namespace harness::autotest::glowstick_drill {

std::string Arm() { return coop::config::ResolveEnum(coop::config_registry::rows::glowstick_drill); }
void Glog(bool warn, const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (warn) UE_LOGW("glowstick-%s: %s", ArmCStr(), line);
    else UE_LOGI("glowstick-%s: %s", ArmCStr(), line);
}

const char* ArmCStr() {
    static const std::string arm = Arm();
    return arm.c_str();
}
std::string TagStr() { return "glowstick-" + Arm(); }
bool ArmIs(const char* a) { return Arm() == a; }

bool Aborted() {
    std::string s;
    return PS::Peek("abort", s);
}

void GiveUp(RV::Outcome outcome, const char* fmt, ...) {
    char reason[384] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(reason, sizeof(reason), fmt, args);
    va_end(args);
    for (char* c = reason; *c; ++c)
        if (*c == '\n' || *c == '\r') *c = ' ';
    RV::Say(TagStr().c_str(), outcome, "%s", reason);
    PS::Mark("abort", reason);
}


float Dist(const FVector& a, const FVector& b) {
    return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
}

// ---- reads of a glowstick (game thread) ----

int Sticked(void* g) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!g || !R::FindBoolProperty(R::ClassOf(g), L"sticked", off, mask)) return -1;
    return (static_cast<const uint8_t*>(g)[off] & mask) ? 1 : 0;
}

FVector LocOf(void* g) {
    FVector v{};
    const int32_t off = g ? R::FindPropertyOffset(R::ClassOf(g), L"loc") : -1;
    if (off >= 0) std::memcpy(&v, static_cast<const uint8_t*>(g) + off, sizeof(v));
    return v;
}

std::wstring AttachKeyOf(void* g) {
    const int32_t off = g ? R::FindPropertyOffset(R::ClassOf(g), L"attachToKey") : -1;
    std::wstring k = off >= 0 ? ue_wrap::save_record::ReadFNameAt(g, off) : std::wstring();
    return k == L"None" ? std::wstring() : k;
}

void* ByKey(const std::wstring& key) { return key.empty() ? nullptr : coop::prop_key_index::Find(key); }

FVector Where(void* a) {
    FVector v{};
    if (a) E::TryGetActorLocation(a, v);
    return v;
}

std::string Num3(const FVector& v) {
    char b[96];
    std::snprintf(b, sizeof(b), "%.1f %.1f %.1f", v.X, v.Y, v.Z);
    return b;
}

bool ParseNum3(std::istringstream& in, FVector& v) { return static_cast<bool>(in >> v.X >> v.Y >> v.Z); }

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string Narrow(const std::wstring& s) { return std::string(s.begin(), s.end()); }

// ---- the host spawns ----

bool ClientBody(void*& body, FVector& at) {
    body = nullptr;
    if (coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1)) body = rp->valid() && rp->HasPose() ? rp->GetActor() : nullptr;
    return body && E::TryGetActorLocation(body, at);
}

// The key of a spawned prop, once it reads.
bool WaitKey(void* actor, std::wstring& key) {
    return Wait("the spawned prop's key", 60, [&] {
        OnGameThread([&] { key = ue_wrap::prop::GetInteractableKeyString(actor); });
        return !key.empty() && key != L"None";
    });
}

// A prop of `path` on the floor `ahead` cm in front of the client's puppet; its key once it reads. `floorSide`
// receives the floor point `side` cm to the puppet's right of it.
bool SpawnAhead(const wchar_t* path, float ahead, float side, void*& actor, std::wstring& key, FVector* floorSide) {
    FVector at{};
    const char* why = "";
    OnGameThread([&] {
        void* body = nullptr;
        FVector me{};
        if (!ClientBody(body, me)) {
            why = "the client's puppet has no pose";
            return;
        }
        const FVector f = E::GetActorForwardVector(body);
        const float rx = f.Y, ry = -f.X;   // the puppet's right
        const FVector above = {me.X + f.X * ahead, me.Y + f.Y * ahead, me.Z};
        const uint8_t types[2] = {0, 1};
        ue_wrap::trace::Hit floor;
        if (!ue_wrap::trace::LineHitForObjects(body, above, {above.X, above.Y, above.Z - kFloorSearchCm}, types, 2, nullptr, 0, &floor) ||
            !floor.blocked) {
            why = "no floor under the spawn point";
            return;
        }
        at = {above.X, above.Y, floor.point.Z + kAboveFloorCm};
        if (floorSide) *floorSide = {above.X + rx * side, above.Y + ry * side, floor.point.Z};
        void* cls = ue_wrap::asset_load::LoadObjectByPath(path);
        if (!cls) {
            why = "the class did not load";
            return;
        }
        actor = E::SpawnActor(cls, at);
        if (!actor) why = "SpawnActor returned null";
    });
    if (!actor) {
        GiveUp(RV::Outcome::Invalid, "the host could not spawn %ls: %s", path, why);
        return false;
    }
    return WaitKey(actor, key);
}

// A prop of `path` resting on top of `support`, at the centre of its bounds.
bool SpawnOnTop(const wchar_t* path, void* support, void*& actor, std::wstring& key) {
    const char* why = "";
    OnGameThread([&] {
        FVector o{}, e{};
        if (!E::GetActorBounds(support, true, o, e)) {
            why = "the support's bounds did not read";
            return;
        }
        void* cls = ue_wrap::asset_load::LoadObjectByPath(path);
        if (!cls) {
            why = "the class did not load";
            return;
        }
        actor = E::SpawnActor(cls, {o.X, o.Y, o.Z + e.Z + 8.f});
        if (!actor) why = "SpawnActor returned null";
    });
    if (!actor) {
        GiveUp(RV::Outcome::Invalid, "the host could not spawn %ls on the support: %s", path, why);
        return false;
    }
    return WaitKey(actor, key);
}

HostRead ReadHost(const std::wstring& key) {
    HostRead r;
    OnGameThread([&] {
        void* g = ByKey(key);
        r.sticked = Sticked(g);
        {
            int32_t off = -1;
            uint8_t mask = 0;
            if (g && R::FindBoolProperty(R::ClassOf(g), L"stickStatic", off, mask))
                r.stickStatic = (static_cast<const uint8_t*>(g)[off] & mask) ? 1 : 0;
        }
        r.loc = LocOf(g);
        r.key = AttachKeyOf(g);
        r.driven = g && coop::remote_prop::IsDrivenBy(g, 1);
        r.unsettled = coop::prop_save_data::dev::Unsettled(key);
    });
    return r;
}

// What the glowstick's constraint component holds, logged to measure what a made stick leaves in the properties the
// engine reflects (ConstraintActor1/2, ComponentName1/2 and IsBroken); the verdicts do not read it yet. Game thread.
void LogConstraint(const char* who, void* g) {
    const int32_t po = g ? R::FindPropertyOffset(R::ClassOf(g), L"PhysicsConstraint") : -1;
    void* pcc = nullptr;
    if (po >= 0) std::memcpy(&pcc, static_cast<const uint8_t*>(g) + po, sizeof(pcc));
    if (!pcc || !R::IsLive(pcc)) {
        GLOG("%s constraint component: none (%d)", who, po);
        return;
    }
    void* actors[2] = {};
    int32_t offs[2] = {R::FindPropertyOffset(R::ClassOf(pcc), L"ConstraintActor1"), R::FindPropertyOffset(R::ClassOf(pcc), L"ConstraintActor2")};
    for (int i = 0; i < 2; ++i)
        if (offs[i] >= 0) std::memcpy(&actors[i], static_cast<const uint8_t*>(pcc) + offs[i], sizeof(void*));
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(pcc), L"IsBroken"));
    int broken = -1;
    if (f.valid() && ue_wrap::Call(pcc, f)) broken = f.Get<bool>(L"ReturnValue") ? 1 : 0;
    GLOG("%s constraint component %p: ConstraintActor1 %p (%ls) @%d, ConstraintActor2 %p (%ls) @%d, IsBroken %d", who, pcc,
            actors[0], actors[0] ? R::ClassNameOf(actors[0]).c_str() : L"-", offs[0], actors[1],
            actors[1] ? R::ClassNameOf(actors[1]).c_str() : L"-", offs[1], broken);
}

GJ::dev::Counts Judged() {
    GJ::dev::Counts c;
    OnGameThread([&] { c = GJ::dev::Get(); });
    return c;
}


}  // namespace harness::autotest::glowstick_drill
