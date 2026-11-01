#include "Tools.h"
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
void ApplyTool(Simulation &sim, const Command &c)
{
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
