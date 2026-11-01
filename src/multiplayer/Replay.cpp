#include "World.h"
#include "Journal.h"
#include "CommandRecord.h"
#include "StateDigest.h"
#include "simulation/SimulationData.h"
#include "prefs/GlobalPrefs.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace multiplayer;
namespace
{
void Need(bool ok, const std::string &error)
{
    if (!ok) throw std::runtime_error(error);
}
uint64_t U64(const Json::Value &v, const char *name)
{
    Need(v[name].isUInt64(), std::string("invalid ") + name);
    return v[name].asUInt64();
}
int Run(const std::string &path, const std::string &digestOutput)
{
    std::ifstream input(path);
    Need(bool(input), "cannot open replay journal");
    std::ofstream output;
    if (!digestOutput.empty())
    {
        output.open(digestOutput, std::ios::trunc);
        Need(bool(output), "cannot open digest output");
    }
    GlobalPrefs prefs;
    SimulationData data;
    World world;
    TickTimeline timeline;
    std::string line;
    uint64_t lastCheck = 0, checks = 0, edits = 0;
    bool headerSeen = false;
    for (uint64_t lineNumber = 1; std::getline(input, line); ++lineNumber)
    {
        if (line.empty()) continue;
        try
        {
            Json::Value row = ParseJson(line);
            Need(row["kind"].isString(), "missing row kind");
            const std::string kind = row["kind"].asString();
            if (!headerSeen)
            {
                Need(kind == "header" && row["format"] == "tpt-replay-v1" &&
                    row["build"] == BuildId && row["checkpoint"] == "empty-world-only" &&
                    row["seed"].isUInt(), "unsupported replay header/build");
                Need(row["paused"].isBool() && row["ensureDeterminism"].isBool(),
                    "missing initial simulation settings");
                world.paused = row["paused"].asBool();
                world.sim->ensureDeterminism = row["ensureDeterminism"].asBool();
                world.sim->rng.seed(row["seed"].asUInt());
                headerSeen = true;
                continue;
            }
            if (kind == "edit")
            {
                uint64_t expectedTick = U64(row, "tick");
                Need(expectedTick >= world.tick, "out-of-order command tick");
                // A recorded gap means the authoritative server ran without
                // edits. Replay the missing physics steps, never wall-clock time.
                while (world.tick < expectedTick)
                {
                    Need(!world.paused, "cannot jump physics ticks while paused");
                    world.Advance();
                }
                auto command = ReadRecordedCommand(row["command"]);
                auto beforeTick = world.tick, beforeRevision = world.revision;
                world.Apply(command);
                auto stamp = timeline.Commit(beforeTick, world.tick, beforeRevision,
                    world.revision, world.resetGeneration);
                Need(stamp.eventId == U64(row,"eventId") &&
                    stamp.tick == expectedTick && stamp.epoch == U64(row,"epoch") &&
                    stamp.ordinal == U64(row,"ordinal") &&
                    stamp.afterTick == U64(row,"afterTick") &&
                    stamp.revision == U64(row,"revision") &&
                    stamp.resetGeneration == U64(row,"resetGeneration"),
                    "replayed command ordering differs");
                ++edits;
            }
            else if (kind == "check")
            {
                uint64_t expectedTick = U64(row, "tick");
                Need(expectedTick >= world.tick, "out-of-order check tick");
                while (world.tick < expectedTick)
                {
                    Need(!world.paused, "cannot advance physics while paused");
                    world.Advance();
                }
                Need(world.revision == U64(row,"revision") &&
                    world.resetGeneration == U64(row,"resetGeneration"),
                    "replayed world revision differs");
                auto fields = DiagnosticDigests(world);
                Need(row["fields"].isObject(), "malformed expected digest fields");
                for (auto &key : fields.getMemberNames())
                    if (fields[key] != row["fields"][key])
                    {
                        std::cerr << "DIVERGENCE tick=" << world.tick << " field=" << key
                                  << " expected=" << row["fields"][key].asString()
                                  << " actual=" << fields[key].asString() << '\n';
                        return 2;
                    }
                Need(row["fields"].size() == fields.size() &&
                    row["digest"] == DigestSummary(fields), "digest field registry mismatch");
                if (output)
                {
                    Json::Value sample;
                    sample["tick"] = Json::UInt64(world.tick);
                    sample["digest"] = row["digest"];
                    output << JsonText(sample) << '\n';
                }
                lastCheck = world.tick;
                ++checks;
            }
            else throw std::runtime_error("unknown journal row kind");
        }
        catch (const std::exception &e)
        {
            throw std::runtime_error("journal line " + std::to_string(lineNumber) + ": " + e.what());
        }
    }
    Need(headerSeen && checks > 0 && input.eof(), "incomplete replay journal");
    std::cout << "Replay verified: " << checks << " checks, " << edits
              << " edits, final check tick " << lastCheck << '\n';
    return 0;
}
}
int main(int argc, char **argv)
{
    try
    {
        if (argc != 3 && argc != 5)
            throw std::runtime_error("usage: tpt-replay --journal FILE [--digest-output FILE]");
        if (std::string(argv[1]) != "--journal" ||
            (argc == 5 && std::string(argv[3]) != "--digest-output"))
            throw std::runtime_error("invalid replay arguments");
        return Run(argv[2], argc == 5 ? argv[4] : "");
    }
    catch (const std::exception &e) { std::cerr << "Replay failed: " << e.what() << '\n'; return 1; }
}
