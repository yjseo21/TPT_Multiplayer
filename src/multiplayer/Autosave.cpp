#include "Autosave.h"
#include "World.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace multiplayer
{
namespace fs = std::filesystem;
namespace
{
std::string Filename(uint64_t generation)
{
    std::ostringstream out; out << "autosave-" << std::setw(20) << std::setfill('0') << generation << ".cps";
    return out.str();
}
void SyncDirectory(const fs::path &directory)
{
#ifndef _WIN32
    int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) throw std::runtime_error("cannot open save directory for sync");
    int result = fsync(fd); close(fd);
    if (result) throw std::runtime_error("cannot sync save directory");
#endif
}
}
void AtomicSaveFile(const fs::path &path, const std::vector<char> &bytes)
{
    auto temporary = path; temporary += ".tmp";
    struct Cleanup
    {
        fs::path path;
        ~Cleanup() { std::error_code ignored; fs::remove(path, ignored); }
    } cleanup{temporary};
#ifdef _WIN32
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create temporary save");
    DWORD written = 0;
    bool ok = bytes.size() <= MAXDWORD && WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr)
        && written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot commit save file");
#else
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) throw std::runtime_error("cannot create temporary save");
    size_t offset = 0;
    while (offset < bytes.size())
    {
        auto written = write(fd, bytes.data()+offset, bytes.size()-offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { close(fd); throw std::runtime_error("cannot write save file"); }
        offset += size_t(written);
    }
    int result = fsync(fd); int closed = close(fd);
    if (result || closed) throw std::runtime_error("cannot flush save file");
    fs::rename(temporary, path);
    SyncDirectory(path.has_parent_path() ? path.parent_path() : fs::path("."));
#endif
}
std::vector<fs::path> AutosaveStore::Candidates() const
{
    std::vector<fs::path> files;
    for (auto &entry : fs::directory_iterator(directory))
    {
        auto name = entry.path().filename().string();
        if (!entry.is_regular_file() || name.size() != 33 || name.substr(0,9) != "autosave-" || name.substr(29) != ".cps") continue;
        if (!std::all_of(name.begin()+9, name.begin()+29, [](char c) { return c >= '0' && c <= '9'; })) continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end(), std::greater<fs::path>());
    return files;
}
AutosaveStore::AutosaveStore(const std::string &path, int count) : directory(fs::absolute(path)), keep(count)
{
    if (keep < 1 || keep > 100) throw std::runtime_error("invalid autosave retention");
    fs::create_directories(directory);
    auto lockPath = directory / ".lock";
#ifdef _WIN32
    auto handle = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("autosave directory is locked or inaccessible");
    lockHandle = reinterpret_cast<intptr_t>(handle);
#else
    int handle = open(lockPath.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (handle < 0) throw std::runtime_error("cannot open autosave directory lock");
    if (flock(handle, LOCK_EX | LOCK_NB)) { close(handle); throw std::runtime_error("autosave directory is already in use"); }
    lockHandle = handle;
#endif
    try
    {
        auto files = Candidates();
        if (!files.empty()) generation = std::stoull(files.front().filename().string().substr(9,20));
        worker = std::thread([this] {
            for (;;)
            {
                std::unique_ptr<GameSave> save;
                uint64_t revision = 0;
                {
                    std::unique_lock guard(mutex);
                    changed.wait(guard, [&] { return stopping || pending; });
                    if (!pending && stopping) break;
                    save = std::move(pending); revision = pendingRevision; busy = true;
                }
                try
                {
                    if (generation == UINT64_MAX) throw std::runtime_error("autosave generation exhausted");
                    auto [missing, bytes] = save->Serialise();
                    if (missing) throw std::runtime_error("cannot serialize autosave");
                    GameSave validation(bytes, false);
                    auto next = ++generation;
                    AtomicSaveFile(directory / Filename(next), bytes);
                    auto files = Candidates();
                    for (size_t i = size_t(keep); i < files.size(); ++i) fs::remove(files[i]);
                    SyncDirectory(directory);
                    { std::lock_guard guard(mutex); committed = next; committedRevision = revision; error.clear(); }
                }
                catch (const std::exception &e) { std::lock_guard guard(mutex); error = e.what(); }
                { std::lock_guard guard(mutex); busy = false; }
                changed.notify_all();
            }
        });
    }
    catch (...)
    {
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(lockHandle));
#else
        close(int(lockHandle));
#endif
        throw;
    }
}
AutosaveStore::~AutosaveStore()
{
    { std::lock_guard guard(mutex); stopping = true; }
    changed.notify_all();
    if (worker.joinable()) worker.join();
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(lockHandle));
#else
    close(int(lockHandle));
#endif
}
bool AutosaveStore::Recover(World &world)
{
    auto files = Candidates();
    for (auto &path : files)
    {
        try { world.Load(path.string()); std::cout << "Recovered " << path.filename().string() << std::endl; return true; }
        catch (const std::exception &e) { std::cerr << "Skipping invalid autosave " << path.filename().string() << ": " << e.what() << '\n'; }
    }
    if (!files.empty()) throw std::runtime_error("no valid autosave could be recovered; existing files preserved");
    std::cout << "No autosave yet; starting a new world" << std::endl;
    return false;
}
void AutosaveStore::Submit(std::unique_ptr<GameSave> save, uint64_t revision)
{
    { std::lock_guard guard(mutex); pending = std::move(save); pendingRevision = revision; }
    changed.notify_all();
}
void AutosaveStore::Wait()
{
    std::unique_lock guard(mutex);
    changed.wait(guard, [&] { return !busy && !pending; });
    if (!error.empty()) throw std::runtime_error(error);
}
AutosaveStore::Status AutosaveStore::GetStatus()
{
    std::lock_guard guard(mutex); return {committed, busy || bool(pending), error, committedRevision};
}
}
