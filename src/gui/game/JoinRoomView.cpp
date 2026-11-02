#include "JoinRoomView.h"
#include "graphics/Graphics.h"
#include "gui/interface/Button.h"
#include "gui/interface/Label.h"
#include "gui/interface/Textbox.h"
#include "gui/Style.h"
#include <SDL.h>

JoinRoomView *JoinRoomView::open = nullptr;

JoinRoomView::JoinRoomView(Request initial, String current, std::function<void (Request)> join, std::function<void ()> leave):
	ui::Window(ui::Point(-1, -1), ui::Point(260, 0))
{
	open = this;
	auto *titleLabel = new ui::Label(ui::Point(4, 5), ui::Point(Size.X-8, 16), "Multiplayer room");
	titleLabel->SetTextColour(style::Colour::InformationTitle);
	titleLabel->Appearance.HorizontalAlign = ui::Appearance::AlignLeft;
	titleLabel->Appearance.VerticalAlign = ui::Appearance::AlignMiddle;
	AddComponent(titleLabel);

	int y = 23;
	if (current.size())
	{
		auto *currentLabel = new ui::Label(ui::Point(8, y), ui::Point(Size.X-16, 14), "In room " + current);
		currentLabel->Appearance.HorizontalAlign = ui::Appearance::AlignLeft;
		currentLabel->Appearance.VerticalAlign = ui::Appearance::AlignMiddle;
		AddComponent(currentLabel);
		y += 16;
	}

	auto field = [this, &y](String label, String text, String placeholder, bool hidden) {
		auto *caption = new ui::Label(ui::Point(8, y), ui::Point(56, 17), label);
		caption->Appearance.HorizontalAlign = ui::Appearance::AlignLeft;
		caption->Appearance.VerticalAlign = ui::Appearance::AlignMiddle;
		AddComponent(caption);
		auto *textbox = new ui::Textbox(ui::Point(64, y), ui::Point(Size.X-72, 17), "", placeholder);
		textbox->Appearance.HorizontalAlign = ui::Appearance::AlignLeft;
		textbox->Appearance.VerticalAlign = ui::Appearance::AlignMiddle;
		textbox->SetHidden(hidden); // before the text, which is masked as it is set
		textbox->SetText(text);
		AddComponent(textbox);
		fields.push_back(textbox);
		y += 21;
		return textbox;
	};
	auto *urlField = field("Address", initial.url.FromUtf8(), "[wss://host:port]", false);
	auto *nameField = field("Name", initial.name.FromUtf8(), "[your name]", false);
	auto *tokenField = field("Password", initial.roomToken.FromUtf8(), "[if the room has one]", true);
	auto *adminField = field("Admin key", initial.adminToken.FromUtf8(), "[if you host the room]", true);
	nameField->SetLimit(32);

	auto *hint = new ui::Label(ui::Point(8, y), ui::Point(Size.X-16, 14), "Ctrl+Shift+M: switch room / own canvas");
	hint->SetTextColour(ui::Colour(160, 160, 160));
	hint->Appearance.HorizontalAlign = ui::Appearance::AlignLeft;
	hint->Appearance.VerticalAlign = ui::Appearance::AlignMiddle;
	AddComponent(hint);
	y += 18;
	Size.Y = y + 16;

	auto button = [this](int x, int width, String text) {
		auto *b = new ui::Button(ui::Point(x, Size.Y-16), ui::Point(width, 16), text);
		b->Appearance.VerticalAlign = ui::Appearance::AlignMiddle;
		AddComponent(b);
		return b;
	};
	int columns = current.size() ? 3 : 2, width = Size.X / columns;
	auto *cancelButton = button(0, width+1, "Cancel");
	cancelButton->SetActionCallback({ [this] {
		CloseActiveWindow();
		SelfDestruct();
	} });
	SetCancelButton(cancelButton);
	if (current.size())
	{
		auto *leaveButton = button(width, width+1, "Leave room");
		leaveButton->SetActionCallback({ [this, leave] {
			CloseActiveWindow();
			leave();
			SelfDestruct();
		} });
	}
	auto *joinButton = button(width*(columns-1), Size.X-width*(columns-1), current.size() ? String("Join instead") : String("Join"));
	joinButton->Appearance.TextInactive = style::Colour::ConfirmButton;
	joinButton->SetActionCallback({ [this, join, urlField, nameField, tokenField, adminField] {
		Request request{ urlField->GetText().ToUtf8(), nameField->GetText().ToUtf8(),
		                 tokenField->GetText().ToUtf8(), adminField->GetText().ToUtf8() };
		CloseActiveWindow();
		join(request);
		SelfDestruct();
	} });
	SetOkayButton(joinButton);

	FocusComponent(urlField->GetText().size() ? nameField : urlField);
	MakeActiveWindow();
}

JoinRoomView::~JoinRoomView()
{
	if (open == this)
		open = nullptr;
}

void JoinRoomView::OnKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt)
{
	if (repeat || key != SDLK_TAB)
		return;
	for (size_t i = 0; i < fields.size(); ++i)
		if (IsFocused(fields[i]))
		{
			FocusComponent(fields[(i + (shift ? fields.size()-1 : 1)) % fields.size()]);
			return;
		}
	FocusComponent(fields[0]);
}

void JoinRoomView::OnTryExit(ExitMethod method)
{
	CloseActiveWindow();
	SelfDestruct();
}

void JoinRoomView::OnDraw()
{
	Graphics *g = GetGraphics();
	g->DrawFilledRect(RectSized(Position - Vec2{ 1, 1 }, Size + Vec2{ 2, 2 }), 0x000000_rgb);
	g->DrawRect(RectSized(Position, Size), 0xC8C8C8_rgb);
}
