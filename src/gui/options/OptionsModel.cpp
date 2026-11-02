#include "OptionsModel.h"
#include "OptionsView.h"
#include "simulation/Simulation.h"
#include "simulation/Air.h"
#include "simulation/gravity/Gravity.h"
#include "prefs/GlobalPrefs.h"
#include "common/clipboard/Clipboard.h"
#include "gui/interface/Engine.h"
#include "gui/game/GameModel.h"
#include "gui/game/GameView.h"
#include "client/Client.h"

OptionsModel::OptionsModel(GameModel * gModel_) {
	gModel = gModel_;
	sim = gModel->GetLocalSimulation(); // the player's own world; a room's goes through ToRoom
}

void OptionsModel::AddObserver(OptionsView* view)
{
	observers.push_back(view);
	view->NotifySettingsChanged(this);
}

// Simulation settings are read from the shown world. In a multiplayer room they
// are the room's: a change goes to the room instead, and reads back as requested
// until the room has applied it.
Simulation *OptionsModel::Shown()
{
	return gModel->GetSimulation();
}

const SimulationSettingsChange &OptionsModel::Requested()
{
	return gModel->RequestedRoomSettings();
}

bool OptionsModel::ToRoom(const SimulationSettingsChange &change)
{
	if (!gModel->ShowingRoom())
		return false;
	gModel->ChangeRoomSettings(change);
	notifySettingsChanged();
	return true;
}

bool OptionsModel::GetHeatSimulation()
{
	return Requested().heat.value_or(!Shown()->legacy_enable);
}

void OptionsModel::SetHeatSimulation(bool state)
{
	if (ToRoom({ .heat = state }))
		return;
	sim->legacy_enable = state?0:1;
	notifySettingsChanged();
}

bool OptionsModel::GetAmbientHeatSimulation()
{
	return Requested().ambientHeat.value_or(Shown()->aheat_enable);
}

void OptionsModel::SetAmbientHeatSimulation(bool state)
{
	if (ToRoom({ .ambientHeat = state }))
		return;
	sim->aheat_enable = state?1:0;
	notifySettingsChanged();
}

bool OptionsModel::GetNewtonianGravity()
{
	return bool(Shown()->grav);
}

void OptionsModel::SetNewtonianGravity(bool state)
{
	if (ToRoom({ .newtonianGravity = state }))
		return;
	sim->EnableNewtonianGravity(state);
	notifySettingsChanged();
}

bool OptionsModel::GetWaterEqualisation()
{
	return Requested().waterEqualisation.value_or(Shown()->water_equal_test);
}

void OptionsModel::SetWaterEqualisation(bool state)
{
	if (ToRoom({ .waterEqualisation = state }))
		return;
	sim->water_equal_test = state?1:0;
	notifySettingsChanged();
}

int OptionsModel::GetAirMode()
{
	return Requested().airMode.value_or(Shown()->air->airMode);
}
void OptionsModel::SetAirMode(int airMode)
{
	if (ToRoom({ .airMode = airMode }))
		return;
	sim->air->airMode = airMode;
	notifySettingsChanged();
}

int OptionsModel::GetEdgeMode()
{
	return Requested().edgeMode.value_or(Shown()->edgeMode);
}
void OptionsModel::SetEdgeMode(int edgeMode)
{
	if (ToRoom({ .edgeMode = edgeMode }))
		return;
	GlobalPrefs::Ref().Set("Simulation.EdgeMode", edgeMode);
	gModel->SetEdgeMode(edgeMode);
	notifySettingsChanged();
}

TempScale OptionsModel::GetTemperatureScale()
{
	return gModel->GetTemperatureScale();
}
void OptionsModel::SetTemperatureScale(TempScale temperatureScale)
{
	GlobalPrefs::Ref().Set("Renderer.TemperatureScale", int(temperatureScale));
	gModel->SetTemperatureScale(temperatureScale);
	notifySettingsChanged();
}

int OptionsModel::GetThreadedRendering()
{
	return gModel->GetThreadedRendering();
}

