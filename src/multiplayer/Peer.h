#pragma once
#include "Canvas.h"
#include "Metrics.h"
#include "Protocol.h"
#include "Traffic.h"
#include <ixwebsocket/IXWebSocket.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include "ExactCheckpoint.h"

namespace multiplayer
{
// Raw snapshot buffers kept between joins. Fresh memory for a 25 MB capture
// costs several times more than the capture itself on some systems.
class SnapshotBuffers
{
public:
    std::string Take()
    {
        std::lock_guard lock(mutex);
        if (free.empty()) return {};
        auto buffer = std::move(free.back()); free.pop_back();
        return buffer;
    }
    void Give(std::string buffer)
    {
        std::lock_guard lock(mutex);
        if (free.size() < 2) free.push_back(std::move(buffer));
    }
private:
    std::mutex mutex;
    std::vector<std::string> free;
};
// A join snapshot. The room thread only captures the raw state (a fast copy);
// the first sender that needs it compresses it on its own thread, once, so a
// joining player never stalls the room for everyone else.
class CheckpointSource
{
public:
    CheckpointSource(std::string raw, std::shared_ptr<SnapshotBuffers> buffers) :
        raw(std::move(raw)), buffers(std::move(buffers)) {}
    std::shared_ptr<const std::string> Packet()
    {
        std::call_once(once, [this] {
            packet = std::make_shared<const std::string>(CompressExactCheckpointRaw(raw));
            buffers->Give(std::move(raw)); raw = {};
        });
        return packet;
    }
private:
    std::string raw;
    std::shared_ptr<SnapshotBuffers> buffers;
    std::once_flag once;
    std::shared_ptr<const std::string> packet;
};
// Each sender encodes against its own last successfully sent frame. Unsent captures
// can be replaced freely. Adaptive ACK gating bounds the receiver backlog.
struct Peer
{
    std::weak_ptr<ix::WebSocket> socket;
    std::atomic<bool> closed{false}, authenticated{false}, admin{false}, senderDone{false}, useWebP{false};
    std::atomic<bool> replica{false}, needsReplicaBootstrap{false};
    std::atomic<unsigned> pending{0};
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<unsigned> view{0};
    std::atomic<int> stickmanSlot{0}; // 1-4: which room stickman this player steers
    std::string address; // remote IP, for limiting password guesses
    // A save being uploaded for a paste (receive callback only): id and base64 so far.
    uint64_t pasteId = 0;
    std::string pasteSave;
    std::atomic<uint64_t> framesSent{0}, fullFrames{0}, deltaFrames{0}, repeats{0}, webpFrames{0};
    uint64_t lastSequence = 0; // Owned by this connection's receive callback.
    uint32_t id;
    std::chrono::steady_clock::time_point connected = std::chrono::steady_clock::now(), refill = connected;
    double operations = 120, work = EditWorkPerSecond;
    double streamControls = 4, streamRefill = MonotonicSeconds();
    double lastProbe = -10, lastDesyncReport = -10;
    std::atomic<double> lastBandwidthWait{-10};
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Json::Value> controls;
    // Bytes queued in controls, roughly: a player who stops reading while others
    // paste large saves is dropped before the queue can exhaust the server's memory.
    std::deque<size_t> controlBytes;
    size_t queuedBytes = 0;
    static constexpr size_t MaxQueuedBytes = 64U * 1024 * 1024;
    static size_t ApproximateBytes(const Json::Value &value)
    {
        const auto &save = value["command"]["save"];
        return 256 + (save.isString() ? save.asString().size() : 0);
    }
    // Sender-thread-only snapshot transfer. Owner thread publishes immutable bytes
    // and queues a checkpoint_begin marker under the same mutex. Commits and
    // tick seals queue behind it, in strict WebSocket ordering.
    std::shared_ptr<const std::string> checkpoint;
    std::shared_ptr<CheckpointSource> checkpointSource; // queued, not compressed yet
    size_t checkpointOffset = 0;
    uint64_t checkpointId = 0;
    bool checkpointStarted = false, checkpointEnding = false;
    // Replica seal batching. A held seal is superseded by later seals and is
    // released before any other message, when it carries a digest or pause, or
    // when it is sealBatch seconds old (released by the sender thread).
    double sealBatch = 0, pendingSealSince = 0;
    Json::Value pendingSeal;
    Json::Value presence, cursor;
    std::string name;
    bool wantsPresence = false, wantsCommits = false;
    double cursorAt = -10, cursorRefill = MonotonicSeconds(), cursorTokens = 20;
    std::shared_ptr<const CanvasFrame> latest;
    std::deque<std::pair<uint64_t, double>> inFlight;
    uint64_t generation = 0, lastAck = 0, lastIssued = 0, minimumCapture = 0;
    UploadBudget &upload;
    StreamPacer pacer;
    std::thread sender;

