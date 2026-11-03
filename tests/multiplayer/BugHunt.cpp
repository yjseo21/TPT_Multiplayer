// Bug hunt: every element against every other, under heat, cold and pressure,
// with the simulation's own bookkeeping checked after every tick.
//
//   tpt-bughunt pairs [--shard K --of N] [--ticks T]
//
// Every check is an invariant the game relies on: particles have a valid,
// enabled type and finite position, velocity and temperature inside the
// screen and the temperature range; element counts match the particles that
// exist (stickman spawning and several elements read them); the stickmen and
// fighters agree with their particles. A sample of scenes also continues from
// a snapshot and survives a save round trip. Run under the sanitizers to catch
// memory errors and undefined behaviour too.
#include "multiplayer/World.h"
#include "multiplayer/ExactCheckpoint.h"
#include "multiplayer/StateDigest.h"
#include "simulation/ElementClasses.h"
#include "simulation/SimulationData.h"
#include "simulation/Air.h"
#include "client/GameSave.h"
#include "common/tpt-rand.h"
#include "multiplayer/Protocol.h"
#include "multiplayer/Canvas.h"
#include "graphics/Renderer.h"
#include "simulation/ToolClasses.h"
#include "simulation/SimTool.h"
#include "simulation/StructProperty.h"
#include "prefs/GlobalPrefs.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <tuple>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace multiplayer;

namespace
{
std::string Name(int t)
{
	auto &sd = SimulationData::CRef();
	return t > 0 && t < PT_NUM ? sd.elements[t].Name.ToUtf8() : "#" + std::to_string(t);
}

// One report per kind of problem and element (the first scene that shows it).
std::set<std::string> reported;
int issues = 0;
void Report(const std::string &kind, const std::string &key, const std::string &detail)
{
	if (!reported.insert(kind + "|" + key).second)
		return;
	++issues;
	std::cout << "ISSUE " << kind << " " << key << " : " << detail << std::endl;
}

void CheckWorld(World &w, const std::string &scene, int tick)
{
	auto &sim = *w.sim;
	auto &sd = SimulationData::CRef();
	std::array<int, PT_NUM> counted{};
	int live = 0;
	// Particles past parts.active are never updated: there must be none.
	auto end = tick % 20 == 0 ? NPART : sim.parts.active;
	for (int i = 0; i < end; ++i)
	{
		auto &p = sim.parts[i];
		if (!p.type)
			continue;
		if (i >= sim.parts.active)
			Report("beyond-active", Name(p.type), scene + " tick " + std::to_string(tick) + " particle " + std::to_string(i));
		if (p.type < 0 || p.type >= PT_NUM || !sd.elements[p.type].Enabled)
		{
			Report("bad-type", std::to_string(p.type), scene + " tick " + std::to_string(tick));
			continue;
		}
		++live; ++counted[p.type];
		auto where = scene + " tick " + std::to_string(tick);
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.vx) || !std::isfinite(p.vy))
			Report("non-finite-motion", Name(p.type), where + " x=" + std::to_string(p.x) + " y=" + std::to_string(p.y) +
			       " vx=" + std::to_string(p.vx) + " vy=" + std::to_string(p.vy));
		if (!std::isfinite(p.temp) || p.temp < MIN_TEMP - .01f || p.temp > MAX_TEMP + .01f)
			Report("temperature-range", Name(p.type), where + " temp=" + std::to_string(p.temp));
		int x = int(p.x + .5f), y = int(p.y + .5f);
		if (std::isfinite(p.x) && std::isfinite(p.y) && (x < 0 || y < 0 || x >= XRES || y >= YRES))
			Report("off-screen", Name(p.type), where + " at " + std::to_string(x) + "," + std::to_string(y));
	}
	if (live != sim.NUM_PARTS)
		Report("particle-count", scene.substr(0, scene.find(' ')), scene + " tick " + std::to_string(tick) +
		       " NUM_PARTS=" + std::to_string(sim.NUM_PARTS) + " live=" + std::to_string(live));
	for (int t = 1; t < PT_NUM; ++t)
		if (counted[t] != sim.elementCount[t])
			Report("element-count", Name(t), scene + " tick " + std::to_string(tick) + " count=" +
			       std::to_string(sim.elementCount[t]) + " particles=" + std::to_string(counted[t]));
	// Stickmen: a spawned player has its particle (or is held by a portal).
	auto inPortal = [&](int type) {
		for (auto &channel : sim.portalp) for (auto &dir : channel) for (auto &p : dir) if (p.type == type) return true;
		return false;
	};
	if (sim.player.spwn && !counted[PT_STKM] && !inPortal(PT_STKM))
		Report("stickman-ghost", "STKM", scene + " tick " + std::to_string(tick));
	if (sim.player2.spwn && !counted[PT_STKM2] && !inPortal(PT_STKM2))
		Report("stickman-ghost", "STK2", scene + " tick " + std::to_string(tick));
	int fighters = 0;
	for (auto &f : sim.fighters) fighters += f.spwn != 0;
	// Every spawned fighter has its particle (or is held by a portal).
	int held = 0;
	for (auto &channel : sim.portalp) for (auto &dir : channel) for (auto &p : dir) held += p.type == PT_FIGH;
	if (fighters != counted[PT_FIGH] + held)
		Report("fighter-ghost", "FIGH", scene + " tick " + std::to_string(tick) + " spawned=" + std::to_string(fighters) +
		       " particles=" + std::to_string(counted[PT_FIGH]) + " in portals=" + std::to_string(held));
	if (fighters != sim.fighcount)
		Report("fighter-count", "FIGH", scene + " tick " + std::to_string(tick) + " fighcount=" + std::to_string(sim.fighcount) +
		       " spawned=" + std::to_string(fighters));
	// The particle maps name each particle's type: an entry whose type differs
	// from its particle's means the type changed without the map (elements then
	// react to a type that is not there).
	for (int y = 0; y < YRES; ++y)
		for (int x = 0; x < XRES; ++x)
			for (auto r : { sim.pmap[y][x], sim.photons[y][x] })
				if (r && sim.parts[ID(r)].type && TYP(r) != sim.parts[ID(r)].type)
					Report("map-type-stale", Name(TYP(r)) + "->" + Name(sim.parts[ID(r)].type), scene + " tick " + std::to_string(tick));
	if (tick % 10)
		return;
	for (int y = 0; y < YCELLS; ++y)
		for (int x = 0; x < XCELLS; ++x)
			if (!std::isfinite(sim.pv[y][x]) || !std::isfinite(sim.vx[y][x]) || !std::isfinite(sim.vy[y][x]) || !std::isfinite(sim.hv[y][x]))
			{
				Report("non-finite-air", scene.substr(0, scene.find(' ')), scene + " tick " + std::to_string(tick));
				return;
			}
}

