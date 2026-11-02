#pragma once
#include "CanvasProducer.h"
#include "Peer.h"
#include "TickTimeline.h"
#include "Traffic.h"
#include "World.h"
#include <json/json.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ix { class WebSocket; struct WebSocketMessage; }
namespace multiplayer
{
class AutosaveStore;
class Journal;
struct RoomConfig
{
    std::string roomToken, adminToken;
    int ticks = 60, updates = 20, bandwidthKbps = 8192, uploadKbps = 8192;
    int webpQuality = 0, webpScale = 1, sealBatchMs = 0, autosaveInterval = 60;
};
// One shared world and its (up to four) players. The world thread calls Step()
// once per physics tick; it alone mutates the world. Socket callbacks (Accept and
// the per-player message handlers) only validate and queue requests, update
// per-player state under that player's mutex, and answer queries.
class Room
{
public:
    Room(RoomConfig config, World &world, AutosaveStore *autosaves, Journal *journal);
    ~Room();
    Room(const Room &) = delete;
    Room &operator=(const Room &) = delete;

    // Connection callback of the WebSocket server.
    void Accept(std::weak_ptr<ix::WebSocket> socket);
    // One world-thread iteration: edits, physics, replication, frames, autosave.
    void Step();
    // World thread, between Step() calls: true as soon as edits are queued
    // (then call ApplyEarly()), false at the deadline.
    bool WaitForRequests(std::chrono::steady_clock::time_point until);
    void ApplyEarly();
    // An applied edit could not be journaled; the room must stop (fail closed).
    bool Failed() const { return failed; }
    // After the WebSocket server has stopped: release players and queued edits.
    void Close();

private:
    RoomConfig config;
    World &world;
    AutosaveStore *autosaves;
    Journal *journal;
    TickTimeline timeline;
    CanvasProducer producer;
    UploadBudget upload;
    bool failed = false;

    // Shared with socket callbacks.
    std::mutex peersMutex, queueMutex, telemetryMutex;
    std::condition_variable queueChanged;
    std::vector<std::shared_ptr<Peer>> peers;
    std::deque<Request> queue;
    uint32_t nextPlayer = 1;
    std::atomic<bool> roomLocked{false};
    Json::Value telemetry;
    // Per-group fingerprints of recently hashed seals, so a diverged replica
    // can be told which state groups differ. Written by the world thread.
    std::mutex digestMutex;
    struct RecentDigest { uint64_t tick, revision; Json::Value fields; };
    std::deque<RecentDigest> recentDigests;

    // World thread only.
    using Clock = std::chrono::steady_clock;
    Clock::time_point nextFrame, nextStats, nextAutosave;
    Clock::duration framePeriod;
    uint64_t capture = 0, capturedTick = UINT64_MAX, capturedRevision = UINT64_MAX;
    unsigned capturedViews = 0;
    double measuredAt, tickMsTotal = 0, simHz = 0, tickMs = 0;
    uint64_t measuredTick, measuredSteps = 0;
    uint64_t savedTick = UINT64_MAX, savedRevision = UINT64_MAX, reportedSave = 0;
    std::string reportedSaveError;
    double nextPresence = 0;
    uint64_t lastSealedTick = UINT64_MAX, lastSealedRevision = UINT64_MAX;

    void OnMessage(const std::shared_ptr<Peer> &p, const ix::WebSocketMessage &msg);
    void Hello(const std::shared_ptr<Peer> &p, const Json::Value &v);
    void Cursor(const std::shared_ptr<Peer> &p, const Json::Value &v);
    void Probe(const std::shared_ptr<Peer> &p, const Json::Value &v);
    void DesyncReport(const std::shared_ptr<Peer> &p, const Json::Value &v);
    void QueueRequest(const std::shared_ptr<Peer> &p, const Json::Value &v, uint64_t &seq);
    void RememberDigest(uint64_t tick, uint64_t revision, const Json::Value &fields);

    std::vector<std::shared_ptr<Peer>> CollectPeers();
    void BroadcastPresence(const std::vector<std::shared_ptr<Peer>> &active);
    void ApplyRequests(const std::vector<std::shared_ptr<Peer>> &active);
    void Advance(const std::vector<std::shared_ptr<Peer>> &active);
    void BootstrapReplicas(const std::vector<std::shared_ptr<Peer>> &active);
    void SealTick(const std::vector<std::shared_ptr<Peer>> &active);
    void Measure();
    void PublishFrames(const std::vector<std::shared_ptr<Peer>> &active);
    void Autosave();
    void Stats(const std::vector<std::shared_ptr<Peer>> &active);
    void PasteChunk(const std::shared_ptr<Peer> &p, const Json::Value &v);
    // Applies, journals and broadcasts one command; actor 0 is the server itself.
    void Commit(const Command &command, uint32_t actor, const std::vector<std::shared_ptr<Peer>> &active);
    void ReleaseStickmen(const std::vector<std::shared_ptr<Peer>> &active);
    std::array<uint32_t, 5> stickmanHolders{}; // player id per stickman slot, as last seen
    uint64_t serverSequence = 0; // commands the server issues itself (actor 0)
    // Since the last stats line: join snapshots captured (time on the room
    // thread) and the longest Step(), which is what every player feels.
    std::shared_ptr<SnapshotBuffers> snapshotBuffers = std::make_shared<SnapshotBuffers>();
    uint64_t snapshotCaptures = 0;
    double snapshotCaptureMs = 0, longestStepMs = 0;
};
}
