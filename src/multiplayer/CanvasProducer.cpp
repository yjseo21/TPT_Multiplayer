#include "CanvasProducer.h"
#include "Metrics.h"
#include <utility>

namespace multiplayer
{
CanvasProducer::CanvasProducer(int quality, int scale) : webpQuality(quality), webpScale(scale)
{
    // Populate shared renderer tables before starting a secondary thread.
    for (auto &renderer : renderers) renderer = std::make_unique<Renderer>();
    worker = std::thread([this] {
        try
        {
            CanvasSet previous{};
            std::array<uint64_t, CanvasViews> previousGeneration{};
            for (;;)
            {
                std::unique_ptr<Snapshot> job;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return stopping || pending; });
                    if (stopping) break;
                    job = std::move(pending);
                }
                auto output = std::make_shared<CanvasSet>(previous);
                for (unsigned view = 0; view < CanvasViews; ++view) if (job->views & (1U << view))
                {
                    // Clear accumulation after a world reset (tick metadata remains monotonic).
                    if (job->resetGeneration != previousGeneration[view])
                        renderers[view]->ClearAccumulation();
                    previousGeneration[view] = job->resetGeneration;
                    auto frame = RenderCanvas(*renderers[view], *job->world,
                        job->capture, job->tick, job->revision, job->paused, uint8_t(view),
                        previous[view] ? previous[view]->pixels : nullptr, webpQuality, webpScale);
                    frame.capturedUs = job->capturedUs;
                    (*output)[view] = std::make_shared<const CanvasFrame>(std::move(frame));
                }
                previous = *output;
                { std::lock_guard lock(mutex); latest = std::move(output); }
            }
        }
        catch (...) { std::lock_guard lock(mutex); failure = std::current_exception(); }
    });
}
CanvasProducer::~CanvasProducer()
{
    { std::lock_guard lock(mutex); stopping = true; pending.reset(); }
    changed.notify_one();
    if (worker.joinable()) worker.join();
}
void CanvasProducer::Submit(const RenderableSimulation &sim, uint64_t capture, uint64_t tick, uint64_t revision, bool paused, unsigned views, uint64_t resetGeneration)
{
    auto snapshot = std::make_unique<Snapshot>();
    snapshot->capturedUs = MonotonicMicros();
    snapshot->world = std::make_unique<RenderableSimulation>();
    *snapshot->world = sim;
    snapshot->capture = capture; snapshot->tick = tick; snapshot->revision = revision;
    snapshot->paused = paused; snapshot->views = views;
    snapshot->resetGeneration = resetGeneration;
    { std::lock_guard lock(mutex); pending = std::move(snapshot); }
    changed.notify_one();
}
std::shared_ptr<const CanvasSet> CanvasProducer::Latest()
{
    std::lock_guard lock(mutex);
    if (failure) std::rethrow_exception(failure);
    return latest;
}
}