void OptionsModel::SetThreadedRendering(bool newThreadedRendering)
{
	GlobalPrefs::Ref().Set("Renderer.SeparateThread", newThreadedRendering);
	gModel->SetThreadedRendering(newThreadedRendering);
	notifySettingsChanged();
}

float OptionsModel::GetAmbientAirTemperature()
{
	return Requested().ambientAirTemp.value_or(Shown()->air->ambientAirTemp);
}
void OptionsModel::SetAmbientAirTemperature(float ambientAirTemp)
{
	if (ToRoom({ .ambientAirTemp = ambientAirTemp }))
		return;
	GlobalPrefs::Ref().Set("Simulation.AmbientAirTemp", ambientAirTemp);
	gModel->SetAmbientAirTemperature(ambientAirTemp);
	notifySettingsChanged();
}

float OptionsModel::GetEdgePressure()
{
	return Requested().edgePressure.value_or(Shown()->air->edgePressure);
}
void OptionsModel::SetEdgePressure(float edgePressure)
{
	if (ToRoom({ .edgePressure = edgePressure }))
		return;
	GlobalPrefs::Ref().Set("Simulation.EdgePressure", edgePressure);
	gModel->SetEdgePressure(edgePressure);
	notifySettingsChanged();
}

float OptionsModel::GetEdgeVelocityX()
{
	return Requested().edgeVelocityX.value_or(Shown()->air->edgeVelocityX);
}
void OptionsModel::SetEdgeVelocityX(float edgeVelocityX)
{
	if (ToRoom({ .edgeVelocityX = edgeVelocityX }))
		return;
	GlobalPrefs::Ref().Set("Simulation.EdgeVelocityX", edgeVelocityX);
	gModel->SetEdgeVelocityX(edgeVelocityX);
	notifySettingsChanged();
}

float OptionsModel::GetEdgeVelocityY()
{
	return Requested().edgeVelocityY.value_or(Shown()->air->edgeVelocityY);
}
void OptionsModel::SetEdgeVelocityY(float edgeVelocityY)
{
	if (ToRoom({ .edgeVelocityY = edgeVelocityY }))
		return;
	GlobalPrefs::Ref().Set("Simulation.EdgeVelocityY", edgeVelocityY);
	gModel->SetEdgeVelocityY(edgeVelocityY);
	notifySettingsChanged();
}

float OptionsModel::GetVorticityCoeff()
{
	return Requested().vorticityCoeff.value_or(Shown()->air->vorticityCoeff);
}
void OptionsModel::SetVorticityCoeff(float vorticityCoeff)
{
	if (ToRoom({ .vorticityCoeff = vorticityCoeff }))
		return;
	GlobalPrefs::Ref().Set("Simulation.VorticityCoeff", vorticityCoeff);
	gModel->SetVorticityCoeff(vorticityCoeff);
	notifySettingsChanged();
}

int OptionsModel::GetConvectionMode()
{
	return Requested().convectionMode.value_or(Shown()->air->convectionMode);
}
void OptionsModel::SetConvectionMode(int convMode)
{
	if (ToRoom({ .convectionMode = convMode }))
		return;
	GlobalPrefs::Ref().Set("Simulation.ConvectionMode", convMode);
	gModel->SetConvectionMode(convMode);
	notifySettingsChanged();
}

int OptionsModel::GetGravityMode()
{
	return Requested().gravityMode.value_or(Shown()->gravityMode);
}
void OptionsModel::SetGravityMode(int gravityMode)
{
	if (ToRoom({ .gravityMode = gravityMode }))
		return;
	sim->gravityMode = gravityMode;
	notifySettingsChanged();
}

float OptionsModel::GetCustomGravityX()
{
	return Requested().customGravityX.value_or(Shown()->customGravityX);
}

void OptionsModel::SetCustomGravityX(float x)
{
	if (ToRoom({ .customGravityX = x }))
		return;
	sim->customGravityX = x;
	notifySettingsChanged();
}

float OptionsModel::GetCustomGravityY()
{
	return Requested().customGravityY.value_or(Shown()->customGravityY);
}

void OptionsModel::SetCustomGravityY(float y)
{
	if (ToRoom({ .customGravityY = y }))
		return;
	sim->customGravityY = y;
	notifySettingsChanged();
}

