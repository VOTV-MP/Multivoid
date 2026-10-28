// coop/dev/pr64_drill_lid_aim.cpp -- see coop/dev/pr64_drill.h: the lid arm's box readers (its lid bit, its arrays, its
// slot components) and the aim at what the arm presses. The steps are in pr64_drill_lid.cpp.

#include "pr64_drill_detail.h"
#include "pr64_drill_lid.h"

#include "coop/player/players_registry.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::pr64_drill::detail {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
using ue_wrap::FVector;

// The player's interaction trace is LookAtFunction's SphereTraceSingle (radius 2, TraceTypeQuery1, mainPlayer.cpp:19962):
// a component it can take must block that channel. TraceTypeQuery1 is ECC_Visibility in a default project config; the
// game's own channel table is not read here.
constexpr uint8_t kVisibilityChannel = 3;

// The sweep: z offsets 0..24 uu in 4 uu steps above the slot's place, then the same 6 uu to the box's right, then the
// same 6 uu to its left.
constexpr int kStepsPerRow = 7, kRows = 3, kSweepN = kStepsPerRow * kRows;
constexpr float kStepZ = 4.f, kSideY = 6.f;

struct Offset {
    float dz = 0.f, dy = 0.f;
};
// Index -1 is the slot's own place (the dwell before the sweep).
Offset OffsetAt(int i) {
    if (i < 0) return {};
    const int row = i / kStepsPerRow;
    return {kStepZ * static_cast<float>(i % kStepsPerRow), row == 0 ? 0.f : (row == 1 ? kSideY : -kSideY)};
}

struct AimState {
    uint64_t firstMs = 0;     // the first aim tick of change B
    bool aimed = false;       // an aim was set on an earlier tick, so the trace has had a tick to read it
    bool sweeping = false;
    int cur = -1;             // the offset aimed on the last tick
    int win = -1;             // the offset that took the slot
    std::string state;        // slot 0's state line, said again when the sweep fails
    std::wstring took;        // every component class the trace took, once each
};
AimState g_a;

// A reflected no-argument read on a component: false when its class does not carry it.
template <class T>
bool Read(void* comp, const wchar_t* fnName, T& out) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), fnName);
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !ue_wrap::Call(comp, f)) return false;
    out = f.Get<T>(L"ReturnValue");
    return true;
}

bool ReadResponse(void* comp, uint8_t channel, uint8_t& out) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), L"GetCollisionResponseToChannel");
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<uint8_t>(L"Channel", channel) || !ue_wrap::Call(comp, f)) return false;
    out = f.Get<uint8_t>(L"ReturnValue");
    return true;
}

// "enabled=<0 none 1 query 2 physics 3 both> objType=<channel> visibility=<0 ignore 1 overlap 2 block>", `?` for a read
// that did not run.
std::string CollisionOf(void* comp) {
    const int enabled = E::GetComponentCollisionEnabled(comp);
    uint8_t objType = 0, response = 0;
    const bool okType = Read(comp, L"GetCollisionObjectType", objType);
    const bool okResponse = ReadResponse(comp, kVisibilityChannel, response);
    char b[3][12];
    std::snprintf(b[0], sizeof(b[0]), enabled >= 0 ? "%d" : "?", enabled);
    std::snprintf(b[1], sizeof(b[1]), okType ? "%d" : "?", objType);
    std::snprintf(b[2], sizeof(b[2]), okResponse ? "%d" : "?", response);
    char line[96];
    std::snprintf(line, sizeof(line), "enabled=%s objType=%s visibility=%s", b[0], b[1], b[2]);
    return line;
}

// The point to aim at: the slot's place, `o.dz` above it and `o.dy` along the box's right vector.
FVector PointFor(void* box, const FVector& base, const Offset& o) {
    const FVector fwd = E::GetActorForwardVector(box);
    const float flat = std::hypot(fwd.X, fwd.Y);
    const float rx = flat > 1e-4f ? -fwd.Y / flat : 0.f, ry = flat > 1e-4f ? fwd.X / flat : 1.f;
    return {base.X + rx * o.dy, base.Y + ry * o.dy, base.Z + o.dz};
}

}  // namespace

// The box's `opened` bit, -1 when it does not read.
int ReadOpened(void* box) {
    const int32_t off = box ? R::FindPropertyOffset(R::ClassOf(box), L"opened") : -1;
    return off < 0 ? -1 : (*(static_cast<const uint8_t*>(box) + off) != 0 ? 1 : 0);
}

// A TArray property of `obj`: its data and element count; false when it does not read.
bool ArrayOf(void* obj, const wchar_t* name, const uint8_t** data, int32_t* num) {
    const int32_t off = obj && R::IsLive(obj) ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    const uint8_t* base = static_cast<const uint8_t*>(obj) + off;
    *data = *reinterpret_cast<uint8_t* const*>(base);
    *num = *reinterpret_cast<const int32_t*>(base + 8);
    return *num >= 0 && (*num == 0 || *data);
}

// boxesTr[i], the slot's component; null when the array does not hold it.
void* SlotComp(void* box, int i) {
    const uint8_t* d = nullptr;
    int32_t n = 0;
    if (!ArrayOf(box, L"boxesTr", &d, &n) || i >= n) return nullptr;
    void* c = reinterpret_cast<void* const*>(d)[i];
    return c && R::IsLive(c) ? c : nullptr;
}

