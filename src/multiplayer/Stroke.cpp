// Game tool strokes as protocol commands. No network dependency, so the core
// library (and its tests) include it too.
#include "GameSession.h"
#include "Protocol.h"
#include "Tools.h"
#include <algorithm>
#include <cmath>

namespace multiplayer
{
bool RoomSupportsTool(const std::string &identifier)
{
    if (RoomOnlyTool(identifier)) return true;
    try { FindTool(identifier); return true; }
    catch (const std::exception &) { return false; }
}
std::vector<Json::Value> StrokeCommands(const ToolStroke &s)
{
    auto clampX = [](int x) { return std::clamp(x, CELL, XRES-CELL-1); };
    auto clampY = [](int y) { return std::clamp(y, CELL, YRES-CELL-1); };
    int rx = std::clamp(s.radiusX, 0, 200), ry = std::clamp(s.radiusY, 0, 200);
    auto command = [&](int x1, int y1, int x2, int y2) {
        Json::Value c;
        c["op"] = "tool"; c["tool"] = s.tool; c["action"] = s.action; c["brush"] = s.brush;
        c["x1"] = clampX(x1); c["y1"] = clampY(y1); c["x2"] = clampX(x2); c["y2"] = clampY(y2);
        c["radius"] = rx; c["radiusY"] = ry;
        c["strength"] = std::clamp(int(std::lround(s.strength*100)), 1, 1000);
        c["dragging"] = s.dragging; c["shift"] = s.shift; c["ctrl"] = s.ctrl; c["alt"] = s.alt;
        c["flags"] = std::clamp(s.replaceFlags, 0, 3); c["replace"] = std::clamp(s.replaceSelected, 0, PT_NUM-1);
        if (DecorationMode(s.tool) >= 0) c["colour"] = s.decoColour;
        else if (s.tool == "DEFAULT_UI_PROPERTY")
        {
            c["property"] = s.property;
            std::visit([&](auto value) { c["value"] = value; }, s.value);
        }
        return c;
    };
    std::vector<Json::Value> out;
    // A drag (line mode, while the mouse moves) and a click (on release) do nothing
    // for most tools; the game calls them anyway.
    if ((s.action == "drag" && !ToolActsOnDrag(s.tool)) || (s.action == "click" && !ToolActsOnClick(s.tool)))
        return out;
    // Wind pushes along the whole line from one stamp: splitting it would change it.
    if (s.action != "line" || s.tool == "DEFAULT_TOOL_WIND")
    {
        out.push_back(command(s.x1, s.y1, s.x2, s.y2));
        return out;
    }
    // A line stamps the brush at every point: split it so each part stays within
    // the server's per-command work limit. Parts share endpoints, as consecutive
    // mouse segments do in single-player.
    int perPart = std::max(1, int(ToolCommandCostLimit/StampCost(s.tool, rx, ry))-1);
    int span = std::max(std::abs(s.x2-s.x1), std::abs(s.y2-s.y1));
    int parts = std::max(1, (span+perPart-1)/perPart);
    for (int i = 0; i < parts; ++i)
        out.push_back(command(s.x1+(s.x2-s.x1)*i/parts, s.y1+(s.y2-s.y1)*i/parts,
                              s.x1+(s.x2-s.x1)*(i+1)/parts, s.y1+(s.y2-s.y1)*(i+1)/parts));
    return out;
}
}
