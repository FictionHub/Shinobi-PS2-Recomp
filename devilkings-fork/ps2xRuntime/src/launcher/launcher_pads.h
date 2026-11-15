#pragma once
// LOCAL PATCH: which kind of controller is connected (launcher_pads.cpp), so the page names the right buttons
// (L1 / R1 on PlayStation pads, LB / RB on Xbox-style ones, L / R on Nintendo ones).
#include <functional>
#include <string>

namespace launcher::pads
{
    // "ps", "xbox", "nintendo", or "" when no controller is connected. Looks at the devices now (a few ms).
    std::string kind();
    // Checks every 2 s on its own thread; `send` gets "pad:kind-ps" / "pad:kind-xbox" / "pad:kind-nintendo" /
    // "pad:kind-none" at the start and whenever it changes.
    void watch(std::function<void(const std::wstring &)> send);
}
