// LOCAL PATCH (GitHub issue #2): CRI ADX / Sofdec streamed audio played on the PC instead of through CRI_ADXI.IRX.
//
// The middleware decodes PCM on the EE and its "RNA" output object sends it to the IOP one channel at a time (a ring
// buffer per channel, moved by DTR transfer engines). Under emulation the IOP side loses track of right-channel
// chunks: the right ear ran one whole ring (8184 of 8192 samples, measured) ahead of the left - the echo / static
// the old "copy left" setting hid - and after a stop the right ring never reported itself empty, so a restarted stream
// (movie, cutscene skip) could stall.
//
// Here PS2RNA_ExecHndl takes the PCM straight out of each channel's source ring buffer (CRI SJRBF) on the EE, the same
// amount from every channel, into one stereo queue per RNA object, and the sound callback mixes those queues into the
// output (resampled, with the object's volume). GetNumData / GetNumRoom report that queue, so ADX's clock (samples
// decoded minus samples still queued) stays exact. The DTRs are never started, so the IOP module just idles.
// Credit: the analysis in GitHub issue #2 (LgSetzeR). PS2X_CRI_RNA_HLE=1 turns it on (off by default for now).
//
// RNA object (from this ELF): +0x10 + ch*4 DTR, +0x24 play switch (s8), +0x26 channel count (s8), +0x28 sample rate,
// +0x30 volume (0.1 dB, 0..-999), +0x48 transfer switch (s8), +0x4B discard switch. DTR +0x4: source SJ.
// SJRBF: +0x0 vtable, +0x0C data bytes (lane 1), +0x10 free bytes (lane 0), +0x18 lane-1 read position, +0x1C buffer,
// +0x20 buffer size.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace
{
    constexpr uint32_t kExecHndl = 0x27B008u;
    constexpr uint32_t kGetNumData = 0x27AF90u;
    constexpr uint32_t kGetNumRoom = 0x27AFD0u;
    constexpr uint32_t kSetDiscardSw = 0x27AF38u;
    constexpr uint32_t kClearBuf = 0x27AF40u;
    constexpr uint32_t kAdxGetNumData = 0x26A808u;
    constexpr uint32_t kAdxGetNumRoom = 0x26A820u;
    constexpr uint32_t kSjrbfGetChunk = 0x27BCD0u; // SJRBF vtable +0x18: the source is a plain ring buffer
    constexpr uint32_t kCapacity = 8192u;          // frames, as CRI's IOP ring (0x4000 bytes of 16-bit samples)

    struct Stream
    {
        std::vector<int16_t> ring = std::vector<int16_t>(kCapacity * 2u); // stereo frames
        uint32_t read = 0u, count = 0u;
        double phase = 0.0;  // fraction between the frame at `read` and the next one
        uint32_t rate = 48000u;
        float gain = 1.0f;
        bool playing = false;
        std::chrono::steady_clock::time_point lastConsumed = std::chrono::steady_clock::now();
    };

    std::mutex g_mutex;
    std::map<uint32_t, Stream> g_streams; // by RNA object address
    PS2Runtime::RecompiledFunction g_origExecHndl = nullptr;
    PS2Runtime::RecompiledFunction g_origSetDiscardSw = nullptr;
    PS2Runtime::RecompiledFunction g_origClearBuf = nullptr;
    std::atomic<bool> g_active{false};

    bool inRam(uint32_t address, uint32_t bytes)
    {
        const uint32_t physical = address & 0x1FFFFFFFu;
        return bytes <= PS2_RAM_SIZE && physical <= PS2_RAM_SIZE - bytes;
    }
    uint32_t rd32(const uint8_t *rdram, uint32_t a)
    {
        if (!inRam(a, 4u))
            return 0u;
        uint32_t v;
        std::memcpy(&v, rdram + (a & 0x1FFFFFFFu), 4u);
        return v;
    }
    int8_t rd8s(const uint8_t *rdram, uint32_t a)
    {
        return inRam(a, 1u) ? static_cast<int8_t>(rdram[a & 0x1FFFFFFFu]) : 0;
    }
    void wr32(uint8_t *rdram, uint32_t a, uint32_t v)
    {
        if (inRam(a, 4u))
            std::memcpy(rdram + (a & 0x1FFFFFFFu), &v, 4u);
    }

    void returnTo(R5900Context *ctx)
    {
        ctx->pc = GPR_U32(ctx, 31);
    }

    // Without a sound device nothing would ever play the queue and ADX's clock would stop: drain it on the wall clock.
    void drainIfIdle(Stream &s)
    {
        const auto now = std::chrono::steady_clock::now();
        const double idle = std::chrono::duration<double>(now - s.lastConsumed).count();
        if (idle < 0.25 || !s.playing)
            return;
        const uint32_t drop = std::min<uint32_t>(s.count, static_cast<uint32_t>(idle * s.rate));
        s.read = (s.read + drop) % kCapacity;
        s.count -= drop;
        s.lastConsumed = now;
    }

    uint32_t queued(uint32_t rna);

    void execHndl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t rna = GPR_U32(ctx, 4);
        const int nch = rd8s(rdram, rna + 0x26u);
        // Anything this does not understand goes to the game's own code.
        auto original = [&]() {
            if (g_origExecHndl)
                g_origExecHndl(rdram, ctx, runtime);
            else
                returnTo(ctx);
        };
        static const bool s_log = std::getenv("PS2X_CRI_RNA_LOG") != nullptr;
        if (s_log)
        {
            static std::map<uint32_t, uint32_t> s_calls;
            const uint32_t n = ++s_calls[rna];
            if (n <= 3u || (n % 600u) == 0u)
                std::fprintf(stderr, "[cri-rna] exec rna 0x%08x call %u: nch %d play %d trans %d rate %u vol %d queued %u\n", rna, n,
                             nch, rd8s(rdram, rna + 0x24u), rd8s(rdram, rna + 0x48u), rd32(rdram, rna + 0x28u),
                             static_cast<int32_t>(rd32(rdram, rna + 0x30u)), queued(rna));
        }
        if (nch < 1 || nch > 2)
            return original();
        uint32_t src[2] = {};
        for (int ch = 0; ch < nch; ++ch)
        {
            const uint32_t dtr = rd32(rdram, rna + 0x10u + 4u * static_cast<uint32_t>(ch));
            src[ch] = dtr ? rd32(rdram, dtr + 4u) : 0u;
            const uint32_t vtbl = src[ch] ? rd32(rdram, src[ch]) : 0u;
            if (!vtbl || rd32(rdram, vtbl + 0x18u) != kSjrbfGetChunk)
            {
                static bool s_logged = false;
                if (!s_logged)
                {
                    s_logged = true;
                    std::fprintf(stderr, "[cri-rna] rna 0x%08x channel %d: source 0x%08x is not a ring buffer (vtable 0x%08x, "
                                         "GetChunk 0x%08x): left to the game\n", rna, ch, src[ch], vtbl, vtbl ? rd32(rdram, vtbl + 0x18u) : 0u);
                }
                return original();
            }
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        Stream &s = g_streams[rna];
        s.rate = std::max<uint32_t>(1000u, std::min<uint32_t>(96000u, rd32(rdram, rna + 0x28u)));
        const int32_t volume = static_cast<int32_t>(rd32(rdram, rna + 0x30u)); // 0.1 dB
        s.gain = static_cast<float>(std::pow(10.0, std::max(-999, std::min(0, volume)) / 200.0));
        s.playing = rd8s(rdram, rna + 0x24u) != 0;
        drainIfIdle(s);
        if (rd8s(rdram, rna + 0x48u) == 1)
        {
            // The same number of samples from every channel, as much as both have and the queue can take.
            uint32_t frames = kCapacity - s.count;
            for (int ch = 0; ch < nch; ++ch)
                frames = std::min(frames, rd32(rdram, src[ch] + 0x0Cu) / 2u);
            if (frames > 0u)
            {
                for (int ch = 0; ch < nch; ++ch)
                {
                    const uint32_t sj = src[ch];
                    const uint32_t buf = rd32(rdram, sj + 0x1Cu), size = rd32(rdram, sj + 0x20u);
                    uint32_t pos = rd32(rdram, sj + 0x18u);
                    if (!size || !inRam(buf, size))
                        continue;
                    uint32_t w = (s.read + s.count) % kCapacity;
                    for (uint32_t i = 0; i < frames; ++i)
                    {
                        int16_t v;
                        std::memcpy(&v, rdram + ((buf + pos) & 0x1FFFFFFFu), 2u);
                        pos += 2u;
                        if (pos >= size)
                            pos -= size;
                        if (nch == 1)
                        {
                            s.ring[w * 2u] = v;
                            s.ring[w * 2u + 1u] = v;
                        }
                        else
                            s.ring[w * 2u + static_cast<uint32_t>(ch)] = v;
                        w = (w + 1u) % kCapacity;
                    }
                    // GetChunk on the data lane, then PutChunk back to the free lane.
                    wr32(rdram, sj + 0x18u, pos);
                    wr32(rdram, sj + 0x0Cu, rd32(rdram, sj + 0x0Cu) - frames * 2u);
                    wr32(rdram, sj + 0x10u, rd32(rdram, sj + 0x10u) + frames * 2u);
                }
                s.count += frames;
            }
            if (inRam(rna + 0x4Bu, 1u))
                rdram[(rna + 0x4Bu) & 0x1FFFFFFFu] = 0; // the discard request is done (the original clears it on start)
        }
        returnTo(ctx);
    }

    uint32_t queued(uint32_t rna)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_streams.find(rna);
        if (it == g_streams.end())
            return 0u;
        drainIfIdle(it->second);
        return it->second.count;
    }

    void getNumData(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_U32(ctx, 2, queued(GPR_U32(ctx, 4)));
        returnTo(ctx);
    }

    void getNumRoom(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_U32(ctx, 2, kCapacity - queued(GPR_U32(ctx, 4)));
        returnTo(ctx);
    }

    void clearQueue(uint32_t rna)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_streams.find(rna);
        if (it != g_streams.end())
        {
            it->second.read = it->second.count = 0u;
            it->second.phase = 0.0;
        }
    }

    void setDiscardSw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if ((GPR_U32(ctx, 5) & 0xFFu) != 0u)
            clearQueue(GPR_U32(ctx, 4));
        if (g_origSetDiscardSw)
            g_origSetDiscardSw(rdram, ctx, runtime);
        else
            returnTo(ctx);
    }

    void clearBuf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        clearQueue(GPR_U32(ctx, 4));
        if (g_origClearBuf)
            g_origClearBuf(rdram, ctx, runtime);
        else
            returnTo(ctx);
    }
}

