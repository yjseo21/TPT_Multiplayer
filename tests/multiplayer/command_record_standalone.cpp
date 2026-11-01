// Release builds set NDEBUG (b_ndebug=true); these checks must always run.
#undef NDEBUG
#include "multiplayer/CommandRecord.h"
#include <cassert>
#include <iostream>
#include <stdexcept>

using multiplayer::Command;
using multiplayer::RecordCommand;
int main()
{
    Command c{};
    c.sequence=17; c.kind=Command::Brush; c.x1=90; c.x2=110;
    c.y1=80; c.y2=81; c.radius=3; c.element=2;
    auto b=RecordCommand(c);
    assert(b.size()==8 && b["op"]=="brush" && b["seq"].asUInt64()==17);
    assert(b["x1"]==90 && b["y2"]==81 && b["element"]==2);
    c.kind=Command::ToolAction; c.tool="DEFAULT_PT_WATR";
    c.action="end"; c.brush="rectangle"; c.strength=77;
    c.dragging=false; c.shift=true; c.ctrl=false; c.alt=true;
    c.flags=3; c.replace=14;
    auto t=RecordCommand(c);
    assert(t["op"]=="tool" && t["tool"]=="DEFAULT_PT_WATR");
    assert(t["dragging"]==false && t["shift"]==true && t["alt"]==true);
    assert(t["strength"]==77 && t["flags"]==3 && t["replace"]==14);
    c.kind=Command::Pause; c.paused=true;
    assert(RecordCommand(c)["paused"]==true);
    c.kind=Command::Step;
    assert(RecordCommand(c)["op"]=="step");
    c.kind=Command::Clear;
    assert(RecordCommand(c)["op"]=="clear");
    c.kind=static_cast<Command::Kind>(-1);
    try { RecordCommand(c); throw std::logic_error("invalid kind accepted"); }
    catch (const std::runtime_error &) {}
    std::cout << "command record tests passed\n";
}
