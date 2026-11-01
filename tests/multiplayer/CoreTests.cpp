#include "multiplayer/World.h"
#include "multiplayer/Canvas.h"
#include "multiplayer/Metrics.h"
#include "multiplayer/EditSession.h"
#include "multiplayer/Traffic.h"
#include "multiplayer/Presence.h"
#include "multiplayer/Tools.h"
#include "multiplayer/TickTimeline.h"
#include "multiplayer/CommandRecord.h"
#include "multiplayer/GameSession.h"
#include "multiplayer/StateDigest.h"
#include "simulation/SimulationData.h"
#include "simulation/ElementClasses.h"
#include "prefs/GlobalPrefs.h"
#include <functional>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace multiplayer;
void Check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void Reject(std::function<void()> action)
{
    try { action(); } catch (const std::exception &) { return; }
    throw std::runtime_error("invalid input was accepted");
}
int main()
{
    try
    {
        GlobalPrefs prefs;
        Json::Value presenceMessage, person;
        person["player"] = 1U; person["name"] = "A\nb"; person["admin"] = false; person["visible"] = false;
        presenceMessage["players"].append(person);
        Check(PresencePlayers(presenceMessage)[0]["name"] == "A?b", "presence names cannot inject rendering controls");
        presenceMessage["players"].append(person);
        Reject([&] { PresencePlayers(presenceMessage); });
        presenceMessage["players"].resize(1); presenceMessage["players"][0]["visible"] = true;
        Reject([&] { PresencePlayers(presenceMessage); });
        UploadBudget upload(100000, 0);
        upload.Join(1, 100000, 0); upload.Join(2, 30000, 0);
        Check(upload.Share(1) == 50000 && upload.Share(2) == 30000, "equal shares respect player ceilings");
        Check(upload.Acquire(1, MaxCanvasPacket, 0) == 0, "room permits one initial keyframe burst");
        Check(upload.Acquire(2, 1000, 0) > 0, "burst is shared across players");
        upload.Leave(1, 0); upload.Join(3, 100000, 0);
        Check(upload.Acquire(3, 1000, 0) > 0, "rejoining cannot refill the room burst");
        upload.Cancel(2); upload.Cancel(3);
        // Keep a large request alive while a small sender tries to consume credit.
        for (int i = 1; i < 20; ++i)
        {
            Check(upload.Acquire(3, 100000, i*.05) > 0, "large frame waits for credit");
            Check(upload.Acquire(2, 100, i*.05) > 0, "small frames cannot starve a keyframe");
        }
        Check(upload.Acquire(3, 100000, 1) == 0, "reserved keyframe eventually sends");
        upload.Cancel(2); upload.Leave(2, 1);
        Check(upload.Share(3) == 100000, "departing player restores share");
        Reject([&] { upload.Acquire(3, MaxCanvasPacket+1, 1); });
        UploadBudget expiry(100000, 0);
        expiry.Join(1, 100000, 0); expiry.Join(2, 100000, 0);
        Check(expiry.Acquire(1, MaxCanvasPacket, 0) == 0, "consume shared burst for reservation test");
        Check(expiry.Acquire(2, MaxCanvasPacket, 0) > 0, "large waiting reservation");
        Check(expiry.Acquire(1, 100, .1) > 0, "active reservation blocks overtaking");
        Check(expiry.Acquire(1, 100, .3) == 0, "abandoned reservation expires without blocking room");
        ByteBudget changing(1000, 0);
        changing.Spend(MaxCanvasPacket, 0);
        changing.SetRate(500, 1);
        Check(changing.Delay(1500, 1) == 1, "rate change preserves credit earned at old rate");
        StreamPacer pacing(20);
        Check(pacing.Window() == 1 && !pacing.Ready(1, 1, 0), "first frame requires an ACK");
        pacing.Acknowledge(.2);
        Check(pacing.Window() == 2 && pacing.Limited() && pacing.Hz() < 7, "slow ACKs reduce delivery rate");
        Check(!pacing.Ready(1, 1, 0), "overdue ACK prevents more backlog");
        pacing.Acknowledge(1);
        Check(pacing.Hz() < 2, "pacing responds immediately to congestion");
        for (int i = 0; i < 80; ++i) pacing.Acknowledge(.01);
        Check(pacing.Hz() > 19.9 && pacing.Hz() <= 20, "delivery rate recovers gradually");
        Reject([&] { pacing.Acknowledge(-1); });
        SimulationData data;
        // Lockstep research gate A: identical seeds + ordered semantic edits
        // must produce identical diagnostic fields in two engine instances.
        {
            World a, b;
            a.paused = b.paused = true;
            a.sim->ensureDeterminism = b.sim->ensureDeterminism = true;
            a.sim->rng.seed(4321); b.sim->rng.seed(4321);
            auto input = ParseJson(R"({"seq":1,"op":"brush","x1":100,"y1":100,"x2":110,"y2":100,"radius":2,"element":2})");
            auto edit = ParseCommand(input, false);
            auto encoded = RecordCommand(edit);
            Check(ParseJson(JsonText(encoded)) == input, "canonical brush command must be lossless");
            a.Apply(edit); b.Apply(ReadRecordedCommand(encoded));
            for (int i = 0; i < 10; ++i) { a.Advance(); b.Advance(); }
            Check(DiagnosticDigests(a) == DiagnosticDigests(b), "same process and seed must reproduce short physics replay");
            b.sim->rng.seed(4322);
            Check(DiagnosticDigests(a)["clock-rng"] != DiagnosticDigests(b)["clock-rng"],
                "RNG desync must be detected in its named state group");
            TickTimeline timeline;
            auto stamp = timeline.Commit(0, 0, 0, 1, 0);
            Check(stamp.eventId == 1 && stamp.ordinal == 0, "first committed edit gets event ID and ordinal");
        }
        {
            World tools;
            tools.paused = true;
            auto command = ParseJson(R"({"seq":1,"op":"tool","tool":"DEFAULT_PT_METL","action":"box","x1":100,"y1":100,"x2":104,"y2":104,"radius":0})");
            tools.Apply(ParseCommand(command, false));
            Check(tools.sim->parts.active == 25 && tools.tick == 0, "rectangle uses upstream element editing without advancing physics");
            auto index = ID(tools.sim->pmap[100][100]);
            auto temperature = tools.sim->parts[index].temp;
            command["tool"] = "DEFAULT_TOOL_HEAT"; command["action"] = "point";
            tools.Apply(ParseCommand(command, false));
            Check(tools.sim->parts[index].temp == temperature+2, "heat tool uses upstream behavior");
            command["tool"] = "DEFAULT_WL_FAN";
            tools.Apply(ParseCommand(command, false));
            Check(tools.sim->bmap[25][25] == WL_FAN, "wall tool creates the upstream wall");
            command["action"] = "line"; command["dragging"] = false; command["x2"] = 120; command["y2"] = 100;
            tools.Apply(ParseCommand(command, false));
            Check(std::abs(tools.sim->fvx[25][25]-.1f) < .00001f, "fan line sets fan velocity");
            Check(tools.sim->replaceModeFlags == 0 && tools.sim->replaceModeSelected == 0, "per-player replace settings do not leak");
            for (const auto &entry : ToolCatalog()) Check(bool(FindTool(entry["identifier"].asString())), "every advertised tool resolves");
            command["action"] = "unknown"; Reject([&] { ParseCommand(command, false); });
            command["action"] = "point"; command["radius"] = 201; Reject([&] { ParseCommand(command, false); });
            Reject([&] { FindTool("not-a-tool"); });
            // The game's brushes: separate radii and all three default shapes.
            {
                auto shaped = ParseJson(R"({"seq":2,"op":"tool","tool":"DEFAULT_PT_DUST","action":"point","x1":200,"y1":200,"radius":6,"radiusY":2})");
                auto parsed = ParseCommand(shaped, false);
                Check(parsed.radius == 6 && parsed.radiusY == 2, "vertical brush radius is parsed");
                Check(RecordCommand(parsed)["radiusY"].asInt() == 2, "vertical radius is recorded");
                shaped.removeMember("radiusY");
                Check(ParseCommand(shaped, false).radiusY == 6 && !RecordCommand(ParseCommand(shaped, false)).isMember("radiusY"),
                    "round brushes default radiusY and keep the old record format");
                for (auto *shape : {"ellipse", "ellipse-perfect", "rectangle", "triangle"})
                {
                    shaped["brush"] = shape; shaped["radiusY"] = 3;
                    World drawn; drawn.paused = true;
                    drawn.Apply(ParseCommand(shaped, false));
                    Check(drawn.sim->parts.active > 0, "each brush shape draws");
                }
                shaped["brush"] = "bitmap"; Reject([&] { ParseCommand(shaped, false); });
                shaped["brush"] = "ellipse"; shaped["radiusY"] = 201; Reject([&] { ParseCommand(shaped, false); });
                // A long line with a huge brush exceeds the per-command limit and must be split.
                shaped["action"] = "line"; shaped["radius"] = 200; shaped["radiusY"] = 200; shaped["x2"] = 400;
                Reject([&] { ParseCommand(shaped, false); });
                // The game client splits it: every part is accepted, and parts join end to end.
                ToolStroke stroke; stroke.tool = "DEFAULT_PT_DUST"; stroke.action = "line";
                stroke.x1 = 20; stroke.y1 = 200; stroke.x2 = 590; stroke.y2 = 260;
                stroke.radiusX = 200; stroke.radiusY = 120; stroke.strength = 1;
                auto parts = StrokeCommands(stroke);
                Check(parts.size() > 1, "a too-large line is split");
                int x = stroke.x1, y = stroke.y1;
                for (auto &part : parts)
                {
                    part["seq"] = Json::UInt64(1);
                    auto parsed = ParseCommand(part, false);
                    Check(parsed.x1 == x && parsed.y1 == y && parsed.radius == 200 && parsed.radiusY == 120, "parts continue the line");
                    x = parsed.x2; y = parsed.y2;
                }
                Check(x == stroke.x2 && y == stroke.y2, "parts end where the stroke ends");
                stroke.radiusX = stroke.radiusY = 4;
                Check(StrokeCommands(stroke).size() == 1, "an ordinary stroke stays one command");
                Check(RoomSupportsTool("DEFAULT_PT_WATR") && RoomSupportsTool("DEFAULT_WL_FAN") && RoomSupportsTool("DEFAULT_TOOL_HEAT"),
                    "element, wall and simulation tools are available in rooms");
                Check(!RoomSupportsTool("DEFAULT_UI_SIGN") && !RoomSupportsTool("DEFAULT_UI_PROPERTY"), "interface tools are not (yet)");
            }
        }
        World w;
        w.paused = true;
        auto c = ParseCommand(ParseJson(R"({"seq":1,"op":"brush","x1":100,"y1":40,"x2":120,"y2":40,"radius":2,"element":1})"), false);
        w.Apply(c);
        Check(w.tick == 0 && w.revision == 1 && w.sim->parts.active > 0, "paused edits must update without advancing physics");
        auto &p = w.sim->parts[0];
        p.tmp = 42; p.tmp2 = -7; p.tmp3 = 19; p.tmp4 = -23; p.dcolour = 0xab123456;
        w.sim->pv[10][20] = 17.25f;
        w.sim->signs.emplace_back("Shared world", 40, 50, sign::Left);
        auto encoded = EncodeFrame(*w.sim, w.tick, w.revision, w.paused);
        auto f = DecodeFrame(encoded);
        Check(f.paused && f.revision == 1 && f.tick == 0, "metadata round trip");
        Check(EncodeFrame(*f.world, f.tick, f.revision, f.paused) == encoded, "render state must round trip exactly");
        Reject([&] { DecodeFrame(encoded.substr(0, encoded.size()-1)); });
        Reject([&] { auto bad = encoded; bad[0] = 'X'; DecodeFrame(bad); });
        Reject([&] { auto bad = encoded; for (int i = 4; i < 8; ++i) bad[i] = char(0xff); DecodeFrame(bad); });
        Reject([&] { DecodeFrame(encoded + "trailing"); });
        Reject([&] { ParseCommand(ParseJson(R"({"seq":1,"op":"pause","paused":true})"), false); });
        Reject([&] { ParseCommand(ParseJson(R"({"seq":1,"op":"brush","x1":-1})"), false); });
        Reject([&] { ParseJson(R"({"seq":1,"seq":2})"); });
        Reject([&] { ParseJson(std::string(5000, ' ')); });
        Reject([&] { ParseCommand(ParseJson(R"({"seq":1,"op":"pause","paused":"false"})"), true); });

        auto oldY = w.sim->parts[0].y;
        for (int i = 0; i < 10; ++i) w.Apply(Command{Command::Step, uint64_t(i+2)});
        Check(w.tick == 10 && w.revision == 11 && w.sim->parts[0].y > oldY, "upstream dust should fall when stepped");

        // Verify that the server wrapper uses the same complete step as upstream.
        World a;
        auto b = Simulation::Factory();
        a.sim->create_part(-1, 200, 40, PT_WATR); b->create_part(-1, 200, 40, PT_WATR);
        a.sim->BeforeSim(false); b->BeforeSim(false);
        b->rng.state(a.sim->rng.state());
        for (int i = 0; i < 30; ++i)
        {
            a.Advance(); b->BeforeSim(true); b->UpdateParticles(0, NPART); b->AfterSim();
        }
        Check(EncodeFrame(*a.sim, 30, 0, false) == EncodeFrame(*b, 30, 0, false), "server must preserve upstream stepping");

        // Incompressible colors ensure small edits select deltas, including erasure to black.
        std::string colors(CanvasBytes, '\0');
        uint32_t random = 12345;
        for (auto &byte : colors) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; byte = char(random); }
        CanvasFrame image{1, 10, 2, 0, true, PackCanvas(colors)};
        auto key = EncodeCanvas(image, 1);
        auto baseline = DecodeCanvas(key);
        Check(baseline.frame.pixels->planes == colors, "lossless keyframe");
        if (SupportsWebP())
        {
            // WebP is a visually lossy, independently decodable keyframe. It must
            // never rely on the server's original pixels as a future delta base.
            std::string pattern(CanvasBytes, '\0');
            uint32_t seed = 1234567;
            for (auto &byte : pattern)
            {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                byte = char(seed);
            }
            CanvasFrame lossy{1, 1, 1, 0, false, PackCanvas(pattern, 35, 2)};
            auto webpPacket = EncodeCanvas(lossy, 10, nullptr, true);
            Check(webpPacket[4] == 3 && webpPacket.size() < EncodeCanvas(lossy, 10).size(),
                "WebP must reduce payload size on a noisy scene");
            auto reconstructed = DecodeCanvas(webpPacket);
            Check(reconstructed.frame.pixels->planes.size() == CanvasBytes && reconstructed.frame.lossy,
                "downsampled WebP must decode to the original canvas dimensions");
            Reject([&] { auto bad = webpPacket; bad.back() ^= 1; DecodeCanvas(bad); });
            Reject([&] { auto bad = webpPacket; bad[16] = 1; DecodeCanvas(bad); });
            // A WebP baseline is not exact; the encoder must not send XOR pixels.
            auto next = EncodeCanvas(lossy, 11, &reconstructed, true);
            Check(next[4] == 3 || next[4] == 0, "lossy frame cannot be a delta baseline");
            Check(DecodeCanvas(next).frame.pixels->planes == reconstructed.frame.pixels->planes,
                "WebP keyframes must not drift over time");
        }
        auto video = CanvasVideo(baseline.frame);
        Check(video[5] == (uint8_t(colors[5]) | uint32_t(uint8_t(colors[CanvasPixels+5])) << 8 |
            uint32_t(uint8_t(colors[2*CanvasPixels+5])) << 16), "BGR planes reconstruct RGB pixels");
        colors[5] = colors[CanvasPixels+5] = colors[2*CanvasPixels+5] = 0;
        image.capture = 100; image.tick = 200; image.revision = 8; image.pixels = PackCanvas(colors);
        auto delta = EncodeCanvas(image, 2, &baseline);
        Check(delta[4] == 1 && delta.size() < key.size()/10, "sparse edits must use small deltas");
        auto changed = DecodeCanvas(delta, &baseline);
        Check(changed.frame.pixels->planes == colors && CanvasVideo(changed.frame)[5] == 0 && changed.frame.tick == 200,
            "skipped captures and erased pixels must reconstruct exactly");
        bool missing = false;
        try { DecodeCanvas(delta); } catch (const MissingCanvasBase &) { missing = true; }
        Check(missing, "missing baseline must request resynchronization");
        auto repeat = EncodeCanvas(image, 3, &changed);
        Check(repeat.size() == CanvasHeaderBytes && repeat[4] == 2, "unchanged pixels need only metadata");
        Check(DecodeCanvas(repeat, &changed).frame.pixels == changed.frame.pixels, "repeat shares decoded pixels");
        Reject([&] { DecodeCanvas(delta, &changed); });
        Reject([&] { DecodeCanvas(key.substr(0, key.size()-1)); });
        Reject([&] { DecodeCanvas(key + "trailing"); });
        Reject([&] { auto corrupt = delta; corrupt[40] ^= 1; DecodeCanvas(corrupt, &baseline); });
        Reject([&] { auto corrupt = key; corrupt[5] = 3; DecodeCanvas(corrupt); });
        Reject([&] { auto corrupt = key; corrupt[16] = 1; DecodeCanvas(corrupt); });
        Reject([&] { DecodeCanvas(repeat + "trailing", &changed); });
        image.view = 1;
        Check(EncodeCanvas(image, 4, &changed)[4] == 0, "view changes require a keyframe");
        Renderer renderer;
        for (uint8_t view = 0; view < CanvasViews; ++view)
        {
            auto canvas = RenderCanvas(renderer, *w.sim, 1, w.tick, w.revision, true, view);
            auto pixels = CanvasVideo(DecodeCanvas(EncodeCanvas(canvas, 1)).frame);
            auto &original = renderer.GetVideo();
            for (int y = 0; y < YRES; ++y) for (int x = 0; x < XRES; ++x)
                Check(pixels[y*XRES+x] == (original[{x,y}] & 0xffffff), "stream must preserve upstream rendered pixels");
        }
        ByteBudget budget(1000, 0);
        budget.Spend(MaxCanvasPacket, 0);
        Check(budget.Delay(1000, .5) == .5, "bandwidth budget refills at the configured rate");
        Reject([&] { budget.Spend(1000, .5); });
        budget.Spend(1000, 1);
        Check(budget.Delay(1000, 1) == 1, "sending consumes the available byte budget");
        Reject([&] { budget.Delay(MaxCanvasPacket+1, 2); });
        // Independent clocks: the server is 100 seconds ahead, each network leg 50 ms,
        // and 30 ms of server queuing must be excluded from network RTT.
        ClientMetrics metrics(10);
        auto probe = metrics.BeginProbe(10);
        Json::Value telemetry;
        telemetry["id"] = probe["id"];
        telemetry["serverReceivedUs"] = Json::UInt64(110050000);
        telemetry["serverSentUs"] = Json::UInt64(110080000);
        telemetry["simHz"] = 60; telemetry["targetHz"] = 60; telemetry["updateHz"] = 20;
        telemetry["tickMs"] = 2; telemetry["inFlight"] = 2; telemetry["bandwidthKbps"] = 8192;
        telemetry["revision"] = Json::UInt64(8); telemetry["paused"] = false; telemetry["bandwidthLimited"] = false;
        Check(metrics.Telemetry(telemetry, 10.13), "matching telemetry probe");
        Check(!metrics.Telemetry(telemetry, 10.14), "duplicate telemetry ignored");
        changed.frame.capturedUs = 110000000;
        auto timedPacket = EncodeCanvas(changed.frame, 20);
        Check(DecodeCanvas(timedPacket).frame.capturedUs == 110000000, "capture timestamp round trip");
        changed.frame.paused = false;
        for (int i = 0; i < 20; ++i) metrics.Frame();
        metrics.Traffic(125000);
        auto sample = metrics.Sample(11, changed, true, false);
        Check(std::abs(sample["pingMs"].asDouble()-100) < .01, "RTT excludes server queue time");
        Check(std::abs(sample["frameAgeMs"].asDouble()-1000) < .01, "capture age includes pipeline delay across independent clocks");
        Check(sample["updatesHz"].asDouble() == 20 && sample["receiveMbps"].asDouble() == 1, "measured frame and byte rates");
        Check(sample["status"] == "UPDATES DELAYED", "old running canvas must be flagged");
        auto nextProbe = metrics.BeginProbe(11);
        telemetry["id"] = nextProbe["id"];
        telemetry["serverReceivedUs"] = Json::UInt64(111050000); telemetry["serverSentUs"] = Json::UInt64(111080000);
        telemetry["paused"] = true; changed.frame.paused = true;
        metrics.Telemetry(telemetry, 11.13);
        Check(metrics.Sample(12, changed, true, false)["status"] == "PAUSED", "old paused capture is not a network stall");
        Check(metrics.Sample(17, changed, true, false)["status"] == "TELEMETRY STALE", "stale measurements must not be displayed as current");
        nextProbe = metrics.BeginProbe(18); telemetry["id"] = nextProbe["id"];
        telemetry["serverReceivedUs"] = Json::UInt64(118050000); telemetry["serverSentUs"] = Json::UInt64(118080000);
        telemetry["paused"] = false; telemetry["simHz"] = 30;
        changed.frame.paused = false; changed.frame.capturedUs = 118100000;
        metrics.Telemetry(telemetry, 18.13);
        Check(metrics.Sample(18.2, changed, true, false)["status"] == "SERVER SLOW", "simulation slowdown must be distinct from network delay");
        nextProbe = metrics.BeginProbe(19); telemetry["id"] = nextProbe["id"];
        telemetry["serverReceivedUs"] = Json::UInt64(119050000); telemetry["serverSentUs"] = Json::UInt64(119080000);
        telemetry["simHz"] = 60; telemetry["bandwidthLimited"] = true;
        changed.frame.capturedUs = 119100000;
        metrics.Telemetry(telemetry, 19.13);
        Check(metrics.Sample(19.2, changed, true, false)["status"] == "BANDWIDTH LIMIT", "configured canvas throttling must be visible");
        nextProbe = metrics.BeginProbe(20); telemetry["id"] = nextProbe["id"];
        telemetry["serverReceivedUs"] = Json::UInt64(120080000); telemetry["serverSentUs"] = Json::UInt64(120050000);
        Reject([&] { metrics.Telemetry(telemetry, 20.13); });
        EditSession edits;
        Check(edits.Begin() == 1 && edits.Begin() == 2, "editing sequences increase per connection");
        edits.Acknowledge(1);
        edits.Disconnect();
        Check(edits.Pending() == 0 && edits.Uncertain() == 1, "unconfirmed edits are counted and discarded on disconnect");
        Check(edits.Begin() == 1, "new connection starts a fresh sequence space");
        edits.Acknowledge(1); edits.Disconnect();
        Check(edits.Uncertain() == 1 && edits.Pending() == 0, "acknowledged edits are never replayed or counted uncertain");
        std::cout << "Core tests passed: physics, paused editing, state and canvas codecs, resync, views, bandwidth budget, malformed input, permissions.\n";
        return 0;
    }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
