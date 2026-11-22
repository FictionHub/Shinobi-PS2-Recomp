#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "runtime/ps2_settings.h"

// LOCAL PATCH (GitHub issue #20): a bound key or mouse button (codes from 1000 are mouse buttons, ps2_settings.cpp).
bool ps2xInGameplay(); // ps2_hud_mod.cpp
bool ps2xMoviePlaying(uint32_t &width, uint32_t &height); // Kernel/Stubs/MPEG.cpp
static bool ps2xInputDown(int code)
{
    return code >= 1000 ? IsMouseButtonDown(code - 1000) : IsKeyDown(code);
}
extern std::atomic<bool> g_ps2MouseLookActive; // Pad.cpp, set by the window loop (ps2_runtime.cpp)
extern std::atomic<float> g_ps2MouseStickX;
extern std::atomic<int> g_ps2MouseWheelNotches; // Pad.cpp (LOCAL PATCH: mouse in menus)
extern std::atomic<bool> g_ps2MouseFakePush;    // ps2_runtime.cpp (LOCAL PATCH: mouse camera)
bool ps2xMouseAimPending();                       // ps2_runtime.cpp
extern std::atomic<float> g_ps2MouseStickY;

// LOCAL PATCH (DualSense): src/lib/ps2_dualsense.cpp
struct Ps2xDualSenseInput
{
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
    uint8_t hat;
    bool square, cross, circle, triangle, l1, r1, l2Button, r2Button, create, options, l3, r3, ps;
};
bool ps2xDualSenseRead(Ps2xDualSenseInput &out);
// LOCAL PATCH (haptics trace): src/lib/ps2_haptics_trace.cpp
void ps2xHapticTraceL2(int pad, int value, bool down);

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    // LOCAL PATCH: GLFW only reports a joystick as a gamepad when it has a mapping, and older tables lack the
    // DualSense Edge (054c:0df2). Same layout as the DualSense entry, for both Windows GUID spellings.
    static bool s_mappingsAdded = false;
    if (!s_mappingsAdded && IsWindowReady())
    {
        s_mappingsAdded = true;
        static const char kPs5Layout[] =
            "a:b1,b:b2,x:b0,y:b3,back:b8,guide:b12,start:b9,leftstick:b10,rightstick:b11,"
            "leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
            "leftx:a0,lefty:a1,rightx:a2,righty:a5,lefttrigger:a3,righttrigger:a4,touchpad:b13,platform:Windows,\n";
        const std::string mappings = std::string("030000004c050000f20d000000000000,DualSense Edge,") + kPs5Layout +
                                     "030000004c050000e60c000000000000,PS5 Controller," + kPs5Layout;
        SetGamepadMappings(mappings.c_str());
    }
    // Prefer a PlayStation pad; racing wheels, shifters and pedals also show up as gamepads and must not win.
    // The scan lowercases device names, so it runs at most twice a second; the choice is reused in between.
    static int s_chosenGamepad = -1;
    static std::chrono::steady_clock::time_point s_lastScan{};
    int kGamepad = s_chosenGamepad;
    const auto scanNow = std::chrono::steady_clock::now();
    const bool rescan = s_lastScan.time_since_epoch().count() == 0 || scanNow - s_lastScan > std::chrono::milliseconds(500);
    if (rescan)
    {
        s_lastScan = scanNow;
        kGamepad = -1;
        const auto contains = [](const char *name, const char *part)
        {
            std::string lower(name ? name : "");
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower.find(part) != std::string::npos;
        };
        int fallback = -1;
        for (int i = 0; i < 4 && kGamepad < 0; ++i)
        {
            if (!IsGamepadAvailable(i))
                continue;
            const char *name = GetGamepadName(i);
            if (contains(name, "dualsense") || contains(name, "ps5") || contains(name, "dualshock") ||
                contains(name, "wireless controller") || contains(name, "ps4") || contains(name, "xbox"))
                kGamepad = i;
            else if (fallback < 0 && !contains(name, "shifter") && !contains(name, "wheel") &&
                     !contains(name, "pedal") && !contains(name, "throttle"))
                fallback = i;
        }
        if (kGamepad < 0)
            kGamepad = fallback;
        s_chosenGamepad = kGamepad;
    }
    const bool useGamepad = kGamepad >= 0;
    static int s_loggedPad = -2;
    if (kGamepad != s_loggedPad)
    {
        s_loggedPad = kGamepad;
        std::fprintf(stderr, "[pad] using gamepad %d: %s\n", kGamepad, useGamepad ? GetGamepadName(kGamepad) : "(none, keyboard)");
    }
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    // LOCAL PATCH: while a menu is open (the in-game menu, F1) the controller drives the menu, not the game: all buttons
    // released and both sticks centred.
    if (ps2x::settings::inputBlocked())
    {
        data[2] = 0xFF;
        data[3] = 0xFF;
        data[4] = data[5] = data[6] = data[7] = kPadStickCenter;
        return true;
    }

    if (useGamepad)
    {
        // LOCAL PATCH: every connected controller is read, not only the chosen one. Steam Input, the EA app and similar tools
        // can take the real DualSense / Xbox pad and hand its input to a virtual controller with another number, so the pad
        // the scan picked sees nothing. Buttons from all of them count; each stick takes the one pushed furthest.
        // Wheels, shifters and pedals stay out (they report gamepad axes of their own).
        static std::vector<int> s_pads;
        if (rescan)
        {
            s_pads.clear();
            for (int i = 0; i < 4; ++i)
            {
                if (!IsGamepadAvailable(i))
                    continue;
                std::string lower(GetGamepadName(i) ? GetGamepadName(i) : "");
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find("shifter") == std::string::npos && lower.find("wheel") == std::string::npos &&
                    lower.find("pedal") == std::string::npos && lower.find("throttle") == std::string::npos)
                    s_pads.push_back(i);
            }
            if (s_pads.empty())
                s_pads.push_back(kGamepad);
            static size_t s_loggedCount = 0u;
            if (s_pads.size() != s_loggedCount)
            {
                s_loggedCount = s_pads.size();
                std::fprintf(stderr, "[pad] reading %zu controller(s):", s_pads.size());
                for (int i : s_pads)
                    std::fprintf(stderr, " %d (%s)", i, GetGamepadName(i) ? GetGamepadName(i) : "?");
                std::fputc('\n', stderr);
            }
        }
        static const std::pair<int, uint16_t> kButtons[] = {
            {GAMEPAD_BUTTON_LEFT_FACE_UP, PAD_UP},         {GAMEPAD_BUTTON_LEFT_FACE_DOWN, PAD_DOWN},
            {GAMEPAD_BUTTON_LEFT_FACE_LEFT, PAD_LEFT},     {GAMEPAD_BUTTON_LEFT_FACE_RIGHT, PAD_RIGHT},
            {GAMEPAD_BUTTON_RIGHT_FACE_DOWN, PAD_CROSS},   {GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, PAD_CIRCLE},
            {GAMEPAD_BUTTON_RIGHT_FACE_LEFT, PAD_SQUARE},  {GAMEPAD_BUTTON_RIGHT_FACE_UP, PAD_TRIANGLE},
            {GAMEPAD_BUTTON_LEFT_TRIGGER_1, PAD_L1},       {GAMEPAD_BUTTON_RIGHT_TRIGGER_1, PAD_R1},
            {GAMEPAD_BUTTON_LEFT_TRIGGER_2, PAD_L2},       {GAMEPAD_BUTTON_RIGHT_TRIGGER_2, PAD_R2},
            {GAMEPAD_BUTTON_MIDDLE_RIGHT, PAD_START},      {GAMEPAD_BUTTON_MIDDLE_LEFT, PAD_SELECT},
            {GAMEPAD_BUTTON_LEFT_THUMB, PAD_L3},           {GAMEPAD_BUTTON_RIGHT_THUMB, PAD_R3}};
        float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
        // LOCAL PATCH (controller remapping, GitHub issue #5): the controller's own buttons are gathered first (same bit
        // layout as the pad report, set = held), then [controls] pad_<button> decides which game button each one presses.
        uint16_t phys = 0u;
        for (int pad : s_pads)
        {
            for (const auto &[button, bit] : kButtons)
                if (IsGamepadButtonDown(pad, button))
                    phys |= bit;
            {
                // LOCAL PATCH (haptics trace): how far L2 is pressed (raylib: -1 released .. 1 fully in).
                const float l2 = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_TRIGGER);
                ::ps2xHapticTraceL2(pad, static_cast<int>(std::clamp((l2 + 1.0f) * 127.5f, 0.0f, 255.0f)),
                                  IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_TRIGGER_2));
            }
            const float plx = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X), ply = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y);
            const float prx = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_X), pry = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y);
            if (plx * plx + ply * ply > lx * lx + ly * ly)
            {
                lx = plx;
                ly = ply;
            }
            if (prx * prx + pry * pry > rx * rx + ry * ry)
            {
                rx = prx;
                ry = pry;
            }
        }
        // LOCAL PATCH: a DualSense read directly (ps2_dualsense.cpp, for haptics): its buttons and sticks count too. Over
        // Bluetooth the usual controller APIs stop seeing it once haptics start, so this keeps it working.
        {
            Ps2xDualSenseInput ds{};
            if (::ps2xDualSenseRead(ds))
            {
                const bool hatUp = ds.hat == 7 || ds.hat == 0 || ds.hat == 1;
                const bool hatRight = ds.hat >= 1 && ds.hat <= 3;
                const bool hatDown = ds.hat >= 3 && ds.hat <= 5;
                const bool hatLeft = ds.hat >= 5 && ds.hat <= 7;
                const std::pair<bool, uint16_t> pressed[] = {
                    {hatUp, PAD_UP},         {hatDown, PAD_DOWN},       {hatLeft, PAD_LEFT},     {hatRight, PAD_RIGHT},
                    {ds.cross, PAD_CROSS},   {ds.circle, PAD_CIRCLE},   {ds.square, PAD_SQUARE}, {ds.triangle, PAD_TRIANGLE},
                    {ds.l1, PAD_L1},         {ds.r1, PAD_R1},           {ds.l2Button, PAD_L2},   {ds.r2Button, PAD_R2},
                    {ds.options, PAD_START}, {ds.create, PAD_SELECT},   {ds.l3, PAD_L3},         {ds.r3, PAD_R3}};
                for (const auto &[down, bit] : pressed)
                    if (down)
                        phys |= bit;
                // LOCAL PATCH: with the L2 trigger resistance on, L2 counts as pressed only past the stiff part (the
                // effect gives way at about 70% of its travel): a light touch no longer fires the ninja magic. The
                // controller's own L2 bit (set at a light touch, here or through the usual APIs) is overridden.
                if (ps2x::settings::getBool("input", "trigger_resistance"))
                {
                    if (ds.l2 >= 190)
                        phys |= PAD_L2;
                    else
                        phys &= static_cast<uint16_t>(~PAD_L2);
                }
                const float dlx = (ds.lx - 127.5f) / 127.5f, dly = (ds.ly - 127.5f) / 127.5f;
                const float drx = (ds.rx - 127.5f) / 127.5f, dry = (ds.ry - 127.5f) / 127.5f;
                if (dlx * dlx + dly * dly > lx * lx + ly * ly)
                {
                    lx = dlx;
                    ly = dly;
                }
                if (drx * drx + dry * dry > rx * rx + ry * ry)
                {
                    rx = drx;
                    ry = dry;
                }
            }
        }
        {
            static uint64_t s_mapVersion = ~0ull;
            static std::vector<std::pair<uint16_t, uint16_t>> s_map; // (game button, controller button)
            const uint64_t version = ps2x::settings::version();
            if (version != s_mapVersion)
            {
                s_mapVersion = version;
                s_map.clear();
                static const std::pair<const char *, uint16_t> kNames[] = {
                    {"up", PAD_UP},         {"down", PAD_DOWN},     {"left", PAD_LEFT},     {"right", PAD_RIGHT},
                    {"cross", PAD_CROSS},   {"circle", PAD_CIRCLE}, {"square", PAD_SQUARE}, {"triangle", PAD_TRIANGLE},
                    {"l1", PAD_L1},         {"r1", PAD_R1},         {"l2", PAD_L2},         {"r2", PAD_R2},
                    {"start", PAD_START},   {"select", PAD_SELECT}, {"l3", PAD_L3},         {"r3", PAD_R3}};
                const auto bitOf = [](std::string name) -> uint16_t
                {
                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    for (const auto &[n, b] : kNames)
                        if (name == n)
                            return b;
                    return 0u; // "none" or unknown: not on the controller
                };
                for (const auto &[name, bit] : kNames)
                {
                    const std::string source = ps2x::settings::get("controls", (std::string("pad_") + name).c_str());
                    s_map.emplace_back(bit, source.empty() ? bit : bitOf(source));
                }
            }
            for (const auto &[target, source] : s_map)
                if (source != 0u && (phys & source) != 0u)
                    clearBit(target);
        }
        // LOCAL PATCH: a round dead zone ([input] stick_deadzone, % of the stick's travel, 15 by default): an analog stick
        // that rests slightly off centre (a worn or drifting DualSense / Xbox pad) would otherwise walk the character or
        // turn the camera on its own. Outside it the stick is stretched to its full range.
        const float zone = static_cast<float>(std::clamp(ps2x::settings::getNumber("input", "stick_deadzone"), 0, 50)) / 100.0f;
        const auto stick = [zone](float x, float y, uint8_t &outX, uint8_t &outY)
        {
            const float length = std::sqrt(x * x + y * y);
            if (length <= zone || length < 0.0001f)
            {
                outX = outY = 128;
                return;
            }
            const float scale = std::min(1.0f, (length - zone) / (1.0f - zone)) / length;
            outX = static_cast<uint8_t>(std::clamp(128.0f + x * scale * 127.0f, 0.0f, 255.0f));
            outY = static_cast<uint8_t>(std::clamp(128.0f + y * scale * 127.0f, 0.0f, 255.0f));
        };
        stick(lx, ly, data[6], data[7]);
        stick(rx, ry, data[4], data[5]);
    }
    // LOCAL PATCH: the keyboard works alongside a connected controller too (it used to be ignored while one was plugged in).
    if (!ps2x::settings::inputBlocked())
    {
        // Keyboard bindings from the settings ([controls], the F1 menu), parsed again when
        // the settings change.
        static uint64_t s_version = 0u;
        static std::vector<std::pair<uint16_t, std::vector<int>>> s_bindings;
        const uint64_t version = ps2x::settings::version();
        if (version != s_version)
        {
            s_bindings.clear();
            for (const ps2x::settings::PadButton &button : ps2x::settings::padButtons())
            {
                std::vector<int> keys;
                const std::string list = ps2x::settings::get("controls", button.key);
                size_t begin = 0;
                while (begin <= list.size())
                {
                    size_t comma = list.find(',', begin);
                    if (comma == std::string::npos)
                        comma = list.size();
                    if (const int code = ps2x::settings::keyCode(list.substr(begin, comma - begin)))
                        keys.push_back(code);
                    begin = comma + 1;
                }
                s_bindings.emplace_back(button.bit, std::move(keys));
            }
            s_version = version;
        }
        // LOCAL PATCH: Enter pressed together with Alt is the window shortcut (Alt+Enter, ps2_runtime.cpp), not Start: it is
        // ignored until it is let go.
        static bool s_altEnter = false;
        const bool enterDown = IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER);
        if (enterDown && (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)))
            s_altEnter = true;
        else if (!enterDown)
            s_altEnter = false;
        // LOCAL PATCH (GitHub issue #20): the move keys ([controls] move_up/down/left/right, WASD by default) push only the
        // left stick. A move key also left on a D-pad button (older settings had "UP,W") no longer presses the D-pad, which
        // in a stage picks the ninpo scroll.
        static uint64_t s_moveVersion = ~0ull;
        static std::vector<int> s_move[4]; // up, down, left, right
        if (version != s_moveVersion)
        {
            static const char *const kMove[4] = {"move_up", "move_down", "move_left", "move_right"};
            for (int axis = 0; axis < 4; ++axis)
            {
                s_move[axis].clear();
                const std::string list = ps2x::settings::get("controls", kMove[axis]); // WASD by default (ps2_settings.cpp)
                size_t begin = 0;
                while (begin <= list.size())
                {
                    size_t comma = list.find(',', begin);
                    if (comma == std::string::npos)
                        comma = list.size();
                    if (const int code = ps2x::settings::keyCode(list.substr(begin, comma - begin)))
                        s_move[axis].push_back(code);
                    begin = comma + 1;
                }
            }
            s_moveVersion = version;
        }
        const auto isMoveKey = [](int key) {
            for (const auto &keys : s_move)
                for (int k : keys)
                    if (k == key)
                        return true;
            return false;
        };
        uint16_t keyboardDirections = 0u;
        for (const auto &[bit, keys] : s_bindings)
        {
            for (int key : keys)
            {
                if (s_altEnter && (key == KEY_ENTER || key == KEY_KP_ENTER))
                    continue;
                if ((bit & 0x00F0u) && isMoveKey(key))
                    continue; // a move key: the stick only (below)
                if (ps2xInputDown(key))
                {
                    // LOCAL PATCH (keyboard in menus): Enter is Start while playing (pause), but in menus, the pause
                    // menu, the title and results it confirms like Cross (submenus only take Cross). During a movie
                    // it stays Start: Start is what skips a cutscene.
                    uint32_t movieW = 0u, movieH = 0u;
                    if (bit == 0x0008u && (key == KEY_ENTER || key == KEY_KP_ENTER) && !ps2xInGameplay() &&
                        !ps2xMoviePlaying(movieW, movieH))
                    {
                        clearBit(0x4000u);
                        break;
                    }
                    clearBit(bit);
                    keyboardDirections |= bit;
                    break;
                }
            }
        }
        {
            static const uint16_t kMoveBits[4] = {0x0010u, 0x0040u, 0x0080u, 0x0020u};
            for (int axis = 0; axis < 4; ++axis)
                for (int key : s_move[axis])
                    if (ps2xInputDown(key))
                    {
                        keyboardDirections |= kMoveBits[axis]; // stick direction only: no clearBit (D-pad untouched)
                        break;
                    }
        }
        // LOCAL PATCH (GitHub issue #11): the direction keys (WASD and the arrows by default) also push the left stick,
        // which is what moves Hotsuma (the D-pad alone only works in menus). Diagonals are kept at full length.
        {
            const float kx = ((keyboardDirections & 0x0020u) ? 1.0f : 0.0f) - ((keyboardDirections & 0x0080u) ? 1.0f : 0.0f);
            const float ky = ((keyboardDirections & 0x0040u) ? 1.0f : 0.0f) - ((keyboardDirections & 0x0010u) ? 1.0f : 0.0f);
            if (kx != 0.0f || ky != 0.0f)
            {
                const float scale = (kx != 0.0f && ky != 0.0f) ? 0.7071f : 1.0f;
                data[6] = static_cast<uint8_t>(std::clamp(128.0f + kx * scale * 127.0f, 0.0f, 255.0f));
                data[7] = static_cast<uint8_t>(std::clamp(128.0f + ky * scale * 127.0f, 0.0f, 255.0f));
            }
        }
    }

    // LOCAL PATCH: the camera (the pad's right stick) on keyboard and mouse. The four keys are [controls] camera_up / camera_down /
    // camera_left / camera_right (I K J L by default, rebindable in the launcher and the F1 menu). The mouse, while the window
    // has captured it (ps2_runtime.cpp, F10 switches that on and off), adds its movement; left click is Square, right click Circle.
    if (!ps2x::settings::inputBlocked())
    {
        static uint64_t s_cameraVersion = ~0ull;
        static std::vector<int> s_camera[4];
        const uint64_t version = ps2x::settings::version();
        if (version != s_cameraVersion)
        {
            static const char *const kNames[4] = {"camera_up", "camera_down", "camera_left", "camera_right"};
            for (int axis = 0; axis < 4; ++axis)
            {
                s_camera[axis].clear();
                const std::string list = ps2x::settings::get("controls", kNames[axis]);
                size_t begin = 0;
                while (begin <= list.size())
                {
                    size_t comma = list.find(',', begin);
                    if (comma == std::string::npos)
                        comma = list.size();
                    if (const int code = ps2x::settings::keyCode(list.substr(begin, comma - begin)))
                        s_camera[axis].push_back(code);
                    begin = comma + 1;
                }
            }
            s_cameraVersion = version;
        }
        const auto held = [](const std::vector<int> &keys)
        {
            for (int key : keys)
                if (ps2xInputDown(key))
                    return true;
            return false;
        };
        float cx = (held(s_camera[3]) ? 1.0f : 0.0f) - (held(s_camera[2]) ? 1.0f : 0.0f);
        float cy = (held(s_camera[1]) ? 1.0f : 0.0f) - (held(s_camera[0]) ? 1.0f : 0.0f);
        if (g_ps2MouseLookActive.load(std::memory_order_relaxed))
        {
            cx += g_ps2MouseStickX.load(std::memory_order_relaxed);
            cy += g_ps2MouseStickY.load(std::memory_order_relaxed);
            // The default clicks (left Square, right Circle) only while that mouse button is not bound to anything in
            // [controls] (GitHub issue #20: a click bound to Triangle also pressed Square, and the sword won).
            static uint64_t s_clickVersion = ~0ull;
            static bool s_leftBound = false, s_rightBound = false, s_middleBound = false;
            if (const uint64_t v = ps2x::settings::version(); v != s_clickVersion)
            {
                s_clickVersion = v;
                s_leftBound = s_rightBound = s_middleBound = false;
                for (const ps2x::settings::PadButton &button : ps2x::settings::padButtons())
                {
                    const std::string list = ps2x::settings::get("controls", button.key);
                    s_leftBound = s_leftBound || list.find("MOUSE_LEFT") != std::string::npos;
                    s_rightBound = s_rightBound || list.find("MOUSE_RIGHT") != std::string::npos;
                    s_middleBound = s_middleBound || list.find("MOUSE_MIDDLE") != std::string::npos;
                }
            }
            // LOCAL PATCH (mouse in menus): out of play (title, menus, pause menu, results) left click confirms (Cross)
            // and right click goes back (Circle); the wheel moves the menu cursor (one notch = one D-pad press).
            // LOCAL PATCH (mouse in play, PC layout): left click = Square (sword), right click = R1 (lock-on), middle
            // click = L2 (ninjutsu), the wheel = R2 (next target; the game has no "previous", so both directions step
            // on). Game defaults from GameSystem::InitSystemData / CPlayer::MainControlLockon: lock-on is config 0 (R1),
            // next target config 6 (R2), ninjutsu L2.
            const bool menus = !ps2xInGameplay();
            if (!s_leftBound && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
                clearBit(menus ? PAD_CROSS : PAD_SQUARE);
            if (!s_rightBound && IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
                clearBit(menus ? PAD_CIRCLE : PAD_R1);
            if (!s_middleBound && !menus && IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))
                clearBit(PAD_L2);
            static int s_wheelFrames = 0;
            static uint16_t s_wheelBit = 0u;
            static bool s_wheelMenus = true;
            if (menus != s_wheelMenus)
            {
                g_ps2MouseWheelNotches.store(0, std::memory_order_relaxed); // a notch never carries over between the two
                s_wheelFrames = 0;
                s_wheelMenus = menus;
            }
            if (s_wheelFrames == 0)
            {
                const int notches = g_ps2MouseWheelNotches.load(std::memory_order_relaxed);
                if (notches != 0)
                {
                    s_wheelBit = !menus ? PAD_R2 : notches > 0 ? PAD_UP : PAD_DOWN;
                    g_ps2MouseWheelNotches.fetch_sub(notches > 0 ? 1 : -1, std::memory_order_relaxed);
                    s_wheelFrames = 12; // 8 reads held, then 4 released, so each notch is a separate press
                }
            }
            if (s_wheelFrames > 0)
            {
                if (s_wheelFrames > 4)
                    clearBit(s_wheelBit);
                --s_wheelFrames;
            }
        }
        if (cx != 0.0f || cy != 0.0f)
        {
            data[4] = static_cast<uint8_t>(std::clamp(128.0f + std::clamp(cx, -1.0f, 1.0f) * 127.0f, 0.0f, 255.0f));
            data[5] = static_cast<uint8_t>(std::clamp(128.0f + std::clamp(cy, -1.0f, 1.0f) * 127.0f, 0.0f, 255.0f));
        }
    }

    // LOCAL PATCH (GitHub issue #4): invert the camera (right stick, camera keys and mouse alike).
    {
        const auto flip = [](uint8_t v) { return static_cast<uint8_t>(std::clamp(256 - static_cast<int>(v), 0, 255)); };
        if (ps2x::settings::getBool("input", "invert_camera_x"))
            data[4] = flip(data[4]);
        if (ps2x::settings::getBool("input", "invert_camera_y"))
            data[5] = flip(data[5]);
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
