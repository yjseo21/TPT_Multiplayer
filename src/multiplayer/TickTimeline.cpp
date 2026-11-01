#include "TickTimeline.h"
#include <limits>
#include <stdexcept>

namespace multiplayer
{
TickTimeline::TickTimeline(uint64_t tick, uint64_t revision, uint64_t reset)
    : lastTick(tick), lastRevision(revision), lastReset(reset), lastStampedTick(tick) {}

EditStamp TickTimeline::Commit(uint64_t beforeTick, uint64_t afterTick,
    uint64_t beforeRevision, uint64_t afterRevision, uint64_t resetGeneration)
{
    if (beforeTick < lastTick || beforeRevision != lastRevision ||
        beforeRevision == std::numeric_limits<uint64_t>::max() ||
        afterRevision != beforeRevision + 1 ||
        (afterTick != beforeTick && (beforeTick == std::numeric_limits<uint64_t>::max() ||
                                     afterTick != beforeTick + 1)) ||
        resetGeneration < lastReset || resetGeneration - lastReset > 1 ||
        eventId == std::numeric_limits<uint64_t>::max())
        throw std::runtime_error("invalid deterministic edit timeline");
    if (beforeTick != lastStampedTick || resetGeneration != lastReset) ordinal = 0;
    if (ordinal == std::numeric_limits<uint64_t>::max())
        throw std::runtime_error("edit ordinal overflow");
    if (resetGeneration != lastReset)
    {
        if (epoch == std::numeric_limits<uint64_t>::max())
            throw std::runtime_error("edit epoch overflow");
        ++epoch;
    }
    EditStamp stamp{++eventId, epoch, beforeTick, ordinal++, afterTick,
                    afterRevision, resetGeneration};
    lastStampedTick = beforeTick;
    lastTick = afterTick;
    lastRevision = afterRevision;
    lastReset = resetGeneration;
    return stamp;
}
}
