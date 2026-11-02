#pragma once
#include "Protocol.h"
#include "gui/game/tool/Tool.h"

namespace multiplayer
{
std::unique_ptr<Tool> FindTool(const std::string &identifier);
Json::Value ToolCatalog();
void ApplyTool(Simulation &sim, const Command &command);
// Decoration and property tools: replayed in rooms, not in FindTool (they need
// the colour or property the command carries).
bool RoomOnlyTool(const std::string &identifier);
}
