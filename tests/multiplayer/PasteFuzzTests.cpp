// Pasted saves are the most complex thing a stranger can send a room: a
// compressed file parsed by the game's save loader, then loaded into the world.
// This fuzzes that path as the server runs it. A real save (every element,
// walls, signs, air and gravity maps) is unpacked, its contents corrupted in
// many ways, packed again with a correct header, and sent through the same
// checks as a player's paste: ParseCommand (format and size), CheckPastedSave
// (the full parse) and, when accepted, applied to two worlds, which must stay
// identical. Nothing may crash; build with sanitizers to catch memory errors.
//
// Usage: tpt-paste-fuzz [CASES [SEED]]
#include "multiplayer/World.h"
#include "multiplayer/Protocol.h"
#include "multiplayer/CommandRecord.h"
#include "multiplayer/StateDigest.h"
#include "simulation/ElementClasses.h"
#include "simulation/SimulationData.h"
#include "client/GameSave.h"
#include "bzip2/bz2wrap.h"
#include "prefs/GlobalPrefs.h"
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace multiplayer;

namespace
{
std::vector<char> BaseSave()
{
	World w;
	auto &sim = *w.sim;
	auto &sd = SimulationData::CRef();
	int n = 0;
	for (int type = 1; type < PT_NUM; ++type)
		if (sd.elements[type].Enabled)
		{
			int x = 20 + (n % 30) * 18, y = 20 + (n / 30) * 18;
			for (int dy = 0; dy < 8; ++dy)
				for (int dx = 0; dx < 8; ++dx)
					sim.create_part(-1, x + dx, y + dy, type);
			++n;
		}
	for (int x = 2; x < 100; ++x)
		sim.bmap[80][x] = WL_FAN;
	sim.signs.emplace_back("fuzz {t}", 300, 200, sign::Middle);
	sim.EnableNewtonianGravity(true);
	for (int i = 0; i < 20; ++i)
		w.Advance(); // live air, heat and gravity maps
	auto [missing, data] = sim.Save(true, RectBetween(Vec2{ 0, 0 }, Vec2{ XRES - 1, YRES - 1 }))->Serialise();
	if (missing || data.size() < 12 || std::string(data.data(), 4) != "OPS1")
		throw std::runtime_error("cannot build the base save");
	return data;
}

std::string Pack(std::vector<char> header, const std::vector<char> &contents)
{
	std::vector<char> packed;
	if (BZ2WCompress(packed, contents) != BZ2WCompressOk)
		throw std::runtime_error("cannot compress");
	uint32_t size = uint32_t(contents.size());
	for (int i = 0; i < 4; ++i)
		header[8 + i] = char(size >> (8 * i));
	std::string out(header.begin(), header.end());
	return out + std::string(packed.begin(), packed.end());
}
}

int main(int argc, char **argv)
{
	try
	{
		GlobalPrefs prefs;
		SimulationData data;
		int cases = argc > 1 ? std::stoi(argv[1]) : 600;
		std::mt19937 rng(argc > 2 ? std::stoul(argv[2]) : 20261102);
		auto base = BaseSave();
		std::vector<char> header(base.begin(), base.begin() + 12), contents;
		if (BZ2WDecompress(contents, std::span(base.data() + 12, base.size() - 12)) != BZ2WDecompressOk)
			throw std::runtime_error("cannot unpack the base save");
		std::cout << "Base save: " << base.size() << " bytes packed, " << contents.size() << " unpacked\n";
		auto pick = [&](size_t n) { return size_t(rng() % std::max<size_t>(n, 1)); };
		int rejectedEarly = 0, rejectedParse = 0, applied = 0;
		for (int c = 0; c < cases; ++c)
		{
			auto mutated = contents;
			auto head = header;
			switch (c % 7)
			{
			case 0: // scattered byte flips
				for (int i = 0, k = 1 + int(pick(16)); i < k; ++i) mutated[pick(mutated.size())] ^= char(1 + pick(255));
				break;
			case 1: // boundary integers written over fields
				for (int i = 0, k = 1 + int(pick(6)); i < k; ++i)
				{
					static const int32_t values[] = { 0, -1, 1, INT32_MAX, INT32_MIN, 65535, 256, NPART, PT_NUM, -PT_NUM };
					int32_t v = values[pick(std::size(values))];
					auto at = pick(mutated.size() - 4);
					for (int b = 0; b < 4; ++b) mutated[at + b] = char(v >> (8 * b));
				}
				break;
			case 2: // truncated
				mutated.resize(pick(mutated.size()));
				break;
			case 3: // a block repeated or removed
			{
				auto at = pick(mutated.size()), length = pick(std::min<size_t>(4096, mutated.size() - at));
				if (rng() & 1) mutated.insert(mutated.begin() + at, mutated.begin() + at, mutated.begin() + at + length);
				else mutated.erase(mutated.begin() + at, mutated.begin() + at + length);
				break;
			}
			case 4: // header dimensions and versions
				head[4] = char(pick(256)); head[5] = char(rng() & 1 ? CELL : pick(256));
				head[6] = char(pick(256)); head[7] = char(pick(256));
				break;
			case 5: // a stretch of random bytes
			{
				auto at = pick(mutated.size());
				for (size_t i = at; i < std::min(mutated.size(), at + 1 + pick(200)); ++i) mutated[i] = char(rng());
				break;
			}
			default: // flips and a truncation together
				for (int i = 0; i < 4; ++i) mutated[pick(mutated.size())] ^= char(rng());
				mutated.resize(mutated.size() - pick(mutated.size() / 4));
				break;
			}
			Json::Value paste;
			paste["seq"] = 1; paste["op"] = "paste";
			paste["x1"] = int(pick(XCELLS)) - XCELLS / 2; paste["y1"] = int(pick(YCELLS)) - YCELLS / 2;
			paste["save"] = Base64Encode(Pack(head, mutated));
			Command command;
			try { command = ParseCommand(paste, false); }
			catch (const std::exception &) { ++rejectedEarly; continue; }
			try { CheckPastedSave(command); }
			catch (const std::exception &) { ++rejectedParse; continue; }
			// Accepted: two rooms apply it identically, and keep running.
			// As in a room: every copy starts from the same state, random generator included.
			World a, b;
			for (auto *w : { &a, &b })
			{
				w->sim->rng.seed(uint64_t(c) * 7919 + 1);
				w->Apply(command);
				for (int t = 0; t < 3; ++t) w->Advance();
			}
			auto da = DiagnosticDigests(a), db = DiagnosticDigests(b);
			if (da != db)
			{
				std::string groups;
				for (const auto &group : da.getMemberNames())
					if (da[group] != db[group]) groups += " " + group;
				throw std::runtime_error("a pasted save loaded differently into two rooms (case " + std::to_string(c) + "):" + groups);
			}
			++applied;
		}
		std::cout << "Paste fuzz passed: " << cases << " corrupted saves (" << rejectedEarly << " refused by format or size, "
		          << rejectedParse << " by the save loader, " << applied << " applied identically twice)\n";
		return 0;
	}
	catch (const std::exception &e)
	{
		std::cerr << "Paste fuzz error: " << e.what() << '\n';
		return 1;
	}
}
