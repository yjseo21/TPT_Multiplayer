#pragma once
#include "Protocol.h"
#include "gui/game/tool/Tool.h"

namespace multiplayer
{
std::unique_ptr<Tool> FindTool(const std::string &identifier);
Json::Value ToolCatalog();
void ApplyTool(Simulation &sim, const Command &command);
}
