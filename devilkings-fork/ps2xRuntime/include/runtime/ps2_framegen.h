#pragma once
// LOCAL PATCH: frame generation. The game draws 60 pictures a second; on a 120 / 144 / 240 Hz screen the window shows
// in-between pictures made on the graphics card from the last two real ones (motion estimated between them, both moved
// part of the way along it and blended), so motion looks as smooth as the screen allows. Shown one game frame late
// (about 16 ms), as every frame-generation method is. [video] frame_generation (F6 in game).
#include <cstdint>

#include "raylib.h"

namespace ps2x::framegen
{
    // A new game picture was just uploaded into the window's frame texture (on the window thread, with its pixels):
    // used to spot cuts (a new scene, a menu), which are never blended.
    void noteFrame(const uint8_t *rgba, uint32_t width, uint32_t height);

    // The picture to draw this window frame: `frame` itself when frame generation is off or has nothing to blend, else
    // an in-between picture of the same size and layout. Call before BeginDrawing, once per window frame.
    Texture2D present(const Texture2D &frame, uint32_t width, uint32_t height);

    // Pictures shown per second by the window, real and generated (for the performance overlay); 0 when off.
    float shownPerSecond();
    bool active();
}
