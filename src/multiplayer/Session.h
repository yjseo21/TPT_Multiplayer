#pragma once
#include "Canvas.h"
#include "EditSession.h"
#include "Metrics.h"
#include "ReplicaState.h"
#include <json/json.h>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include <string>

namespace ix { class WebSocket; }
namespace multiplayer
{
struct SessionOptions
{
    std::string url = "ws://127.0.0.1:34403", name = "Player", ca, roomToken, adminToken;
    // Local simulation: receive one exact checkpoint, then only ordered commands
    // and tick seals; physics runs on this computer. Otherwise a video stream.
    bool localSim = false;
    bool reconnect = true;
    bool serverErrorsFatal = false; // scripted runs: a rejected command ends the session
    int view = 0;
    uint64_t debugCorruptTick = 0; // test-only: perturb local state once to exercise resync
    double debugStepDelayMs = 0;   // test-only: a slower computer (extra time per local tick)
    bool debugNoSkipAhead = false; // test-only: an overloaded CI machine stays behind instead
};

// One player's connection to a room, independent of any user interface.
// Owned and driven by a single thread: call Update() once per frame. It drains
// the network, applies the ordered simulation stream (local simulation) or
// decodes canvas frames (video), and reconnects with backoff. Every disconnect
// or divergence discards the local world; a new connection always starts from a
// fresh exact checkpoint, and unconfirmed edits are never replayed.
class Session
{
public:
    explicit Session(SessionOptions options);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Throws on a permanent failure, or on any failure when reconnect is off.
    void Update();
    // An edit or room action ({"op": ...}); adds the per-connection sequence.
    // False when the world is not synchronized or the connection is saturated.
    bool Send(Json::Value command);
    bool SendControl(const Json::Value &value);
    size_t Buffered() const; // bytes queued on the connection, not yet sent
    // Rate-limited (10/s, plus a 1 s refresh), and only sent when it changed.
    void SendCursor(bool visible, int x, int y, int radius, bool drawing);
    // Video: asks the server for that view. Local simulation: views are local.
    void SetView(int view);
    // Stops the connection; afterwards Update() must not be called.
    void Stop();

    const SessionOptions &Options() const { return options; }
    bool Ready() const { return ready; }
    bool Admin() const { return admin; }
    bool RoomLocked() const { return roomLocked; }
    bool PresenceEnabled() const { return presenceEnabled; }
    bool ToolActionsSupported() const { return toolActionsSupported; }
    unsigned OwnPlayer() const { return ownPlayer; }
    int OwnStickman() const { return ownStickman; } // 1-4, 0 if none
    unsigned Connections() const { return connections; }
    int View() const { return desiredView; }
    // Players from the latest presence broadcast; empty once it is 2 s old.
    const Json::Value &Players() const { return players; }
    // Edits are possible: the shown world is current for this connection.
    bool Synchronized() const;
    bool Disconnected() const;
    const EditSession &Edits() const { return edits; }
    uint64_t AcknowledgedRevision() const { return acknowledgedRevision; }
    // The latest own edit (by sequence) applied to the local world on this connection.
    uint64_t OwnCommitted() const { return ownCommitted; }

    // The latest picture. Video mode fills it from the stream; a local-simulation
    // presenter publishes its own renders so status and metrics stay meaningful.
    const ReceivedCanvas &Picture() const { return canvas; }
    bool TakePictureChanged() { bool c = pictureChanged; pictureChanged = false; return c; }
    void PublishLocalPicture(CanvasFrame frame);

    // Local simulation only; null until the first checkpoint arrives.
    ReplicaState *Replica() { return replica.get(); }
    const ReplicaState *Replica() const { return replica.get(); }
    uint64_t Resyncs() const { return resyncs; }

    // Status and measurements for the HUD and the metrics log.
    Json::Value Sample();
    // Own-edit latencies (ms) measured since the previous call; local simulation only.
    std::vector<double> TakeEditLatencies();

private:
    struct Mailbox;
    SessionOptions options;
    std::unique_ptr<Mailbox> mailbox; // shared with socket callbacks; outlives the socket
    std::unique_ptr<ix::WebSocket> socket;
    double began;
    ClientMetrics metrics;
    EditSession edits;
    ReceivedCanvas canvas;
    bool pictureChanged = false, waitingForKeyframe = false;

    bool ready = false, admin = false, roomLocked = false, presenceEnabled = false, toolActionsSupported = false;
    unsigned ownPlayer = 0, connections = 0, failures = 0;
    std::string lastFailure; // why the last connection (or attempt) ended, shown while reconnecting
    int ownStickman = 0;
    int desiredView = 0;
    bool attempting = true;
    double retryAt = 0, lastReceived, presenceAt = -10;
    Json::Value players{Json::arrayValue}, lastCursor;
    double cursorAt = 0, cursorSent = -10;
    uint64_t acknowledgedRevision = 0;

    std::unique_ptr<ReplicaState> replica;
    double desyncWaitUntil = 0;
    uint64_t resyncs = 0, sealsBefore = 0, checksBefore = 0, editsBefore = 0;
    bool corruptionInjected = false;
    // Paced stepping: run sealed ticks at the server's observed rate, keeping a
    // small margin behind the latest seal so irregular arrivals do not stall the
    // picture. The margin follows the largest recent gap between seal advances.
    double tickCredit = 0, lastPump = 0, tickRate = 60, rateSince = 0;
    uint64_t rateTick = 0, sealBatchMs = 0;
    uint64_t lastSealed = 0;
    double lastSealAdvance = 0;
    bool pausedSinceSeal = false;
    std::deque<std::pair<double, double>> sealGaps; // (time, seconds since the previous advance)
    uint64_t Lead() const;
    // A computer that cannot keep up: unable to catch up with the room for 3 s
    // ("TOO SLOW"); after 10 s, it jumps to the room's present.
    double behindSince = 0, lastSkipAhead = -100, behindSeconds = 0;
    uint64_t behindBacklog = 0, catchingUp = 0; // backlog when the behind clock last restarted; ticks still to run
    uint64_t skipAheads = 0;
    // Room pacing: physics ticks run and the time they took, reported to the
    // server every 2 s as the rate this computer can follow.
    double paceSince = 0, paceSeconds = 0;
    uint64_t paceSteps = 0;

    // Own-edit latency: from Send() until the edit is applied to the local world.
    std::map<uint64_t, double> sentAt;
    uint64_t ownCommitted = 0;
    std::vector<double> latencies; // since the last TakeEditLatencies()
    uint64_t latencySamples = 0;
    double latencyTotalMs = 0, latencyMaxMs = 0;

    void Connect();
    void Fail(const std::string &reason);
    void Lost();
    void HandleControl(const Json::Value &v, double received);
    void HandleOrdered(bool binary, const std::string &bytes, double received);
    void Pump();
    void Diverged(const ReplicaDesync &e);
    void ReplicaFailed(const std::exception &e);
    void RetireReplica();
    void AddLocalStatus(Json::Value &out) const;
};
}
