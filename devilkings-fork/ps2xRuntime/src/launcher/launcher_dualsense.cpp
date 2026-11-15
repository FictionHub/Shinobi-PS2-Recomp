// LOCAL PATCH: the launcher reads a PS5 DualSense / DualSense Edge itself (Windows HID, read only) and drives the page
// with "pad:up" / "pad:down" / ... messages. The page's Gamepad API loses the DualSense once the game has used its
// haptics over Bluetooth: that switches the controller to its full report (0x31), which only a direct reader decodes,
// and it stays that way until the controller reconnects. This reads both the simple (0x01) and the full report, over
// USB and Bluetooth, and sends nothing to the controller. Other controllers stay with the page's Gamepad API.

#include "launcher_dualsense.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace launcher::dualsense
{
    namespace
    {
        std::atomic<bool> g_running{false};
        std::thread g_thread;

        // ---- LOCAL PATCH: menu haptics. Short taps on the controller while the page is driven with it (a thump per
        // move, beads while the right stick scrolls). A DualSense gets the vibration-emulation output report (the game's
        // layout, ps2_dualsense.cpp: 0x02 over USB, 0x31 + CRC32 over Bluetooth) through a write handle of its own, so a
        // tap never waits behind the reader's ReadFile; an Xbox-style pad gets XInput rumble.
        std::mutex g_outLock;
        std::condition_variable g_outWake;
        std::wstring g_outPath;
        bool g_outBluetooth = false;
        USHORT g_outLength = 0;
        uint8_t g_btSeq = 0;
        struct Tap
        {
            uint8_t left = 0, right = 0;
            int ms = 0;
            uint64_t serial = 0;
        } g_tap;
        bool g_outStarted = false;

        uint32_t crc32(const uint8_t *data, size_t n, uint32_t crc = 0xFFFFFFFFu)
        {
            for (size_t i = 0; i < n; ++i)
            {
                crc ^= data[i];
                for (int k = 0; k < 8; ++k)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return crc;
        }

        bool rumbleDualSense(HANDLE h, bool bt, USHORT len, uint8_t left, uint8_t right)
        {
            if (h == INVALID_HANDLE_VALUE)
                return false;
            uint8_t report[80] = {};
            uint8_t *c = nullptr;
            DWORD size = 0;
            if (bt)
            {
                report[0] = 0x31;
                report[1] = static_cast<uint8_t>(g_btSeq << 4);
                g_btSeq = static_cast<uint8_t>((g_btSeq + 1u) & 15u);
                report[2] = 0x10;
                c = report + 3;
            }
            else
            {
                report[0] = 0x02;
                c = report + 1;
            }
            // In vibration emulation the left actuator (the "big motor") hits far harder than the right one, so equal
            // levels are felt on the left only: the left is turned well down and the right kept at full, so a tap is
            // felt in the centre of the pad.
            left = static_cast<uint8_t>(left * 0.40f);
            c[0] = 0x01 | 0x02; // vibration emulation + haptics select (the triggers are left alone)
            c[2] = right;       // right actuator (the light, crisp one)
            c[3] = left;        // left actuator (the heavy one)
            c[38] = 0x04;       // improved vibration emulation (newer firmware)
            if (bt)
            {
                const uint8_t seed = 0xA2;
                const uint32_t crc = ~crc32(report, 74, crc32(&seed, 1));
                std::memcpy(report + 74, &crc, 4);
                size = std::max<DWORD>(78, len);
            }
            else
                size = std::min<DWORD>(64, std::max<DWORD>(48, len));
            std::vector<uint8_t> buffer(std::max<DWORD>(size, 80), 0);
            std::memcpy(buffer.data(), report, sizeof(report));
            DWORD written = 0;
            return WriteFile(h, buffer.data(), size, &written, nullptr) != FALSE;
        }

        // Xbox-style pads (XInput): the same tap on their rumble motors.
        void rumbleXInput(uint8_t left, uint8_t right)
        {
            struct Vibration { WORD left, right; };
            using SetState = DWORD(WINAPI *)(DWORD, Vibration *);
            using GetState = DWORD(WINAPI *)(DWORD, void *);
            static SetState s_set = nullptr;
            static GetState s_get = nullptr;
            static bool s_loaded = false;
            if (!s_loaded)
            {
                s_loaded = true;
                for (const wchar_t *dll : {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"})
                    if (HMODULE m = LoadLibraryW(dll))
                    {
                        s_set = reinterpret_cast<SetState>(GetProcAddress(m, "XInputSetState"));
                        s_get = reinterpret_cast<GetState>(GetProcAddress(m, "XInputGetState"));
                        break;
                    }
            }
            if (!s_set || !s_get)
                return;
            unsigned char state[16] = {};
            for (DWORD i = 0; i < 4; ++i)
                if (s_get(i, state) == ERROR_SUCCESS)
                {
                    Vibration v{static_cast<WORD>(left * 257), static_cast<WORD>(right * 257)};
                    s_set(i, &v);
                }
        }

        void outputWorker()
        {
            HANDLE h = INVALID_HANDLE_VALUE;
            std::wstring openedPath;
            uint64_t played = 0;
            std::unique_lock<std::mutex> lock(g_outLock);
            for (;;)
            {
                g_outWake.wait(lock, [&] { return g_tap.serial != played; });
                const Tap tap = g_tap;
                played = tap.serial;
                if (g_outPath != openedPath)
                {
                    if (h != INVALID_HANDLE_VALUE)
                        CloseHandle(h);
                    openedPath = g_outPath;
                    h = openedPath.empty() ? INVALID_HANDLE_VALUE
                                           : CreateFileW(openedPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                         OPEN_EXISTING, 0, nullptr);
                }
                const bool bt = g_outBluetooth;
                const USHORT len = g_outLength;
                lock.unlock();
                if (!rumbleDualSense(h, bt, len, tap.left, tap.right) && h != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(h); // the controller went away: opened again with the next one
                    h = INVALID_HANDLE_VALUE;
                    openedPath.clear();
                }
                rumbleXInput(tap.left, tap.right);
                lock.lock();
                // the tap's length, unless a newer tap takes over (then it simply continues with the new strength)
                if (!g_outWake.wait_for(lock, std::chrono::milliseconds(tap.ms), [&] { return g_tap.serial != played; }))
                {
                    lock.unlock();
                    rumbleDualSense(h, bt, len, 0, 0);
                    rumbleXInput(0, 0);
                    lock.lock();
                }
            }
        }

        void setOutputDevice(const std::wstring &path, bool bluetooth, USHORT length)
        {
            std::lock_guard<std::mutex> lock(g_outLock);
            g_outPath = path;
            g_outBluetooth = bluetooth;
            g_outLength = length;
        }

        HANDLE openPad(bool &bluetooth, std::wstring *pathOut = nullptr, USHORT *outLen = nullptr)
        {
            GUID hidGuid;
            HidD_GetHidGuid(&hidGuid);
            HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (set == INVALID_HANDLE_VALUE)
                return INVALID_HANDLE_VALUE;
            HANDLE found = INVALID_HANDLE_VALUE;
            SP_DEVICE_INTERFACE_DATA iface{};
            iface.cbSize = sizeof(iface);
            for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &iface); ++i)
            {
                DWORD need = 0;
                SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
                std::vector<uint8_t> buffer(need + sizeof(WCHAR));
                auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr))
                    continue;
                HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                HIDD_ATTRIBUTES a{};
                a.Size = sizeof(a);
                PHIDP_PREPARSED_DATA prepared = nullptr;
                HIDP_CAPS caps{};
                if (HidD_GetAttributes(h, &a) && a.VendorID == 0x054C && (a.ProductID == 0x0CE6 || a.ProductID == 0x0DF2) &&
                    HidD_GetPreparsedData(h, &prepared))
                {
                    HidP_GetCaps(prepared, &caps);
                    HidD_FreePreparsedData(prepared);
                    if (caps.UsagePage == 0x01 && caps.Usage == 0x05)
                    {
                        bluetooth = caps.OutputReportByteLength >= 78;
                        if (pathOut)
                            *pathOut = detail->DevicePath; // LOCAL PATCH: menu haptics write through their own handle
                        if (outLen)
                            *outLen = caps.OutputReportByteLength;
                        found = h;
                        continue;
                    }
                }
                CloseHandle(h);
            }
            SetupDiDestroyDeviceInfoList(set);
            return found;
        }

        // The menu buttons from a report: up, down, left, right, cross, circle, l1, r1, options. False if not a report
        // with buttons (the simple Bluetooth report has them elsewhere).
        bool decode(const uint8_t *r, DWORD size, bool bluetooth, bool out[16], float &rightY)
        {
            const uint8_t *p = nullptr;
            uint8_t lx = 128, ly = 128, ry = 128, b0 = 8, b1 = 0;
            if (r[0] == 0x31 && size >= 12)
                p = r + 2; // full report (Bluetooth)
            else if (r[0] == 0x01 && !bluetooth && size >= 11)
                p = r + 1; // USB
            if (p)
            {
                lx = p[0];
                ly = p[1];
                ry = p[3];
                b0 = p[7];
                b1 = p[8];
            }
            else if (r[0] == 0x01 && bluetooth && size >= 10)
            {
                // Simple Bluetooth report: sticks at 1..4, buttons at 5..6 (as a DualShock 4's).
                lx = r[1];
                ly = r[2];
                ry = r[4];
                b0 = r[5];
                b1 = r[6];
            }
            else
                return false;
            rightY = (ry - 127.5f) / 127.5f; // LOCAL PATCH: the right stick scrolls lists
            const int hat = b0 & 0x0F;
            const float dx = (lx - 127.5f) / 127.5f, dy = (ly - 127.5f) / 127.5f;
            out[0] = hat == 7 || hat == 0 || hat == 1 || dy < -0.6f;
            out[1] = (hat >= 3 && hat <= 5) || dy > 0.6f;
            out[2] = (hat >= 5 && hat <= 7) || dx < -0.6f;
            out[3] = (hat >= 1 && hat <= 3) || dx > 0.6f;
            out[4] = (b0 & 0x20) != 0; // cross
            out[5] = (b0 & 0x40) != 0; // circle
            out[6] = (b1 & 0x01) != 0; // L1
            out[7] = (b1 & 0x02) != 0; // R1
            out[8] = (b1 & 0x20) != 0; // options
            // LOCAL PATCH (controller remapping): the rest, so the Configure Controller page can bind any button.
            out[9] = (b0 & 0x10) != 0;  // square
            out[10] = (b0 & 0x80) != 0; // triangle
            out[11] = (b1 & 0x04) != 0; // L2
            out[12] = (b1 & 0x08) != 0; // R2
            out[13] = (b1 & 0x10) != 0; // create
            out[14] = (b1 & 0x40) != 0; // L3
            out[15] = (b1 & 0x80) != 0; // R3
            return true;
        }

        void worker(std::function<void(const std::wstring &)> send)
        {
            static const wchar_t *kNames[16] = {L"up", L"down", L"left", L"right", L"cross", L"circle", L"l1", L"r1", L"start",
                                                 L"square", L"triangle", L"l2", L"r2", L"select", L"l3", L"r3"};
            const auto clock = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
            while (g_running.load())
            {
                bool bluetooth = false;
                std::wstring devicePath;
                USHORT outputLength = 0;
                HANDLE h = openPad(bluetooth, &devicePath, &outputLength);
                if (h == INVALID_HANDLE_VALUE)
                {
                    for (int i = 0; i < 20 && g_running.load(); ++i)
                        Sleep(100);
                    continue;
                }
                send(L"pad:host"); // the page leaves this controller to us from now on
                setOutputDevice(devicePath, bluetooth, outputLength); // LOCAL PATCH: menu haptics
                bool held[16] = {};
                double next[16] = {};
                bool first = true;
                std::vector<uint8_t> report(128);
                while (g_running.load())
                {
                    DWORD got = 0;
                    if (!ReadFile(h, report.data(), static_cast<DWORD>(report.size()), &got, nullptr) || got == 0)
                        break; // disconnected
                    bool now[16] = {};
                    float rightY = 0.0f;
                    if (!decode(report.data(), got, bluetooth, now, rightY))
                        continue;
                    {
                        // LOCAL PATCH: the right stick (up / down) for the page's lists, about 60 times a second while held.
                        static double s_lastStick = 0.0;
                        static bool s_stickHeld = false;
                        const double ts = clock();
                        const bool stickHeld = rightY > 0.2f || rightY < -0.2f;
                        if (stickHeld && ts - s_lastStick >= 0.016)
                        {
                            s_lastStick = ts;
                            send(L"pad:rsy:" + std::to_wstring(static_cast<int>(rightY * 100.0f)));
                        }
                        else if (!stickHeld && s_stickHeld)
                            send(L"pad:rsy:0");
                        s_stickHeld = stickHeld;
                    }
                    const double t = clock();
                    for (int i = 0; i < 16; ++i)
                    {
                        if (first)
                        {
                            held[i] = now[i]; // already held at start: counts after a release
                            next[i] = 1e30;
                            continue;
                        }
                        if (!now[i])
                        {
                            held[i] = false;
                            continue;
                        }
                        if (!held[i])
                        {
                            held[i] = true;
                            next[i] = t + 0.38;
                            send(std::wstring(L"pad:") + kNames[i]);
                        }
                        else if (i < 4 && t >= next[i])
                        {
                            next[i] = t + 0.11;
                            send(std::wstring(L"pad:") + kNames[i]);
                        }
                    }
                    first = false;
                }
                CloseHandle(h);
                send(L"pad:lost");
            }
        }
    }

    // LOCAL PATCH: a menu haptic tap: strengths 0..1 (left = heavy actuator, right = light one), `ms` long.
    void tap(float left, float right, int ms)
    {
        std::lock_guard<std::mutex> lock(g_outLock);
        if (!g_outStarted)
        {
            g_outStarted = true;
            std::thread(outputWorker).detach();
        }
        const auto level = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        g_tap.left = level(left);
        g_tap.right = level(right);
        g_tap.ms = std::clamp(ms, 5, 200);
        ++g_tap.serial;
        g_outWake.notify_one();
    }

    void start(std::function<void(const std::wstring &)> send)
    {
        if (g_running.exchange(true))
            return;
        g_thread = std::thread(worker, std::move(send));
        g_thread.detach(); // ReadFile blocks: the thread ends with the process
    }
}
