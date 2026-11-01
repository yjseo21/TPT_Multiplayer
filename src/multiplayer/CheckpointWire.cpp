#include "CheckpointWire.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace multiplayer
{
namespace
{
void Put(std::string &s, uint64_t n, unsigned size)
{
    while (size--) { s.push_back(char(n)); n >>= 8; }
}
uint64_t Get(std::string_view s, size_t at, unsigned count)
{
    uint64_t n = 0;
    for (unsigned i=0;i<count;++i) n |= uint64_t(uint8_t(s[at+i])) << (8*i);
    return n;
}
}
std::string EncodeCheckpointChunk(uint64_t id, std::string_view checkpoint, size_t offset)
{
    if (!id || checkpoint.empty() || checkpoint.size() > MaxTransferBytes ||
        offset >= checkpoint.size() || offset % CheckpointChunkBytes)
        throw std::runtime_error("invalid exact checkpoint transfer parameters");
    size_t count = std::min(CheckpointChunkBytes, checkpoint.size()-offset);
    std::string packet = "TPTC";
    Put(packet, 1, 1); Put(packet, 0, 1); Put(packet, 0, 2);
    Put(packet, id, 8); Put(packet, checkpoint.size(), 4); Put(packet, offset, 4);
    packet.append(checkpoint.data()+offset,count);
    return packet;
}
void CheckpointAssembler::Begin(uint64_t id, size_t bytes)
{
    if (!id || bytes == 0 || bytes > MaxTransferBytes)
        throw std::runtime_error("invalid checkpoint declaration");
    Reset();
    transferId = id; expectedSize = bytes; started = true;
}
bool CheckpointAssembler::Accept(std::string_view p)
{
    if (!started || complete) throw std::runtime_error("unexpected checkpoint packet");
    if (p.size() <= CheckpointWireHeaderBytes || p.size() > CheckpointWireHeaderBytes+CheckpointChunkBytes ||
        p.substr(0,4) != "TPTC" || uint8_t(p[4]) != 1 || p[5] || p[6] || p[7] ||
        Get(p,8,8) != transferId || Get(p,16,4) != expectedSize || Get(p,20,4) != data.size())
        throw std::runtime_error("checkpoint packet header or ordering invalid");
    size_t count = p.size()-CheckpointWireHeaderBytes;
    if (count > expectedSize-data.size() ||
        (data.size()+count < expectedSize && count != CheckpointChunkBytes))
        throw std::runtime_error("invalid checkpoint packet length");
    // Bound memory growth and avoid allocating untrusted declared size upfront.
    data.append(p.data()+CheckpointWireHeaderBytes,count);
    complete = data.size() == expectedSize;
    return complete;
}
std::string CheckpointAssembler::Take()
{
    if (!complete) throw std::runtime_error("checkpoint not complete");
    auto result = std::move(data);
    Reset();
    return result;
}
void CheckpointAssembler::Reset()
{
    transferId = 0; expectedSize = 0; data.clear(); started = false; complete = false;
}
}