// At start-up, after the game's functions are registered (ps2_runtime.cpp loadELF).
void ps2xCriRnaInstall(PS2Runtime &runtime)
{
    // On by default (v0.5.4; GitHub issue #2, cause and method confirmed by LgSetzeR's write-up). PS2X_CRI_RNA_HLE=0
    // goes back to the sound chip path (with audio.stereo_repair's left-channel copy).
    const char *on = std::getenv("PS2X_CRI_RNA_HLE");
    if (on && on[0] == '0')
        return;
    if (!runtime.hasFunction(kExecHndl) || !runtime.hasFunction(kGetNumData) || !runtime.hasFunction(kGetNumRoom))
        return;
    g_origExecHndl = runtime.lookupFunction(kExecHndl);
    g_origSetDiscardSw = runtime.hasFunction(kSetDiscardSw) ? runtime.lookupFunction(kSetDiscardSw) : nullptr;
    g_origClearBuf = runtime.hasFunction(kClearBuf) ? runtime.lookupFunction(kClearBuf) : nullptr;
    runtime.replaceFunction(kExecHndl, execHndl);
    runtime.replaceFunction(kGetNumData, getNumData);
    runtime.replaceFunction(kGetNumRoom, getNumRoom);
    // The ADXRNA_ wrappers tail-call these with a direct C++ call that skips the table: replaced too.
    runtime.replaceFunction(kAdxGetNumData, getNumData);
    runtime.replaceFunction(kAdxGetNumRoom, getNumRoom);
    if (g_origSetDiscardSw)
        runtime.replaceFunction(kSetDiscardSw, setDiscardSw);
    if (g_origClearBuf)
        runtime.replaceFunction(kClearBuf, clearBuf);
    g_active.store(true);
    std::fprintf(stderr, "[cri-rna] streamed audio played on the PC (CRI RNA output replaced)\n");
}