// Every particle's graphics function runs (players' computers draw the room).
void Render(World &w)
{
	static Renderer renderer;
	static unsigned view = 0;
	RenderCanvas(renderer, *w.sim, 1, w.tick, 0, false, uint8_t(view++ % CanvasViews), {}, 0, 1, false);
	// Signs as the game lays them out (text, values, links).
	for (auto &s : w.sim->signs)
	{
		int x, y, wd, h;
		s.getDisplayText(w.sim.get(), x, y, wd, h);
		s.split();
	}
}

void Fill(Simulation &sim, int type, int x0, int y0, int x1, int y1, float temp = -1)
{
	for (int y = y0; y <= y1; ++y)
		for (int x = x0; x <= x1; ++x)
		{
			auto i = sim.create_part(-1, x, y, type);
			if (i >= 0 && temp >= 0) sim.parts[i].temp = temp;
		}
}

// A against B: two touching blocks in a diamond box, in one of four conditions.
void PairScene(World &w, int a, int b, int variant)
{
	auto &sim = *w.sim;
	sim.rng.seed(a * 1000 + b);
	Fill(sim, PT_DMND, 150, 280, 330, 283);
	Fill(sim, PT_DMND, 150, 180, 153, 283);
	Fill(sim, PT_DMND, 327, 180, 330, 283);
	float ta = -1, tb = -1;
	switch (variant)
	{
	case 1: ta = 4000; break;                       // A very hot
	case 2: tb = 5; break;                          // B near absolute zero
	case 3: for (int y = 9; y < 14; ++y) for (int x = 20; x < 30; ++x) sim.pv[y][x] = 150; break; // high pressure
	}
	Fill(sim, a, 200, 230, 239, 279, ta);
	Fill(sim, b, 240, 230, 279, 279, tb);
	// A spark at their seam, for conductors.
	sim.create_part(-1, 240, 278, PT_SPRK);
	sim.BeforeSim(false);
}

int Pairs(int shard, int of, int ticks)
{
	auto &sd = SimulationData::CRef();
	std::vector<int> elements;
	for (int t = 1; t < PT_NUM; ++t)
		if (sd.elements[t].Enabled)
			elements.push_back(t);
	int index = 0, scenes = 0;
	World w; // one world, cleared between scenes (building one costs far more than a scene)
	for (int a : elements)
		for (int b : elements)
		{
			if (index++ % of != shard)
				continue;
			int variant = (a * 7 + b) % 4;
			auto scene = Name(a) + "+" + Name(b) + " v" + std::to_string(variant);
			w.sim->clear_sim();
			PairScene(w, a, b, variant);
			for (int t = 0; t < ticks; ++t)
			{
				w.Advance();
				CheckWorld(w, scene, t);
			}
			if (scenes % 4 == 0)
				Render(w);
			// A sample continues exactly from a snapshot, and survives a save.
			if ((a + b) % 16 == 0)
			{
				World copy;
				RestoreExactCheckpoint(copy, EncodeExactCheckpoint(w));
				for (int t = 0; t < 20; ++t) { w.Advance(); copy.Advance(); }
				if (DiagnosticDigests(w) != DiagnosticDigests(copy))
					Report("snapshot-divergence", Name(a) + "+" + Name(b), scene);
				try
				{
					auto save = w.CaptureSave();
					auto bytes = save->Serialise().second;
					GameSave back(bytes, false);
					World loaded;
					loaded.sim->Load(&back, true, { 0, 0 });
					loaded.sim->BeforeSim(false);
					CheckWorld(loaded, scene + " (loaded)", 0);
					for (int t = 0; t < 5; ++t) { loaded.Advance(); CheckWorld(loaded, scene + " (loaded)", t + 1); }
				}
				catch (const std::exception &e)
				{
					Report("save-roundtrip", Name(a) + "+" + Name(b), scene + ": " + e.what());
				}
			}
			++scenes;
		}
	std::cout << "shard " << shard << "/" << of << ": " << scenes << " scenes, " << issues << " issues" << std::endl;
	return 0;
}

// One element in a block, as for every action scene.
void ElementScene(World &w, int element)
{
	auto &sim = *w.sim;
	sim.rng.seed(element);
	Fill(sim, PT_DMND, 150, 280, 330, 283);
	Fill(sim, element, 200, 230, 279, 279);
	Fill(sim, PT_WATR, 280, 250, 300, 279);
	sim.BeforeSim(false);
}

// A command as a player sends it: rejected ones (validation) are skipped.
bool Send(World &w, Json::Value v)
{
	v["seq"] = 1;
	Command c;
	try { c = ParseCommand(v, true); }
	catch (const std::exception &) { return false; }
	w.Apply(c);
	return true;
}

