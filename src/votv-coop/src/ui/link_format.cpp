// ui/link_format.cpp -- see ui/link_format.h.

#include "ui/link_format.h"

#include <cstdio>

namespace ui::link_format {

using coop::net::LinkKind;

const char* LinkLabel(LinkKind kind) {
    switch (kind) {
        case LinkKind::Local:   return "n/a";     // the host -- no link exists to describe
        case LinkKind::Lan:     return "LAN";
        case LinkKind::Direct:  return "DIRECT";
        case LinkKind::Relayed: return "RELAY";
        case LinkKind::Unknown: break;
    }
    return "--";  // no measurement has landed yet
}

void FormatPing(int pingMs, LinkKind kind, char* out, int outLen) {
    if (!out || outLen <= 0) return;
    if (kind == LinkKind::Local)  { std::snprintf(out, static_cast<size_t>(outLen), "n/a");  return; }
    if (pingMs > 0)  std::snprintf(out, static_cast<size_t>(outLen), "%dms", pingMs);
    else if (pingMs == 0) std::snprintf(out, static_cast<size_t>(outLen), "<1ms");  // sub-ms LAN
    else std::snprintf(out, static_cast<size_t>(outLen), "--");
}

}  // namespace ui::link_format
