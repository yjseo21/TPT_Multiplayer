#include "CommandRecord.h"
#include <stdexcept>

namespace multiplayer
{
Json::Value RecordCommand(const Command &c)
{
    Json::Value v(Json::objectValue);
    v["seq"] = Json::UInt64(c.sequence);
    switch (c.kind)
    {
        case Command::Brush:
            v["op"] = "brush";
            v["x1"] = c.x1; v["y1"] = c.y1; v["x2"] = c.x2; v["y2"] = c.y2;
            v["radius"] = c.radius; v["element"] = c.element;
            break;
        case Command::ToolAction:
            v["op"] = "tool";
            v["tool"] = c.tool; v["action"] = c.action; v["brush"] = c.brush;
            v["x1"] = c.x1; v["y1"] = c.y1; v["x2"] = c.x2; v["y2"] = c.y2;
            v["radius"] = c.radius; v["flags"] = c.flags; v["replace"] = c.replace;
            // Only when different, so journals of round brushes are unchanged.
            if (c.radiusY != c.radius) v["radiusY"] = c.radiusY;
            v["strength"] = c.strength;
            v["dragging"] = c.dragging; v["shift"] = c.shift;
            v["ctrl"] = c.ctrl; v["alt"] = c.alt;
            break;
        case Command::Pause: v["op"] = "pause"; v["paused"] = c.paused; break;
        case Command::Step: v["op"] = "step"; break;
        case Command::Clear: v["op"] = "clear"; break;
        case Command::Settings: v["op"] = "settings"; v["settings"] = SettingsJson(c.settings); break;
        default: throw std::runtime_error("unrecognized command kind");
    }
    return v;
}
Command ReadRecordedCommand(const Json::Value &v)
{
    // Reuse the exact same validation path as live clients. Administrative
    // commands are allowed because journal input is locally trusted but still
    // strictly validated before replay.
    return ParseCommand(v, true);
}
}
