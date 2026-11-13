// LOCAL PATCH: frame generation (see runtime/ps2_framegen.h).
//
// Per new game picture (GPU, all in the window thread's OpenGL context):
//   1. copy it into "B" (the previous one becomes "A"), and a brightness pyramid of it: 1/2 .. 1/32 size.
//   2. motion from B to A, coarse to fine (1/32 -> 1/4 size): every pixel tries the offsets around its estimate from the
//      level above and its neighbours' estimates, scoring each by the brightness difference over a 3x3 patch, with a
//      small pull towards the estimate it started from (so flat areas do not wander).
//   3. a 3x3 median of the motion, which removes single wrong vectors without blurring motion edges.
// Per window frame:
//   4. the picture at the moment t between A (t = 0) and B (t = 1): A sampled t of the way along the motion, B the rest
//      of the way, blended by t. Where the two disagree (something uncovered or covered, or a wrong vector), the
//      nearer real picture is used instead of a blend, so mistakes show as a tiny judder, never as smearing.
// The window shows the moment one game frame ago, so there is always a real picture on each side of it.
// Never blended: a cut (big change between the two pictures), a gap (a load, a pause, the F1 menu), a screen under 90 Hz.

#include "runtime/ps2_framegen.h"
#include "runtime/ps2_settings.h"

#include "rlgl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
    constexpr int kLevels = 5;      // brightness pyramid: 1/2, 1/4, 1/8, 1/16, 1/32
    constexpr int kFinestFlow = 2;  // motion is found down to level 2 (1/4 size)

    // ---- shaders -----------------------------------------------------------------------------------------------------
    // Every pass draws one rectangle over its whole target and works out its texel from gl_FragCoord, so every texture
    // keeps the frame texture's own row order and the result can be drawn exactly like it.
    const char *kCopy = R"(#version 330
out vec4 finalColor;
uniform sampler2D src;
uniform vec2 outSize;
uniform vec2 srcUvScale; // the picture's share of the source texture
void main() { finalColor = vec4(texture(src, gl_FragCoord.xy / outSize * srcUvScale).rgb, 1.0); }
)";

    const char *kLuma = R"(#version 330
out vec4 finalColor;
uniform sampler2D src;
uniform vec2 outSize;
uniform int fromColour;
void main()
{
    // Half size: one bilinear sample half way between four source texels is their average.
    // Colour, not only brightness: a red cloth and a grey wall can be equally bright.
    vec4 c = texture(src, gl_FragCoord.xy / outSize);
    finalColor = vec4(c.rgb, 1.0);
}
)";

    const char *kSearch = R"(#version 330
out vec4 finalColor;
uniform sampler2D lumaA;
uniform sampler2D lumaB;
uniform sampler2D coarse;     // motion from the level above (full-size pixels), when hasCoarse == 1
uniform vec2 outSize;         // this level's size
uniform vec2 fullSize;        // the picture's size
uniform int hasCoarse;
uniform int radius;           // offsets tried around each starting estimate, in this level's texels
uniform float levelScale;     // full-size pixels per texel of this level

float cost(vec2 uv, vec2 v)
{
    vec2 px = 1.0 / outSize;
    vec2 off = v / fullSize;
    float s = 0.0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
        {
            vec2 p = uv + vec2(i, j) * px;
            vec3 d = abs(texture(lumaB, p).rgb - texture(lumaA, p + off).rgb);
            s += (d.r + d.g + d.b) * (1.0 / 3.0);
        }
    return s;
}

void main()
{
    vec2 uv = gl_FragCoord.xy / outSize;
    vec2 start = vec2(0.0);
    vec2 cand[5];
    int n = 1;
    cand[0] = vec2(0.0);
    if (hasCoarse == 1)
    {
        vec2 cpx = 1.0 / outSize; // a neighbour at this level; the coarse texture is sampled bilinearly
        start = texture(coarse, uv).xy;
        cand[1] = texture(coarse, uv + vec2(cpx.x * 2.0, 0.0)).xy;
        cand[2] = texture(coarse, uv - vec2(cpx.x * 2.0, 0.0)).xy;
        cand[3] = texture(coarse, uv + vec2(0.0, cpx.y * 2.0)).xy;
        cand[4] = texture(coarse, uv - vec2(0.0, cpx.y * 2.0)).xy;
        n = 5;
    }
    vec2 best = start;
    float bestCost = cost(uv, start);
    // Neighbours' estimates and no motion at all.
    for (int k = 0; k < n; ++k)
    {
        float c = cost(uv, cand[k]) + 0.002 * length(cand[k] - start) / levelScale;
        if (c < bestCost) { bestCost = c; best = cand[k]; }
    }
    // Offsets around the best so far.
    vec2 centre = best;
    for (int j = -radius; j <= radius; ++j)
        for (int i = -radius; i <= radius; ++i)
        {
            if (i == 0 && j == 0)
                continue;
            vec2 v = centre + vec2(i, j) * levelScale;
            float c = cost(uv, v) + 0.002 * length(v - start) / levelScale;
            if (c < bestCost) { bestCost = c; best = v; }
        }
    finalColor = vec4(best, bestCost, 1.0);
}
)";

    const char *kMedian = R"(#version 330
