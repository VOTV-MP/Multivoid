// ui/server_browser_surface.cpp -- see ui/server_browser_surface.h.

#include "ui/server_browser_surface.h"

#include "coop/config/config.h"
#include "coop/session/session_manager.h"  // RefreshLatestVersion -- the update check's ONE trigger
#include "ui/server_browser.h"          // the ImGui overlay browser -- the fallback
#include "ui/server_browser_native.h"   // the UMG screen -- the default
#include "ue_wrap/core/log.h"

namespace ui::server_browser_surface {

bool UseNative() {
    // Latched, INCLUDING the log line: this is asked from a click poll and from four
    // recovery paths, and the answer cannot change without a restart.
    static const bool s = [] {
        const bool native =
            coop::config::ResolveFlag(::coop::config_registry::rows::browser_native);
        UE_LOGI("server_browser_surface: this session uses the %s server browser "
                "(browser_native=%d)", native ? "NATIVE" : "overlay", native ? 1 : 0);
        return native;
    }();
    return s;
}

void Open() {
    // THE UPDATE CHECK LIVES HERE, and that placement is a privacy fix, not a convenience.
    // /v1/latest used to fire at boot and again on every main-menu entrance, so the master learned
    // the player's source IP at game launch -- before any multiplayer decision existed, and while
    // the host window's LAN ONLY row promised them, in so many words, that nothing contacts a
    // Multivoid server. Opening the browser IS a request to talk to the master, the same trigger
    // /v1/lobbies already has one call down, and everything else the mod sends is downstream of an
    // action: Host, the visibility tick, clicking a server. So the rule is that unasked lanes do
    // not fire.
    //
    // Deliberately at the SURFACE owner rather than inside either browser: this is the one place
    // that knows a browser is being opened at all, and putting it in both would be two
    // implementations of one decision. Still a separate request rather than riding the /v1/lobbies
    // response, which would be strictly better -- one round trip, arriving when it is relevant --
    // but that needs a master-side change, so it is a follow-up and not a precondition.
    coop::session_manager::RefreshLatestVersion();

    if (UseNative()) ui::server_browser_native::Open();
    else             ui::server_browser::Open();
}

bool IsOpen() {
    return ui::server_browser::IsOpen() || ui::server_browser_native::IsOpen();
}

}  // namespace ui::server_browser_surface
