#pragma once
#include "GravityData.h"
#include "GravityPtr.h"

class Gravity
{
protected:
	Gravity() = default;
	// Multiplayer rooms (DeterministicGravity.cpp): computed in the calling
	// thread, identically on every machine.
	bool deterministic = false;
	void ExchangeDeterministic(GravityOutput &gravOut, GravityInput &gravIn, bool forceRecalc);
	static void DestroyDeterministic(Gravity *gravity);
	friend struct GravityDeleter;

public:
	// potentially clobbers gravIn
	void Exchange(GravityOutput &gravOut, GravityInput &gravIn, bool forceRecalc);

	static GravityPtr Create();
	static GravityPtr CreateDeterministic();
};
