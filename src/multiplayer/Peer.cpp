#include "Peer.h"
#include "CheckpointWire.h"
#include <iostream>

namespace multiplayer
{
void Peer::Start()
{
    sender = std::thread([this] {
        try
        {
        ReceivedCanvas base;
        double lastSent = 0;
        uint64_t knownGeneration = 0;
        std::shared_ptr<const CanvasFrame> cachedFrame;
        std::string cachedPacket;
        uint64_t cachedGeneration = 0;
        while (!closed)
        {
            std::string text;
            Json::Value control;
            std::shared_ptr<const CanvasFrame> frame;
            std::string replicaChunk;
            uint64_t encodingGeneration = 0;
            {
                std::unique_lock lock(mutex);
                auto available = [&] {
                    return !replica && latest && latest->view == view && pacer.Ready(MonotonicSeconds(), inFlight.size(), inFlight.empty() ? 0 : inFlight.front().second) &&
                        (generation != knownGeneration || !base.id || latest->capture > base.frame.capture || MonotonicSeconds()-lastSent >= 1);
                };
                double wait = .05;
                if (!pendingSeal.isNull()) wait = std::clamp(pendingSealSince+sealBatch-MonotonicSeconds(), 0.0, wait);
                changed.wait_for(lock, std::chrono::duration<double>(wait), [&] { return closed || !controls.empty() || !presence.isNull() || checkpointEnding || (checkpoint && checkpointStarted) || available(); });
                if (closed) break;
                if (!pendingSeal.isNull() && MonotonicSeconds()-pendingSealSince >= sealBatch) FlushSealLocked();
                if (!inFlight.empty() && MonotonicSeconds()-inFlight.front().second > 10) { closed = true; break; }
                if (generation != knownGeneration)
                {
                    base = {}; inFlight.clear(); lastAck = lastIssued;
                    knownGeneration = generation;
                }
                if (checkpointEnding)
                {
                    control["type"] = "checkpoint_end";
                    control["id"] = Json::UInt64(checkpointId);
                    checkpointEnding = false;
                }
                else if (checkpoint && checkpointStarted)
                {
                    replicaChunk = EncodeCheckpointChunk(checkpointId, *checkpoint, checkpointOffset);
                }
                else if (!controls.empty())
                {
                    control = std::move(controls.front()); controls.pop_front();
                    if (control["type"] == "checkpoint_begin") checkpointStarted = true;
                }
                else if (!presence.isNull()) { control = std::move(presence); presence = Json::Value(); }
                else if (available()) { frame = latest; encodingGeneration = generation; }
            }
            if (!control.isNull())
            {
                if (control["type"] == "telemetry") control["serverSentUs"] = Json::UInt64(MonotonicMicros());
                text = JsonText(control);
            }
            if (text.empty() && !frame && replicaChunk.empty()) continue;
            auto ws = socket.lock();
            if (!ws) { closed = true; break; }
            std::string packet;
            uint64_t id = 0;
            if (frame)
            {
                { std::lock_guard lock(mutex); id = lastIssued + 1; }
                if (cachedFrame != frame || cachedGeneration != encodingGeneration)
                {
                    cachedPacket = EncodeCanvas(*frame, id, base.id ? &base : nullptr, useWebP);
                    cachedFrame = frame; cachedGeneration = encodingGeneration;
                }
                packet = cachedPacket;
                // Include a conservative allowance for WebSocket/TLS framing in the budget.
                double delay;
                {
                    std::lock_guard lock(mutex);
                    auto now = MonotonicSeconds();
                    if (generation != encodingGeneration || frame->view != view ||
                        !pacer.Ready(now, inFlight.size(), inFlight.empty() ? 0 : inFlight.front().second)) continue;
                    delay = upload.Acquire(this->id, packet.size()+512, now);
                    if (!delay) { lastIssued = id; inFlight.emplace_back(id, now); pacer.Sent(now); }
                }
                if (delay > 0)
                {
                    lastBandwidthWait = MonotonicSeconds();
                    std::unique_lock lock(mutex);
                    changed.wait_for(lock, std::chrono::duration<double>(std::min(delay, .05)));
                    continue; // Pick the newest capture when the budget becomes available.
                }
            }
            if (!replicaChunk.empty())
            {
                // Checkpoint bytes share the room's upload token bucket with
                // canvas frames, avoiding unrestricted large sync bursts.
                double delay = upload.Acquire(this->id, replicaChunk.size()+128, MonotonicSeconds());
                if (delay > 0)
                {
                    std::unique_lock lock(mutex);
                    changed.wait_for(lock, std::chrono::duration<double>(std::min(delay,.05)));
                    continue;
                }
            }
            auto info = !replicaChunk.empty() ? ws->sendBinary(replicaChunk) :
                frame ? ws->sendBinary(packet) : ws->sendText(text);
            if (!info.success) { closed = true; break; }
            bytesSent += info.wireSize;
            if (!replicaChunk.empty())
            {
                std::lock_guard lock(mutex);
                checkpointOffset += replicaChunk.size()-CheckpointWireHeaderBytes;
                if (checkpointOffset == checkpoint->size())
                {
                    checkpoint.reset(); checkpointStarted = false;
                    checkpointEnding = true;
                }
            }
            if (frame)
            {
                base = {id, *frame};
                // After a lossy WebP frame the receiver pixels are different from
                // the server original. Never XOR against that unshared baseline.
                if (packet[4] == 3) base.frame.lossy = true;
                lastSent = MonotonicSeconds(); ++framesSent;
                cachedFrame.reset(); cachedPacket.clear();
                if (packet[4] == 0) ++fullFrames;
                else if (packet[4] == 1) ++deltaFrames;
                else if (packet[4] == 2) ++repeats;
                else ++webpFrames;
            }
        }
        }
        catch (const std::exception &e) { std::cerr << "Player stream: " << e.what() << '\n'; closed = true; }
        { std::lock_guard lock(mutex); upload.Leave(id, MonotonicSeconds()); }
        if (auto ws = socket.lock()) ws->close();
        senderDone = true;
    });
}
}
