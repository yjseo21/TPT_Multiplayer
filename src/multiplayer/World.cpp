#include "World.h"
#include "FloatingPoint.h"
#include "Autosave.h"
#include "ImportSeed.h"
#include "Tools.h"
#include "client/GameSave.h"
#include "common/platform/Platform.h"
#include "gui/game/EllipseBrush.h"
#include "graphics/Renderer.h"
#include "simulation/Air.h"
#include "simulation/AccessProperty.h"
#include "simulation/Sign.h"
#include "simulation/ElementClasses.h"
#include "simulation/SimulationData.h"
#include <cstring>
#include <stdexcept>

namespace multiplayer
{
World::World() : sim(Simulation::Factory())
{
    sim->deterministicGravity = true;
    // Physics reads some colour tables (fireworks pick their colour from one).
    // They are filled by the renderer or the save loader, which a room server
    // may never use: without this, the first firework crashed the server.
    Renderer::PopulateTables();
}
// Room stickmen: slot 1 is STKM, slot 2 STKM2, slots 3 and 4 are fighters
// steered by their player instead of their AI.
playerst *RoomStickman(Simulation &sim, int slot)
{
    if (slot == 1) return sim.player.spwn ? &sim.player : nullptr;
    if (slot == 2) return sim.player2.spwn ? &sim.player2 : nullptr;
    for (auto &fighter : sim.fighters) if (fighter.spwn && fighter.controller == slot) return &fighter;
    return nullptr;
}
namespace
{
void SpawnRoomStickman(Simulation &sim, int slot, int x, int y)
{
    // One stickman per player: placing it again moves it (the old one dies).
    for (int i = 0; i < NPART; ++i)
    {
        auto &p = sim.parts[i];
        if ((slot == 1 && p.type == PT_STKM) || (slot == 2 && p.type == PT_STKM2) ||
            (slot > 2 && p.type == PT_FIGH && p.tmp >= 0 && p.tmp < MAX_FIGHTERS && sim.fighters[p.tmp].controller == slot))
            sim.kill_part(i);
    }
    // ...including one inside a portal: it still holds its stickman (or fighter) slot.
    for (auto &channel : sim.portalp)
        for (auto &direction : channel)
            for (auto &p : direction)
            {
                bool fighter = slot > 2 && p.type == PT_FIGH && p.tmp >= 0 && p.tmp < MAX_FIGHTERS && sim.fighters[p.tmp].controller == slot;
                if (!((slot == 1 && p.type == PT_STKM) || (slot == 2 && p.type == PT_STKM2) || fighter))
                    continue;
                if (slot == 1) sim.player.spwn = 0;
                else if (slot == 2) sim.player2.spwn = 0;
                else if (sim.fighters[p.tmp].spwn) { sim.fighters[p.tmp].spwn = 0; sim.fighcount--; }
                p = Particle{};
            }
    auto i = sim.create_part(-1, x, y, slot == 1 ? PT_STKM : slot == 2 ? PT_STKM2 : PT_FIGH);
    if (i >= 0 && slot > 2 && sim.parts[i].type == PT_FIGH && sim.parts[i].tmp >= 0 && sim.parts[i].tmp < MAX_FIGHTERS)
        sim.fighters[sim.parts[i].tmp].controller = slot;
}
// The console's set / create / delete / bubble, as the game's console performs them.
void ApplyConsole(Simulation &sim, const Command &c)
{
    if (c.cmd == "set")
    {
        AccessProperty change{ c.property, c.value };
        auto at = [&](int i) { if (i >= 0 && i < NPART && sim.parts[i].type) change.Set(&sim, i); };
        if (c.selector == "index") at(c.index);
        else if (c.selector == "point")
        {
            auto r = sim.pmap[c.y1][c.x1];
            if (!r) r = sim.photons[c.y1][c.x1];
            if (r) at(ID(r));
        }
        else
            for (int i = 0; i < NPART; ++i)
                if (sim.parts[i].type && (c.selector == "all" || sim.parts[i].type == c.element)) change.Set(&sim, i);
    }
    else if (c.cmd == "create") sim.create_part(-1, c.x1, c.y1, TYP(c.element), ID(c.element) ? ID(c.element) : -1); // as the console: -1 is "no value"
    else if (c.cmd == "delete")
    {
        if (c.index >= 0) sim.kill_part(c.index);
        else sim.delete_part(c.x1, c.y1);
    }
    else if (c.cmd == "bubble")
    {
        // The console's ring of 31 SOAP particles. Its offsets are a fixed table
        // (18 cos and sin of i/5, rounded as the console does): math library results
        // differ slightly between platforms, and every machine must place the same parts.
        static constexpr int ring[30][2] = { {18,4},{17,7},{15,10},{13,13},{10,15},{7,17},{3,18},{-1,18},{-4,18},{-7,16},
            {-11,15},{-13,12},{-15,9},{-17,6},{-18,3},{-18,-1},{-17,-5},{-16,-8},{-14,-11},{-12,-14},
            {-9,-16},{-6,-17},{-2,-18},{2,-18},{5,-17},{8,-16},{11,-14},{14,-11},{16,-8},{17,-5} };
        int first = sim.create_part(-1, c.x1+18, c.y1, PT_SOAP), rem1 = first, rem2;
        for (auto [dx, dy] : ring)
        {
            rem2 = sim.create_part(-1, c.x1+dx, c.y1+dy, PT_SOAP);
            if (rem1 >= 0) { sim.parts[rem1].ctype = 7; sim.parts[rem1].tmp = rem2; }
            if (rem2 >= 0) sim.parts[rem2].tmp2 = rem1;
            rem1 = rem2;
        }
        if (rem1 >= 0) { sim.parts[rem1].ctype = 7; sim.parts[rem1].tmp = first; }
        if (first >= 0) { sim.parts[first].tmp2 = rem1; sim.parts[first].ctype = 7; }
    }
}
// The game's reset and invert actions, as its controller performs them.
void ApplyReset(Simulation &sim, const std::string &what)
{
    auto &sd = SimulationData::CRef();
    if (what == "air")
    {
        sim.air->Clear();
        for (int i = 0; i < NPART; i++)
            if (GameSave::PressureInTmp3(sim.parts[i].type)) sim.parts[i].tmp3 = 0;
    }
    else if (what == "pressure")
    {
        for (int y = 0; y < YCELLS; y++) for (int x = 0; x < XCELLS; x++) sim.pv[y][x] = 0;
    }
    else if (what == "velocity")
    {
        for (int y = 0; y < YCELLS; y++) for (int x = 0; x < XCELLS; x++) sim.vx[y][x] = sim.vy[y][x] = 0;
    }
    else if (what == "sparks") sim.ResetSparks();
    else if (what == "ambientHeat") sim.air->ClearAirH();
    else if (what == "temperature")
    {
        for (int i = 0; i < NPART; i++)
            if (sim.parts[i].type) sim.parts[i].temp = sd.elements[sim.parts[i].type].DefaultProperties.temp;
    }
    else if (what == "invertAir") sim.air->Invert();
}
}
void CheckPastedSave(const Command &paste)
{
    auto bytes = Base64Decode(paste.text);
    GameSave save(std::vector<char>(bytes.begin(), bytes.end()), false);
    if (paste.kind != Command::Load) return;
    // A loaded save brings its settings, which a save file does not check: the
    // ranges a room's settings command allows.
    auto inRange = [](float value, float minimum, float maximum) { return value >= minimum && value <= maximum; };
    if (save.gravityMode < 0 || save.gravityMode >= NUM_GRAVMODES || save.airMode < 0 || save.airMode >= NUM_AIRMODES ||
        save.edgeMode < 0 || save.edgeMode >= NUM_EDGEMODES || save.convectionMode < 0 || save.convectionMode >= NUM_CONVMODES ||
        !inRange(save.ambientAirTemp, MIN_TEMP, MAX_TEMP) || !inRange(save.edgePressure, -MAX_PRESSURE, MAX_PRESSURE) ||
        !inRange(save.edgeVelocityX, -MAX_PRESSURE, MAX_PRESSURE) || !inRange(save.edgeVelocityY, -MAX_PRESSURE, MAX_PRESSURE) ||
        !inRange(save.vorticityCoeff, 0, 1) || !inRange(save.customGravityX, -1000, 1000) || !inRange(save.customGravityY, -1000, 1000))
        throw std::runtime_error("this save's settings are out of range");
}
void ApplySettings(Simulation &sim, const SimulationSettingsChange &s)
{
    if (s.heat) sim.legacy_enable = !*s.heat;
    if (s.ambientHeat) sim.aheat_enable = *s.ambientHeat;
    if (s.waterEqualisation) sim.water_equal_test = *s.waterEqualisation;
    if (s.prettyPowder) sim.pretty_powder = *s.prettyPowder;
    if (s.newtonianGravity) sim.EnableNewtonianGravity(*s.newtonianGravity);
    if (s.airMode) sim.air->airMode = *s.airMode;
    if (s.edgeMode) sim.SetEdgeMode(*s.edgeMode);
    if (s.convectionMode) sim.air->convectionMode = *s.convectionMode;
    if (s.gravityMode) sim.gravityMode = *s.gravityMode;
    if (s.ambientAirTemp) sim.air->ambientAirTemp = *s.ambientAirTemp;
    if (s.edgePressure) sim.air->edgePressure = *s.edgePressure;
    if (s.edgeVelocityX) sim.air->edgeVelocityX = *s.edgeVelocityX;
    if (s.edgeVelocityY) sim.air->edgeVelocityY = *s.edgeVelocityY;
    if (s.vorticityCoeff) sim.air->vorticityCoeff = *s.vorticityCoeff;
    if (s.customGravityX) sim.customGravityX = *s.customGravityX;
    if (s.customGravityY) sim.customGravityY = *s.customGravityY;
}
void World::Advance()
{
    MatchGameFloatingPoint();
    sim->BeforeSim(true);
    sim->UpdateParticles(0, NPART);
    sim->AfterSim();
    ++tick;
}
void World::Apply(const Command &c)
{
    MatchGameFloatingPoint();
    switch (c.kind)
    {
    case Command::ToolAction: ApplyTool(*sim, c); break;
    case Command::Brush:
    {
        EllipseBrush brush(false); brush.SetRadius({c.radius, c.radius});
        sim->CreateLine(c.x1, c.y1, c.x2, c.y2, c.element, brush, 0);
        break;
    }
    case Command::Pause: paused = c.paused; break;
    case Command::Step:
        if (!paused) throw std::runtime_error("pause before stepping");
        Advance(); break;
    case Command::Clear: sim->clear_sim(); ++resetGeneration; break;
    case Command::Load:
    {
        // As opening a save in the game: its world and settings replace the room's,
        // which stays paused if it was.
        auto bytes = Base64Decode(c.text);
        LoadSave(std::vector<char>(bytes.begin(), bytes.end()), paused);
        break;
    }
    case Command::Settings: ApplySettings(*sim, c.settings); break;
    case Command::Reset: ApplyReset(*sim, c.what); break;
    case Command::Console: ApplyConsole(*sim, c); break;
    case Command::Sign:
        // Edits apply only if the sign is still where its editor saw it.
        if (c.index < 0)
        {
            if (sim->signs.size() < MAXSIGNS)
                sim->signs.push_back(sign(ByteString(c.text).FromUtf8(), c.x1, c.y1, sign::Justification(c.justification)));
        }
        else if (c.index < int(sim->signs.size()) && sim->signs[c.index].x == c.x2 && sim->signs[c.index].y == c.y2)
        {
            if (c.remove) sim->signs.erase(sim->signs.begin() + c.index);
            else sim->signs[c.index] = sign(ByteString(c.text).FromUtf8(), c.x1, c.y1, sign::Justification(c.justification));
        }
        break;
    case Command::Paste:
    {
        // As GameController::PlaceSave does, without its local history or pausing.
        auto bytes = Base64Decode(c.text);
        GameSave save(std::vector<char>(bytes.begin(), bytes.end()), false);
        sim->Load(&save, c.includePressure, {c.x1, c.y1});
        break;
    }
    case Command::SignButton:
        if (c.index < int(sim->signs.size()) && sim->signs[c.index].x == c.x1 && sim->signs[c.index].y == c.y1 &&
            sim->signs[c.index].split().second == sign::Type::Button)
            sim->create_part(-1, c.x1, c.y1, PT_SPRK);
        break;
    case Command::Stickman:
        if (auto *stickman = RoomStickman(*sim, c.slot)) { stickman->comm = char(c.comm); stickman->pcomm = char(c.pcomm); }
        break;
    case Command::StickmanSpawn:
        if (c.slot < 1 || c.slot > 4) throw std::runtime_error("no stickman slot");
        SpawnRoomStickman(*sim, c.slot, c.x1, c.y1);
        break;
    }
    ++revision;
    // Map/free-list maintenance without advancing physics, including while paused.
    sim->BeforeSim(false);
}
void World::Load(const std::string &path)
{
    std::vector<char> bytes;
    if (!Platform::ReadFile(bytes, path)) throw std::runtime_error("cannot read world file");
    LoadSave(std::move(bytes), false);
    ++revision;
}
void World::LoadSave(std::vector<char> bytes, bool stayPaused)
{
    GameSave save(bytes, false);
    sim->clear_sim();
    ++resetGeneration;
    // The standard file may omit RNG state. Force a deterministic initial
    // state from immutable imported bytes; embedded RNG state wins below.
    sim->rng.seed(SeedForLegacySave(std::string_view(bytes.data(), bytes.size())));
    sim->gravityMode = save.gravityMode; sim->customGravityX = save.customGravityX; sim->customGravityY = save.customGravityY;
    sim->air->airMode = save.airMode; sim->air->ambientAirTemp = save.ambientAirTemp;
    sim->air->edgePressure = save.edgePressure; sim->air->edgeVelocityX = save.edgeVelocityX; sim->air->edgeVelocityY = save.edgeVelocityY;
    sim->air->vorticityCoeff = save.vorticityCoeff; sim->air->convectionMode = save.convectionMode;
    sim->edgeMode = save.edgeMode; sim->legacy_enable = save.legacyEnable;
    sim->water_equal_test = save.waterEEnabled; sim->aheat_enable = save.aheatEnable;
    sim->EnableNewtonianGravity(save.gravityEnable);
    sim->Load(&save, true, {0, 0});
    sim->frameCount = save.frameCount;
    if (save.hasRngState) sim->rng.state(save.rngState);
    sim->ensureDeterminism = save.ensureDeterminism;
    paused = save.paused || stayPaused;
    sim->BeforeSim(false);
}
std::unique_ptr<GameSave> World::CaptureSave() const
{
    auto save = sim->Save(true, RES.OriginRect());
    save->paused = paused;
    return save;
}
void World::Save(const std::string &path) const
{
    auto save = CaptureSave();
    auto [missing, data] = save->Serialise();
    if (missing) throw std::runtime_error("cannot serialize world file");
    AtomicSaveFile(std::filesystem::path(path), data);
}
}
