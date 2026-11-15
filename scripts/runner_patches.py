"""Patches applied to the generated runner (devilkings-fork/ps2xRuntime/src/runner, not in the repository) after the
recompiler has written it. Safe to run more than once.

1. Resume points the recompiler left out (GitHub issue #6). A fragment (entry_XXXX) that the EE scheduler resumes in can
   run off its own end into code it does not contain: it sets ctx->pc to the next address P and returns, and the
   dispatcher has no function for P ("[guest-branch:missing-target]"), so the game stops dead. Options > Screen Position
   moved left or down did exactly that (SJIS_njVertex, 0x33f208). The code at P is in the parent function, so: a label at
   P there, a case for it in the parent's dispatch switch, and the parent registered for P in register_functions.cpp.

2. HUD mod: the game's HUD draw functions return straight away while ps2xHudHidden() (ps2_hud_mod.cpp) is true.

usage: python scripts/runner_patches.py [runner directory]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'devilkings-fork', 'ps2xRuntime', 'src', 'runner')
REG = os.path.join(D, 'register_functions.cpp')


def read(path):
    with open(path, encoding='utf-8', errors='replace', newline='') as h:
        return h.read()


def write(path, text, nl):
    with open(path, 'w', encoding='utf-8', newline='') as h:
        h.write(text.replace('\n', nl))


# ---- 1. resume points --------------------------------------------------------------------------------------------
reg_raw = read(REG)
reg_nl = '\r\n' if '\r\n' in reg_raw else '\n'
reg = reg_raw.replace('\r\n', '\n')
base = int(re.search(r'g_ps2RecompiledFunctionTableBase = 0x([0-9a-f]+)u', reg).group(1), 16)
mapped = {int(m.group(2), 16) for m in re.finditer(r'= (\w+); // 0x([0-9a-f]+)', reg)}

funcs = []
dead = []
for f in os.listdir(D):
    if not f.endswith('.cpp') or f == 'register_functions.cpp':
        continue
    s = read(os.path.join(D, f)).replace('\r\n', '\n')
    r = re.search(r'// Address: 0x([0-9a-f]+) - 0x([0-9a-f]+)', s)
    if not r:
        continue
    start, end = int(r.group(1), 16), int(r.group(2), 16)
    funcs.append((start, end, f))
    t = re.search(r'\n    ctx->pc = 0x([0-9A-Fa-f]+)u;\n\}\s*$', s)
    if t:
        pc = int(t.group(1), 16)
        if pc > end and pc not in mapped:
            dead.append((f, pc))

added = []
for frag, P in dead:
    for start, end, f in sorted((x for x in funcs if x[0] < P <= x[1] and x[2] != frag), key=lambda x: x[0] - x[1]):
        path = os.path.join(D, f)
        raw = read(path)
        nl = '\r\n' if '\r\n' in raw else '\n'
        t = raw.replace('\r\n', '\n')
        i = t.find('\n    ctx->pc = 0x%Xu;\n    // 0x%x:' % (P, P))
        if i < 0:
            continue  # not at the top level of this function (a delay slot), or not here
        label = 'label_%x' % P
        if label + ':' not in t:
            t = t[:i + 1] + label + ':\n' + t[i + 1:]
        case = '        case 0x%xu: goto %s;\n' % (P, label)
        sw = re.search(r'\n    switch \(ctx->pc\) \{\n', t)
        if sw:
            if case not in t:
                t = t[:sw.end()] + case + t[sw.end():]
        else:
            k = t.find('#endif\n', t.find('void ' + f[:-4] + '('))
            if k < 0:
                continue
            k += len('#endif\n')
            t = t[:k] + '\n    switch (ctx->pc) {\n' + case + '        default: break;\n    }\n' + t[k:]
        write(path, t, nl)
        added.append((P, f[:-4]))
        break

if added:
    marker = '    }\n};\nstatic const GeneratedFunctionTableInitializer'
    assert reg.count(marker) == 1
    lines = ''.join('        g_ps2RecompiledFunctionTable[%du] = %s; // 0x%x LOCAL PATCH: resume point the recompiler left out\n'
                    % ((P - base) // 4, fn, P) for P, fn in added)
    write(REG, reg.replace(marker, lines + marker), reg_nl)
print('resume points added:', len(added), '(of', len(dead), 'fragments that end outside themselves)')

# ---- 2. HUD mod ----------------------------------------------------------------------------------------------------
HUD = [
    'Draw__14DrawPlayerInfoFv_0x3daac0', 'Draw__10DrawHPInfoFv_0x2f4fd0', 'Draw__12DrawTateInfoFv_0x2f3c80',
    'Draw__13DrawKunaiInfoFv_0x2f6de0', 'Draw__13DrawSLASHInfoFv_0x2f7930', 'Draw__12DrawMakimonoFv_0x3d90a0',
    'Draw__13DrawOboroCoinFv_0x3d6c80', 'Draw__9DrawGaugeFv_0x3d8310', 'Draw__13DrawBossGaugeFv_0x3d7b30',
    'Draw__12DrawDecGaugeFv_0x3d8820', 'DrawBossGauge02__FP6MlTaski_0x3d7cc0', 'Draw__7DrainHPFv_0x2f6600',
    'Draw__12VanishEffectFv_0x3db870', 'Draw__13VanishEffectsFv_0x3dba70',
]
# The scarf (Remove scarf mod): its own switch, ps2xScarfHidden().
SCARF = 'Draw__14CPlayerMufflerFv_0x2b8e60'
HOOK = ('    // LOCAL PATCH (HUD mod): with the HUD hidden (F5, [game] hide_hud) this HUD piece is not drawn.\n'
        '    {\n'
        '        extern bool ps2xHudHidden();\n'
        '        if (ps2xHudHidden())\n'
        '        {\n'
        '            ctx->pc = GPR_U32(ctx, 31);\n'
        '            return;\n'
        '        }\n'
        '    }\n')
hooked = 0
for name in HUD:
    path = os.path.join(D, name + '.cpp')
    raw = read(path)
    if 'ps2xHudHidden' in raw:
        continue
    nl = '\r\n' if '\r\n' in raw else '\n'
    t = raw.replace('\r\n', '\n')
    m = re.search(r'\n    ctx->pc = 0x0*%su;\n' % name.rsplit('_0x', 1)[1], t, re.I)
    if not m:
        print('HUD: no entry found in', name)
        continue
    write(path, t[:m.end()] + HOOK + t[m.end():], nl)
    hooked += 1
print('HUD draw functions hooked:', hooked, 'of', len(HUD))

path = os.path.join(D, SCARF + '.cpp')
raw = read(path)
if 'ps2xScarfHidden' not in raw:
    nl = '\r\n' if '\r\n' in raw else '\n'
    t = raw.replace('\r\n', '\n')
    m = re.search(r'\n    ctx->pc = 0x0*2b8e60u;\n', t, re.I)
    if m:
        t = t[:m.end()] + HOOK.replace('ps2xHudHidden', 'ps2xScarfHidden').replace(
            'with the HUD hidden (F5, [game] hide_hud) this HUD piece is not drawn', 'Remove scarf mod: the scarf is not drawn') + t[m.end():]
        write(path, t, nl)
        print('scarf draw hooked')

# ---- 3. Cinematic bars mod: EventAction::Draw's two band draws (EasyDrawPol2DZ at 0x307f58 and 0x307f9c) skipped while
# ps2xCinemaBarsHidden() (ps2_hud_mod.cpp). Skipping a call is the same as the call returning at once.
path = os.path.join(D, 'Draw__11EventActionFv_0x307ec0.cpp')
raw = read(path)
nl = '\r\n' if '\r\n' in raw else '\n'
t = raw.replace('\r\n', '\n')
bars = 0
if 'ps2xCinemaBarsHidden' not in t:
    for site, ret in (('0x307F58u', '0x307F60u'), ('0x307F9Cu', '0x307FA4u')):
        old = ('    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x247740u, %s, %s, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {'
               % (site, ret))
        new = ('    if (!ps2xCinemaBarsHidden() && !runtime->dispatchGuestBranch(rdram, ctx, 0x247740u, %s, %s, '
               'PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {' % (site, ret))
        if t.count(old) == 1:
            t = t.replace(old, new)
            bars += 1
    if bars:
        k = t.find('void Draw__11EventActionFv_0x307ec0(')
        t = t[:k] + 'bool ps2xCinemaBarsHidden(); // ps2_hud_mod.cpp (LOCAL PATCH: cinematic bars mod)\n' + t[k:]
        write(path, t, nl)
print('cinematic bar draws made optional:', bars, '(0 = already done)')

# ---- 4. Scarf body collision: after CPlayerMuffler::Main's own update (just before its call to ResetWind at 0x2b8df0),
# ps2xScarfCollide (ps2_scarf_collide.cpp) with the scarf object ($fp).
path = os.path.join(D, 'Main__14CPlayerMufflerFv_0x2b8a50.cpp')
raw = read(path)
nl = '\r\n' if '\r\n' in raw else '\n'
t = raw.replace('\r\n', '\n')
done = 0
if 'ps2xScarfCollide' not in t:
    old = 'label_2b8df0:\n'
    if t.count(old) == 1:
        t = t.replace(old, old + '''    // LOCAL PATCH (mod): the scarf slides around the body instead of through it (ps2_scarf_collide.cpp).
    {
        extern void ps2xScarfCollide(uint8_t *rdram, uint32_t muffler);
        ps2xScarfCollide(rdram, GPR_U32(ctx, 30));
    }
''')
        write(path, t, nl)
        done = 1
print('scarf collision hook added:', done, '(0 = already done)')

# ---- 5. Cinematic bars mod, second source: a scene can narrow HyScreen's clipping rectangle (+0x10..+0x1C, applied as
# the GS scissor by HyScreen::_settingToEnvironment, 0x3322c0) to a letterbox - the picture is then simply not drawn
# above and below. While ps2xCinemaBarsHidden() the per-frame copy of that rectangle is widened to the whole screen
# (HyScreen::_clippingRect then clamps it to the real size); the game's own value is left as it is.
path = os.path.join(D, '_settingToEnvironment__8HyScreenFv_0x3322c0.cpp')
raw = read(path)
done = 0
if 'ps2xCinemaBarsHidden' not in raw:
    nl = '\r\n' if '\r\n' in raw else '\n'
    t = raw.replace('\r\n', '\n')
    old = '    ctx->in_delay_slot = false;\n    ctx->pc = 0x332200u;\n'
    if t.count(old) == 1:
        t = t.replace(old, '''    ctx->in_delay_slot = false;
    // LOCAL PATCH (cinematic bars mod): the clipping rectangle's copy (sp+0x50..0x5C) to the whole screen.
    if (ps2xCinemaBarsHidden())
    {
        const uint32_t sp = GPR_U32(ctx, 29);
        const float full[4] = {0.0f, 0.0f, 100000.0f, 100000.0f};
        for (uint32_t k = 0; k < 4u; ++k)
        {
            uint32_t bits;
            std::memcpy(&bits, &full[k], sizeof(bits));
            WRITE32(ADD32(sp, 0x50u + 4u * k), bits);
        }
    }
    ctx->pc = 0x332200u;
''')
        k = t.find('void _settingToEnvironment__8HyScreenFv_0x3322c0(')
        t = t[:k] + 'bool ps2xCinemaBarsHidden(); // ps2_hud_mod.cpp (LOCAL PATCH: cinematic bars mod)\n' + t[k:]
        write(path, t, nl)
        done = 1
print('cinematic bars (screen clipping) hook added:', done, '(0 = already done)')

# ---- 6. No sword hunger mod: CPlayer::MainControlMeters reads CheckSwordWake's answer at 0x24ad00 to decide whether the
# sword gauge, then the health, is drained; while ps2xSwordHungerOff() that answer is read as "asleep" there.
path = os.path.join(D, 'MainControlMeters__7CPlayerFv_0x24acc0.cpp')
raw = read(path)
done = 0
if 'ps2xSwordHungerOff' not in raw:
    nl = '\r\n' if '\r\n' in raw else '\n'
    t = raw.replace('\r\n', '\n')
    old = 'label_24ad00:\n'
    if t.count(old) == 1:
        t = t.replace(old, old + '    if (ps2xSwordHungerOff()) // LOCAL PATCH (mod): no sword hunger\n        SET_GPR_U64(ctx, 2, 0);\n')
        k = t.find('void MainControlMeters__7CPlayerFv_0x24acc0(')
        t = t[:k] + 'bool ps2xSwordHungerOff(); // ps2_hud_mod.cpp (LOCAL PATCH)\n' + t[k:]
        write(path, t, nl)
        done = 1
print('no sword hunger hook added:', done, '(0 = already done)')

# ---- 7. (research) HyScreen settings logged when they change (ps2xScreenTrace in ps2_hud_mod.cpp), at the start of
# HyScreen::_settingToEnvironment.
path = os.path.join(D, '_settingToEnvironment__8HyScreenFv_0x3322c0.cpp')
raw = read(path)
if 'ps2xScreenTrace' not in raw:
    nl = '\r\n' if '\r\n' in raw else '\n'
    t = raw.replace('\r\n', '\n')
    m = re.search(r'\n    ctx->pc = 0x3322C0u;\n', t)
    if m:
        t = t[:m.end()] + '    { extern void ps2xScreenTrace(const uint8_t *, uint32_t); ps2xScreenTrace(rdram, GPR_U32(ctx, 4)); }\n' + t[m.end():]
        write(path, t, nl)
        print('screen trace hook added')

# ---- 8. Cinematic bars mod: movies and story cutscenes keep their bars (the picture never covers them, so removing
# them shows leftover memory under the subtitles). Their Draw functions mark "keep bars" (ps2xKeepBarsMark in
# ps2_hud_mod.cpp); the GS-level filter (gs_bars.cpp) leaves bars alone while that mark is recent.
for name, pc in (('Draw__9movieTaskFv_0x242280.cpp', '0x242280u'), ('Draw__10TitleMovieFv_0x2416a0.cpp', '0x2416A0u'),
                 ('Draw__12cEventPlayerFv_0x2ffce0.cpp', '0x2FFCE0u')):
    path = os.path.join(D, name)
    raw = read(path)
    if 'ps2xKeepBarsMark' in raw:
        continue
    nl = '\r\n' if '\r\n' in raw else '\n'
    t = raw.replace('\r\n', '\n')
    m = re.search(r'\n    ctx->pc = ' + pc + r';\n', t, re.I)
    if m:
        t = t[:m.end()] + '    { extern void ps2xKeepBarsMark(); ps2xKeepBarsMark(); } // LOCAL PATCH (cinematic bars mod)\n' + t[m.end():]
        write(path, t, nl)
        print('keep-bars mark added:', name)
    else:
        print('keep-bars mark NOT added (pattern missing):', name)

# ---- 9. Cinematic bars mod: the TATE camera's bars are Ml_TexScreen / Ml_TexScreenPT effects; their Draw returns at
# once while ps2xTexScreenIsBar (ps2_hud_mod.cpp) says the effect is a full-width strip and the mod is on.
for name, pc in (('Draw__12Ml_TexScreenFv_0x3b1650.cpp', '3B1650'), ('Draw__14Ml_TexScreenPTFv_0x3b8d30.cpp', '3B8D30')):
    path = os.path.join(D, name)
    raw = read(path)
    if 'ps2xTexScreenIsBar' in raw:
        continue
    nl = '\r\n' if '\r\n' in raw else '\n'
    t = raw.replace('\r\n', '\n')
    m = re.search(r'\n    ctx->pc = 0x' + pc + r'u;\n', t, re.I)
    if not m:
        print('TexScreen hook NOT added (pattern missing):', name)
        continue
    hook = ('    // LOCAL PATCH (cinematic bars mod): a TATE bar is not drawn.\n'
            '    {\n'
            '        extern bool ps2xTexScreenIsBar(const uint8_t *, uint32_t);\n'
            '        if (ps2xTexScreenIsBar(rdram, GPR_U32(ctx, 4)))\n'
            '        {\n'
            '            ctx->pc = GPR_U32(ctx, 31);\n'
            '            return;\n'
            '        }\n'
            '    }\n')
    write(path, t[:m.end()] + hook + t[m.end():], nl)
    print('TexScreen hook added:', name)

# ---- 10. Fair final boss mod (ps2_fair_boss.cpp; how the fight works: reference/hiruko_boss.md). Each hook is a line of
# code put just before one instruction of Hiruko's (CEc380) or the talismans' (CEc225) code and does nothing while
# [game] fair_final_boss is off.
FAIR_DECL = 'bool ps2xFairBoss(); // ps2_fair_boss.cpp (LOCAL PATCH: fair final boss mod)\n'


def fair_hook(fname, before_pc, code, marker):
    """Put `code` just before the instruction at `before_pc` (after its label, so jumps to it run it too)."""
    path = os.path.join(D, fname)
    raw = read(path)
    if marker in raw:
        return 0
    nl = '\r\n' if '\r\n' in raw else '\n'
    t = raw.replace('\r\n', '\n')
    anchor = '\n    // 0x%x: ' % before_pc
    if t.count(anchor) != 1:
        print('fair boss: anchor 0x%x not found once in %s (%d)' % (before_pc, fname, t.count(anchor)))
        return 0
    k = t.index(anchor) + 1
    t = t[:k] + code + t[k:]
    if FAIR_DECL not in t:
        f = t.find('\nvoid ')
        t = t[:f + 1] + FAIR_DECL + t[f + 1:]
    write(path, t, nl)
    return 1


def fair_call_arg(fname, target, call_pc, code, marker):
    """Put `code` just before the call (jal) at `call_pc` to `target` is made (its delay slot has already run)."""
    path = os.path.join(D, fname)
    raw = read(path)
    if marker in raw:
        return 0
    nl = '\r\n' if '\r\n' in raw else '\n'
    t = raw.replace('\r\n', '\n')
    anchor = '    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x%Xu, 0x%Xu, ' % (target, call_pc)
    if t.count(anchor) != 1:
        print('fair boss: call 0x%x at 0x%x not found once in %s (%d)' % (target, call_pc, fname, t.count(anchor)))
        return 0
    k = t.index(anchor)
    t = t[:k] + code + t[k:]
    if FAIR_DECL not in t:
        f = t.find('\nvoid ')
        t = t[:f + 1] + FAIR_DECL + t[f + 1:]
    write(path, t, nl)
    return 1


KB = 'KNOCK_BACK__15CEc380AnimationFii_0x319540.cpp'
DMG = 'Damage__6CEc380FP7IzPointP10SPowerData_0x317fc0.cpp'
MAIN = 'Main__6CEc380Fv_0x3178b0.cpp'
TP = 'Teleport__15CEc380AnimationFv_0x319c10.cpp'
TAUNT = 'TAUNT__15CEc380AnimationFii_0x318240.cpp'
IDLE2 = 'IDLE2__15CEc380AnimationFii_0x318570.cpp'
TALISMAN = 'ATTACK02__15CEc225AnimationFii_0x3ccc80.cpp'
SET_LOCK_OK, SET_DAMAGE_OK = 0x25B360, 0x252870
n = 0
# 1. Seals: the hit counter (+0xC6) jumps to the count that opens the seal (3, then 7) on the first counted hit.
n += fair_hook(KB, 0x319630,
               '    if (ps2xFairBoss()) { SET_GPR_U32(ctx, 2, 3u); WRITE16(ADD32(GPR_U32(ctx, 17), 198), 3u); } // LOCAL PATCH (fair boss 1a)\n',
               'fair boss 1a')
n += fair_hook(KB, 0x3197d0,
               '    if (ps2xFairBoss()) { SET_GPR_U32(ctx, 2, 7u); WRITE16(ADD32(GPR_U32(ctx, 17), 198), 7u); } // LOCAL PATCH (fair boss 1b)\n',
               'fair boss 1b')
# 2. No parry before the real fight: the hit's attack type (SPowerData+0xD) reads as a guard-breaking one.
n += fair_hook(DMG, 0x317fdc,
               '    if (ps2xFairBoss()) SET_GPR_U32(ctx, 2, 1u); // LOCAL PATCH (fair boss 2)\n',
               'fair boss 2')
# 3. No guard against slashes while idle in the real fight: his guard-break level (+0x515) reads as broken.
n += fair_hook(MAIN, 0x317e1c,
               '    if (ps2xFairBoss()) SET_GPR_U32(ctx, 3, 2u); // LOCAL PATCH (fair boss 3)\n',
               'fair boss 3')
# 8. Talismans come back twice as fast (the respawn timer's limit, performance +0xAC, halved).
n += fair_hook(MAIN, 0x317db4,
               '    if (ps2xFairBoss()) SET_GPR_U32(ctx, 3, GPR_U32(ctx, 3) / 2u); // LOCAL PATCH (fair boss 8)\n',
               'fair boss 8')
# Hiruko is on the stage (for 7): marked every frame from his Main.
n += fair_hook(MAIN, 0x3178b0,
               '    { extern void ps2xHirukoMark(); ps2xHirukoMark(); } // LOCAL PATCH (fair boss: Hiruko seen)\n',
               'fair boss: Hiruko seen')
# 4. Ground spots only in the real fight: the random pick's upper end (19) becomes 13 (spots 10-13 stand on the floor).
n += fair_hook(TP, 0x319c74,
               '    if (ps2xFairBoss() && GPR_U32(ctx, 5) == 19u) SET_GPR_U32(ctx, 5, 13u); // LOCAL PATCH (fair boss 4)\n',
               'fair boss 4')
# 5 + 6. While he laughs: hittable and lockable. While he fades out to teleport: still lockable (damage stays off).
for pc in (0x3182AC, 0x318378):
    n += fair_call_arg(TAUNT, SET_LOCK_OK, pc,
                       '    if (ps2xFairBoss()) SET_GPR_U32(ctx, 5, 1u); // LOCAL PATCH (fair boss 6 @%x)\n' % pc,
                       'fair boss 6 @%x' % pc)
for pc in (0x3182B8, 0x318384):
    n += fair_call_arg(TAUNT, SET_DAMAGE_OK, pc,
                       '    if (ps2xFairBoss()) SET_GPR_U32(ctx, 5, 1u); // LOCAL PATCH (fair boss 5 @%x)\n' % pc,
                       'fair boss 5 @%x' % pc)
n += fair_call_arg(IDLE2, SET_LOCK_OK, 0x3185D8,
                   '    if (ps2xFairBoss()) SET_GPR_U32(ctx, 5, 1u); // LOCAL PATCH (fair boss 6 @3185d8)\n',
                   'fair boss 6 @3185d8')
# 7. A talisman that blows itself up counts as killed: just after its blast is made (before action 0x25 is set).
n += fair_hook(TALISMAN, 0x3ccda4,
               '    { extern void ps2xFairBossTalismanBlast(uint8_t *, R5900Context *, PS2Runtime *, uint32_t);\n'
               '      ps2xFairBossTalismanBlast(rdram, ctx, runtime, READ32(ADD32(GPR_U32(ctx, 16), 168))); } // LOCAL PATCH (fair boss 7)\n',
               'fair boss 7')
# 9. Hittable while he fades out and back in to teleport (IDLE2's SetDamageOk(0) / SetHitOk(0) become 1).
for pc, target in ((0x3185E4, SET_DAMAGE_OK), (0x3185F0, 0x252880)):
    n += fair_call_arg(IDLE2, target, pc,
                       '    if (ps2xFairBoss()) SET_GPR_U32(ctx, 5, 1u); // LOCAL PATCH (fair boss 9 @%x)\n' % pc,
                       'fair boss 9 @%x' % pc)
# 9b. A hit during the fade ends IDLE2 early, so its own clean-up (constant material off, alpha 1) never runs: do it
# when the hit lands, or he stays see-through. CObjectBase +0x410 constant material, +0x414 its alpha flag, +0x420 alpha.
n += fair_hook(DMG, 0x317fc0,
               '    if (ps2xFairBoss() && READ32(ADD32(GPR_U32(ctx, 4), 0x410))) { WRITE32(ADD32(GPR_U32(ctx, 4), 0x410), 0u);'
               ' WRITE32(ADD32(GPR_U32(ctx, 4), 0x414), 0u); WRITE32(ADD32(GPR_U32(ctx, 4), 0x420), 0x3F800000u); }'
               ' // LOCAL PATCH (fair boss 9b)\n',
               'fair boss 9b')
print('fair final boss hooks added:', n, '(0 = already done; 16 on a fresh runner)')

# ---- 11. Infinite health mod (ps2_hud_mod.cpp ps2xInfiniteHealth, [game] infinite_health). CPlayer::Damage (0x24e010)
# subtracts the hit's damage from the health (+0x32C) from 0x24e1c8 on. Just before that, while the mod is on, it ends the
# way it does for a hit the game ignores (label 0x24e588: returns 3 = "no effect"), so no hit costs health. The floor
# damage and the other hit routes reach this same function through its virtual call.
DMG_PLAYER = 'Damage__7CPlayerFP7IzPointP10SPowerData_0x24e010.cpp'
n = fair_hook(DMG_PLAYER, 0x24E1A4,
              '    { extern bool ps2xInfiniteHealth(); if (ps2xInfiniteHealth()) goto label_24e588; } // LOCAL PATCH (infinite health)'+chr(10),
              'LOCAL PATCH (infinite health)')
print('infinite health hook added:', n, '(0 = already done)')

# ---- 12. Jukebox (ps2_jukebox.cpp): which music track each ADX player starts. ADXT_StartFname(adxt, fname) starts a
# named file (the BGMxx_yy_zz?.ADX music); the other starts (AFS index, memory, stream joint) play something else on
# that player, so its replacement ends. The player's RNA output object (+0xC) is the stream ps2_cri_rna.cpp plays.
JUKE = 'extern void ps2xJukeboxStart(uint8_t *rdram, uint32_t adxt, uint32_t fname); // ps2_jukebox.cpp (LOCAL PATCH)'
n = 0
n += fair_hook('ADXT_StartFname_0x26fe48.cpp', 0x26FE48,
               '    { ' + JUKE + chr(10) + '      ps2xJukeboxStart(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5)); } // LOCAL PATCH (jukebox fname)' + chr(10),
               'LOCAL PATCH (jukebox fname)')
for fname, pc in (('ADXT_StartAfs_0x26fd70.cpp', 0x26FD70), ('ADXT_StartSj_0x26ec88.cpp', 0x26EC88)):
    n += fair_hook(fname, pc,
                   '    { ' + JUKE + chr(10) + '      ps2xJukeboxStart(rdram, GPR_U32(ctx, 4), 0u); } // LOCAL PATCH (jukebox other)' + chr(10),
                   'LOCAL PATCH (jukebox other)')
print('jukebox hooks added:', n, '(0 = already done; 3 on a fresh runner)')

# ---- 13. Languages (ps2_lang_overlay.cpp): a story movie's audio track. HySfdPlayer::setSoundChannel(this, ch) passes
# ch (0 Japanese / 1 English on the US movies) to mwPlySetAudioCh; with another language chosen, the European movies
# are played and their track for that language is asked for instead (0 English, 1 French, 2 German, 3 Italian, 4 Spanish).
n = fair_hook('setSoundChannel__11HySfdPlayerFUc_0x243190.cpp', 0x243190,
              '    { extern int ps2xLangMovieChannel(int requested); // ps2_lang_overlay.cpp (LOCAL PATCH)' + chr(10) +
              '      SET_GPR_U32(ctx, 5, (uint32_t)ps2xLangMovieChannel((int)(GPR_U32(ctx, 5) & 0xFFu))); } // LOCAL PATCH (languages movie audio)' + chr(10),
              'LOCAL PATCH (languages movie audio)')
print('languages movie audio hook added:', n, '(0 = already done)')

# ---- 14. 1:1 mouse aiming (ps2_runtime.cpp ps2xMouseAimX / Y). The camera reads the right stick with PadGetAnalogX2 /
# PadGetAnalogY2 and turns value / 128 x 768 angle units a frame; just after each read, the mouse movement since the last
# frame is added to the value (no smoothing, no top speed). _MovePad reads both; _MovePadOtherFollow (the lock-on camera)
# reads the vertical one only.
MOVEPAD = 'ps2__MovePad__13CCameraManageFP10CameraBasei_0x2e6640.cpp'
OTHERFOLLOW = 'ps2__MovePadOtherFollow__13CCameraManageFP10CameraBasei_0x2e6910.cpp'
AIMX = '    { extern int ps2xMouseAimX(int stick); SET_GPR_S32(ctx, 2, ps2xMouseAimX((int32_t)GPR_U32(ctx, 2))); } // LOCAL PATCH (mouse aim x)' + chr(10)
AIMY = '    { extern int ps2xMouseAimY(int stick); SET_GPR_S32(ctx, 2, ps2xMouseAimY((int32_t)GPR_U32(ctx, 2))); } // LOCAL PATCH (mouse aim y)' + chr(10)
n = 0
n += fair_hook(MOVEPAD, 0x2E66D8, AIMX, 'LOCAL PATCH (mouse aim x)')
n += fair_hook(MOVEPAD, 0x2E670C, AIMY, 'LOCAL PATCH (mouse aim y)')
n += fair_hook(OTHERFOLLOW, 0x2E69A0, AIMY, 'LOCAL PATCH (mouse aim y)')
print('mouse aim hooks added:', n, '(0 = already done; 3 on a fresh runner)')

