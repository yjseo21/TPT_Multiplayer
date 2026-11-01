#include "Canvas.h"
#include <zlib.h>
#include <algorithm>
#include <cmath>
#ifdef TPT_MP_WEBP
#include <webp/encode.h>
#include <webp/decode.h>
#endif

namespace multiplayer
{
namespace
{
std::string Compress(std::string_view raw)
{
    uLongf size = compressBound(uLong(raw.size()));
    std::string compressed(size, '\0');
    if (compress2(reinterpret_cast<Bytef *>(compressed.data()), &size,
        reinterpret_cast<const Bytef *>(raw.data()), uLong(raw.size()), 3) != Z_OK)
        throw std::runtime_error("canvas compression failed");
    compressed.resize(size);
    return compressed;
}
uint32_t Checksum(std::string_view data)
{
    return uint32_t(crc32(0, reinterpret_cast<const Bytef *>(data.data()), uInt(data.size())));
}
void Put(std::string &data, uint64_t n, unsigned count)
{
    while (count--) { data.push_back(char(n)); n >>= 8; }
}
uint64_t Get(std::string_view data, size_t offset, unsigned count)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < count; ++i) n |= uint64_t(uint8_t(data[offset+i])) << (8*i);
    return n;
}
}

bool SupportsWebP()
{
#ifdef TPT_MP_WEBP
    return true;
#else
    return false;
#endif
}
std::shared_ptr<const CanvasPixelsData> PackCanvas(std::string planes, int webpQuality, int webpScale)
{
    if (webpScale < 1 || webpScale > 3 || XRES % webpScale || YRES % webpScale)
        throw std::runtime_error("invalid WebP scale");
    if (webpQuality < 0 || webpQuality > 100)
        throw std::runtime_error("invalid WebP quality");
    if (webpQuality && !SupportsWebP())
        throw std::runtime_error("WebP support not built");
    if (planes.size() != CanvasBytes) throw std::runtime_error("invalid canvas size");
    auto pixels = std::make_shared<CanvasPixelsData>();
    pixels->checksum = Checksum(planes);
    pixels->compressed = Compress(planes);
#ifdef TPT_MP_WEBP
    if (webpQuality)
    {
        // Encode exactly once per captured view, never once per connected client.
        // TPT uses B,G,R planes; libwebp accepts interleaved RGB.
        const int width = XRES/webpScale, height = YRES/webpScale;
        std::string rgb(size_t(width)*height*3, '\0');
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        {
            unsigned r = 0, g = 0, b = 0;
            for (int dy=0; dy<webpScale; ++dy) for (int dx=0; dx<webpScale; ++dx)
            {
                size_t i = size_t(y*webpScale+dy)*XRES + x*webpScale+dx;
                b += uint8_t(planes[i]); g += uint8_t(planes[CanvasPixels+i]);
                r += uint8_t(planes[2*CanvasPixels+i]);
            }
            size_t i = (size_t(y)*width+x)*3;
            auto area = unsigned(webpScale*webpScale);
            rgb[i] = char((r+area/2)/area);
            rgb[i+1] = char((g+area/2)/area);
            rgb[i+2] = char((b+area/2)/area);
        }
        uint8_t *encoded = nullptr;
        size_t count = WebPEncodeRGB(reinterpret_cast<const uint8_t *>(rgb.data()),
            width, height, width*3, float(webpQuality), &encoded);
        if (!count || !encoded) throw std::runtime_error("WebP encoding failed");
        // Keep the lossless fallback for high-entropy or poorly compressible scenes.
        if (count < pixels->compressed.size() && count <= MaxCanvasPacket-CanvasHeaderBytes)
        {
            pixels->webp.assign(reinterpret_cast<const char *>(encoded), count);
            pixels->webpChecksum = Checksum(pixels->webp);
            pixels->webpQuality = webpQuality;
            pixels->webpScale = webpScale;
        }
        WebPFree(encoded);
    }
#endif
    pixels->planes = std::move(planes);
    return pixels;
}
CanvasFrame RenderCanvas(Renderer &renderer, const RenderableSimulation &world,
    uint64_t capture, uint64_t tick, uint64_t revision, bool paused, uint8_t view,
    std::shared_ptr<const CanvasPixelsData> previous, int webpQuality, int webpScale, bool forNetwork)
{
    if (view >= CanvasViews) throw std::runtime_error("invalid canvas view");
    RendererSettings settings;
    if (view == 1) settings.colorMode = COLOUR_HEAT;
    if (view == 2) settings.displayMode = DISPLAY_AIRP;
    renderer.sim = &world;
    renderer.ApplySettings(settings);
    renderer.Clear(); renderer.RenderSimulation();
    const auto &video = renderer.GetVideo();
    std::string planes(CanvasBytes, '\0');
    for (int y = 0; y < YRES; ++y) for (int x = 0; x < XRES; ++x)
    {
        auto px = video[{x, y}];
        size_t i = size_t(y)*XRES+x;
        planes[i] = char(px); planes[CanvasPixels+i] = char(px >> 8); planes[2*CanvasPixels+i] = char(px >> 16);
    }
    std::shared_ptr<const CanvasPixelsData> pixels;
    if (!forNetwork)
    {
        auto local = std::make_shared<CanvasPixelsData>();
        local->checksum = Checksum(planes); local->planes = std::move(planes);
        pixels = std::move(local);
    }
    else pixels = previous && previous->planes == planes &&
        (!webpQuality || (previous->webpQuality == webpQuality && previous->webpScale == webpScale) || previous->webp.empty())
        ? previous : PackCanvas(std::move(planes), webpQuality, webpScale);
    renderer.sim = nullptr;
    return {capture, tick, revision, view, paused, pixels};
}

