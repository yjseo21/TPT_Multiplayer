#pragma once
#include "lua/CommandInterfacePtr.h"
#include "client/ClientListener.h"
#include "client/StartupInfo.h"
#include "common/ExplicitSingleton.h"
#include "gui/interface/Point.h"
#include "gui/interface/Colour.h"
#include "gui/SavePreviewType.h"
#include "simulation/Sign.h"
#include "simulation/Particle.h"
#include "simulation/SimulationSettings.h"
#include "Misc.h"
#include <vector>
#include <utility>
#include <memory>
#include <optional>

constexpr auto DEBUG_PARTS      = 0x0001;
constexpr auto DEBUG_ELEMENTPOP = 0x0002;
constexpr auto DEBUG_LINES      = 0x0004;
constexpr auto DEBUG_PARTICLE   = 0x0008;
constexpr auto DEBUG_SURFNORM   = 0x0010;
constexpr auto DEBUG_SIMHUD     = 0x0020;
constexpr auto DEBUG_RENHUD     = 0x0040;
constexpr auto DEBUG_AIRVEL     = 0x0080;
constexpr auto DEBUG_FRAMETIME  = 0x0100;

class FrameTime;
class DebugInfo;
class SaveFile;
class Notification;
class GameModel;
class GameView;
class Snapshot;
class OptionsController;
class LocalBrowserController;
class SearchController;
class PreviewController;
class RenderController;
class VideoBuffer;
class Tool;
class Menu;
class SaveInfo;
class GameSave;
class LoginController;
class TagsController;
class ConsoleController;
namespace multiplayer { class GameSession; }
namespace Json
{
	class Value;
}
class GameController : public ClientListener, public ExplicitSingleton<GameController>
{
	CommandInterfacePtr commandInterface;

private:
	bool firstTick;
	int foundSignID;

	PreviewController * activePreview;
	GameView * gameView;
	GameModel * gameModel;
	SearchController * search;
	RenderController * renderOptions;
	LoginController * loginWindow;
	ConsoleController * console;
	TagsController * tagsWindow;
	LocalBrowserController * localBrowser;
	OptionsController * options;
	std::vector<std::unique_ptr<DebugInfo>> debugInfo;
	std::unique_ptr<Snapshot> beforeRestore;
	unsigned int debugFlags;
	
	void OpenSaveDone();
	void CreateDebugInfo();

	// Multiplayer: a room on a dedicated server, shown instead of the player's own
	// world while showRoom is set. The own world keeps its state underneath.
	std::unique_ptr<multiplayer::GameSession> room;
	bool showRoom = false;
	// While the room is shown: true (after telling the player) if this action
	// cannot be done in a room. Never true when the own world is shown.
	bool RoomRefuses(String what);
	// Sends a tool use to the room instead of applying it locally. False when the
	// player's own world is shown and the caller should apply it as usual.
	bool RoomTool(int toolSelection, const char *action, ui::Point a, ui::Point b, bool dragging, bool useToolStrength);
	void UpdateRoom();
	void UpdateRoomOverlay();
	void UpdateRoomSigns(); // the sign tool sends its changes to the room while it is shown
	void CloseRoomSignWindow();
	// While the room is shown: arrow keys steer this player's stickman.
	bool RoomStickmanKey(int key, bool pressed);
	void RoomStickmanStop();
	int roomStickmanComm = 0, roomStickmanPcomm = 0;
	void EndRoom(String why);
	// Credentials of the latest join, offered again by the dialog (never saved).
	ByteString lastRoomToken, lastAdminToken;
	double roomSettingsAt = 0; // when settings were last sent to the room
	size_t roomGhostsPeak = 0; // largest preview since the last automation log row
	struct RoomAutomation;
	std::unique_ptr<RoomAutomation> roomAutomation;
	void RunRoomAutomation();
	// Throttled player notices (a refused drag would otherwise log every frame).
	void RoomNotice(String message);
	String lastRoomNotice;
	double lastRoomNoticeAt = -10;
public:
	enum MouseupReason
	{
		mouseUpNormal,
		mouseUpBlur,
		mouseUpDrawEnd,
	};

	bool HasDone;
	GameController();
	~GameController();
	GameView * GetView();
	int GetSignAt(int x, int y);
	String GetSignText(int signID);
	std::pair<int, sign::Type> GetSignSplit(int signID);

