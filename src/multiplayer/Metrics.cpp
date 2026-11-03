#include "Metrics.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace multiplayer
{
double MonotonicSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint64_t MonotonicMicros() { return uint64_t(MonotonicSeconds()*1000000); }
bool ClientMetrics::ProbeDue(double now) const
{
    return now-lastProbe >= 1 && (!probe || now-probeSent >= 5);
}
Json::Value ClientMetrics::BeginProbe(double now)
{
    // One outstanding probe; timed-out responses cannot overwrite a newer estimate.
    probe = uint64_t(now*1000000)+1; probeSent = lastProbe = now;
    Json::Value request; request["type"] = "probe"; request["id"] = Json::UInt64(probe);
    return request;
}
bool RoomSlowed(const Json::Value &telemetry)
{
    return telemetry["pacedHz"].isNumeric() && telemetry["pacedHz"].asDouble() < telemetry["targetHz"].asDouble()*.85;
}
bool ClientMetrics::Telemetry(const Json::Value &v, double received)
{
    if (!probe || !v["id"].isUInt64() || v["id"].asUInt64() != probe) return false;
    if (!v["serverReceivedUs"].isUInt64() || !v["serverSentUs"].isUInt64() ||
        !v["revision"].isUInt64() || !v["paused"].isBool() || !v["bandwidthLimited"].isBool())
        throw std::runtime_error("invalid server telemetry");
    for (const auto *key : {"simHz", "tickMs", "targetHz", "updateHz", "bandwidthKbps", "inFlight"})
        if (!v[key].isNumeric() || !std::isfinite(v[key].asDouble()) || v[key].asDouble() < 0 || v[key].asDouble() > 1000000)
            throw std::runtime_error("invalid telemetry measurement");
    for (const auto *key : {"uploadKbps", "shareKbps", "streamWindow", "pacingHz", "frameAckMs", "pacedHz"})
        if (!v[key].isNull() && (!v[key].isNumeric() || !std::isfinite(v[key].asDouble()) || v[key].asDouble() < 0 || v[key].asDouble() > 1000000))
            throw std::runtime_error("invalid pacing measurement");
    if (!v["networkPaced"].isNull() && !v["networkPaced"].isBool())
        throw std::runtime_error("invalid pacing status");
    double s1 = v["serverReceivedUs"].asUInt64()/1000000.0;
    double s2 = v["serverSentUs"].asUInt64()/1000000.0;
    if (s2 < s1 || received < probeSent || s2-s1 > received-probeSent+.001)
        throw std::runtime_error("invalid telemetry timestamps");
    rttMs = std::max(0.0, (received-probeSent-(s2-s1))*1000);
    offset = ((s1-probeSent)+(s2-received))/2;
    calibrated = true; lastTelemetry = received; probe = 0; server = v;
    return true;
}
Json::Value ClientMetrics::Sample(double now, const ReceivedCanvas &canvas, bool ready, bool resyncing)
{
    double elapsed = now-windowStart;
    if (elapsed >= 1)
    {
        hz = windowFrames/elapsed; mbps = windowBytes*8.0/elapsed/1000000;
        windowStart = now; windowFrames = windowBytes = 0;
    }
    bool fresh = calibrated && now-lastTelemetry < 5;
    bool paused = canvas.id && canvas.frame.paused && fresh && server["paused"].asBool()
        && canvas.frame.revision >= server["revision"].asUInt64();
    double age = fresh && canvas.id ? std::max(0.0, (now+offset-canvas.frame.capturedUs/1000000.0)*1000) : -1;
    std::string status = "LIVE";
    if (!ready) status = "CONNECTING";
    else if (resyncing) status = "RESYNCING";
    else if (!canvas.id) status = "WAITING FOR CANVAS";
    else if (!fresh) status = calibrated ? "TELEMETRY STALE" : "MEASURING";
    else if (paused) status = "PAUSED";
    else if (!server["paused"].asBool() && RoomSlowed(server)) status = "ROOM SLOWED";
    else if (!server["paused"].asBool() && server["simHz"].asDouble() < server["targetHz"].asDouble()*.85) status = "SERVER SLOW";
    else if (server["bandwidthLimited"].asBool()) status = "BANDWIDTH LIMIT";
    else if (age > 500) status = "UPDATES DELAYED";
    else if (server["networkPaced"].asBool()) status = "NETWORK PACING";
    else if (rttMs > 150) status = "HIGH LATENCY";
    Json::Value result;
    result["type"] = "metrics"; result["status"] = status; result["updatesHz"] = hz; result["receiveMbps"] = mbps;
    result["telemetryFresh"] = fresh; result["paused"] = paused;
    result["frame"] = Json::UInt64(canvas.id); result["tick"] = Json::UInt64(canvas.frame.tick);
    result["revision"] = Json::UInt64(canvas.frame.revision);
    result["view"] = canvas.frame.view;
    if (canvas.frame.pixels) result["canvasChecksum"] = canvas.frame.pixels->checksum;
    if (fresh)
    {
        result["pingMs"] = rttMs;
        if (canvas.id) result["frameAgeMs"] = age;
        for (const auto *key : {"simHz", "tickMs", "targetHz", "updateHz", "bandwidthKbps", "inFlight", "bandwidthLimited"})
            result[key] = server[key];
        for (const auto *key : {"uploadKbps", "shareKbps", "streamWindow", "pacingHz", "frameAckMs", "networkPaced", "pacedHz"})
            result[key] = server[key];
    }
    return result;
}
std::array<std::string, 3> MetricLines(const Json::Value &v)
{
    auto number = [&](const char *key, int precision = 0) {
        if (v[key].isNull()) return std::string("--");
        std::ostringstream out; out << std::fixed << std::setprecision(precision) << v[key].asDouble(); return out.str();
    };
    if (v["status"] == "RECONNECTING") return {
        "RECONNECTING  |  Retry in " + number("retrySeconds", 1) + " s  |  Escape to exit",
        "Last received canvas is frozen. Editing resumes after synchronization.",
        "Unconfirmed edits not replayed: " + number("uncertainEdits")
    };
    if (v["localSimulation"].asBool()) return {
        v["status"].asString() + "  |  Ping " + number("pingMs") + " ms  |  RX " + number("receiveMbps", 3)
            + " Mbps  |  Local frames " + number("updatesHz", 1) + "/s",
        "Local physics tick " + number("localTick") + "  |  Server " + number("simHz", 1) + "/" + number("targetHz")
            + " steps/s  |  Step " + number("tickMs", 1) + " ms",
        "Verified fingerprints " + number("verifiedHashes") + "  |  Commands " + number("appliedCommands")
            + "  |  Resyncs " + number("resyncs") + "  |  F4: hide status"
    };
    return {
        v["status"].asString() + "  |  Ping " + number("pingMs") + " ms  |  Updates " + number("updatesHz", 1)
            + "/s  |  RX " + number("receiveMbps", 2) + " Mbps",
        "Simulation " + number("simHz", 1) + "/" + number("targetHz") + " steps/s  |  Step " + number("tickMs", 1)
            + " ms  |  Canvas age " + (v["paused"].asBool() ? std::string("paused") : "~"+number("frameAgeMs")+" ms"),
        "Share " + number("shareKbps") + " kbps  |  Pace " + number("pacingHz", 1) + "/s  |  In flight " + number("inFlight")
            + "/" + number("streamWindow") + "  |  F4: hide status"
    };
}
}
