#include "Journal.h"
#include "CommandRecord.h"
#include "StateDigest.h"
#include "World.h"
#include <stdexcept>

namespace multiplayer
{
void Journal::Write(const Json::Value &record)
{
    // Never put any authentication token into this journal.
    file << JsonText(record) << '\n';
    file.flush();
    if (!file) throw std::runtime_error("deterministic journal write failed");
}
Journal::Journal(const std::string &path, uint32_t seed, unsigned tickRate, const World &initial)
    : file(path, std::ios::out | std::ios::trunc)
{
    if (!file) throw std::runtime_error("cannot create deterministic journal");
    Json::Value header;
    header["kind"] = "header"; header["format"] = "tpt-replay-v1";
    header["build"] = BuildId;
    header["seed"] = seed; header["tickRate"] = tickRate;
    header["paused"] = initial.paused; header["ensureDeterminism"] = initial.sim->ensureDeterminism;
    header["checkpoint"] = "empty-world-only";
    Write(header);
    Check(initial, true);
}
void Journal::Edit(const EditStamp &stamp, uint32_t actor, const Command &command)
{
    Json::Value row;
    row["kind"] = "edit";
    row["eventId"] = Json::UInt64(stamp.eventId);
    row["epoch"] = Json::UInt64(stamp.epoch);
    row["tick"] = Json::UInt64(stamp.tick);
    row["ordinal"] = Json::UInt64(stamp.ordinal);
    row["afterTick"] = Json::UInt64(stamp.afterTick);
    row["revision"] = Json::UInt64(stamp.revision);
    row["resetGeneration"] = Json::UInt64(stamp.resetGeneration);
    row["actor"] = actor;
    row["command"] = RecordCommand(command);
    Write(row);
}
void Journal::Check(const World &world, bool force)
{
    if (!force && (world.tick < lastCheckedTick + 60)) return;
    Json::Value row;
    row["kind"] = "check";
    row["tick"] = Json::UInt64(world.tick);
    row["revision"] = Json::UInt64(world.revision);
    row["resetGeneration"] = Json::UInt64(world.resetGeneration);
    row["fields"] = DiagnosticDigests(world);
    row["digest"] = DigestSummary(row["fields"]);
    Write(row);
    lastCheckedTick = world.tick;
}
}