out vec4 finalColor;
uniform sampler2D flow;
uniform vec2 outSize;
// The 13th smallest of 25 (partial selection sort).
float med25(float v[25])
{
    for (int i = 0; i < 13; ++i)
        for (int j = i + 1; j < 25; ++j)
            if (v[j] < v[i]) { float t = v[i]; v[i] = v[j]; v[j] = t; }
    return v[12];
}
void main()
{
    vec2 px = 1.0 / outSize;
    vec2 uv = gl_FragCoord.xy / outSize;
    float xs[25];
    float ys[25];
    int k = 0;
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i)
        {
            vec2 f = texture(flow, uv + vec2(i, j) * px).xy;
            xs[k] = f.x;
            ys[k] = f.y;
            ++k;
        }
    finalColor = vec4(med25(xs), med25(ys), 0.0, 1.0);
}
)";

    const char *kInterp = R"(#version 330
out vec4 finalColor;
uniform sampler2D frameA;
uniform sampler2D frameB;
uniform sampler2D flowBA; // B pixel -> where it was in A (full-size pixels)
uniform sampler2D flowAB; // A pixel -> where it went in B
uniform vec2 outSize;
uniform float t;          // 0 = A, 1 = B
vec2 F(vec2 uv) { return texture(flowBA, uv).xy / outSize; }
vec2 G(vec2 uv) { return texture(flowAB, uv).xy / outSize; }
void main()
{
    vec2 q = gl_FragCoord.xy / outSize;
    // The B pixel that is at q at time t: pB + (1 - t) * F(pB) = q. A few fixed-point steps find it.
    vec2 vb = F(q);
    for (int i = 0; i < 3; ++i) vb = F(q - (1.0 - t) * vb);
    vec2 pB = q - (1.0 - t) * vb;
    // The A pixel that is at q at time t: pA + t * G(pA) = q.
    vec2 va = G(q);
    for (int i = 0; i < 3; ++i) va = G(q - t * va);
    vec2 pA = q - t * va;
    vec3 a = texture(frameA, pA).rgb;
    vec3 b = texture(frameB, pB).rgb;
    // Each is trusted when the other direction agrees: going there and back lands where it started. A pixel that is
    // covered in the other picture (or a wrong vector) fails that, and the other picture is used for it.
    float eb = length((vb + G(pB + vb)) * outSize);
    float ea = length((va + F(pA + va)) * outSize);
    float wb = t * exp(-eb * eb / 18.0) + 1e-4;
    float wa = (1.0 - t) * exp(-ea * ea / 18.0) + 1e-4;
    vec3 c = (a * wa + b * wb) / (wa + wb);
    // Neither trustworthy: the nearer real picture, along its own motion.
    float either = max(exp(-ea * ea / 18.0), exp(-eb * eb / 18.0));
    vec3 near = t < 0.5 ? a : b;
    finalColor = vec4(mix(near, c, clamp(either * 4.0, 0.0, 1.0)), 1.0);
}
)";

    // ---- state -------------------------------------------------------------------------------------------------------
    struct Frame
    {
        RenderTexture2D colour{};
        std::array<RenderTexture2D, kLevels + 1> luma{}; // [1..kLevels]
    };

    struct State
    {
        bool tried = false;
        bool ok = false;
        Shader copy{}, luma{}, search{}, median{}, interp{};
        int w = 0, h = 0;
        Frame frames[2];
        int a = 0, b = 1;             // which of frames[] is A (older) and B (newer)
        std::array<RenderTexture2D, kLevels + 1> flow{}; // [kFinestFlow..kLevels]
        RenderTexture2D flowRefine{}; // the finest level again, in 1-pixel steps
        RenderTexture2D flowSmooth{};   // B -> A
        RenderTexture2D flowSmoothAB{}; // A -> B
        RenderTexture2D out{};
        bool haveA = false, haveB = false;
        double timeA = 0.0, timeB = 0.0;
        uint64_t seen = 0u;
        bool pairUsable = false;
        // shown per second
        uint64_t shownCount = 0u;
        double shownSince = 0.0;
        float shownRate = 0.0f;
        bool activeNow = false;
    };

    State &st()
    {
        static State s;
        return s;
    }

    // Written by noteFrame (window thread), read by present (window thread too).
    uint64_t g_serial = 0u;
    bool g_cut = true;              // the newest picture does not follow on from the one before
    std::vector<float> g_signature; // coarse brightness grid of the newest picture

    RenderTexture2D makeTarget(int w, int h, int format)
    {
        RenderTexture2D t{};
        t.id = rlLoadFramebuffer();
        if (!t.id)
            return t;
        rlEnableFramebuffer(t.id);
        t.texture.id = rlLoadTexture(nullptr, w, h, format, 1);
        t.texture.width = w;
        t.texture.height = h;
        t.texture.format = format;
        t.texture.mipmaps = 1;
        rlFramebufferAttach(t.id, t.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
        const bool complete = rlFramebufferComplete(t.id);
        rlDisableFramebuffer();
        if (!complete || !t.texture.id)
        {
            if (t.texture.id)
                rlUnloadTexture(t.texture.id);
            rlUnloadFramebuffer(t.id);
            return RenderTexture2D{};
        }
        SetTextureFilter(t.texture, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(t.texture, TEXTURE_WRAP_CLAMP);
        return t;
    }

    void freeTarget(RenderTexture2D &t)
    {
        if (t.id)
        {
            if (t.texture.id)
                rlUnloadTexture(t.texture.id);
            rlUnloadFramebuffer(t.id);
        }
        t = RenderTexture2D{};
    }

    int levelSize(int full, int level)
    {
        return std::max(1, (full + (1 << level) - 1) >> level);
    }

    void freeAll(State &s)
    {
        for (Frame &f : s.frames)
        {
            freeTarget(f.colour);
            for (int l = 1; l <= kLevels; ++l)
                freeTarget(f.luma[l]);
        }
        for (int l = kFinestFlow; l <= kLevels; ++l)
            freeTarget(s.flow[l]);
        freeTarget(s.flowRefine);
        freeTarget(s.flowSmooth);
        freeTarget(s.flowSmoothAB);
        freeTarget(s.out);
        s.w = s.h = 0;
        s.haveA = s.haveB = false;
    }

    bool allocate(State &s, int w, int h)
    {
        freeAll(s);
        bool good = true;
        for (Frame &f : s.frames)
        {
            f.colour = makeTarget(w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            good = good && f.colour.id;
            for (int l = 1; l <= kLevels; ++l)
            {
                f.luma[l] = makeTarget(levelSize(w, l), levelSize(h, l), PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
                good = good && f.luma[l].id;
            }
        }
        for (int l = kFinestFlow; l <= kLevels; ++l)
        {
            s.flow[l] = makeTarget(levelSize(w, l), levelSize(h, l), PIXELFORMAT_UNCOMPRESSED_R16G16B16A16);
            good = good && s.flow[l].id;
        }
        s.flowRefine = makeTarget(levelSize(w, kFinestFlow), levelSize(h, kFinestFlow), PIXELFORMAT_UNCOMPRESSED_R16G16B16A16);
        s.flowSmooth = makeTarget(levelSize(w, kFinestFlow), levelSize(h, kFinestFlow), PIXELFORMAT_UNCOMPRESSED_R16G16B16A16);
        s.flowSmoothAB = makeTarget(levelSize(w, kFinestFlow), levelSize(h, kFinestFlow), PIXELFORMAT_UNCOMPRESSED_R16G16B16A16);
        s.out = makeTarget(w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        good = good && s.flowRefine.id && s.flowSmooth.id && s.flowSmoothAB.id && s.out.id;
        if (!good)
        {
            std::fprintf(stderr, "[framegen] could not create the graphics buffers (%dx%d): frame generation stays off\n", w, h);
            freeAll(s);
            return false;
        }
        s.w = w;
        s.h = h;
        std::fprintf(stderr, "[framegen] buffers for %dx%d ready\n", w, h);
        return true;
    }

    void setVec2(Shader &sh, const char *name, float x, float y)
    {
        const float v[2] = {x, y};
        SetShaderValue(sh, GetShaderLocation(sh, name), v, SHADER_UNIFORM_VEC2);
    }
    void setInt(Shader &sh, const char *name, int v)
    {
        SetShaderValue(sh, GetShaderLocation(sh, name), &v, SHADER_UNIFORM_INT);
    }
    void setFloat(Shader &sh, const char *name, float v)
    {
        SetShaderValue(sh, GetShaderLocation(sh, name), &v, SHADER_UNIFORM_FLOAT);
    }
    void setTex(Shader &sh, const char *name, const Texture2D &t)
    {
        SetShaderValueTexture(sh, GetShaderLocation(sh, name), t);
    }

    // One pass: `setup` sets the shader's inputs, then a rectangle covers the whole target.
    template <typename Setup>
    void pass(RenderTexture2D &target, Shader &sh, Setup setup)
    {
        BeginTextureMode(target);
        BeginShaderMode(sh);
        setVec2(sh, "outSize", static_cast<float>(target.texture.width), static_cast<float>(target.texture.height));
        setup();
        DrawRectangle(0, 0, target.texture.width, target.texture.height, WHITE);
        EndShaderMode();
        EndTextureMode();
    }

    bool loadShaders(State &s)
    {
        s.copy = LoadShaderFromMemory(nullptr, kCopy);
        s.luma = LoadShaderFromMemory(nullptr, kLuma);
        s.search = LoadShaderFromMemory(nullptr, kSearch);
        s.median = LoadShaderFromMemory(nullptr, kMedian);
        s.interp = LoadShaderFromMemory(nullptr, kInterp);
        const bool good = IsShaderValid(s.copy) && IsShaderValid(s.luma) && IsShaderValid(s.search) &&
                          IsShaderValid(s.median) && IsShaderValid(s.interp);
        if (!good)
            std::fprintf(stderr, "[framegen] shaders unavailable on this graphics driver: frame generation stays off\n");
        return good;
    }

    // The newest picture into B, its pyramid, and the motion from B to A.
    void ingest(State &s, const Texture2D &frame, uint32_t width, uint32_t height)
    {
        std::swap(s.a, s.b);
        Frame &fb = s.frames[s.b];
        const float su = frame.width > 0 ? static_cast<float>(width) / static_cast<float>(frame.width) : 1.0f;
        const float sv = frame.height > 0 ? static_cast<float>(height) / static_cast<float>(frame.height) : 1.0f;
        pass(fb.colour, s.copy, [&] {
            setTex(s.copy, "src", frame);
            setVec2(s.copy, "srcUvScale", su, sv);
        });
        for (int l = 1; l <= kLevels; ++l)
        {
            const Texture2D &src = l == 1 ? fb.colour.texture : fb.luma[l - 1].texture;
            pass(fb.luma[l], s.luma, [&] {
                setTex(s.luma, "src", src);
                setInt(s.luma, "fromColour", l == 1 ? 1 : 0);
            });
        }
        if (!s.haveB)
            return;
        Frame &fa = s.frames[s.a];
        // Motion of every pixel of `to` back to `from` (lumaA = from, lumaB = to in the shader), into `result`.
        auto motion = [&](Frame &from, Frame &to, RenderTexture2D &result) {
            for (int l = kLevels; l >= kFinestFlow; --l)
            {
                const float scale = static_cast<float>(s.w) / static_cast<float>(levelSize(s.w, l));
                pass(s.flow[l], s.search, [&] {
                    setTex(s.search, "lumaA", from.luma[l].texture);
                    setTex(s.search, "lumaB", to.luma[l].texture);
                    if (l < kLevels)
                        setTex(s.search, "coarse", s.flow[l + 1].texture);
                    setInt(s.search, "hasCoarse", l < kLevels ? 1 : 0);
                    setInt(s.search, "radius", l == kLevels ? 3 : (l == kFinestFlow ? 1 : 2));
                    setFloat(s.search, "levelScale", scale);
                    setVec2(s.search, "fullSize", static_cast<float>(s.w), static_cast<float>(s.h));
                });
            }
            // The finest level once more in 1-pixel steps (its own search moved in steps of one texel, 4 pixels): the
            // brightness is sampled between texels, so the motion is found to the pixel.
            const int l = kFinestFlow;
            pass(s.flowRefine, s.search, [&] {
                setTex(s.search, "lumaA", from.luma[l].texture);
                setTex(s.search, "lumaB", to.luma[l].texture);
                setTex(s.search, "coarse", s.flow[l].texture);
                setInt(s.search, "hasCoarse", 1);
                setInt(s.search, "radius", 2);
                setFloat(s.search, "levelScale", 1.0f);
                setVec2(s.search, "fullSize", static_cast<float>(s.w), static_cast<float>(s.h));
            });
            pass(result, s.median, [&] { setTex(s.median, "flow", s.flowRefine.texture); });
        };
        motion(fa, fb, s.flowSmooth);   // B -> A
        motion(fb, fa, s.flowSmoothAB); // A -> B
    }
}

namespace ps2x::framegen
{
    void noteFrame(const uint8_t *rgba, uint32_t width, uint32_t height)
    {
        ++g_serial;
        if (!rgba || width < 16u || height < 16u)
        {
            g_cut = true;
            return;
        }
        // A 48x27 grid of brightness: a big average change between two pictures is a cut.
        constexpr uint32_t gw = 48u, gh = 27u;
        std::vector<float> sig(gw * gh);
        for (uint32_t gy = 0; gy < gh; ++gy)
            for (uint32_t gx = 0; gx < gw; ++gx)
            {
                const uint32_t x = (gx * 2u + 1u) * width / (gw * 2u), y = (gy * 2u + 1u) * height / (gh * 2u);
                const uint8_t *p = rgba + (static_cast<size_t>(y) * width + x) * 4u;
                sig[gy * gw + gx] = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
            }
        float diff = 0.0f;
        if (g_signature.size() == sig.size())
        {
            for (size_t i = 0; i < sig.size(); ++i)
                diff += std::fabs(sig[i] - g_signature[i]);
            diff /= static_cast<float>(sig.size());
        }
        g_cut = g_signature.size() != sig.size() || diff > 28.0f;
        static const bool s_log = std::getenv("PS2X_FRAMEGEN_LOG") != nullptr;
        if (s_log)
            std::fprintf(stderr, "[framegen] picture %llu: change %.1f%s\n", static_cast<unsigned long long>(g_serial), diff,
                         g_cut ? " (cut)" : "");
        g_signature.swap(sig);
    }

    Texture2D present(const Texture2D &frame, uint32_t width, uint32_t height)
    {
        State &s = st();
        const double now = GetTime();
        // Shown-per-second counter for the overlay.
        ++s.shownCount;
        if (now - s.shownSince >= 0.5)
        {
            s.shownRate = s.shownSince > 0.0 ? static_cast<float>(s.shownCount / (now - s.shownSince)) : 0.0f;
            s.shownCount = 0u;
            s.shownSince = now;
        }
        s.activeNow = false;

        const bool wanted = ps2x::settings::getBool("video", "frame_generation");
        // PS2X_FRAMEGEN_FORCE=1 also uses it under 90 Hz (testing, or a 75 Hz screen with 30 fps).
        static const bool s_force = [] { const char *e = std::getenv("PS2X_FRAMEGEN_FORCE"); return e && e[0] == '1'; }();
        const int refresh = GetMonitorRefreshRate(GetCurrentMonitor());
        if (!wanted || (refresh < 90 && !s_force) || width < 16u || height < 16u)
        {
            s.haveA = s.haveB = false;
            s.seen = g_serial;
            return frame;
        }
        if (!s.tried)
        {
            s.tried = true;
            s.ok = loadShaders(s);
        }
        if (!s.ok)
            return frame;
        if (s.w != static_cast<int>(width) || s.h != static_cast<int>(height))
        {
            if (!allocate(s, static_cast<int>(width), static_cast<int>(height)))
            {
                s.ok = false;
                return frame;
            }
        }
        if (g_serial != s.seen)
        {
            // A new real picture. One that does not follow on from the last one seen, or a cut, starts afresh.
            const bool follows = g_serial == s.seen + 1u && !g_cut;
            s.seen = g_serial;
            const bool hadB = s.haveB;
            ingest(s, frame, width, height);
            s.haveA = hadB;
            s.haveB = true;
            s.timeA = s.timeB;
            s.timeB = now;
            s.pairUsable = s.haveA && follows;
        }
        if (!s.haveB)
            return frame;
        const Texture2D &a = s.frames[s.a].colour.texture;
        const Texture2D &b = s.frames[s.b].colour.texture;
        const double period = s.timeB - s.timeA;
        // A gap of more than 1/20 s (a load, a pause) is not motion to smooth.
        if (!s.pairUsable || period <= 0.004 || period > 0.05)
            return b;
        const double t = (now - s.timeB) / period;
        if (t >= 1.0)
            return b; // the next picture is late: hold the newest
        if (t <= 0.02)
            return a;
        pass(s.out, s.interp, [&] {
            setTex(s.interp, "frameA", a);
            setTex(s.interp, "frameB", b);
            setTex(s.interp, "flowBA", s.flowSmooth.texture);
            setTex(s.interp, "flowAB", s.flowSmoothAB.texture);
            setFloat(s.interp, "t", static_cast<float>(t));
        });
        s.activeNow = true;
        return s.out.texture;
    }

    float shownPerSecond()
    {
        return ps2x::settings::getBool("video", "frame_generation") ? st().shownRate : 0.0f;
    }

    bool active()
    {
        return st().activeNow;
    }
}
