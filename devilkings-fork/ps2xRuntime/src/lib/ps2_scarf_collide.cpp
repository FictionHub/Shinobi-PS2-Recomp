// LOCAL PATCH (mod): the scarf no longer passes through Hotsuma's body.
//
// CPlayerMuffler::Main (0x2b8a50) moves the scarf: a few strands (+0x10), each a chain of points (+0xC per strand),
// positions at +0x4 and velocities at +0x8 (16 bytes a point), pinned to bones at the neck, with gravity, a fixed
// length between points and damping - that is where its float and trail come from. It never checks the body, so on a
// turn the points pass straight through it. After the game's own update (hook in src/runner, scripts/runner_patches.py)
// the body is taken as a capsule from the neck (the strands' pinned first points) down to the feet (the player's
// position, +0xB8), sized from that height so the game's units do not matter; a point inside is moved out to its
// surface and the part of its velocity going into the body is removed. The first points of each strand sit at the neck
// and are left alone, the push fades in over the next ones. [game] scarf_collision (Effects tab).
// PS2X_SCARF_RADIUS=<fraction of the height> tunes the capsule (0.075 by default). Torso only (neck to hips).

#include "runtime/ps2_settings.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
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
    bool rdVec(const uint8_t *rdram, uint32_t a, float out[3])
    {
        if (!inRam(a, 12u))
            return false;
        std::memcpy(out, rdram + (a & 0x1FFFFFFFu), 12u);
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    }
    void wrVec(uint8_t *rdram, uint32_t a, const float v[3])
    {
        if (inRam(a, 12u))
            std::memcpy(rdram + (a & 0x1FFFFFFFu), v, 12u);
    }

    bool enabled()
    {
        static std::atomic<uint64_t> s_version{~0ull};
        static std::atomic<bool> s_on{true};
        const uint64_t version = ps2x::settings::version();
        if (s_version.load(std::memory_order_relaxed) != version)
        {
            s_on.store(ps2x::settings::getBool("game", "scarf_collision"), std::memory_order_relaxed);
            s_version.store(version, std::memory_order_relaxed);
        }
        return s_on.load(std::memory_order_relaxed);
    }
}

// Called at the end of CPlayerMuffler::Main with the scarf object, on the game thread.
void ps2xScarfCollide(uint8_t *rdram, uint32_t muffler)
{
    if (!muffler || !enabled())
        return;
    const uint32_t count = rd32(rdram, muffler + 0x0Cu);
    const uint32_t strands = rd32(rdram, muffler + 0x10u);
    const uint32_t pos = rd32(rdram, muffler + 0x04u);
    const uint32_t vel = rd32(rdram, muffler + 0x08u);
    const uint32_t where = rd32(rdram, muffler + 0xB8u);
    if (strands < 1u || strands > 8u || count < 3u || count > 64u || !pos || !vel || !where ||
        !inRam(pos, strands * count * 16u) || !inRam(vel, strands * count * 16u))
        return;
    float feet[3];
    if (!rdVec(rdram, where, feet))
        return;
    // The neck: the strands' pinned first points, averaged.
    float neck[3] = {0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < strands; ++i)
    {
        float r[3];
        if (!rdVec(rdram, pos + i * count * 16u, r))
            return;
        for (int k = 0; k < 3; ++k)
            neck[k] += r[k] / static_cast<float>(strands);
    }
    const float axis[3] = {feet[0] - neck[0], feet[1] - neck[1], feet[2] - neck[2]};
    const float height2 = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
    if (!(height2 > 1e-6f) || !std::isfinite(height2))
        return;
    const float height = std::sqrt(height2);
    static const float s_fraction = [] {
        const char *e = std::getenv("PS2X_SCARF_RADIUS");
        const float f = e ? static_cast<float>(std::atof(e)) : 0.075f;
        return f > 0.0f && f < 0.5f ? f : 0.075f;
    }();
    const float radius = height * s_fraction;

    for (uint32_t i = 0; i < strands; ++i)
        for (uint32_t j = 2; j < count; ++j)
        {
            const uint32_t pa = pos + (i * count + j) * 16u, va = vel + (i * count + j) * 16u;
            float p[3], v[3];
            if (!rdVec(rdram, pa, p) || !rdVec(rdram, va, v))
                continue;
            // Closest point of the body's axis (below the neck itself, so the attachment is never pushed).
            const float rel[3] = {p[0] - neck[0], p[1] - neck[1], p[2] - neck[2]};
            float t = (rel[0] * axis[0] + rel[1] * axis[1] + rel[2] * axis[2]) / height2;
            // Only the torso: from just under the neck down to the hips (about 45% of the way to the feet). Below the hips
            // nothing is pushed, so the scarf hangs and trails freely as in the original game (a capsule down to the feet
            // made the whole scarf a wide skirt around the legs).
            if (t > 0.48f)
                continue;
            t = std::clamp(t, 0.06f, 0.45f);
            const float c[3] = {neck[0] + axis[0] * t, neck[1] + axis[1] * t, neck[2] + axis[2] * t};
            float d[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]};
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const float r = radius * std::min(1.0f, static_cast<float>(j - 1u) / 2.0f); // fade in near the neck
            if (len >= r)
                continue;
            if (len < 1e-5f)
            {
                // Exactly on the axis: out the way the point before it hangs.
                float prev[3];
                if (!rdVec(rdram, pa - 16u, prev))
                    continue;
                d[0] = prev[0] - c[0];
                d[1] = 0.0f;
                d[2] = prev[2] - c[2];
                const float l2 = std::sqrt(d[0] * d[0] + d[2] * d[2]);
                if (l2 < 1e-5f)
                    continue;
                d[0] /= l2;
                d[2] /= l2;
            }
            else
                for (float &x : d)
                    x /= len;
            const float out[3] = {c[0] + d[0] * r, c[1] + d[1] * r, c[2] + d[2] * r};
            wrVec(rdram, pa, out);
            const float into = v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
            if (into < 0.0f)
            {
                const float nv[3] = {v[0] - d[0] * into, v[1] - d[1] * into, v[2] - d[2] * into};
                wrVec(rdram, va, nv);
            }
        }
}
