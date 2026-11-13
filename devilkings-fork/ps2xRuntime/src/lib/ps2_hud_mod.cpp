// LOCAL PATCH (mod): hide the HUD. The game's HUD pieces (DrawPlayerInfo, DrawHPInfo, DrawTateInfo, DrawKunaiInfo,
// DrawSLASHInfo, DrawMakimono, DrawOboroCoin, the boss and enemy gauges) each have a Draw function; the runner's copies
// of those return straight away while [game] hide_hud is on (F5 in game, the launcher's Display tab). Only drawing is
// skipped: the game still counts health, kunai and so on, so the HUD comes back exactly as it should be.
#include "runtime/ps2_settings.h"

#include <atomic>
#include <cstdint>

bool ps2xHudHidden()
{
    static std::atomic<uint64_t> s_version{~0ull};
    static std::atomic<bool> s_hidden{false};
    const uint64_t version = ps2x::settings::version();
    if (s_version.load(std::memory_order_relaxed) != version)
    {
        s_hidden.store(ps2x::settings::getBool("game", "hide_hud"), std::memory_order_relaxed);
        s_version.store(version, std::memory_order_relaxed);
    }
    return s_hidden.load(std::memory_order_relaxed);
}

// LOCAL PATCH (mod): no black bars in the game's own cinematics (EventAction::Draw, 0x307ec0: a 640x48 band at the top
// and a 640x80 one at the bottom, e.g. the TATE kill camera). [game] hide_cinema_bars, Shift+F5 in game. Fades and the
// movies' own borders are not touched.
bool ps2xCinemaBarsHidden()
{
    static std::atomic<uint64_t> s_version{~0ull};
    static std::atomic<bool> s_hidden{false};
    const uint64_t version = ps2x::settings::version();
    if (s_version.load(std::memory_order_relaxed) != version)
    {
        s_hidden.store(ps2x::settings::getBool("game", "hide_cinema_bars"), std::memory_order_relaxed);
        s_version.store(version, std::memory_order_relaxed);
    }
    return s_hidden.load(std::memory_order_relaxed);
}
