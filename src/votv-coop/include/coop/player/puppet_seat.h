// coop/player/puppet_seat.h -- a remote rider's body on its ATV: the driver's in the seat, a passenger's behind it. The driver's own player is attached to the
// ATV and hidden while it drives, and its stream carries a standing pose that trails the ATV. While the host's seat
// table puts the peer on an ATV, the puppet is placed in that ATV's driver seat (ue_wrap::atv::GetSeatTransform) every
// frame, in the AnimBP's ATV seat pose (`isOnAtv`, the pose a kerfur rides in), with its
// collision off so it cannot push the ATV it sits on, and attached to the ATV so the ATV's own movement carries it within
// the frame. Let go, it is detached, the pose and collision come back and the stream places the body again. Game thread.

#pragma once

#include <cstdint>

namespace coop {

class PuppetSeat {
public:
    // The ATV the peer sits on, or nullptr, at the handlebars or (`passenger`) in the spot behind them
    // (ue_wrap::atv_passenger_seat::BodyTransform). True when it changed; a change lets the old seat go first.
    bool Set(void* puppet, void* atv, bool passenger = false);
    bool Active() const { return atv_ != nullptr; }
    // Place the puppet in the seat. False when not seated, or when the ATV is gone (the seat is then let go).
    bool Drive(void* puppet);
    // Let the seat go: the pose and collision come back. Nothing when not seated.
    void Release(void* puppet);
    // Forget the seat without touching the puppet (the puppet is being destroyed).
    void Forget() { atv_ = nullptr; atvIdx_ = -1; attached_ = false; }

private:
    void* atv_ = nullptr;
    int32_t atvIdx_ = -1;
    bool passenger_ = false;
    bool attached_ = false;   // the body is attached to the ATV, which carries it within the frame
};

}  // namespace coop