    Peer(std::weak_ptr<ix::WebSocket> s, uint32_t i, UploadBudget &u, int updates) : socket(s), id(i), upload(u), pacer(updates) {}
    void Start();
    void QueueCheckpoint(std::shared_ptr<CheckpointSource> source,
        uint64_t tick, uint64_t revision, uint64_t resetGeneration, bool paused)
    {
        std::lock_guard lock(mutex);
        if (!replica || !source || checkpoint || checkpointSource || checkpointEnding ||
            controls.size() >= 30) throw std::runtime_error("checkpoint transfer already queued");
        FlushSealLocked();
        checkpointSource = std::move(source); checkpointOffset = 0; checkpointStarted = false;
        // Transfer ID is unique within a connection; reconnect always starts
        // another WebSocket with new state.
        ++checkpointId;
        Json::Value begin;
        begin["type"] = "checkpoint_begin"; begin["id"] = Json::UInt64(checkpointId);
        // "bytes" is filled in by the sender once the snapshot is compressed.
        begin["tick"] = Json::UInt64(tick); begin["revision"] = Json::UInt64(revision);
        begin["resetGeneration"] = Json::UInt64(resetGeneration);
        begin["paused"] = paused;
        controls.push_back(begin); controlBytes.push_back(256); queuedBytes += 256; changed.notify_one();
    }
    void Text(Json::Value value)
    {
        std::lock_guard lock(mutex);
        if (replica && value["type"] == "tick_seal" && sealBatch > 0)
        {
            // Hold empty-interval seals briefly: the latest one lets the client run
            // every intermediate tick. Digests and pause transitions go out at once.
            bool urgent = value.isMember("digest") || value.get("paused", false).asBool();
            if (pendingSeal.isNull()) pendingSealSince = MonotonicSeconds();
            else CarrySealFields(pendingSeal, value);
            pendingSeal = std::move(value);
            if (urgent) FlushSealLocked();
            return;
        }
        // Never coalesce across a command_commit boundary.
        FlushSealLocked();
        PushLocked(std::move(value));
    }
    // A later compact seal replacing an earlier one keeps the earlier one's
    // metadata, so the replica still checks every change.
    static void CarrySealFields(const Json::Value &from, Json::Value &to)
    {
        for (auto key : {"revision", "resetGeneration", "paused"})
            if (from.isMember(key) && !to.isMember(key)) to[key] = from[key];
    }
    // Requires mutex. Queues the held seal (if any) ahead of later messages.
    void FlushSealLocked()
    {
        if (pendingSeal.isNull()) return;
        PushLocked(std::move(pendingSeal)); pendingSeal = Json::Value();
    }
    void PushLocked(Json::Value value)
    {
        // Coalesce empty edit intervals when the sender is behind, but keep any
        // queued seal that carries a fingerprint so it is still verified.
        if (replica && value["type"] == "tick_seal" && !controls.empty() &&
            controls.back()["type"] == "tick_seal" && !controls.back().isMember("digest"))
        {
            CarrySealFields(controls.back(), value);
            controls.back() = std::move(value); changed.notify_one(); return;
        }
        auto bytes = ApproximateBytes(value);
        if (controls.size() >= (replica ? 2048U : 32U) || queuedBytes + bytes > MaxQueuedBytes)
        { closed = true; changed.notify_one(); return; }
        controls.push_back(std::move(value)); controlBytes.push_back(bytes); queuedBytes += bytes; changed.notify_one();
    }
    void Publish(std::shared_ptr<const CanvasFrame> frame)
    {
        std::lock_guard lock(mutex);
        if (!replica && frame->view == view && frame->capture >= minimumCapture && latest != frame)
        { latest = std::move(frame); changed.notify_one(); }
    }
    void Acknowledge(uint64_t id)
    {
        std::lock_guard lock(mutex);
        if (id <= lastAck) return;
        auto it = std::find_if(inFlight.begin(), inFlight.end(), [&](auto &entry) { return entry.first == id; });
        if (it == inFlight.end()) throw std::runtime_error("invalid frame acknowledgement");
        pacer.Acknowledge(MonotonicSeconds()-it->second);
        inFlight.erase(inFlight.begin(), it+1); lastAck = id; changed.notify_one();
    }
    void ResetStream()
    {
        std::lock_guard lock(mutex); upload.Cancel(id); ++generation; changed.notify_one();
    }
    void StreamControl()
    {
        // Receive callback only; prevents repeated keyframe/view requests from defeating coalescing.
        auto now = MonotonicSeconds();
        streamControls = std::min(4.0, streamControls + (now-streamRefill)*2); streamRefill = now;
        if (streamControls < 1) throw std::runtime_error("stream control rate exceeded");
        streamControls -= 1;
    }
    void ChangeView(unsigned next)
    {
        std::lock_guard lock(mutex);
        if (view == next) return;
        upload.Cancel(id);
        if (latest) minimumCapture = latest->capture;
        latest.reset(); view = next; ++generation; changed.notify_one();
    }
    void Error(const std::string &message, uint64_t seq = 0)
    {
        Json::Value e; e["type"] = "error"; e["message"] = message; e["seq"] = Json::UInt64(seq); Text(e);
    }
    ~Peer()
    {
        closed = true; changed.notify_one();
        if (sender.joinable()) sender.join();
    }
};
struct Request
{
    enum class Administration { None, Kick, Lock } administration = Administration::None;
    std::shared_ptr<Peer> peer;
    Command command{};
    uint32_t target = 0;
    bool locked = false;
};}
