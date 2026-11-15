// LOCAL PATCH: the Jukebox. Any of the game's music tracks can be replaced by a file of the player's own.
//
// The game plays its music with CRI ADX: ADXT_StartFname(player, "BGM03_00_00T.ADX") starts a named track on an ADX
// player, whose RNA output object (+0xC) is a stream ps2_cri_rna.cpp plays on the PC. A hook at the top of
// ADXT_StartFname (scripts/runner_patches.py, section 12) calls ps2xJukeboxStart; if jukebox/<TRACK>.mp3 (or .ogg /
// .wav) exists next to the game folder, that stream's sound is replaced by the file, decoded on a worker thread (the
// original plays until it is ready), looped, and played with the stream's own volume, so the game's fades and its
// pause / stop still apply. The game keeps decoding its own track silently, so its timing is untouched.
//
// The launcher's Jukebox tab copies the chosen files into jukebox/ (updates never touch that folder); a file can also
// be put there by hand. A track is looked up each time it starts, so a change applies the next time it plays.

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr float kLevel = 0.8f; // players' files are usually mastered louder than the game's music

    struct Track
    {
        std::vector<int16_t> samples; // 48 kHz stereo
    };

    struct Playing
    {
        std::string key;                  // e.g. BGM03_00_00T
        std::shared_ptr<const Track> track; // null until decoded
        size_t frame = 0;                   // position, in stereo frames
        uint64_t serial = 0;
    };

    std::mutex g_mutex;
    std::map<uint32_t, Playing> g_playing;                        // by RNA object address
    std::map<std::string, std::shared_ptr<const Track>> g_cache; // by file path (the last few)
    std::vector<std::string> g_cacheOrder;
    uint64_t g_serial = 0;

    uint32_t rd32(const uint8_t *rdram, uint32_t a)
    {
        const uint32_t p = a & 0x1FFFFFFFu;
        if (p > 0x2000000u - 4u)
            return 0u;
        uint32_t v;
        std::memcpy(&v, rdram + p, 4u);
        return v;
    }

    // "cdrom0:\BGM03_00_00T.ADX;1" / "BGM03_00_00T.ADX" -> "BGM03_00_00T"
    std::string trackKey(const uint8_t *rdram, uint32_t address)
    {
        std::string name;
        for (uint32_t i = 0; i < 128u; ++i)
        {
            const uint32_t p = (address + i) & 0x1FFFFFFFu;
            if (p >= 0x2000000u)
                break;
            const char c = static_cast<char>(rdram[p]);
            if (c == '\0')
                break;
            name.push_back(c);
        }
        const size_t slash = name.find_last_of("\\/:");
        if (slash != std::string::npos)
            name = name.substr(slash + 1);
        const size_t semi = name.find(';');
        if (semi != std::string::npos)
            name.resize(semi);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name.resize(dot);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return name;
    }

    // The player's file for a track (the game runs in game/, the jukebox folder is beside it), or empty.
    fs::path replacementFor(const std::string &key)
    {
        if (key.empty())
            return {};
        std::error_code ec;
        for (const char *ext : {".mp3", ".ogg", ".wav"})
        {
            const fs::path path = fs::path("..") / "jukebox" / (key + ext);
            if (fs::exists(path, ec))
                return path;
        }
        return {};
    }

    std::shared_ptr<const Track> decode(const fs::path &path)
    {
        const std::u8string u8 = path.u8string();
        const std::string name(reinterpret_cast<const char *>(u8.data()), u8.size());
        Wave wave = LoadWave(name.c_str());
        if (!wave.data || wave.frameCount == 0)
        {
            std::fprintf(stderr, "[jukebox] %s could not be read (mp3, ogg and wav are supported)\n", name.c_str());
            UnloadWave(wave);
            return nullptr;
        }
        WaveFormat(&wave, 48000, 16, 2);
        auto track = std::make_shared<Track>();
        const int16_t *data = static_cast<const int16_t *>(wave.data);
        track->samples.assign(data, data + static_cast<size_t>(wave.frameCount) * 2u);
        UnloadWave(wave);
        std::fprintf(stderr, "[jukebox] %s ready (%.1f s)\n", name.c_str(),
                     static_cast<double>(track->samples.size() / 2u) / 48000.0);
        return track;
    }
}

