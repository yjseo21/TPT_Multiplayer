#include "multiplayer/ReplicaState.h"
#include "multiplayer/ExactCheckpoint.h"
#include "multiplayer/CheckpointWire.h"
#include "multiplayer/CommandRecord.h"
#include "multiplayer/StateDigest.h"
#include "prefs/GlobalPrefs.h"
#include "simulation/SimulationData.h"
#include "simulation/ElementClasses.h"
#include <stdexcept>
#include <iostream>
using namespace multiplayer;
namespace {
void Need(bool ok,const char *s){if(!ok)throw std::runtime_error(s);}
template<class F> void Reject(F f){try{f();}catch(const std::exception &){return;}throw std::runtime_error("missed invalid packet");}
Json::Value Welcome(){Json::Value v;v["type"]="welcome";v["protocol"]=ProtocolVersion;v["build"]=BuildId;v["localSimulation"]=true;return v;}
Json::Value Seal(const World &w){Json::Value v;v["type"]="tick_seal";v["tick"]=Json::UInt64(w.tick);v["revision"]=Json::UInt64(w.revision);v["resetGeneration"]=Json::UInt64(w.resetGeneration);v["paused"]=w.paused;v["digest"]=DigestSummary(DiagnosticDigests(w));return v;}
}
int main(int argc, char **argv)
{
 try {
 const int steps = argc > 1 ? std::stoi(argv[1]) : 120;
 if (steps < 1 || steps > 10000) throw std::runtime_error("steps must be between 1 and 10000");
 GlobalPrefs prefs;SimulationData data;
 World authoritative;authoritative.sim->ensureDeterminism=true;authoritative.sim->rng.seed(642);
 for(int x=90;x<120;++x)for(int y=80;y<105;++y)authoritative.sim->create_part(-1,x,y,PT_WATR);
 authoritative.sim->BeforeSim(false);
 auto raw=EncodeExactCheckpoint(authoritative);
 ReplicaState replica;
 Reject([&]{replica.OnJson(Seal(authoritative));});
 replica.OnJson(Welcome());
 Json::Value begin;begin["type"]="checkpoint_begin";begin["id"]=Json::UInt64(17);
 begin["bytes"]=Json::UInt64(raw.size());begin["tick"]=Json::UInt64(authoritative.tick);
 begin["revision"]=Json::UInt64(authoritative.revision);
 begin["resetGeneration"]=Json::UInt64(authoritative.resetGeneration);begin["paused"]=authoritative.paused;
 replica.OnJson(begin);
 for(size_t off=0;off<raw.size();off+=CheckpointChunkBytes)
   replica.OnBinary(EncodeCheckpointChunk(17,raw,off));
 Json::Value end;end["type"]="checkpoint_end";end["id"]=Json::UInt64(17);replica.OnJson(end);
 Need(replica.Ready(),"replica did not load checkpoint");
 Need(EncodeExactCheckpoint(replica.world)==raw,"replica exact bootstrap bytes differ");
 // Server processes an edit *before* advancing to the seal's target tick.
 Command edit{};edit.kind=Command::Brush;edit.sequence=1;edit.x1=200;edit.y1=100;
 edit.x2=203;edit.y2=100;edit.radius=1;edit.element=PT_FIRE;
 authoritative.Apply(edit);
 Json::Value commit;commit["type"]="command_commit";commit["id"]=Json::UInt64(88);
 commit["tick"]=Json::UInt64(0);commit["afterTick"]=Json::UInt64(0);
 commit["revision"]=Json::UInt64(authoritative.revision);
 commit["resetGeneration"]=Json::UInt64(authoritative.resetGeneration);
 commit["command"]=RecordCommand(edit);
 replica.OnJson(commit);
 for(int i=0;i<steps;++i)authoritative.Advance();
 auto seal=Seal(authoritative);
 replica.OnJson(seal);
 Need(replica.world.tick==uint64_t(steps) && replica.Edits()==1 && replica.Checks()==1,"replica did not match long seal");
 auto broken=Seal(authoritative);broken["digest"]="aaaaaaaaaaaaaaaa";
 Reject([&]{replica.OnJson(broken);});
 auto stale=commit;stale["tick"]=Json::UInt64(steps);
 Reject([&]{replica.OnJson(stale);});
 // Paced mode: the same stream queues, then runs within each step budget.
 ReplicaState paced;paced.SetPaced(true);
 paced.OnJson(Welcome());paced.OnJson(begin);
 for(size_t off=0;off<raw.size();off+=CheckpointChunkBytes)
   paced.OnBinary(EncodeCheckpointChunk(17,raw,off));
 paced.OnJson(end);paced.OnJson(commit);paced.OnJson(seal);
 Need(paced.world.tick==0 && paced.Edits()==0 && paced.Backlog()==uint64_t(steps),"paced replica ran before Pump");
 Need(paced.Pump(0)==0 && paced.Edits()==1 && paced.world.tick==0,"paced replica did not apply the commit at its tick");
 Need(paced.Pump(1)==1 && paced.world.tick==1 && paced.Checks()==0,"paced replica exceeded its step budget");
 Need(paced.Pump(steps)==uint64_t(steps-1) && paced.Backlog()==0 && paced.Checks()==1,"paced replica did not finish the seal");
 Need(EncodeExactCheckpoint(paced.world)==EncodeExactCheckpoint(replica.world),"paced and immediate replicas differ");
 auto behind=seal;behind["tick"]=Json::UInt64(steps-1);
 Reject([&]{paced.OnJson(behind);});
 std::cout<<"replica: exact bootstrap + ordered edit + " << steps << " simulated ticks + divergence detection + paced stepping PASS\n";
 return 0;
 }catch(const std::exception &e){std::cerr<<"replica test FAILED: "<<e.what()<<'\n';return 1;}
}
