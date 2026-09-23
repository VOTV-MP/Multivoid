// coop/dev/rehost_rejoin.cpp -- see coop/dev/rehost_rejoin.h.

#include "coop/dev/rehost_rejoin.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/session/join_progress.h"
#include "coop/session/session_manager.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"
#include "ue_wrap/engine/world_identity.h"

#include <windows.h>

#include <chrono>
#include <string>

namespace coop::dev::rehost_rejoin {
namespace {

namespace WI = ue_wrap::world_identity;

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::rehost_rejoin);
    return s;
}

std::chrono::steady_clock::time_point g_nextProbe{};
int g_dials = 0;

}  // namespace

void Tick(const coop::net::Session& session) {
    if (!IsEnabled() || session.running()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextProbe) return;
    g_nextProbe = now + std::chrono::seconds(1);
    // At the menu, with nothing in flight: mid-travel the world kind is Unknown, and a dial over a
    // join already running would be a second click, not the case the drill stands for.
    if (WI::CurrentWorldKind() != WI::WorldKind::Other || coop::join_progress::Active()) return;
    static const std::wstring path = ue_wrap::paths::ExeDir() + L"\\multivoid-rejoin.trigger";
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    ::DeleteFileW(path.c_str());
    const std::string addr =
        coop::config::ResolveString(::coop::config_registry::rows::net_peer) + ":" +
        std::to_string(coop::config::ResolveInt(::coop::config_registry::rows::net_port));
    const bool accepted = coop::session_manager::ConnectDirect(addr);
    UE_LOGI("rehost_rejoin: [C] the rig's trigger -- dialing %s again (dial #%d, accepted=%d)",
            addr.c_str(), ++g_dials, accepted ? 1 : 0);
}

}  // namespace coop::dev::rehost_rejoin
