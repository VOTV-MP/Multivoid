// l10n/quiet.h -- the selftest's quiet switch (private to src/l10n/). While the selftest drives the
// refusals on purpose, the call-site defect lines of Fmt and Label -- said once per process -- are
// neither printed nor spent, so a real defect later is still said.
#pragma once

#include <atomic>

namespace l10n::detail {

extern std::atomic<bool> g_selftestQuiet;

}  // namespace l10n::detail
