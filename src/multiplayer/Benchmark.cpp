#include "World.h"
#include "ExactCheckpoint.h"
#include "simulation/gravity/Gravity.h"
#include "Canvas.h"
#include "simulation/SimulationData.h"
#include "simulation/ElementClasses.h"
#include "prefs/GlobalPrefs.h"
#include <chrono>
#include <iomanip>
#include <iostream>

using namespace multiplayer;
using Clock = std::chrono::steady_clock;
int main()
{
    GlobalPrefs prefs;
    SimulationData data;
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
    }
}
