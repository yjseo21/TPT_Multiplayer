#pragma once
#include "Tool.h"
#include "simulation/Sign.h"
#include <functional>
#include <optional>

class GameModel;
namespace ui
{
	class Window;
}

class SignTool: public Tool
{
	GameModel &gameModel;

	friend class SignWindow;

public:
	SignTool(GameModel &model):
		Tool(0, "SIGN", "Sign. Displays text. Click on a sign to edit it or anywhere else to place a new one.",
			0x000000_rgb, "DEFAULT_UI_SIGN", SignTool::GetIcon
		),
		gameModel(model)
	{}

	// Multiplayer: set while a room's world is shown. The sign window then never
	// changes the world itself: it hands the change here (index -1: a new sign;
	// original: the sign as the window found it; result: none to delete it).
	std::function<void (int index, std::optional<sign> original, std::optional<sign> result)> roomChange;
	// The open sign window, if any. It shows the world it was opened on, so when
	// a room replaces that world (resync, disconnect) the game closes it.
	ui::Window *window = nullptr;
	void CloseWindow();

	static std::unique_ptr<VideoBuffer> GetIcon(int toolID, Vec2<int> size);
	void Click(Simulation * sim, Brush const &brush, ui::Point position) override;
	void Draw(Simulation * sim, Brush const &brush, ui::Point position) override { }
	void DrawLine(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2, bool dragging) override { }
	void DrawRect(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2) override { }
	void DrawFill(Simulation * sim, Brush const &brush, ui::Point position) override { }
};
