// coop/props/disc_record_ceiling.cpp -- see coop/props/disc_record_ceiling.h.

#include "coop/props/disc_record_ceiling.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

namespace coop::disc_record {

namespace {
// The dev red arm of the full-disc drill: the ceiling the lanes had before a disc of 21 server rows was measured.
constexpr size_t kOldCeilingBytes = 32 * 1024;
}  // namespace

size_t MaxBodyBytes() {
    static const bool old = coop::config::ResolveFlag(coop::config_registry::rows::disc_ceiling_32k);
    return old ? kOldCeilingBytes : kTransportBodyBytes;
}

}  // namespace coop::disc_record