// drives_in[0]: 1 a drive, 0 empty, -1 does not read.
int ReadDrive0(void* box) {
    const uint8_t* d = nullptr;
    int32_t n = 0;
    return ArrayOf(box, L"drives_in", &d, &n) && n >= 1 ? (d[0] != 0 ? 1 : 0) : -1;
}

int32_t Count(void* box, const wchar_t* name) {
    const uint8_t* d = nullptr;
    int32_t n = 0;
    return ArrayOf(box, name, &d, &n) ? n : -1;
}

// Aims at `target` and says whether the player's own interaction trace has `want` (the component when it is not null,
// else the actor `box`); false while it has not, and after the arm failed (the trace not taking it within kAimBoundMs
// of the step is a failure that names what it took).
bool Aimed(const ue_wrap::FVector& target, void* box, void* wantComp, const char* what) {
    void* me = coop::players::Registry::Get().Local();
    if (!Aim(me, target)) {
        Fail("the player has no controller to aim with");
        return false;
    }
    void* hit = wantComp ? E::ReadMainPlayerHitComponent(me) : E::ReadMainPlayerHitActor(me);
    if (hit && hit == (wantComp ? wantComp : box)) return true;
    if (Late(kAimBoundMs)) Fail("the interaction trace never took the %s (it took %ls)", what,
                                hit ? R::ClassNameOf(hit).c_str() : L"nothing");
    return false;
}

void LogSlotState(void* box) {
    void* const slot = SlotComp(box, 0);
    void* const root = E::RootComponentOf(box);
    const uint8_t* zd = nullptr;
    int32_t zn = 0;
    const bool zipOk = ArrayOf(box, L"zipData", &zd, &zn);
    FVector at{};
    const bool haveAt = slot && E::TryGetComponentLocation(slot, at);
    char head[80];
    std::snprintf(head, sizeof(head), "at (%.0f,%.0f,%.0f), zipData %s", at.X, at.Y, at.Z,
                  zipOk ? (zn <= 1 ? "empty" : "set") : "unread");
    g_a.state = std::string("slot 0 ") + head + (slot ? ", " + CollisionOf(slot) : ", no component");
    UE_LOGI("[PR64-DRILL] host slot0 class=%ls %s%s", slot ? R::ClassNameOf(slot).c_str() : L"none", g_a.state.c_str(),
            haveAt ? "" : " (place unread)");
    UE_LOGI("[PR64-DRILL] host box root class=%ls %s", root ? R::ClassNameOf(root).c_str() : L"none",
            root ? CollisionOf(root).c_str() : "no component");
}

bool SlotAimed(void* box, void* slot) {
    FVector base{};
    void* const me = coop::players::Registry::Get().Local();
    if (!slot || !E::TryGetComponentLocation(slot, base)) {
        Fail("slot 0's component cannot be read");
        return false;
    }
    if (!g_a.firstMs) g_a.firstMs = NowMs();
    if (g_a.aimed) {
        void* const hit = E::ReadMainPlayerHitComponent(me);
        const std::wstring cls = hit ? R::ClassNameOf(hit) : L"nothing";
        if (hit && hit == slot) {
            g_a.win = g_a.cur;
            const Offset o = OffsetAt(g_a.cur);
            UE_LOGI("[PR64-DRILL] host aim took slot 0 at <%.0f,%.0f> after %llu ms", static_cast<double>(o.dz),
                    static_cast<double>(o.dy), static_cast<unsigned long long>(NowMs() - g_a.firstMs));
            return true;
        }
        if (g_a.sweeping && g_a.cur >= 0) {
            const Offset o = OffsetAt(g_a.cur);
            UE_LOGI("[PR64-DRILL] host aim try <%.0f,%.0f> took %ls", static_cast<double>(o.dz), static_cast<double>(o.dy),
                    cls.c_str());
            if ((L"," + g_a.took + L",").find(L"," + cls + L",") == std::wstring::npos)
                g_a.took += (g_a.took.empty() ? L"" : L",") + cls;
            if (g_a.cur == kSweepN - 1) {
                Fail("no aim offset takes slot 0 (%s); the sweep took %ls", g_a.state.c_str(), g_a.took.c_str());
                return false;
            }
        } else if (!g_a.sweeping && Expired(g_a.firstMs, kSlotDwellMs)) {
            g_a.sweeping = true;
            UE_LOGI("[PR64-DRILL] host the trace has not taken slot 0 in %llu ms (it took %ls): sweeping the aim",
                    static_cast<unsigned long long>(NowMs() - g_a.firstMs), cls.c_str());
        }
    }
    if (g_a.sweeping) ++g_a.cur;
    if (!Aim(me, PointFor(box, base, OffsetAt(g_a.cur)))) {
        Fail("the player has no controller to aim with");
        return false;
    }
    g_a.aimed = true;
    return false;
}

void HoldSlotAim(void* box, void* slot) {
    FVector base{};
    if (slot && E::TryGetComponentLocation(slot, base))
        Aim(coop::players::Registry::Get().Local(), PointFor(box, base, OffsetAt(g_a.win)));
}

void AimReset() { g_a = AimState{}; }

}  // namespace coop::dev::pr64_drill::detail
