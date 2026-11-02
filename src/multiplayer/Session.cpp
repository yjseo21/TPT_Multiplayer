#include "Session.h"
#include "CheckpointWire.h"
#include "Presence.h"
#include "Protocol.h"
#include "simulation/Simulation.h"
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

namespace multiplayer
{
#ifndef _WIN32
namespace
{
std::string Env(const char *name) { auto p = std::getenv(name); return p ? p : ""; }
}
#endif
namespace
{
// Local physics per frame while catching up, at the least (see Pump), so a frame stays responsive.
constexpr double CatchUpBudget = .012;
// Unable to catch up for this long, a computer jumps to the room's present;
// at most this often, so a merely busy moment never causes repeated rejoins.
constexpr double SkipAheadSeconds = 10, SkipAheadInterval = 30;
}
// Written by the socket's callback thread, drained by Update().
struct Session::Mailbox
{
    std::mutex mutex;
    std::string failure;
    std::deque<std::string> packets;
    struct Control { Json::Value value; double received; };
    std::deque<Control> messages;
    // Local simulation needs JSON and binary checkpoint chunks in their exact
    // wire order, so it uses one ordered mailbox instead of the two above.
    struct Inbound { bool binary; std::string bytes; double received; };
    std::deque<Inbound> ordered;
    uint64_t incomingBytes = 0;
    bool disconnected = false, permanentFailure = false;
};

Session::Session(SessionOptions o) :
    options(std::move(o)), mailbox(std::make_unique<Mailbox>()), socket(std::make_unique<ix::WebSocket>()),
    began(MonotonicSeconds()), metrics(began), desiredView(options.view), lastReceived(began)
{
    ix::initNetSystem();
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    socket->setUrl(options.url);
    socket->disablePerMessageDeflate(); socket->disableAutomaticReconnection(); socket->setHandshakeTimeout(5);
    ix::SocketTLSOptions tls;
    if (!options.ca.empty()) tls.caFile = options.ca;
#ifndef _WIN32
    else
    {
        // Mbed TLS does not discover Unix trust stores itself. An explicit CA file
        // remains available for private CAs; never disable verification.
        auto configured = Env("SSL_CERT_FILE");
        if (!configured.empty()) tls.caFile = configured;
        else for (const auto *path : {"/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt"})
            if (std::filesystem::is_regular_file(path)) { tls.caFile = path; break; }
    }
#endif
    socket->setTLSOptions(tls);
    auto *box = mailbox.get();
    auto *ws = socket.get();
    socket->setOnMessageCallback([this, box, ws](const ix::WebSocketMessagePtr &msg) {
        try
        {
            if (msg->type == ix::WebSocketMessageType::Open)
            {
                Json::Value hello;
                hello["type"] = "hello"; hello["protocol"] = ProtocolVersion; hello["build"] = BuildId;
                hello["name"] = options.name; hello["token"] = options.roomToken; hello["adminToken"] = options.adminToken;
                hello["presence"] = true;
                hello["webp"] = !options.localSim && SupportsWebP();
                if (options.localSim) hello["localSimulation"] = true;
                ws->sendText(JsonText(hello));
            }
            else if (msg->type == ix::WebSocketMessageType::Message)
            {
                std::lock_guard lock(box->mutex);
                box->incomingBytes += msg->str.size();
                if (options.localSim)
                {
                    // Bounded: one 64 MiB checkpoint in 32 KiB chunks plus queued seals.
                    if ((msg->binary && msg->str.size() > CheckpointWireHeaderBytes+CheckpointChunkBytes) ||
                        (!msg->binary && msg->str.size() > MaxOrderedBytes) || box->ordered.size() >= 4096)
                        throw std::runtime_error("local simulation receive limit exceeded");
                    box->ordered.push_back({msg->binary, msg->str, MonotonicSeconds()});
                }
                else if (msg->binary)
                {
                    if (msg->str.size() > MaxCanvasPacket || box->packets.size() >= 8)
                        throw std::runtime_error("canvas receive limit exceeded");
                    box->packets.push_back(msg->str);
                }
                else
                {
                    if (box->messages.size() >= 128) throw std::runtime_error("too many server messages");
                    box->messages.push_back({ParseJson(msg->str), MonotonicSeconds()});
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Error || msg->type == ix::WebSocketMessageType::Close)
            {
                std::lock_guard lock(box->mutex); box->disconnected = true;
                box->failure = msg->type == ix::WebSocketMessageType::Error ? msg->errorInfo.reason : msg->closeInfo.reason;
                if (msg->type == ix::WebSocketMessageType::Close)
                    box->permanentFailure = msg->closeInfo.code == 1008 || msg->closeInfo.code == 1002 || msg->closeInfo.code == 1009;
                else
                {
                    auto reason = box->failure;
                    std::transform(reason.begin(), reason.end(), reason.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                    box->permanentFailure = reason.find("certificate") != std::string::npos || reason.find("x509") != std::string::npos;
                }
            }
        }
        catch (const std::exception &e)
        {
            std::lock_guard lock(box->mutex); box->failure = e.what(); box->disconnected = true;
            box->permanentFailure = std::string_view(e.what()).find("receive limit") == std::string_view::npos;
        }
    });
    socket->start();
}
Session::~Session()
{
    socket->stop();
    socket.reset();
    ix::uninitNetSystem();
}
void Session::Stop() { socket->stop(); }
void Session::Fail(const std::string &reason)
{
    std::lock_guard lock(mailbox->mutex); mailbox->failure = reason; mailbox->disconnected = true;
}
bool Session::Disconnected() const
{
    std::lock_guard lock(mailbox->mutex); return mailbox->disconnected;
}
size_t Session::Buffered() const
{
    return socket ? socket->bufferedAmount() : 0;
}
bool Session::SendControl(const Json::Value &value)
{
    if (Disconnected()) return false;
    if (!socket->sendText(JsonText(value)).success) { Fail("could not send message"); return false; }
    return true;
}
bool Session::Synchronized() const
{
    if (options.localSim) return ready && replica && replica->Ready() && !desyncWaitUntil;
    return ready && canvas.id && !waitingForKeyframe && canvas.frame.view == desiredView;
}
bool Session::Send(Json::Value command)
{
    bool live = options.localSim ? ready && replica && replica->Ready() && !desyncWaitUntil
                                 : ready && canvas.id && !waitingForKeyframe;
    if (!live) return false;
    if (socket->bufferedAmount() > 64*1024 || edits.Pending() >= 64)
    { Fail("connection cannot keep up with input"); return false; }
    auto seq = edits.Begin();
    command["seq"] = Json::UInt64(seq);
    if (options.localSim && sentAt.size() < 1024) sentAt[seq] = MonotonicSeconds();
    return SendControl(command);
}
uint64_t Session::Lead() const
{
    double gap = sealBatchMs/1000.0;
    if (!sealGaps.empty())
    {
        gap = 0;
        for (auto &entry : sealGaps) gap = std::max(gap, entry.second);
    }
    return std::max<uint64_t>(1, uint64_t(gap*tickRate+.5));
}
void Session::SendCursor(bool visible, int x, int y, int radius, bool drawing)
{
    auto now = MonotonicSeconds();
    if (!ready || !presenceEnabled || now < cursorAt) return;
    cursorAt = now+.1;
    Json::Value c; c["type"] = "cursor";
    c["visible"] = visible && x >= 0 && x < XRES && y >= 0 && y < YRES && Synchronized();
    c["x"] = std::clamp(x, 0, XRES-1); c["y"] = std::clamp(y, 0, YRES-1);
    c["radius"] = std::clamp(radius, 0, 16); c["drawing"] = drawing; // cursor outline only
    if ((c != lastCursor || now-cursorSent >= 1) && socket->bufferedAmount() < 4096 && SendControl(c))
    { lastCursor = c; cursorSent = now; }
}
void Session::SetView(int view)
{
    desiredView = view;
    if (ready && !options.localSim)
    {
        Json::Value request; request["type"] = "view"; request["view"] = view; SendControl(request);
    }
}
void Session::PublishLocalPicture(CanvasFrame frame)
{
    canvas.id = canvas.id+1; canvas.frame = std::move(frame);
    pictureChanged = true; failures = 0; metrics.Frame();
}
void Session::RetireReplica()
{
    if (replica) { sealsBefore += replica->Seals(); checksBefore += replica->Checks(); editsBefore += replica->Edits(); }
    replica.reset();
}
void Session::Lost()
{
    std::string reason; bool fatal;
    { std::lock_guard lock(mailbox->mutex); reason = mailbox->failure; fatal = mailbox->permanentFailure; }
    if (fatal || !options.reconnect) throw std::runtime_error("disconnected: " + reason);
    // Stop/join before clearing mailboxes or restarting. No old frames, commands
    // or callbacks may cross into the next connection's baseline/sequence space.
    socket->stop();
    {
        std::lock_guard lock(mailbox->mutex);
        mailbox->packets.clear(); mailbox->messages.clear(); mailbox->ordered.clear();
        mailbox->incomingBytes = 0; mailbox->disconnected = mailbox->permanentFailure = false;
    }
    RetireReplica(); desyncWaitUntil = 0; behindSince = 0; behindSeconds = 0;
    edits.Disconnect(); sentAt.clear();
    ready = admin = roomLocked = waitingForKeyframe = false;
    canvas.id = 0; attempting = false;
    players = Json::Value(Json::arrayValue); presenceEnabled = false; lastCursor = Json::Value();
    metrics = ClientMetrics(MonotonicSeconds());
    double delay = std::min(10.0, double(1U << std::min(failures++, 4U)));
    retryAt = MonotonicSeconds()+delay;
    std::cout << "Connection interrupted (" << reason << "); retry in " << delay << " seconds. Unconfirmed edits are not replayed.\n" << std::flush;
}
void Session::HandleControl(const Json::Value &v, double receivedAt)
{
    if (v["type"] == "welcome")
    {
        if (v["build"] != BuildId || !v["protocol"].isUInt() || v["protocol"].asUInt() != ProtocolVersion)
            throw std::runtime_error("server build mismatch");
        ready = true; admin = v["admin"].asBool(); roomLocked = v["roomLocked"].asBool();
        ownPlayer = v["player"].asUInt(); presenceEnabled = v["presence"].asBool();
        ownStickman = v.isMember("stickman") ? Integer(v, "stickman", 0, 4) : 0;
        toolActionsSupported = v["toolActions"].asBool();
        if (options.localSim && !(v["localSimulation"].isBool() && v["localSimulation"].asBool()))
            throw std::runtime_error("server does not support local simulation");
        sealBatchMs = v["sealBatchMs"].isUInt() ? v["sealBatchMs"].asUInt() : 0;
        ++connections;
        std::cout << "Connected as player " << v["player"].asUInt() << (admin ? " (admin)" : "") << std::endl;
        if (desiredView && !options.localSim) SetView(desiredView);
    }
    else if (v["type"] == "ack" || v["type"] == "error")
    {
        if (v["seq"].isUInt64()) edits.Acknowledge(v["seq"].asUInt64());
        if (v["type"] == "ack") acknowledgedRevision = std::max(acknowledgedRevision, uint64_t(v["revision"].asUInt64()));
        if (v["type"] == "error")
        {
            std::cerr << "Server: " << v["message"].asString() << '\n';
            if (options.serverErrorsFatal) throw std::runtime_error("script command rejected: "+v["message"].asString());
        }
    }
    else if (v["type"] == "telemetry") metrics.Telemetry(v, receivedAt);
    else if (v["type"] == "presence") { players = PresencePlayers(v); presenceAt = receivedAt; }
    else if (v["type"] == "room" && v["locked"].isBool()) roomLocked = v["locked"].asBool();
    else if (v["type"] == "desync_detail" && desyncWaitUntil)
    {
        std::string groups;
        for (const auto &g : v["fields"]) groups += (groups.empty() ? "" : ", ")+g.asString();
        std::cout << "Server comparison at tick " << v["tick"].asUInt64() << ": differing state groups: "
            << (groups.empty() ? std::string(v["retained"].asBool() ? "none" : "(tick no longer retained)") : groups) << std::endl;
        desyncWaitUntil = 0;
        Fail("local simulation resynchronizing after divergence");
    }
}
void Session::Diverged(const ReplicaDesync &e)
{
    ++resyncs;
    std::cerr << "Local simulation diverged at tick " << e.tick << "; discarding local world and resynchronizing" << std::endl;
    Json::Value report; report["type"] = "desync_report"; report["tick"] = Json::UInt64(e.tick); report["revision"] = Json::UInt64(e.revision);
    report["fields"] = e.localFields;
    RetireReplica();
    canvas.id = 0;
    // Ask the server which state groups differ; reconnect on reply or after 2 s.
    if (SendControl(report)) desyncWaitUntil = MonotonicSeconds()+2;
    else Fail("local simulation diverged");
}
void Session::ReplicaFailed(const std::exception &e)
{
    // Protocol/ordering failures also invalidate the local world. Failures
    // before the first synchronization (unsupported server, rejected
    // checkpoint) would repeat on every attempt, so they are permanent.
    bool synchronized = replica && replica->Ready();
    if (synchronized) ++resyncs;
    std::cerr << "Local simulation error: " << e.what() << std::endl;
    RetireReplica(); canvas.id = 0;
    Fail(std::string("local simulation error: ")+e.what());
    if (!synchronized) { std::lock_guard lock(mailbox->mutex); mailbox->permanentFailure = true; }
}
void Session::HandleOrdered(bool binary, const std::string &bytes, double received)
{
    lastReceived = MonotonicSeconds();
    if (desyncWaitUntil)
    {
        // The diverged world is already discarded; only wait for the server's comparison.
        if (!binary) HandleControl(ParseJson(bytes, MaxOrderedBytes), received);
        return;
    }
    if (binary)
    {
        if (!replica) throw std::runtime_error("checkpoint data before welcome");
        replica->OnBinary(bytes);
        return;
    }
    auto v = ParseJson(bytes, MaxOrderedBytes);
    auto type = v["type"].asString();
    HandleControl(v, received);
    if (type == "welcome")
    {
        RetireReplica(); replica = std::make_unique<ReplicaState>(); replica->SetPaced(true);
        tickCredit = 0; rateSince = 0;
        lastSealed = 0; lastSealAdvance = 0; pausedSinceSeal = false; sealGaps.clear();
        ownCommitted = 0;
        replica->committed = [this](const Json::Value &commit) {
            if (commit["actor"].asUInt() != ownPlayer) return;
            ownCommitted = std::max(ownCommitted, commit["command"]["seq"].asUInt64());
            auto it = sentAt.find(commit["command"]["seq"].asUInt64());
            if (it == sentAt.end()) return;
            auto ms = (MonotonicSeconds()-it->second)*1000;
            sentAt.erase(sentAt.begin(), std::next(it));
            latencies.push_back(ms); ++latencySamples; latencyTotalMs += ms;
            latencyMaxMs = std::max(latencyMaxMs, ms);
        };
    }
    if (type == "welcome" || type == "checkpoint_begin" || type == "checkpoint_end" ||
        type == "command_commit" || type == "tick_seal")
    {
        if (!replica) throw std::runtime_error("simulation stream before welcome");
        replica->OnJson(v);
        if (type == "checkpoint_end")
        {
            failures = 0;
            std::cout << "Local simulation synchronized at tick " << replica->world.tick
                << " revision " << replica->world.revision << std::endl;
        }
    }
}
void Session::Pump()
{
    auto now = MonotonicSeconds();
    double dt = lastPump ? std::clamp(now-lastPump, 0.0, .25) : 0;
    lastPump = now;
    if (!replica || !replica->Ready() || desyncWaitUntil) return;
    try
    {
        auto sealed = replica->world.tick+replica->Backlog();
        // Seal arrival gaps set the margin. Gaps that span a pause say nothing
        // about the network, so they are skipped.
        if (replica->world.paused) pausedSinceSeal = true;
        if (sealed > lastSealed)
        {
            if (lastSealAdvance && !pausedSinceSeal) sealGaps.push_back({now, std::min(now-lastSealAdvance, .5)});
            lastSealed = sealed; lastSealAdvance = now; pausedSinceSeal = false;
        }
        while (!sealGaps.empty() && now-sealGaps.front().first > 3) sealGaps.pop_front();
        // Estimate the server's step rate from seal progress; keep the
        // previous estimate while paused.
        if (!rateSince) { rateSince = now; rateTick = sealed; }
        else if (now-rateSince >= 1)
        {
            if (sealed > rateTick) tickRate = std::clamp((sealed-rateTick)/(now-rateSince), 1.0, 60.0);
            rateSince = now; rateTick = sealed;
        }
        auto backlog = replica->Backlog();
        auto lead = Lead();
        tickCredit += dt*tickRate;
        // Far behind (slow frame, reconnect catch-up): drop back to one batch.
        if (backlog > 2*lead+2) tickCredit = std::max(tickCredit, double(backlog-lead));
        auto steps = std::min<uint64_t>(backlog, uint64_t(tickCredit));
        // While one of our own edits is in flight, close the margin at up to one
        // extra tick per frame, so the edit shows as soon as its tick is sealed.
        // Bounded: a heavy world never runs more than two steps in one frame.
        if (!sentAt.empty() && steps < backlog) steps = std::min<uint64_t>(backlog, std::max<uint64_t>(steps, 2));
        // Local physics per frame: at most three quarters of the time since the
        // last frame (at least CatchUpBudget, and always one tick when due). A
        // computer that cannot keep up stays responsive and falls behind visibly
        // instead of freezing ever longer on catch-up; one that merely draws few
        // frames a second still keeps up.
        auto budget = std::max(CatchUpBudget, .75*dt);
        uint64_t ran = 0;
        if (!steps) replica->Pump(0); // edits and seals at the current tick
        while (ran < steps)
        {
            if (options.debugStepDelayMs) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(options.debugStepDelayMs));
            auto stepped = replica->Pump(1);
            if (!stepped) break;
            ran += stepped;
            if (MonotonicSeconds()-now > budget) break;
        }
        tickCredit = std::max(tickCredit-double(ran), -1.0);
        if (ran == backlog) tickCredit = std::min(tickCredit, 1.0);
        // Behind: more than half a second of ticks past the margin. What counts is
        // how long it has been unable to catch up; rates measured across pauses
        // and frame steps say nothing about this computer's speed.
        if (replica->Backlog() <= lead + 30) behindSince = 0;
        else if (!behindSince) behindSince = now;
        behindSeconds = behindSince ? now-behindSince : 0;
        if (behindSeconds > SkipAheadSeconds && now-lastSkipAhead > SkipAheadInterval)
        {
            // Rejoin from a fresh snapshot of the room's present. Cheap for the
            // room: only a raw copy happens on its thread.
            lastSkipAhead = now; ++skipAheads;
            std::cerr << "This computer has been behind the room for " << int(behindSeconds) << " s; jumping ahead" << std::endl;
            RetireReplica(); canvas.id = 0;
            Fail("this computer cannot keep up with the room; jumping to its present");
            return;
        }
        if (options.debugCorruptTick && !corruptionInjected && replica->world.tick >= options.debugCorruptTick)
        {
            // Test hook: silently change local physics state once. The next
            // fingerprint must detect it and trigger a full resynchronization.
            corruptionInjected = true;
            replica->world.sim->pv[YCELLS/2][XCELLS/2] += 1.0f;
            std::cout << "DEBUG: perturbed local pressure at tick " << replica->world.tick << std::endl;
        }
    }
    catch (const ReplicaDesync &e) { Diverged(e); }
    catch (const std::exception &e) { ReplicaFailed(e); }
}
void Session::Update()
{
    std::deque<std::string> incoming;
    std::deque<Mailbox::Control> controls;
    std::deque<Mailbox::Inbound> ordered;
    bool lost;
    {
        std::lock_guard lock(mailbox->mutex);
        lost = mailbox->disconnected;
        if (!lost) { incoming.swap(mailbox->packets); controls.swap(mailbox->messages); ordered.swap(mailbox->ordered); }
        metrics.Traffic(mailbox->incomingBytes); mailbox->incomingBytes = 0;
    }
    if (lost) Lost();
    if (!attempting && MonotonicSeconds() >= retryAt)
    {
        attempting = true; lastReceived = MonotonicSeconds(); socket->start();
    }
    for (auto &control : controls) HandleControl(control.value, control.received);
    // Local simulation: apply the server's ordered stream exactly in wire order.
    for (auto &in : ordered)
    {
        // Once this connection has failed (for example after a divergence
        // report is answered), nothing else it delivered may be applied.
        if (Disconnected()) break;
        try { HandleOrdered(in.binary, in.bytes, in.received); }
        catch (const ReplicaDesync &e) { Diverged(e); }
        catch (const std::exception &e) { ReplicaFailed(e); break; }
    }
    Pump();
    if (desyncWaitUntil && MonotonicSeconds() >= desyncWaitUntil)
    {
        desyncWaitUntil = 0;
        Fail("local simulation resynchronizing after divergence");
    }
    for (auto &packet : incoming)
    {
        if (!ready) throw std::runtime_error("frame arrived before welcome");
        if (waitingForKeyframe && packet.size() >= CanvasHeaderBytes && packet[4] != 0 && packet[4] != 3) continue;
        try { canvas = DecodeCanvas(packet, canvas.id ? &canvas : nullptr); }
        catch (const MissingCanvasBase &)
        {
            if (!waitingForKeyframe)
            {
                Json::Value request; request["type"] = "resync";
                SendControl(request); waitingForKeyframe = true;
            }
            continue;
        }
        waitingForKeyframe = false;
        Json::Value ack; ack["type"] = "frame_ack"; ack["frame"] = Json::UInt64(canvas.id);
        if (!SendControl(ack)) break;
        failures = 0;
        lastReceived = MonotonicSeconds();
        metrics.Frame(); pictureChanged = true;
    }
    if (attempting && MonotonicSeconds()-lastReceived > 15) Fail("no world updates for 15 seconds");
    if (ready && metrics.ProbeDue(MonotonicSeconds())) SendControl(metrics.BeginProbe(MonotonicSeconds()));
    if (MonotonicSeconds()-presenceAt > 2) players = Json::Value(Json::arrayValue);
}
std::vector<double> Session::TakeEditLatencies()
{
    std::vector<double> taken;
    taken.swap(latencies);
    return taken;
}
void Session::AddLocalStatus(Json::Value &out) const
{
    if (!options.localSim) return;
    out["localSimulation"] = true;
    out["resyncs"] = Json::UInt64(resyncs);
    out["skipAheads"] = Json::UInt64(skipAheads);
    out["seals"] = Json::UInt64(sealsBefore+(replica ? replica->Seals() : 0));
    out["verifiedHashes"] = Json::UInt64(checksBefore+(replica ? replica->Checks() : 0));
    out["appliedCommands"] = Json::UInt64(editsBefore+(replica ? replica->Edits() : 0));
    out.removeMember("frameAgeMs"); // no server capture: the picture is rendered here
    auto status = out["status"].asString();
    if (desyncWaitUntil) out["status"] = "RESYNCHRONIZING";
    else if (replica && replica->Ready())
    {
        out["localTick"] = Json::UInt64(replica->world.tick);
        if (status != "RECONNECTING" && status != "CONNECTING")
        {
            bool slow = !out["simHz"].isNull() && !replica->world.paused &&
                out["simHz"].asDouble() < out["targetHz"].asDouble()*.85;
            bool tooSlow = behindSeconds > 3;
            out["status"] = replica->world.paused ? "PAUSED" : tooSlow ? "TOO SLOW" : (slow ? "SERVER SLOW" : "LOCAL SIM");
            out["behindSeconds"] = behindSeconds;
            out["paused"] = replica->world.paused;
        }
    }
    else if (ready && status != "RECONNECTING") out["status"] = "SYNCHRONIZING";
}
Json::Value Session::Sample()
{
    auto sample = metrics.Sample(MonotonicSeconds(), canvas, ready, waitingForKeyframe);
    sample["players"] = players;
    sample["roomLocked"] = roomLocked;
    sample["connections"] = connections; sample["uncertainEdits"] = Json::UInt64(edits.Uncertain());
    sample["pendingEdits"] = unsigned(edits.Pending());
    sample["connected"] = ready && canvas.id && !waitingForKeyframe && canvas.frame.view == desiredView;
    if (!attempting || (!ready && (connections || failures)))
    {
        sample["status"] = "RECONNECTING";
        sample["retrySeconds"] = attempting ? 0.0 : std::max(0.0, retryAt-MonotonicSeconds());
    }
    // Local simulation is synchronized by its replica (the game shows the world
    // itself and publishes no picture); video needs a current frame.
    else if (ready && (options.localSim ? !Synchronized() : !canvas.id || canvas.frame.view != desiredView))
        sample["status"] = "SYNCHRONIZING";
    AddLocalStatus(sample);
    if (options.localSim)
    {
        // Own-edit latency: send to applied in the local world (shown on the next render).
        sample["editLatencySamples"] = Json::UInt64(latencySamples);
        sample["editLatencyMeanMs"] = latencySamples ? latencyTotalMs/latencySamples : 0.0;
        sample["editLatencyMaxMs"] = latencyMaxMs;
        sample["paceLeadTicks"] = Json::UInt64(Lead());
    }
    return sample;
}
}