// Console set: every property of every element, to ordinary and extreme values.
int Actions(int shard, int of, int ticks)
{
	auto &sd = SimulationData::CRef();
	auto &properties = Particle::GetProperties();
	const std::vector<double> floats = { -1e30, -1e4, -300, -1, 0, 0.5, 300, 1e4, 1e30 };
	const std::vector<long long> ints = { INT32_MIN, -1000, -1, 0, 1, 2, 7, 255, 256, 65535, 1 << 20, INT32_MAX };
	const std::vector<long long> uints = { 0, 1, 0xFF000000u, 0xFFFFFFFFu };
	int index = 0, scenes = 0;
	World w;
	for (int element = 1; element < PT_NUM; ++element)
	{
		if (!sd.elements[element].Enabled)
			continue;
		for (int property = 0; property < int(properties.size()); ++property)
		{
			auto type = properties[property].Type;
			std::vector<Json::Value> values;
			if (type == StructProperty::Float) for (auto f : floats) values.push_back(f);
			else if (type == StructProperty::UInteger) for (auto u : uints) values.push_back(Json::UInt64(u));
			else for (auto i : ints) values.push_back(Json::Int64(i));
			for (auto &value : values)
			for (int hot = 0; hot < 2; ++hot)
			{
				if (index++ % of != shard)
					continue;
				w.sim->clear_sim();
				ElementScene(w, element);
				if (hot)
					for (int i = 0; i < NPART; ++i)
						if (w.sim->parts[i].type == element) w.sim->parts[i].temp = 1500;
				Json::Value v; v["op"] = "console"; v["cmd"] = "set"; v["property"] = property; v["value"] = value;
				v["selector"] = "type"; v["element"] = element;
				if (!Send(w, v))
					continue;
				auto scene = "set " + std::string(properties[property].Name) + "=" + value.toStyledString().substr(0, 20) + " on " + Name(element);
				scene.erase(std::remove(scene.begin(), scene.end(), '\n'), scene.end());
				if (hot) scene += " hot";
				CheckWorld(w, scene, 0);
				Render(w);
				for (int t = 1; t <= ticks; ++t) { w.Advance(); CheckWorld(w, scene, t); }
				Render(w);
				++scenes;
			}
		}
	}
	std::cout << "shard " << shard << "/" << of << ": " << scenes << " scenes, " << issues << " issues" << std::endl;
	return 0;
}

// Every tool (heat, air, gravity, walls, decoration, elements), every action and
// brush, on every element.
int Tools(int shard, int of, int ticks)
{
	auto &sd = SimulationData::CRef();
	std::vector<std::string> tools;
	for (auto &t : GetTools()) tools.push_back(std::string(t.Identifier));
	for (auto &wall : sd.wtypes) tools.push_back(std::string(wall.identifier));
	for (auto deco : { "DEFAULT_DECOR_SET", "DEFAULT_DECOR_CLR", "DEFAULT_DECOR_ADD", "DEFAULT_DECOR_SUB", "DEFAULT_DECOR_MUL", "DEFAULT_DECOR_DIV", "DEFAULT_DECOR_SMDG" })
		tools.push_back(deco);
	for (auto element : { PT_NONE, PT_SPRK, PT_LIGH, PT_TESC, PT_STKM, PT_FIGH, PT_SOAP, PT_PIPE, PT_PRTI, PT_BHOL, PT_SING, PT_NEUT, PT_FIRE, PT_PLSM, PT_CLNE, PT_VIRS, PT_LIFE })
		tools.push_back(std::string(sd.elements[element].Identifier));
	const char *actions[] = { "point", "line", "end", "box", "fill", "click", "drag" };
	const char *brushes[] = { "ellipse", "rectangle", "triangle", "ellipse-perfect" };
	int index = 0, scenes = 0;
	World w;
	for (int element = 1; element < PT_NUM; ++element)
	{
		if (!sd.elements[element].Enabled)
			continue;
		for (auto &tool : tools)
			for (int a = 0; a < 7; ++a)
			{
				if (index++ % of != shard)
					continue;
				w.sim->clear_sim();
				ElementScene(w, element);
				Json::Value v; v["op"] = "tool"; v["tool"] = tool; v["action"] = actions[a]; v["brush"] = brushes[(element + a) % 4];
				v["x1"] = 230; v["y1"] = 250; v["x2"] = 260 + a * 3; v["y2"] = 270;
				v["radius"] = a * 3; v["radiusY"] = (a * 5) % 9;
				v["strength"] = std::array{ 1, 100, 1000 }[(element + a) % 3];
				v["flags"] = (element + a) % 4; v["replace"] = element;
				v["shift"] = a % 2 == 1; v["ctrl"] = a % 3 == 1; v["alt"] = a % 4 == 1;
				v["colour"] = Json::UInt(0x80FF4020u + a);
				if (tool == "DEFAULT_PT_TESC" || tool == "DEFAULT_PT_LIGH") v["radius"] = 5;
				auto scene = tool + " " + actions[a] + " on " + Name(element);
				for (int repeat = 0; repeat < 3; ++repeat)
					Send(w, v);
				for (int t = 0; t < ticks; ++t) { w.Advance(); CheckWorld(w, scene, t); }
				Render(w);
				// A sample continues exactly from a snapshot (walls, fans, decoration...).
				if ((element + a) % 9 == 0)
				{
					World copy;
					RestoreExactCheckpoint(copy, EncodeExactCheckpoint(w));
					for (int t = 0; t < 15; ++t) { w.Advance(); copy.Advance(); }
					auto da = DiagnosticDigests(w), db = DiagnosticDigests(copy);
					if (da != db)
					{
						std::string groups;
						for (auto &g : da.getMemberNames()) if (da[g] != db[g]) groups += g + " ";
						Report("snapshot-divergence", tool, scene + ": " + groups);
					}
				}
				++scenes;
			}
	}
	std::cout << "shard " << shard << "/" << of << ": " << scenes << " scenes, " << issues << " issues" << std::endl;
	return 0;
}

