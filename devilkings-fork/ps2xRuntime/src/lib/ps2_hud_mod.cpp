#include <chrono>
// LOCAL PATCH (mod): hide the HUD. The game's HUD pieces (DrawPlayerInfo, DrawHPInfo, DrawTateInfo, DrawKunaiInfo,
// DrawSLASHInfo, DrawMakimono, DrawOboroCoin, the boss and enemy gauges) each have a Draw function; the runner's copies
// of those return straight away while [game] hide_hud is on (F5 in game, the launcher's Display tab). Only drawing is
// skipped: the game still counts health, kunai and so on, so the HUD comes back exactly as it should be.
#include "runtime/ps2_settings.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
// LOCAL PATCH (keyboard in menus): a heartbeat while Hotsuma is really being played. CPlayer::MainControlMeters (this
// hook) and the stage camera (ps2xMouseAimX / Y in ps2_runtime.cpp) run every frame of play and stop in menus, the pause
// menu, cutscenes and the title; ps2_pad.cpp makes Enter confirm (Cross) whenever they have stopped.
std::atomic<double> g_ps2PlayHeartbeat{-100.0};
double ps2xNowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void ps2xPlayHeartbeat()
{
    g_ps2PlayHeartbeat.store(ps2xNowSeconds(), std::memory_order_relaxed);
}
bool ps2xInGameplay()
{
    return ps2xNowSeconds() - g_ps2PlayHeartbeat.load(std::memory_order_relaxed) < 0.25;
}

// LOCAL PATCH (mod, GitHub #22): no camera recentering. CCameraManage::_FollowTarget (0x2e59c0) takes the camera's
// turn (yaw) each frame from where the camera now stands relative to Hotsuma (VectorToRotation at 0x2e5c14, into
// sp+0x124). The camera's position trails him, so running sideways swings it round behind him. With [game]
// no_camera_recenter that yaw is replaced by the camera's own yaw from the last frame (CameraBase +0xB8): the angle stays
// where it is, and the game's own distance keeping (_moveToRate) slides the camera along with him. The right stick
// (_MovePad, eased by _FollowRotation), the camera-reset button, walls and collision all still turn it as before.
// Only the normal follow camera with the player in control: not the lock-on / other-follow camera (+0x2C), a scripted
// linear move (+0x38), or while the pad is off (cutscenes, CCameraManage +0x3C).
void ps2xCameraKeepYaw(uint8_t *rdram, uint32_t manage, uint32_t cam, uint32_t sp)
{
    static uint64_t s_version = ~0ull;
    static bool s_on = false;
    const uint64_t version = ps2x::settings::version();
    if (s_version != version)
    {
        s_on = ps2x::settings::getBool("game", "no_camera_recenter");
        s_version = version;
    }
    auto rd = [rdram](uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4u); return v; };
    // Test hook: PS2X_CAM_TRACE=1 logs the camera's yaw (and the one the game wanted) twice a second.
    static const bool s_trace = std::getenv("PS2X_CAM_TRACE") != nullptr;
    static uint32_t s_calls = 0;
    if (s_trace && (++s_calls % 30u) == 0u)
        std::fprintf(stderr, "[camtrace] yaw=%u wanted=%u pad=%u other=%u linear=%u on=%d\n", rd(cam + 0xB8u) & 0xFFFFu,
                     rd(sp + 0x124u) & 0xFFFFu, rd(manage + 0x3Cu), rd(cam + 0x2Cu), rd(cam + 0x38u), s_on ? 1 : 0);
    if (!s_on)
        return;
    if (rd(manage + 0x3Cu) == 0u || rd(cam + 0x28u) == 0u || rd(cam + 0x2Cu) != 0u || rd(cam + 0x38u) != 0u)
        return;
    const uint32_t yaw = rd(cam + 0xB8u);
    std::memcpy(rdram + ((sp + 0x124u) & 0x01FFFFFFu), &yaw, 4u);
}