int OptionsModel::GetScale()
{
	return ui::Engine::Ref().GetScale();
}

void OptionsModel::SetScale(int scale)
{
	ui::Engine::Ref().SetScale(scale);
	GlobalPrefs::Ref().Set("Scale", int(scale));
	notifySettingsChanged();
}

bool OptionsModel::GetGraveExitsConsole()
{
	return ui::Engine::Ref().GraveExitsConsole;
}

void OptionsModel::SetGraveExitsConsole(bool graveExitsConsole)
{
	ui::Engine::Ref().GraveExitsConsole = graveExitsConsole;
	GlobalPrefs::Ref().Set("GraveExitsConsole", graveExitsConsole);
	notifySettingsChanged();
}

bool OptionsModel::GetNativeClipoard()
{
	return Clipboard::GetEnabled();
}

void OptionsModel::SetNativeClipoard(bool nativeClipoard)
{
	Clipboard::SetEnabled(nativeClipoard);
	GlobalPrefs::Ref().Set("NativeClipboard.Enabled", nativeClipoard);
	notifySettingsChanged();
}

bool OptionsModel::GetResizable()
{
	return ui::Engine::Ref().GetResizable();
}

void OptionsModel::SetResizable(bool resizable)
{
	ui::Engine::Ref().SetResizable(resizable);
	GlobalPrefs::Ref().Set("Resizable", resizable);
	notifySettingsChanged();
}

bool OptionsModel::GetFullscreen()
{
	return ui::Engine::Ref().GetFullscreen();
}
void OptionsModel::SetFullscreen(bool fullscreen)
{
	ui::Engine::Ref().SetFullscreen(fullscreen);
	GlobalPrefs::Ref().Set("Fullscreen", fullscreen);
	notifySettingsChanged();
}

bool OptionsModel::GetChangeResolution()
{
	return ui::Engine::Ref().GetChangeResolution();
}

void OptionsModel::SetChangeResolution(bool newChangeResolution)
{
	ui::Engine::Ref().SetChangeResolution(newChangeResolution);
	GlobalPrefs::Ref().Set("AltFullscreen", newChangeResolution);
	notifySettingsChanged();
}

bool OptionsModel::GetForceIntegerScaling()
{
	return ui::Engine::Ref().GetForceIntegerScaling();
}

void OptionsModel::SetForceIntegerScaling(bool forceIntegerScaling)
{
	ui::Engine::Ref().SetForceIntegerScaling(forceIntegerScaling);
	GlobalPrefs::Ref().Set("ForceIntegerScaling", forceIntegerScaling);
	notifySettingsChanged();
}

bool OptionsModel::GetBlurryScaling()
{
	return ui::Engine::Ref().GetBlurryScaling();
}

void OptionsModel::SetBlurryScaling(bool newBlurryScaling)
{
	ui::Engine::Ref().SetBlurryScaling(newBlurryScaling);
	GlobalPrefs::Ref().Set("BlurryScaling", newBlurryScaling);
	notifySettingsChanged();
}

bool OptionsModel::GetFastQuit()
{
	return ui::Engine::Ref().GetFastQuit();
}
void OptionsModel::SetFastQuit(bool fastquit)
{
	ui::Engine::Ref().SetFastQuit(fastquit);
	GlobalPrefs::Ref().Set("FastQuit", bool(fastquit));
	notifySettingsChanged();
}

bool OptionsModel::GetGlobalQuit()
{
	return ui::Engine::Ref().GetGlobalQuit();
}
void OptionsModel::SetGlobalQuit(bool newGlobalQuit)
{
	ui::Engine::Ref().SetGlobalQuit(newGlobalQuit);
	GlobalPrefs::Ref().Set("GlobalQuit", newGlobalQuit);
	notifySettingsChanged();
}

int OptionsModel::GetDecoSpace()
{
	return gModel->GetDecoSpace();
}
void OptionsModel::SetDecoSpace(int decoSpace)
{
	GlobalPrefs::Ref().Set("Simulation.DecoSpace", decoSpace);
	gModel->SetDecoSpace(decoSpace);
	notifySettingsChanged();
}

