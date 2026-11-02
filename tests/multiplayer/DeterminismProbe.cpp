// Cross-platform determinism: the same scripted worlds must reach the same
// state, bit for bit, on every compiler, maths library and CPU.
//
// A room's server and its players are often different systems (a Linux server,
// Windows and macOS players). Each runs physics itself, so any difference in
// floating-point code generation or maths functions makes them disagree, and
// the player resynchronizes over and over. This runs a busy mixed world and a
// scene for every element, and prints fingerprints as it goes. With --expect,
// it compares the final fingerprint with the value pinned for all platforms.
//
// Usage: tpt-determinism [--verbose] [--expect HEX]
#include "multiplayer/World.h"
#include "multiplayer/StateDigest.h"
#include "simulation/ElementClasses.h"
#include "simulation/SimulationData.h"
#include "simulation/Air.h"
#include "prefs/GlobalPrefs.h"
#include <iostream>
#include <string>

using namespace multiplayer;

namespace
{
void Fill(Simulation &sim, int type, int x0, int y0, int x1, int y1)
{
	for (int y = y0; y <= y1; ++y)
		for (int x = x0; x <= x1; ++x)
			sim.create_part(-1, x, y, type);
}

// Many elements interacting: liquids, fire, explosives, electronics, gases,
// heat and pressure, with ambient heat and Newtonian gravity on.
void Busy(World &w)
{
	auto &sim = *w.sim;
	sim.rng.seed(20261102);
	sim.aheat_enable = 1;
	sim.EnableNewtonianGravity(true);
	Fill(sim, PT_DMND, 20, 360, 590, 365);
	Fill(sim, PT_WATR, 30, 300, 180, 355);
	Fill(sim, PT_OIL, 200, 320, 260, 355);
	Fill(sim, PT_LAVA, 280, 330, 320, 355);
	Fill(sim, PT_SAND, 340, 250, 400, 300);
	Fill(sim, PT_SNOW, 420, 250, 470, 300);
	Fill(sim, PT_GUNP, 490, 320, 520, 355);
	Fill(sim, PT_FIRE, 480, 300, 530, 315);
	Fill(sim, PT_PLSM, 100, 200, 115, 215);
	Fill(sim, PT_GAS, 150, 150, 230, 200);
	Fill(sim, PT_H2, 250, 150, 300, 190);
	Fill(sim, PT_O2, 300, 150, 350, 190);
	Fill(sim, PT_METL, 40, 240, 580, 241);
	sim.create_part(-1, 40, 240, PT_SPRK);
	Fill(sim, PT_C5, 360, 200, 380, 215);
	Fill(sim, PT_LNTG, 382, 200, 386, 215);
	Fill(sim, PT_PHOT, 10, 100, 12, 110);
	Fill(sim, PT_NEUT, 200, 100, 204, 104);
	Fill(sim, PT_URAN, 205, 110, 230, 130);
	Fill(sim, PT_ACID, 540, 330, 580, 355);
	Fill(sim, PT_STKM, 300, 100, 300, 100);
	for (int i = 0; i < 400; ++i)
		sim.create_part(-1, 20 + (i * 37) % 570, 20 + (i * 53) % 220, 1 + (i * 7) % (PT_NUM - 1));
	for (int y = 0; y < YCELLS; y += 7)
		for (int x = 0; x < XCELLS; x += 5)
		{
			sim.pv[y][x] = float((x * 3 + y * 5) % 17) - 8.f;
			sim.vx[y][x] = float((x + y) % 7) - 3.f;
			sim.vy[y][x] = float((x * y) % 5) - 2.f;
			sim.hv[y][x] = 200.f + float((x * 11 + y) % 300);
		}
	sim.BeforeSim(false);
}

// One element in a small reactive scene, as the element sweep builds it.
void ElementScene(World &w, int element)
{
	auto &sim = *w.sim;
	sim.rng.seed(5000 + element);
	Fill(sim, PT_DMND, 60, 330, 560, 334);
	Fill(sim, element, 280, 250, 320, 290);
	for (int i = 0; i < 60; ++i)
		sim.create_part(-1, 100 + (i * 37) % 420, 120 + (i * 53) % 150, element);
	Fill(sim, PT_WATR, 330, 300, 380, 325);
	Fill(sim, PT_FIRE, 230, 300, 250, 320);
	Fill(sim, PT_METL, 150, 290, 450, 291);
	sim.create_part(-1, 150, 290, PT_SPRK);
	for (int y = 240; y < 300; ++y)
		for (int x = 270; x < 330; ++x)
			if (auto r = sim.pmap[y][x])
				sim.parts[ID(r)].temp = (x + y) % 3 ? 3000.f : 20.f;
	sim.pv[60][70] = 20.f; sim.vx[60][70] = 5.f;
	sim.BeforeSim(false);
}

std::string Groups(const Json::Value &fields)
{
	std::string out;
	for (const auto &name : fields.getMemberNames())
		out += " " + name + "=" + fields[name].asString().substr(0, 8);
	return out;
}
}

int main(int argc, char **argv)
{
	try
	{
		bool verbose = false;
		std::string expect;
		for (int i = 1; i < argc; ++i)
		{
			std::string a = argv[i];
			if (a == "--verbose") verbose = true;
			else if (a == "--expect" && i + 1 < argc) expect = argv[++i];
			else throw std::runtime_error("usage: tpt-determinism [--verbose] [--expect HEX]");
		}
		GlobalPrefs prefs;
		SimulationData data;
		auto &sd = SimulationData::CRef();
		std::string all;
		{
			World w;
			Busy(w);
			for (int t = 1; t <= 1200; ++t)
			{
				w.Advance();
				if (t % 60 == 0)
				{
					auto fields = DiagnosticDigests(w);
					all += DigestSummary(fields);
					std::cout << "busy tick " << t << " " << DigestSummary(fields) << (verbose ? Groups(fields) : "") << "\n";
				}
			}
		}
		for (int element = 1; element < PT_NUM; ++element)
		{
			if (!sd.elements[element].Enabled)
				continue;
			World w;
			ElementScene(w, element);
			for (int t = 0; t < 120; ++t)
				w.Advance();
			auto fields = DiagnosticDigests(w);
			all += DigestSummary(fields);
			if (verbose)
				std::cout << "element " << sd.elements[element].Name.ToUtf8() << " " << DigestSummary(fields) << Groups(fields) << "\n";
		}
		Json::Value total;
		total["all"] = all;
		auto final = DigestSummary(total);
		std::cout << "determinism fingerprint " << final << std::endl;
		if (!expect.empty() && final != expect)
		{
			std::cerr << "This platform computes different physics: expected " << expect << ", got " << final
			          << ". Rooms between this platform and others would keep resynchronizing.\n";
			return 1;
		}
		return 0;
	}
	catch (const std::exception &e)
	{
		std::cerr << "determinism probe error: " << e.what() << '\n';
		return 1;
	}
}
