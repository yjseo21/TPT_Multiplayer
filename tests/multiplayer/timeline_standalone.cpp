// Release builds set NDEBUG (b_ndebug=true); these checks must always run.
#undef NDEBUG
#include "multiplayer/TickTimeline.h"
#include <cassert>
#include <stdexcept>
#include <iostream>
using multiplayer::TickTimeline;
int main()
{
    TickTimeline seq;
    auto a=seq.Commit(0,0,0,1,0); assert(a.eventId==1 && a.tick==0 && a.ordinal==0 && a.epoch==0);
    auto b=seq.Commit(0,0,1,2,0); assert(b.eventId==2 && b.ordinal==1);
    auto c=seq.Commit(0,1,2,3,0); assert(c.afterTick==1 && c.ordinal==2);
    auto d=seq.Commit(1,1,3,4,1); assert(d.epoch==1 && d.ordinal==0);
    auto e=seq.Commit(50,50,4,5,1); assert(e.tick==50 && e.ordinal==0);
    auto reject=[&](auto fn){ try{fn();}catch(const std::runtime_error&){return;} throw std::runtime_error("invalid commit accepted");};
    reject([&]{seq.Commit(49,49,5,6,1);});
    reject([&]{seq.Commit(50,50,4,6,1);});
    reject([&]{seq.Commit(50,52,5,6,1);});
    reject([&]{seq.Commit(50,50,5,6,3);});
    auto f=seq.Commit(50,50,5,6,1); assert(f.eventId==6 && f.ordinal==1);
    std::cout << "tick timeline tests passed\n";
}
