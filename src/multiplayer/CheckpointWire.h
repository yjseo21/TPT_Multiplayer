#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace multiplayer
{
// The WebSocket binary message transport for an exact snapshot. Every packet
// is an independently framed, strictly ordered piece of one transfer.
// No user-supplied allocation size can exceed MaxTransferBytes.
constexpr size_t CheckpointChunkBytes = 32 * 1024;
constexpr size_t CheckpointWireHeaderBytes = 24;
constexpr size_t MaxTransferBytes = 64 * 1024 * 1024;

std::string EncodeCheckpointChunk(uint64_t transferId, std::string_view checkpoint, size_t offset);
class CheckpointAssembler
{
    uint64_t transferId = 0;
    size_t expectedSize = 0;
    std::string data;
    bool started = false, complete = false;
public:
    void Begin(uint64_t id, size_t bytes);
    // Returns true exactly once, when all expected bytes have arrived.
    bool Accept(std::string_view packet);
    bool Complete() const { return complete; }
    size_t Received() const { return data.size(); }
    std::string Take();
    void Reset();
};
}
