// Standalone experimental low-bandwidth TPT client. It receives exactly one
// checkpoint and a server-ordered stream of commands/tick seals. Local TPT
// computes physics and a local Renderer creates the final PNG, with NO canvas
// or particle updates sent per physics tick.
#include "ReplicaState.h"
#include "Canvas.h"
#include "Metrics.h"
#include "graphics/VideoBuffer.h"
#include "prefs/GlobalPrefs.h"
#include "simulation/SimulationData.h"
#include "common/platform/Platform.h"
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXUrlParser.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using namespace multiplayer;
namespace
{
std::string Env(const char *name) { auto p=std::getenv(name); return p ? p : ""; }
struct Options
{
    std::string url="ws://127.0.0.1:34403",name="Replica",ca,output;
    int seconds=10;
    uint64_t minSeals=0;
};
Options Args(int argc,char **argv)
{
    Options opt;
    for (int i=1;i<argc;++i)
    {
        std::string key=argv[i];
        if (key=="--help")
        {
            std::cout << "tpt-replica [--url ws[s]://host:port] [--name NAME] [--ca CA.pem] "
                "[--run-for SECONDS] [--min-seals N] [--output final.png]\n";
            std::exit(0);
        }
        if (++i>=argc) throw std::runtime_error("missing replica option value");
        std::string v=argv[i];
        if (key=="--url") opt.url=v;
        else if (key=="--name") opt.name=v;
        else if (key=="--ca") opt.ca=v;
        else if (key=="--output") opt.output=v;
        else if (key=="--run-for") opt.seconds=std::stoi(v);
        else if (key=="--min-seals") opt.minSeals=std::stoull(v);
        else throw std::runtime_error("unknown replica option: "+key);
    }
    if (opt.seconds<1 || opt.seconds>86400 || opt.name.empty() || opt.name.size()>32)
        throw std::runtime_error("invalid replica duration or name");
    std::string scheme,host,path,query; int port=0;
    if (!ix::UrlParser::parse(opt.url,scheme,host,path,query,port) ||
        (scheme!="ws" && scheme!="wss")) throw std::runtime_error("invalid WebSocket URL");
    if (scheme=="ws" && host!="127.0.0.1" && host!="localhost" && host!="::1" && host!="[::1]")
        throw std::runtime_error("nonloopback replica connections require wss://");
    return opt;
}
}
int main(int argc,char **argv)
{
    try
    {
        auto opt=Args(argc,argv);
        GlobalPrefs prefs;
        SimulationData data;
        ReplicaState replica;
        ix::initNetSystem();
        struct Mail { bool binary=false; std::string value; };
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<Mail> messages;
        bool closed=false;
        std::atomic<uint64_t> wirePayloadBytes{0};
        std::atomic<uint64_t> wireCheckpointBytes{0};
        std::atomic<uint64_t> wireControlBytes{0};
        std::string failure;
        ix::WebSocket socket;
        socket.setUrl(opt.url);
        socket.disablePerMessageDeflate(); socket.disableAutomaticReconnection();
        socket.setHandshakeTimeout(5);
        ix::SocketTLSOptions tls;
        if (!opt.ca.empty()) tls.caFile=opt.ca;
#ifndef _WIN32
        else
        {
            auto env=Env("SSL_CERT_FILE");
            if (!env.empty()) tls.caFile=env;
            else for (auto path : {"/etc/ssl/cert.pem","/etc/ssl/certs/ca-certificates.crt","/etc/pki/tls/certs/ca-bundle.crt"})
                if (std::filesystem::is_regular_file(path)) { tls.caFile=path; break; }
        }
#endif
        socket.setTLSOptions(tls);
        socket.setOnMessageCallback([&](const ix::WebSocketMessagePtr &msg)
        {
            if (msg->type==ix::WebSocketMessageType::Open)
            {
                Json::Value hello;
                hello["type"]="hello"; hello["protocol"]=ProtocolVersion; hello["build"]=BuildId;
                hello["name"]=opt.name; hello["token"]=Env("TPT_ROOM_TOKEN");
                hello["localSimulation"]=true; hello["presence"]=false; hello["webp"]=false;
                socket.sendText(JsonText(hello));
            }
            else if (msg->type==ix::WebSocketMessageType::Message)
            {
                wirePayloadBytes += msg->str.size();
                if (msg->binary) wireCheckpointBytes += msg->str.size();
                else wireControlBytes += msg->str.size();
                std::lock_guard lock(mutex);
                if (messages.size()>=256 || msg->str.size()>CheckpointWireHeaderBytes+CheckpointChunkBytes+1024)
                { closed=true; failure="replica receive queue overflow"; }
                else messages.push_back({msg->binary,msg->str});
                changed.notify_one();
            }
            else if (msg->type==ix::WebSocketMessageType::Error || msg->type==ix::WebSocketMessageType::Close)
            {
                std::lock_guard lock(mutex); closed=true;
                failure=msg->type==ix::WebSocketMessageType::Error ? msg->errorInfo.reason : msg->closeInfo.reason;
                changed.notify_one();
            }
        });
        socket.start();
        auto end=std::chrono::steady_clock::now()+std::chrono::seconds(opt.seconds);
        while (std::chrono::steady_clock::now()<end && (!opt.minSeals || replica.Seals()<opt.minSeals))
        {
            std::deque<Mail> incoming;
            {
                std::unique_lock lock(mutex);
                changed.wait_for(lock,std::chrono::milliseconds(100),[&]{return closed || !messages.empty();});
                incoming.swap(messages);
                if (closed && incoming.empty()) throw std::runtime_error("replica disconnected: "+failure);
            }
            for (auto &msg : incoming)
            {
                if (msg.binary) replica.OnBinary(msg.value);
                else replica.OnJson(ParseJson(msg.value));
            }
        }
        if (!replica.Ready()) throw std::runtime_error("checkpoint bootstrap did not complete");
        if (opt.minSeals && replica.Seals()<opt.minSeals)
            throw std::runtime_error("requested minimum physics seals not received");
        socket.stop();
        if (!opt.output.empty())
        {
            Renderer renderer;
            auto canvas=RenderCanvas(renderer,*replica.world.sim,1,replica.world.tick,
                replica.world.revision,replica.world.paused,0);
            auto video=CanvasVideo(canvas);
            auto png=VideoBuffer(video.data(),RES,XRES).ToPNG();
            if (!Platform::WriteFile(*png,opt.output)) throw std::runtime_error("cannot write locally rendered image");
        }
        std::cout << "replica OK tick=" << replica.world.tick << " revision=" << replica.world.revision
            << " seals=" << replica.Seals() << " edits=" << replica.Edits()
            << " matching_hashes=" << replica.Checks()
            << " wire_payload_bytes=" << wirePayloadBytes.load()
            << " checkpoint_payload_bytes=" << wireCheckpointBytes.load()
            << " control_payload_bytes=" << wireControlBytes.load() << '\n';
        ix::uninitNetSystem();
        return 0;
    }
    catch (const std::exception &e) { std::cerr << "Replica: " << e.what() << '\n'; return 1; }
}
