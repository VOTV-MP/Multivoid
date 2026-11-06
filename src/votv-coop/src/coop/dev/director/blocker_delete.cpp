// coop/dev/director/blocker_delete.cpp -- see coop/dev/director/blocker_delete.h.

#include "coop/dev/director/blocker_delete.h"

#include "coop/config/config.h"
#include "coop/props/prop_lifecycle.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <string>

namespace coop::director {
namespace {
namespace R = ue_wrap::reflection;

constexpr int kMaxDeletes = 6;   // props deleted per process: a pile of boxes, not a warehouse

int g_deletes = 0;
bool g_boundSaid = false;

}  // namespace

// VOTVCOOP_DRILL_DELETE=0: a blocking prop is left to the detour, for a run that measures the walk against its props.
// Not a config row (as the walk stage's switch is not), so the process logs its effective value once.
bool BlockerDeleteOff() {
    static const bool s = [] {
        const std::string v = coop::config::ReadEnv("VOTVCOOP_DRILL_DELETE");
        const bool off = (v == "0");
        UE_LOGI("director: the blocker delete is %s (VOTVCOOP_DRILL_DELETE=%s)", off ? "OFF" : "ON",
                v.empty() ? "unset" : v.c_str());
        return off;
    }();
    return s;
}

bool DeleteBlocker(void* /*player*/, void* prop, const ue_wrap::FVector& at, const char* why) {
    if (BlockerDeleteOff() || !prop) return false;
    if (g_deletes >= kMaxDeletes) {
        if (!g_boundSaid) {
            g_boundSaid = true;
            UE_LOGW("director: the blocker delete bound (%d) is spent", kMaxDeletes);
        }
        return false;
    }
    if (!R::IsLive(prop) || !ue_wrap::prop::IsDescendantOfProp(prop)) return false;
    const std::wstring cls = R::ClassNameOf(prop);
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(prop);
    // An empty name stringifies to None, which the destroy seam also reads as keyless: it sends no PropDestroy for it.
    if (key.empty() || key == L"None") {
        UE_LOGW("director: NOT deleting %ls blocking %s -- it has no prop key", cls.c_str(), why);
        return false;
    }
    // The seam leaves a child actor out of the broadcast (the parent's construction script owns it on every peer).
    // An unreadable answer refuses too: a delete that may not reach the peers is not made.
    bool childReadable = false;
    if (ue_wrap::engine::IsChildActor(prop, childReadable) || !childReadable) {
        UE_LOGW("director: NOT deleting %ls %ls blocking %s -- it is a child actor (or not readable as one), which the "
                "destroy seam does not carry", cls.c_str(), key.c_str(), why);
        return false;
    }
    if (coop::prop_lifecycle::ClientDestroyWindowOpen()) {
        UE_LOGW("director: NOT deleting %ls %ls blocking %s -- inside the world-load window, where the destroy seam "
                "sends nothing", cls.c_str(), key.c_str(), why);
        return false;
    }
    if (!ue_wrap::engine::DestroyActor(prop)) {
        UE_LOGW("director: the destroy of %ls %ls blocking %s did not run", cls.c_str(), key.c_str(), why);
        return false;
    }
    ++g_deletes;
    UE_LOGW("director: DELETED %ls %ls blocking %s at (%.0f,%.0f,%.0f)", cls.c_str(), key.c_str(), why, at.X, at.Y,
            at.Z);
    return true;
}

}  // namespace coop::director
