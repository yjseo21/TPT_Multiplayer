#pragma once
#include "Canvas.h"
#include <json/json.h>
#include <array>

namespace multiplayer
{
double MonotonicSeconds();
uint64_t MonotonicMicros();

// All client metric state is owned by the UI thread. Timestamps are monotonic;
// frame age is an estimate because asymmetric network delay cannot be measured here.
class ClientMetrics
{
    uint64_t probe = 0;
    double probeSent = 0, lastProbe = -10, lastTelemetry = -10;
    double offset = 0, rttMs = 0;
    bool calibrated = false;
    double windowStart, hz = 0, mbps = 0;
    uint64_t windowFrames = 0, windowBytes = 0;
    Json::Value server;
public:
    explicit ClientMetrics(double now) : windowStart(now) {}
    bool ProbeDue(double now) const;
    Json::Value BeginProbe(double now);
    bool Telemetry(const Json::Value &reply, double received);
    void Traffic(uint64_t bytes) { windowBytes += bytes; }
    void Frame() { ++windowFrames; }
    Json::Value Sample(double now, const ReceivedCanvas &canvas, bool ready, bool resyncing);
};
std::array<std::string, 3> MetricLines(const Json::Value &sample);
}