	bool MouseMove(int x, int y, int dx, int dy);
	bool MouseDown(int x, int y, unsigned button);
	bool MouseUp(int x, int y, unsigned button, MouseupReason reason);
	bool MouseWheel(int x, int y, int d);
	bool TextInput(String text);
	bool TextEditing(String text);
	bool KeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt);
	bool KeyRelease(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt);
	void Tick();
	void Blur();
	void Exit();

	void Install();

	bool HistoryRestore();
	void HistorySnapshot();
	bool HistoryForward();

	void AdjustGridSize(int direction);
	void InvertAirSim();
	void LoadRenderPreset(int presetNum);
	void SetZoomEnabled(bool zoomEnable);
	void SetZoomPosition(ui::Point position);
	void AdjustBrushSize(int direction, bool logarithmic = false, bool xAxis = false, bool yAxis = false);
	void SetBrushSize(ui::Point newSize);
	void AdjustZoomSize(int direction, bool logarithmic = false);
	void ToolClick(int toolSelection, ui::Point point);
	void ToolDrag(int toolSelection, ui::Point point1, ui::Point point2);
	void DrawPoints(int toolSelection, ui::Point oldPos, ui::Point newPos, bool held);
	void DrawRect(int toolSelection, ui::Point point1, ui::Point point2);
	void DrawLine(int toolSelection, ui::Point point1, ui::Point point2);
	void DrawFill(int toolSelection, ui::Point point);
	ByteString StampRegion(ui::Point point1, ui::Point point2);
	ByteString StampRegion(ui::Point point1, ui::Point point2, bool includePressure);
	void CopyRegion(ui::Point point1, ui::Point point2);
	void CutRegion(ui::Point point1, ui::Point point2);
	void Update();
	bool GetPaused() const;
	void SetPaused(bool pauseState);
	void SetPaused();
	void SetDecoration(bool decorationState);
	void SetDecoration();
	void ShowGravityGrid();
	void SetHudEnable(bool hudState);
	bool GetHudEnable();
	void SetBrushEnable(bool brushState);
	bool GetBrushEnable();
	void SetDebugHUD(bool hudState);
	bool GetDebugHUD();
	void SetTemperatureScale(TempScale temperatureScale);
	TempScale GetTemperatureScale();
	int GetEdgeMode();
	void SetEdgeMode(int edgeMode);
	void SetDebugFlags(unsigned int flags) { debugFlags = flags; }
	unsigned int GetDebugFlags() const { return debugFlags; }
	void SetActiveMenu(int menuID);
	std::vector<Menu*> GetMenuList();
	int GetNumMenus(bool onlyEnabled);
	void RebuildFavoritesMenu();
	Tool * GetActiveTool(int selection);
	void SetActiveTool(int toolSelection, Tool * tool);
	void SetActiveTool(int toolSelection, ByteString identifier);
	void SetLastTool(Tool * tool);
	Tool *GetLastTool();
	int GetReplaceModeFlags();
	void SetReplaceModeFlags(int flags);
	void SetActiveColourPreset(int preset);
	void SetColour(ui::Colour colour);
	void SetToolStrength(float value);
	void LoadSaveFile(std::unique_ptr<SaveFile> file);
	void LoadSave(std::unique_ptr<SaveInfo> save);
	void OpenSearch(String searchText);
	void OpenLogin();
	void OpenProfile();
	void OpenTags();
	void OpenSavePreview(int saveID, int saveDate, SavePreviewType savePreiviewType);
	void OpenSavePreview();
	void OpenLocalSaveWindow(bool asCurrent);
	void OpenLocalBrowse();
	void OpenOptions();
	void OpenRenderOptions();
	void OpenSaveWindow();
	void SaveAsCurrent();
	void OpenStamps();
	void OpenElementSearch();
	void OpenColourPicker();
	void PlaceSave(ui::Point position);
	void ClearSim();
	void ReloadSim();
	void Vote(int direction);
	void ChangeBrush();
	void ShowConsole();
	void HideConsole();
	void FrameStep();
	void TransformPlaceSave(Mat2<int> transform, Vec2<int> nudge);
	bool MouseInZoom(ui::Point position);
	ui::Point PointTranslate(ui::Point point);
	ui::Point NormaliseBlockCoord(ui::Point point);
	String ElementResolve(int type, int ctype);
	String BasicParticleInfo(Particle const &sample_part);
	bool IsValidElement(int type);
	String WallName(int type);
	ByteString TakeScreenshot(int captureUI, int fileType);
	int Record(bool record);

	void ResetAir();
	void ResetSpark();
	void SwitchGravity();
	void SwitchAir();
	void ToggleAHeat();
	bool GetAHeatEnable();
	void ResetAHeat();
	void ToggleNewtonianGravity();

	bool LoadClipboard();
	void LoadStamp(std::unique_ptr<GameSave> stamp);

	void RemoveNotification(Notification * notification);

	void NotifyUpdateAvailable(Client * sender) override;
	void NotifyAuthUserChanged(Client * sender) override;
	void NotifyNewNotification(Client * sender, ServerNotification notification) override;
	void RunUpdater(UpdateInfo info);
	bool GetMouseClickRequired();
	bool GetThreadedRendering();

	void RemoveCustomGol(const ByteString &identifier);

	void BeforeSimDraw();
	void AfterSimDraw();
	bool ThreadedRenderingAllowed();

	void SetToolIndex(ByteString identifier, std::optional<int> index);
	void InitCommandInterface();

	// Multiplayer. Credentials not given come from TPT_ROOM_TOKEN / TPT_ADMIN_TOKEN.
	// Joining while in a room leaves it first.
	void JoinRoom(ByteString url, ByteString name, std::optional<ByteString> roomToken = std::nullopt,
	              std::optional<ByteString> adminToken = std::nullopt);
	void LeaveRoom();
	// While the room is shown: the room's own reset (admin), true if handled there.
	// air, pressure, velocity, sparks, ambientHeat, temperature or invertAir.
	bool RoomReset(ByteString what);
	// While the room is shown: a console command resolved into a room command (admin).
	bool RoomConsole(const Json::Value &command); // false (and a notice) when the room refused it
	// The in-game dialog (Ctrl+Shift+J): join a room, or leave the current one.
	void OpenRoomDialog();
	// Switch between the room's world and the player's own (the room stays connected).
	void ToggleRoom();
	bool ShowingRoom() const { return showRoom; }

	FrameTime *GetFrameTime() const;
};
