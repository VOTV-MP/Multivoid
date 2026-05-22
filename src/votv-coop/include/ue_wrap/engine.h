// ue_wrap/engine.h -- high-level engine operations built on reflection.
//
// Engine-wrapper layer (principle 7): wraps specific engine UFunction calls
// behind a clean C++ API. No gameplay/network/coop state lives here -- just the
// marshaling to drive the engine through reflection::CallFunction (ProcessEvent).
//
// IMPORTANT: every call here invokes a UFunction, so it MUST run on the game
// thread (post via ue_wrap::game_thread::Post). Calling from another thread will
// crash the engine.

#pragma once

namespace ue_wrap::engine {

// Run a console command (UKismetSystemLibrary::ExecuteConsoleCommand) -- the
// universal lever for "open <map>", "HighResShot ...", etc. Resolves the
// KismetSystemLibrary CDO + UFunction and a valid world context on first use
// (cached). Returns false if anything could not be resolved. Game thread only.
bool ExecuteConsoleCommand(const wchar_t* command);

}  // namespace ue_wrap::engine
