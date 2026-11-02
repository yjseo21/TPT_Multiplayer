#include "Room.h"
#include "Autosave.h"
#include "CommandRecord.h"
#include "ExactCheckpoint.h"
#include "Journal.h"
#include "StateDigest.h"
#include <ixwebsocket/IXWebSocket.h>
#include <algorithm>
#include <iostream>

namespace multiplayer
{
Room::Room(RoomConfig c, World &w, AutosaveStore *a, Journal *j) :
    config(std::move(c)), world(w), autosaves(a), journal(j),
    timeline(world.tick, world.revision, world.resetGeneration),
    producer(config.webpQuality, config.webpScale),
    upload(config.uploadKbps*1000.0/8, MonotonicSeconds()),
    measuredAt(MonotonicSeconds()), measuredTick(world.tick)
{
    telemetry["targetHz"] = config.ticks; telemetry["updateHz"] = config.updates;
    telemetry["simHz"] = config.ticks; telemetry["tickMs"] = 0.0;
    telemetry["paused"] = world.paused; telemetry["revision"] = Json::UInt64(world.revision);
    auto now = Clock::now();
    nextFrame = now; nextStats = now + std::chrono::seconds(5);
    nextAutosave = now + std::chrono::seconds(config.autosaveInterval);
    framePeriod = std::chrono::nanoseconds(1000000000 / config.updates);
    // Warm one snapshot buffer now, so even the first join is a fast copy.
    try { snapshotBuffers->Give(CaptureExactCheckpointRaw(world)); }
    catch (const std::exception &) {} // reported, if it persists, when a player joins
}
Room::~Room() { Close(); }
void Room::Close()
{
    std::vector<std::shared_ptr<Peer>> released;
    { std::lock_guard lock(peersMutex); released.swap(peers); }
    { std::lock_guard lock(queueMutex); queue.clear(); }
}
void Room::RememberDigest(uint64_t tick, uint64_t revision, const Json::Value &fields)
{
    std::lock_guard lock(digestMutex);
    recentDigests.push_back({tick, revision, fields});
    while (recentDigests.size() > 32) recentDigests.pop_front();
}
void Room::Accept(std::weak_ptr<ix::WebSocket> weak, std::string address)
{
    std::shared_ptr<Peer> p;
    {
        std::lock_guard lock(peersMutex);
        p = std::make_shared<Peer>(weak, nextPlayer++, upload, config.updates); peers.push_back(p);
        p->address = std::move(address);
    }
    p->Start();
    auto ws = weak.lock();
    if (!ws) return;
    ws->setOnMessageCallback([this, p](const ix::WebSocketMessagePtr &msg) { OnMessage(p, *msg); });
}
void Room::OnMessage(const std::shared_ptr<Peer> &p, const ix::WebSocketMessage &message)
{
    auto *msg = &message;
    if (msg->type == ix::WebSocketMessageType::Close || msg->type == ix::WebSocketMessageType::Error)
    { p->closed = true; p->changed.notify_one(); return; }
    if (msg->type != ix::WebSocketMessageType::Message || p->closed) return;
    uint64_t seq = 0;
    try
    {
        if (msg->binary) throw std::runtime_error("client messages must be JSON text");
        auto v = ParseJson(msg->str);
        if (!p->authenticated) { Hello(p, v); return; }
        if (v["type"] == "cursor") { Cursor(p, v); return; }
        if (v["type"] == "frame_ack")
        {
            if (!v["frame"].isUInt64() || !v["frame"].asUInt64()) throw std::runtime_error("invalid frame ID");
            p->Acknowledge(v["frame"].asUInt64()); return;
        }
        if (v["type"] == "probe") { Probe(p, v); return; }
        if (v["type"] == "paste_chunk") { PasteChunk(p, v); return; }
        if (v["type"] == "resync") { p->StreamControl(); p->ResetStream(); return; }
        if (v["type"] == "desync_report") { DesyncReport(p, v); return; }
        if (v["type"] == "view")
        {
            auto view = unsigned(Integer(v, "view", 0, CanvasViews-1));
            p->StreamControl();
            p->ChangeView(view);
            return;
        }
        QueueRequest(p, v, seq);
    }
    catch (const std::exception &e)
    {
        if (!p->authenticated)
        {
            if (auto ws = p->socket.lock())
                ws->close(std::string(e.what()).starts_with("room full") ||
                          std::string(e.what()).starts_with("room locked") ? 1013 : 1008, e.what());
            p->closed = true; p->changed.notify_one();
        }
        else p->Error(e.what(), seq);
    }
}
void Room::PasteChunk(const std::shared_ptr<Peer> &p, const Json::Value &v)
{
    // Chunks of one upload arrive in order on this connection; a new id starts over.
    if (!v["id"].isUInt64() || !v["data"].isString() || v["data"].asString().size() > MaxPasteChunkChars)
        throw std::runtime_error("invalid paste upload");
    if (v["id"].asUInt64() != p->pasteId) { p->pasteId = v["id"].asUInt64(); p->pasteSave.clear(); }
    if (p->pasteSave.size() + v["data"].asString().size() > (MaxPasteBytes+2)/3*4)
        throw std::runtime_error("pasted save too large for a room");
    // Uploads share the player's editing budget (one operation per chunk).
    auto now = Clock::now();
    double elapsed = std::chrono::duration<double>(now - p->refill).count(); p->refill = now;
    p->operations = std::min(120.0, p->operations + elapsed * 120);
    p->work = std::min(EditWorkPerSecond, p->work + elapsed * EditWorkPerSecond);
    if (p->operations < 1) throw std::runtime_error("editing rate exceeded");
    p->operations -= 1;
    p->pasteSave += v["data"].asString();
}
namespace
{
// Same time whatever the input: comparing passwords leaks nothing about them.
bool SameSecret(const std::string &given, const std::string &expected)
{
    unsigned char difference = given.size() != expected.size();
    for (size_t i = 0; i < given.size(); ++i)
        difference |= uint8_t(given[i]) ^ uint8_t(expected.empty() ? 0 : expected[i % expected.size()]);
    return !difference;
}
}
bool Room::AuthBlocked(const std::string &address)
{
    std::lock_guard lock(authMutex);
    auto &failures = authFailures[address];
    while (!failures.empty() && MonotonicSeconds() - failures.front() > 60) failures.pop_front();
    if (failures.empty()) authFailures.erase(address);
    else if (failures.size() >= 5) return true;
    if (authFailures.size() > 4096) authFailures.clear(); // bounded memory under a flood
    return false;
}
void Room::AuthFailed(const std::string &address)
{
    std::lock_guard lock(authMutex);
    authFailures[address].push_back(MonotonicSeconds());
}
void Room::Hello(const std::shared_ptr<Peer> &p, const Json::Value &v)
{
    if (v["type"] != "hello" || !v["protocol"].isUInt() || v["protocol"].asUInt() != ProtocolVersion || v["build"] != BuildId)
        throw std::runtime_error("incompatible protocol or build");
    if (AuthBlocked(p->address))
        throw std::runtime_error("too many wrong passwords from this address; wait a minute");
    if (!v["token"].isString() || !SameSecret(v["token"].asString(), config.roomToken))
    {
        AuthFailed(p->address);
        throw std::runtime_error("invalid room credential");
    }
    if (!v["name"].isString() || v["name"].asString().empty() || v["name"].asString().size() > 32)
        throw std::runtime_error("name must contain 1-32 bytes");
    bool isAdmin = !config.adminToken.empty() && v["adminToken"].isString() && SameSecret(v["adminToken"].asString(), config.adminToken);
    if (!isAdmin && v["adminToken"].isString() && !v["adminToken"].asString().empty())
    {
        // A wrong admin key is a failed guess too; the player joins without admin rights.
        AuthFailed(p->address);
    }
    std::lock_guard lock(peersMutex);
    if (roomLocked && !isAdmin) throw std::runtime_error("room locked by administrator");
    auto active = std::count_if(peers.begin(), peers.end(), [](auto &p) { return p->authenticated && !p->closed; });
    if (active >= 4) throw std::runtime_error("room full (4 players)");
    {
        std::lock_guard peerLock(p->mutex);
        if (p->closed) return;
        upload.Join(p->id, config.bandwidthKbps*1000.0/8, MonotonicSeconds());
    }
    p->admin = isAdmin;
    // The lowest stickman slot no connected player holds.
    for (int slot = 1; slot <= 4 && !p->stickmanSlot; ++slot)
        if (std::none_of(peers.begin(), peers.end(), [&](auto &o) { return o != p && o->authenticated && !o->closed && o->stickmanSlot == slot; }))
            p->stickmanSlot = slot;
    p->name = v["name"].asString();
    for (auto &c : p->name) if (static_cast<unsigned char>(c) < 32 || c == 127) c = '?';
    p->wantsPresence = v["presence"].isBool() && v["presence"].asBool();
    p->replica = v["localSimulation"].isBool() && v["localSimulation"].asBool();
    p->needsReplicaBootstrap = bool(p->replica);
    { std::lock_guard peerLock(p->mutex); p->sealBatch = p->replica ? config.sealBatchMs/1000.0 : 0; }
    p->wantsCommits = bool(p->replica) || (v["commandCommits"].isBool() && v["commandCommits"].asBool());
    p->useWebP = config.webpQuality && SupportsWebP() &&
        v["webp"].isBool() && v["webp"].asBool();
    Json::Value welcome;
    welcome["type"] = "welcome"; welcome["protocol"] = ProtocolVersion; welcome["build"] = BuildId;
    welcome["player"] = p->id; welcome["admin"] = bool(p->admin); welcome["stickman"] = p->stickmanSlot.load();
    welcome["width"] = XRES; welcome["height"] = YRES;
    welcome["stream"] = "canvas-delta-v3"; welcome["bandwidthKbps"] = config.bandwidthKbps;
    welcome["webp"] = bool(p->useWebP);
    welcome["webpQuality"] = p->useWebP ? config.webpQuality : 0;
    welcome["webpScale"] = p->useWebP ? config.webpScale : 1;
    if (p->replica) welcome["sealBatchMs"] = config.sealBatchMs;
    welcome["uploadKbps"] = config.uploadKbps;
    welcome["presence"] = p->wantsPresence;
    welcome["commandCommits"] = p->wantsCommits;
    welcome["localSimulation"] = bool(p->replica);
    welcome["toolActions"] = true;
    welcome["roomLocked"] = bool(roomLocked);
    p->Text(welcome); p->authenticated = true;
}
void Room::Cursor(const std::shared_ptr<Peer> &p, const Json::Value &v)
{
    if (!p->wantsPresence) throw std::runtime_error("presence was not negotiated");
    Json::Value cursor;
    if (!v["visible"].isBool() || !v["drawing"].isBool()) throw std::runtime_error("invalid cursor flags");
    cursor["visible"] = v["visible"]; cursor["drawing"] = v["drawing"];
    cursor["x"] = Integer(v, "x", 0, XRES-1); cursor["y"] = Integer(v, "y", 0, YRES-1);
    cursor["radius"] = Integer(v, "radius", 0, 16);
    auto now = MonotonicSeconds();
    std::lock_guard lock(p->mutex);
    p->cursorTokens = std::min(20.0, p->cursorTokens+(now-p->cursorRefill)*20); p->cursorRefill = now;
    if (p->cursorTokens < 1) return; // Cosmetic updates cannot flood command replies.
    --p->cursorTokens; p->cursor = std::move(cursor); p->cursorAt = now;
    return;
}
void Room::Probe(const std::shared_ptr<Peer> &p, const Json::Value &v)
{
    auto receivedUs = MonotonicMicros();
    if (!v["id"].isUInt64() || !v["id"].asUInt64()) throw std::runtime_error("invalid probe ID");
    if (MonotonicSeconds()-p->lastProbe < .25) throw std::runtime_error("probe rate exceeded");
    p->lastProbe = MonotonicSeconds();
    Json::Value response;
    { std::lock_guard lock(telemetryMutex); response = telemetry; }
    response["type"] = "telemetry"; response["id"] = v["id"];
    response["serverReceivedUs"] = Json::UInt64(receivedUs);
    response["bandwidthKbps"] = config.bandwidthKbps;
    response["uploadKbps"] = config.uploadKbps;
    response["shareKbps"] = upload.Share(p->id)*8/1000;
    response["bandwidthLimited"] = MonotonicSeconds()-p->lastBandwidthWait.load() < 2;
    {
        std::lock_guard lock(p->mutex);
        response["inFlight"] = unsigned(p->inFlight.size());
        response["streamWindow"] = p->pacer.Window();
        response["pacingHz"] = p->pacer.Hz();
        response["frameAckMs"] = p->pacer.AckMs();
        response["networkPaced"] = p->pacer.Limited();
    }
    p->Text(std::move(response)); return;
}
// Diagnostic only: a replica found a fingerprint mismatch and has
// already discarded its world. Never affects authoritative state.
void Room::DesyncReport(const std::shared_ptr<Peer> &p, const Json::Value &v)
{
    if (!p->replica) throw std::runtime_error("desync reports require local simulation");
    if (MonotonicSeconds()-p->lastDesyncReport < 1) throw std::runtime_error("desync report rate exceeded");
    p->lastDesyncReport = MonotonicSeconds();
    if (!v["tick"].isUInt64() || !v["revision"].isUInt64() || !v["fields"].isObject() || v["fields"].size() > 32)
        throw std::runtime_error("invalid desync report");
    auto tick = v["tick"].asUInt64(), revision = v["revision"].asUInt64();
    Json::Value detail; detail["type"] = "desync_detail"; detail["tick"] = Json::UInt64(tick);
    detail["fields"] = Json::Value(Json::arrayValue); detail["retained"] = false;
    {
        std::lock_guard lock(digestMutex);
        for (auto &[at, atRevision, fields] : recentDigests) if (at == tick && atRevision == revision)
        {
            detail["retained"] = true;
            for (const auto &name : fields.getMemberNames())
            {
                const auto &local = v["fields"][name];
                if (!local.isString() || local.asString() != fields[name].asString())
                    detail["fields"].append(name);
            }
        }
    }
    std::string groups;
    for (const auto &g : detail["fields"]) groups += (groups.empty() ? "" : ",")+g.asString();
    std::cout << "Replica player " << p->id << " diverged at tick " << tick << ": "
        << (detail["retained"].asBool() ? "differing groups=" + (groups.empty() ? std::string("none") : groups)
                                       : std::string("tick no longer retained")) << std::endl;
    p->Text(std::move(detail)); return;
}
void Room::QueueRequest(const std::shared_ptr<Peer> &p, const Json::Value &v, uint64_t &seq)
{
    seq = Sequence(v);
    if (seq <= p->lastSequence) { p->Error("duplicate or stale sequence", seq); return; }
    // A rejected command also consumes its sequence, preventing ambiguous retries.
    p->lastSequence = seq;
    Request request; request.peer = p; request.command.sequence = seq;
    unsigned cost = 1;
    if (v["op"] == "admin")
    {
        if (!p->admin) throw std::runtime_error("administrator permission required");
        if (!v["action"].isString()) throw std::runtime_error("invalid administration action");
        if (v["action"] == "kick")
        {
            request.administration = Request::Administration::Kick;
            request.target = uint32_t(Integer(v, "player", 1, INT32_MAX));
        }
        else if (v["action"] == "lock")
        {
            if (!v["locked"].isBool()) throw std::runtime_error("invalid locked flag");
            request.administration = Request::Administration::Lock;
            request.locked = v["locked"].asBool();
        }
        else throw std::runtime_error("invalid administration action");
    }
    else
    {
        if (v["op"] == "paste")
        {
            // The uploaded save becomes part of the command, and of its commit.
            if (!v["upload"].isUInt64() || v["upload"].asUInt64() != p->pasteId || p->pasteSave.empty())
                throw std::runtime_error("paste without its uploaded save");
            auto withSave = v;
            withSave["save"] = std::move(p->pasteSave);
            withSave.removeMember("upload");
            p->pasteSave.clear(); p->pasteId = 0;
            request.command = ParseCommand(withSave, p->admin);
        }
        else request.command = ParseCommand(v, p->admin);
        // A player only ever steers their own stickman.
        if (request.command.kind == Command::Stickman || request.command.kind == Command::StickmanSpawn)
            request.command.slot = p->stickmanSlot;
        cost = request.command.Cost();
    }
    auto now = Clock::now();
    double elapsed = std::chrono::duration<double>(now - p->refill).count(); p->refill = now;
    p->operations = std::min(120.0, p->operations + elapsed * 120);
    p->work = std::min(EditWorkPerSecond, p->work + elapsed * EditWorkPerSecond);
    if (p->operations < 1 || p->work < cost) throw std::runtime_error("editing rate exceeded");
    p->operations -= 1; p->work -= cost;
    std::lock_guard lock(queueMutex);
    if (queue.size() >= 256 || p->pending >= 64) throw std::runtime_error("command queue full");
    ++p->pending; queue.push_back(std::move(request));
    queueChanged.notify_one();
}
std::vector<std::shared_ptr<Peer>> Room::CollectPeers()
{
    std::vector<std::shared_ptr<Peer>> active;
    {
        std::lock_guard lock(peersMutex);
        for (auto &p : peers)
        {
            if (!p->authenticated && Clock::now() - p->connected > std::chrono::seconds(5))
            { p->closed = true; p->changed.notify_one(); }
            if (p->socket.expired()) { p->closed = true; p->changed.notify_one(); }
            if (p->authenticated && !p->closed) active.push_back(p);
        }
        // Completed writers can be joined immediately; never wait for a slow socket here.
        std::erase_if(peers, [](auto &p) { return p->closed && p->senderDone; });
    }
    return active;
}
void Room::BroadcastPresence(const std::vector<std::shared_ptr<Peer>> &active)
{
    if (MonotonicSeconds() >= nextPresence)
    {
        auto now = MonotonicSeconds(); nextPresence = now+.1;
        Json::Value roster; roster["type"] = "presence"; roster["players"] = Json::Value(Json::arrayValue);
        for (auto &p : active)
        {
            std::lock_guard lock(p->mutex);
            Json::Value player = p->cursor;
            player["player"] = p->id; player["name"] = p->name; player["admin"] = bool(p->admin);
            player["stickman"] = p->stickmanSlot.load();
            player["visible"] = !p->cursor.isNull() && p->cursor["visible"].asBool() && now-p->cursorAt < 2;
            roster["players"].append(player);
        }
        for (auto &p : active) if (p->wantsPresence)
        {
            std::lock_guard lock(p->mutex);
            p->presence = roster; p->changed.notify_one(); // Replace unsent cosmetic state.
        }
    }
}
void Room::Commit(const Command &command, uint32_t actor, const std::vector<std::shared_ptr<Peer>> &active)
{
    auto beforeTick = world.tick, beforeRevision = world.revision;
    world.Apply(command);
    EditStamp stamp;
    try
    {
        stamp = timeline.Commit(beforeTick, world.tick, beforeRevision,
            world.revision, world.resetGeneration);
        if (journal) journal->Edit(stamp, actor, command);
    }
    catch (...) { failed = true; throw; } // Fail closed after an unjournaled mutation.
    // The server has a total ordering even when multiple edits
    // are applied at the same (paused) simulation tick.
    Json::Value event;
    event["type"] = "command_commit";
    event["id"] = Json::UInt64(stamp.eventId);
    event["epoch"] = Json::UInt64(stamp.epoch);
    event["tick"] = Json::UInt64(stamp.tick);
    event["ordinal"] = Json::UInt64(stamp.ordinal);
    event["afterTick"] = Json::UInt64(stamp.afterTick);
    event["revision"] = Json::UInt64(stamp.revision);
    event["resetGeneration"] = Json::UInt64(world.resetGeneration);
    event["actor"] = actor; // 0: the server itself
    event["command"] = RecordCommand(command);
    // The current canvas clients do not consume commits;
    // future lockstep clients can explicitly opt in.
    for (auto &viewer : active)
        if (viewer->authenticated && !viewer->closed && viewer->wantsCommits &&
            (!viewer->replica || !viewer->needsReplicaBootstrap))
            viewer->Text(event);
}
void Room::ReleaseStickmen(const std::vector<std::shared_ptr<Peer>> &active)
{
    // A stickman whose player left (or whose slot changed hands) stops moving
    // (only if it is moving: no command for nothing).
    for (int slot = 1; slot <= 4; ++slot)
    {
        uint32_t holder = 0;
        for (auto &p : active) if (p->stickmanSlot == slot) holder = p->id;
        auto *stickman = RoomStickman(*world.sim, slot);
        if (stickmanHolders[slot] && stickmanHolders[slot] != holder && stickman && stickman->comm)
        {
            Command stop{Command::Stickman, ++serverSequence};
            stop.slot = slot;
            Commit(stop, 0, active);
        }
        stickmanHolders[slot] = holder;
    }
}
void Room::ApplyRequests(const std::vector<std::shared_ptr<Peer>> &active)
{
    ReleaseStickmen(active);
    std::deque<Request> pending;
    { std::lock_guard lock(queueMutex); pending.swap(queue); }
    for (auto &r : pending)
    {
        --r.peer->pending;
        if (r.peer->closed) continue;
        try
        {
            if (r.administration == Request::Administration::Lock)
            {
                roomLocked = r.locked;
                Json::Value state;
                state["type"] = "room"; state["locked"] = r.locked;
                std::lock_guard lock(peersMutex);
                for (auto &p : peers) if (p->authenticated && !p->closed) p->Text(state);
                std::cout << "Room " << (r.locked ? "locked" : "unlocked") << " by player " << r.peer->id << std::endl;
            }
            else if (r.administration == Request::Administration::Kick)
            {
                std::shared_ptr<Peer> target;
                {
                    std::lock_guard lock(peersMutex);
                    for (auto &p : peers) if (p->id == r.target && p->authenticated && !p->closed)
                    { target = p; break; }
                }
                if (!target) throw std::runtime_error("player not connected");
                if (target == r.peer) throw std::runtime_error("cannot remove yourself");
                if (target->admin) throw std::runtime_error("cannot remove another administrator");
                if (auto ws = target->socket.lock()) ws->close(1008, "removed by room administrator");
                target->closed = true; target->changed.notify_one();
                std::cout << "Player " << r.target << " removed by player " << r.peer->id << std::endl;
            }
            else Commit(r.command, r.peer->id, active);
            Json::Value ack;
            ack["type"] = "ack"; ack["seq"] = Json::UInt64(r.command.sequence);
            ack["tick"] = Json::UInt64(world.tick); ack["revision"] = Json::UInt64(world.revision);
            r.peer->Text(ack);
        }
        catch (const std::exception &e) { r.peer->Error(e.what(), r.command.sequence); }
    }
}
void Room::Advance(const std::vector<std::shared_ptr<Peer>> &active)
{
    if (!world.paused && !active.empty())
    {
        auto before = MonotonicSeconds(); world.Advance(); if (journal) journal->Check(world); tickMsTotal += (MonotonicSeconds()-before)*1000; ++measuredSteps;
    }
}
void Room::BootstrapReplicas(const std::vector<std::shared_ptr<Peer>> &active)
{
    // Bootstrap replicas only at a quiescent physics boundary, after
    // processing this iteration's edits and physics. All events from
    // earlier iterations are included in the snapshot itself.
    bool bootstrap = false;
    for (auto &p : active) if (p->replica && p->needsReplicaBootstrap && !p->closed)
        bootstrap = true;
    if (bootstrap)
    {
        try
        {
            // Only the raw capture runs here; compression happens on the sender threads.
            auto began = MonotonicSeconds();
            auto snapshot = std::make_shared<CheckpointSource>(CaptureExactCheckpointRaw(world, snapshotBuffers->Take()), snapshotBuffers);
            snapshotCaptures += 1; snapshotCaptureMs += (MonotonicSeconds()-began)*1000;
            for (auto &p : active) if (p->replica && p->needsReplicaBootstrap && !p->closed)
            {
                p->QueueCheckpoint(snapshot,world.tick,world.revision,world.resetGeneration,world.paused);
                // A newly joined paused replica otherwise never receives a
                // tick seal (global tick/revision may not have changed).
                Json::Value initialSeal;
                initialSeal["type"] = "tick_seal";
                initialSeal["tick"] = Json::UInt64(world.tick);
                initialSeal["revision"] = Json::UInt64(world.revision);
                initialSeal["resetGeneration"] = Json::UInt64(world.resetGeneration);
                initialSeal["paused"] = world.paused;
                auto fields = DiagnosticDigests(world); RememberDigest(world.tick, world.revision, fields);
                initialSeal["digest"] = DigestSummary(fields);
                p->Text(initialSeal);
                p->needsReplicaBootstrap = false;
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Replica bootstrap unsupported: " << e.what() << '\n';
            // No image fallback in this experimental client: fail clearly.
            for (auto &p : active) if (p->replica && p->needsReplicaBootstrap)
            { p->Error(std::string("local simulation unavailable: ")+e.what()); p->closed = true; p->changed.notify_one(); }
        }
    }
}
void Room::SealTick(const std::vector<std::shared_ptr<Peer>> &active)
{
    // A seal certifies that all edits for the preceding interval have
    // been sent. Checkpoints and subsequent edits share FIFO ordering.
    // Hashing every 60 physics ticks keeps state validation inexpensive.
    // A paused edit also gets a digest on its next seal.
    if (world.tick != lastSealedTick || world.revision != lastSealedRevision)
    {
        Json::Value seal;
        seal["type"] = "tick_seal";
        seal["tick"] = Json::UInt64(world.tick);
        seal["revision"] = Json::UInt64(world.revision);
        seal["resetGeneration"] = Json::UInt64(world.resetGeneration);
        seal["paused"] = world.paused;
        if (world.tick % 60 == 0 || (world.paused && world.revision != lastSealedRevision))
        {
            auto fields = DiagnosticDigests(world); RememberDigest(world.tick, world.revision, fields);
            seal["digest"] = DigestSummary(fields);
        }
        for (auto &p : active) if (p->authenticated && !p->closed && p->replica && !p->needsReplicaBootstrap)
            p->Text(seal);
        lastSealedTick = world.tick; lastSealedRevision = world.revision;
    }
}
void Room::Measure()
{
    if (MonotonicSeconds()-measuredAt >= 1)
    {
        auto now = MonotonicSeconds();
        simHz = (world.tick-measuredTick)/(now-measuredAt);
        tickMs = measuredSteps ? tickMsTotal/measuredSteps : 0;
        measuredAt = now; measuredTick = world.tick; measuredSteps = 0; tickMsTotal = 0;
    }
    {
        std::lock_guard lock(telemetryMutex);
        telemetry["simHz"] = simHz; telemetry["tickMs"] = tickMs;
        telemetry["paused"] = world.paused; telemetry["revision"] = Json::UInt64(world.revision);
    }
}
void Room::PublishFrames(const std::vector<std::shared_ptr<Peer>> &active)
{
    unsigned views = 0;
    for (auto &p : active) if (!p->replica) views |= 1U << p->view.load();
    if (views && Clock::now() >= nextFrame)
    {
        if (world.tick != capturedTick || world.revision != capturedRevision || (views & ~capturedViews))
        {
            producer.Submit(*world.sim, ++capture, world.tick, world.revision, world.paused, views, world.resetGeneration);
            capturedTick = world.tick; capturedRevision = world.revision; capturedViews = views;
        }
        nextFrame += framePeriod;
        if (nextFrame < Clock::now()) nextFrame = Clock::now() + framePeriod;
    }
    if (views) if (auto rendered = producer.Latest())
        for (auto &p : active) if (!p->replica) if (auto frame = (*rendered)[p->view.load()]) p->Publish(frame);
}
void Room::Autosave()
{
    if (autosaves)
    {
        auto status = autosaves->GetStatus();
        if (status.committed != reportedSave)
        {
            reportedSave = status.committed;
            std::cout << "Autosave committed generation=" << reportedSave << " revision=" << status.revision << std::endl;
        }
        if (status.error != reportedSaveError)
        {
            reportedSaveError = status.error;
            if (!status.error.empty()) std::cerr << "Autosave failed (will retry): " << status.error << std::endl;
        }
        if (Clock::now() >= nextAutosave)
        {
            if (!status.busy && (savedTick != world.tick || savedRevision != world.revision || !status.error.empty()))
            {
                try
                {
                    autosaves->Submit(world.CaptureSave(), world.revision);
                    savedTick = world.tick; savedRevision = world.revision;
                }
                catch (const std::exception &e) { std::cerr << "Autosave capture failed: " << e.what() << std::endl; }
            }
            nextAutosave = Clock::now() + std::chrono::seconds(config.autosaveInterval);
        }
    }
}
void Room::Stats(const std::vector<std::shared_ptr<Peer>> &active)
{
    if (Clock::now() >= nextStats)
    {
        uint64_t sent = 0, full = 0, delta = 0, repeats = 0, webp = 0;
        for (auto &p : active) { sent += p->bytesSent.load(); full += p->fullFrames; delta += p->deltaFrames; repeats += p->repeats; webp += p->webpFrames; }
        std::cout << "tick=" << world.tick << " revision=" << world.revision << " players=" << active.size()
            << " sim_hz=" << simHz << " tick_ms=" << tickMs
            << " full=" << full << " delta=" << delta << " repeat=" << repeats << " webp=" << webp
            << " active_connection_bytes_sent=" << sent
            << " longest_step_ms=" << longestStepMs << " snapshots=" << snapshotCaptures
            << " snapshot_capture_ms=" << (snapshotCaptures ? snapshotCaptureMs/snapshotCaptures : 0.0) << std::endl;
        longestStepMs = 0; snapshotCaptures = 0; snapshotCaptureMs = 0;
        nextStats = Clock::now() + std::chrono::seconds(5);
    }
}
bool Room::WaitForRequests(std::chrono::steady_clock::time_point until)
{
    std::unique_lock lock(queueMutex);
    return queueChanged.wait_until(lock, until, [&] { return !queue.empty(); });
}
void Room::ApplyEarly()
{
    // Between physics ticks: apply edits at the current tick as soon as they
    // arrive, instead of on the next Step(). They follow this tick's seal, as
    // edits applied by Step() do, so replicas see the same order.
    auto active = CollectPeers();
    ApplyRequests(active);
}
void Room::Step()
{
    auto began = MonotonicSeconds();
    auto active = CollectPeers();
    BroadcastPresence(active);
    ApplyRequests(active);
    Advance(active);
    BootstrapReplicas(active);
    SealTick(active);
    Measure();
    PublishFrames(active);
    Autosave();
    longestStepMs = std::max(longestStepMs, (MonotonicSeconds()-began)*1000);
    Stats(active);
}
}
