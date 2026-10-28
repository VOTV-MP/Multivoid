// coop/net/receive_budget.cpp -- see coop/net/receive_budget.h.

#include "coop/net/receive_budget.h"

#include "ue_wrap/core/log.h"

namespace coop::net {

void ReceiveBudget::Configure(long burst, long perSecond) {
    budget_.burst = static_cast<float>(burst);
    budget_.perSecond = static_cast<float>(perSecond);
    for (Slot& s : slots_) s = Slot{};
}

void ReceiveBudget::ConfigureStreams(int perSecond) {
    streamBudget_.burst = static_cast<float>(perSecond);
    streamBudget_.perSecond = static_cast<float>(perSecond);
    for (Slot& s : slots_) s = Slot{};
}

int ReceiveBudget::StreamIndex(MsgType type) {
    switch (type) {
    case MsgType::PoseSnapshot:   return 0;
    case MsgType::PropPose:       return 1;
    case MsgType::RagdollPose:    return 2;
    case MsgType::HandPose:       return 3;
    case MsgType::DeskCursorPose: return 4;
    case MsgType::ClockPose:      return 5;
    case MsgType::DeskSimPose:    return 6;
    case MsgType::DishPose:       return 7;
    case MsgType::ReelPose:       return 8;
    case MsgType::VoiceFrame:     return 9;
    case MsgType::PropDrivePose:  return 10;
    default:                      return -1;
    }
}

ReceiveBudget::Slot& ReceiveBudget::For(int slot, uint32_t hConn) {
    Slot& s = slots_[slot];
    if (s.hConn != hConn) {
        s.hConn = hConn;
        s.spent = false;
        s.messages.Reset();
        for (IntentBucket& b : s.streams) b.Reset();
    }
    return s;
}

ReceiveBudget::Take ReceiveBudget::TakePacket(int slot, uint32_t hConn, MsgType type, uint64_t nowMs) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return Take::Ok;
    const int stream = StreamIndex(type);
    if (stream >= 0) {
        if (streamBudget_.perSecond <= 0.0f) return Take::Ok;
        return For(slot, hConn).streams[stream].Take(streamBudget_, nowMs) ? Take::Ok : Take::StreamOver;
    }
    if (budget_.perSecond <= 0.0f) return Take::Ok;
    Slot& s = For(slot, hConn);
    if (s.spent) return Take::Spent;
    if (s.messages.Take(budget_, nowMs)) return Take::Ok;
    s.spent = true;
    return Take::Emptied;
}

bool ReceiveBudget::RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("receive_budget selftest FAIL: %s", what);
    };
    using T = Take;
    {
        ReceiveBudget b;
        b.Configure(4, 2);
        b.ConfigureStreams(3);
        bool ok = true;
        for (int i = 0; i < 4; ++i) ok = ok && b.TakePacket(1, 7, MsgType::Reliable, 1000) == T::Ok;
        check(ok, "a fresh connection takes its burst");
        check(b.TakePacket(1, 7, MsgType::Reliable, 1000) == T::Emptied, "the next empties it");
        check(b.TakePacket(1, 7, MsgType::Reliable, 1001) == T::Spent, "and the one after finds it spent");
        check(b.TakePacket(1, 7, MsgType::Reliable, 100'000) == T::Spent, "spent stays spent for that connection");
        check(b.TakePacket(1, 8, MsgType::Reliable, 100'000) == T::Ok, "a new connection on the slot starts full");
        check(b.TakePacket(2, 9, MsgType::Reliable, 100'000) == T::Ok, "another slot is its own");
    }
    {
        // A type no stream names, a batch only the host sends, and a header that did not parse are messages: each
        // spends the one token a burst of 1 holds, and the reliable after it empties the bucket.
        const MsgType messages[] = {static_cast<MsgType>(200), MsgType::EntityPose, MsgType{}};
        for (const MsgType m : messages) {
            ReceiveBudget b;
            b.Configure(1, 1);
            b.ConfigureStreams(3);
            check(b.TakePacket(1, 7, m, 1000) == T::Ok && b.TakePacket(1, 7, MsgType::Reliable, 1000) == T::Emptied,
                  "a packet no stream names counts against the message bucket");
        }
    }
    {
        ReceiveBudget b;
        b.Configure(2, 10);
        b.ConfigureStreams(3);
        check(b.TakePacket(1, 7, MsgType::Reliable, 1000) == T::Ok, "one of two");
        check(b.TakePacket(1, 7, MsgType::Reliable, 1100) == T::Ok &&
                  b.TakePacket(1, 7, MsgType::Reliable, 1100) == T::Ok,
              "a tenth of a second at 10 a second refills one");
        check(b.TakePacket(1, 7, MsgType::Reliable, 1100) == T::Emptied, "and no more");
        bool ok = true;
        for (int i = 0; i < 3; ++i) ok = ok && b.TakePacket(2, 9, MsgType::PoseSnapshot, 1000) == T::Ok;
        check(ok, "a stream takes its burst");
        check(b.TakePacket(2, 9, MsgType::PoseSnapshot, 1000) == T::StreamOver, "and drops past it");
        check(b.TakePacket(2, 9, MsgType::VoiceFrame, 1000) == T::Ok, "another stream type is its own");
        check(b.TakePacket(2, 9, MsgType::PoseSnapshot, 1334) == T::Ok, "a third of a second refills one");
    }
    {
        // A dropped stream spends nothing of the message bucket: a burst of 1 still holds its token after it.
        ReceiveBudget b;
        b.Configure(1, 1);
        b.ConfigureStreams(1);
        check(b.TakePacket(1, 7, MsgType::PoseSnapshot, 1000) == T::Ok &&
                  b.TakePacket(1, 7, MsgType::PoseSnapshot, 1000) == T::StreamOver &&
                  b.TakePacket(1, 7, MsgType::Reliable, 1000) == T::Ok,
              "a dropped stream spends no message");
    }
    {
        ReceiveBudget b;
        b.Configure(4, 0);
        b.ConfigureStreams(0);
        bool ok = true;
        for (int i = 0; i < 100; ++i)
            ok = ok && b.TakePacket(1, 7, MsgType::Reliable, 1000) == T::Ok &&
                 b.TakePacket(1, 7, MsgType::PoseSnapshot, 1000) == T::Ok;
        check(ok, "a rate of 0 bounds nothing");
    }
    if (pass == total) {
        UE_LOGI("receive_budget selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("receive_budget selftest: %d of %d checks FAILED", total - pass, total);
    return false;
}

}  // namespace coop::net
