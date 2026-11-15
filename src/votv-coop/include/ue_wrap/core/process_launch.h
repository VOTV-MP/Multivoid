// ue_wrap/core/process_launch.h -- a watch on the processes this game launches: a detour on kernel32's
// CreateProcessW that names each launch to one registered callback, on the launching thread, before the process is
// created, then creates it unchanged. The camcorder's recording plugin (MSC_Record) launches its merger this way, once
// the capture's audio file is written and before anything reads it.
//
// Engine-wrapper layer (principle 7): no gameplay logic; the callback is the caller's. The hook is installed once
// through ue_wrap::hook and never removed (hook.h's retirement rule).

#pragma once

namespace ue_wrap::process_launch {

// The launch about to happen: the application name and the command line as CreateProcessW was given them (either may
// be null). It runs on the launching thread and must return; the launch waits for it.
using BeforeLaunch = void (*)(const wchar_t* application, const wchar_t* commandLine);

// Installs the detour (once) and registers `fn` (a later call replaces it). False when the hook could not be made.
bool Install(BeforeLaunch fn);

}  // namespace ue_wrap::process_launch
