// coop/dev/cleanup_trace.cpp -- see coop/dev/cleanup_trace.h.

#include "coop/dev/cleanup_trace.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

namespace coop::dev::cleanup_trace {

bool On() {
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::cleanup_trace);
    return on;
}

void Step(const char* phase, const char* call) {
    UE_LOGI("cleanup-trace: %s %s", phase, call);
}

}  // namespace coop::dev::cleanup_trace
