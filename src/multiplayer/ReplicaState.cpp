#include "ReplicaState.h"
#include "CommandRecord.h"
#include "ExactCheckpoint.h"
#include "StateDigest.h"
#include <limits>
#include <stdexcept>
#include <string>

namespace multiplayer
{
namespace
{
uint64_t Required64(const Json::Value &value, const char *key)
{
    if (!value[key].isUInt64()) throw std::runtime_error(std::string("invalid replica field ")+key);
    return value[key].asUInt64();
}
}
ReplicaDesync::ReplicaDesync(uint64_t t, uint64_t r, Json::Value fields) :
    std::runtime_error("REPLICA DESYNC: complete simulation fingerprint mismatch at tick "+std::to_string(t)),
    tick(t), revision(r), localFields(std::move(fields)) {}
void ReplicaState::OnJson(const Json::Value &v)
{
    if (!v["type"].isString()) throw std::runtime_error("invalid replica message type");
    auto type = v["type"].asString();
    if (type == "welcome")
    {
        if (welcomed || !v["protocol"].isUInt() || v["protocol"].asUInt() != ProtocolVersion ||
            !v["build"].isString() || v["build"].asString() != BuildId ||
            !v["localSimulation"].isBool() || !v["localSimulation"].asBool())
            throw std::runtime_error("server did not negotiate local simulation");
        welcomed = true;
    }
    else if (type == "checkpoint_begin")
    {
        if (!welcomed || checkpointPending || ready)
            throw std::runtime_error("unexpected replica checkpoint start");
        checkpointId = Required64(v,"id");
        auto size = Required64(v,"bytes");
        checkpointTick = Required64(v,"tick");
        checkpointRevision = Required64(v,"revision");
        checkpointReset = Required64(v,"resetGeneration");
        if (!v["paused"].isBool()) throw std::runtime_error("invalid checkpoint pause state");
        checkpointPaused = v["paused"].asBool();
        incoming.Begin(checkpointId,size);
        checkpointPending = true;
    }
    else if (type == "checkpoint_end")
    {
        if (!checkpointPending || Required64(v,"id") != checkpointId ||
            !incoming.Complete() || !receivedCheckpoint.empty())
            throw std::runtime_error("incomplete replica checkpoint transfer");
        RestoreExactCheckpoint(world,incoming.Take());
        if (world.tick != checkpointTick || world.revision != checkpointRevision ||
            world.resetGeneration != checkpointReset || world.paused != checkpointPaused)
            throw std::runtime_error("checkpoint metadata does not match authenticated declaration");
        checkpointPending = false; ready = true; sealedTick = world.tick;
    }
    else if (type == "command_commit" || type == "tick_seal")
    {
        if (!ready || checkpointPending) throw std::runtime_error("simulation event received before checkpoint");
        if (type == "tick_seal")
        {
            uint64_t tick=Required64(v,"tick");
            if (!v["paused"].isBool() || tick < sealedTick || tick-sealedTick > 10000)
                throw std::runtime_error("invalid or unbounded replica tick seal");
            sealedTick = tick;
        }
        if (!paced) { type == "tick_seal" ? Seal(v) : Commit(v); return; }
        // Bounded: about 2.5 minutes of per-tick seals, far beyond any pacing lag.
        if (queued.size() >= 8192) throw std::runtime_error("replica fell too far behind the server");
        queued.push_back(v);
    }
    else if (type == "error") throw std::runtime_error("server: "+v["message"].asString());
    // Ignore presence, telemetry, acks and room administration broadcasts.
}
void ReplicaState::Commit(const Json::Value &v)
{
    uint64_t id=Required64(v,"id"), tick=Required64(v,"tick");
    if (!id || (previousEventId && (previousEventId == UINT64_MAX || id != previousEventId+1)) || tick != world.tick)
        throw std::runtime_error("replica missed or reordered a command");
    auto command = ReadRecordedCommand(v["command"]);
    world.Apply(command);
    if (world.tick != Required64(v,"afterTick") ||
        world.revision != Required64(v,"revision") ||
        world.resetGeneration != Required64(v,"resetGeneration"))
        throw std::runtime_error("replica command changed incorrect tick/revision");
    previousEventId = id; ++edits;
    if (committed) committed(v);
}
void ReplicaState::Seal(const Json::Value &v)
{
    uint64_t tick=Required64(v,"tick"), rev=Required64(v,"revision");
    if (tick < world.tick) throw std::runtime_error("invalid or unbounded replica tick seal");
    while (world.tick < tick)
    {
        if (world.paused) throw std::runtime_error("server advanced while replica paused");
        world.Advance();
    }
    if (world.tick != tick || world.revision != rev ||
        world.paused != v["paused"].asBool() ||
        world.resetGeneration != Required64(v,"resetGeneration"))
        throw std::runtime_error("replica metadata diverged at tick seal");
    if (!v["digest"].isNull())
    {
        if (!v["digest"].isString()) throw std::runtime_error("invalid replica digest");
        auto fields = DiagnosticDigests(world);
        if (v["digest"].asString() != DigestSummary(fields)) throw ReplicaDesync(tick, rev, std::move(fields));
        ++checks;
    }
    ++seals;
}
uint64_t ReplicaState::Pump(uint64_t maxTicks)
{
    uint64_t stepped = 0;
    while (!queued.empty())
    {
        auto &v = queued.front();
        if (v["type"] == "tick_seal")
        {
            // Run toward the seal within the budget; finish it once it is reached.
            uint64_t tick = Required64(v,"tick");
            while (world.tick < tick && stepped < maxTicks)
            {
                if (world.paused) throw std::runtime_error("server advanced while replica paused");
                world.Advance(); ++stepped;
            }
            if (world.tick < tick) break;
            Seal(v);
        }
        else Commit(v);
        queued.pop_front();
    }
    return stepped;
}
void ReplicaState::OnBinary(std::string_view bytes)
{
    if (!checkpointPending) throw std::runtime_error("unsolicited binary world state");
    incoming.Accept(bytes);
}
}