// Pasting: every element's scene, as a stamp, onto another busy scene at an
// offset that overlaps it (pasting replaces what is there) and partly off screen.
int Paste(int shard, int of, int ticks)
{
	auto &sd = SimulationData::CRef();
	int index = 0, scenes = 0;
	World w;
	for (int element = 1; element < PT_NUM; ++element)
	{
		if (!sd.elements[element].Enabled)
			continue;
		World source;
		ElementScene(source, element);
		for (int t = 0; t < 10; ++t) source.Advance();
		auto stamp = source.sim->Save(true, RectBetween(Vec2{ 140, 220 }, Vec2{ 340, 290 }));
		auto bytes = stamp->Serialise().second;
		std::string text(bytes.begin(), bytes.end());
		for (int onto : { PT_WATR, PT_FIGH, PT_SOAP, PT_STKM, element })
			for (auto [cx, cy] : { std::pair{ 20, 25 }, std::pair{ -10, 40 }, std::pair{ 70, 60 }, std::pair{ 21, 26 } })
			{
				if (index++ % of != shard)
					continue;
				w.sim->clear_sim();
				ElementScene(w, onto);
				Json::Value v; v["op"] = "paste"; v["save"] = Base64Encode(text); v["x1"] = cx; v["y1"] = cy;
				auto scene = "paste " + Name(element) + " onto " + Name(onto) + " at " + std::to_string(cx) + "," + std::to_string(cy);
				for (int repeat = 0; repeat < 2; ++repeat)
					if (!Send(w, v))
						Report("paste-refused", Name(element), scene);
				CheckWorld(w, scene, 0);
				for (int t = 1; t <= ticks; ++t) { w.Advance(); CheckWorld(w, scene, t); }
				++scenes;
			}
	}
	std::cout << "shard " << shard << "/" << of << ": " << scenes << " scenes, " << issues << " issues" << std::endl;
	return 0;
}

// Room settings: every element against a few partners under every setting.
void ApplySetting(Simulation &sim, int setting)
{
	switch (setting)
	{
	case 0: sim.SetEdgeMode(EDGE_LOOP); break;
	case 1: sim.SetEdgeMode(EDGE_SOLID); break;
	case 2: sim.gravityMode = GRAV_RADIAL; break;
	case 3: sim.gravityMode = GRAV_OFF; break;
	case 4: sim.gravityMode = GRAV_CUSTOM; sim.customGravityX = -1000; sim.customGravityY = 1000; break;
	case 5: sim.air->airMode = AIR_PRESSUREOFF; break;
	case 6: sim.air->airMode = AIR_VELOCITYOFF; break;
	case 7: sim.air->airMode = AIR_OFF; break;
	case 8: sim.air->airMode = AIR_NOUPDATE; break;
	case 9: sim.EnableNewtonianGravity(true); break;
	case 10: sim.aheat_enable = 1; sim.air->ambientAirTemp = MAX_TEMP; break;
	case 11: sim.legacy_enable = 1; break;
	case 12: sim.water_equal_test = 1; break;
	case 13: sim.air->convectionMode = AIRC_BOUSSINESQ; sim.aheat_enable = 1; break;
	case 14: sim.air->edgePressure = MAX_PRESSURE; sim.air->edgeVelocityX = -MAX_PRESSURE; sim.air->vorticityCoeff = 1; break;
	case 15: sim.SetEdgeMode(EDGE_LOOP); sim.gravityMode = GRAV_CUSTOM; sim.customGravityX = 3; sim.EnableNewtonianGravity(true); break;
	}
}
int Settings(int shard, int of, int ticks)
{
	auto &sd = SimulationData::CRef();
	int index = 0, scenes = 0;
	for (int element = 1; element < PT_NUM; ++element)
	{
		if (!sd.elements[element].Enabled)
			continue;
		for (int partner : { PT_WATR, PT_FIRE, PT_METL, element })
			for (int setting = 0; setting < 16; ++setting)
			{
				if (index++ % of != shard)
					continue;
				World w;
				ApplySetting(*w.sim, setting);
				PairScene(w, element, partner, (element + setting) % 4);
				// Spread across the whole screen too, so edges and loops are crossed.
				for (int i = 0; i < 200; ++i)
					w.sim->create_part(-1, 5 + (i * 97) % (XRES - 10), 5 + (i * 61) % (YRES - 10), i % 2 ? element : partner);
				w.sim->BeforeSim(false);
				auto scene = Name(element) + "+" + Name(partner) + " setting " + std::to_string(setting);
				for (int t = 0; t < ticks; ++t) { w.Advance(); CheckWorld(w, scene, t); }
				if ((element + setting) % 3 == 0)
				{
					World copy;
					RestoreExactCheckpoint(copy, EncodeExactCheckpoint(w));
					for (int t = 0; t < 15; ++t) { w.Advance(); copy.Advance(); }
					auto da = DiagnosticDigests(w), db = DiagnosticDigests(copy);
					if (da != db)
					{
						std::string groups;
						for (auto &g : da.getMemberNames()) if (da[g] != db[g]) groups += g + " ";
						Report("snapshot-divergence", Name(element) + " setting " + std::to_string(setting), scene + ": " + groups);
					}
				}
				++scenes;
			}
	}
	std::cout << "shard " << shard << "/" << of << ": " << scenes << " scenes, " << issues << " issues" << std::endl;
	return 0;
}

