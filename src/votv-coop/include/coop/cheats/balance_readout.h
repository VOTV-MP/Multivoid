// coop/cheats/balance_readout.h -- the credit balance as the Economy panel shows it. The balance is a game object read
// on the game thread; the panel is drawn on the render thread, so a refresh is posted (the panel asks every frame, the
// module limits itself to four a second) and the panel reads the last value. On a client the number is the host's,
// mirrored into its own game by the balance lane.

#pragma once

#include <cstdint>

namespace coop::cheats::balance_readout {

// Post a read of the balance on the game thread. Any thread; cheap to call every frame.
void RequestRefresh();

// The last balance read; false before the first. Any thread.
bool Latest(int32_t* out);

}  // namespace coop::cheats::balance_readout
