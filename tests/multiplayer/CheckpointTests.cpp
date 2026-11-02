#include <algorithm>
#include "multiplayer/ExactCheckpoint.h"
#include "multiplayer/World.h"
#include "multiplayer/StateDigest.h"
#include "simulation/SimulationData.h"
#include "simulation/Simulation.h"
#include "simulation/Air.h"
#include "simulation/ElementClasses.h"
#include "prefs/GlobalPrefs.h"
#include <stdexcept>
#include <iostream>
#include <string>

using namespace multiplayer;
namespace
{
void Check(bool ok, const char *why)
{
    if (!ok) throw std::runtime_error(why);
}
template<class F> void Reject(F f, const char *why)
{
    try { f(); } catch (const std::exception &) { return; }
    throw std::runtime_error(why);
}
void Compare(const World &a, const World &b)
{
    Check(a.tick==b.tick && a.revision==b.revision && a.paused==b.paused &&
          a.resetGeneration==b.resetGeneration, "checkpoint lost world metadata");
    Check(DiagnosticDigests(a)==DiagnosticDigests(b),"checkpoint diagnostic state mismatch");
    Check(a.sim->parts.FreeHead()==b.sim->parts.FreeHead() &&
          a.sim->parts.active==b.sim->parts.active,"checkpoint lost particle allocation order");
}
}
int main()
{
    try
    {
        GlobalPrefs prefs;
        SimulationData data;
        World original;
        original.paused=true;
        original.sim->ensureDeterminism=true;
        original.sim->rng.seed(0x1234);
        // Nontrivial allocations, deleted slots, electronic/air transient state.
        for (int x=100;x<170;++x)
            for (int y=70;y<90;++y)
                original.sim->create_part(-1,x,y,(x+y)%2 ? PT_WATR : PT_DUST);
        for (int x=100;x<170;x+=2)
            original.sim->delete_part(x,71);
        original.sim->BeforeSim(false);
        original.sim->pv[10][20]=12.25f;
        original.sim->air->opv[10][20]=-3.5f;
        original.sim->air->ovx[10][20]=4.0f;
        original.sim->Element_PPIP_ppip_changed=7;
        original.sim->signs.emplace_back("Exact checkpoint",50,80,sign::Middle);
        original.tick=25; original.revision=10;
        original.resetGeneration=2;
        auto raw=CaptureExactCheckpointRaw(original);
        auto checkpoint=EncodeExactCheckpoint(original);
        Check(CompressExactCheckpointRaw(raw)==checkpoint,
              "raw capture / standalone compression differs from original packet");
        Reject([&] { CompressExactCheckpointRaw({}); },
               "accepted an empty raw checkpoint");
        Check(checkpoint.size()>20 && checkpoint.size()<8*1024*1024,
              "zlib compression did not reduce ordinary sparse checkpoint");
        World restored;
        RestoreExactCheckpoint(restored,checkpoint);
        Compare(original,restored);
        Check(EncodeExactCheckpoint(restored)==checkpoint,
              "complete canonical exact checkpoint did not round trip");
        // Existing world must survive decode failures, not be partially overwritten.
        auto clean=EncodeExactCheckpoint(restored);
        Reject([&] { RestoreExactCheckpoint(restored,checkpoint.substr(0,checkpoint.size()-4)); },
               "accepted truncated checkpoint");
        Check(EncodeExactCheckpoint(restored)==clean,"failed restore corrupted current world");
        // A changed *state* must never be accepted. Two different deflate bit
        // streams may legitimately decode to identical bytes (e.g. alternative
        // back-reference distances inside a run of zeros), so arbitrary
        // compressed-byte flips must either be rejected or restore the exact
        // original canonical world.
        for (size_t at=20;at<checkpoint.size();at+=std::max<size_t>(1,checkpoint.size()/97))
        {
            auto flipped=checkpoint; flipped[at] ^= 0x40;
            World probe;
            try { RestoreExactCheckpoint(probe,flipped); }
            catch (const std::exception &) { continue; }
            Check(EncodeExactCheckpoint(probe)==checkpoint,"accepted a compressed-byte flip that changed state");
        }
        // Raw payload changed but the declared CRC left as the original.
        auto rawChanged=raw; rawChanged[rawChanged.size()/2] ^= 0x40;
        auto changed=CompressExactCheckpointRaw(rawChanged);
        for (unsigned i=16;i<20;++i) changed[i]=checkpoint[i];
        Reject([&] { RestoreExactCheckpoint(restored,changed); },"accepted tampered checkpoint payload");
        changed=checkpoint; changed[17] ^= 0x01;
        Reject([&] { RestoreExactCheckpoint(restored,changed); },"accepted tampered checkpoint CRC");
        changed=checkpoint+"trailing";
        Reject([&] { RestoreExactCheckpoint(restored,changed); },"accepted trailing compressed bytes");
        changed=checkpoint; changed[8]=char(9);
        Reject([&] { RestoreExactCheckpoint(restored,changed); },"accepted wrong checkpoint version");
        changed=checkpoint; changed[15]=char(127);
        Reject([&] { RestoreExactCheckpoint(restored,changed); },"accepted impossible decompressed length");
        Check(EncodeExactCheckpoint(restored)==clean,"corrupt packet mutated the world");
        // Both worlds allocate from the exact same free-list head after restore.
        auto a=original.sim->create_part(-1,180,110,PT_WATR);
        auto b=restored.sim->create_part(-1,180,110,PT_WATR);
        Check(a==b && a>=0,"restored particle allocation diverged");
        original.sim->BeforeSim(false); restored.sim->BeforeSim(false);
        Compare(original,restored);
        // Test independently executed physics, not just bytewise decoding.
        for (int i=0;i<120;++i)
        {
            original.Advance(); restored.Advance();
            if (i%20==0)
            {
                Compare(original,restored);
                Check(EncodeExactCheckpoint(original)==EncodeExactCheckpoint(restored),
                      "independent simulation state bytes diverged after restore");
            }
        }
        Check(EncodeExactCheckpoint(original)==EncodeExactCheckpoint(restored),
              "exact state diverged after continued physics");
        // Rooms' own gravity solver is part of the state; the game's threaded one is not.
        restored.sim->EnableNewtonianGravity(true);
        World withGravity;
        RestoreExactCheckpoint(withGravity, EncodeExactCheckpoint(restored));
        Check(withGravity.sim->grav && EncodeExactCheckpoint(withGravity)==EncodeExactCheckpoint(restored),
              "room gravity should round-trip exactly");
        restored.sim->EnableNewtonianGravity(false);
        restored.sim->deterministicGravity = false;
        restored.sim->EnableNewtonianGravity(true);
        Reject([&] { EncodeExactCheckpoint(restored); },"checkpoint with the game's gravity worker should be unsupported");
        std::cout<<"ExactCheckpointV1: round-trip, corruption, allocator and 120-tick physics replay PASS\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr<<"ExactCheckpointV1 FAIL: "<<e.what()<<'\n';
        return 1;
    }
}
