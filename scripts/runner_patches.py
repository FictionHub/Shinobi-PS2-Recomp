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
