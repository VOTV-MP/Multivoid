// coop/dev/log_drill.h -- [dev] two processes of one install keep separate logs (L18). Once at boot, with
// log_drill=1: a process writing the install's usual log name says so; one that found it held writes its own
// multivoid.<PID>.log, reads the usual file's process line and passes when that names another process, its own file
// carrying its PID. Run with mp.py twolog, which starts the second process from the host's own folder;
// "[LOG-DRILL] DONE" is the pass, "[LOG-DRILL] FAIL" the lane failing.

#pragma once

namespace coop::dev::log_drill {

// The one check, at boot; a latched read when off. Reads files only, no engine. Any thread.
void RunOnce();

}  // namespace coop::dev::log_drill
