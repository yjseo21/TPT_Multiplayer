#include "Room.h"
#include "Autosave.h"
#include "Canvas.h"
#include "ExactCheckpoint.h"
#include "Journal.h"
#include "common/platform/Platform.h"
#include "simulation/SimulationData.h"
#include "prefs/GlobalPrefs.h"
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace multiplayer;
using Clock = std::chrono::steady_clock;
namespace
{
volatile std::sig_atomic_t stopping = 0;
void Signal(int) { stopping = 1; }
std::string Env(const char *name) { auto p = std::getenv(name); return p ? p : ""; }
struct Options
{
    std::string bind = "127.0.0.1", cert, key, load, save, autosaveDirectory, journalPath, exactLoad, exactSave;
    int port = 34403, ticks = 60, updates = 20, runFor = 0, bandwidthKbps = 8192;
    bool paused = false;
    int uploadKbps = 8192;
    int replaySeed = -1;
    int autosaveInterval = 60, autosaveKeep = 5;
    bool recover = false;
    int webpQuality = 0, webpScale = 1;
    int sealBatchMs = 0;
};
int Number(const std::string &s, int low, int high)
{
    size_t end; int n = std::stoi(s, &end);
    if (end != s.size() || n < low || n > high) throw std::runtime_error("invalid numeric option");
    return n;
}
Options Arguments(int argc, char **argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--paused") { o.paused = true; continue; }
        if (a == "--recover") { o.recover = true; continue; }
        if (a == "--help")
        {
            std::cout << "tpt-server [--bind ADDRESS] [--port 34403] [--cert PEM --key PEM]\n"
                "  [--load world.cps] [--save world.cps] [--paused]\n"
                "  [--exact-load world.tptx] [--exact-save world.tptx] (experimental, no gravity/Lua)\n"
                "  [--tick-rate 60] [--update-rate 20] [--run-for SECONDS]\n"
                "  [--bandwidth-kbps 8192] (per player; lossless frames, adaptive update rate)\n"
                "  [--upload-kbps 8192] (shared room canvas budget)\n"
                "  [--webp-quality 0|1..100] (0=lossless, opt-in bandwidth-saving WebP)\n"
                "  [--webp-scale 1|2|3] (1=full-resolution, 2/3=bandwidth-saving downscale)\n"
                "  [--seal-batch-ms 0] (0..1000; batch local-sim tick seals: less bandwidth, more latency)\n"
                "  [--autosave-dir DIRECTORY --autosave-interval 60 --autosave-keep 5 --recover]\n"
                "  [--seed UNSIGNED_31_BIT --journal FILE.jsonl] (experimental deterministic replay audit)\n"
                "Credentials: TPT_ROOM_TOKEN and TPT_ADMIN_TOKEN environment variables.\n";
            std::exit(0);
        }
        if (++i >= argc) throw std::runtime_error("missing option value");
        std::string v = argv[i];
        if (a == "--bind") o.bind = v;
        else if (a == "--port") o.port = Number(v, 1, 65535);
        else if (a == "--cert") o.cert = v;
        else if (a == "--key") o.key = v;
        else if (a == "--load") o.load = v;
        else if (a == "--exact-load") o.exactLoad = v;
        else if (a == "--exact-save") o.exactSave = v;
        else if (a == "--journal") o.journalPath = v;
        else if (a == "--seed") o.replaySeed = Number(v, 0, INT32_MAX);
        else if (a == "--save") o.save = v;
        else if (a == "--autosave-dir") o.autosaveDirectory = v;
        else if (a == "--autosave-interval") o.autosaveInterval = Number(v, 1, 86400);
        else if (a == "--autosave-keep") o.autosaveKeep = Number(v, 1, 100);
        else if (a == "--tick-rate") o.ticks = Number(v, 1, 60);
        else if (a == "--update-rate") o.updates = Number(v, 1, 30);
        else if (a == "--run-for") o.runFor = Number(v, 1, 86400);
        else if (a == "--bandwidth-kbps") o.bandwidthKbps = Number(v, 64, 1000000);
        else if (a == "--upload-kbps") o.uploadKbps = Number(v, 64, 1000000);
        else if (a == "--webp-quality") o.webpQuality = Number(v, 0, 100);
        else if (a == "--webp-scale") o.webpScale = Number(v, 1, 3);
        else if (a == "--seal-batch-ms") o.sealBatchMs = Number(v, 0, 1000);
        else throw std::runtime_error("unknown option: " + a);
    }
    if (!o.journalPath.empty() && (o.replaySeed < 0 || !o.load.empty() || o.recover))
        throw std::runtime_error("--journal requires --seed and an empty, non-recovered initial world");
    if (!o.exactLoad.empty() && (!o.load.empty() || o.recover || o.replaySeed >= 0 || !o.journalPath.empty()))
        throw std::runtime_error("--exact-load cannot combine with CPS load, recover, seed or journal");
    if (o.webpScale > 1 && !o.webpQuality) throw std::runtime_error("WebP scale requires nonzero quality");
    if (o.webpQuality && !SupportsWebP()) throw std::runtime_error("WebP was not built into this server");
    if (o.cert.empty() != o.key.empty()) throw std::runtime_error("provide both --cert and --key");
    if (o.recover && (o.autosaveDirectory.empty() || !o.load.empty()))
        throw std::runtime_error("--recover requires --autosave-dir and cannot be combined with --load");
    if (o.bind != "127.0.0.1" && o.bind != "::1" && (o.cert.empty() || Env("TPT_ROOM_TOKEN").empty()))
        throw std::runtime_error("non-loopback binding requires TLS certificate/key and TPT_ROOM_TOKEN");
    return o;
}
}

