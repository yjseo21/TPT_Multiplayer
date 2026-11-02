// Every element, in a room: a snapshot taken mid-run continues exactly.
//
// A player joining a room continues from a snapshot of it. If any element keeps
// state outside what the snapshot carries (a static variable, a field the
// snapshot forgets), that player silently diverges. For every enabled element
// this builds a small busy scene around it (heat, sparks, liquids, fire, acid,
// neutrons, a conductor wire, walls), runs it, snapshots it into a second world
// and runs both side by side, comparing every fingerprint group as they go.
// Elements rotate through room settings (Newtonian gravity, gravity and air
// modes, edge modes, ambient heat) so those paths are swept too.
#include "multiplayer/World.h"
#include "multiplayer/ExactCheckpoint.h"
#include "multiplayer/StateDigest.h"
#include "multiplayer/FloatingPoint.h"
#include "simulation/ElementClasses.h"
#include "simulation/SimulationData.h"
#include "simulation/Air.h"
#include "prefs/GlobalPrefs.h"
#include <iostream>
#include <string>
#include <vector>

using namespace multiplayer;

namespace
{
void Fill(Simulation &sim, int type, int x0, int y0, int x1, int y1)
{
	for (int y = y0; y <= y1; ++y)
		for (int x = x0; x <= x1; ++x)
			sim.create_part(-1, x, y, type);
}

// The element in the middle, reactive neighbours around it.
void Scene(World &w, int element, int variant)
{
	auto &sim = *w.sim;
	sim.rng.seed(1000 + element);
	// Room settings, rotated across elements.
	switch (variant % 6)
	{
	case 0: break;
	case 1: sim.EnableNewtonianGravity(true); break;
	case 2: sim.gravityMode = GRAV_RADIAL; sim.aheat_enable = 1; break;
	case 3: sim.SetEdgeMode(EDGE_LOOP); sim.air->airMode = AIR_PRESSUREOFF; break;
	case 4: sim.EnableNewtonianGravity(true); sim.SetEdgeMode(EDGE_SOLID); sim.legacy_enable = 1; break;
	case 5: sim.gravityMode = GRAV_CUSTOM; sim.customGravityX = .3f; sim.customGravityY = -.2f; sim.water_equal_test = 1; break;
	}
	// A floor and walls of stone in a box.
	Fill(sim, PT_DMND, 60, 330, 560, 334);
	Fill(sim, PT_DMND, 60, 200, 64, 334);
	Fill(sim, PT_DMND, 556, 200, 560, 334);
	// The element itself, a block and a scattering.
	Fill(sim, element, 280, 250, 320, 290);
	for (int i = 0; i < 60; ++i)
		sim.create_part(-1, 100 + (i * 37) % 420, 120 + (i * 53) % 150, element);
	// Neighbours that react with most things.
	Fill(sim, PT_WATR, 330, 300, 380, 325);
	Fill(sim, PT_FIRE, 230, 300, 250, 320);
	Fill(sim, PT_LAVA, 200, 310, 220, 325);
	Fill(sim, PT_ACID, 390, 300, 400, 325);
	Fill(sim, PT_SALT, 260, 230, 270, 240);
	Fill(sim, PT_ICEI, 420, 300, 440, 325);
	Fill(sim, PT_NEUT, 300, 200, 304, 204);
	Fill(sim, PT_PLNT, 450, 310, 470, 325);
	// A sparked metal wire through the element.
	Fill(sim, PT_METL, 150, 290, 450, 291);
	sim.create_part(-1, 150, 290, PT_SPRK);
	// Hot and cold spots, pressure.
	for (int y = 240; y < 300; ++y)
		for (int x = 270; x < 330; ++x)
			if (auto r = sim.pmap[y][x])
				sim.parts[ID(r)].temp = (x + y) % 3 ? 4000.f : 10.f;
	sim.pv[60][70] = 20.f; sim.vx[60][70] = 5.f;
	sim.BeforeSim(false);
}
}

int main()
{
	try
	{
		GlobalPrefs prefs;
		SimulationData data;
		auto &sd = SimulationData::CRef();
		int checked = 0, variant = 0;
		std::vector<std::string> failures;
		for (int element = 1; element < PT_NUM; ++element)
		{
			if (!sd.elements[element].Enabled)
				continue;
			World a;
			Scene(a, element, variant++);
			for (int t = 0; t < 30; ++t) a.Advance();
			World b;
			RestoreExactCheckpoint(b, EncodeExactCheckpoint(a));
			auto name = sd.elements[element].Name.ToUtf8();
			for (int t = 0; t <= 60; ++t)
			{
				if (t % 20 == 0)
				{
					auto da = DiagnosticDigests(a), db = DiagnosticDigests(b);
					if (da != db)
					{
						std::string groups;
						for (const auto &group : da.getMemberNames())
							if (da[group] != db[group])
								groups += (groups.empty() ? "" : ",") + group;
						failures.push_back(name + " (settings " + std::to_string((variant - 1) % 6) + ") diverged " +
						                   std::to_string(t) + " ticks after the snapshot: " + groups);
						break;
					}
				}
				a.Advance(); b.Advance();
			}
			++checked;
		}
		if (!failures.empty())
		{
			for (auto &failure : failures)
				std::cerr << failure << '\n';
			return 1;
		}
		std::cout << "Element sweep passed: " << checked << " elements continue exactly from a mid-run snapshot\n";
		return 0;
	}
	catch (const std::exception &e)
	{
		std::cerr << "Element sweep error: " << e.what() << '\n';
		return 1;
	}
}
