#pragma once
#include "SimulationData.h"
#include "Element.h"
#include "ElementNumbers.h"
#include <array>

#define ELEMENT_NUMBERS_ENUMERATE(name, id) constexpr int PT_ ## name = id;
ELEMENT_NUMBERS(ELEMENT_NUMBERS_ENUMERATE);
#undef ELEMENT_NUMBERS_ENUMERATE

std::array<Element, PT_NUM> const &GetElements();

#include "elements/PIPE.h"

bool SimulationData::IsHeatInsulator(const Particle &p) const
{
	return elements[p.type].HeatConduct == 0 || (p.type == PT_HSWC && p.life != 10) || ((p.type == PT_PIPE || p.type == PT_PPIP) && (p.tmp & PFLAG_CAN_CONDUCT) == 0);
}

float SimulationData::HeatCapacityOf(const Particle &p) const
{
	if ((p.type == PT_PIPE || p.type == PT_PPIP) && IsElement(p.ctype))
		return elements[p.type].HeatCapacity + elements[p.ctype].HeatCapacity;
	else
		return elements[p.type].HeatCapacity;
}