// From the hook at the top of ADXT_StartFname / ADXT_StartAfs / ADXT_StartSj (fname 0: not a named track).
void ps2xJukeboxStart(uint8_t *rdram, uint32_t adxt, uint32_t fname)
{
    if (!rdram || !adxt)
        return;
    const uint32_t rna = rd32(rdram, adxt + 0xCu);
    if (!rna)
        return;
    const std::string key = fname ? trackKey(rdram, fname) : std::string();
    const fs::path path = replacementFor(key);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (path.empty())
    {
        g_playing.erase(rna);
        if (!key.empty())
            std::fprintf(stderr, "[jukebox] %s started (the game's own)\n", key.c_str());
        return;
    }
    Playing &p = g_playing[rna];
    p.key = key;
    p.frame = 0;
    p.serial = ++g_serial;
    // A file chosen again under the same name (the launcher copies over it) is decoded again: its time is in the key.
    std::error_code timeEc;
    const auto written = fs::last_write_time(path, timeEc);
    const std::string cacheKey = path.generic_string() + "|" + std::to_string(timeEc ? 0 : written.time_since_epoch().count());
    const auto cached = g_cache.find(cacheKey);
    if (cached != g_cache.end())
    {
        p.track = cached->second;
        std::fprintf(stderr, "[jukebox] %s started: playing %s\n", key.c_str(), path.generic_string().c_str());
        return;
    }
    p.track = nullptr; // the original plays until the file is decoded
    std::fprintf(stderr, "[jukebox] %s started: loading %s\n", key.c_str(), path.generic_string().c_str());
    const uint64_t serial = p.serial;
    std::thread([rna, serial, path, cacheKey]() {
        std::shared_ptr<const Track> track = decode(path);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (track)
        {
            g_cache[cacheKey] = track;
            g_cacheOrder.erase(std::remove(g_cacheOrder.begin(), g_cacheOrder.end(), cacheKey), g_cacheOrder.end());
            g_cacheOrder.push_back(cacheKey);
            while (g_cacheOrder.size() > 4u) // a few tracks kept (a stage's music, its boss, a retry), not all of them
            {
                g_cache.erase(g_cacheOrder.front());
                g_cacheOrder.erase(g_cacheOrder.begin());
            }
        }
        const auto it = g_playing.find(rna);
        if (it == g_playing.end() || it->second.serial != serial)
            return; // another track started meanwhile
        if (track)
            it->second.track = track;
        else
            g_playing.erase(it);
    }).detach();
}

// From the sound callback (ps2_cri_rna.cpp): adds the replacement for this stream to `out` (48 kHz stereo) with the
// stream's volume. True when the stream's own sound is to be left out.
bool ps2xJukeboxMix(uint32_t rna, int16_t *out, size_t frames, float gain)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_playing.empty())
        return false;
    const auto it = g_playing.find(rna);
    if (it == g_playing.end() || !it->second.track || it->second.track->samples.size() < 2u)
        return false;
    Playing &p = it->second;
    const std::vector<int16_t> &s = p.track->samples;
    const size_t total = s.size() / 2u;
    const float g = gain * kLevel;
    for (size_t i = 0; i < frames; ++i)
    {
        if (p.frame >= total)
            p.frame = 0; // looped, as the game's music is
        for (int c = 0; c < 2; ++c)
        {
            const int mixed = static_cast<int>(out[i * 2u + c]) + static_cast<int>(static_cast<float>(s[p.frame * 2u + c]) * g);
            out[i * 2u + c] = static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
        }
        ++p.frame;
    }
    return true;
}
