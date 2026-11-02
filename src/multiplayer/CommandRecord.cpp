#include "CommandRecord.h"
#include <stdexcept>
#include <variant>

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
            if (DecorationMode(c.tool) >= 0) v["colour"] = c.decoColour;
            else if (c.tool == "DEFAULT_UI_PROPERTY")
            {
                v["property"] = c.property;
                std::visit([&](auto value) { v["value"] = value; }, c.value);
            }
            break;
        case Command::Pause: v["op"] = "pause"; v["paused"] = c.paused; break;
        case Command::Step: v["op"] = "step"; break;
        case Command::Clear: v["op"] = "clear"; break;
        case Command::Settings: v["op"] = "settings"; v["settings"] = SettingsJson(c.settings); break;
        case Command::Stickman: v["op"] = "stickman"; v["slot"] = c.slot; v["comm"] = c.comm; v["pcomm"] = c.pcomm; break;
        case Command::StickmanSpawn: v["op"] = "stickman_spawn"; v["slot"] = c.slot; v["x1"] = c.x1; v["y1"] = c.y1; break;
        case Command::Reset: v["op"] = "reset"; v["what"] = c.what; break;
        case Command::Sign:
            v["op"] = "sign"; v["index"] = c.index;
            if (c.index >= 0) { v["x2"] = c.x2; v["y2"] = c.y2; }
            if (c.remove) v["remove"] = true;
            else { v["text"] = c.text; v["x1"] = c.x1; v["y1"] = c.y1; v["justification"] = c.justification; }
            break;
        case Command::Paste:
            v["op"] = "paste"; v["save"] = c.text; v["x1"] = c.x1; v["y1"] = c.y1;
            if (!c.includePressure) v["pressure"] = false;
            break;
        case Command::SignButton: v["op"] = "sign_button"; v["index"] = c.index; v["x1"] = c.x1; v["y1"] = c.y1; break;
        case Command::Console:
            v["op"] = "console"; v["cmd"] = c.cmd;
            if (c.cmd == "set")
            {
                v["property"] = c.property; v["selector"] = c.selector;
                std::visit([&](auto value) { v["value"] = value; }, c.value);
                if (c.selector == "index") v["index"] = c.index;
                else if (c.selector == "point") { v["x1"] = c.x1; v["y1"] = c.y1; }
                else if (c.selector == "type") v["element"] = c.element;
            }
            else if (c.cmd == "create") { v["element"] = c.element; v["x1"] = c.x1; v["y1"] = c.y1; }
            else if (c.cmd == "delete" && c.index >= 0) v["index"] = c.index;
            else { v["x1"] = c.x1; v["y1"] = c.y1; }
            break;
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
