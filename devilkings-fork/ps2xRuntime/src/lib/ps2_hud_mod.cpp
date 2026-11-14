// LOCAL PATCH (mod): hide the HUD. The game's HUD pieces (DrawPlayerInfo, DrawHPInfo, DrawTateInfo, DrawKunaiInfo,
// DrawSLASHInfo, DrawMakimono, DrawOboroCoin, the boss and enemy gauges) each have a Draw function; the runner's copies
// of those return straight away while [game] hide_hud is on (F5 in game, the launcher's Display tab). Only drawing is
// skipped: the game still counts health, kunai and so on, so the HUD comes back exactly as it should be.
#include "runtime/ps2_settings.h"

#include <algorithm>
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

// LOCAL PATCH (mod): Infinite health (Mod Menu, F8): CPlayer::Damage (runner hook, scripts/runner_patches.py section 11) ends at once, as
// it does for a hit the game ignores, so no hit costs health. Hotsuma still plays his usual animations.
bool ps2xInfiniteHealth()
{
    static std::atomic<uint64_t> s_version{~0ull};
    static std::atomic<bool> s_on{false};
    const uint64_t version = ps2x::settings::version();
    if (s_version.load(std::memory_order_relaxed) != version)
    {
        s_on.store(ps2x::settings::getBool("game", "infinite_health"), std::memory_order_relaxed);
        s_version.store(version, std::memory_order_relaxed);
    }
    return s_on.load(std::memory_order_relaxed);
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
        // Infinite health also stops the drain (it takes health, so it would otherwise still hurt).
        s_off.store(ps2x::settings::getBool("game", "no_sword_hunger") || ps2x::settings::getBool("game", "infinite_health"),
                    std::memory_order_relaxed);
        s_version.store(version, std::memory_order_relaxed);
    }
    return s_off.load(std::memory_order_relaxed);
}

// LOCAL DEBUG (cinematic bars research): HyScreen's settings (+0x10 .. +0x54, floats) logged each time they change, so
// one TATE scene shows which value letterboxes the picture. At most 60 lines.
#include <cstdio>
#include <cstring>
#include <vector>
void ps2xScreenTrace(const uint8_t *rdram, uint32_t screen)
{
    // Each distinct (screen, values) once, so two screens alternating every frame cannot use the lines up.
    static std::vector<std::vector<float>> s_seen;
    const uint32_t p = screen & 0x1FFFFFFFu;
    if (!screen || p + 0x58u > 0x2000000u || s_seen.size() >= 80)
        return;
    float v[18];
    std::memcpy(v, rdram + p + 0x10u, sizeof(v));
    std::vector<float> key(v, v + 18);
    key.push_back(static_cast<float>(screen & 0xFFFFFu));
    for (const auto &k : s_seen)
        if (k == key)
            return;
    s_seen.push_back(key);
    std::fprintf(stderr, "[screen] 0x%08x clip %.1f %.1f %.1f %.1f | 3d %.2f %.2f %.2f %.2f %.2f | near/far %.2f %.2f | +3c %.2f %.2f %.2f | 2d %.2f %.2f %.2f %.2f\n",
                 screen, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17]);
}

// LOCAL PATCH (cinematic bars mod): movies (movieTask, TitleMovie) and story cutscenes (cEventPlayer) call this from
// their Draw; their letterbox stays, since nothing is drawn behind it. Only the TATE kill camera loses its bars.
#include <chrono>
static std::atomic<int64_t> s_keepBarsAt{INT64_MIN / 2};
static int64_t ps2xNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void ps2xKeepBarsMark()
{
    s_keepBarsAt.store(ps2xNowMs(), std::memory_order_relaxed);
}
bool ps2xKeepBars()
{
    return ps2xNowMs() - s_keepBarsAt.load(std::memory_order_relaxed) < 300;
}

// LOCAL PATCH (cinematic bars mod): the TATE kill camera's black bars are effects of the game's effect system
// (Ml_TexScreen / Ml_TexScreenPT): a textured rectangle given by 4 corners (this + 0x128, 0x18 apart, x y z floats) that
// the effect glides in and out. Their Draw returns at once while the mod is on and the rectangle is a full-width thin
// strip; every other screen effect (flashes, full-screen tints) draws as normal.
bool ps2xTexScreenIsBar(const uint8_t *rdram, uint32_t self)
{
    if (!ps2xCinemaBarsHidden() || ps2xKeepBars())
        return false;
    const uint32_t p = self & 0x1FFFFFFu;
    if (!self || p + 0x128u + 4u * 0x18u > 0x2000000u)
        return false;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < 4u; ++i)
    {
        float v[2];
        std::memcpy(v, rdram + p + 0x128u + i * 0x18u, sizeof(v));
        x0 = std::min(x0, v[0]);
        x1 = std::max(x1, v[0]);
        y0 = std::min(y0, v[1]);
        y1 = std::max(y1, v[1]);
    }
    const float w = x1 - x0, h = y1 - y0;
    const bool bar = w >= 560.0f && w <= 800.0f && h <= 180.0f;
    static std::vector<uint64_t> s_seen;
    const uint64_t key = (uint64_t(int(x0) & 0xFFFF) << 48) | (uint64_t(int(x1) & 0xFFFF) << 32) |
                         (uint64_t(int(y0) & 0xFFFF) << 16) | uint64_t(int(y1) & 0xFFFF);
    if (s_seen.size() < 60 && std::find(s_seen.begin(), s_seen.end(), key) == s_seen.end())
    {
        s_seen.push_back(key);
        std::fprintf(stderr, "[texscreen] x %.0f..%.0f y %.0f..%.0f -> %s\n", x0, x1, y0, y1, bar ? "bar, hidden" : "drawn");
    }
    return bar;
}
