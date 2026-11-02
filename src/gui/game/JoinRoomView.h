#pragma once
#include "common/String.h"
#include "gui/interface/Window.h"
#include <functional>
#include <vector>

namespace ui
{
	class Textbox;
}

// The in-game dialog for joining a multiplayer room, or leaving the current one.
class JoinRoomView : public ui::Window
{
public:
	struct Request
	{
		ByteString url, name, roomToken, adminToken;
	};

	// current: the room the player is in, or empty. leave is only offered then.
	JoinRoomView(Request initial, String current, std::function<void (Request)> join, std::function<void ()> leave);
	~JoinRoomView() override;
	static JoinRoomView *open; // the open dialog, if any

	void OnDraw() override;
	void OnKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
	void OnTryExit(ExitMethod method) override;

private:
	std::vector<ui::Textbox *> fields;
};
