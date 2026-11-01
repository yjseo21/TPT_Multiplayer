#pragma once
#include "Canvas.h"
#include "simulation/Simulation.h"
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace multiplayer
{
using CanvasSet = std::array<std::shared_ptr<const CanvasFrame>, CanvasViews>;

// Only the simulation thread calls Submit. The worker owns all Renderer instances.
// Pending snapshots are replaced, never queued; neither rendering nor encoding blocks physics.
class CanvasProducer
{
    struct Snapshot
    {
        std::unique_ptr<RenderableSimulation> world;
        uint64_t capture, tick, revision, resetGeneration, capturedUs;
        bool paused;
        unsigned views;
    };
    std::array<std::unique_ptr<Renderer>, CanvasViews> renderers;
    std::mutex mutex;
    std::condition_variable changed;
    std::unique_ptr<Snapshot> pending;
    std::shared_ptr<const CanvasSet> latest;
    std::exception_ptr failure;
    bool stopping = false;
    std::thread worker;
    int webpQuality, webpScale;
public:
    explicit CanvasProducer(int webpQuality = 0, int webpScale = 1);
    ~CanvasProducer();
    void Submit(const RenderableSimulation &sim, uint64_t capture, uint64_t tick, uint64_t revision, bool paused, unsigned views, uint64_t resetGeneration);
    std::shared_ptr<const CanvasSet> Latest();
};
}
