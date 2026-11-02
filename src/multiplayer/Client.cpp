#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "Protocol.h"
#include "Canvas.h"
#include "Metrics.h"
#include "Session.h"
#include "Tools.h"
#include "prefs/GlobalPrefs.h"
#include "simulation/SimulationData.h"
#include "simulation/ElementClasses.h"
#include "graphics/Renderer.h"
#include "graphics/VideoBuffer.h"
#include "common/platform/Platform.h"
#include <ixwebsocket/IXUrlParser.h>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <cctype>
#include <iostream>
#include <optional>
#include <thread>

using namespace multiplayer;
namespace
{
std::string Env(const char *name) { auto p = std::getenv(name); return p ? p : ""; }
struct Options
{
    std::string url = "ws://127.0.0.1:34403", name = "Player", ca, output, metrics, outputHud;
    int frames = 0, runFor = 0, view = 0;
    bool headless = false, reconnect = true;
    // Local simulation: receive one exact checkpoint, then only ordered commands
    // and tick seals; physics and rendering run on this computer.
    bool localSim = false;
    uint64_t debugCorruptTick = 0; // test-only: perturb local state once to exercise resync
    double debugStepDelayMs = 0;   // test-only: a slower computer
    bool listTools = false;
    std::string tool, commands;
};
Options Arguments(int argc, char **argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--headless") { o.headless = true; continue; }
        if (a == "--no-reconnect") { o.reconnect = false; continue; }
        if (a == "--list-tools") { o.listTools = true; continue; }
        if (a == "--local-sim") { o.localSim = true; continue; }
        if (a == "--help")
        {
            std::cout << "tpt-remote [--url ws[s]://host:port] [--name NAME] [--ca CA.pem]\n"
                "  [--headless --frames N --output image.png]\n"
                "  [--run-for SECONDS --metrics measurements.jsonl --output-hud display.png]\n"
                "  [--view 0|1|2] [--no-reconnect] (otherwise retry transient failures)\n"
                "  [--tool IDENTIFIER] [--list-tools] [--commands FILE.jsonl]\n"
                "  [--local-sim] run physics locally from an exact checkpoint + ordered edits (no video stream)\n"
                "1-9: DUST/WATR/STNE/FIRE/METL/SPRK/OIL/WOOD/erase; wheel: radius\n"
                "Left drag: draw; right drag: erase; Space: pause; F: step; Ctrl+Backspace: clear\n"
                "F1/F2/F3: normal/heat/pressure view; F4: connection status. Global controls require TPT_ADMIN_TOKEN.\n"
                "E: search tools; B: brush shape; Shift drag: line; Ctrl drag: rectangle; Ctrl+Shift: fill\n";
            std::exit(0);
        }
        if (++i >= argc) throw std::runtime_error("missing option value");
        std::string v = argv[i];
        if (a == "--url") o.url = v;
        else if (a == "--name") o.name = v;
        else if (a == "--tool") o.tool = v;
        else if (a == "--commands") o.commands = v;
        else if (a == "--ca") o.ca = v;
        else if (a == "--output") o.output = v;
        else if (a == "--metrics") o.metrics = v;
        else if (a == "--output-hud") o.outputHud = v;
        else if (a == "--debug-corrupt-at-tick")
        {
            size_t end; o.debugCorruptTick = std::stoull(v, &end);
            if (end != v.size() || !o.debugCorruptTick) throw std::runtime_error("invalid debug tick");
        }
        else if (a == "--debug-step-delay-ms")
        {
            size_t end; o.debugStepDelayMs = std::stod(v, &end);
            if (end != v.size() || !(o.debugStepDelayMs > 0 && o.debugStepDelayMs <= 1000)) throw std::runtime_error("invalid debug delay");
        }
        else if (a == "--view")
        {
            if (v != "0" && v != "1" && v != "2") throw std::runtime_error("invalid view");
            o.view = v[0]-'0';
        }
        else if (a == "--run-for")
        {
            size_t end; o.runFor = std::stoi(v, &end);
            if (end != v.size() || o.runFor < 1 || o.runFor > 86400) throw std::runtime_error("invalid duration");
        }
        else if (a == "--frames")
        {
            size_t end; o.frames = std::stoi(v, &end);
            if (end != v.size() || o.frames < 1 || o.frames > 100000) throw std::runtime_error("invalid frame count");
        }
        else throw std::runtime_error("unknown option: " + a);
    }
    if (o.debugStepDelayMs && !o.localSim) throw std::runtime_error("--debug-step-delay-ms requires --local-sim");
    if (o.debugCorruptTick && !o.localSim) throw std::runtime_error("--debug-corrupt-at-tick requires --local-sim");
    if (o.headless && !o.frames && !o.runFor) throw std::runtime_error("--headless requires --frames or --run-for");
    std::string protocol, host, path, query;
    int port;
    if (!ix::UrlParser::parse(o.url, protocol, host, path, query, port) || (protocol != "ws" && protocol != "wss"))
        throw std::runtime_error("expected ws:// or wss:// URL");
    if (protocol == "ws" && host != "127.0.0.1" && host != "localhost" && host != "::1" && host != "[::1]")
        throw std::runtime_error("remote servers require wss://");
    return o;
}
constexpr int StatusHeight = 78;
RGB PlayerColor(unsigned id)
{
    constexpr RGB colors[] = {0x70D6FF_rgb, 0xFF91B8_rgb, 0xFFE080_rgb, 0x9BE596_rgb};
    return colors[(id-1)%4];
}
void DrawPresence(VideoBuffer &overlay, const Json::Value &players, unsigned ownPlayer)
{
    for (const auto &p : players) if (p["player"].asUInt() != ownPlayer && p["visible"].asBool())
    {
        int px = p["x"].asInt(), py = p["y"].asInt(), r = p["radius"].asInt();
        auto color = PlayerColor(p["player"].asUInt());
        overlay.DrawRect({{px-r, py-r}, {r*2+1, r*2+1}}, color);
        auto label = "#"+std::to_string(p["player"].asUInt())+" "+p["name"].asString()+(p["drawing"].asBool() ? " *" : "");
        overlay.BlendText({std::min(px+7, XRES-260), std::clamp(py+7, 0, YRES-14)}, ByteString(label).FromUtf8(), color.WithAlpha(255));
    }
}
VideoBuffer StatusPanel(const Json::Value &sample)
{
    VideoBuffer panel({XRES, StatusHeight});
    panel.DrawFilledRect({{0, 0}, {XRES, StatusHeight}}, 0x101820_rgb);
    auto lines = MetricLines(sample);
    auto status = sample["status"].asString();
    auto color = status == "LIVE" || status == "PAUSED" ? 0x8CE6B0_rgb : 0xFFD080_rgb;
    for (int i = 0; i < 3; ++i)
        panel.BlendText({6, 5+i*14}, ByteString(lines[i]).FromUtf8(), (i ? 0xCAD5E0_rgb : color).WithAlpha(255));
    int index = 0;
    for (const auto &p : sample["players"])
    {
        auto label = "#"+std::to_string(p["player"].asUInt())+" "+p["name"].asString()+(p["admin"].asBool() ? " (admin)" : "");
        panel.BlendText({6+(index%2)*306, 49+(index/2)*14}, ByteString(label).FromUtf8(), PlayerColor(p["player"].asUInt()).WithAlpha(255));
        ++index;
    }
    return panel;
}
struct Display
{
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    SDL_Texture *status = nullptr;
    void Open()
    {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
        window = SDL_CreateWindow("The Powder Toy Multiplayer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            XRES*2, (YRES+StatusHeight)*2, SDL_WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer) throw std::runtime_error(SDL_GetError());
        SDL_RenderSetLogicalSize(renderer, XRES, YRES+StatusHeight);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, XRES, YRES);
        if (!texture) throw std::runtime_error(SDL_GetError());
        status = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, XRES, StatusHeight);
        if (!status) throw std::runtime_error(SDL_GetError());
    }
    ~Display()
    {
        if (texture) SDL_DestroyTexture(texture);
        if (status) SDL_DestroyTexture(status);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) { SDL_DestroyWindow(window); SDL_Quit(); }
    }
};
}


