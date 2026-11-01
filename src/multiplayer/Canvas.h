#pragma once
#include "graphics/Renderer.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>

namespace multiplayer
{
constexpr size_t CanvasPixels = XRES * YRES;
constexpr size_t CanvasBytes = CanvasPixels * 3;
constexpr size_t CanvasHeaderBytes = 52;
constexpr size_t MaxCanvasPacket = CanvasBytes + 4096;
constexpr unsigned CanvasViews = 3;

struct CanvasPixelsData
{
    // B, G, R byte planes compress much better than interleaved float particle fields.
    std::string planes;
    std::string compressed;
    // Optional one-time lossy WebP encoding. Immutable and shared across every viewer
    // of this renderer snapshot; it is never used as a simulation state baseline.
    std::string webp;
    uint32_t webpChecksum = 0;
    int webpQuality = 0, webpScale = 1;
    uint32_t checksum = 0;
};
struct CanvasFrame
{
    uint64_t capture = 0, tick = 0, revision = 0;
    uint8_t view = 0;
    bool paused = false;
    std::shared_ptr<const CanvasPixelsData> pixels;
    uint64_t capturedUs = 0;
    bool lossy = false;
};
struct ReceivedCanvas
{
    uint64_t id = 0;
    CanvasFrame frame;
};
struct MissingCanvasBase : std::runtime_error
{
    MissingCanvasBase() : std::runtime_error("missing canvas baseline") {}
};

bool SupportsWebP();
std::shared_ptr<const CanvasPixelsData> PackCanvas(std::string planes, int webpQuality = 0, int webpScale = 1);
CanvasFrame RenderCanvas(Renderer &renderer, const RenderableSimulation &world,
    uint64_t capture, uint64_t tick, uint64_t revision, bool paused, uint8_t view,
    std::shared_ptr<const CanvasPixelsData> previous = {}, int webpQuality = 0, int webpScale = 1,
    bool forNetwork = true);
// forNetwork=false skips compression (local display): planes and checksum only.
// WebP is opt-in per connection. If unavailable, use original lossless modes 0-2.
std::string EncodeCanvas(const CanvasFrame &frame, uint64_t id, const ReceivedCanvas *base = nullptr,
    bool allowWebP = false);
ReceivedCanvas DecodeCanvas(std::string_view packet, const ReceivedCanvas *base = nullptr);
std::vector<pixel> CanvasVideo(const CanvasFrame &frame);

// A burst accommodates one maximum-size keyframe; sustained bytes are rate limited.
class ByteBudget
{
    double rate, capacity, tokens, last;
public:
    ByteBudget(double bytesPerSecond, double now);
    double Delay(size_t bytes, double now);
    void Spend(size_t bytes, double now);
    void SetRate(double bytesPerSecond, double now);
};
}
