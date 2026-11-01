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