// Stickmen: all four room stickmen hold every element in turn and use it while
// walking, jumping and crouching, through the room's own commands.
int Stickmen(int shard, int of, int ticks)
{
	auto &sd = SimulationData::CRef();
	int index = 0, scenes = 0;
	World w;
	for (int element = 0; element < PT_NUM; ++element)
	{
		if (element && !sd.elements[element].Enabled)
			continue;
		if (index++ % of != shard)
			continue;
		w.sim->clear_sim();
		auto &sim = *w.sim;
		Fill(sim, PT_DMND, CELL, 300, XRES - CELL - 1, 304);
		Fill(sim, PT_DMND, CELL, 200, CELL + 3, 300);
		Fill(sim, PT_DMND, XRES - CELL - 4, 200, XRES - CELL - 1, 300);
		for (int slot = 1; slot <= 4; ++slot)
		{
			Json::Value v; v["op"] = "stickman_spawn"; v["slot"] = slot; v["x1"] = 60 + slot * 110; v["y1"] = 270;
			Send(w, v);
		}
		for (int t = 0; t < 20; ++t) w.Advance();
		// Each stickman picks up the element (as touching it would).
		for (int slot = 1; slot <= 4; ++slot)
			if (auto *st = RoomStickman(sim, slot))
				for (int i = 0; i < NPART; ++i)
					if ((sim.parts[i].type == PT_STKM || sim.parts[i].type == PT_STKM2 || sim.parts[i].type == PT_FIGH) && element)
					{
						sim.parts[i].ctype = element;
						(void)st;
					}
		auto scene = "stickmen with " + Name(element);
		for (int t = 0; t < ticks; ++t)
		{
			if (t % 10 == 0)
				for (int slot = 1; slot <= 4; ++slot)
				{
					Json::Value v; v["op"] = "stickman"; v["slot"] = slot;
					v["comm"] = std::array{ 0x08, 0x0A, 0x04, 0x09, 0x01, 0x0C, 0x0F, 0x02 }[(t / 10 + slot) % 8];
					v["pcomm"] = (t / 10 + slot) % 3;
					Send(w, v);
				}
			w.Advance();
			CheckWorld(w, scene, t);
		}
		Render(w);
		++scenes;
	}
	std::cout << "shard " << shard << "/" << of << ": " << scenes << " scenes, " << issues << " issues" << std::endl;
	return 0;
}

// Signs: text with the game's formatting (values, links, buttons), broken and
// edge-case forms, at every edge of the screen, laid out as the game does.
int Signs(int shard, int of, int ticks)
{
	const std::vector<std::string> texts = { "{p}", "{t}", "{aheat}", "{type}", "{ctype}", "{life}", "{tmp}", "{tmp2}",
		"{b|button}", "{c:123|save}", "{t:456|thread}", "{s:search|find}", "{", "}", "{}", "{|}", "{b|", "{b|}", "{c:|x}",
		"{c:99999999999999999999|x}", "{t:-1|x}", "{p}{t}{p}{t}{p}{t}{p}{t}{p}{t}{p}{t}{p}{t}", "{{{{{{{{", "|||||", "{b|{c:1|x}}",
		"é中😀", "{p:}", "{x}", "{life}{tmp}{ctype}{type}{t}{p}{aheat}{b|", "a", "{c:1|}", "{b|{b|{b|x}}}" };
	const std::vector<std::pair<int, int>> spots = { {0, 0}, {XRES - 1, 0}, {0, YRES - 1}, {XRES - 1, YRES - 1}, {300, 200}, {4, 380} };
	int index = 0, scenes = 0;
	World w;
	for (size_t t = 0; t < texts.size(); ++t)
		for (size_t s = 0; s < spots.size(); ++s)
			for (int j = 0; j < 3; ++j)
			{
				if (index++ % of != shard)
					continue;
				w.sim->clear_sim();
				ElementScene(w, PT_METL);
				Json::Value v; v["op"] = "sign"; v["index"] = -1; v["text"] = texts[t]; v["x1"] = spots[s].first; v["y1"] = spots[s].second;
				v["justification"] = j;
				Send(w, v);
				auto scene = "sign '" + texts[t] + "' at " + std::to_string(spots[s].first) + "," + std::to_string(spots[s].second);
				if (!w.sim->signs.empty())
				{
					Json::Value b; b["op"] = "sign_button"; b["index"] = 0; b["x1"] = spots[s].first; b["y1"] = spots[s].second;
					Send(w, b);
				}
				for (int k = 0; k < ticks; ++k) { w.Advance(); CheckWorld(w, scene, k); }
				Render(w);
				++scenes;
			}
	std::cout << "shard " << shard << "/" << of << ": " << scenes << " scenes, " << issues << " issues" << std::endl;
	return 0;
}

// Fingerprints of every pair scene and setting, to compare across compilers and
// systems: any difference is a room where players on different systems desync.
int Digests(int shard, int of, int ticks)
{
	auto &sd = SimulationData::CRef();
	std::vector<int> elements;
	for (int t = 1; t < PT_NUM; ++t)
		if (sd.elements[t].Enabled)
			elements.push_back(t);
	int index = 0;
	World w;
	for (int a : elements)
		for (int b : elements)
		{
			if (index++ % of != shard)
				continue;
			int setting = (a * 3 + b) % 17; // 16: default settings
			w.sim->clear_sim();
			// clear_sim keeps settings; reset the ones ApplySetting changes.
			w.sim->SetEdgeMode(EDGE_VOID); w.sim->gravityMode = GRAV_VERTICAL; w.sim->customGravityX = w.sim->customGravityY = 0;
			w.sim->air->airMode = AIR_ON; w.sim->EnableNewtonianGravity(false); w.sim->aheat_enable = 0; w.sim->air->ambientAirTemp = R_TEMP + 273.15f;
			w.sim->legacy_enable = 0; w.sim->water_equal_test = 0; w.sim->air->convectionMode = AIRC_NONE;
			w.sim->air->edgePressure = 0; w.sim->air->edgeVelocityX = 0; w.sim->air->vorticityCoeff = 0;
			ApplySetting(*w.sim, setting);
			PairScene(w, a, b, (a + b) % 4);
			std::string first;
			for (int t = 0; t < ticks; ++t)
			{
				w.Advance();
				if (t == ticks / 4) first = DigestSummary(DiagnosticDigests(w));
			}
			std::cout << "DIGEST " << Name(a) << "+" << Name(b) << " s" << setting << " " << first << " " << DigestSummary(DiagnosticDigests(w)) << std::endl;
		}
	return 0;
}

