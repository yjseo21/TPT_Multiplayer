#pragma once
#include "simulation/Simulation.h"
#include "simulation/SimulationSettings.h"
#include "simulation/StructProperty.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <json/json.h>

namespace multiplayer
{
constexpr unsigned ProtocolVersion = 3;
constexpr size_t MaxFrameBytes = 32 * 1024 * 1024;
constexpr size_t MaxCommandBytes = 4096;
// A pasted save or stamp travels inside its command: at most this many bytes of
// save file (base64 in JSON), uploaded in chunks, then committed to everyone.
constexpr size_t MaxPasteBytes = 224 * 1024; // base64 of this, in JSON, fits MaxOrderedBytes
constexpr size_t MaxPasteChunkChars = 3000;
// A pasted save may unpack to at most this much (a full-canvas save is a few MB;
// the game itself accepts up to 200 MB, far too much for a server to unpack on request).
constexpr size_t MaxPasteUnpackedBytes = 16U * 1024 * 1024;
// Ordered server messages (commits) may carry a paste.
constexpr size_t MaxOrderedBytes = 320 * 1024;
// Bump with any change to the engine, serializer, or enabled elements.
constexpr const char *BuildId = "tpt-c205becb-mp3-ingame8";

struct Frame
{
    uint64_t tick = 0, revision = 0;
    bool paused = false;
    std::unique_ptr<RenderableSimulation> world;
};

std::string EncodeFrame(const RenderableSimulation &sim, uint64_t tick, uint64_t revision, bool paused);
Frame DecodeFrame(std::string_view packet);
Json::Value ParseJson(std::string_view text, size_t maxBytes = MaxCommandBytes);
std::string Base64Encode(std::string_view bytes);
std::string Base64Decode(std::string_view text); // throws on malformed input
std::string JsonText(const Json::Value &value);
uint64_t Sequence(const Json::Value &value);
int Integer(const Json::Value &value, const char *field, int minimum, int maximum);

struct Command
{
    enum Kind { Brush, Pause, Step, Clear, ToolAction, Settings, Stickman, StickmanSpawn, Reset, Console, Sign, SignButton, Paste } kind;
    uint64_t sequence;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0, radius = 0, element = 0;
    int radiusY = 0; // tool brushes: vertical radius (radius is horizontal)
    bool paused = false;
    std::string tool, action = "line", brush = "ellipse";
    int flags = 0, replace = 0;
    bool dragging = true, shift = false, ctrl = false, alt = false;
    int strength = 100;
    SimulationSettingsChange settings; // Settings: the room's simulation settings (admin)
    // Stickman / StickmanSpawn: the player's stickman slot (1-4, set by the server
    // from the sender), its key state (comm, pcomm) or where to place it (x1, y1).
    int slot = 0, comm = 0, pcomm = 0;
    std::string what; // Reset (admin): air, pressure, velocity, sparks, ambientHeat, temperature, invertAir
    // Console (admin): the game console's set / create / delete / bubble, resolved
    // on the sender (names, temperature units) into exact values.
    // set: property, value, selector all / index / point (x1, y1) / type (element).
    // create: element (with ID bits), x1, y1. delete: index, or point. bubble: x1, y1.
    std::string cmd, selector;
    int property = 0, index = -1;
    PropertyValue value = 0;
    // ToolAction: decoration tools carry their colour (ARGB); the property tool
    // carries property and value, as console set does.
    uint32_t decoColour = 0;
    // Sign: index -1 adds one, otherwise it replaces or (remove) deletes sign
    // index, if that sign is still at (x2, y2). Text, position (x1, y1), justification.
    // SignButton: presses button sign index, if it is still at (x1, y1).
    std::string text;
    int justification = 1;
    bool remove = false;
    // Paste: a save file (text, base64), placed at cell (x1, y1) as the game places it.
    bool includePressure = true;
    unsigned Cost() const;
};
Command ParseCommand(const Json::Value &json, bool admin);
// Decoration tool mode (DECO_*) for a tool identifier, or -1.
int DecorationMode(const std::string &identifier);
// The "settings" member of a settings command: only the fields that are set.
Json::Value SettingsJson(const SimulationSettingsChange &settings);
// Parses a paste command's save fully (throws if it is not a valid save). The
// server calls this on the sender's connection, so a broken save never reaches
// the room, and the room's own thread never spends time on a bad one.
void CheckPastedSave(const Command &paste);
// Brush-cell operations: one tool command, and a player's sustained rate (and burst).
constexpr unsigned ToolCommandCostLimit = 1000000;
constexpr double EditWorkPerSecond = 3000000;
}
