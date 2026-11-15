// LOCAL PATCH (issue #7, frame-time wobble at high frame rates): the window loop's frame cap. raylib's own limiter
// sleeps 95% of the frame with Sleep(), whose wake-up can be ~1 ms late; at 155+ pictures a second (6.5 ms a frame)
// that is a 15% wobble, visible as an uneven frame-time graph. This one sleeps on a high-resolution waitable timer
// until 0.5 ms before the deadline, then spins to it, and keeps a steady beat (deadlines are added up, not measured
// from "now", so a late frame does not shift the next ones).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cstdint>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// fps <= 0: no cap (vsync or nothing paces the loop).
void ps2xPaceFrame(int fps)
{
#ifdef _WIN32
    static LARGE_INTEGER s_freq{};
    static int64_t s_next = 0;
    static int s_lastFps = 0;
    static HANDLE s_timer = [] {
        HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t) // Windows before 10 1803: an ordinary timer (still better than Sleep with the spin below)
            t = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        return t;
    }();
    if (s_freq.QuadPart == 0)
        QueryPerformanceFrequency(&s_freq);
    if (fps <= 0)
    {
        s_next = 0;
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t period = s_freq.QuadPart / fps;
    // A new cap, the first frame, or more than 4 frames behind (a load, the F1 menu): start the beat from now.
    if (fps != s_lastFps || s_next == 0 || now.QuadPart - s_next > period * 4)
        s_next = now.QuadPart;
    s_lastFps = fps;
    s_next += period;
    const int64_t remaining = s_next - now.QuadPart;
    if (remaining <= 0)
        return;
    const int64_t spin = s_freq.QuadPart / 2000; // the last 0.5 ms are spun
    if (remaining > spin && s_timer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((remaining - spin) * 10000000 / s_freq.QuadPart); // 100 ns units, relative
        if (SetWaitableTimerEx(s_timer, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(s_timer, INFINITE);
    }
    for (;;)
    {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= s_next)
            break;
        YieldProcessor();
    }
#else
    (void)fps;
#endif
}