bool ps2xSwordHungerOff()
{
    ps2xPlayHeartbeat();
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

// LOCAL PATCH (GitHub issue #23): field of view. [video] fov is a percentage (100 = the original view). It changes the
// projection distance (HyScreen +0x30) of the MAIN camera's screen only - the one the game widens for 16:9
// (AdjustDisplayGameAspect, ps2xWidescreenMainScreen) - never the second, un-widened screen the game uses for 3D HUD
// pieces. The value is changed where it lives and kept changed (not only around the matrix build), so every part of the
// game that reads it (effects, glows, shadows, culling) sees the same view: changing it only for the matrix put effects
// in the wrong place (black boxes, a black scroll, a broken health ring at 110 %). Whenever the game writes its own
// value again it is scaled again; at 100 % the game's own value is put back. Hook: HyScreen::_settingToEnvironment
// (scripts/runner_patches.py section 15).
#include "runtime/ps2_settings.h"
#include <algorithm>
uint32_t ps2xWidescreenMainScreen(); // ps2_runtime.cpp
namespace
{
    uint32_t g_fovScreen = 0;   // the screen whose distance is changed now (0 = none)
    float g_fovOriginal = 0.0f; // the game's own value
    float g_fovWritten = 0.0f;  // the value written there
}
void ps2xFovApply(uint8_t *rdram, uint32_t screen)
{
    const uint32_t p = screen & 0x1FFFFFFFu;
    if (!screen || p + 0x34u > 0x2000000u)
        return;
    static uint64_t s_version = ~0ull;
    static float s_scale = 1.0f;
    if (const uint64_t v = ps2x::settings::version(); v != s_version)
    {
        s_version = v;
        s_scale = static_cast<float>(std::clamp(ps2x::settings::getNumber("video", "fov"), 70, 150)) / 100.0f;
    }
    float current;
    std::memcpy(&current, rdram + p + 0x30u, 4u);
    bool ours = g_fovScreen == screen && current == g_fovWritten;
    // LOCAL PATCH (save states): a loaded state can hold a distance this mod wrote at another FOV setting (or in another
    // session). It is recognised as a known original divided by a whole percentage 70..150, so it is not scaled twice.
    if (!ours && screen == ps2xWidescreenMainScreen())
    {
        const float bases[2] = {g_fovOriginal, 554.3f};
        for (const float base : bases)
        {
            if (!(base > 1.0f) || !(current > 1.0f))
                continue;
            const float pct = base / current * 100.0f;
            const float whole = std::round(pct);
            if (whole >= 70.0f && whole <= 150.0f && whole != 100.0f && std::fabs(pct - whole) < 0.02f)
            {
                g_fovScreen = screen;
                g_fovOriginal = base;
                g_fovWritten = current;
                ours = true;
                std::fprintf(stderr, "[fov] loaded view distance %.1f recognised as %.0f %% of %.1f\n", current, whole, base);
                break;
            }
        }
    }
    const bool main = screen == ps2xWidescreenMainScreen();
    if (!main || (s_scale > 0.999f && s_scale < 1.001f))
    {
        if (ours) // back to the game's own view (setting back at 100 %)
            std::memcpy(rdram + p + 0x30u, &g_fovOriginal, 4u);
        if (g_fovScreen == screen)
            g_fovScreen = 0;
        return;
    }
    const float original = ours ? g_fovOriginal : current; // the game wrote its own value again: that is the base
    if (!(original > 1.0f && original < 100000.0f)) // not a projection distance: left alone
        return;
    const float wanted = original / s_scale;
    if (ours && wanted == current)
        return;
    std::memcpy(rdram + p + 0x30u, &wanted, 4u);
    g_fovScreen = screen;
    g_fovOriginal = original;
    g_fovWritten = wanted;
}
void ps2xFovRestore(uint8_t *, uint32_t)
{
    // nothing: the changed value stays (see ps2xFovApply)
}

// LOCAL DEBUG (infinite health and pits): every hit on Hotsuma in the log - its kind (SPowerData +0xC: 7 / 8 are the
// game's instant kills), attack type (+0xD), power and his health - so a pit or death zone shows how it kills.
void ps2xPlayerHitTrace(uint8_t *rdram, uint32_t player, uint32_t power)
{
    if (!player || !power)
        return;
    auto rd32 = [rdram](uint32_t a) { int32_t v; std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4u); return v; };
    const uint8_t kind = rdram[(power + 0xCu) & 0x01FFFFFFu];
    const uint8_t type = rdram[(power + 0xDu) & 0x01FFFFFFu];
    std::fprintf(stderr, "[player-hit] kind=%u type=%u power=%d hp=%d\n", kind, type, rd32(power), rd32(player + 0x32Cu));
}