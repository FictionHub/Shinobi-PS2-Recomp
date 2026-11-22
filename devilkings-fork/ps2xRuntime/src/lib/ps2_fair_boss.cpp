// LOCAL PATCH (mod): Fair final boss. [game] fair_final_boss (Mod Menu). The hooks live in the runner's copies of
// Hiruko's (CEc380) and the talismans' (CEc225) code, added by scripts/runner_patches.py section 10; this file holds
// their switch and the helpers they call. What each hook changes, and why, is in reference/hiruko_boss.md:
//   1. seals: one counted hit opens each seal phase (KNOCK_BACK's 3 and 7 hit checks),
//   2. no parry of plain sword hits before the real fight (CEc380::Damage),
//   3. no guard against slashes while he stands idle in the real fight (CEc380::Main),
//   4. he only teleports to the four spots on the ground in the real fight (Teleport),
//   5. he can be hit while he laughs (TAUNT), and
//   6. lock-on keeps him while he laughs or fades out to teleport (TAUNT, IDLE2),
//   7. a talisman that blows itself up still counts towards the TATE chain (CEc225 ATTACK02),
//   8. new talismans come twice as fast (CEc380::Main).
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_settings.h"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace
{
    constexpr uint32_t kGetStageCharaBase = 0x236F60u; // GetStageCharaBase__Fv
    constexpr uint32_t kAddEnemyKilled = 0x2D86E0u;    // CStageChara::AddEnemyKilled(CTargetBase *)

    std::atomic<int64_t> g_hirukoSeenMs{INT64_MIN / 2};

    int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
}

bool ps2xFairBoss()
{
    static std::atomic<uint64_t> s_version{~0ull};
    static std::atomic<bool> s_on{false};
    const uint64_t version = ps2x::settings::version();
    if (s_version.load(std::memory_order_relaxed) != version)
    {
        s_on.store(ps2x::settings::getBool("game", "fair_final_boss"), std::memory_order_relaxed);
        s_version.store(version, std::memory_order_relaxed);
    }
    return s_on.load(std::memory_order_relaxed);
}

// CEc380::Main calls this every frame: Hiruko is on the stage.
void ps2xHirukoMark()
{
    g_hirukoSeenMs.store(nowMs(), std::memory_order_relaxed);
}

static bool hirukoActive()
{
    return nowMs() - g_hirukoSeenMs.load(std::memory_order_relaxed) < 1000;
}

// CEc225's self-destruct, just after its blast is made: count the talisman as killed, as the player's own hit would
// (CPlayer::CollisionCallHit -> CStageChara::AddEnemyKilled). Only while Hiruko is fighting, so the talismans' other
// stages are unchanged. The game's registers are put back afterwards; only what AddEnemyKilled writes stays.
void ps2xFairBossTalismanBlast(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t talisman)
{
    if (!ps2xFairBoss() || !hirukoActive() || !talisman || !runtime)
        return;
    if (!runtime->hasFunction(kGetStageCharaBase) || !runtime->hasFunction(kAddEnemyKilled))
        return;
    const R5900Context saved = *ctx;
    SET_GPR_U32(ctx, 31, saved.pc);
    runtime->lookupFunction(kGetStageCharaBase)(rdram, ctx, runtime);
    const uint32_t stageChara = GPR_U32(ctx, 2);
    if (stageChara)
    {
        SET_GPR_U32(ctx, 4, stageChara);
        SET_GPR_U32(ctx, 5, talisman);
        SET_GPR_U32(ctx, 31, saved.pc);
        runtime->lookupFunction(kAddEnemyKilled)(rdram, ctx, runtime);
        static int s_logged = 0;
        if (s_logged++ < 10)
            std::fprintf(stderr, "[fairboss] talisman 0x%08x blew itself up: counted as a kill (%u)\n", talisman,
                         GPR_U32(ctx, 2));
    }
    *ctx = saved;
}

// LOCAL PATCH (mod): the final boss's health. CEc380 KNOCK_BACK (0x319540) starts the real fight (phase 2): it creates
// the boss gauge with Hiruko's health at that moment (+0x32C) as the gauge's full length. Just before, the health is set
// to [game] final_boss_health percent of it (100 = the original), so the gauge starts full and drains in step.
void ps2xFinalBossHealth(uint8_t *rdram, uint32_t boss)
{
    const int percent = std::clamp(ps2x::settings::getNumber("game", "final_boss_health"), 10, 100);
    if (percent >= 100 || boss == 0u)
        return;
    const uint32_t p = (boss & 0x01FFFFFFu) + 0x32Cu;
    int32_t hp = 0;
    std::memcpy(&hp, rdram + p, 4u);
    if (hp <= 1)
        return;
    const int32_t reduced = std::max<int32_t>(1, static_cast<int32_t>(static_cast<int64_t>(hp) * percent / 100));
    std::memcpy(rdram + p, &reduced, 4u);
    std::fprintf(stderr, "[fair-boss] final boss health %d -> %d (%d %%)\n", hp, reduced, percent);
}
