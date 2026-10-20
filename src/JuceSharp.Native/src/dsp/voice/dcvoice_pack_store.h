// dcvoice_pack_store.h — where the sample packs live and who holds them.
//
// LOCATION: `voices/` next to the module this code is compiled into
// (juce_sharp.dll in the app, the test executable in a test), unless the
// environment variable DOTCHARTER_VOICE_DIR names a directory. The app build
// copies the packs there (DotCharter.UI.csproj) and the release payload ships
// them (tools/release/New-Release.ps1).
//
// LIFETIME: acquirePack() loads a pack once per process and hands out shared
// owners; when the last voice holding it is destroyed, the memory goes with
// it — the engine destroys a voice (off the audio thread) when the chart source
// is switched off or its instrument model changes (#61 part 2, #50), and
// livePackBytes() reports what is held. CONTROL THREAD ONLY (it reads a file and takes a mutex); the audio
// thread only ever sees the raw SamplePack pointer its voice holds.
//
// LAZY (#417): PackLoad::Lazy reads only the zone table; the engine's voices
// decode the zones a chart plays (dcvoice_pack.h, ensureZones). LazyFresh is a
// NEW lazy instance that replaces the shared one for later acquirers — how a
// voice drops the zones an old chart left decoded: the previous holders keep
// the old instance until they go. Eager and lazy instances are cached apart.
#pragma once

#include "dcvoice_pack.h"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dcvoice {

inline std::filesystem::path voicePackDirectory()
{
#ifdef _WIN32
    wchar_t env[2048];
    const DWORD n = GetEnvironmentVariableW(L"DOTCHARTER_VOICE_DIR", env, 2048);
    if (n > 0 && n < 2048)
        return std::filesystem::path(env);
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&voicePackDirectory), &self))
    {
        wchar_t buf[4096];
        const DWORD len = GetModuleFileNameW(self, buf, 4096);
        if (len > 0 && len < 4096)
            return std::filesystem::path(buf).parent_path() / "voices";
    }
    return std::filesystem::path("voices");
#else
    if (const char* env = std::getenv("DOTCHARTER_VOICE_DIR"))
        return std::filesystem::path(env);
    Dl_info info {};
    if (dladdr(reinterpret_cast<void*>(&voicePackDirectory), &info) != 0 && info.dli_fname != nullptr)
        return std::filesystem::path(info.dli_fname).parent_path() / "voices";
    return std::filesystem::path("voices");
#endif
}

namespace packstore {
struct Cache
{
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<const SamplePack>> packs;
    std::vector<std::weak_ptr<const SamplePack>> all;   // every instance loaded (a LazyFresh one replaces its key in `packs`)
};
inline Cache& cache()
{
    static Cache c;
    return c;
}
} // namespace packstore

/// The path of pack `name` ("guitar", "bass", "strat-bridge", ...).
inline std::filesystem::path voicePackPath(const char* name)
{
    return voicePackDirectory() / (std::string(name) + ".dcvp");
}

/// True when pack `name` is installed (the file exists). Control thread (I/O).
inline bool voicePackInstalled(const char* name)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(voicePackPath(name), ec);
}

/// How acquirePack loads: every zone decoded now (Eager), or the zone table
/// only (Lazy, shared; LazyFresh, a new instance — see LAZY above).
enum class PackLoad
{
    Eager,
    Lazy,
    LazyFresh,
};

/// The pack `name` ("guitar", "bass", "strat-bridge", ...), loaded on first use
/// and shared while anyone holds it: the memory goes with the LAST holder, so a
/// voice that is released (synth off, model changed) frees its pack. nullptr
/// (and `error`) when the file is missing or malformed.
inline std::shared_ptr<const SamplePack> acquirePack(const char* instrument, std::string& error, PackLoad mode = PackLoad::Eager)
{
    auto& c = packstore::cache();
    const std::filesystem::path file = voicePackPath(instrument);
    const std::string key = file.u8string() + (mode == PackLoad::Eager ? "" : "#lazy");
    std::lock_guard<std::mutex> lock(c.mutex);
    if (mode != PackLoad::LazyFresh)
        if (auto live = c.packs[key].lock())
            return live;
    if (mode != PackLoad::Eager)
    {
        auto lazy = std::make_shared<SamplePack>();
        if (! loadPackLazy(file, *lazy, error))
        {
            if (error.empty())
                error = "cannot read " + key;
            return nullptr;
        }
        std::shared_ptr<const SamplePack> shared = std::move(lazy);
        c.packs[key] = shared;
        c.all.push_back(shared);
        return shared;
    }
    auto pack = std::make_shared<SamplePack>();
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    if (! f)
    {
        error = "no sample pack at " + key;
        return nullptr;
    }
    const std::streamoff size = f.tellg();
    if (size <= 0 || size > (std::streamoff) 2000 * 1024 * 1024)
    {
        error = "bad sample pack size at " + key;
        return nullptr;
    }
    {
        std::vector<uint8_t> bytes((size_t) size);
        f.seekg(0);
        if (! f.read((char*) bytes.data(), size) || ! parsePack(bytes.data(), bytes.size(), *pack, error))
        {
            if (error.empty())
                error = "cannot read " + key;
            return nullptr;
        }
    }   // the file image is freed here, before the pack is published
    std::shared_ptr<const SamplePack> shared = std::move(pack);
    c.packs[key] = shared;
    c.all.push_back(shared);
    return shared;
}

/// Bytes held by every pack alive in this process (the chart voice's RAM).
/// Any thread but the audio thread (takes the store's mutex).
inline int64_t livePackBytes()
{
    auto& c = packstore::cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    int64_t total = 0;
    for (auto it = c.all.begin(); it != c.all.end();)
    {
        if (auto live = it->lock())
        {
            total += (int64_t) live->bytes();
            ++it;
        }
        else
            it = c.all.erase(it);
    }
    for (auto it = c.packs.begin(); it != c.packs.end();)
        it = it->second.expired() ? c.packs.erase(it) : std::next(it);
    return total;
}

} // namespace dcvoice
