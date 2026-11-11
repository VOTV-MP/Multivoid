// coop/dev/container_intent_drill_depart.cpp -- the container intent drill's client-side mark for the throwdepart arm: it
// tells a relaunched client its drill has run. See coop/dev/container_intent_drill.h.

#include "container_intent_drill_detail.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace coop::dev::container_intent_drill::detail {
namespace {

// The mark's path in the mod's data folder; empty when the mod has none.
std::filesystem::path MarkPath() {
    const std::wstring dir = ue_wrap::paths::ModDataDir();
    return dir.empty() ? std::filesystem::path() : std::filesystem::path(dir) / L"container_intent_depart.once";
}

// The mark holds the wall clock at the park, in ms: the client sees no host session identity, so a mark from an earlier
// run is told by its age.
constexpr uint64_t kDepartMarkMaxAgeMs = 300000;
uint64_t WallMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace

void DepartMarkParked() {
    const std::filesystem::path p = MarkPath();
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!p.empty() && out) out << WallMs();
    out.flush();
    if (p.empty() || !out)
        UE_LOGW("[CI-DRILL] client depart: the mark could not be written, so the relaunch will run the arm again");
}

bool DepartRejoined() {
    const std::filesystem::path p = MarkPath();
    std::error_code ec;
    if (p.empty() || !std::filesystem::exists(p, ec)) return false;
    uint64_t at = 0;
    {
        std::ifstream in(p, std::ios::binary);
        in >> at;
    }
    std::filesystem::remove(p, ec);
    if (ec) UE_LOGW("[CI-DRILL] client depart: the mark could not be removed (%s)", ec.message().c_str());
    const uint64_t now = WallMs();
    const bool ours = at != 0 && now >= at && now - at < kDepartMarkMaxAgeMs;
    if (!ours) UE_LOGI("[CI-DRILL] client depart: a stale mark (not from this run) was removed; the arm runs");
    return ours;
}

}  // namespace coop::dev::container_intent_drill::detail
