#pragma once
#include <optional>

enum EdgeMode
{
	EDGE_VOID,
	EDGE_SOLID,
	EDGE_LOOP,
	NUM_EDGEMODES,
};

enum AirMode
{
	AIR_ON,
	AIR_PRESSUREOFF,
	AIR_VELOCITYOFF,
	AIR_OFF,
	AIR_NOUPDATE,
	NUM_AIRMODES,
};

enum ConvectionMode
{
	AIRC_NONE,
	AIRC_LEGACY,
	AIRC_BOUSSINESQ,
	NUM_CONVMODES,
};

enum GravityMode
{
	GRAV_VERTICAL,
	GRAV_OFF,
	GRAV_RADIAL,
	GRAV_CUSTOM,
	NUM_GRAVMODES,
};

enum DecoSpace
{
	DECOSPACE_SRGB,
	DECOSPACE_LINEAR,
	DECOSPACE_GAMMA22,
	DECOSPACE_GAMMA18,
	NUM_DECOSPACES,
};

enum TempScale
{
	TEMPSCALE_KELVIN,
	TEMPSCALE_CELSIUS,
	TEMPSCALE_FAHRENHEIT,
	NUM_TEMPSCALES,
};

// A change to the simulation settings the options dialog and the mode shortcuts
// control. Unset fields stay as they are. Multiplayer rooms apply these for
// everyone, in order with edits.
struct SimulationSettingsChange
{
	std::optional<bool> heat, ambientHeat, waterEqualisation, newtonianGravity, prettyPowder;
	std::optional<int> airMode, edgeMode, convectionMode, gravityMode;
	std::optional<float> ambientAirTemp, edgePressure, edgeVelocityX, edgeVelocityY, vorticityCoeff, customGravityX, customGravityY;

	// Visits every field as (name, member pointer).
	template<class Visit>
	static void Fields(Visit &&visit)
	{
		visit("heat", &SimulationSettingsChange::heat);
		visit("ambientHeat", &SimulationSettingsChange::ambientHeat);
		visit("waterEqualisation", &SimulationSettingsChange::waterEqualisation);
		visit("newtonianGravity", &SimulationSettingsChange::newtonianGravity);
		visit("prettyPowder", &SimulationSettingsChange::prettyPowder);
		visit("airMode", &SimulationSettingsChange::airMode);
		visit("edgeMode", &SimulationSettingsChange::edgeMode);
		visit("convectionMode", &SimulationSettingsChange::convectionMode);
		visit("gravityMode", &SimulationSettingsChange::gravityMode);
		visit("ambientAirTemp", &SimulationSettingsChange::ambientAirTemp);
		visit("edgePressure", &SimulationSettingsChange::edgePressure);
		visit("edgeVelocityX", &SimulationSettingsChange::edgeVelocityX);
		visit("edgeVelocityY", &SimulationSettingsChange::edgeVelocityY);
		visit("vorticityCoeff", &SimulationSettingsChange::vorticityCoeff);
		visit("customGravityX", &SimulationSettingsChange::customGravityX);
		visit("customGravityY", &SimulationSettingsChange::customGravityY);
	}

	// Takes every field other sets.
	void Merge(const SimulationSettingsChange &other)
	{
		Fields([this, &other](const char *, auto member) {
			if (other.*member)
				this->*member = other.*member;
		});
	}

	bool Empty() const
	{
		bool empty = true;
		Fields([this, &empty](const char *, auto member) {
			if (this->*member)
				empty = false;
		});
		return empty;
	}
};