bool ps2xJukeboxMix(uint32_t rna, int16_t *out, size_t frames, float gain); // ps2_jukebox.cpp (LOCAL PATCH)

// The sound callback (48 kHz stereo s16): adds every playing stream to `out`.
void ps2xCriRnaMix(int16_t *out, size_t frames)
{
    if (!g_active.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &entry : g_streams)
    {
        Stream &s = entry.second;
        // LOCAL PATCH (Jukebox): a track the player replaced plays their file here; the game's own sound is still
        // taken from the queue below (so ADX's clock runs on) but not added.
        const bool replaced = s.playing && ps2xJukeboxMix(entry.first, out, frames, s.gain);
        if (!s.playing || s.count < 2u)
            continue;
        const double step = static_cast<double>(s.rate) / 48000.0;
        for (size_t i = 0; i < frames && s.count >= 2u; ++i)
        {
            const uint32_t a = s.read, b = (s.read + 1u) % kCapacity;
            const float t = static_cast<float>(s.phase);
            for (int c = 0; c < 2 && !replaced; ++c)
            {
                const float v = (static_cast<float>(s.ring[a * 2u + c]) * (1.0f - t) + static_cast<float>(s.ring[b * 2u + c]) * t) * s.gain;
                const int mixed = static_cast<int>(out[i * 2u + c]) + static_cast<int>(std::lround(v));
                out[i * 2u + c] = static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
            }
            s.phase += step;
            const uint32_t whole = static_cast<uint32_t>(s.phase);
            if (whole)
            {
                const uint32_t n = std::min(whole, s.count);
                s.read = (s.read + n) % kCapacity;
                s.count -= n;
                s.phase -= whole;
            }
        }
        s.lastConsumed = std::chrono::steady_clock::now();
    }
}

