// LOCAL PATCH (research + sword colour): the player's model textures. CObjectBase+0x2A0 is the MlMultiModelData;
// +0x1C its texture count (u16), +0x28 the array of HyTexture pointers (the scarf is the last one, GetExtraTexture).
// PS2X_PLAYER_TEX_DUMP=<dir> writes each one once as a .ppm (palette applied, rows as stored) to find the sword's.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    bool inRam(uint32_t address, uint32_t bytes)
    {
        const uint32_t physical = address & 0x1FFFFFFFu;
        return bytes <= PS2_RAM_SIZE && physical <= PS2_RAM_SIZE - bytes;
    }

    uint32_t rd32(const uint8_t *rdram, uint32_t address)
    {
        if (!inRam(address, 4u))
            return 0u;
        uint32_t value;
        std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), 4u);
        return value;
    }

    uint16_t rd16(const uint8_t *rdram, uint32_t address)
    {
        if (!inRam(address, 2u))
            return 0u;
        uint16_t value;
        std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), 2u);
        return value;
    }

    void dumpTexture(const uint8_t *rdram, uint32_t texture, int index, const std::string &dir)
    {
        const uint32_t units = texture ? rd32(rdram, texture + 0x18u) : 0u;
        const uint32_t unit = units ? rd32(rdram, units) : 0u;
        const uint32_t bufIndex = unit ? rd32(rdram, unit + 0x48u) : 0u;
        const uint32_t buffer = unit && bufIndex < 4u ? rd32(rdram, unit + 0x4Cu + 4u * bufIndex) : 0u;
        if (!buffer || rd32(rdram, buffer) != 0x54525650u) // "PVRT"
        {
            std::fprintf(stderr, "[playertex] #%d texture 0x%08x unit 0x%08x buffer 0x%08x: not PVRT (%08x)\n", index, texture,
                         unit, buffer, buffer ? rd32(rdram, buffer) : 0u);
            return;
        }
        const uint32_t size = rd32(rdram, buffer + 4u) - 8u;
        const uint32_t w = rd16(rdram, buffer + 0xCu), h = rd16(rdram, buffer + 0xEu);
        const uint32_t data = buffer + 0x10u;
        int bpp = 0;
        uint32_t clut = 0u;
        if (size == w * h + 1024u) { bpp = 8; clut = 256u; }
        else if (size == w * h / 2u + 64u) { bpp = 4; clut = 16u; }
        else if (size == w * h * 4u) bpp = 32;
        else if (size == w * h * 2u) bpp = 16;
        std::fprintf(stderr, "[playertex] #%d texture 0x%08x unit 0x%08x data 0x%08x %ux%u %d bpp (chunk 0x%x)\n", index,
                     texture, unit, data, w, h, bpp, size);
        if (!bpp || !inRam(data, size) || w > 1024u || h > 1024u)
            return;
        const uint8_t *p = rdram + (data & 0x1FFFFFFFu);
        const uint8_t *pal = p;
        const uint8_t *px = p + clut * 4u;
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3u);
        for (uint32_t i = 0; i < w * h; ++i)
        {
            uint8_t r = 0, g = 0, b = 0;
            if (bpp == 8 || bpp == 4)
            {
                uint32_t idx = bpp == 8 ? px[i] : ((px[i / 2u] >> ((i & 1u) * 4u)) & 15u);
                if (bpp == 8) // PS2 CLUT order: entries 8..15 and 16..23 of each 32 are swapped
                    idx = (idx & 0xE7u) | ((idx & 0x08u) << 1) | ((idx & 0x10u) >> 1);
                r = pal[idx * 4u]; g = pal[idx * 4u + 1u]; b = pal[idx * 4u + 2u];
            }
            else if (bpp == 32)
            {
                r = px[i * 4u]; g = px[i * 4u + 1u]; b = px[i * 4u + 2u];
            }
            else
            {
                uint16_t v; std::memcpy(&v, px + i * 2u, 2u);
                r = static_cast<uint8_t>((v & 31u) << 3); g = static_cast<uint8_t>(((v >> 5) & 31u) << 3); b = static_cast<uint8_t>(((v >> 10) & 31u) << 3);
            }
            rgb[i * 3u] = r; rgb[i * 3u + 1u] = g; rgb[i * 3u + 2u] = b;
        }
        const std::string path = dir + "/player_tex_" + std::to_string(index) + ".ppm";
        if (FILE *f = std::fopen(path.c_str(), "wb"))
        {
            std::fprintf(f, "P6\n%u %u\n255\n", w, h);
            std::fwrite(rgb.data(), 1u, rgb.size(), f);
            std::fclose(f);
        }
    }
}

void ps2xRecolourPlayerTexture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int slot, uint32_t texture,
                               const std::string &wanted); // ps2_scarf_colour.cpp

// Called from CPlayer::DrawControlMuffler (src/runner) with the player object, every frame.
void ps2xPlayerTextureHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t player)
{
    // Research: PS2X_PLAYER_TEX_TINT=<index>:#rrggbb[,...] recolours those player textures (up to 4).
    if (static const char *s_tint = std::getenv("PS2X_PLAYER_TEX_TINT"); s_tint && player)
    {
        const uint32_t model = rd32(rdram, player + 0x2A0u);
        const uint32_t list = model ? rd32(rdram, model + 0x28u) : 0u;
        const uint32_t count = model ? rd16(rdram, model + 0x1Cu) : 0u;
        int slot = 0;
        for (const char *p = s_tint; p && *p && slot < 4; ++slot)
        {
            unsigned index = 0; char colour[8] = {};
            if (std::sscanf(p, "%u:%7s", &index, colour) == 2 && index < count)
                ps2xRecolourPlayerTexture(rdram, ctx, runtime, slot, rd32(rdram, list + 4u * index), colour);
            p = std::strchr(p, ',');
            if (p) ++p;
        }
    }
    static const char *s_dir = std::getenv("PS2X_PLAYER_TEX_DUMP");
    static bool s_done = false;
    if (!s_dir || s_done || !player)
        return;
    const uint32_t model = rd32(rdram, player + 0x2A0u);
    if (!model)
        return;
    const uint32_t count = rd16(rdram, model + 0x1Cu);
    const uint32_t list = rd32(rdram, model + 0x28u);
    std::fprintf(stderr, "[playertex] player 0x%08x model 0x%08x: %u textures (list 0x%08x, extra flag %u)\n", player, model,
                 count, list, rd16(rdram, model + 0x18u));
    for (uint32_t i = 0; i < count && i < 64u; ++i)
        dumpTexture(rdram, rd32(rdram, list + 4u * i), static_cast<int>(i), s_dir);
    s_done = true;
}
