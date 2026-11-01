// Release builds set NDEBUG (b_ndebug=true); these checks must always run.
#undef NDEBUG
#include "multiplayer/CheckpointWire.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace multiplayer;
template<class F> void Reject(F f) { try { f(); } catch (const std::exception &) { return; } throw std::runtime_error("accepted malformed input"); }
int main()
{
    for (size_t n : {size_t(1), CheckpointChunkBytes, CheckpointChunkBytes+1, CheckpointChunkBytes*5+31})
    {
        std::string initial(n,'A'); for (size_t i=0;i<n;++i) initial[i]=char(i*73);
        CheckpointAssembler r; r.Begin(123,initial.size());
        for (size_t offset=0;offset<n;offset+=CheckpointChunkBytes)
        {
            auto p=EncodeCheckpointChunk(123,initial,offset);
            assert(r.Accept(p)==(offset+CheckpointChunkBytes>=n));
            if (offset==0) Reject([&]{r.Accept(p);});
        }
        assert(r.Take()==initial);
    }
    std::string bytes(CheckpointChunkBytes*2+1,'!');
    auto first=EncodeCheckpointChunk(5,bytes,0);
    CheckpointAssembler r; r.Begin(5,bytes.size());
    auto mutated=first; mutated[0]='?'; Reject([&]{r.Accept(mutated);});
    mutated=first; mutated[4]=2; Reject([&]{r.Accept(mutated);});
    mutated=first; mutated[8]++; Reject([&]{r.Accept(mutated);});
    mutated=first; mutated[20]++; Reject([&]{r.Accept(mutated);});
    mutated=first; mutated.pop_back(); Reject([&]{r.Accept(mutated);});
    Reject([&]{r.Begin(10,MaxTransferBytes+1);});
    Reject([&]{EncodeCheckpointChunk(5,bytes,1);});
    assert(!r.Accept(first));
    Reject([&]{r.Accept(first);});
    r.Reset(); Reject([&]{r.Take();});
    std::cout << "checkpoint wire: bounded strict framing, round trips, and corruption tests passed\n";
}
