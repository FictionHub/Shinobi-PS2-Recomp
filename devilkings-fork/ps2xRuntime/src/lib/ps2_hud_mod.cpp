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

// LOCAL PATCH (mod): no scarf at all. CPlayerMuffler::Draw (0x2b8e60) returns straight away while [game] hide_scarf is
// on (Mod Menu); its movement still runs, so it is back exactly in place when shown again.
bool ps2xScarfHidden()
{
    static std::atomic<uint64_t> s_version{~0ull};
    static std::atomic<bool> s_hidden{false};
    const uint64_t version = ps2x::settings::version();
    if (s_version.load(std::memory_order_relaxed) != version)
    {
        s_hidden.store(ps2x::settings::getBool("game", "hide_scarf"), std::memory_order_relaxed);
        s_version.store(version, std::memory_order_relaxed);
    }
    return s_hidden.load(std::memory_order_relaxed);
}

// LOCAL PATCH (mod): Akujiki's hunger off. CPlayer::MainControlMeters (0x24acc0) drains the sword gauge (+0x330) every
// frame while the sword is "awake" (CheckSwordWake), then your health (+0x32C) once the gauge is empty. With [game]
// no_sword_hunger (Mod Menu) that drain step is skipped (the check reads as asleep there only); kills, the gauge and
// everything else work as normal.
bool ps2xSwordHungerOff()
{
    static std::atomic<uint64_t> s_version{~0ull};
    static std::atomic<bool> s_off{false};
    const uint64_t version = ps2x::settings::version();
    if (s_version.load(std::memory_order_relaxed) != version)
    {
        s_off.store(ps2x::settings::getBool("game", "no_sword_hunger"), std::memory_order_relaxed);
        s_version.store(version, std::memory_order_relaxed);
    }
    return s_off.load(std::memory_order_relaxed);
}