bool OptionsModel::GetShowAvatars()
{
	return ui::Engine::Ref().ShowAvatars;
}

void OptionsModel::SetShowAvatars(bool state)
{
	ui::Engine::Ref().ShowAvatars = state;
	GlobalPrefs::Ref().Set("ShowAvatars", state);
	notifySettingsChanged();
}

bool OptionsModel::GetMouseClickRequired()
{
	return gModel->GetMouseClickRequired();
}

void OptionsModel::SetMouseClickRequired(bool mouseClickRequired)
{
	GlobalPrefs::Ref().Set("MouseClickRequired", mouseClickRequired);
	gModel->SetMouseClickRequired(mouseClickRequired);
	notifySettingsChanged();
}

bool OptionsModel::GetIncludePressure()
{
	return gModel->GetIncludePressure();
}

void OptionsModel::SetIncludePressure(bool includePressure)
{
	GlobalPrefs::Ref().Set("Simulation.IncludePressure", includePressure);
	gModel->SetIncludePressure(includePressure);
	notifySettingsChanged();
}

bool OptionsModel::GetPerfectCircle()
{
	return gModel->GetPerfectCircle();
}

void OptionsModel::SetPerfectCircle(bool perfectCircle)
{
	GlobalPrefs::Ref().Set("PerfectCircleBrush", perfectCircle);
	gModel->SetPerfectCircle(perfectCircle);
	notifySettingsChanged();
}

bool OptionsModel::GetMomentumScroll()
{
	return ui::Engine::Ref().MomentumScroll;
}

void OptionsModel::SetMomentumScroll(bool state)
{
	GlobalPrefs::Ref().Set("MomentumScroll", state);
	ui::Engine::Ref().MomentumScroll = state;
	notifySettingsChanged();
}

bool OptionsModel::GetRedirectStd()
{
	return Client::Ref().GetRedirectStd();
}

void OptionsModel::SetRedirectStd(bool newRedirectStd)
{
	GlobalPrefs::Ref().Set("RedirectStd", newRedirectStd);
	Client::Ref().SetRedirectStd(newRedirectStd);
	notifySettingsChanged();
}
bool OptionsModel::GetAutoStartupRequest()
{
	return Client::Ref().GetAutoStartupRequest();
}

void OptionsModel::SetAutoStartupRequest(bool newAutoStartupRequest)
{
	GlobalPrefs::Ref().Set("AutoStartupRequest", newAutoStartupRequest);
	Client::Ref().SetAutoStartupRequest(newAutoStartupRequest);
	notifySettingsChanged();
}

SimFpsLimit OptionsModel::GetFpsLimit()
{
	return gModel->GetView()->GetSimFpsLimit();
}
void OptionsModel::SetFpsLimit(SimFpsLimit newFpsLimit)
{
	float saved = 2;
	if (auto *fpsLimitExplicit = std::get_if<FpsLimitExplicit>(&newFpsLimit))
	{
		saved = fpsLimitExplicit->value;
	}
	GlobalPrefs::Ref().Set("FpsLimit", saved);
	return gModel->GetView()->SetSimFpsLimit(newFpsLimit);
	notifySettingsChanged();
}

DrawLimit OptionsModel::GetDrawLimit()
{
	return ui::Engine::Ref().GetDrawingFrequencyLimit();
}
void OptionsModel::SetDrawLimit(DrawLimit newDrawLimit)
{
	int saved = 0;
	if (std::holds_alternative<DrawLimitDisplay>(newDrawLimit))
	{
		saved = -1;
	}
	if (auto *drawLimitExplicit = std::get_if<DrawLimitExplicit>(&newDrawLimit))
	{
		saved = drawLimitExplicit->value;
	}
	GlobalPrefs::Ref().Set("DrawLimit", saved);
	ui::Engine::Ref().SetDrawingFrequencyLimit(newDrawLimit);
	notifySettingsChanged();
}

void OptionsModel::notifySettingsChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifySettingsChanged(this);
	}
}

OptionsModel::~OptionsModel() {
}

