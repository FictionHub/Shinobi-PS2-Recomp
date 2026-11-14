// LOCAL PATCH (mod): everything unlocked from the start ([game] unlock_all, Mod Menu).
//
// Shinobi keeps its unlocks in GameSystem: 128 secret-item flags at +0x27F (the Oboro coins and the like, read by
// GameSystem::CheckSecretItem, 0x23ceb0; their count unlocks the extra characters and the Extra menu's pages), and
// which difficulties may be picked (CheckValidGameLevel, 0x2434e0: the first two always, the higher ones once the
// save says they were earned). While the mod is on both answer "yes"; off, the game's own functions answer. The save
// itself is never changed, so switching the mod off puts everything back as it was.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace
{
    constexpr uint32_t kCheckSecretItem = 0x23CEB0u;
    constexpr uint32_t kCheckValidGameLevel = 0x2434E0u;
    PS2Runtime::RecompiledFunction g_origSecret = nullptr;
    PS2Runtime::RecompiledFunction g_origLevel = nullptr;

    bool unlocked()
    {
        static std::atomic<uint64_t> s_version{~0ull};
        static std::atomic<bool> s_on{false};
        const uint64_t version = ps2x::settings::version();
        if (s_version.load(std::memory_order_relaxed) != version)
        {
            s_on.store(ps2x::settings::getBool("game", "unlock_all"), std::memory_order_relaxed);
            s_version.store(version, std::memory_order_relaxed);
        }
        return s_on.load(std::memory_order_relaxed);
    }

    // GameSystem::CheckSecretItem(this, short index): the flag of that item (0 for an index past the 128).
    void checkSecretItem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!unlocked() && g_origSecret)
            return g_origSecret(rdram, ctx, runtime);
        const int16_t index = static_cast<int16_t>(GPR_U32(ctx, 5) & 0xFFFFu);
        SET_GPR_U32(ctx, 2, index >= 0 && index < 0x80 ? 1u : 0u);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // CheckValidGameLevel(unsigned char level): 1 when that difficulty may be picked.
    void checkValidGameLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!unlocked() && g_origLevel)
            return g_origLevel(rdram, ctx, runtime);
        SET_GPR_U32(ctx, 2, (GPR_U32(ctx, 4) & 0xFFu) <= 3u ? 1u : 0u);
        ctx->pc = GPR_U32(ctx, 31);
    }
}

// At start-up, after the game's functions are registered (ps2_runtime.cpp loadELF).
void ps2xUnlocksInstall(PS2Runtime &runtime)
{
    if (!runtime.hasFunction(kCheckSecretItem) || !runtime.hasFunction(kCheckValidGameLevel))
        return;
    g_origSecret = runtime.lookupFunction(kCheckSecretItem);
    g_origLevel = runtime.lookupFunction(kCheckValidGameLevel);
    runtime.replaceFunction(kCheckSecretItem, checkSecretItem);
    runtime.replaceFunction(kCheckValidGameLevel, checkValidGameLevel);
    std::fprintf(stderr, "[mods] unlock-everything switch ready (Mod Menu)\n");
}
