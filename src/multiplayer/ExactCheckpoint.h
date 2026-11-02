#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace multiplayer
{
class World;

// Experimental, same-build ExactCheckpointV1 for gravity-disabled, callback-free
// rooms. Binary and endian-stable, with bounded zlib compression and CRC32.
// Capture only between complete physics steps/edits, on the world owner thread.
// Decode validates the complete payload into a new World before committing.
// This is NOT a portable cross-build deterministic simulation guarantee.
// Raw capture accesses mutable simulation state and MUST remain on the owner
// thread. Compression only consumes immutable bytes and may be offloaded.
// buffer: storage to reuse (its contents are discarded). Reusing one avoids the
// cost of fresh memory for every capture, which dominates on some systems.
std::string CaptureExactCheckpointRaw(World &world, std::string buffer = {});
std::string CompressExactCheckpointRaw(std::string_view raw);
std::string EncodeExactCheckpoint(World &world);
void RestoreExactCheckpoint(World &world, std::string_view packet);
}
