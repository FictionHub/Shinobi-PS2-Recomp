// Offline test of ps2_framegen.cpp: a real game picture A, and B = A moved (a camera pan) with a square moving on its
// own. Feeds them as two game frames 1/60 s apart, asks for the picture half way, and checks it is the half-way motion,
// not a 50/50 double image.
#include "raylib.h"
#include "runtime/ps2_framegen.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace ps2x::settings
{
    bool getBool(const char *, const char *) { return true; }
}

static void sleepUntil(double t)
{
    while (GetTime() < t)
        std::this_thread::sleep_for(std::chrono::microseconds(200));
}

// Mean absolute difference (0..255) over the middle of the picture, between image x and a shifted copy of a.
static double errShift(const Image &x, const Image &a, int dx, int dy)
{
    const auto *px = static_cast<const unsigned char *>(x.data);
    const auto *pa = static_cast<const unsigned char *>(a.data);
    double e = 0.0;
    long n = 0;
    for (int y = x.height / 4; y < x.height * 3 / 4; y += 2)
        for (int xx = x.width / 4; xx < x.width * 3 / 4; xx += 2)
        {
            const int sx = xx - dx, sy = y - dy;
            for (int c = 0; c < 3; ++c)
                e += std::fabs(static_cast<double>(px[(y * x.width + xx) * 4 + c]) - pa[(sy * a.width + sx) * 4 + c]);
            n += 3;
        }
    return e / n;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(320, 240, "fgtest");
    Image a = LoadImage(argv[1]);
    ImageFormat(&a, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    const int W = a.width, H = a.height;
    const int panX = 24, panY = 10; // B = A moved right 24, down 10
    Image b = GenImageColor(W, H, BLACK);
    ImageDraw(&b, a, Rectangle{0, 0, static_cast<float>(W), static_cast<float>(H)},
              Rectangle{static_cast<float>(panX), static_cast<float>(panY), static_cast<float>(W), static_cast<float>(H)}, WHITE);
    // A square that moves 40 px left between A and B (on top of the pan).
    Image a2 = ImageCopy(a), b2 = ImageCopy(b);
    ImageDrawRectangle(&a2, W / 2 + 40, H / 2 - 60, 120, 120, Color{230, 40, 40, 255});
    ImageDrawRectangle(&b2, W / 2, H / 2 - 60, 120, 120, Color{230, 40, 40, 255});
    if (argc >= 3) // two real pictures instead
    {
        UnloadImage(b2);
        b2 = LoadImage(argv[2]);
        ImageFormat(&b2, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        UnloadImage(a2);
        a2 = ImageCopy(a);
    }
    Texture2D ta = LoadTextureFromImage(a2), tb = LoadTextureFromImage(b2);

    using namespace ps2x::framegen;
    // Warm-up (shaders compiled, buffers made), then A on a clean 1/60 s beat.
    noteFrame(static_cast<const uint8_t *>(a2.data), W, H);
    present(ta, W, H);
    sleepUntil(GetTime() + 1.0 / 60.0);
    noteFrame(static_cast<const uint8_t *>(a2.data), W, H);
    present(ta, W, H);
    const double t0 = GetTime();
    sleepUntil(t0 + 1.0 / 60.0);
    noteFrame(static_cast<const uint8_t *>(b2.data), W, H);
    Texture2D r0 = present(tb, W, H);
    const double tB = GetTime();
    std::printf("right after B arrives: shows %s\n", r0.id == ta.id ? "frame A (texture)" : (r0.id == tb.id ? "B" : "a buffer (A copy)"));
    sleepUntil(tB + 0.5 / 60.0);
    const double when = GetTime();
    Texture2D mid = present(tb, W, H);
    const double t = (when - tB) / (tB - t0);
    std::printf("asked at t = %.2f; generated: %s\n", t, active() ? "yes" : "no");
    Image out = LoadImageFromTexture(mid);
    ImageFormat(&out, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    ExportImage(out, "fg_mid.png");
    ExportImage(a2, "fg_A.png");
    ExportImage(b2, "fg_B.png");
    // Background: which pan best explains the middle picture?
    double best = 1e9;
    int bx = 0, by = 0;
    for (int dy = 0; dy <= panY; ++dy)
        for (int dx = 0; dx <= panX; ++dx)
        {
            const double e = errShift(out, a, dx, dy);
            if (e < best) { best = e; bx = dx; by = dy; }
        }
    std::printf("background: best pan (%d, %d) of (%d, %d), error %.2f; expected about (%.1f, %.1f)\n", bx, by, panX, panY, best,
                t * panX, t * panY);
    std::printf("for comparison: error vs A %.2f, vs B-pan %.2f\n", errShift(out, a, 0, 0), errShift(out, a, panX, panY));
    // The square: where is its left edge on the middle row?
    const auto *po = static_cast<const unsigned char *>(out.data);
    const int row = H / 2;
    int left = -1;
    for (int x = W / 2 - 80; x < W / 2 + 200; ++x)
    {
        const unsigned char *p = po + (row * W + x) * 4;
        if (p[0] > 180 && p[1] < 90 && p[2] < 90) { left = x; break; }
    }
    std::printf("square left edge at x=%d (A %d, B %d, expected about %.0f)\n", left, W / 2 + 40, W / 2,
                W / 2 + 40 - t * 40.0);
    CloseWindow();
    return 0;
}
