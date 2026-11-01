#pragma once
#include <cstdint>

namespace multiplayer
{
// One globally ordered edit is committed immediately before a physics tick.
// Edits while paused share a tick, but never an ordinal. A Step command can
// itself advance the tick. The stamp records both sides to make replay exact.
struct EditStamp
{
    uint64_t eventId = 0;
    uint64_t epoch = 0;
    uint64_t tick = 0;
    uint64_t ordinal = 0;
    uint64_t afterTick = 0;
    uint64_t revision = 0;
    uint64_t resetGeneration = 0;
};

class TickTimeline
{
    uint64_t lastTick = 0, lastRevision = 0, lastReset = 0, lastStampedTick = 0;
    uint64_t ordinal = 0, eventId = 0, epoch = 0;
public:
    TickTimeline(uint64_t initialTick = 0, uint64_t initialRevision = 0,
        uint64_t initialResetGeneration = 0);
    EditStamp Commit(uint64_t beforeTick, uint64_t afterTick,
        uint64_t beforeRevision, uint64_t afterRevision,
        uint64_t resetGeneration);
    uint64_t LastEventId() const { return eventId; }
};
}
