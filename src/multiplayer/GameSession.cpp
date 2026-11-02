#include "GameSession.h"
#include "Protocol.h"
#include "simulation/Simulation.h"
#include "Metrics.h"
#include <algorithm>
#include <cmath>

namespace multiplayer
{
GameSession::GameSession(SessionOptions options) :
    session([&] { options.localSim = true; return std::move(options); }()),
    placeholder(Simulation::Factory()), shown(placeholder.get()),
    workTokens(EditWorkPerSecond), operationTokens(120), refilledAt(MonotonicSeconds()) {}
namespace
{
// Slightly below the server's limits, so network timing never trips them.
constexpr double WorkRate = EditWorkPerSecond*.9, OperationRate = 120*.9;
constexpr size_t MaxUnconfirmed = 48;
}
bool GameSession::Enqueue(std::vector<Json::Value> commands, std::string &why)
{
    std::vector<Outgoing> parsed;
    double work = 0;
    for (auto &command : commands)
    {
        command["seq"] = Json::UInt64(1);
        double cost;
        try { cost = ParseCommand(command, true).Cost(); }
        catch (const std::exception &e) { why = e.what(); return false; }
        command.removeMember("seq");
        parsed.push_back({std::move(command), cost});
        work += cost;
    }
    // At most about two seconds of the room's editing budget may wait here; the
    // rest of an enormous stroke is skipped, as a lagging single-player game would.
    bool skipped = false;
    for (auto &entry : parsed)
    {
        if (queuedWork+entry.cost > 2*WorkRate) { skipped = true; break; }
        queuedWork += entry.cost; outbox.push_back(std::move(entry));
    }
    Flush();
    if (skipped) why = "This stroke is too large for the room to keep up; part of it was skipped.";
    return !skipped;
}
void GameSession::Flush()
{
    auto now = MonotonicSeconds();
    workTokens = std::min(EditWorkPerSecond*.9, workTokens+(now-refilledAt)*WorkRate);
    operationTokens = std::min(120*.9, operationTokens+(now-refilledAt)*OperationRate);
    refilledAt = now;
    while (!outbox.empty() && session.Edits().Pending() < MaxUnconfirmed &&
           operationTokens >= 1 && workTokens >= outbox.front().cost)
    {
        auto &next = outbox.front();
        if (!session.Send(next.command)) break;
        if (next.command["op"] == "tool") unapplied.push_back({std::move(next.command), session.Edits().Last(), now});
        workTokens -= next.cost; operationTokens -= 1;
        queuedWork -= next.cost;
        outbox.pop_front();
    }
}
GameSession::~GameSession() = default;
bool GameSession::Update()
{
    session.Update();
    // Unsent edits belong to the connection: never carried into a resynchronized world.
    if (!session.Synchronized()) { outbox.clear(); queuedWork = 0; unapplied.clear(); }
    Flush();
    // A rejected edit is never committed: its preview lapses after a few seconds.
    auto now = MonotonicSeconds();
    while (!unapplied.empty() && (unapplied.front().seq <= session.OwnCommitted() || now-unapplied.front().sentAt > 3))
        unapplied.pop_front();
    auto *replica = session.Replica();
    Simulation *next = session.Synchronized() && replica ? replica->world.sim.get() : placeholder.get();
    if (next == shown) return false;
    shown = next;
    return true;
}
bool GameSession::Paused() const
{
    auto *replica = session.Replica();
    return session.Synchronized() && replica && replica->world.paused;
}
std::string GameSession::Status()
{
    auto sample = session.Sample();
    std::string status = sample["status"].asString();
    if (status == "TOO SLOW")
        status = "TOO SLOW FOR THIS ROOM (" + std::to_string(int(sample["behindSeconds"].asDouble())) + " s behind)";
    std::string line = "Room " + session.Options().url + " | " + status;
    unsigned players = session.Players().size();
    if (players) line += " | " + std::to_string(players) + (players == 1 ? " player" : " players");
    if (session.Admin()) line += " | admin";
    if (!sample["rttMs"].isNull()) line += " | ping " + std::to_string(int(sample["rttMs"].asDouble()+.5)) + " ms";
    return line;
}
bool GameSession::Send(const ToolStroke &stroke, std::string &why)
{
    if (!session.Synchronized()) { why = "The room is still synchronizing."; return false; }
    if (!RoomSupportsTool(stroke.tool)) { why = "This tool is not available in rooms yet."; return false; }
    return Enqueue(StrokeCommands(stroke), why);
}
bool GameSession::SendAdmin(Json::Value command, std::string &why)
{
    if (!session.Admin()) { why = "Only the room's administrator can do this."; return false; }
    if (!session.Synchronized()) { why = "The room is still synchronizing."; return false; }
    return Enqueue({command}, why);
}
bool GameSession::SetPaused(bool paused, std::string &why)
{
    Json::Value c; c["op"] = "pause"; c["paused"] = paused; return SendAdmin(c, why);
}
bool GameSession::Step(std::string &why)
{
    Json::Value c; c["op"] = "step"; return SendAdmin(c, why);
}
bool GameSession::Clear(std::string &why)
{
    Json::Value c; c["op"] = "clear"; return SendAdmin(c, why);
}
bool GameSession::ChangeSettings(const SimulationSettingsChange &settings, std::string &why)
{
    Json::Value c; c["op"] = "settings"; c["settings"] = SettingsJson(settings); return SendAdmin(c, why);
}
}
