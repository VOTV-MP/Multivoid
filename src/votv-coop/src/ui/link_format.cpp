// ui/link_format.cpp -- see ui/link_format.h.

#include "ui/link_format.h"

#include "l10n/l10n.h"

#include <cstdio>

namespace ui::link_format {

using coop::net::LinkKind;

const char* LinkLabel(LinkKind kind) {
    switch (kind) {
        case LinkKind::Local:   return l10n::T("n/a");  // the host -- no link exists to describe
        case LinkKind::Lan:     return l10n::T("LAN");
        case LinkKind::Direct:  return l10n::T("DIRECT");
        case LinkKind::Relayed: return l10n::T("RELAY");
        case LinkKind::Unknown: break;
    }
    return "--";  // no measurement has landed yet
}

void FormatPing(int pingMs, LinkKind kind, char* out, int outLen) {
    if (!out || outLen <= 0) return;
    if (kind == LinkKind::Local)  { l10n::Fmt(out, static_cast<size_t>(outLen), "%s", l10n::T("n/a")); return; }
    if (pingMs > 0)  std::snprintf(out, static_cast<size_t>(outLen), "%dms", pingMs);
    else if (pingMs == 0) std::snprintf(out, static_cast<size_t>(outLen), "<1ms");  // sub-ms LAN
    else std::snprintf(out, static_cast<size_t>(outLen), "--");
}

const char* LobbyLinkLabel(const std::string& word) {
    if (word == "relay") return l10n::T("Relay");
    if (word == "direct") return l10n::T("Direct");
    if (word == "lan") return l10n::T("LAN");
    return "--";
}

}  // namespace ui::link_format
