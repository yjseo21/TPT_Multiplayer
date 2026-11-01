#pragma once
#include "client/GameSave.h"
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace multiplayer
{
class World;
// Temp-file flush + same-directory atomic replacement. POSIX also fsyncs the directory.
void AtomicSaveFile(const std::filesystem::path &path, const std::vector<char> &bytes);

class AutosaveStore
{
    std::filesystem::path directory;
    int keep;
    intptr_t lockHandle = -1;
    uint64_t generation = 0;
    std::mutex mutex;
    std::condition_variable changed;
    std::unique_ptr<GameSave> pending;
    bool busy = false, stopping = false;
    std::string error;
    uint64_t committed = 0, pendingRevision = 0, committedRevision = 0;
    std::thread worker;
    std::vector<std::filesystem::path> Candidates() const;
public:
    AutosaveStore(const std::string &directory, int keep);
    ~AutosaveStore();
    bool Recover(World &world);
    // revision labels the world state for diagnostics and tests.
    void Submit(std::unique_ptr<GameSave> save, uint64_t revision = 0);
    void Wait();
    struct Status { uint64_t committed; bool busy; std::string error; uint64_t revision = 0; };
    Status GetStatus();
};
}
