// coop/net/prop_spawn_hook.h -- a drill seam on Session::SendPropSpawn, the one funnel every PropSpawn broadcast passes
// (the Init body, the spawn watcher, the census drain, a trash collect). Null by default: a drill that must know a
// birth's spawn went out sets it, and clears it when done. Header-only so the session's own header stays as it is.

#pragma once

#include <cstdint>

namespace coop::net::prop_spawn_hook {

// Called on the thread that sent, right after the send, with the PropSpawn's element id and whether it entered the
// stream or the backlog (false: the session refused it).
using PropSpawnSentFn = void (*)(uint32_t eid, bool sent);

inline PropSpawnSentFn& PropSpawnSentHook() {
    static PropSpawnSentFn fn = nullptr;
    return fn;
}

}  // namespace coop::net::prop_spawn_hook
