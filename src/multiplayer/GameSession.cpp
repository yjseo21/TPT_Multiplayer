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
        // Paste uploads: chunks are messages of their own, then the paste itself.
        if (command.isMember("type") || command["op"] == "paste")
        {
            double cost = command.isMember("type") ? 0 : XRES*YRES;
            parsed.push_back({std::move(command), cost});
            work += cost;
            continue;
        }
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
           operationTokens >= 1 && workTokens >= outbox.front().cost && session.Buffered() < 32*1024)
    {
        auto &next = outbox.front();
        if (next.command.isMember("type"))
        {
            // An upload chunk: not an edit (no sequence), but it uses the operation budget.
            if (!session.SendControl(next.command)) break;
            operationTokens -= 1;
            outbox.pop_front();
            continue;
        }
        if (!session.Send(next.command)) break;
        auto op = next.command["op"].asString();
        if (op == "stickman" || op == "stickman_spawn") --stickmanQueued;
        if (op == "tool") unapplied.push_back({std::move(next.command), session.Edits().Last(), now});
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
    if (!session.Synchronized()) { outbox.clear(); queuedWork = 0; unapplied.clear(); stickmanQueued = 0; }
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
        status = "TOO SLOW FOR THIS ROOM (behind for " + std::to_string(int(sample["behindSeconds"].asDouble())) + " s)";
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
bool GameSession::SendStickman(Json::Value command, std::string &why)
{
    // Stickman commands go out at once, ahead of any stroke still being paced
    // out; placing and steering take the same path, so they stay in order.
    Flush();
    if (stickmanQueued == 0 && operationTokens >= 1 && workTokens >= 1 &&
        session.Edits().Pending() < MaxUnconfirmed && session.Send(command))
    {
        operationTokens -= 1; workTokens -= 1;
        return true;
    }
    // Saturated: queue it, and keep later stickman commands behind it.
    if (!Enqueue({command}, why)) return false;
    ++stickmanQueued;
    return true;
}
bool GameSession::Sign(int index, int expectX, int expectY, bool remove, const std::string &text, int x, int y, int justification, std::string &why)
{
    if (!session.Synchronized()) { why = "The room is still synchronizing."; return false; }
    Json::Value c; c["op"] = "sign"; c["index"] = index;
    if (index >= 0) { c["x2"] = expectX; c["y2"] = expectY; }
    if (remove) c["remove"] = true;
    else { c["text"] = text; c["x1"] = x; c["y1"] = y; c["justification"] = justification; }
    return Enqueue({c}, why);
}
bool GameSession::SignButton(int index, int x, int y, std::string &why)
{
    if (!session.Synchronized()) { why = "The room is still synchronizing."; return false; }
    Json::Value c; c["op"] = "sign_button"; c["index"] = index; c["x1"] = x; c["y1"] = y;
    return Enqueue({c}, why);
}
bool GameSession::Paste(const std::vector<char> &save, int x, int y, bool includePressure, std::string &why)
{
    if (!session.Synchronized()) { why = "The room is still synchronizing."; return false; }
    if (save.size() > MaxPasteBytes)
    {
        why = "This is too large to paste into a room (over " + std::to_string(MaxPasteBytes/1024) + " KB as a save file).";
        return false;
    }
    auto text = Base64Encode(std::string_view(save.data(), save.size()));
    auto id = ++pasteIds;
    std::vector<Json::Value> parts;
    for (size_t at = 0; at < text.size(); at += MaxPasteChunkChars)
    {
        Json::Value chunk; chunk["type"] = "paste_chunk"; chunk["id"] = Json::UInt64(id);
        chunk["data"] = text.substr(at, MaxPasteChunkChars);
        parts.push_back(std::move(chunk));
    }
    Json::Value paste; paste["op"] = "paste"; paste["upload"] = Json::UInt64(id);
    paste["x1"] = std::clamp(x, -XCELLS, XCELLS); paste["y1"] = std::clamp(y, -YCELLS, YCELLS); paste["pressure"] = includePressure;
    parts.push_back(std::move(paste));
    return Enqueue(std::move(parts), why);
}
bool GameSession::Reset(const std::string &what, std::string &why)
{
    Json::Value c; c["op"] = "reset"; c["what"] = what; return SendAdmin(c, why);
}
bool GameSession::Stickman(int comm, int pcomm, std::string &why)
{
    if (!session.Synchronized()) { why = "The room is still synchronizing."; return false; }
    Json::Value c; c["op"] = "stickman"; c["comm"] = comm; c["pcomm"] = pcomm;
    return SendStickman(c, why);
}
bool GameSession::SpawnStickman(int x, int y, std::string &why)
{
    if (!session.Synchronized()) { why = "The room is still synchronizing."; return false; }
    if (!OwnStickman()) { why = "This room has no stickman left for you."; return false; }
    Json::Value c; c["op"] = "stickman_spawn";
    c["x1"] = std::clamp(x, CELL, XRES-CELL-1); c["y1"] = std::clamp(y, CELL, YRES-CELL-1);
    return SendStickman(c, why);
}
bool GameSession::ChangeSettings(const SimulationSettingsChange &settings, std::string &why)
{
    Json::Value c; c["op"] = "settings"; c["settings"] = SettingsJson(settings); return SendAdmin(c, why);
}
}
