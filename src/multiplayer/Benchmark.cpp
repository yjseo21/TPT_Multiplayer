#include "World.h"
#include "ExactCheckpoint.h"
#include "StateDigest.h"
#include "simulation/gravity/Gravity.h"
#include "Canvas.h"
#include "simulation/SimulationData.h"
#include "simulation/ElementClasses.h"
#include "prefs/GlobalPrefs.h"
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace multiplayer;
using Clock = std::chrono::steady_clock;
namespace
{
void Fill(Simulation &sim, int x0, int y0, int x1, int y1, int element)
{
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            sim.create_part(-1, x, y, element);
}
// Heavy rooms: explosions and full screens, timed tick by tick (the slowest
// ticks are the ones players feel).
const std::vector<std::pair<std::string, void (*)(Simulation &)>> heavyScenes = {
    { "c4-block", [](Simulation &s) { Fill(s, 100, 200, 500, 380, PT_PLEX); Fill(s, 98, 200, 100, 380, PT_FIRE); } },
    { "nitro-pool", [](Simulation &s) { Fill(s, 50, 250, 560, 380, PT_NITR); Fill(s, 50, 240, 560, 242, PT_FIRE); } },
    { "gunpowder", [](Simulation &s) { Fill(s, 20, 150, 590, 380, PT_GUNP); Fill(s, 300, 140, 310, 145, PT_FIRE); } },
    { "bomb-rain", [](Simulation &s) { Fill(s, 10, 250, 600, 380, PT_SAND); for (int x = 20; x < 590; x += 6) Fill(s, x, 20, x+2, 30, PT_BOMB); } },
    { "thunder-sea", [](Simulation &s) { Fill(s, 10, 200, 600, 380, PT_WATR); for (int x = 20; x < 590; x += 12) Fill(s, x, 30, x+3, 33, PT_THDR); } },
    { "full-mix", [](Simulation &s) {
        Fill(s, 4, 4, 608, 100, PT_WATR); Fill(s, 4, 100, 608, 200, PT_SAND);
        Fill(s, 4, 200, 608, 300, PT_OIL); Fill(s, 4, 300, 608, 380, PT_LAVA); } },
};
int HeavyScenes(const std::string &only, int ticks)
{
    std::cout << "scene,start_particles,peak_particles,mean_tick_ms,p95_tick_ms,max_tick_ms,digest\n";
    for (auto &[name, build] : heavyScenes)
    {
        if (!only.empty() && only != name)
            continue;
        World world;
        world.sim->rng.seed(12345);
        build(*world.sim);
        world.sim->BeforeSim(false);
        auto start = world.sim->NUM_PARTS;
        int peak = start;
        std::vector<double> times;
        for (int tick = 0; tick < ticks; ++tick)
        {
            auto begin = Clock::now();
            world.Advance();
            times.push_back(std::chrono::duration<double, std::milli>(Clock::now()-begin).count());
            peak = std::max(peak, world.sim->NUM_PARTS);
        }
        auto sorted = times;
        std::sort(sorted.begin(), sorted.end());
        double mean = 0;
        for (auto t : times) mean += t;
        std::cout << name << ',' << start << ',' << peak << ',' << std::fixed << std::setprecision(2)
            << mean/ticks << ',' << sorted[ticks*95/100] << ',' << sorted.back() << ','
            << DigestSummary(DiagnosticDigests(world)) << std::endl;
    }
    return 0;
}
}
int main(int argc, char **argv)
{
    GlobalPrefs prefs;
    SimulationData data;
    // tpt-benchmark --heavy [scene] [ticks]: explosion and full-screen scenes only.
    if (argc > 1 && std::string(argv[1]) == "--heavy")
        return HeavyScenes(argc > 2 ? argv[2] : "", argc > 3 ? std::stoi(argv[3]) : 300);
    std::cout << "scene,particles,mean_tick_ms,mean_encode_ms,mean_decode_ms,mean_frame_bytes,state_mbps_at_20hz,canvas_mbps_at_20hz,canvas_render_ms,canvas_delta_ms\n";
    for (auto [name, count, element] : {
        std::tuple{"static-metal", 6000, PT_METL},
        std::tuple{"moving-water", 30000, PT_WATR},
        std::tuple{"dense-dust", 180000, PT_DUST},
        std::tuple{"fire", 15000, PT_FIRE},
    })
    {
        World world;
        Renderer renderer;
        world.sim->rng.seed(12345);
        ReceivedCanvas previous;
        int created = 0;
        for (int y = CELL+1; y < YRES-CELL-1 && created < count; ++y)
            for (int x = CELL+1; x < XRES-CELL-1 && created < count; ++x)
                if (world.sim->create_part(-1, x, y, element) >= 0) ++created;
        world.sim->BeforeSim(false);
        double tickMs = 0, encodeMs = 0, decodeMs = 0;
        size_t bytes = 0;
        size_t canvasBytes = 0;
        double renderMs = 0, deltaMs = 0;
        constexpr int steps = 120, samples = steps/3;
        for (int tick = 0; tick < steps; ++tick)
        {
            auto begin = Clock::now(); world.Advance();
            tickMs += std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
            if (tick % 3 == 0)
            {
                begin = Clock::now();
                auto frame = EncodeFrame(*world.sim, world.tick, world.revision, false);
                encodeMs += std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
                bytes += frame.size();
                begin = Clock::now();
                auto decoded = DecodeFrame(frame);
                decodeMs += std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
                begin = Clock::now();
                auto canvas = RenderCanvas(renderer, *world.sim, tick+1, world.tick, 0, false, 0, previous.frame.pixels);
                renderMs += std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
                begin = Clock::now();
                auto packet = EncodeCanvas(canvas, tick+1, previous.id ? &previous : nullptr);
                deltaMs += std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
                canvasBytes += packet.size();
                auto decodedCanvas = DecodeCanvas(packet, previous.id ? &previous : nullptr);
                if (decodedCanvas.frame.pixels->planes != canvas.pixels->planes)
                    throw std::runtime_error("benchmark canvas reconstruction mismatch");
                previous = {uint64_t(tick+1), std::move(canvas)};
            }
        }
        auto meanBytes = bytes / samples;
        std::cout << name << ',' << created << ',' << std::fixed << std::setprecision(2)
            << tickMs/steps << ',' << encodeMs/samples << ',' << decodeMs/samples << ',' << meanBytes << ','
            << meanBytes*20.0*8/1000000 << ',' << (canvasBytes/double(samples))*20*8/1000000 << ',' << renderMs/samples << ',' << deltaMs/samples << std::endl;
        // Join snapshot: the capture runs on the room's thread, compression does not.
        constexpr int captures = 5;
        double captureMs = 0, compressMs = 0;
        size_t rawBytes = 0, packetBytes = 0;
        std::string reused;
        for (int i = 0; i < captures; ++i)
        {
            auto begin = Clock::now();
            auto raw = CaptureExactCheckpointRaw(world, std::move(reused));
            captureMs += std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
            begin = Clock::now();
            auto packet = CompressExactCheckpointRaw(raw);
            compressMs += std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
            rawBytes = raw.size(); packetBytes = packet.size();
            reused = std::move(raw);
        }
        // Room gravity: a full solve per tick (the mass moves every tick).
        {
            auto gravity = Gravity::CreateDeterministic();
            GravityInput in;
            GravityOutput out;
            constexpr int solves = 20;
            double solveMs = 0;
            for (int i = 0; i < solves; ++i)
            {
                in.mass[{ (i * 7) % CELLS.X, (i * 3) % CELLS.Y }] += 1.f;
                auto begin = Clock::now();
                gravity->Exchange(out, in, false);
                solveMs += std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
            }
            std::cout << "  room gravity: solve_ms=" << solveMs/solves << std::endl;
        }
        std::cout << "  snapshot: capture_ms=" << captureMs/captures << " compress_ms=" << compressMs/captures
            << " raw_bytes=" << rawBytes << " packet_bytes=" << packetBytes << std::endl;
        // The fingerprint every 60 ticks, on the server and on every player's computer.
        double digestMs = 0;
        for (int i = 0; i < 5; ++i)
        {
            auto start = std::chrono::steady_clock::now();
            auto summary = DigestSummary(DiagnosticDigests(world));
            digestMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
            if (summary.empty()) throw std::runtime_error("empty digest");
        }
        std::cout << "  fingerprint: digest_ms=" << digestMs/5 << std::endl;
    }
}
