// ui/server_browser_surface.cpp -- see ui/server_browser_surface.h.

#include "ui/server_browser_surface.h"

#include "coop/config/config.h"
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
    if (UseNative()) ui::server_browser_native::Open();
    else             ui::server_browser::Open();
}

bool IsOpen() {
    return ui::server_browser::IsOpen() || ui::server_browser_native::IsOpen();
}

}  // namespace ui::server_browser_surface
