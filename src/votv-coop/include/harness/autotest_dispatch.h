// harness/autotest_dispatch.h -- env-gated autonomous-test thread dispatch.
//
// Extracted from harness/harness.cpp (2026-05-30, soft-cap discipline): the
// cluster of `if (ReadEnv("VOTVCOOP_RUN_*_TEST")) CreateThread(...)` blocks had
// grown to five near-identical copies and pushed harness.cpp past 800 LOC. They
// are boot/scenario glue, but a cohesive, still-growing unit -- one per
// autonomous test -- so they get their own home + a single SpawnIf helper that
// removes the copy-paste.
#pragma once

#include "coop/net/session.h"  // coop::net::Role

namespace harness::autotest {

// Spawn each autonomous-test worker thread whose VOTVCOOP_RUN_*_TEST env flag is
// "1". `role` only feeds the per-spawn log line -- every test routine self-gates
// via autotest::IsClientRole() internally (host-only / client-only / both; the
// registry net.role resolve, arc 3 T2b). Call once from the play-ready path
// after the session has started.
void SpawnEnvGatedTests(coop::net::Role role);

}  // namespace harness::autotest
