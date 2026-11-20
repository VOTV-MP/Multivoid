// coop/dev/cleanup_trace.h -- the session's clean-up calls, named as each runs.
//
// subsystems.cpp's DisconnectSlot and DisconnectAll fan out to every lane that holds per-peer or
// per-session state. A change that moves calls between the files holding that fan-out is proven
// by the sequence of calls it runs: with the `cleanup_trace` row set, each wrapped call logs
// "cleanup-trace: <phase> <the call's own text>" before it runs, so one run on the bytes before
// the move and one after give two lists that must be equal line for line. Row off (the default),
// the cost of a wrapped call is one latched bool test.
//
// Dev only. Nothing here ships behaviour: the wrapped call runs exactly as written.

#pragma once

namespace coop::dev::cleanup_trace {

// The `cleanup_trace` row, read once. Game thread.
bool On();

// Logs one line: the phase ("peer-left" or "disconnect") and the call's own text. Game thread.
void Step(const char* phase, const char* call);

}  // namespace coop::dev::cleanup_trace

// Runs `call` after naming it when the row is set. `call` is the whole statement's expression.
#define COOP_CLEANUP_STEP(phase, ...)                                         \
    do {                                                                      \
        if (::coop::dev::cleanup_trace::On())                                 \
            ::coop::dev::cleanup_trace::Step(phase, #__VA_ARGS__);            \
        __VA_ARGS__;                                                          \
    } while (0)
