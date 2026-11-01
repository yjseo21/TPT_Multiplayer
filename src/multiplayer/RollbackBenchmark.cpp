// Feasibility measurements for client-side prediction with rollback: physics
// step cost, state capture/restore cost, and whether a restored state replays
// to the identical fingerprint as an uninterrupted run.
#include "ExactCheckpoint.h"
#include "StateDigest.h"
#include "World.h"
#include "prefs/GlobalPrefs.h"
#include "simulation/ElementClasses.h"
#include "simulation/Simulation.h"
#include "simulation/SimulationData.h"
#include "simulation/Snapshot.h"
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>

using namespace multiplayer;
using Clock = std::chrono::steady_clock;
namespace
{
double Ms(Clock::time_point begin) { return std::chrono::duration<double, std::milli>(Clock::now()-begin).count(); }
template<class F> double Mean(int n, F f)
{
    auto begin = Clock::now();
    for (int i = 0; i < n; ++i) f();
    return Ms(begin)/n;
}
void Fill(World &w, int element, int count, int x0, int y0, int x1, int y1)
{
    int made = 0;
    for (int y = y0; y < y1 && made < count; ++y)
        for (int x = x0; x < x1 && made < count; ++x)
            if (w.sim->create_part(-1, x, y, element) >= 0) ++made;
}
std::string Digest(const World &w) { return DigestSummary(DiagnosticDigests(w)); }
}
int main()
{
    GlobalPrefs prefs;
    SimulationData data;
    struct Scene { const char *name; std::function<void(World &)> build; };
    std::vector<Scene> scenes = {
        {"water-30k", [](World &w) { Fill(w, PT_WATR, 30000, 100, 40, 500, 200); }},
        {"fire-15k", [](World &w) { Fill(w, PT_WOOD, 20000, 100, 250, 500, 330); Fill(w, PT_FIRE, 15000, 100, 200, 500, 250); }},
        {"mixed-60k", [](World &w) {
            Fill(w, PT_METL, 4000, 40, 340, 570, 350);
            Fill(w, PT_WATR, 20000, 60, 60, 300, 160); Fill(w, PT_LAVA, 8000, 320, 60, 560, 100);
            Fill(w, PT_PLNT, 8000, 60, 250, 300, 300); Fill(w, PT_GUNP, 8000, 320, 250, 560, 300);
            Fill(w, PT_DUST, 12000, 60, 170, 560, 200); }},
        {"dust-180k", [](World &w) { Fill(w, PT_DUST, 180000, CELL+1, CELL+1, XRES-CELL-1, YRES-CELL-1); }},
    };
    std::cout << "scene,particles,tick_ms,snapshot_ms,restore_ms,checkpoint_encode_ms,checkpoint_restore_ms,"
                 "snapshot_replay_exact,checkpoint_replay_exact\n";
    for (auto &scene : scenes)
    {
        World world;
        world.sim->ensureDeterminism = true;
        world.sim->rng.seed(4242);
        scene.build(world);
        world.sim->BeforeSim(false);
        for (int i = 0; i < 30; ++i) world.Advance(); // let it start moving
        int particles = 0;
        for (int i = 0; i < NPART; ++i) if (world.sim->parts[i].type) ++particles;
        double tick = Mean(60, [&] { world.Advance(); });

        std::unique_ptr<Snapshot> snap;
        double snapshot = Mean(20, [&] { snap = world.sim->CreateSnapshot(); });
        double restore = Mean(20, [&] { world.sim->Restore(*snap); });
        std::string bytes;
        double encode = Mean(10, [&] { bytes = EncodeExactCheckpoint(world); });
        double decode = Mean(10, [&] { RestoreExactCheckpoint(world, bytes); });

        // Replay exactness: capture, run 60 ticks, rewind, run again, compare.
        constexpr int replay = 60;
        auto exact = [&](auto capture, auto rewind) {
            auto baseTick = world.tick;
            auto state = capture();
            for (int i = 0; i < replay; ++i) world.Advance();
            auto expected = Digest(world);
            rewind(state); world.tick = baseTick;
            for (int i = 0; i < replay; ++i) world.Advance();
            return Digest(world) == expected;
        };
        bool snapshotExact = exact([&] { return world.sim->CreateSnapshot(); },
            [&](const std::unique_ptr<Snapshot> &s) { world.sim->Restore(*s); world.sim->BeforeSim(false); });
        bool checkpointExact = exact([&] { return EncodeExactCheckpoint(world); },
            [&](const std::string &s) { RestoreExactCheckpoint(world, s); });
        std::cout << std::fixed << std::setprecision(3) << scene.name << ',' << particles << ',' << tick << ','
                  << snapshot << ',' << restore << ',' << encode << ',' << decode << ','
                  << (snapshotExact ? "yes" : "NO") << ',' << (checkpointExact ? "yes" : "NO") << std::endl;
    }
}