int main(int argc, char **argv)
{
    try
    {
        auto options = Arguments(argc, argv);
        std::optional<GlobalPrefs> prefs;
        if (options.localSim) prefs.emplace();
        SimulationData simulationData;
        auto catalog = ToolCatalog();
        if (options.listTools) { std::cout << JsonText(catalog) << '\n'; return 0; }
        if (!options.tool.empty()) FindTool(options.tool);
        std::deque<Json::Value> scriptedCommands;
        if (!options.commands.empty())
        {
            std::ifstream input(options.commands);
            if (!input) throw std::runtime_error("cannot open command file");
            std::string line;
            while (std::getline(input, line))
            {
                if (line.empty()) continue;
                if (scriptedCommands.size() >= 10000) throw std::runtime_error("too many scripted commands");
                auto command = ParseJson(line); command["seq"] = Json::UInt64(1);
                if (command["op"] == "admin")
                {
                    if (command["action"] == "kick") Integer(command, "player", 1, INT32_MAX);
                    else if (command["action"] == "lock" && command["locked"].isBool()) {}
                    else throw std::runtime_error("invalid scripted administration action");
                }
                else ParseCommand(command, true);
                command.removeMember("seq"); scriptedCommands.push_back(command);
            }
            if (!options.headless || !options.runFor || options.reconnect)
                throw std::runtime_error("--commands requires --headless --run-for SECONDS --no-reconnect");
        }
        auto began = MonotonicSeconds();
        std::ofstream metricsLog;
        if (!options.metrics.empty())
        {
            metricsLog.open(options.metrics);
            if (!metricsLog) throw std::runtime_error("cannot open metrics log");
        }
        std::vector<pixel> video;
        Display display;
        if (!options.headless) display.Open();
        SessionOptions sessionOptions;
        sessionOptions.url = options.url; sessionOptions.name = options.name; sessionOptions.ca = options.ca;
        sessionOptions.roomToken = Env("TPT_ROOM_TOKEN"); sessionOptions.adminToken = Env("TPT_ADMIN_TOKEN");
        sessionOptions.localSim = options.localSim; sessionOptions.reconnect = options.reconnect;
        sessionOptions.view = options.view; sessionOptions.debugCorruptTick = options.debugCorruptTick;
        sessionOptions.debugStepDelayMs = options.debugStepDelayMs;
        sessionOptions.serverErrorsFatal = !options.commands.empty();
        Session session(sessionOptions);
        // The picture of a local-simulation session is rendered here.
        std::optional<Renderer> localRenderer;
        if (options.localSim) localRenderer.emplace();
        uint64_t renderedTick = UINT64_MAX, renderedRevision = UINT64_MAX;
        int renderedView = -1;
        double lastLocalRender = -1;

        constexpr int palette[] = {PT_DUST, PT_WATR, PT_STNE, PT_FIRE, PT_METL, PT_SPRK, PT_OIL, PT_WOOD, PT_NONE};
        const char *names[] = {"DUST", "WATR", "STNE", "FIRE", "METL", "SPRK", "OIL", "WOOD", "ERASE"};
        int selected = 0, radius = 4, received = 0, x = 100, y = 100, lastX = x, lastY = y;
        bool quit = false, drawing = false, erasing = false, strokePending = false, mouseInside = false;
        bool showStatus = true;
        double nextMetrics = began+1, nextPanel = 0, nextScript = 0;
        Json::Value sample;
        std::string selectedTool = options.tool;
        std::string strokeAction;
        bool chooseTool = false, squareBrush = false;
        std::string toolQuery;
        std::vector<unsigned> toolMatches;
        unsigned toolChoice = 0;
        auto filterTools = [&] {
            toolMatches.clear(); toolChoice = 0;
            auto query = toolQuery;
            std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            for (unsigned i = 0; i < catalog.size(); ++i)
            {
                auto text = catalog[i]["identifier"].asString()+" "+catalog[i]["name"].asString();
                std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (text.find(query) != std::string::npos) toolMatches.push_back(i);
            }
        };
        auto lastStroke = std::chrono::steady_clock::now();
        while (!quit && (!options.frames || received < options.frames) && (!options.runFor || MonotonicSeconds()-began < options.runFor))
        {
            session.Update();
            if (session.Ready() && !session.ToolActionsSupported() && !selectedTool.empty())
                throw std::runtime_error("server does not support extended tools");
            // A lost connection or local world ends any stroke in progress.
            if (!session.Ready() || (options.localSim && !session.Synchronized())) drawing = strokePending = false;
            auto *replica = session.Replica();
            // Render the local world at most 60 times per second, only when it changed.
            if (replica && session.Synchronized() &&
                (replica->world.tick != renderedTick || replica->world.revision != renderedRevision || session.View() != renderedView) &&
                MonotonicSeconds()-lastLocalRender >= 1.0/60)
            {
                auto &w = replica->world;
                auto local = RenderCanvas(*localRenderer, *w.sim, session.Picture().id+1, w.tick, w.revision, w.paused,
                    uint8_t(session.View()), {}, 0, 1, false);
                local.capturedUs = MonotonicMicros();
                renderedTick = w.tick; renderedRevision = w.revision; renderedView = session.View();
                lastLocalRender = MonotonicSeconds();
                session.PublishLocalPicture(std::move(local));
            }
            const auto &frame = session.Picture().frame;
            if (session.TakePictureChanged())
            {
                ++received;
                video = CanvasVideo(frame);
                if (display.window) SDL_UpdateTexture(display.texture, nullptr, video.data(), XRES * sizeof(pixel));
            }
            bool editable = session.Synchronized() && session.Picture().id;
            if (editable && !scriptedCommands.empty() && !session.Edits().Pending() && MonotonicSeconds() >= nextScript)
            {
                if (session.Send(scriptedCommands.front())) scriptedCommands.pop_front();
                nextScript = MonotonicSeconds()+.7; // Allow one full-canvas operation per work-budget refill.
            }
            sample = session.Sample();
            if (MonotonicSeconds() >= nextMetrics)
            {
                sample["elapsedSeconds"] = MonotonicSeconds()-began;
                if (options.localSim)
                {
                    sample["editLatencyMs"] = Json::Value(Json::arrayValue);
                    for (auto ms : session.TakeEditLatencies()) sample["editLatencyMs"].append(ms);
                }
                if (metricsLog.is_open()) { metricsLog << JsonText(sample) << '\n'; metricsLog.flush(); }
                nextMetrics = MonotonicSeconds()+1;
            }
            if (!display.window) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            const auto &players = session.Players();
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_QUIT) quit = true;
                if (chooseTool)
                {
                    if (event.type == SDL_TEXTINPUT) { if (toolQuery.size() < 64) toolQuery += event.text.text; filterTools(); }
                    if (event.type == SDL_KEYDOWN)
                    {
                        auto key = event.key.keysym.sym;
                        if (key == SDLK_BACKSPACE && !toolQuery.empty()) { toolQuery.pop_back(); filterTools(); }
                        if (key == SDLK_DOWN && toolChoice+1 < toolMatches.size()) ++toolChoice;
                        if (key == SDLK_UP && toolChoice) --toolChoice;
                        if (key == SDLK_RETURN && !toolMatches.empty()) selectedTool = catalog[toolMatches[toolChoice]]["identifier"].asString();
                        if (key == SDLK_ESCAPE || key == SDLK_RETURN) { chooseTool = false; SDL_StopTextInput(); }
                    }
                    continue;
                }
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    drawing = strokePending = mouseInside = false;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_LEAVE) mouseInside = false;
                if (event.type == SDL_MOUSEMOTION) { x = event.motion.x; y = event.motion.y; mouseInside = true; }
                if (event.type == SDL_MOUSEBUTTONDOWN && editable &&
                    event.button.x >= 0 && event.button.x < XRES && event.button.y >= 0 && event.button.y < YRES &&
                    (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT))
                {
                    x = lastX = event.button.x; y = lastY = event.button.y;
                    drawing = strokePending = true; erasing = event.button.button == SDL_BUTTON_RIGHT;
                    auto mods = SDL_GetModState();
                    strokeAction = (mods & KMOD_CTRL) ? ((mods & KMOD_SHIFT) ? "fill" : "box") : ((mods & KMOD_SHIFT) ? "line" : "");
                    if (!session.ToolActionsSupported()) strokeAction.clear();
                    if (!strokeAction.empty() && strokeAction != "fill") strokePending = false;
                }
                if (event.type == SDL_MOUSEBUTTONUP && drawing)
                {
                    x = event.button.x; y = event.button.y;
                    drawing = false; strokePending = true;
                }
                if (event.type == SDL_MOUSEWHEEL) radius = std::clamp(radius + event.wheel.y, 0, 16);
                if (event.type == SDL_KEYDOWN && !event.key.repeat)
                {
                    auto key = event.key.keysym.sym;
                    auto mods = event.key.keysym.mod;
                    if (key >= SDLK_1 && key <= SDLK_9 && !(mods & KMOD_CTRL)) { selected = int(key - SDLK_1); selectedTool.clear(); }
                    if (key == SDLK_e && session.ToolActionsSupported()) { chooseTool = true; drawing = strokePending = false; toolQuery.clear(); filterTools(); SDL_StartTextInput(); }
                    if (key == SDLK_b && session.ToolActionsSupported()) squareBrush = !squareBrush;
                    if (key == SDLK_ESCAPE) quit = true;
                    if (key == SDLK_F4)
                    {
                        showStatus = !showStatus;
                        SDL_RenderSetLogicalSize(display.renderer, XRES, YRES+(showStatus ? StatusHeight : 0));
                    }
                    if (key == SDLK_F1 || key == SDLK_F2 || key == SDLK_F3) session.SetView(int(key-SDLK_F1));
                    if (session.Admin() && editable)
                    {
                        Json::Value command;
                        if (key == SDLK_SPACE) { command["op"] = "pause"; command["paused"] = !frame.paused; }
                        if (key == SDLK_f) command["op"] = "step";
                        if (key == SDLK_BACKSPACE && (mods & KMOD_CTRL)) command["op"] = "clear";
                        if (key == SDLK_l && (mods & KMOD_CTRL))
                        { command["op"] = "admin"; command["action"] = "lock"; command["locked"] = !session.RoomLocked(); }
                        if (key >= SDLK_1 && key <= SDLK_4 && (mods & KMOD_CTRL) && (mods & KMOD_SHIFT))
                        {
                            auto index = unsigned(key - SDLK_1);
                            if (index < players.size() && players[index]["player"].asUInt() != session.OwnPlayer() && !players[index]["admin"].asBool())
                            { command["op"] = "admin"; command["action"] = "kick"; command["player"] = players[index]["player"]; }
                        }
                        if (!command.isNull()) session.Send(command);
                    }
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (editable && strokePending && !strokeAction.empty())
            {
                Json::Value c; c["op"] = "tool"; c["radius"] = radius; c["action"] = strokeAction; c["dragging"] = false;
                c["brush"] = squareBrush ? "rectangle" : "ellipse";
                c["tool"] = erasing ? "DEFAULT_PT_NONE" : (selectedTool.empty() ? std::string(simulationData.elements[palette[selected]].Identifier) : selectedTool);
                c["x1"] = std::clamp(lastX, CELL, XRES-CELL-1); c["y1"] = std::clamp(lastY, CELL, YRES-CELL-1);
                c["x2"] = std::clamp(x, CELL, XRES-CELL-1); c["y2"] = std::clamp(y, CELL, YRES-CELL-1);
                session.Send(c); strokePending = false;
            }
            if (editable && strokeAction.empty() &&
                (strokePending || (drawing && now - lastStroke >= std::chrono::milliseconds(33))))
            {
                x = std::clamp(x, CELL, XRES-CELL-1); y = std::clamp(y, CELL, YRES-CELL-1);
                lastX = std::clamp(lastX, CELL, XRES-CELL-1); lastY = std::clamp(lastY, CELL, YRES-CELL-1);
                // Keep each segment's raster work within the server's request budget.
                int span = std::max(std::abs(x-lastX), std::abs(y-lastY));
                int maxSpan = std::max(1, 65536 / ((radius*2+1)*(radius*2+1)) - 1);
                int steps = std::max(1, (span + maxSpan - 1) / maxSpan);
                for (int i = 1; i <= steps; ++i)
                {
                    Json::Value c; c["op"] = "brush"; c["radius"] = radius; c["element"] = erasing ? 0 : palette[selected];
                    if ((!selectedTool.empty() || squareBrush) && !erasing)
                    {
                        c["op"] = "tool"; c["tool"] = selectedTool.empty() ? std::string(simulationData.elements[palette[selected]].Identifier) : selectedTool;
                        c["action"] = !drawing && i == steps ? "end" : ((x == lastX && y == lastY) ? "point" : "line");
                        c["brush"] = squareBrush ? "rectangle" : "ellipse";
                    }
                    c["x1"] = lastX + (x-lastX)*(i-1)/steps; c["y1"] = lastY + (y-lastY)*(i-1)/steps;
                    c["x2"] = lastX + (x-lastX)*i/steps; c["y2"] = lastY + (y-lastY)*i/steps;
                    if (!session.Send(c)) break;
                }
                lastX = x; lastY = y; lastStroke = now; strokePending = false;
            }
            session.SendCursor(mouseInside && editable, x, y, radius, drawing);
            if (!video.empty())
            {
                VideoBuffer overlay(video.data(), RES, XRES);
                DrawPresence(overlay, players, session.OwnPlayer());
                if (chooseTool)
                {
                    overlay.DrawFilledRect({{60, 40}, {492, 290}}, 0x101820_rgb);
                    overlay.BlendText({72, 52}, ByteString("Tool: "+toolQuery+" | Enter select, Esc cancel").FromUtf8(), (0xFFFFFF_rgb).WithAlpha(255));
                    unsigned start = toolChoice > 14 ? toolChoice-14 : 0;
                    for (unsigned row = 0; row < 16 && start+row < toolMatches.size(); ++row)
                    {
                        auto &tool = catalog[toolMatches[start+row]];
                        auto label = (start+row == toolChoice ? "> " : "  ")+tool["name"].asString()+"  "+tool["identifier"].asString();
                        overlay.BlendText({72, 74+int(row)*15}, ByteString(label).FromUtf8(), (0xCAD5E0_rgb).WithAlpha(255));
                    }
                }
                SDL_UpdateTexture(display.texture, nullptr, overlay.Data(), XRES*sizeof(pixel));
            }
            SDL_SetRenderDrawColor(display.renderer, 0, 0, 0, 255); SDL_RenderClear(display.renderer);
            SDL_Rect canvasRect{0, 0, XRES, YRES};
            if (!video.empty()) SDL_RenderCopy(display.renderer, display.texture, nullptr, &canvasRect);
            SDL_SetRenderDrawColor(display.renderer, 255, 255, 255, 255);
            SDL_RenderSetClipRect(display.renderer, &canvasRect);
            SDL_Rect cursor{x-radius, y-radius, radius*2+1, radius*2+1}; SDL_RenderDrawRect(display.renderer, &cursor);
            if (drawing && strokeAction == "line") SDL_RenderDrawLine(display.renderer, lastX, lastY, x, y);
            if (drawing && strokeAction == "box")
            {
                SDL_Rect box{std::min(x,lastX), std::min(y,lastY), std::abs(x-lastX)+1, std::abs(y-lastY)+1};
                SDL_RenderDrawRect(display.renderer, &box);
            }
            SDL_RenderSetClipRect(display.renderer, nullptr);
            if (showStatus)
            {
                if (MonotonicSeconds() >= nextPanel)
                {
                    auto panel = StatusPanel(sample);
                    SDL_UpdateTexture(display.status, nullptr, panel.Data(), XRES*sizeof(pixel));
                    nextPanel = MonotonicSeconds()+.2;
                }
                SDL_Rect statusRect{0, YRES, XRES, StatusHeight};
                SDL_RenderCopy(display.renderer, display.status, nullptr, &statusRect);
            }
            SDL_RenderPresent(display.renderer);
            std::string title = std::string("TPT Multiplayer | ") + sample["status"].asString() + " | " + (selectedTool.empty() ? names[selected] : selectedTool) + " | radius " + std::to_string(radius)
                + " | tick " + std::to_string(frame.tick) + (frame.paused ? " | PAUSED" : "")
                + (session.Admin() ? " | admin" : "") + (session.RoomLocked() ? " | ROOM LOCKED" : "")
                + " | 1-9 materials, wheel brush, right click erase";
            SDL_SetWindowTitle(display.window, title.c_str());
        }
        const auto &frame = session.Picture().frame;
        bool complete = session.Synchronized() && session.Picture().id;
        if (!options.commands.empty() && (!scriptedCommands.empty() || session.Edits().Pending() || frame.revision < session.AcknowledgedRevision()))
            throw std::runtime_error("run ended before commands and their resulting canvas were confirmed");
        complete = complete && !session.Disconnected();
        session.Stop();
        if (!complete && !quit)
            throw std::runtime_error("run ended before a complete current canvas was received");
        sample = session.Sample();
        sample["connected"] = complete;
        sample["elapsedSeconds"] = MonotonicSeconds()-began;
        if (metricsLog.is_open()) metricsLog << JsonText(sample) << '\n';
        if (!options.output.empty() && session.Picture().id)
        {
            auto png = VideoBuffer(video.data(), RES, XRES).ToPNG();
            if (!png || !Platform::WriteFile(*png, options.output)) throw std::runtime_error("cannot write screenshot");
        }
        if (!options.outputHud.empty() && session.Picture().id)
        {
            VideoBuffer screenshot({XRES, YRES+StatusHeight});
            screenshot.BlendImage(video.data(), 255, {{0,0}, RES});
            // Draw into a canvas-sized buffer so cursors cannot overlap the status panel.
            VideoBuffer overlay(video.data(), RES, XRES); DrawPresence(overlay, session.Players(), session.OwnPlayer());
            screenshot.BlendImage(overlay.Data(), 255, {{0,0}, RES});
            auto panel = StatusPanel(sample);
            screenshot.BlendImage(panel.Data(), 255, {{0,YRES}, {XRES,StatusHeight}});
            auto png = screenshot.ToPNG();
            if (!png || !Platform::WriteFile(*png, options.outputHud)) throw std::runtime_error("cannot write status screenshot");
        }
        std::cout << "Received " << received << " frames; tick=" << frame.tick << " revision=" << frame.revision << std::endl;
        if (options.localSim)
            std::cout << "Local simulation: seals=" << sample["seals"].asUInt64() << " verified_hashes=" << sample["verifiedHashes"].asUInt64()
                << " commands=" << sample["appliedCommands"].asUInt64() << " resyncs=" << session.Resyncs()
                << " connections=" << session.Connections() << " checksum=" << (frame.pixels ? frame.pixels->checksum : 0) << std::endl;
        return 0;
    }
    catch (const std::exception &e) { std::cerr << "Client: " << e.what() << '\n'; return 1; }
}
