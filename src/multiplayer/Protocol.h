#pragma once
#include "simulation/Simulation.h"
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
// Bump with any change to the engine, serializer, or enabled elements.
constexpr const char *BuildId = "tpt-c205becb-mp3-ingame1";

struct Frame
{
    uint64_t tick = 0, revision = 0;
    bool paused = false;
    std::unique_ptr<RenderableSimulation> world;
};

std::string EncodeFrame(const RenderableSimulation &sim, uint64_t tick, uint64_t revision, bool paused);
Frame DecodeFrame(std::string_view packet);
Json::Value ParseJson(std::string_view text);
std::string JsonText(const Json::Value &value);
uint64_t Sequence(const Json::Value &value);
int Integer(const Json::Value &value, const char *field, int minimum, int maximum);

struct Command
{
    enum Kind { Brush, Pause, Step, Clear, ToolAction } kind;
    uint64_t sequence;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0, radius = 0, element = 0;
    int radiusY = 0; // tool brushes: vertical radius (radius is horizontal)
    bool paused = false;
    std::string tool, action = "line", brush = "ellipse";
    int flags = 0, replace = 0;
    bool dragging = true, shift = false, ctrl = false, alt = false;
    int strength = 100;
    unsigned Cost() const;
};
Command ParseCommand(const Json::Value &json, bool admin);
// Brush-cell operations: one tool command, and a player's sustained rate (and burst).
constexpr unsigned ToolCommandCostLimit = 1000000;
constexpr double EditWorkPerSecond = 3000000;
}