// Chaos: random scenes, settings and player commands of every kind for a long
// run, with every check (invariants, snapshots, saves, rendering) along the way.
int Chaos(int shard, int of, int ticks)
{
	auto &sd = SimulationData::CRef();
	auto &properties = Particle::GetProperties();
	std::vector<int> elements;
	for (int t = 1; t < PT_NUM; ++t)
		if (sd.elements[t].Enabled)
			elements.push_back(t);
	std::vector<std::string> tools;
	for (auto &t : GetTools()) tools.push_back(std::string(t.Identifier));
	for (auto &wall : sd.wtypes) tools.push_back(std::string(wall.identifier));
	for (int e : elements) tools.push_back(std::string(sd.elements[e].Identifier));
	for (auto deco : { "DEFAULT_DECOR_SET", "DEFAULT_DECOR_ADD", "DEFAULT_DECOR_SMDG", "DEFAULT_UI_PROPERTY" }) tools.push_back(deco);
	const char *actions[] = { "point", "line", "end", "box", "fill", "click", "drag" };
	const char *brushes[] = { "ellipse", "rectangle", "triangle", "ellipse-perfect" };
	const char *resets[] = { "air", "pressure", "velocity", "sparks", "ambientHeat", "temperature", "invertAir" };
	const char *signTexts[] = { "{b|go}", "{p} {t}", "{c:1|x}", "hello" };
	const char *consoleCmds[] = { "create", "delete", "bubble" };
	const char *selectors[] = { "all", "type", "point", "index" };
	int scenes = 0;
	auto only = std::getenv("BUGHUNT_RUN"); // replay one run, logging its commands
	for (int run = only ? std::atoi(only) : shard; run < 30000; run += of)
	{
		if (only && run != std::atoi(only))
			break;
		static const int seedOffset = std::getenv("BUGHUNT_SEED") ? std::atoi(std::getenv("BUGHUNT_SEED")) : 0;
		RNG r; r.seed((run + seedOffset) * 7919 + 1);
		auto pick = [&](int n) { return int(r.gen() % unsigned(n)); };
		auto element = [&] { return elements[pick(int(elements.size()))]; };
		World w;
		ApplySetting(*w.sim, pick(17));
		if (pick(3) == 0) ApplySetting(*w.sim, pick(17));
		int kinds = 2 + pick(10);
		for (int k = 0; k < kinds; ++k)
		{
			int e = element();
			int x = CELL + pick(XRES - 2 * CELL - 60), y = CELL + pick(YRES - 2 * CELL - 40);
			Fill(*w.sim, e, x, y, x + 5 + pick(55), y + 3 + pick(35), pick(4) == 0 ? float(pick(9999)) : -1.f);
		}
		w.sim->BeforeSim(false);
		std::string stamp;
		std::string scene = "chaos run " + std::to_string(run);
		uint64_t seq = 0;
		for (int t = 0; t < ticks; ++t)
		{
			if (pick(4) == 0)
			{
				Json::Value v;
				int kind = pick(16);
				int x1 = CELL + pick(XRES - 2 * CELL), y1 = CELL + pick(YRES - 2 * CELL);
				int x2 = std::clamp(x1 + pick(81) - 40, CELL, XRES - CELL - 1), y2 = std::clamp(y1 + pick(81) - 40, CELL, YRES - CELL - 1);
				v["x1"] = x1; v["y1"] = y1; v["x2"] = x2; v["y2"] = y2;
				switch (kind)
				{
				case 0: case 1: v["op"] = "brush"; v["radius"] = pick(8); v["element"] = pick(5) ? element() : 0; break;
				case 2: case 3: case 4:
					v["op"] = "tool"; v["tool"] = tools[pick(int(tools.size()))]; v["action"] = actions[pick(7)]; v["brush"] = brushes[pick(4)];
					v["radius"] = pick(12); v["radiusY"] = pick(12); v["strength"] = 1 + pick(1000); v["flags"] = pick(4);
					v["replace"] = element(); v["shift"] = pick(2) == 1; v["ctrl"] = pick(2) == 1; v["alt"] = pick(2) == 1;
					v["colour"] = Json::UInt(r.gen()); v["property"] = pick(int(properties.size())); v["value"] = pick(3) ? pick(200) : int(r.gen());
					break;
				case 5: case 6:
				{
					int property = pick(int(properties.size()));
					v["op"] = "console"; v["cmd"] = "set"; v["property"] = property;
					if (property == int(FIELD_TYPE)) v["value"] = element();
					else if (properties[property].Type == StructProperty::Float) v["value"] = double(pick(20000)) - 5000;
					else if (properties[property].Type == StructProperty::UInteger) v["value"] = Json::UInt(r.gen());
					else v["value"] = pick(4) ? pick(300) : int(r.gen());
					v["selector"] = selectors[pick(4)]; v["element"] = element(); v["index"] = pick(NPART);
					break;
				}
				case 7: v["op"] = "console"; v["cmd"] = consoleCmds[pick(3)]; v["element"] = element() | (pick(4) == 0 ? (pick(300) << PMAPBITS) : 0); break;
				case 8: v["op"] = "reset"; v["what"] = resets[pick(7)]; break;
				case 9:
				{
					int index = w.sim->signs.empty() || pick(2) ? -1 : pick(int(w.sim->signs.size()));
					v["op"] = "sign"; v["index"] = index;
					if (index >= 0) { v["x2"] = w.sim->signs[index].x; v["y2"] = w.sim->signs[index].y; v["remove"] = pick(2) == 1; }
					v["text"] = signTexts[pick(4)]; v["justification"] = pick(4);
					break;
				}
				case 10: v["op"] = "sign_button"; v["index"] = 0;
					if (!w.sim->signs.empty()) { v["x1"] = w.sim->signs[0].x; v["y1"] = w.sim->signs[0].y; }
					break;
				case 11: v["op"] = "stickman_spawn"; v["slot"] = 1 + pick(4); break;
				case 12: v["op"] = "stickman"; v["slot"] = 1 + pick(4); v["comm"] = pick(16); v["pcomm"] = pick(16); break;
				case 13:
					if (stamp.empty() || pick(3) == 0)
					{
						auto save = w.sim->Save(true, RectBetween(Vec2{ x1, y1 }, Vec2{ x2, y2 }));
						auto bytes = save->Serialise().second;
						stamp.assign(bytes.begin(), bytes.end());
					}
					if (pick(6) == 0) { v["op"] = "load"; v["save"] = Base64Encode(stamp); break; } // the stamp replaces the world
					v["op"] = "paste"; v["save"] = Base64Encode(stamp); v["x1"] = pick(XCELLS) - 5; v["y1"] = pick(YCELLS) - 5; v["pressure"] = pick(2) == 1;
					break;
				case 14:
				{
					v["op"] = "settings";
					Json::Value s;
					switch (pick(8))
					{
					case 0: s["gravityMode"] = pick(NUM_GRAVMODES); break;
					case 1: s["airMode"] = pick(NUM_AIRMODES); break;
					case 2: s["edgeMode"] = pick(NUM_EDGEMODES); break;
					case 3: s["newtonianGravity"] = pick(2) == 1; break;
					case 4: s["heat"] = pick(2) == 1; s["ambientHeat"] = pick(2) == 1; break;
					case 5: s["customGravityX"] = double(pick(2001)) - 1000; s["customGravityY"] = double(pick(21)) - 10; break;
					case 6: s["convectionMode"] = pick(NUM_CONVMODES); s["waterEqualisation"] = pick(2) == 1; break;
					case 7: s["vorticityCoeff"] = pick(101) / 100.0; s["edgePressure"] = double(pick(513)) - 256; break;
					}
					v["settings"] = s;
					break;
				}
				case 15: v["op"] = pick(2) ? "pause" : "step"; v["paused"] = pick(3) != 0; break;
				}
				v["seq"] = Json::UInt64(++seq);
				Command c;
				bool parsed = true;
				try { c = ParseCommand(v, true); if (c.kind == Command::Paste || c.kind == Command::Load) CheckPastedSave(c); }
				catch (const std::exception &) { parsed = false; }
				if (only)
					std::cerr << "tick " << t << " " << (parsed ? "" : "(refused) ") << JsonText(v).substr(0, 300) << std::endl;
				if (parsed)
				{
					try { w.Apply(c); }
					catch (const std::exception &e)
					{
						// A command that passes validation may still be refused (a step while
						// running); anything else is a bug.
						std::string what = e.what();
						if (what != "pause before stepping" && what != "no stickman slot")
							Report("apply-threw", v["op"].asString(), scene + " tick " + std::to_string(t) + ": " + what);
					}
				}
			}
			if (!w.paused) w.Advance();
			CheckWorld(w, scene, t);
			if (t % 150 == 149)
			{
				World copy;
				RestoreExactCheckpoint(copy, EncodeExactCheckpoint(w));
				for (int k = 0; k < 10; ++k) { w.Advance(); copy.Advance(); }
				auto da = DiagnosticDigests(w), db = DiagnosticDigests(copy);
				if (da != db)
				{
					std::string groups;
					for (auto &g : da.getMemberNames()) if (da[g] != db[g]) groups += g + " ";
					Report("snapshot-divergence", scene, "tick " + std::to_string(t) + ": " + groups);
				}
				Render(w);
				auto bytes = w.CaptureSave()->Serialise().second;
				try
				{
					GameSave back(bytes, false);
					World loaded;
					loaded.sim->Load(&back, true, { 0, 0 });
					loaded.sim->BeforeSim(false);
					CheckWorld(loaded, scene + " (loaded)", t);
				}
				catch (const std::exception &e) { Report("save-roundtrip", scene, e.what()); }
			}
		}
		++scenes;
	}
	std::cout << "shard " << shard << "/" << of << ": " << scenes << " scenes, " << issues << " issues" << std::endl;
	return 0;
}

