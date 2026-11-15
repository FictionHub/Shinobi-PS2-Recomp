// LOCAL PATCH: which kind of controller is connected, without waiting for a button press (the page's Gamepad API only
// lists a controller after one). Every HID game controller (usage page 1, joystick or gamepad) is looked up by its
// maker's USB vendor ID, then by its product name for makers that build several kinds; XInput finds Xbox pads that do
// not show up as HID. When several are connected a PlayStation pad wins, then a Nintendo one, then the rest.

#include "launcher_pads.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace launcher::pads
{
    namespace
    {
        enum Kind
        {
            None = 0,
            Xbox = 1,
            Nintendo = 2,
            PlayStation = 3,
        };

        // Makers and the buttons their controllers carry. 0 = decided by the product name.
        struct Maker
        {
            USHORT vendor;
            Kind kind;
        };
        constexpr Maker kMakers[] = {
            {0x054C, PlayStation}, // Sony: DualShock 3/4, DualSense, DualSense Edge
            {0x146B, PlayStation}, // Nacon (PlayStation licensed)
            {0x045E, Xbox},        // Microsoft: Xbox 360 / One / Series, Elite
            {0x0E6F, Xbox},        // PDP
            {0x24C6, Xbox},        // PowerA / BDA
            {0x046D, Xbox},        // Logitech (F310 / F710)
            {0x0738, Xbox},        // Mad Catz
            {0x28DE, Xbox},        // Valve (Steam Controller, Steam Deck)
            {0x2563, Xbox},        // ShanWan generic pads
            {0x0079, Xbox},        // DragonRise generic pads
            {0x057E, Nintendo},    // Nintendo: Switch Pro Controller, Joy-Con
            {0x0F0D, None},        // Hori: PlayStation, Xbox and Switch pads
            {0x1532, None},        // Razer: Raiju (PlayStation), Wolverine (Xbox)
            {0x2DC8, None},        // 8BitDo: modes for each
            {0x20D6, None},        // PowerA Switch pads
            {0x0E8F, None},        // generic
        };

        Kind fromName(std::wstring name)
        {
            std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            const auto has = [&](const wchar_t *s) { return name.find(s) != std::wstring::npos; };
            if (has(L"dualsense") || has(L"dualshock") || has(L"playstation") || has(L"ps5") || has(L"ps4") || has(L"ps3") ||
                has(L"wireless controller") || has(L"raiju"))
                return PlayStation;
            if (has(L"switch") || has(L"pro controller") || has(L"joy-con") || has(L"nintendo"))
                return Nintendo;
            return Xbox;
        }

        Kind hidKind()
        {
            GUID hidGuid;
            HidD_GetHidGuid(&hidGuid);
            HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (set == INVALID_HANDLE_VALUE)
                return None;
            Kind best = None;
            SP_DEVICE_INTERFACE_DATA iface{};
            iface.cbSize = sizeof(iface);
            for (DWORD i = 0; best != PlayStation && SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &iface); ++i)
            {
                DWORD need = 0;
                SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
                std::vector<uint8_t> buffer(need + sizeof(WCHAR));
                auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr))
                    continue;
                // No access asked for: enough to read what the device is, and never in the way of the game.
                HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                HIDD_ATTRIBUTES a{};
                a.Size = sizeof(a);
                PHIDP_PREPARSED_DATA prepared = nullptr;
                HIDP_CAPS caps{};
                bool controller = false;
                if (HidD_GetAttributes(h, &a) && HidD_GetPreparsedData(h, &prepared))
                {
                    HidP_GetCaps(prepared, &caps);
                    HidD_FreePreparsedData(prepared);
                    controller = caps.UsagePage == 0x01 && (caps.Usage == 0x04 || caps.Usage == 0x05);
                }
                if (controller)
                {
                    Kind kind = Xbox; // an unknown maker: the usual Xbox-style layout
                    bool byName = true;
                    for (const Maker &m : kMakers)
                    {
                        if (m.vendor != a.VendorID)
                            continue;
                        if (m.kind != None)
                        {
                            kind = m.kind;
                            byName = false;
                        }
                        break;
                    }
                    if (byName)
                    {
                        wchar_t product[128] = {};
                        if (HidD_GetProductString(h, product, sizeof(product)))
                            kind = fromName(product);
                    }
                    best = std::max(best, kind);
                }
                CloseHandle(h);
            }
            SetupDiDestroyDeviceInfoList(set);
            return best;
        }

        // Xbox pads on the XInput driver (not always listed as HID game controllers).
        bool xinputPad()
        {
            using GetState = DWORD(WINAPI *)(DWORD, void *);
            static GetState s_get = [] {
                for (const wchar_t *dll : {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"})
                    if (HMODULE m = LoadLibraryW(dll))
                        return reinterpret_cast<GetState>(GetProcAddress(m, "XInputGetState"));
                return static_cast<GetState>(nullptr);
            }();
            if (!s_get)
                return false;
            unsigned char state[16] = {}; // XINPUT_STATE
            for (DWORD i = 0; i < 4; ++i)
                if (s_get(i, state) == ERROR_SUCCESS)
                    return true;
            return false;
        }

        const char *name(Kind k)
        {
            return k == PlayStation ? "ps" : k == Nintendo ? "nintendo" : k == Xbox ? "xbox" : "";
        }

        std::atomic<bool> g_watching{false};
    }

    std::string kind()
    {
        Kind k = hidKind();
        if (k == None && xinputPad())
            k = Xbox;
        return name(k);
    }

    void watch(std::function<void(const std::wstring &)> send)
    {
        if (g_watching.exchange(true))
            return;
        std::thread([send]() {
            std::string last = "?";
            for (;;)
            {
                const std::string now = kind();
                if (now != last)
                {
                    last = now;
                    send(L"pad:kind-" + std::wstring(now.empty() ? L"none" : std::wstring(now.begin(), now.end())));
                }
                Sleep(2000);
            }
        }).detach();
    }
}
