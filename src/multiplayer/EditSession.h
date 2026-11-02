#pragma once
#include <cstdint>
#include <cstddef>
#include <set>

namespace multiplayer
{
// Commands belong to one connection. A disconnect records uncertainty and drops
// pending IDs; there is deliberately no replay queue across sessions.
class EditSession
{
    uint64_t next = 0, uncertain = 0;
    std::set<uint64_t> pending;
public:
    uint64_t Begin() { pending.insert(++next); return next; }
    void Acknowledge(uint64_t id) { pending.erase(id); }
    void Disconnect() { uncertain += pending.size(); pending.clear(); next = 0; }
    std::size_t Pending() const { return pending.size(); }
    uint64_t Last() const { return next; } // the latest Begin(), 0 on a new connection
    uint64_t Uncertain() const { return uncertain; }
};
}