// A room restart: the autosave (a normal save) loaded into a new world must hold
// what the room held. Particles are matched by position and type (a save may
// renumber them); settings, walls, air and stickmen are compared too.
int Restart(int shard, int of, int ticks)
{
	auto &sd = SimulationData::CRef();
	auto &properties = Particle::GetProperties();
	int index = 0, scenes = 0;
	for (int element = 1; element < PT_NUM; ++element)
	{
		if (!sd.elements[element].Enabled || index++ % of != shard)
			continue;
		World a;
		auto &sim = *a.sim;
		// Room settings, rotated across elements.
		switch (element % 5)
		{
		case 1: sim.gravityMode = GRAV_CUSTOM; sim.customGravityX = .25f; sim.customGravityY = -.5f; sim.air->vorticityCoeff = .5f; break;
		case 2: sim.air->airMode = AIR_PRESSUREOFF; sim.SetEdgeMode(EDGE_SOLID); sim.aheat_enable = 1; sim.air->ambientAirTemp = 400; break;
		case 3: sim.EnableNewtonianGravity(true); sim.air->edgePressure = 3; sim.air->edgeVelocityX = 1; sim.air->convectionMode = AIRC_BOUSSINESQ; break;
		case 4: sim.legacy_enable = 1; sim.water_equal_test = 1; sim.SetEdgeMode(EDGE_LOOP); break;
		}
		Fill(sim, PT_DMND, 150, 280, 330, 283);
		Fill(sim, element, 200, 230, 279, 279);
		Fill(sim, PT_METL, 180, 270, 199, 279);
		sim.create_part(-1, 190, 270, PT_SPRK);
		sim.signs.push_back(sign("restart {b|test}", 100, 100, sign::Middle));
		for (int x = 50; x < 70; x += CELL) sim.bmap[50 / CELL][x / CELL] = WL_FAN, sim.fvx[50 / CELL][x / CELL] = .5f;
		sim.BeforeSim(false);
		for (int t = 0; t < ticks; ++t) a.Advance();
		a.paused = true;
		auto bytes = a.CaptureSave()->Serialise().second;
		auto path = "bughunt-restart-" + std::to_string(shard) + ".cps";
		{ std::ofstream out(path, std::ios::binary); out.write(bytes.data(), bytes.size()); }
		World b;
		b.Load(path);
		auto &back = *b.sim;
		auto scene = "restart " + Name(element) + " settings " + std::to_string(element % 5);
		CheckWorld(b, scene, 0);
		// Particles, by (x, y, type) in index order on both sides.
		std::map<std::tuple<int, int, int>, std::vector<const Particle *>> before, after;
		for (int i = 0; i < NPART; ++i)
		{
			if (sim.parts[i].type) before[{ int(sim.parts[i].x + .5f), int(sim.parts[i].y + .5f), sim.parts[i].type }].push_back(&sim.parts[i]);
			if (back.parts[i].type) after[{ int(back.parts[i].x + .5f), int(back.parts[i].y + .5f), back.parts[i].type }].push_back(&back.parts[i]);
		}
		std::map<std::string, int> lost;
		for (auto &[key, list] : before)
		{
			auto it = after.find(key);
			if (it == after.end() || it->second.size() != list.size()) { ++lost["particle missing"]; continue; }
			for (size_t k = 0; k < list.size(); ++k)
			{
				auto &p = *list[k], &q = *it->second[k];
				for (size_t f = 0; f < properties.size(); ++f)
				{
					auto &prop = properties[f];
					if (prop.Name == "x" || prop.Name == "y") continue;
					auto *pa = reinterpret_cast<const char *>(&p) + prop.Offset, *pb = reinterpret_cast<const char *>(&q) + prop.Offset;
					bool same;
					if (prop.Type == StructProperty::Float)
					{
						float fa = *reinterpret_cast<const float *>(pa), fb = *reinterpret_cast<const float *>(pb);
						same = std::abs(fa - fb) <= (prop.Name == "temp" ? 1.f : .5f);
					}
					else same = !std::memcmp(pa, pb, 4);
					if (!same) ++lost[std::string(prop.Name) + " " + Name(std::get<2>(key))];
				}
			}
		}
		for (auto &[what, count] : lost)
			Report("restart-loses", what, scene + " (" + std::to_string(count) + " particles)");
		auto compare = [&](const char *what, bool same) { if (!same) Report("restart-loses", what, scene); };
		compare("gravityMode", sim.gravityMode == back.gravityMode);
		compare("customGravity", sim.customGravityX == back.customGravityX && sim.customGravityY == back.customGravityY);
		compare("airMode", sim.air->airMode == back.air->airMode);
		compare("ambientAirTemp", sim.air->ambientAirTemp == back.air->ambientAirTemp);
		compare("edgePressure", sim.air->edgePressure == back.air->edgePressure);
		compare("edgeVelocity", sim.air->edgeVelocityX == back.air->edgeVelocityX && sim.air->edgeVelocityY == back.air->edgeVelocityY);
		compare("vorticityCoeff", sim.air->vorticityCoeff == back.air->vorticityCoeff);
		compare("convectionMode", sim.air->convectionMode == back.air->convectionMode);
		compare("edgeMode", sim.edgeMode == back.edgeMode);
		compare("heat (legacy)", sim.legacy_enable == back.legacy_enable);
		compare("waterEqualisation", sim.water_equal_test == back.water_equal_test);
		compare("ambientHeat", sim.aheat_enable == back.aheat_enable);
		compare("newtonianGravity", bool(sim.grav) == bool(back.grav));
		compare("paused", a.paused == b.paused);
		compare("signs", sim.signs.size() == back.signs.size() && (sim.signs.empty() || sim.signs[0].text == back.signs[0].text));
		compare("walls", !std::memcmp(sim.bmap, back.bmap, sizeof(sim.bmap)));
		compare("fan velocity", !std::memcmp(sim.fvx, back.fvx, sizeof(sim.fvx)));
		int air = 0;
		for (int y = 0; y < YCELLS; ++y) for (int x = 0; x < XCELLS; ++x) air += std::abs(sim.pv[y][x] - back.pv[y][x]) > .5f;
		compare("pressure", air == 0);
		std::remove(path.c_str());
		++scenes;
	}
	std::cout << "shard " << shard << "/" << of << ": " << scenes << " scenes, " << issues << " issues" << std::endl;
	return 0;
}
}

