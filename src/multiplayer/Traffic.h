#pragma once
#include "Canvas.h"
#include <deque>
#include <map>
#include <mutex>

namespace multiplayer
{
// The room burst is never replenished by joins, resyncs or view changes.
class UploadBudget
{
    struct Player
    {
        double ceiling;
        ByteBudget budget;
        size_t requested = 0;
        double requestedAt = 0;
        Player(double ceiling, double now) : ceiling(ceiling), budget(ceiling, now) {}
    };
    double rate;
    ByteBudget room;
    std::map<uint32_t, Player> players;
    std::deque<uint32_t> waiting;
    std::mutex mutex;
    void Allocate(double now);
public:
    UploadBudget(double bytesPerSecond, double now) : rate(bytesPerSecond), room(bytesPerSecond, now) {}
    void Join(uint32_t id, double ceiling, double now);
    void Leave(uint32_t id, double now);
    void Cancel(uint32_t id);
    double Share(uint32_t id);
    // Zero means both budgets were charged; otherwise returns retry delay.
    double Acquire(uint32_t id, size_t bytes, double now);
};

// Owned under the peer mutex. ACK delay includes transfer and client decode time.
class StreamPacer
{
    double period, interval, lastSent = -100, ackSeconds = 0;
    bool sampled = false;
public:
    explicit StreamPacer(double updatesPerSecond);
    unsigned Window() const { return sampled ? 2 : 1; }
    bool Ready(double now, unsigned inFlight, double oldestSent) const;
    void Sent(double now) { lastSent = now; }
    void Acknowledge(double seconds);
    double Hz() const { return 1/interval; }
    double AckMs() const { return ackSeconds*1000; }
    bool Limited() const { return interval > period*1.25; }
};
}