// LOCAL PATCH (save states, GitHub #16): the queued PCM of every RNA object (the decoder's own position is in guest
// memory, so the queue must match it).
#include "runtime/ps2_savestate.h"
void ps2xCriRnaSaveState(ps2x::savestate::Writer &out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    out.section("cri-rna");
    out.u32(static_cast<uint32_t>(g_streams.size()));
    for (const auto &[rna, s] : g_streams)
    {
        out.u32(rna);
        out.podVector(s.ring);
        out.u32(s.read);
        out.u32(s.count);
        out.pod(s.phase);
        out.u32(s.rate);
        out.pod(s.gain);
        out.pod(s.playing);
    }
}

bool ps2xCriRnaLoadState(ps2x::savestate::Reader &in)
{
    if (!in.section("cri-rna"))
        return false;
    std::map<uint32_t, Stream> streams;
    const uint32_t n = in.u32();
    for (uint32_t i = 0; i < n && !in.failed(); ++i)
    {
        const uint32_t rna = in.u32();
        Stream s;
        in.podVector(s.ring);
        s.read = in.u32();
        s.count = in.u32();
        in.pod(s.phase);
        s.rate = in.u32();
        in.pod(s.gain);
        in.pod(s.playing);
        if (s.ring.size() != kCapacity * 2u)
            s.ring.assign(kCapacity * 2u, 0);
        s.lastConsumed = std::chrono::steady_clock::now();
        streams[rna] = std::move(s);
    }
    if (in.failed())
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_streams = std::move(streams);
    return true;
}