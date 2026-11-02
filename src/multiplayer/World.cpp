#include "World.h"
#include "FloatingPoint.h"
#include "Autosave.h"
#include "ImportSeed.h"
#include "Tools.h"
#include "client/GameSave.h"
#include "common/platform/Platform.h"
#include "gui/game/EllipseBrush.h"
#include "simulation/Air.h"
#include <stdexcept>

namespace multiplayer
{
World::World() : sim(Simulation::Factory()) {}
void ApplySettings(Simulation &sim, const SimulationSettingsChange &s)
{
    // Newtonian gravity runs on a worker thread whose timing differs between
    // machines; it is refused when parsed and never reaches here.
    if (s.heat) sim.legacy_enable = !*s.heat;
    if (s.ambientHeat) sim.aheat_enable = *s.ambientHeat;
    if (s.waterEqualisation) sim.water_equal_test = *s.waterEqualisation;
    if (s.prettyPowder) sim.pretty_powder = *s.prettyPowder;
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
    case Command::Settings: ApplySettings(*sim, c.settings); break;
    }
    ++revision;
    // Map/free-list maintenance without advancing physics, including while paused.
    sim->BeforeSim(false);
}
void World::Load(const std::string &path)
{
    std::vector<char> bytes;
    if (!Platform::ReadFile(bytes, path)) throw std::runtime_error("cannot read world file");
    GameSave save(bytes, false);
    // Newtonian gravity is not deterministic across machines yet.
    if (save.gravityEnable) throw std::runtime_error("this save uses Newtonian gravity, which rooms do not support yet; turn it off and save again");
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
    sim->Load(&save, true, {0, 0});
    sim->frameCount = save.frameCount;
    if (save.hasRngState) sim->rng.state(save.rngState);
    sim->ensureDeterminism = save.ensureDeterminism;
    paused = save.paused;
    sim->BeforeSim(false);
    ++revision;
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
