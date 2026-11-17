// coop/creatures/kerfur_camera.cpp -- see coop/creatures/kerfur_camera.h.

#include "coop/creatures/kerfur_camera.h"

#include "coop/net/session.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cstdint>
#include <unordered_set>

namespace coop::kerfur_camera {
namespace {

namespace R = ue_wrap::reflection;

constexpr uint64_t kCheckMs = 2000;
constexpr int32_t kMaxCams = 256;   // a list longer than this is unreadable garbage
uint64_t g_nextAt = 0;
std::unordered_set<void*> g_said;   // cameras whose put-back was said once

struct Arr {
    void** data;
    int32_t num;
    int32_t max;
};

void* CameraOf(void* kerfur) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(kerfur), L"camera");
    void* const c = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(kerfur) + off) : nullptr;
    return c && R::IsLive(c) ? c : nullptr;
}

bool Listed(const Arr* cams, void* cam) {
    for (int32_t i = 0; i < cams->num && i < kMaxCams; ++i)
        if (cams->data[i] == cam) return true;
    return false;
}

}  // namespace

void Tick(coop::net::Session& session) {
    if (!session.running() || session.role() != coop::net::Role::Client) return;
    if (::GetTickCount64() < g_nextAt) return;
    g_nextAt = ::GetTickCount64() + kCheckMs;
    void* const gm = ue_wrap::world_singleton::Gamemode();
    const int32_t camsOff = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"cams") : -1;
    if (camsOff < 0) return;
    const auto* cams = reinterpret_cast<const Arr*>(static_cast<const uint8_t*>(gm) + camsOff);
    if (cams->num < 0 || cams->num > kMaxCams) return;
    for (void* k : ue_wrap::kerfur::AllKerfurs()) {
        void* const cam = CameraOf(k);
        if (!cam || Listed(cams, cam)) continue;
        // Its saved index cleared, the camera's own saveInit adds it to the list and names it as it would a new one.
        const int32_t idxOff = R::FindPropertyOffset(R::ClassOf(cam), L"index_0");
        void* const fn = R::FindDispatchFunctionCached(R::ClassOf(cam), L"saveInit");
        if (idxOff < 0 || !fn) continue;
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(cam) + idxOff) = -1;
        ue_wrap::ParamFrame f(fn);
        const bool ran = f.valid() && ue_wrap::Call(cam, f);
        if (g_said.insert(cam).second)
            UE_LOGI("kerfur_camera: an Omega's camera was off the laptop's list -- put back (ran=%d, listed=%d)", ran ? 1 : 0,
                    Listed(cams, cam) ? 1 : 0);
    }
}

}  // namespace coop::kerfur_camera