int main(int argc, char **argv)
{
    try
    {
        auto options = Arguments(argc, argv);
        ix::initNetSystem();
        std::signal(SIGINT, Signal); std::signal(SIGTERM, Signal);
#ifdef _WIN32
        std::signal(SIGBREAK, Signal);
#endif
#ifndef _WIN32
        // Mbed TLS may write while a peer disconnects. Handle the socket error instead
        // of allowing SIGPIPE to terminate the whole room.
        std::signal(SIGPIPE, SIG_IGN);
#endif
        GlobalPrefs prefs;
        SimulationData data;
        World world;
        std::unique_ptr<AutosaveStore> autosaves;
        if (!options.autosaveDirectory.empty())
        {
            autosaves = std::make_unique<AutosaveStore>(options.autosaveDirectory, options.autosaveKeep);
            if (options.recover) autosaves->Recover(world);
        }
        if (!options.load.empty()) world.Load(options.load);
        if (!options.exactLoad.empty())
        {
            std::vector<char> packet;
            if (!Platform::ReadFile(packet,options.exactLoad))
                throw std::runtime_error("cannot open exact checkpoint file");
            RestoreExactCheckpoint(world,std::string_view(packet.data(),packet.size()));
            std::cout << "Exact checkpoint restored at tick " << world.tick << std::endl;
        }
        if (options.paused) world.paused = true;
        if (options.replaySeed >= 0)
        {
            world.sim->rng.seed(unsigned(options.replaySeed));
            world.sim->ensureDeterminism = true;
        }
        std::unique_ptr<Journal> journal;
        if (!options.journalPath.empty())
            journal = std::make_unique<Journal>(options.journalPath, unsigned(options.replaySeed), options.ticks, world);

        RoomConfig config;
        config.roomToken = Env("TPT_ROOM_TOKEN"); config.adminToken = Env("TPT_ADMIN_TOKEN");
        config.ticks = options.ticks; config.updates = options.updates;
        config.bandwidthKbps = options.bandwidthKbps; config.uploadKbps = options.uploadKbps;
        config.webpQuality = options.webpQuality; config.webpScale = options.webpScale;
        config.sealBatchMs = options.sealBatchMs; config.autosaveInterval = options.autosaveInterval;
        // The room outlives the server and its callbacks, including during exception unwinding.
        Room room(config, world, autosaves.get(), journal.get());
        ix::WebSocketServer server(options.port, options.bind, 8, 8, 5,
            ix::SocketServer::kDefaultAddressFamily, 20, 1);
        server.disablePerMessageDeflate();
        if (!options.cert.empty())
        {
            ix::SocketTLSOptions tls;
            tls.tls = true; tls.certFile = options.cert; tls.keyFile = options.key;
            tls.caFile = "NONE"; // Server authenticates players using tokens, not client certificates.
            server.setTLSOptions(tls);
        }
        server.setOnConnectionCallback([&room](std::weak_ptr<ix::WebSocket> weak, std::shared_ptr<ix::ConnectionState>) {
            room.Accept(std::move(weak));
        });
        auto listening = server.listen();
        if (!listening.first) throw std::runtime_error(listening.second);
        server.start();
        std::cout << "Listening on " << (options.cert.empty() ? "ws://" : "wss://") << options.bind << ':' << options.port
            << " | " << BuildId << " | 4 players maximum" << std::endl;
        auto began = Clock::now(), nextTick = began;
        auto tickPeriod = std::chrono::nanoseconds(1000000000 / options.ticks);
        while (!stopping && !room.Failed() && (!options.runFor || Clock::now() - began < std::chrono::seconds(options.runFor)))
        {
            room.Step();
            nextTick += tickPeriod;
            if (nextTick < Clock::now()) nextTick = Clock::now();
            // Edits arriving before the next tick apply at once, not up to a tick later.
            while (!stopping && !room.Failed() && room.WaitForRequests(nextTick)) room.ApplyEarly();
        }
        server.stop();
        if (journal) journal->Check(world, true);
        room.Close();
        if (autosaves)
        {
            autosaves->Submit(world.CaptureSave(), world.revision); autosaves->Wait();
            std::cout << "Final autosave committed generation=" << autosaves->GetStatus().committed << std::endl;
        }
        if (!options.save.empty()) world.Save(options.save);
        if (!options.exactSave.empty())
        {
            auto packet=EncodeExactCheckpoint(world);
            AtomicSaveFile(options.exactSave,std::vector<char>(packet.begin(),packet.end()));
            std::cout << "Exact checkpoint saved bytes=" << packet.size() << std::endl;
        }
        std::cout << "Stopped at tick " << world.tick << ", revision " << world.revision << std::endl;
        ix::uninitNetSystem();
        return 0;
    }
    catch (const std::exception &e) { std::cerr << "Server: " << e.what() << '\n'; return 1; }
}