std::string EncodeCanvas(const CanvasFrame &f, uint64_t id, const ReceivedCanvas *base, bool allowWebP)
{
    if (!id || !f.pixels || f.view >= CanvasViews || f.pixels->planes.size() != CanvasBytes)
        throw std::runtime_error("invalid canvas frame");
    uint8_t kind = 0; // full / XOR delta / unchanged pixels
    uint64_t baseId = 0;
    std::string delta;
    auto full = f.pixels->compressed.empty() ? Compress(f.pixels->planes) : std::string{};
    std::string_view body = full.empty() ? f.pixels->compressed : full;
    if (base && base->id && base->id < id && base->frame.view == f.view && !base->frame.lossy && base->frame.pixels)
    {
        const auto &old = base->frame.pixels->planes;
        if (old.size() != CanvasBytes) throw std::runtime_error("invalid baseline size");
        if (base->frame.pixels == f.pixels || old == f.pixels->planes)
        { kind = 2; baseId = base->id; body = {}; }
        else
        {
            delta.resize(CanvasBytes);
            for (size_t i = 0; i < CanvasBytes; ++i) delta[i] = char(uint8_t(old[i]) ^ uint8_t(f.pixels->planes[i]));
            delta = Compress(delta);
            if (delta.size() < body.size()) { kind = 1; baseId = base->id; body = delta; }
        }
    }
    if (allowWebP && !f.pixels->webp.empty() && f.pixels->webp.size() < body.size())
    {
        // Kind 3 is an independently decodable WebP keyframe (no baseline drift).
        // Header checksum covers the *encoded* payload. No pixel-perfect promise.
        kind = 3; baseId = 0; body = f.pixels->webp;
    }
    std::string packet = "TPC3";
    packet.push_back(char(kind)); packet.push_back(char(f.view)); packet.push_back(char(f.paused)); packet.push_back(0);
    Put(packet, id, 8); Put(packet, baseId, 8); Put(packet, f.tick, 8); Put(packet, f.revision, 8);
    Put(packet, kind == 3 ? f.pixels->webpChecksum : f.pixels->checksum, 4);
    Put(packet, f.capturedUs, 8);
    packet.append(body);
    return packet;
}
ReceivedCanvas DecodeCanvas(std::string_view packet, const ReceivedCanvas *base)
{
    if (packet.size() < CanvasHeaderBytes || packet.size() > MaxCanvasPacket || packet.substr(0,4) != "TPC3")
        throw std::runtime_error("invalid canvas packet");
    auto kind = uint8_t(packet[4]), view = uint8_t(packet[5]), paused = uint8_t(packet[6]);
    auto id = Get(packet,8,8), baseId = Get(packet,16,8);
    auto checksum = uint32_t(Get(packet,40,4));
    if (kind > 3 || view >= CanvasViews || paused > 1 || packet[7] || !id || (base && id <= base->id))
        throw std::runtime_error("invalid canvas header");
    if ((kind == 0 || kind == 3) && baseId) throw std::runtime_error("keyframe has a baseline");
    if ((kind == 1 || kind == 2) && (!baseId || baseId >= id)) throw std::runtime_error("invalid canvas baseline ID");
    if ((kind == 1 || kind == 2) && (!base || base->id != baseId || base->frame.view != view || !base->frame.pixels))
        throw MissingCanvasBase();
    std::shared_ptr<const CanvasPixelsData> pixels;
    auto body = packet.substr(CanvasHeaderBytes);
    if (kind == 3)
    {
#ifndef TPT_MP_WEBP
        throw std::runtime_error("WebP canvas unsupported by this build");
#else
        // Reject corruption before passing untrusted compressed bytes to libwebp.
        if (body.empty() || Checksum(body) != checksum)
            throw std::runtime_error("invalid WebP payload checksum");
        int width = 0, height = 0;
        if (!WebPGetInfo(reinterpret_cast<const uint8_t *>(body.data()), body.size(), &width, &height)
            || width <= 0 || height <= 0 || XRES % width || YRES % height
            || XRES/width != YRES/height || XRES/width > 3)
            throw std::runtime_error("invalid WebP canvas dimensions");
        const int scale = XRES/width;
        std::string rgb(size_t(width)*height*3, '\0');
        if (!WebPDecodeRGBInto(reinterpret_cast<const uint8_t *>(body.data()), body.size(),
            reinterpret_cast<uint8_t *>(rgb.data()), rgb.size(), width*3))
            throw std::runtime_error("invalid WebP canvas bitstream");
        std::string planes(CanvasBytes, '\0');
        for (int y = 0; y < YRES; ++y) for (int x = 0; x < XRES; ++x)
        {
            size_t i = size_t(y)*XRES+x;
            size_t j = (size_t(y/scale)*width+x/scale)*3;
            planes[i] = rgb[j+2];
            planes[CanvasPixels+i] = rgb[j+1];
            planes[2*CanvasPixels+i] = rgb[j];
        }
        auto decoded = std::make_shared<CanvasPixelsData>();
        decoded->planes = std::move(planes); decoded->checksum = Checksum(decoded->planes);
        pixels = decoded;
#endif
    }
    else if (kind == 2)
    {
        if (!body.empty() || checksum != base->frame.pixels->checksum) throw std::runtime_error("invalid repeat frame");
        pixels = base->frame.pixels;
    }
    else
    {
        std::string planes(CanvasBytes, '\0');
        uLongf size = CanvasBytes;
        uLong sourceSize = uLong(body.size());
        if (uncompress2(reinterpret_cast<Bytef *>(planes.data()), &size,
            reinterpret_cast<const Bytef *>(body.data()), &sourceSize) != Z_OK || size != CanvasBytes || sourceSize != body.size())
            throw std::runtime_error("invalid compressed canvas");
        if (kind == 1)
        {
            const auto &old = base->frame.pixels->planes;
            if (old.size() != CanvasBytes) throw std::runtime_error("invalid baseline pixels");
            for (size_t i = 0; i < CanvasBytes; ++i) planes[i] = char(uint8_t(planes[i]) ^ uint8_t(old[i]));
        }
        if (Checksum(planes) != checksum) throw std::runtime_error("canvas checksum mismatch");
        auto decoded = std::make_shared<CanvasPixelsData>();
        decoded->planes = std::move(planes); decoded->checksum = checksum;
        pixels = decoded; // The client never needs to recompress received pixels.
    }
    return {id, {id, Get(packet,24,8), Get(packet,32,8), view, paused != 0, pixels, Get(packet,44,8), kind == 3}};
}
std::vector<pixel> CanvasVideo(const CanvasFrame &frame)
{
    if (!frame.pixels || frame.pixels->planes.size() != CanvasBytes) throw std::runtime_error("invalid canvas pixels");
    const auto &p = frame.pixels->planes;
    std::vector<pixel> video(CanvasPixels);
    for (size_t i = 0; i < CanvasPixels; ++i)
        video[i] = uint8_t(p[i]) | (uint32_t(uint8_t(p[CanvasPixels+i])) << 8) | (uint32_t(uint8_t(p[2*CanvasPixels+i])) << 16);
    return video;
}
ByteBudget::ByteBudget(double bytesPerSecond, double now) :
    rate(bytesPerSecond), capacity(MaxCanvasPacket), tokens(capacity), last(now)
{
    if (!(rate > 0)) throw std::runtime_error("invalid bandwidth budget");
}
double ByteBudget::Delay(size_t bytes, double now)
{
    if (bytes > capacity) throw std::runtime_error("packet exceeds burst budget");
    tokens = std::min(capacity, tokens + std::max(0.0, now-last)*rate);
    last = std::max(now, last);
    return std::max(0.0, (double(bytes)-tokens)/rate);
}
void ByteBudget::Spend(size_t bytes, double now)
{
    if (Delay(bytes, now) > 0.000001) throw std::runtime_error("bandwidth budget exhausted");
    tokens -= bytes;
}
void ByteBudget::SetRate(double bytesPerSecond, double now)
{
    if (!(bytesPerSecond > 0) || !std::isfinite(bytesPerSecond)) throw std::runtime_error("invalid bandwidth rate");
    Delay(0, now); rate = bytesPerSecond;
}
}