int main(int argc, char **argv)
{
	GlobalPrefs prefs;
	SimulationData data;
	std::string mode = argc > 1 ? argv[1] : "pairs";
	int shard = 0, of = 1, ticks = 40;
	for (int i = 2; i + 1 < argc; i += 2)
	{
		std::string a = argv[i];
		if (a == "--shard") shard = std::stoi(argv[i + 1]);
		else if (a == "--of") of = std::stoi(argv[i + 1]);
		else if (a == "--ticks") ticks = std::stoi(argv[i + 1]);
	}
	try
	{
		if (mode == "pairs") return Pairs(shard, of, ticks);
		if (mode == "actions") return Actions(shard, of, ticks);
		if (mode == "tools") return Tools(shard, of, ticks);
		if (mode == "restart") return Restart(shard, of, ticks);
		if (mode == "paste") return Paste(shard, of, ticks);
		if (mode == "settings") return Settings(shard, of, ticks);
		if (mode == "stickmen") return Stickmen(shard, of, ticks);
		if (mode == "signs") return Signs(shard, of, ticks);
		if (mode == "digests") return Digests(shard, of, ticks);
		if (mode == "chaos") return Chaos(shard, of, ticks);
		std::cerr << "unknown mode " << mode << '\n';
		return 2;
	}
	catch (const std::exception &e)
	{
		std::cerr << "bughunt error: " << e.what() << '\n';
		return 1;
	}
}
