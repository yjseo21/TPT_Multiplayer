#include "Traffic.h"
#include <algorithm>
#include <cmath>

namespace multiplayer
{
void UploadBudget::Allocate(double now)
{
    for (auto &[id, p] : players) p.budget.SetRate(std::min(p.ceiling, rate/players.size()), now);
}
void UploadBudget::Join(uint32_t id, double ceiling, double now)
{
    std::lock_guard lock(mutex);
    Allocate(now);
    if (!players.try_emplace(id, ceiling, now).second) throw std::runtime_error("duplicate upload participant");
    Allocate(now);
}
void UploadBudget::Leave(uint32_t id, double now)
{
    std::lock_guard lock(mutex);
    Allocate(now); players.erase(id); std::erase(waiting, id); Allocate(now);
}
void UploadBudget::Cancel(uint32_t id)
{
    std::lock_guard lock(mutex);
    std::erase(waiting, id);
    if (auto it = players.find(id); it != players.end()) it->second.requested = 0;
}
double UploadBudget::Share(uint32_t id)
{
    std::lock_guard lock(mutex);
    auto it = players.find(id);
    return it == players.end() ? 0 : std::min(it->second.ceiling, rate/players.size());
}
double UploadBudget::Acquire(uint32_t id, size_t bytes, double now)
{
    std::lock_guard lock(mutex);
    auto it = players.find(id);
    if (it == players.end()) throw std::runtime_error("unregistered upload participant");
    if (!bytes || bytes > MaxCanvasPacket) throw std::runtime_error("invalid upload packet size");
    auto &p = it->second;
    if (!p.requested) waiting.push_back(id);
    p.requested = bytes; p.requestedAt = now;
    // A sender that stops polling must not reserve the room forever. Normal
    // retries occur at most 50 ms apart; delayed network ACKs hold no reservation.
    std::erase_if(waiting, [&](uint32_t other) {
        auto &entry = players.at(other);
        if (now-entry.requestedAt <= .25) return false;
        entry.requested = 0; return true;
    });
    for (auto candidate : waiting)
    {
        auto &entry = players.at(candidate);
        if (entry.budget.Delay(entry.requested, now) > 0) continue;
        // Reserve credit for the first eligible request, so small frames cannot
        // repeatedly consume all credit and starve a joining keyframe.
        double sharedDelay = room.Delay(entry.requested, now);
        if (candidate != id) return std::max(.001, sharedDelay);
        if (sharedDelay > 0) return sharedDelay;
        room.Spend(bytes, now); p.budget.Spend(bytes, now);
        p.requested = 0; std::erase(waiting, id);
        return 0;
    }
    return std::max(.001, p.budget.Delay(bytes, now));
}
StreamPacer::StreamPacer(double updatesPerSecond) : period(1/updatesPerSecond), interval(period)
{
    if (!std::isfinite(updatesPerSecond) || updatesPerSecond < 1 || updatesPerSecond > 30)
        throw std::runtime_error("invalid stream rate");
}
bool StreamPacer::Ready(double now, unsigned inFlight, double oldestSent) const
{
    if (inFlight >= Window() || now-lastSent < interval) return false;
    if (inFlight && sampled && now-oldestSent > std::max(.25, ackSeconds*2)) return false;
    return true;
}
void StreamPacer::Acknowledge(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0) throw std::runtime_error("invalid acknowledgement delay");
    ackSeconds = sampled ? ackSeconds*.75+seconds*.25 : seconds;
    sampled = true;
    double desired = std::clamp(seconds*.75, period, 2.0);
    interval = desired > interval ? desired : std::max(period, interval*.8+desired*.2);
}
}
