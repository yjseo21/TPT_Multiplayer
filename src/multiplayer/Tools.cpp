#include "Tools.h"
#include "simulation/AccessProperty.h"
#include <algorithm>
#include <cstdlib>
#include "simulation/SimulationData.h"
#include "simulation/ElementClasses.h"
#include "simulation/ToolClasses.h"
#include "gui/game/tool/ElementTool.h"
#include "gui/game/tool/WallTool.h"
#include "gui/game/EllipseBrush.h"
#include "gui/game/RectangleBrush.h"
#include "gui/game/TriangleBrush.h"

namespace multiplayer
{
std::unique_ptr<Tool> FindTool(const std::string &identifier)
{
    auto &sd = SimulationData::CRef();
    for (int i = 0; i < PT_NUM; ++i)
    {
        auto &e = sd.elements[i];
        if (!e.Enabled || e.Identifier != identifier) continue;
        if (i == PT_LIGH) return std::make_unique<Element_LIGH_Tool>(i, e.Identifier);
        if (i == PT_TESC) return std::make_unique<Element_TESC_Tool>(i, e.Identifier);
        if (i == PT_STKM || i == PT_STKM2 || i == PT_FIGH) return std::make_unique<PlopTool>(i, e.Identifier);
        return std::make_unique<ElementTool>(i, e.Identifier);
    }
    for (int i = 0; i < int(sd.wtypes.size()); ++i)
        if (sd.wtypes[i].identifier == identifier)
            return std::make_unique<WallTool>(i, "", 0xFFFFFF_rgb, sd.wtypes[i].identifier);
    for (const auto &tool : GetTools())
        if (tool.Identifier == identifier) return std::make_unique<SimTool>(tool);
    throw std::runtime_error("unknown tool identifier");
}
Json::Value ToolCatalog()
{
    Json::Value result(Json::arrayValue);
    auto append = [&](const auto &identifier, const auto &name) {
        Json::Value item; item["identifier"] = std::string(identifier); item["name"] = std::string(name); result.append(item);
    };
    for (auto &e : SimulationData::CRef().elements) if (e.Enabled) append(e.Identifier, e.Name.ToUtf8());
    for (auto &w : SimulationData::CRef().wtypes) append(w.identifier, w.name.ToUtf8());
    for (auto &t : GetTools()) append(t.Identifier, t.Name.ToUtf8());
    return result;
}
namespace
{
// What PropertyTool does with a configuration (it needs the game's interface).
void SetPropertyAt(Simulation &sim, const AccessProperty &change, ui::Point p)
{
    if (p.X < 0 || p.X >= XRES || p.Y < 0 || p.Y >= YRES) return;
    int i = sim.pmap[p.Y][p.X];
    if (!i) i = sim.photons[p.Y][p.X];
    if (i) change.Set(&sim, ID(i));
}
void SetPropertyBrush(Simulation &sim, const AccessProperty &change, const Brush &brush, ui::Point at)
{
    for (ui::Point off : brush) SetPropertyAt(sim, change, at + off);
}
void SetPropertyLine(Simulation &sim, const AccessProperty &change, const Brush &brush, ui::Point a, ui::Point b)
{
    int x1 = a.X, y1 = a.Y, x2 = b.X, y2 = b.Y, x, y, sy, rx = brush.GetRadius().X, ry = brush.GetRadius().Y;
    bool reverseXY = std::abs(y2-y1) > std::abs(x2-x1);
    float e = 0.0f, de;
    if (reverseXY) { std::swap(x1, y1); std::swap(x2, y2); }
    if (x1 > x2) { std::swap(x1, x2); std::swap(y1, y2); }
    int dx = x2 - x1, dy = std::abs(y2 - y1);
    de = dx ? dy/(float)dx : 0.0f;
    y = y1;
    sy = (y1<y2) ? 1 : -1;
    auto draw = [&](int px, int py) { SetPropertyBrush(sim, change, brush, reverseXY ? ui::Point(py, px) : ui::Point(px, py)); };
    for (x = x1; x <= x2; x++)
    {
        draw(x, y);
        e += de;
        if (e >= 0.5f)
        {
            y += sy;
            if (!(rx+ry) && ((y1<y2) ? (y<=y2) : (y>=y2))) draw(x, y);
            e -= 1.0f;
        }
    }
}
}
bool RoomOnlyTool(const std::string &identifier)
{
    return DecorationMode(identifier) >= 0 || identifier == "DEFAULT_UI_PROPERTY";
}
void ApplyTool(Simulation &sim, const Command &c)
{
    if (RoomOnlyTool(c.tool))
    {
        std::unique_ptr<Brush> brush;
        if (c.brush == "rectangle") brush = std::make_unique<RectangleBrush>();
        else if (c.brush == "triangle") brush = std::make_unique<TriangleBrush>();
        else brush = std::make_unique<EllipseBrush>(c.brush == "ellipse-perfect");
        brush->SetRadius({c.radius, c.radiusY});
        ui::Point a{c.x1, c.y1}, b{c.x2, c.y2};
        if (auto mode = DecorationMode(c.tool); mode >= 0)
        {
            // What DecorationTool does, with the colour the command carries.
            auto colour = RGBA::Unpack(c.decoColour);
            if (c.action == "point") sim.ApplyDecorationPoint(a.X, a.Y, colour.Red, colour.Green, colour.Blue, colour.Alpha, mode, *brush);
            else if (c.action == "line") sim.ApplyDecorationLine(a.X, a.Y, b.X, b.Y, colour.Red, colour.Green, colour.Blue, colour.Alpha, mode, *brush);
            else if (c.action == "box") sim.ApplyDecorationBox(a.X, a.Y, b.X, b.Y, colour.Red, colour.Green, colour.Blue, colour.Alpha, mode);
            return;
        }
        AccessProperty change{ c.property, c.value };
        if (c.action == "point") SetPropertyBrush(sim, change, *brush, a);
        else if (c.action == "line") SetPropertyLine(sim, change, *brush, a, b);
        else if (c.action == "box")
            for (int y = std::min(a.Y, b.Y); y <= std::max(a.Y, b.Y); y++)
                for (int x = std::min(a.X, b.X); x <= std::max(a.X, b.X); x++)
                    SetPropertyAt(sim, change, {x, y});
        else if (c.action == "fill") sim.flood_prop(a.X, a.Y, change);
        return;
    }
    auto tool = FindTool(c.tool);
    std::unique_ptr<Brush> brush;
    if (c.brush == "rectangle") brush = std::make_unique<RectangleBrush>();
    else if (c.brush == "triangle") brush = std::make_unique<TriangleBrush>();
    else brush = std::make_unique<EllipseBrush>(c.brush == "ellipse-perfect");
    brush->SetRadius({c.radius, c.radiusY});
    tool->Strength = c.strength/100.0f; tool->shiftBehaviour = c.shift; tool->ctrlBehaviour = c.ctrl; tool->altBehaviour = c.alt;
    struct Restore
    {
        Simulation &sim; int flags, selected;
        ~Restore() { sim.replaceModeFlags = flags; sim.replaceModeSelected = selected; }
    } restore{sim, sim.replaceModeFlags, sim.replaceModeSelected};
    sim.replaceModeFlags = c.flags; sim.replaceModeSelected = c.replace;
    ui::Point a{c.x1, c.y1}, b{c.x2, c.y2};
    if (c.action == "point") tool->Draw(&sim, *brush, a);
    else if (c.action == "end") { tool->DrawLine(&sim, *brush, a, b, true); tool->Click(&sim, *brush, b); }
    else if (c.action == "line") tool->DrawLine(&sim, *brush, a, b, c.dragging);
    else if (c.action == "box") tool->DrawRect(&sim, *brush, a, b);
    else if (c.action == "fill") tool->DrawFill(&sim, *brush, a);
    else if (c.action == "click") tool->Click(&sim, *brush, a);
    else if (c.action == "drag") tool->Drag(&sim, *brush, a, b);
}
}
