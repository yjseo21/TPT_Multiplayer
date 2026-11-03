#pragma once
#include "Session.h"
#include "simulation/SimulationSettings.h"
#include "simulation/StructProperty.h"
#include <json/json.h>
#include <deque>
#include <memory>
#include <string>
#include <vector>

class Simulation;
namespace multiplayer
{
// One tool use from the game's interface, as the game's controller performs it.
struct ToolStroke
{
    std::string tool;               // tool identifier, for example DEFAULT_PT_DUST
    std::string action;             // point, line, box, fill, click or drag
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int radiusX = 0, radiusY = 0;
    std::string brush = "ellipse";  // ellipse, rectangle or triangle
    float strength = 1;
    bool dragging = false, shift = false, ctrl = false, alt = false;
    int replaceFlags = 0, replaceSelected = 0;
    uint32_t decoColour = 0;        // decoration tools: ARGB
    int property = 0;               // property tool: property and value
    PropertyValue value = 0;
};
// Turns a stroke into protocol commands, splitting lines whose brush work
// exceeds the per-command limit. Coordinates are clamped to the editable area.
std::vector<Json::Value> StrokeCommands(const ToolStroke &stroke);
// Whether the server can replay this tool (elements, walls, simulation tools).
bool RoomSupportsTool(const std::string &identifier);

// The game's connection to a room: a local-simulation Session plus what the
// game's interface needs from it. Main thread only; call Update() every frame.
class GameSession
{
public:
    explicit GameSession(SessionOptions options);
    ~GameSession();
    // Drains the network and steps the room's world. True when Shown() now
    // returns a different simulation (synchronized, resynchronized or lost).
    bool Update();
    // The room's world when synchronized, otherwise an empty placeholder.
    // Valid until the next Update() that returns true.
    Simulation *Shown() const { return shown; }
    bool Synchronized() const { return session.Synchronized(); }
    bool Paused() const;
    bool Admin() const { return session.Admin(); }
    const SessionOptions &Options() const { return session.Options(); }
    // One short line for the game's HUD.
    std::string Status();
    const Json::Value &Players() const { return session.Players(); }
    Json::Value Sample() { return session.Sample(); }
    std::vector<double> TakeEditLatencies() { return session.TakeEditLatencies(); }

    // Edits. On refusal, `why` says so in words the player can act on.
    bool Send(const ToolStroke &stroke, std::string &why);
    bool SetPaused(bool paused, std::string &why);
    bool Step(std::string &why);
    bool Clear(std::string &why);
    // The room's simulation settings (admin), applied in order with edits.
    bool ChangeSettings(const SimulationSettingsChange &settings, std::string &why);
    // Admin: reset air, pressure, velocity, sparks, ambientHeat or temperature; invertAir.
    bool Reset(const std::string &what, std::string &why);
    // Admin: a console command ({"op": "console", ...}), validated before it is sent.
    bool Console(Json::Value command, std::string &why) { return SendAdmin(std::move(command), why); }
    // This player's stickman: its key state, or placing it (once per player).
    bool Stickman(int comm, int pcomm, std::string &why);
    bool SpawnStickman(int x, int y, std::string &why);
    int OwnStickman() const { return session.OwnStickman(); }
    // Signs: add (index -1), edit or remove one, as the sign window does; or press a button sign.
    bool Sign(int index, int expectX, int expectY, bool remove, const std::string &text, int x, int y, int justification, std::string &why);
    bool SignButton(int index, int x, int y, std::string &why);
    // A save or stamp (save file bytes) placed at cell (x, y), as the game places it.
    bool Paste(const std::vector<char> &save, int x, int y, bool includePressure, std::string &why);
    // Admin: a save file (bytes) replaces the room's world and settings, as opening it does in the game.
    bool Load(const std::vector<char> &save, std::string &why);
    // Every edit this player made has been applied to the shown world (or the
    // connection that carried it is gone).
    bool Settled() const { return outbox.empty() && session.OwnCommitted() >= session.Edits().Last(); }
    void Cursor(bool visible, int x, int y, int radius, bool drawing) { session.SendCursor(visible, x, y, radius, drawing); }

    // Commands accepted but not yet sent (paced to the server's editing budget).
    size_t Queued() const { return outbox.size(); }
    // The player's own tool commands not yet applied to the shown world, oldest
    // first: sent ones awaiting their commit, then queued ones. Render-only preview.
    template<class Visit> void ForEachPending(Visit visit) const
    {
        for (auto &edit : unapplied) visit(edit.command);
        for (auto &edit : outbox) visit(edit.command);
    }
    size_t PendingCount() const { return unapplied.size() + outbox.size(); }
    unsigned OwnPlayer() const { return session.OwnPlayer(); }

private:
    Session session;
    std::unique_ptr<Simulation> placeholder;
    Simulation *shown = nullptr;
    // Outgoing commands in order, released as the server's budget allows, so a huge
    // stroke arrives progressively instead of being refused or dropping the link.
    struct Outgoing { Json::Value command; double cost; };
    std::deque<Outgoing> outbox;
    struct Unapplied { Json::Value command; uint64_t seq; double sentAt; };
    std::deque<Unapplied> unapplied;
    double queuedWork = 0, workTokens, operationTokens, refilledAt;
    bool Enqueue(std::vector<Json::Value> commands, std::string &why, bool whole = false);
    void Flush();
    bool SendAdmin(Json::Value command, std::string &why);
    bool SendStickman(Json::Value command, std::string &why);
    // A save uploaded in chunks, then the paste or load command that uses it.
    bool Upload(const std::vector<char> &save, Json::Value command, std::string &why);
    size_t stickmanQueued = 0; // stickman commands waiting in the outbox
    uint64_t pasteIds = 0;
};
}
