"""Builds the launcher page from the Claude Design file.

usage: python build_launcher_page.py [design.html] [box scan .jpg] [out.html]

Takes the design's bundled page as it is (styles, fonts, images, the Component logic) and changes only what is needed for
it to drive the real game through the WebView2 host (ShinobiLauncher.exe):
  - the page fills the launcher window (the design shows it on a 1100x720 stage);
  - title bar: drag, minimise, close go to the host;
  - settings, key bindings and mods load from / save to the host (game/settings.ini, launcher.ini);
  - Play starts the real game, Stop Game ends it, the status chip shows the real state;
  - options this port has: renderer D3D11/Vulkan, Edge AA instead of MSAA, widescreen 16:9/21:9/32:9/Auto chips, mods
    that exist (lock to 30 fps, fast VU maths, fullscreen, record diagnostics, legacy sound chip);
  - the four guide pictures (cropped from the box scan's back cover) are packed in, and the credits name PCSX2.
Rerun it after changing the design.
"""
import base64
import io
import json
import os
import re
import sys
import uuid

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
design = sys.argv[1] if len(sys.argv) > 1 else os.path.join(r'X:\HAPPYNESS\shinobi_port\Launcher Claude Design',
                                                             'Shinobi PS2 Recom Launcher \u2013 Launcher.html')
box_scan = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.environ['USERPROFILE'], 'Pictures', 'Shinobi - Box.jpg')
out_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, 'launcher_assets', 'launcher.html')

page = open(design, encoding='utf-8').read()


def get_block(name):
    m = re.search(r'(<script type="__bundler/%s">\s*)(.*?)(\s*</script>)' % re.escape(name), page, re.S)
    if not m:
        raise SystemExit('design file has no __bundler/%s block' % name)
    return m


def set_block(name, text):
    global page
    m = get_block(name)
    page = page[:m.start(2)] + text + page[m.end(2):]


manifest = json.loads(get_block('manifest').group(2))
template = json.loads(get_block('template').group(2))

# The SEGA and PlayStation logo pictures are not used (always): their names stay on the page as plain text.
_b = io.BytesIO()
Image.new('RGBA', (2, 2), (0, 0, 0, 0)).save(_b, 'PNG')
_blank_logo = base64.b64encode(_b.getvalue()).decode()
for _uid in ('2e2c0ffd-b51c-4997-b617-7a8681613443', 'a6a19fc5-2af0-4019-98aa-627e71c90e00'):
    if _uid in manifest:
        manifest[_uid] = {'mime': 'image/png', 'compressed': False, 'data': _blank_logo}
template = re.sub(r'<img src="[^"]+" alt="PlayStation" height="30">\s*<img src="[^"]+" alt="SEGA" height="22">',
                  '<span class="mark">Shinobi &copy; SEGA<br>PlayStation is a trademark of Sony</span>', template, count=1)
assert template.count('@media (prefers-reduced-motion:reduce)') == 1
template = template.replace('@media (prefers-reduced-motion:reduce)',
                            '.mark{font-family:Rajdhani,sans-serif;font-size:11px;font-weight:700;line-height:1.25;color:#3b3b42;letter-spacing:.02em}'
                            '@media (prefers-reduced-motion:reduce)')

# SHINOBI_PUBLIC_PAGE=1: the page for the public release. The key art, the game's title logo, the SEGA and PlayStation logos,
# the four guide pictures (cut from the box) and the game's back-of-box text are replaced by plain pictures and text made here.
PUBLIC = os.environ.get('SHINOBI_PUBLIC_PAGE') == '1'


def _font(size):
    from PIL import ImageFont
    for name in ('impact.ttf', 'arialbd.ttf', 'segoeuib.ttf', 'arial.ttf'):
        try:
            return ImageFont.truetype(os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts', name), size)
        except OSError:
            continue
    return ImageFont.load_default()


def _png(img):
    buf = io.BytesIO()
    img.save(buf, 'PNG', optimize=True)
    return buf.getvalue()


def _jpg(img):
    buf = io.BytesIO()
    img.convert('RGB').save(buf, 'JPEG', quality=88)
    return buf.getvalue()


def _gradient(w, h, top, bottom):
    from PIL import ImageDraw
    img = Image.new('RGB', (w, h), top)
    d = ImageDraw.Draw(img)
    for y in range(h):
        k = y / max(h - 1, 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * k) for i in range(3)))
    return img


def _public_assets():
    from PIL import ImageDraw
    out = {}
    # hero: dark crimson, a few soft diagonal bands
    hero = _gradient(1200, 1500, (74, 12, 30), (12, 10, 16))
    d = ImageDraw.Draw(hero, 'RGBA')
    for i, a in enumerate((26, 18, 12)):
        x = 140 + i * 330
        d.polygon([(x, 0), (x + 150, 0), (x - 380, 1500), (x - 530, 1500)], fill=(179, 18, 42, a))
    out['10424391-f9dc-401f-80f1-60c74c105824'] = ('image/jpeg', _jpg(hero))
    # logo: plain text title (transparent png)
    logo = Image.new('RGBA', (900, 300), (0, 0, 0, 0))
    d = ImageDraw.Draw(logo)
    f1, f2 = _font(190), _font(58)
    d.text((450, 120), 'SHINOBI', font=f1, fill=(244, 244, 246, 255), anchor='mm', stroke_width=7, stroke_fill=(179, 18, 42, 255))
    d.text((450, 262), 'PS2  RECOMP', font=f2, fill=(244, 244, 246, 255), anchor='mm', stroke_width=3, stroke_fill=(20, 12, 16, 255))
    out['0fc8ae0c-9e41-4676-90d9-741760077d0e'] = ('image/png', _png(logo))
    # the SEGA and PlayStation logos: empty
    blank = _png(Image.new('RGBA', (2, 2), (0, 0, 0, 0)))
    out['2e2c0ffd-b51c-4997-b617-7a8681613443'] = ('image/png', blank)
    out['a6a19fc5-2af0-4019-98aa-627e71c90e00'] = ('image/png', blank)
    return out


if PUBLIC:
    for uid, (mime, data) in _public_assets().items():
        assert uid in manifest, uid
        manifest[uid] = {'mime': mime, 'compressed': False, 'data': base64.b64encode(data).decode()}
    template = template.replace('alt="Shinobi key art: Hotsuma with sword raised, flanked by two women"', 'alt="Shinobi PS2 Recomp"')


def must_replace(text, old, new, count=1):
    n = text.count(old)
    if n != count:
        raise SystemExit('design changed? expected %d x %r, found %d' % (count, old[:70], n))
    return text.replace(old, new)


# ---------------------------------------------------------------- guide pictures (back of the box)
GUIDES = [  # (key, caption, label, crop in back-cover thumbnail units (280 x 420 for the left 45% of the scan))
    ('chain', 'Chain attacks together to destroy hordes of enemies', 'Chain attacks', (24, 104, 126, 178)),
    ('magic', 'Unleash devastating Ninja Magic', 'Ninja Magic', (145, 140, 247, 213)),
    ('wall', 'Wall run to explore secret areas', 'Wall run', (17, 209, 119, 283)),
    ('dash', 'Stealth-dash to confuse enemies with ghost images', 'Stealth dash', (137, 236, 238, 310)),
]
if PUBLIC:  # the guides' captions in plain words of our own (the box's phrases are not used)
    GUIDES = [('chain', 'String sword attacks together to deal with groups of enemies', 'Chain attacks', None),
              ('magic', 'Ninja Magic is a powerful special attack', 'Ninja Magic', None),
              ('wall', 'Run along walls to reach hidden places', 'Wall run', None),
              ('dash', 'Dash through enemies to get out of their way', 'Stealth dash', None)]
guide_ids = {}
if PUBLIC:
    from PIL import ImageDraw
    for key, caption, label, _ in GUIDES:
        card = _gradient(900, 500, (58, 14, 28), (14, 12, 18))
        d = ImageDraw.Draw(card)
        d.rectangle([0, 0, 899, 499], outline=(179, 18, 42), width=6)
        d.text((450, 250), label.upper(), font=_font(84), fill=(244, 244, 246), anchor='mm')
        uid = str(uuid.uuid5(uuid.NAMESPACE_URL, 'shinobi-launcher-guide-' + key))
        manifest[uid] = {'mime': 'image/jpeg', 'compressed': False, 'data': base64.b64encode(_jpg(card)).decode()}
        guide_ids[key] = uid
elif os.path.exists(box_scan):
    scan = Image.open(box_scan).convert('RGB')
    scale = (0.45 * scan.width) / 280.0
    for key, _, _, box in GUIDES:
        crop = scan.crop(tuple(int(v * scale) for v in box))
        crop.thumbnail((900, 600))
        buf = io.BytesIO()
        crop.save(buf, 'JPEG', quality=88)
        uid = str(uuid.uuid5(uuid.NAMESPACE_URL, 'shinobi-launcher-guide-' + key))
        manifest[uid] = {'mime': 'image/jpeg', 'compressed': False, 'data': base64.b64encode(buf.getvalue()).decode()}
        guide_ids[key] = uid
else:
    print('warning: box scan not found, guide pictures stay empty:', box_scan)

# ---------------------------------------------------------------- styles: fill the launcher window
css = """
html{width:100%;height:100%;overflow:hidden;background:#16161a;display:grid;place-items:center}
body{margin:0;width:1000px;height:640px;overflow:hidden;background:#16161a}
.stage{width:1000px!important;height:640px!important;background:none!important}
.win{border:0!important;border-radius:0!important;box-shadow:none!important}
.tb{user-select:none;-webkit-user-select:none;app-region:drag;-webkit-app-region:drag}
.tbb{app-region:no-drag;-webkit-app-region:no-drag}
html.manual-open .tb{app-region:no-drag;-webkit-app-region:no-drag}
.foot-wrap{margin-top:auto;display:flex;flex-direction:column;gap:5px}
.foot-wrap .foot{margin-top:0;padding-top:0}
.own{margin:0;font-family:Rajdhani,sans-serif;font-size:11px;font-weight:600;line-height:1.25;color:#4a4a52;
     border-left:2px solid #b3122a;padding:2px 0 2px 7px}
.studio{margin:0;font-family:Rajdhani,sans-serif;font-size:11px;font-weight:600;line-height:1.2;color:#3b3b42;letter-spacing:.02em}
.studio b{color:#17171a;font-weight:700}
.left .btns{gap:7px}
.left .btns .btn{height:38px}
.foot-wrap{gap:4px!important}
.grp{gap:9px!important}
.inl{display:flex;align-items:center;justify-content:space-between;gap:8px}
.inl .fl{margin:0;white-space:nowrap}
.inl .segs{flex:0 0 148px}
.inl .seg{height:30px;font-size:14px;padding:0 2px}
.inl .seg.wide{flex:1.9}
html.ingame .tbb:not(.cl){display:none}
html.ingame .tb{app-region:no-drag;-webkit-app-region:no-drag}
.btn.warn .lab{color:#b3122a}
.pad-focus{outline:3px solid #b3122a!important;outline-offset:2px!important;border-radius:4px}
input[type=range].pad-focus{outline-offset:4px!important}
.ic-mute{display:none}.tbb.upd{width:auto!important;padding:0 12px!important;margin:4px 8px 4px 0;background:#b3122a!important;color:#fff!important;font-weight:700;font-size:12px;letter-spacing:.06em;border-radius:3px;text-transform:uppercase}.tbb.upd:hover{background:#d0152f!important}
html.muted .ic-snd{display:none}
html.muted .ic-mute{display:inline}
html.nomusic .snd{opacity:.35}
.ic-res{display:none}
html.maxed .ic-max{display:none}
html.maxed .ic-res{display:inline}
.gview img{height:270px!important}
"""

# ---------------------------------------------------------------- entrance animation
# The key art first (up from black, out of a blur, settling from a slight zoom), then the title bar, the logo (popping in),
# the left column item by item from the left, the settings panel row by row from the right, the footer last.
# --t scales every time: 1 in the launcher, 0.4 in the in-game menu. Off with Windows' "reduce animations".
# Individual transform properties (translate / scale) are animated so the elements' own transforms are kept.
intro_css = """
:root{--t:1.6}
html.ingame{--t:.5}
@keyframes ivHero{from{opacity:0;filter:brightness(.1) blur(10px)}to{opacity:1;filter:none}}
@keyframes ivArt{from{scale:1.14}to{scale:1}}
@keyframes ivLogo{0%{opacity:0;scale:.55;translate:0 22px;filter:blur(8px) drop-shadow(0 4px 10px rgba(40,0,30,.45))}
  62%{opacity:1;scale:1.07;translate:0 -5px;filter:blur(0) drop-shadow(0 4px 10px rgba(40,0,30,.45))}
  100%{opacity:1;scale:1;translate:0 0;filter:drop-shadow(0 4px 10px rgba(40,0,30,.45))}}
@keyframes ivDown{from{opacity:0;translate:0 -100%}to{opacity:1;translate:0 0}}
@keyframes ivFade{from{opacity:0}to{opacity:1}}
@keyframes ivLeft{from{opacity:0;translate:-26px 0}to{opacity:1;translate:0 0}}
@keyframes ivRight{from{opacity:0;translate:26px 0}to{opacity:1;translate:0 0}}
@keyframes ivPop{0%{opacity:0;scale:.92}70%{opacity:1;scale:1.02}100%{opacity:1;scale:1}}
@media (prefers-reduced-motion:no-preference){
.hero{animation:ivHero calc(1.6s*var(--t)) ease-out backwards}
.art{animation:drift 38s ease-in-out infinite alternate,ivArt calc(2.6s*var(--t)) cubic-bezier(.2,.7,.2,1) backwards}
.logo{animation:ivLogo calc(1.1s*var(--t)) cubic-bezier(.2,.9,.3,1) calc(.9s*var(--t)) backwards}
.tb{animation:ivDown calc(.6s*var(--t)) ease-out calc(.3s*var(--t)) backwards}
.left,.right{animation:ivFade calc(.8s*var(--t)) ease-out calc(.6s*var(--t)) backwards}
.grp{animation:ivPop calc(.7s*var(--t)) ease-out calc(1.2s*var(--t)) backwards}
"""
_stagger = []
for k in range(1, 9):  # left column: guides, card (the buttons and footer have their own steps)
    _stagger.append('.left>:not(.btns):not(.foot-wrap):nth-child(%d){animation:ivLeft calc(.55s*var(--t)) ease-out '
                    'calc(%.2fs*var(--t)) backwards}' % (k, 1.0 + 0.12 * k))
for k in range(1, 9):  # left buttons and the game-file caption (every child of .btns)
    _stagger.append('.btns>:nth-child(%d){animation:ivLeft calc(.5s*var(--t)) cubic-bezier(.2,.8,.3,1.15) '
                    'calc(%.2fs*var(--t)) backwards}' % (k, 1.35 + 0.11 * k))
for k in range(1, 13):  # settings rows
    _stagger.append('.grp>:nth-child(%d){animation:ivRight calc(.5s*var(--t)) ease-out calc(%.2fs*var(--t)) backwards}'
                    % (k, 1.35 + 0.075 * k))
for k in range(1, 6):  # footer: notice, logos, studio lines
    _stagger.append('.foot-wrap>:nth-child(%d){animation:ivFade calc(.6s*var(--t)) ease-out calc(%.2fs*var(--t)) backwards}'
                    % (k, 1.85 + 0.1 * k))
# Interactions: the drop-down slides open, buttons slide and light up, switches and tabs change colour smoothly, dialogs
# rise, the toast slides up.
intro_css += '\n'.join(_stagger) + """
@keyframes ivMenu{from{opacity:0;translate:0 -6px;scale:1 .94}to{opacity:1;translate:0 0;scale:1 1}}
@keyframes ivToast{from{opacity:0;translate:0 12px}to{opacity:1;translate:0 0}}
.menu{transform-origin:top center;animation:ivMenu .3s cubic-bezier(.2,.8,.3,1)}
.opt{transition:background .2s,color .2s,padding-left .2s}
.opt:hover{padding-left:14px}
.btn{transition:filter .22s,translate .26s cubic-bezier(.2,.8,.3,1),box-shadow .26s,transform .1s}
.btns .btn:hover{translate:4px 0}
.seg,.chipb,.gtab,.th{transition:background .26s,color .26s,box-shadow .26s,filter .26s}
.th:hover,.gtab:hover{filter:brightness(1.12)}
.chk .bx{transition:background .25s,box-shadow .25s}
.dlg{animation:rise .4s cubic-bezier(.2,.8,.3,1)}
.toast{animation:ivToast .4s ease-out!important}
}
"""
css += intro_css
template = must_replace(template, '@media (prefers-reduced-motion:reduce)', css + '@media (prefers-reduced-motion:reduce)')

# ---------------------------------------------------------------- markup
# The title bar is a real Windows title bar (CSS app-region, see the host): drag, double-click to maximise, snap.
# Left of minimise: the music mute button (speaker / crossed speaker, html.muted picks the icon).
template = must_replace(template, '<button class="tbb" aria-label="Minimize">',
                        '<sc-if value="{{hasUpdate}}" hint-placeholder-val="{{false}}"><button class="tbb upd" title="{{updTitle}}" '
                        'sc-camel-on-click="{{doUpdate}}">{{updLabel}}</button></sc-if>'
                        '<button class="tbb snd" aria-label="Mute music" title="Music on / off" sc-camel-on-click="{{toggleMute}}">'
                        '<svg class="ic-snd" width="14" height="12" sc-camel-view-box="0 0 14 12" aria-hidden="true">'
                        '<path d="M1 4h3l4-3v10l-4-3H1z" fill="currentColor"></path>'
                        '<path d="M10 3.5c1 .7 1.5 1.5 1.5 2.5s-.5 1.8-1.5 2.5M11.5 1.5c1.6 1.2 2.2 2.8 2.2 4.5s-.6 3.3-2.2 4.5" '
                        'fill="none" stroke="currentColor" stroke-width="1.1"></path></svg>'
                        '<svg class="ic-mute" width="14" height="12" sc-camel-view-box="0 0 14 12" aria-hidden="true">'
                        '<path d="M1 4h3l4-3v10l-4-3H1z" fill="currentColor"></path>'
                        '<path d="M9.5 4l4 4M13.5 4l-4 4" fill="none" stroke="currentColor" stroke-width="1.2"></path></svg>'
                        '</button>'
                        '<button class="tbb" aria-label="Minimize" sc-camel-on-click="{{winMin}}">')
template = must_replace(template,
                        '<button class="tbb" aria-label="Maximize"><svg width="10" height="10" sc-camel-view-box="0 0 10 10" '
                        'aria-hidden="true"><rect x=".5" y=".5" width="9" height="9" fill="none" stroke="currentColor" '
                        'stroke-width="1"></rect></svg></button>',
                        '<button class="tbb" aria-label="Maximize" sc-camel-on-click="{{winMax}}"><svg class="ic-max" width="10" '
                        'height="10" sc-camel-view-box="0 0 10 10" aria-hidden="true"><rect x=".5" y=".5" width="9" height="9" '
                        'fill="none" stroke="currentColor" stroke-width="1"></rect></svg><svg class="ic-res" width="10" height="10" '
                        'sc-camel-view-box="0 0 10 10" aria-hidden="true"><rect x=".5" y="2.5" width="7" height="7" fill="none" '
                        'stroke="currentColor" stroke-width="1"></rect><path d="M2.5 2.5V.5h7v7h-2" fill="none" '
                        'stroke="currentColor" stroke-width="1"></path></svg></button>')
template = must_replace(template, '<button class="tbb cl" aria-label="Close">',
                        '<button class="tbb cl" aria-label="Close" sc-camel-on-click="{{winClose}}">')
# In-game mode (the same page as the game's pause menu, ps2_ingame_menu.cpp): title, card, last left button and the reset
# button take their text from the logic so the game can relabel them.
template = must_replace(template, '<span>Shinobi (PS2 Recom) Launcher - by TheRealAfsan</span>', '<span>{{titleText}}</span>')
template = must_replace(template, '<h2>Shinobi - PS2 Recompilation</h2>', '<h2>{{cardTitle}}</h2>')
template = re.sub(r'<p>Ignite the fiery vengeance.*?</p>', '<p>{{cardText}}</p>', template, count=1, flags=re.S)
template = must_replace(template, '<button class="btn" sc-camel-on-click="{{openCredits}}"><span class="lab">Credits</span></button>',
                        # LOCAL PATCH (save states, GitHub #16): in the game's pause menu only.
                        # three slots (Ctrl+1/2/3 in game): pick one, then save / load it
                        '<div style="{{stateSlotsStyle}}">'
                        '<b style="font-size:12px;letter-spacing:.08em;flex:0 0 auto">SLOT</b>'
                        '<div class="segs" role="group" aria-label="Save state slot" style="flex:0 0 auto">'
                        '<sc-for list="{{stateSlotChips}}" as="t" hint-placeholder-count="3">'
                        '<button class="seg {{t.cls}}" style="min-width:30px;white-space:nowrap" aria-pressed="{{t.on}}" title="{{t.tip}}" '
                        'sc-camel-on-click="{{t.pick}}">{{t.name}}</button>'
                        '</sc-for></div>'
                        '<small style="flex:1 1 auto;min-width:0;opacity:.75;font-size:11.5px;white-space:nowrap;overflow:hidden;'
                        'text-overflow:ellipsis">{{stateSlotText}}</small>'
                        '</div>'
                        '<div style="{{stateRowStyle}}">'
                        '<button class="btn" style="flex:1 1 0;min-width:0" title="Save the game exactly where you are, in the slot above (Ctrl+F1)" '
                        'sc-camel-on-click="{{saveStateBtn}}"><span class="lab">Save State</span></button>'
                        '<button class="btn" style="flex:1 1 0;min-width:0" title="Back to where you saved in the slot above (Ctrl+F2)" '
                        'sc-camel-on-click="{{loadStateBtn}}"><span class="lab">Load State</span></button>'
                        '</div>'
                        '<div style="display:flex;gap:8px">'
                        '<button class="btn" style="flex:1 1 0;min-width:0" title="The game manual (your own PDF), turned like a book" '
                        'sc-camel-on-click="{{openManual}}"><span class="lab">Manual</span></button>'
                        '<button class="btn {{lastCls}}" style="flex:1 1 0;min-width:0" sc-camel-on-click="{{lastBtn}}"><span class="lab">{{lastLabel}}</span></button>'
                        '</div>')
template = must_replace(template, '<button class="btn sm" sc-camel-on-click="{{reset}}"><span class="lab">Reset to Defaults</span></button>',
                        '<div style="display:flex;gap:8px">'
                        '<button class="btn sm {{resetCls}}" style="flex:1 1 0;min-width:0" sc-camel-on-click="{{resetBtn}}"><span class="lab">{{resetLabel}}</span></button>'
                        '<button class="btn sm" style="flex:1 1 0;min-width:0" title="Lighter settings for slower PCs" sc-camel-on-click="{{lowEndBtn}}"><span class="lab">Low-end PC</span></button>'
                        '</div>')

# Mod Menu: Hotsuma's scarf colour (settings [game] scarf_color, ps2_scarf_colour.cpp), above the switches.
template = must_replace(template, '<sc-for list="{{modRows}}" as="m" hint-placeholder-count="4">',
                        # LOCAL PATCH: Mod Menu pages (Look / Camera / Gameplay / Controller / Start-up / Advanced),
                        # switched like the settings pages.
                        '<div class="tabs tabnav modnav" role="tablist" aria-label="Mod Menu pages">'
                        '<button class="tnav" aria-label="Previous mod page" sc-camel-on-click="{{modTabPrev}}"><b class="kh">{{tabHintL}}</b><span class="ar">&#8249;</span></button>'
                        '<div class="mtabs"><sc-for list="{{modTabOpts}}" as="t" hint-placeholder-count="6">'
                        '<button class="mtab {{t.cls}}" role="tab" aria-selected="{{t.on}}" sc-camel-on-click="{{t.pick}}"><span>{{t.name}}</span></button></sc-for></div>'
                        '<button class="tnav" aria-label="Next mod page" sc-camel-on-click="{{modTabNext}}"><span class="ar">&#8250;</span><b class="kh">{{tabHintR}}</b></button></div>'
                        '<sc-if value="{{modTabLook}}" hint-placeholder-val="{{true}}">'
                        '<div class="mrow scarfrow">'
                        '<div><b>Scarf colour</b><span>{{scarfDesc}}</span></div>'
                        '<div style="display:flex;flex-wrap:wrap;gap:6px;align-items:center;justify-content:flex-end;max-width:260px">'
                        '<sc-for list="{{scarfSwatches}}" as="c" hint-placeholder-count="8">'
                        '<button class="swatch" style="{{c.style}}" title="{{c.name}}" aria-label="{{c.name}}" '
                        'aria-pressed="{{c.on}}" sc-camel-on-click="{{c.pick}}"></button>'
                        '</sc-for>'
                        '<input type="color" style="width:30px;height:24px;padding:0;border:1px solid #888;border-radius:4px;background:none;cursor:pointer" value="{{scarfHex}}" title="Any colour" aria-label="Pick any scarf colour" '
                        'sc-camel-on-change="{{scarfPick}}">'
                        '</div></div>'
                        '</sc-if>'
                        # LOCAL PATCH: haptic strength (settings [input] haptic_strength), live in the game
                        '<sc-if value="{{modTabPad}}" hint-placeholder-val="{{false}}">'
                        '<div class="mrow scarfrow"><div><b>Haptic strength</b><span>How strong the DualSense haptics feel: {{hapStrName}}.</span></div>'
                        '<div class="segs" style="flex:0 0 auto" role="group" aria-label="Haptic strength">'
                        '<sc-for list="{{hapChips}}" as="h" hint-placeholder-count="4">'
                        '<button class="seg {{h.cls}}" aria-pressed="{{h.on}}" sc-camel-on-click="{{h.pick}}">{{h.name}}</button>'
                        '</sc-for></div></div>'
                        '</sc-if>'
                        # LOCAL PATCH: final boss health (settings [game] final_boss_health), Gameplay page
                        '<sc-if value="{{modTabGameplay}}" hint-placeholder-val="{{false}}">'
                        '<div class="mrow scarfrow"><div><b>Final boss health</b><span>Hiruko\'s health in the real fight: {{bossHpName}}</span></div>'
                        '<div class="segs" style="flex:0 0 auto" role="group" aria-label="Final boss health">'
                        '<sc-for list="{{bossHpChips}}" as="h" hint-placeholder-count="4">'
                        '<button class="seg {{h.cls}}" aria-pressed="{{h.on}}" sc-camel-on-click="{{h.pick}}">{{h.name}}</button>'
                        '</sc-for></div></div>'
                        '</sc-if>'
                        '<sc-for list="{{modRows}}" as="m" hint-placeholder-count="4">')


# ---- LOCAL PATCH: settings panel in three tabs (Display / Graphics / Controller), window mode dropdown, haptics ----
CHK = ('<span class="bx"><svg width="12" height="10" sc-camel-view-box="0 0 12 10" aria-hidden="true"><path d="M1 5l3.5 3.5L11 1.5" '
       'fill="none" stroke="#fff" stroke-width="2.2"></path></svg></span>')
template = must_replace(template, '<h2>Graphical Settings</h2>',
    '<h2>Settings</h2>\n'
    # One page name at a time; L1 / R1 (LB / RB, Q / E) or the arrows switch pages, the dots jump to one.
    '        <div class="tabs tabnav" role="tablist" aria-label="Settings pages">'
    '<button class="tnav" aria-label="Previous settings page" sc-camel-on-click="{{tabPrev}}"><b class="kh">{{tabHintL}}</b><span class="ar">&#8249;</span></button>'
    '<div class="tcur"><div class="tname">{{tabName}}</div><div class="tdots"><sc-for list="{{tabOpts}}" as="t" hint-placeholder-count="6">'
    '<button class="tdot {{t.cls}}" role="tab" aria-label="{{t.name}}" title="{{t.name}}" aria-selected="{{t.on}}" sc-camel-on-click="{{t.pick}}"></button></sc-for></div></div>'
    '<button class="tnav" aria-label="Next settings page" sc-camel-on-click="{{tabNext}}"><span class="ar">&#8250;</span><b class="kh">{{tabHintR}}</b></button></div>\n'
    '        <sc-if value="{{tabDisplay}}" hint-placeholder-val="{{true}}">')
_m = re.search(r'\n(\s*)<div( class="inl")?>\s*<div class="fl">Renderer</div>', template)
assert _m, 'renderer anchor'
template = template[:_m.start()] + (
    '\n        <div>\n'
    '          <div class="fl">Window Mode</div>\n'
    '          <div class="selwrap">\n'
    '            <button class="sel" aria-haspopup="listbox" aria-expanded="{{winOpen}}" sc-camel-on-click="{{toggleWin}}">\n'
    '              <span>{{winLabel}}</span>\n'
    '              <svg width="12" height="8" sc-camel-view-box="0 0 12 8" aria-hidden="true"><path d="M1 1l5 5 5-5" fill="none" stroke="#17171a" stroke-width="2"></path></svg>\n'
    '            </button>\n'
    '            <sc-if value="{{winOpen}}" hint-placeholder-val="{{false}}">\n'
    '              <button class="away" tabindex="-1" aria-hidden="true" sc-camel-on-click="{{closeWin}}"></button>\n'
    '              <div class="menu" role="listbox" aria-label="Window mode">\n'
    '                <sc-for list="{{winOptions}}" as="o" hint-placeholder-count="3">\n'
    '                  <button class="opt {{o.cls}}" role="option" aria-selected="{{o.on}}" sc-camel-on-click="{{o.pick}}"><span>{{o.name}}</span><em>{{o.sub}}</em></button>\n'
    '                </sc-for>\n'
    '              </div>\n'
    '            </sc-if>\n'
    '          </div>\n'
    '        </div>\n') + template[_m.start():]
_m = re.search(r'\n\s*<div>\s*<div class="fl"[^>]*><span>Texture Quality', template)
assert _m, 'texture anchor'
template = template[:_m.start()] + '\n        </sc-if>\n        <sc-if value="{{tabGraphics}}" hint-placeholder-val="{{false}}">' + (
    # LOCAL PATCH (GitHub issue #23): Field of view slider (70..150 %, 100 = the original view; live in game)
    '\n        <div>\n'
    '          <div class="fl" title="How much of the world the camera shows. 100% is the original view."><span>Field of View</span><em>{{fovName}}</em></div>\n'
    '          <div class="rng"><div class="trk" style="background: {{fovBg}}"></div><input type="range" min="70" max="150" step="5" '
    'value="{{fovNum}}" sc-camel-on-change="{{setFov}}" aria-label="Field of view"></div>\n'
    '        </div>') + template[_m.start():]
_m = re.search(r'\n\s*<div class="act">', template)
assert _m, 'act anchor'
template = template[:_m.start()] + (
    '\n        </sc-if>\n'
    '        <sc-if value="{{tabController}}" hint-placeholder-val="{{false}}">\n'
    '        <div>\n'
    '          <label class="chk"><input type="checkbox" checked="{{haptics}}" sc-camel-on-change="{{toggleHaptics}}">' + CHK +
    'Haptic Feedback (DualSense)</label>\n'
    '          <p class="note" style="margin:2px 0 6px 30px;font-size:11px;color:#777">Footsteps, shuriken, sword hits, dash and ninja magic, on the left and right of the controller. USB or Bluetooth.</p>\n'
    '        </div>\n'
    '        <div>\n'
    '          <div class="fl"><span>Haptic Strength</span><em>{{hapStrName}}</em></div>\n'
    '          <div class="rng"><div class="trk" style="background: {{hapStrBg}}"></div><input type="range" min="1" max="10" step="1" value="{{hapStr}}" sc-camel-on-change="{{setHapStr}}" aria-label="Haptic strength"></div>\n'
    '        </div>\n'
    '        <div>\n'
    '          <label class="chk"><input type="checkbox" checked="{{trig}}" sc-camel-on-change="{{toggleTrig}}">' + CHK +
    'L2 Trigger Resistance</label>\n'
    '          <p class="note" style="margin:2px 0 0 30px;font-size:11px;color:#777">L2 stiffens part way down and clicks through just before the ninja magic fires.</p>\n'
    '        </div>\n'
    '        </sc-if>') + template[_m.start():]
css_tabs = ('html.manual-only body>*:not([role=dialog]){visibility:hidden!important}html body .pad-focus.pad-focus.pad-focus{outline:3px solid #ff1f3d!important;outline-offset:-3px!important;box-shadow:inset 0 0 14px rgba(255,31,61,.55),0 0 14px rgba(255,31,61,.7)!important;position:relative;z-index:2}.tabs{margin:0 0 6px;flex-shrink:0}.grp label.chk{height:26px!important;min-height:0!important}.tabs .seg{flex:1 1 auto;min-width:0;padding-left:6px!important;padding-right:6px!important;font-size:12.5px!important;letter-spacing:0!important}html.tabbed .grp>*{animation:none!important;opacity:1!important}.segs:not(.tabs):has(.seg:nth-of-type(6)) .seg{font-size:13px!important;padding-left:2px!important;padding-right:2px!important;letter-spacing:0!important;white-space:nowrap}'
            '.grp>sc-if>div,.grp sc-if>div{margin-bottom:2px}')
# LOCAL PATCH: Mod Menu pages: the dialog keeps one size on every page, the page bar stays at the top while scrolling.
css_tabs += '.db:has(.modnav){min-height:min(430px,52vh)}.modnav{position:sticky;top:0;z-index:3;background:#0c0c0d;margin-bottom:6px}'
# Mod Menu page bar: every page's name side by side, slanted tabs overlapping their neighbours, the current one raised.
css_tabs += ('.modnav{grid-template-columns:auto minmax(0,1fr) auto}.modnav .tnav{flex:0 0 auto;position:relative;z-index:4}.modnav .mtabs{display:flex;justify-content:center;align-items:center;padding:4px 8px;min-width:0}'
             '.modnav .mtab{position:relative;margin:0 -5px;padding:7px 12px;background:#18181b;border:1px solid #34343a;color:#8b8b92;'
             'font:inherit;font-size:11px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;white-space:nowrap;cursor:pointer;'
             'transform:skewX(-14deg);transition:transform .2s cubic-bezier(.2,.8,.3,1),background .2s,color .2s,box-shadow .2s;z-index:1}'
             '.modnav .mtab>span{display:inline-block;transform:skewX(14deg)}'
             '.modnav .mtab:hover{color:#e6e6e6;background:#222226;z-index:2}'
             '.modnav .mtab.on{margin:0 3px;background:#f2f2f2;color:#0b0b0c;border-color:#fff;z-index:3;transform:skewX(-14deg) scale(1.05);'
             'box-shadow:0 6px 18px rgba(0,0,0,.7),0 0 0 1px rgba(255,255,255,.35)}')
template = must_replace(template, '@media (prefers-reduced-motion:reduce)', css_tabs + '@media (prefers-reduced-motion:reduce)')


# ---- LOCAL PATCH: hero slideshow. The key art, then the pictures in Pictures\lauccher art N.jpg (N = 2, 3, ...),
# crossfading every 5 seconds, in a loop. Each picture is a layer over the key art, under the shading.
_slides = []
_pics = os.path.join(os.environ['USERPROFILE'], 'Pictures')
for _n in range(2, 20):
    _f = os.path.join(_pics, 'lauccher art %d.jpg' % _n)
    if not os.path.exists(_f):
        continue
    _im = Image.open(_f).convert('RGB')
    if _im.height > 1200:
        _im = _im.resize((round(_im.width * 1200 / _im.height), 1200), Image.LANCZOS)
    _uid = str(uuid.uuid4())
    manifest[_uid] = {'mime': 'image/jpeg', 'compressed': False, 'data': base64.b64encode(_jpg(_im)).decode()}
    _slides.append(_uid)
if _slides:
    _hold = 5.0
    _cycle = _hold * (len(_slides) + 1)
    _imgs = ''.join('\n      <img class="slide" src="%s" alt="" aria-hidden="true" style="animation-delay:%.1fs">' % (u, _hold * (i + 1))
                    for i, u in enumerate(_slides))
    _m = re.search(r'(<img class="art" src="[^"]+" alt="[^"]*">)', template)
    assert _m, 'hero art'
    template = template[:_m.end()] + _imgs + template[_m.end():]
    _fade = 1.0 / _cycle * 100.0          # one second, in percent of the cycle
    _show = _hold / _cycle * 100.0
    css_slides = ('.hero .slide{position:absolute;inset:0;width:100%%;height:100%%;object-fit:cover;opacity:0;'
                  'animation:heroSlide %.1fs linear infinite;pointer-events:none}'
                  '@keyframes heroSlide{0%%{opacity:0}%.2f%%{opacity:1}%.2f%%{opacity:1}%.2f%%{opacity:0}100%%{opacity:0}}'
                  % (_cycle, _fade, _show, _show + _fade))
    template = must_replace(template, '@media (prefers-reduced-motion:reduce)', css_slides + '@media (prefers-reduced-motion:reduce)')
    print('hero slideshow:', len(_slides), 'pictures')


# ---- LOCAL PATCH: Graphics tab gets Blending + Texture Filtering; a new Effects tab (screen filter, dithering,
# mipmapping, upscaling fixes) ----
def _segrow(label, binding, count, tip=''):
    return ('        <div>\n'
            '          <div class="fl"' + (' title="%s"' % tip if tip else '') + '>' + label + '</div>\n'
            '          <div class="segs" role="group" aria-label="' + label + '">\n'
            '            <sc-for list="{{' + binding + '}}" as="o" hint-placeholder-count="' + str(count) + '">\n'
            '              <button class="seg {{o.cls}}" aria-pressed="{{o.on}}" sc-camel-on-click="{{o.pick}}">{{o.name}}</button>\n'
            '            </sc-for>\n'
            '          </div>\n'
            '        </div>\n')
def _chkrow(label, binding, toggle, note):
    return ('        <div>\n'
            '          <label class="chk"><input type="checkbox" checked="{{' + binding + '}}" sc-camel-on-change="{{' + toggle + '}}">' + CHK + label + '</label>\n'
            '          <p class="note" style="margin:0 0 4px 30px;font-size:11px;color:#777">' + note + '</p>\n'
            '        </div>\n')
template = must_replace(template, '        </sc-if>\n        <sc-if value="{{tabController}}"',
    _segrow('Blending Accuracy', 'blendOpts', 4, 'How exactly glows, smoke and see-through effects are drawn. Higher costs more GPU.') +
    _segrow('Texture Filtering', 'texfOpts', 2, 'As on PS2 keeps each texture as the game asked; Bilinear smooths them all.') +
    '        </sc-if>\n'
    '        <sc-if value="{{tabEffects}}" hint-placeholder-val="{{false}}">\n' +
    _segrow('Screen Filter', 'scrfOpts', 4, 'Sharp shows the pixels; Smooth blends them; Scanlines and CRT look like an old TV.') +
    _chkrow('Dithering', 'dith', 'toggleDith', 'The PS2&#39;s grainy colour pattern. Untick for smooth, clean gradients.') +
    _chkrow('Mipmapping', 'mip', 'toggleMip', 'Less shimmer on distant floors and walls. Applies after a restart.') +
    _chkrow('Upscaling Fixes', 'upfix', 'toggleUpfix', 'Removes seams and thin lines at high resolutions. Applies after a restart.') +
    '        </sc-if>\n        <sc-if value="{{tabController}}"')

# (The mod switches - close-up camera, HUD, bars, scarf - are in the Mod Menu: see GAME_MODS.)

# ---- LOCAL PATCH: the Performance tab (frame generation, its target, low latency, the overlay) ----
template = must_replace(template, '        </sc-if>\n        <sc-if value="{{tabController}}"',
    '        </sc-if>\n'
    '        <sc-if value="{{tabPerf}}" hint-placeholder-val="{{false}}">\n' +
    _chkrow('Frame Generation (F4)', 'fg', 'toggleFg', 'In-between pictures on a 120 Hz+ screen: much smoother motion, about 16 ms more delay.') +
    _segrow('Frame Generation Target', 'fgtOpts', 6, 'Pictures a second with frame generation. Auto = just under your screen&#39;s refresh.') +
    _chkrow('Low Latency (F6)', 'lowlat', 'toggleLowlat', 'Stops the graphics driver queueing frames: 16-33 ms less delay, like NVIDIA Reflex.') +
    _chkrow('Performance Overlay (F2)', 'ovl', 'toggleOvl', 'FPS, frame time, 1% lows, CPU, GPU, RAM and VRAM in the top-left corner.') +
    '        </sc-if>\n        <sc-if value="{{tabController}}"')

# Six settings tabs: smaller tab labels so they all fit; the Jukebox's own compact rows (name + file, two small buttons).
template = must_replace(template, '@media (prefers-reduced-motion:reduce)',
    # The boot screen (see the constructor): gold retro LOADING bar over black with faint scanlines; the page's own
    # animations wait, paused, until it goes.
    'html.booting .stage *,html.booting .stage{animation-play-state:paused!important}'
    '.boot{position:fixed;inset:0;z-index:100000;background:radial-gradient(ellipse at 50% 45%,#141006 0%,#050505 62%,#000 100%);'
    'display:grid;place-items:center;transition:opacity .45s ease,filter .45s ease}'
    '.boot::after{content:"";position:absolute;inset:0;pointer-events:none;'
    'background:repeating-linear-gradient(180deg,rgba(255,255,255,.035) 0 1px,transparent 1px 3px);mix-blend-mode:screen}'
    '.boot.boot-out{opacity:0;filter:blur(6px) brightness(1.4)}'
    '.boot-vid{position:absolute;inset:0;width:100%;height:100%;object-fit:cover;opacity:0;animation:bootVid .6s ease-out .05s forwards;'
    'filter:saturate(1.15) contrast(1.08)}'
    '@keyframes bootVid{to{opacity:.62}}'
    '.boot-shade{position:absolute;inset:0;background:radial-gradient(ellipse at 50% 48%,rgba(0,0,0,.25) 0%,rgba(0,0,0,.55) 55%,rgba(0,0,0,.9) 100%)}'
    # 1985 digital-dashboard look (Nissan 300ZX style): phosphor-green bar graph like a tachometer, small boxed
    # readouts; small and low on the screen, the lightning behind it.
    '.boot{place-items:end start!important;padding:0 0 5.5vh 4vw;box-sizing:border-box}'
    '.boot-in{position:relative;z-index:1;width:min(19vw,240px);padding:0;'
    'animation:bootIn .35s ease-out both;'
    'font-family:Consolas,"Lucida Console",monospace;color:#5dff9a}'
    '@keyframes bootIn{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}'
    '.boot-top{display:flex;justify-content:space-between;align-items:center;margin:0 0 6px}'
    '.boot-word{font-size:10.5px;letter-spacing:.3em;text-shadow:0 0 6px rgba(60,255,130,.8)}'
    '.boot-dots b{display:inline-block;font-style:normal;font-weight:400;animation:bootDot 1s steps(1) infinite}'
    '.boot-dots b:nth-child(2){animation-delay:.2s}.boot-dots b:nth-child(3){animation-delay:.4s}'
    '@keyframes bootDot{0%{opacity:1}50%{opacity:.15}}'
    '.boot-pct{text-align:right;font-size:12px;letter-spacing:.08em;'
    'text-shadow:0 0 8px rgba(60,255,130,.9);font-variant-numeric:tabular-nums}'
    '.boot-graph{display:flex;align-items:flex-end;gap:2px;height:22px}'
    '.boot-graph i{flex:1;border-radius:1px;background:rgba(60,255,130,.10);box-shadow:inset 0 0 0 1px rgba(60,255,130,.12);'
    'transition:background .08s,box-shadow .08s}'
    '.boot-graph i.on{background:linear-gradient(180deg,#9dffc0,#2fe07a);box-shadow:0 0 7px rgba(60,255,130,.75)}'
    '.boot-graph i.on.hot{background:linear-gradient(180deg,#eaffb0,#b8f03a);box-shadow:0 0 8px rgba(190,255,80,.8)}'
    '.boot-scale{display:flex;justify-content:space-between;margin-top:4px;padding-top:3px;border-top:1px solid rgba(70,255,140,.3);'
    'font-size:8.5px;letter-spacing:.06em;color:rgba(93,255,154,.55)}'
    '.boot-sub{margin-top:6px;text-align:left;font-size:8.5px;letter-spacing:.34em;color:rgba(93,255,154,.4)}'
    # The credits under the readout, one after the other.
    '.boot-credits{margin-top:10px;display:flex;flex-direction:column;align-items:flex-start;gap:4px;font-size:9.5px;letter-spacing:.2em;white-space:nowrap;'
    'color:rgba(93,255,154,.75);text-shadow:0 0 6px rgba(60,255,130,.55)}'
    '.boot-credits span{opacity:0;transform:translateY(3px);animation:bootLine .3s ease-out forwards}'
    '.boot-credits span:nth-child(1){animation-delay:.25s}.boot-credits span:nth-child(2){animation-delay:.5s}'
    '.boot-credits span:nth-child(3){animation-delay:.75s}'
    '.boot-credits .boot-tra{font-size:13px;letter-spacing:.38em;font-weight:700;color:#5dff9a;text-shadow:0 0 9px rgba(60,255,130,.85)}'
    '@keyframes bootLine{to{opacity:1;transform:none}}'
    # A fiercer intro (after the loading screen, on the bass drop): quicker overall, the art slamming in on a white
    # lightning flash with a short shake, the logo crashing in from big with a recoil, the side columns whipping in
    # with a skew and motion blur. (Same names as the design's: these later definitions take over.)
    ':root{--t:.9}'
    '@keyframes ivHero{0%{opacity:0;filter:brightness(2.6) contrast(1.2) blur(14px)}'
    '18%{opacity:1;filter:brightness(1.9) blur(4px)}45%{filter:brightness(1.15) blur(0)}100%{opacity:1;filter:none}}'
    '@keyframes ivArt{0%{scale:1.28}55%{scale:.985}100%{scale:1}}'
    '@keyframes ivShake{0%,100%{translate:0 0}15%{translate:-7px 3px}30%{translate:6px -4px}45%{translate:-4px 2px}'
    '60%{translate:3px -2px}75%{translate:-2px 1px}}'
    '.hero{animation:ivHero calc(1.2s*var(--t)) cubic-bezier(.12,.9,.2,1) backwards,ivShake .38s linear calc(.12s*var(--t)) backwards!important}'
    '@keyframes ivLogo{0%{opacity:0;scale:1.9;translate:0 0;filter:blur(14px) drop-shadow(0 4px 10px rgba(40,0,30,.45))}'
    '45%{opacity:1;scale:.93;filter:blur(0) drop-shadow(0 4px 10px rgba(40,0,30,.45))}'
    '68%{scale:1.04}84%{scale:.99}100%{opacity:1;scale:1;translate:0 0;filter:drop-shadow(0 4px 10px rgba(40,0,30,.45))}}'
    '@keyframes ivLeft{0%{opacity:0;translate:-70px 0;transform:skewX(-10deg);filter:blur(7px)}'
    '60%{opacity:1;translate:7px 0;transform:skewX(2deg);filter:blur(0)}100%{opacity:1;translate:0 0;transform:none;filter:none}}'
    '@keyframes ivRight{0%{opacity:0;translate:70px 0;transform:skewX(10deg);filter:blur(7px)}'
    '60%{opacity:1;translate:-7px 0;transform:skewX(-2deg);filter:blur(0)}100%{opacity:1;translate:0 0;transform:none;filter:none}}'
    '@keyframes ivPop{0%{opacity:0;scale:.82;filter:blur(6px)}55%{opacity:1;scale:1.035;filter:blur(0)}100%{opacity:1;scale:1;filter:none}}'
    '@keyframes ivDown{0%{opacity:0;translate:0 -100%}65%{opacity:1;translate:0 5px}100%{opacity:1;translate:0 0}}'
    '@keyframes ivFade{0%{opacity:0;filter:brightness(1.8)}100%{opacity:1;filter:none}}'
    # The Refresh button under the TRA logo.
    '.refbtn{display:flex;align-items:center;justify-content:center;gap:5px;margin:6px 0 0 auto;padding:2px 9px 2px 7px;'
    'font-family:Rajdhani,sans-serif;font-weight:700;font-size:11px;letter-spacing:.14em;text-transform:uppercase;color:#bdbdc4;'
    'background:transparent;border:1px solid #3a3a40;border-radius:3px;cursor:pointer;transition:color .15s,border-color .15s}'
    '.refbtn span{font-size:14px;line-height:1;transition:transform .45s cubic-bezier(.2,.8,.3,1)}'
    '.refbtn:hover{color:#fff;border-color:#8a8a92}.refbtn:hover span{transform:rotate(180deg)}'
    'html.ingame .refbtn{display:none}'
    '.markcol{display:flex;flex-direction:column;gap:3px;min-width:0}.markcol .studio{white-space:nowrap}'
    '.foot{align-items:flex-end!important}'
    # A narrower left column, a wider settings column (the art keeps the middle).
    '.body{grid-template-columns:260px minmax(0,1fr) 360px!important}'
    '.left .btn{padding-left:10px!important;padding-right:10px!important;letter-spacing:.04em!important;min-width:0!important}'
    '.left{gap:6px!important}.left .card p{font-size:11.5px!important;line-height:1.28!important}.left .card{padding:8px 11px 9px!important}'
    '.tabnav{display:grid;grid-template-columns:auto 1fr auto;align-items:center;gap:8px;border:1px solid #2c2c31;border-radius:4px;'
    'background:linear-gradient(180deg,#151518,#0b0b0d);padding:5px 6px;overflow:hidden}'
    '.tnav{display:flex;align-items:center;gap:4px;height:34px;padding:0 7px;border:1px solid #3a3a40;border-radius:3px;'
    'background:linear-gradient(180deg,#1f1f23,#121214);color:#eee;cursor:pointer;font-family:Rajdhani,sans-serif;font-weight:700}'
    '.tnav:hover{border-color:#8a8a92}.tnav .ar{font-size:24px;line-height:1;margin-top:-3px}'
    '.tnav .kh{font-size:11.5px;letter-spacing:.04em;padding:1px 5px;border:1px solid #55555c;border-radius:3px;color:#cfcfd4}'
    '.tcur{text-align:center;min-width:0}'
    '.tname{font-family:Rajdhani,sans-serif;font-weight:700;font-size:18px;letter-spacing:.05em;text-transform:uppercase;color:#fff;'
    'white-space:nowrap;overflow:hidden;text-overflow:ellipsis;will-change:transform,filter,opacity}'
    '.tdots{display:flex;justify-content:center;gap:6px;margin-top:3px}'
    '.tdot{width:7px;height:7px;padding:0;border-radius:50%;border:0;background:#4a4a50;cursor:pointer;transition:background .2s,transform .2s}'
    '.tdot.on{background:#fff;transform:scale(1.25)}.tdot:hover{background:#9a9aa2}'
    '.jlist{max-height:236px;overflow-y:auto;overflow-x:hidden;padding-right:6px}'
    '.jrow{display:grid;grid-template-columns:minmax(0,1fr) auto auto;gap:6px;align-items:center;padding:5px 0;border-bottom:1px solid #26262b}'
    '.jfoot{border-bottom:0;margin-top:6px}'
    '.jname{min-width:0;line-height:1.2}'
    '.jname b{display:block;font-size:13px;color:#eee;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}'
    '.jname small{display:block;font-size:11.5px;color:#8a8a92;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}'
    '.jrow .btn{min-width:0;height:28px;padding:0 10px;font-size:12px;letter-spacing:.04em}'
    '.jrow .btn:hover{translate:0 0}'
    '@media (prefers-reduced-motion:reduce)')

# ---- LOCAL PATCH: the Languages page (ps2_lang_overlay.cpp in the game): voices and text in English or Japanese from the
# US disc, or French / German / Italian / Spanish from the player's European disc image. ----
template = must_replace(template, '        </sc-if>\n        <sc-if value="{{tabController}}"',
    '        </sc-if>\n'
    '        <sc-if value="{{tabLanguages}}" hint-placeholder-val="{{false}}">\n'
    '          <div class="jrow jfoot" style="margin:0 0 6px"><div class="jname" title="{{euDiscPath}}"><b>European disc</b>'
    '<small style="{{euDiscStyle}}">{{euDiscText}}</small></div>'
    '<button class="btn" sc-camel-on-click="{{euPick}}"><span class="lab">Choose</span></button>'
    '<button class="btn" sc-camel-on-click="{{euClear}}"><span class="lab">Remove</span></button></div>\n' +
    _segrow('Voices', 'langVoiceOpts', 6, 'Voices, cutscene dubs and story movie audio.') +
    _segrow('Text and subtitles', 'langTextOpts', 6, 'Subtitles, messages and menus.') +
    '          <p class="note" style="margin:6px 0 0;font-size:11.5px">Japanese is on your US disc. French, German, Italian and '
    'Spanish come from your own Shinobi (Europe) disc image: nothing is copied. Changes apply the next time the game starts; '
    'subtitles are switched on or off in the game&#39;s own Options menu.</p>\n'
    '        </sc-if>\n        <sc-if value="{{tabController}}"')

# ---- LOCAL PATCH: the Feature Set page: everything the recomp does now, and what is planned. Edit the lists here. ----
FEATURES_NOW = [
    ('Graphics', [
        ('Field of View slider (#23)', ''),
        ('Up to 4K and 8K resolution (D3D11 or Vulkan)', ''),
        ('Widescreen: 16:9, 21:9, 32:9 and Auto', ''),
        ('IMAX mode: no bars, even in movies', 'F7'),
        ('HD texture packs, switched live', 'Shift+F5'),
        ('HD Texture Pack v2: 372 textures', ''),
        ('FXAA, Edge AA, sharpening, anisotropic filtering', ''),
        ('Screen filters: Sharp, Smooth, Scanlines, CRT', ''),
        ('Dithering, mipmapping and upscaling fixes', ''),
        ('Picture centred, no black strip', ''),
    ]),
    ('Performance', [
        ('Lighter on the CPU', ''),
        ('Runs natively as PC code, not emulated', ''),
        ('Frame generation up to 360 FPS', 'F4'),
        ('Low latency mode', 'F6'),
        ('Performance overlay (FPS, CPU, GPU)', 'F2'),
        ('Steady frame pacing at high frame rates', ''),
        ('Shaders cached for fast starts', ''),
        ('Runs on older CPUs without AVX2', ''),
    ]),
    ('Sound', [
        ('Streamed music and movies in true stereo', ''),
        ('Game volume', ''),
        ('Jukebox: your music for all 30 tracks', ''),
        ('Your own launcher music', ''),
    ]),
    ('Languages', [
        ('Japanese voices and text, from your US disc', ''),
        ('French, German, Italian, Spanish (EU disc)', ''),
        ('Dubbed movies and subtitles', ''),
    ]),
    ('Controls', [
        ('PC mouse and key layout', ''),
        ('Mouse in game menus', ''),
        ('Separate move keys (#20)', ''),
        ('Bindable mouse buttons (#20)', ''),
        ('Enter in every menu', ''),
        ('DualSense Bluetooth reconnect', ''),
        ('DualSense HD haptics over USB or Bluetooth', ''),
        ('L2 adaptive trigger for ninja magic', ''),
        ('Haptics switch and strength in the Mod Menu', ''),
        ('Menu haptics: taps and scrolling beads', ''),
        ('Full controller and keyboard remapping', ''),
        ('WASD movement and 1:1 mouse aiming', 'F10'),
        ('Camera invert and stick dead zone', ''),
        ('PlayStation, Xbox and Nintendo pads detected', ''),
    ]),
    ('Mods', [
        ('No camera recentering (#22)', ''),
        ('Final boss health slider', ''),
        ('Fair final boss', 'F9'),
        ('Infinite health', 'F8'),
        ('Uncursed Sword: no sword hunger', 'Shift+F7'),
        ('Unlock everything', 'Shift+F6'),
        ('Close-up camera', 'F3'),
        ('Hide the HUD', 'F5'),
        ('Scarf colours, remove scarf, scarf collision', ''),
        ('Skip the start-up logos', ''),
    ]),
    ('Launcher', [
        ('Save states, 3 slots (#16)', 'Ctrl+F1'),
        ('Save / Load in F1 menu', ''),
        ('Restart the game', 'Ctrl+R x2'),
        ('Shortcuts panel', 'H'),
        ('Mod Menu with category tabs', ''),
        ('Sets the game up from your own disc image', ''),
        ('Automatic updates', ''),
        ('In-game menu with every setting live', 'F1'),
        ('Saves kept safe, with automatic backups', ''),
        ('Game manual viewer', ''),
        ('Full controller navigation', ''),
        ('Rebuilt launcher: boot screen, theme, pages', ''),
        ('Works on Linux through Proton', ''),
    ]),
]
# LOCAL PATCH: new in this version: these rows get gold sparks and a NEW tag on the Feature Set page.
FEATURES_NEW = {
    'Field of View slider (#23)',
    'HD Texture Pack v2: 372 textures',
    'Lighter on the CPU',
    'PC mouse and key layout',
    'Mouse in game menus',
    'Separate move keys (#20)',
    'Bindable mouse buttons (#20)',
    'Enter in every menu',
    'DualSense Bluetooth reconnect',
    'No camera recentering (#22)',
    'Final boss health slider',
    'Save states, 3 slots (#16)',
    'Save / Load in F1 menu',
    'Restart the game',
    'Shortcuts panel',
    'Mod Menu with category tabs',
}
# LOCAL PATCH: fixed in this version (text, tag: a GitHub issue number or '' for FIXED). Shown between "Available now"
# and "Coming in future updates".
FEATURES_FIXED = [
    ('Sound effects lost after about 10 Retries', '#17'),
    ('"Game not found" on a fresh install', '#13'),
    ('RecompLauncher / Wine setup failing', '#15'),
    ('Mouse camera barely turned', '#20'),
    ('WASD changed the ninjutsu scroll', '#20'),
    ('Mouse clicks pressing two buttons', '#20'),
    ('Blue line in the 7-A boss cutscene', '#18'),
    ('Missing rank text on stage results', ''),
    ('Stuck in pits with Infinite health on', ''),
    ('Enter only worked in the main menu', ''),
    ('DualSense Bluetooth dropouts', ''),
    ('D-pad on the launcher Graphics page', ''),
    ('Black boxes in the HD texture tool', ''),
]
FEATURES_SOON = [
    'IMAX for in-game (non-movie) cutscenes',
    '4K on Vulkan fixes (intro, pause menu)',
    '21:9 effect fixes (smoke, blood, kanji)',
    'More HD textures with every update',
    'Easy Setup in RecompLauncher',
]


def _features_html():
    import html as _h
    import random
    out = ['          <div class="fver"><span>Installed version</span><b>{{versionText}}</b></div>\n',
           '          <div class="flist" tabindex="0" aria-label="Feature set (scroll with the right stick)">\n',
           '            <div class="fhead"><span>Available now</span></div>\n']
    nnew = 0
    for group, rows in FEATURES_NOW:
        out.append('            <div class="fgroup">%s</div>\n' % _h.escape(group))
        for text, key in rows:
            kbd = ('<kbd>%s</kbd>' % _h.escape(key)) if key else ''
            if text in FEATURES_NEW:
                # a few gold sparks drifting up out of the letters, here and there (fixed random spots and
                # timings per row, so the page is the same every build)
                rnd = random.Random(nnew * 7919 + 17)
                nnew += 1
                sparks = ''.join(
                    '<b style="left:%d%%;top:%d%%;--dx:%dpx;--dy:-%dpx;animation-duration:%.1fs;animation-delay:-%.1fs"></b>'
                    % (rnd.randint(4, 96), rnd.randint(25, 75), rnd.randint(-7, 7), rnd.randint(14, 24),
                       rnd.uniform(4.5, 7.5), rnd.uniform(0, 7.5)) for _ in range(2))
                out.append('            <div class="frow fnew"><i></i><span><span class="ftx">%s%s</span></span>%s'
                           '<em class="new">NEW</em></div>\n' % (_h.escape(text), sparks, kbd))
            else:
                out.append('            <div class="frow"><i></i><span>%s</span>%s</div>\n' % (_h.escape(text), kbd))
    assert nnew == len(FEATURES_NEW), 'a FEATURES_NEW row is not in FEATURES_NOW'
    out.append('            <div class="fhead soon"><span>Fixed in this version</span></div>\n')
    for text, tag in FEATURES_FIXED:
        out.append('            <div class="frow ffix"><i></i><span>%s</span><em>%s</em></div>\n'
                   % (_h.escape(text), _h.escape(tag or 'FIXED')))
    out.append('            <div class="fhead soon"><span>Coming in future updates</span></div>\n')
    for text in FEATURES_SOON:
        out.append('            <div class="frow fsoon"><i></i><span>%s</span><em>SOON</em></div>\n' % _h.escape(text))
    out.append('          </div>\n')
    return ''.join(out)


template = must_replace(template, '        </sc-if>\n        <sc-if value="{{tabController}}"',
    '        </sc-if>\n'
    '        <sc-if value="{{tabFeatures}}" hint-placeholder-val="{{false}}">\n' + _features_html() +
    '        </sc-if>\n        <sc-if value="{{tabController}}"')
template = must_replace(template, '@media (prefers-reduced-motion:reduce)',
    '.fver{display:flex;align-items:center;justify-content:space-between;margin:0 0 8px;padding:7px 12px;border:1px solid rgba(255,201,53,.35);'
    'border-radius:4px;background:linear-gradient(90deg,rgba(255,201,53,.10),rgba(255,201,53,.02))}'
    '.fver span{font-family:Rajdhani,sans-serif;font-weight:700;font-size:12px;letter-spacing:.16em;text-transform:uppercase;color:#a9a9b0}'
    '.fver b{font-family:Rajdhani,sans-serif;font-weight:700;font-size:17px;letter-spacing:.08em;color:#ffc935;text-shadow:0 0 8px rgba(255,190,40,.35)}'
    '.segs .seg.locked{opacity:.35}'
    '.flist{max-height:330px;overflow-y:auto;overflow-x:hidden;padding:0 10px 12px 2px;outline:none;scrollbar-width:thin;'
    'scrollbar-color:#3a3a40 transparent;-webkit-mask:linear-gradient(180deg,#000 0,#000 calc(100% - 22px),transparent 100%)}'
    # selected with the controller: no box over the text, just a thin gold bar down its left edge (beats the general
    # 'html body .pad-focus.pad-focus.pad-focus' selection box)
    'html body .flist.pad-focus.pad-focus.pad-focus.pad-focus{outline:none!important;box-shadow:inset 2px 0 0 rgba(255,201,53,.75)!important;z-index:auto!important}'
    '.fhead{display:flex;align-items:center;gap:10px;font-family:Rajdhani,sans-serif;font-weight:700;font-size:15px;letter-spacing:.18em;'
    'text-transform:uppercase;color:#ffc935;text-shadow:0 0 8px rgba(255,190,40,.35);margin:4px 0 2px}'
    '.fhead::after{content:"";flex:1;height:1px;background:linear-gradient(90deg,rgba(255,201,53,.45),transparent)}'
    '.fhead.soon{margin-top:18px}'
    '.fgroup{font-family:Rajdhani,sans-serif;font-weight:700;font-size:12px;letter-spacing:.14em;text-transform:uppercase;color:#7d7d86;margin:12px 0 4px}'
    '.frow{display:flex;align-items:center;gap:9px;padding:4px 0;font-size:12.5px;line-height:1.3;color:#e6e6ea;border-bottom:1px solid #1c1c20}'
    '.frow i{flex:none;width:5px;height:5px;border-radius:50%;background:#ffc935;box-shadow:0 0 5px rgba(255,201,53,.6)}'
    '.frow span{flex:1;min-width:0}'
    '.frow kbd{flex:none;font-family:Rajdhani,sans-serif;font-weight:700;font-size:11px;letter-spacing:.03em;padding:1px 6px;'
    'border:1px solid #4a4a52;border-radius:3px;color:#d4d4da;background:#141417;white-space:nowrap}'
    '.frow.fnew>span{white-space:nowrap;overflow:visible}'
    '.frow.fnew .ftx{position:relative;color:#fff3d0}'
    '.frow.fnew .ftx b{position:absolute;width:2px;height:2px;border-radius:50%;background:#ffe9a0;pointer-events:none;'
    'box-shadow:0 0 4px 1px rgba(255,201,53,.85);opacity:0;animation:fspark 6s ease-out infinite}'
    '@keyframes fspark{0%{transform:translate(0,0) scale(1);opacity:0}5%{opacity:.95}'
    '38%{transform:translate(var(--dx),var(--dy)) scale(.25);opacity:0}100%{transform:translate(var(--dx),var(--dy));opacity:0}}'
    '.frow em.new{font-size:9px;letter-spacing:.1em;padding:0 5px;color:#141417;'
    'background:linear-gradient(90deg,#ffc935,#ffe08a,#ffc935);background-size:200% 100%;border-color:#ffc935;'
    'animation:fshine 3s linear infinite}'
    '@keyframes fshine{0%{background-position:100% 0}100%{background-position:-100% 0}}'
    '.frow.fsoon{color:#9a9aa2}.frow.fsoon i{background:#55555c;box-shadow:none}'
    '.frow.ffix i{background:#5fd38a;box-shadow:0 0 5px rgba(95,211,138,.6)}'
    '.frow.ffix em{color:#5fd38a;border-color:rgba(95,211,138,.5)}'
    '.frow em{flex:none;font-style:normal;font-family:Rajdhani,sans-serif;font-weight:700;font-size:10px;letter-spacing:.14em;'
    'padding:1px 6px;border-radius:3px;color:#ffc935;border:1px solid rgba(255,201,53,.5)}'
    '@media (prefers-reduced-motion:reduce)')

# ---- LOCAL PATCH: the Jukebox tab (ps2_jukebox.cpp in the game): any of the game's music tracks replaced by a file
# of the player's own, copied into jukebox/ (kept through updates). ----
template = must_replace(template, '        </sc-if>\n        <sc-if value="{{tabController}}"',
    '        </sc-if>\n'
    '        <sc-if value="{{tabJukebox}}" hint-placeholder-val="{{false}}">\n' +
    # Sound page: the game's volume and the launcher's music first, then the Jukebox.
    # LOCAL PATCH: Game Volume as a slider (0..100 in steps of 5; left / right on a controller)
    '        <div>\n'
    '          <div class="fl" title="Music, voices and effects together."><span>Game Volume</span><em>{{volName}}</em></div>\n'
    '          <div class="rng"><div class="trk" style="background: {{volBg}}"></div><input type="range" min="0" max="100" step="5" '
    'value="{{volNum}}" sc-camel-on-change="{{setVol}}" aria-label="Game volume"></div>\n'
    '        </div>\n' +
    '        <div class="jrow jfoot" style="margin:2px 0 4px"><div class="jname" title="Any mp3, ogg, m4a, wav, flac or opus file of yours. Kept through updates.">'
    '<b>Launcher Music</b><small>{{musicName}}</small></div>'
    '<button class="btn" sc-camel-on-click="{{pickMusic}}"><span class="lab">Choose</span></button>'
    '<button class="btn" sc-camel-on-click="{{defaultMusic}}"><span class="lab">Default</span></button></div>\n'
    '          <div class="fl" style="margin-top:4px">Jukebox</div>\n'
    '          <p class="note" style="margin:0 0 4px;font-size:11.5px">Your own mp3, ogg or wav for any track. '
    'Loops, and follows the game&#39;s volume and fades.</p>\n'
    '          <div class="jlist">\n'
    '            <sc-for list="{{jukeRows}}" as="j" hint-placeholder-count="6">\n'
    '              <div class="jrow"><div class="jname" title="{{j.code}}"><b>{{j.label}}</b><small style="{{j.style}}">{{j.file}}</small></div>'
    '<button class="btn" sc-camel-on-click="{{j.pick}}"><span class="lab">Choose</span></button>'
    '<button class="btn" sc-camel-on-click="{{j.reset}}"><span class="lab">Default</span></button></div>\n'
    '            </sc-for>\n'
    '          </div>\n'
    '          <div class="jrow jfoot"><div class="jname"><small>Or put files in the jukebox folder, named after the track (e.g. BGM01_00_00T.mp3).</small></div>'
    '<button class="btn" sc-camel-on-click="{{jukeFolder}}"><span class="lab">Open folder</span></button></div>\n'
    '        </sc-if>\n        <sc-if value="{{tabController}}"')

# ---- LOCAL PATCH: invert camera switches in Configure Controller (Gamepad view) ----
template = must_replace(template, '              <p class="note">{{bindNote}}</p>',
    '              <sc-if value="{{padView}}" hint-placeholder-val="{{false}}">\n'
    '                <div style="display:flex;gap:22px;margin:10px 0 2px">\n'
    '                  <label class="chk"><input type="checkbox" checked="{{invX}}" sc-camel-on-change="{{toggleInvX}}">' + CHK + 'Invert camera left/right</label>\n'
    '                  <label class="chk"><input type="checkbox" checked="{{invY}}" sc-camel-on-change="{{toggleInvY}}">' + CHK + 'Invert camera up/down</label>\n'
    '                </div>\n'
    '              </sc-if>\n'
    '              <p class="note">{{bindNote}}</p>')

# Bottom of the left column: the ownership notice above the PlayStation / SEGA logos, the studio and author lines below.
template = must_replace(template,
                        '      <div class="foot">\n'
                        '        <div style="display:flex;align-items:center;gap:14px">',
                        '      <div class="foot-wrap">\n'
                        '      <p class="own">Requires your own original copy of Shinobi (PS2).<br>Fan project, not affiliated '
                        'with SEGA or Sony.</p>\n'
                        '      <div class="foot">\n'
                        '        <div style="display:flex;align-items:center;gap:14px">')
template = must_replace(template,
                        '<span class="tra-b">TR<i>A</i></span></div>\n      </div>\n    </div>',
                        '<span class="tra-b">TR<i>A</i></span>'
                        # LOCAL PATCH: Refresh: the launcher starts its page again (loading screen, intro, settings from disk)
                        '<button class="refbtn" title="Refresh the launcher" aria-label="Refresh the launcher" '
                        'sc-camel-on-click="{{refreshLauncher}}"><span>&#8635;</span>Refresh</button></div>\n      </div>\n'
                        '      </div>\n    </div>')
# The studio and author lines sit under the SEGA / Sony lines, beside the TRA logo and Refresh (one block, no overflow).
template = must_replace(template,
                        '<span class="mark">Shinobi &copy; SEGA<br>PlayStation is a trademark of Sony</span>',
                        '<div class="markcol"><span class="mark">Shinobi &copy; SEGA<br>PlayStation is a trademark of Sony</span>'
                        '<p class="studio">Developed by <b>Twilight Orchard</b></p>'
                        '<p class="studio">Created by <b>TheRealAfsan</b></p></div>')
template = must_replace(template, '<p>Launcher and recompilation tooling by TheRealAfsan.</p>',
                        '<p>Developed by Twilight Orchard. Created by TheRealAfsan.</p>\n'
                        '                <p>Launcher and recompilation tooling by TheRealAfsan.</p>')

# "Effects Quality" is the picture sharpening (the game's own shader): named for what it does.
template = must_replace(template, '<div class="fl"><span>Effects Quality</span><em>{{fxName}}</em></div>',
                        '<div class="fl"><span>Sharpening</span><em>{{fxName}}</em></div>')
template = must_replace(template, 'aria-label="Effects quality"', 'aria-label="Sharpening"')
template = must_replace(template, '<div class="fl"><span>Texture Quality</span><em>{{texName}}</em></div>',
                        '<div class="fl" title="Anisotropic filtering: sharper floors and walls seen at an angle">'
                        '<span>Texture Quality</span><em>{{texName}}</em></div>')

# Renderer and the new Frame Rate as one-line rows (label left, buttons right) so the panel still fits.
template = must_replace(template, '<div>\n          <div class="fl">Renderer</div>', '<div class="inl">\n          <div class="fl">Renderer</div>')
template = must_replace(template, '        <div>\n          <label class="chk"><input type="checkbox" checked="{{ws}}"',
                        '        <div class="inl">\n          <div class="fl">Frame Rate</div>\n'
                        '          <div class="segs" role="group" aria-label="Frame rate">\n'
                        '            <sc-for list="{{fpsOpts}}" as="o" hint-placeholder-count="4">\n'
                        '              <button class="seg {{o.cls}}" aria-pressed="{{o.on}}" title="{{o.tip}}" '
                        'sc-camel-on-click="{{o.pick}}">{{o.name}}</button>\n'
                        '            </sc-for>\n          </div>\n        </div>\n\n'
                        '        <div>\n          <label class="chk"><input type="checkbox" checked="{{ws}}"')
# Widescreen: the label names the shape; chips for 16:9 / 21:9 / 32:9 / Auto in the design's MSAA-chip style.
template = must_replace(template, 'Widescreen Hack (16:9)</label>',
                        'Widescreen Hack ({{wsName}})</label>\n'
                        '          <sc-if value="{{ws}}" hint-placeholder-val="{{false}}"><div class="sub" role="group" '
                        'aria-label="Widescreen shape" style="margin:0 0 4px"><sc-for list="{{wsOpts}}" as="w" '
                        'hint-placeholder-count="4"><button class="chipb {{w.cls}}" aria-pressed="{{w.on}}" '
                        'sc-camel-on-click="{{w.pick}}">{{w.name}}</button></sc-for></div></sc-if>')
template = must_replace(template, '<p>Launcher and recompilation tooling by TheRealAfsan.</p>',
                        '<p>Launcher and recompilation tooling by TheRealAfsan.</p>\n'
                        '                <h4>Built with</h4>\n'
                        '                <p>PS2Recomp (ps2xRecomp / ps2xRuntime). Graphics: PCSX2\'s GS through the '
                        'pcsx2-gs-bridge; sound: PCSX2\'s SPU2 core (both GPL-3.0+, PCSX2 Dev Team). raylib, Microsoft '
                        'WebView2.</p>')

# ---------------------------------------------------------------- game file (the user's own disc image)
# A "Game File" button and dialog: Browse... for the Shinobi (USA) disc image, check that it is the right game, Set up copies
# the game's files out of it (launcher_iso.cpp). Nothing of the game ships with this program.
template = must_replace(template, '@media (prefers-reduced-motion:reduce)',
                        '.pathrow{display:flex;align-items:center;gap:10px;margin:12px 0 4px}.pathrow .btn{width:auto;flex:0 0 auto;padding:0 26px}'
                        '.pathtxt{flex:1;min-width:0;padding:9px 11px;border:1px solid #b5b5bb;background:#f2f2f4;color:#222;'
                        'font-size:12.5px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;direction:rtl;text-align:left}'
                        '.gamebox .go{margin-top:14px;min-width:240px;position:relative;overflow:hidden}'
                        '@media (prefers-reduced-motion:reduce)')
template = must_replace(template,
                        '<button class="btn" sc-camel-on-click="{{openMods}}"><span class="lab">Mod Menu</span></button>',
                        '<button class="btn" sc-camel-on-click="{{openMods}}"><span class="lab">Mod Menu</span></button>\n'
                        '        <button class="btn {{gameBtnCls}}" sc-camel-on-click="{{openGame}}"><span class="lab">{{gameBtnLabel}}</span></button>')
template = must_replace(template, '            <sc-if value="{{showCredits}}" hint-placeholder-val="{{false}}">',
                        '            <sc-if value="{{showGame}}" hint-placeholder-val="{{false}}">\n'
                        '              <div class="gamebox">\n'
                        '                <p class="note" style="margin-top:0">{{gameIntro}}</p>\n'
                        '                <div style="display:flex;flex-direction:column;gap:5px;margin:10px 0 2px">'
                        '<sc-for list="{{gameSteps}}" as="g" hint-placeholder-count="3">'
                        '<div style="display:flex;align-items:center;gap:10px;font-size:13.5px">'
                        '<span style="{{g.mark}}">{{g.icon}}</span><b style="{{g.text}}">{{g.label}}</b></div>'
                        '</sc-for></div>\n'
                        '                <div class="pathrow"><span class="pathtxt" title="{{isoPath}}">{{isoPath}}</span>'
                        '<button class="btn sm" sc-camel-on-click="{{browseIso}}"><span class="lab">Browse...</span></button></div>\n'
                        '                <p class="note">{{isoStatus}}</p>\n'
                        '                <label style="display:flex;align-items:center;gap:10px;margin:12px 0 0;cursor:pointer;font-size:13.5px">'
                        '<input type="checkbox" checked="{{owned}}" sc-camel-on-change="{{toggleOwned}}" '
                        'style="width:18px;height:18px;accent-color:#b3122a;cursor:pointer" aria-label="I own this game">'
                        '<span>I own this copy of Shinobi and made the disc image from my own disc.</span></label>\n'
                        '                <button class="btn go" disabled="{{installBusy}}" sc-camel-on-click="{{installIso}}">'
                        '<sc-if value="{{installBusy}}" hint-placeholder-val="{{false}}"><span class="fill" style="width: {{installFill}}"></span></sc-if>'
                        '<span class="lab">{{installLabel}}</span></button>\n'
                        '                <p class="note">{{installNote}}</p>\n'
                        '                <div class="pathrow"><span class="pathtxt" title="{{savesPath}}">{{savesPath}}</span>'
                        '<button class="btn sm" sc-camel-on-click="{{openSaves}}"><span class="lab">Open saves folder</span></button></div>\n'
                        '                <p class="note">Your saves are kept in this folder, apart from the game files, so they stay yours when the game files are set up again.</p>\n'
                        '              </div>\n'
                        '            </sc-if>\n\n'
                        '            <sc-if value="{{showCredits}}" hint-placeholder-val="{{false}}">')

# ---------------------------------------------------------------- logic
# Left column: five buttons no longer fit, so tighter buttons and gaps; the saved game file's place sits under its button.
template = must_replace(template, '@media (prefers-reduced-motion:reduce)',
                        '.left .btns{gap:6px}.left{gap:8px}.th{height:50px}.card{padding:9px 12px 10px}.card p{font-size:12px;line-height:1.3}'
                        '.foot img[alt="PlayStation"]{height:26px}'
                        '.gfile{margin:-2px 0 0;font-family:Rajdhani,sans-serif;font-size:11px;font-weight:600;color:#4a4a52;'
                        'overflow:hidden;text-overflow:ellipsis;white-space:nowrap;padding-left:2px}'
                        '.gfile.warn{color:#b3122a}'
                        '@media (prefers-reduced-motion:reduce)')
template = must_replace(template,
                        '<button class="btn {{gameBtnCls}}" sc-camel-on-click="{{openGame}}"><span class="lab">{{gameBtnLabel}}</span></button>',
                        '<button class="btn {{gameBtnCls}}" sc-camel-on-click="{{openGame}}"><span class="lab">{{gameBtnLabel}}</span></button>\n'
                        '        <p class="gfile {{gfileCls}}" title="{{isoPath}}">{{gameCaption}}</p>')

logic_start = template.index('class Component extends DCLogic')
logic_end = template.index('</script>', logic_start)
logic = template[logic_start:logic_end]

# LOCAL PATCH: the Jukebox's tracks (the BGM*.ADX files in ROFSSMP.CVM) and the names the page shows. *_00_00T is a
# stage's main music; *_03_00T plays later in the stage (the boss area); the E tracks are short pieces (events,
# stage clear). The game logs "[jukebox] <TRACK> started" each time one starts, to check a name against.
_JUKE = [('BGM00_00_01T', 'Title & menus'), ('BGM00_00_03T', 'Title & menus 2')]
_STAGE_FILES = {
    1: ['00_00T', '03_00T', '00_00E', '00_01E', '00_02E'], 2: ['00_00T', '00_00E', '00_01E'], 3: ['00_00T', '00_00E'],
    4: ['00_00T', '00_00E'], 5: ['00_00T', '00_00E', '00_01E', '00_02E', '00_03E'], 6: ['00_00T', '03_00T', '00_00E'],
    7: ['00_00T', '01_00T', '03_00T', '00_00E', '00_01E'], 8: ['00_00T', '03_00T', '00_00E']}
for _st, _files in _STAGE_FILES.items():
    _short = 0
    for _f in _files:
        if _f == '00_00T':
            _name = 'Stage %d' % _st
        elif _f == '01_00T':
            _name = 'Stage %d - part 2' % _st
        elif _f == '03_00T':
            _name = 'Stage %d - boss' % _st
        else:
            _short += 1
            _name = 'Stage %d - short piece %d' % (_st, _short)
        _JUKE.append(('BGM%02d_%s' % (_st, _f), _name))
JUKEBOX_JS = json.dumps([list(t) for t in _JUKE])


def L(old, new, count=1):
    global logic
    logic = must_replace(logic, old, new, count)


# Host bridge, initial state from the host, real defaults.
L("""    this.defaults = { res: '1080p', renderer: 'Vulkan', ws: true, vsync: false, tex: 2, fx: 3, aa: 'Off', msaa: 4 };
    this.defBinds = { up: 'W', down: 'S', left: 'A', right: 'D', cross: 'K', circle: 'L', square: 'J', triangle: 'I', l1: 'Q', r1: 'E', l2: 'Z', r2: 'C', start: 'Enter', select: 'Backspace' };
    this.state = Object.assign({}, this.defaults, {
      saved: this.sig(this.defaults),
      resOpen: false, modal: null, guide: 0,
      launch: 'idle', progress: 0, toast: '',
      device: 'keyboard', listening: null,
      bind: Object.assign({}, this.defBinds),
      mods: { intro: true, fps: false, tex: false, dbg: false }
    });""",
  """    // Defaults: the game's own (ps2_settings.cpp) where it has them.
    this.defaults = { res: '1440p', renderer: 'D3D11', fps: '60', ws: true, wsShape: '16:9', vsync: true, tex: 1, fx: 1, aa: 'FXAA', msaa: 4, blend: 'basic' };
    this.defBinds = { up: 'UP', down: 'DOWN', left: 'LEFT', right: 'RIGHT,E', cross: 'X,SPACE', circle: 'C,ESCAPE', square: 'Z,KP_0', triangle: 'Q,V,KP_1', l1: 'F', r1: 'R', l2: 'LEFT_SHIFT', r2: 'RIGHT_SHIFT', start: 'ENTER', select: 'TAB' };
    this.defMods = { studio: false, nscale: true, rr5fix: false, dropst: false, record: false, intro: true, fastvu: false, dbg: false, full: true, diag: false, legacy: false, k8: false };
    const init = window.__launcherInit || null;
    this.manualOnly = !!(init && init.manualOnly);
    const start = Object.assign({}, this.defaults, init ? init.settings : {});
    this.state = Object.assign({}, start, {
      resOpen: false, modal: null, guide: 0,
      launch: 'idle', progress: 0, toast: '',
      device: 'keyboard', listening: null,
      bind: Object.assign({}, this.defBinds, init ? init.binds : {}),
      mods: Object.assign({}, this.defMods, init ? init.mods : {})
    });
    this.state.saved = this.sig(this.state);
    this.host = (window.chrome && window.chrome.webview) ? window.chrome.webview : null;
    if (this.host) {
      this.host.addEventListener('message', (e) => this.onHost(e.data));
    }
    // In-game mode: the game's pause menu (ps2_ingame_menu.cpp opens this page with init.ingame).
    this.ingame = !!(init && init.ingame);
    this.state.restart = !!(init && init.restart);
    this.state.quitArmed = false;
    // LOCAL PATCH (save state slots): the slot the game has picked, and when each slot was saved ('' = empty).
    this.state.stateSlot = (init && init.stateSlot) || 1;
    this.stateSlots = (init && init.stateSlots) || ['', '', ''];
    document.documentElement.classList.toggle('ingame', this.ingame);
    // LOCAL PATCH: the boot screen. About 2 s of a retro LOADING bar (gold, in steps), then it fades away and the
    // page's own intro animations start from the beginning (they wait, paused, under it). Not in the F1 menu.
    if (!this.ingame && !document.querySelector('.boot')) {
      const root = document.documentElement;
      root.classList.add('booting');
      const boot = document.createElement('div');
      boot.className = 'boot';
      boot.setAttribute('aria-hidden', 'true');
      const BARS = 28; // the bar graph: thin bars rising like a tachometer's, the last few in yellow-green (redline)
      let graph = '';
      for (let i = 0; i < BARS; i++) graph += '<i style="height:' + (28 + 72 * Math.pow(i / (BARS - 1), 1.3)).toFixed(1) + '%"' + (i >= BARS - 4 ? ' class="hot-zone"' : '') + '></i>';
      boot.innerHTML = '<video class="boot-vid" src="boot.mp4" autoplay muted loop playsinline></video><div class="boot-shade"></div><div class="boot-in"><div class="boot-top"><span class="boot-word">LOADING<i class="boot-dots"><b>.</b><b>.</b><b>.</b></i></span>'
        + '<span class="boot-pct">0%</span></div><div class="boot-graph">' + graph + '</div>'
        + '<div class="boot-scale"><span>0</span><span>25</span><span>50</span><span>75</span><span>100</span></div>'
        + '<div class="boot-sub">SHINOBI &middot; PS2 RECOMP</div>'
        + '<div class="boot-credits"><span>DEVELOPED BY TWILIGHT ORCHARD</span><span>THEREALAFSAN</span><span class="boot-tra">TRA</span></div></div>';
      document.body.appendChild(boot);
      const bars = Array.from(boot.querySelectorAll('.boot-graph i')), pct = boot.querySelector('.boot-pct');
      const total = 4400, steps = BARS, start = performance.now(); // about 4.5 s with the hand-over
      const tick = () => {
        const k = Math.min(1, (performance.now() - start) / total);
        // eased, then snapped to whole blocks: the bar fills in chunky retro steps
        const eased = 1 - Math.pow(1 - k, 1.6);
        const shown = Math.min(steps, Math.floor(eased * steps + 0.0001)) / steps;
        const lit = Math.round(shown * steps);
        bars.forEach((el, i) => { el.classList.toggle('on', i < lit); el.classList.toggle('hot', i < lit && el.classList.contains('hot-zone')); });
        pct.textContent = Math.round(eased * 100) + '%';
        if (k < 1) { requestAnimationFrame(tick); return; }
        bars.forEach((el) => { el.classList.add('on'); el.classList.toggle('hot', el.classList.contains('hot-zone')); }); pct.textContent = '100%';
        setTimeout(() => {
          boot.classList.add('boot-out');
          root.classList.remove('booting');
          // the intro animations start now, from their beginning
          document.getAnimations().forEach((a) => { if (!(a.effect && a.effect.target && a.effect.target.closest && a.effect.target.closest('.boot'))) { try { a.currentTime = 0; a.play(); } catch (e) {} } });
          setTimeout(() => boot.remove(), 520);
        }, 160);
      };
      requestAnimationFrame(tick);
    }
    this.navSetup();
    // Launcher music (launcher_assets/music.*, found by the host): loops while the launcher is open, fades in, pauses
    // while the game runs. The mute button in the title bar is remembered (launcher.ini).
    this.state.muted = !!(init && init.muted);
    document.documentElement.classList.toggle('muted', this.state.muted);
    this.music = null;
    this.state.musicName = (init && init.musicName) || 'Original theme';
    this.state.juke = (init && init.jukebox) || {};
    // The installed version (version.txt beside the launcher; shown at the top of the Feature Set page).
    this.appVersion = (init && init.version) || '';
    // The kind of controller connected, from the host (launcher_pads.cpp; the game in the F1 menu): its button names.
    this.state.padKind = (init && init.padKind) || null;
    this._hostKind = !!(init && 'padKind' in init);""" + chr(10) + "    window.JUKEBOX_TRACKS = " + JUKEBOX_JS + ";" + """
    // The host names the file (null = none); a host that does not know about music yet -> music.mp3 beside this page.
    this.setMusic((init && 'music' in init) ? init.music : 'music.mp3', this.state.musicName === 'Original theme' ? 24 : 0);""")

L("""  sig(s) {""",
  """  musicFade(to, ms, then) {
    if (!this.music) return;
    clearInterval(this.fadeTimer);
    const from = this.music.volume, start = Date.now();
    this.fadeTimer = setInterval(() => {
      const k = Math.min((Date.now() - start) / ms, 1);
      this.music.volume = from + (to - from) * k;
      if (k >= 1) { clearInterval(this.fadeTimer); if (then) then(); }
    }, 30);
  }

  // LOCAL PATCH: start (or change to) a track; null = none. The launcher's Choose... / Default swap it at once.
  setMusic(src, fromSec) {
    if (this.music) { clearInterval(this.fadeTimer); this.music.pause(); this.music = null; }
    document.documentElement.classList.toggle('nomusic', !src);
    if (!src) return;
    const music = new Audio(src);
    music.addEventListener('error', () => {
      if (this.music !== music) return;
      this.music = null;
      document.documentElement.classList.add('nomusic');
      this.toastMsg('That music file could not be played');
    });
    // LOCAL PATCH: the launcher's own track starts at 0:24 (its bass drop, 28.55 s, lands on the art's slam and shake) and loops back there (the player's own tracks play whole).
    if (fromSec) {
      music.loop = false;
      music.addEventListener('loadedmetadata', () => { try { if (music.currentTime < fromSec - 1) music.currentTime = Math.min(fromSec, Math.max(0, music.duration - 1)); } catch (e) {} }, { once: true });
      music.addEventListener('ended', () => { try { music.currentTime = fromSec; } catch (e) {} const p = music.play(); if (p && p.catch) p.catch(() => {}); });
    } else music.loop = true;
    music.volume = 0;
    music.muted = this.state.muted;
    this.music = music;
    if (!this.state.launch || this.state.launch === 'idle') {
      // LOCAL PATCH: when the launcher opens, the music comes in 5 s after the loading screen starts; a track picked
      // later plays at once.
      this.musicOn(); // at once: the music starts with the launcher (its start point times the bass drop to the logo)
    }
  }

  musicOn() {
    if (!this.music) return;
    const p = this.music.play();
    // If the browser still wants a click before sound, start on the first one.
    if (p && p.catch) p.catch(() => document.addEventListener('pointerdown', () => this.musicOn(), { once: true }));
    this.musicFade(0.12, 1500); // 12% volume (the track is mastered loud)
  }

  musicOff() {
    if (!this.music) return;
    this.musicFade(0, 600, () => this.music.pause());
  }

  // Controller (Gamepad API) and arrow-key navigation, for the in-game menu and the launcher alike. Up/down move through
  // the controls in page order (inside the open dialog if there is one); left/right step sliders and pick the next/previous
  // option of a segmented row, else move too; Cross / Enter presses; Circle / Escape closes the dialog (in game: resumes);
  // Start resumes in game. A red outline shows where you are.
  navItems() {
    const scope = document.querySelector('.menu') || document.querySelector('.dlg') || document;
    return Array.from(scope.querySelectorAll('button, input, [tabindex="0"]')).filter((el) => {
      if (el.disabled || el.classList.contains('scrim-bg') || el.classList.contains('away')) return false;
      if (el.closest('.tb')) return false;
      const r = el.getBoundingClientRect();
      return r.width > 0 && r.height > 0 && getComputedStyle(el).visibility !== 'hidden';
    });
  }

  navFocus(el) {
    if (!el) return;
    if (el !== this.navCurrent && !this._stickMoving) this.hap('move');
    document.querySelectorAll('.pad-focus').forEach((e) => e.classList.remove('pad-focus'));
    const target = el.matches('input[type=checkbox]') ? (el.closest('label') || el) : el;
    target.classList.add('pad-focus');
    this.navCurrent = el;
    // Remembered by position and text, to put the marker back when the page redraws its controls.
    const all = this.navItems();
    this.navIdx = all.indexOf(el);
    this.navTxt = (target.textContent || '').trim();
    el.focus({ preventScroll: true });
    // LOCAL PATCH: in a scrolling list (the Mod Menu, the Jukebox) the list glides to the control with a short motion
    // blur; elsewhere the page just brings it into view.
    const scroller = this.navScroller(target);
    if (scroller) {
      const s = scroller.getBoundingClientRect(), r = target.getBoundingClientRect();
      const pad = 8;
      let to = scroller.scrollTop;
      if (r.top < s.top + pad) to += r.top - s.top - pad;
      else if (r.bottom > s.bottom - pad) to += r.bottom - s.bottom + pad;
      if (Math.abs(to - scroller.scrollTop) > 1) scroller.scrollTo({ top: to, behavior: 'smooth' });
      return;
    }
    target.scrollIntoView({ block: 'nearest' });
  }

  // LOCAL PATCH: the right stick scrolls the list in use (the selected control's, else the one on screen), faster the
  // further it is pushed; a selection scrolled out of sight moves to the nearest row still in view.
  stickScroll(v) {
    let scroller = this.navCurrent && document.contains(this.navCurrent) ? this.navScroller(this.navCurrent) : null;
    if (!scroller) {
      const scope = document.querySelector('.dlg') || document;
      scroller = Array.from(scope.querySelectorAll('div')).find((el) => {
        const oy = getComputedStyle(el).overflowY;
        if ((oy !== 'auto' && oy !== 'scroll') || el.scrollHeight <= el.clientHeight + 2) return false;
        const r = el.getBoundingClientRect();
        return r.width > 0 && r.height > 0;
      }) || null;
    }
    if (!scroller) return;
    // By the clock, not per frame (a 240 Hz screen would scroll 4x faster): about 60 px a second just past the dead
    // zone, up to 420 px a second pushed all the way. Fractions are kept, so slow scrolling still moves.
    const now = performance.now();
    const dt = this._stickT && now - this._stickT < 100 ? (now - this._stickT) / 1000 : 1 / 60;
    this._stickT = now;
    const push = (Math.abs(v) - 0.2) / 0.8;
    const pxPerSecond = 60 + 360 * push * push;
    this._stickCarry = (this._stickCarry || 0) + Math.sign(v) * pxPerSecond * dt;
    const whole = Math.trunc(this._stickCarry);
    this._stickCarry -= whole;
    if (!whole) return;
    const before = scroller.scrollTop;
    scroller.scrollTop += whole;
    const moved = Math.abs(scroller.scrollTop - before);
    if (moved) {
      this._beadPx = (this._beadPx || 0) + moved;
      if (this._beadPx >= 30) { this._beadPx = 0; this.hap('bead', Math.min(100, push * 100)); }
    }
    const cur = this.navCurrent;
    if (!cur || !scroller.contains(cur)) return;
    const s = scroller.getBoundingClientRect(), r = cur.getBoundingClientRect();
    if (r.top >= s.top && r.bottom <= s.bottom) return;
    let best = null, bestD = Infinity;
    this.navItems().forEach((el) => {
      if (!scroller.contains(el)) return;
      const e = el.getBoundingClientRect();
      if (e.top < s.top || e.bottom > s.bottom) return;
      const d = Math.abs((e.top + e.bottom) / 2 - (r.top + r.bottom) / 2);
      if (d < bestD) { bestD = d; best = el; }
    });
    if (best) { this._stickMoving = true; this.navFocus(best); this._stickMoving = false; }
  }

  // The nearest scrolling box (a list taller than its space) around a control, or null.
  // LOCAL PATCH: menu haptics. The host plays them on the controller (the launcher: DualSense output report or
  // XInput rumble; the F1 menu: the game's HD haptics), following the haptics switch and strength. Only while the page
  // is driven by a controller (no buzz under the mouse or keyboard).
  hap(kind, v) {
    if (!this.host) return;
    const now = performance.now();
    if (now - (this._padT || 0) > 450) return;
    const gap = kind === 'bead' ? 350 : kind === 'move' ? 30 : 60;
    this._hapT = this._hapT || {};
    if (now - (this._hapT[kind] || 0) < gap) return;
    this._hapT[kind] = now;
    this.host.postMessage('hap\\nkind=' + kind + '\\nv=' + Math.round(v || 0));
  }

  // LOCAL PATCH: the six language choices of the Languages page; the European four are locked until the disc is added.
  langOpts(s, key) {
    const LANGS = [['en', 'English'], ['ja', '日本語'], ['fr', 'Français'], ['de', 'Deutsch'], ['it', 'Italiano'], ['es', 'Español']];
    const cur = s[key] || 'en';
    return LANGS.map((o) => {
      const locked = !s.euDisc && ['fr', 'de', 'it', 'es'].indexOf(o[0]) >= 0;
      return { name: o[1], on: cur === o[0], cls: (cur === o[0] ? 'on' : '') + (locked ? ' locked' : ''),
        pick: () => {
          if (locked) { this.toastMsg('Add your European disc first (Choose, above)'); return; }
          this.setState({ [key]: o[0] });
        } };
    });
  }

  navScroller(el) {
    for (let p = el && el.parentElement; p && p !== document.body; p = p.parentElement) {
      const oy = getComputedStyle(p).overflowY;
      if ((oy === 'auto' || oy === 'scroll') && p.scrollHeight > p.clientHeight + 2) return p;
    }
    return null;
  }

  // After a redraw the marked control is a new element: find it again (same text, nearest place) and mark it.
  navRestore() {
    if (this.navIdx === undefined || this.navIdx < 0) return;
    const cur = this.navCurrent;
    const curTarget = cur && (cur.matches('input[type=checkbox]') ? (cur.closest('label') || cur) : cur);
    if (cur && document.contains(cur) && curTarget.classList.contains('pad-focus')) return;
    const items = this.navItems();
    let best = null, bestD = Infinity;
    items.forEach((el, i) => {
      const t = el.matches('input[type=checkbox]') ? (el.closest('label') || el) : el;
      if ((t.textContent || '').trim() !== this.navTxt) return;
      const d = Math.abs(i - this.navIdx);
      if (d < bestD) { bestD = d; best = el; }
    });
    if (!best) best = items[Math.min(this.navIdx, items.length - 1)];
    if (best) {
      document.querySelectorAll('.pad-focus').forEach((e) => e.classList.remove('pad-focus'));
      (best.matches('input[type=checkbox]') ? (best.closest('label') || best) : best).classList.add('pad-focus');
      this.navCurrent = best;
    }
  }

  navMove(delta) {
    const items = this.navItems();
    if (!items.length) return;
    let i = items.indexOf(this.navCurrent);
    i = i < 0 ? 0 : (i + delta + items.length) % items.length;
    this.navFocus(items[i]);
  }

  // Moves to the nearest control in direction (dx, dy): candidates must lie that way; ones overlapping the current
  // control across the move (same row for left/right, same column for up/down) win, then the closest.
  navDir(dx, dy) {
    const all = this.navItems();
    if (!all.length) return;
    // LOCAL PATCH: up / down inside a scrolling list stay in that list (its rows above or below, even scrolled out of
    // sight); only past its first or last row does the move leave it.
    // LOCAL PATCH: up / down go row by row, in page order, through the column (or the open window) the selection is
    // in: a row is the controls side by side on one line (a row of options, Choose + Default, Reset + Low-end PC; the
    // page switcher counts as one). The column is kept (Default stays on Default); a row of options is entered at its
    // chosen option. Order, not distance on screen: scrolled lists (still moving) and wide buttons no longer make it
    // jump past rows. Open drop-down lists keep the nearest-control search below.
    let items = all;
    const cur0 = this.navCurrent && document.contains(this.navCurrent) ? this.navCurrent : null;
    if (dy && cur0 && cur0.matches('.flist')) {
      const max = cur0.scrollHeight - cur0.clientHeight;
      if ((dy > 0 && cur0.scrollTop < max - 1) || (dy < 0 && cur0.scrollTop > 1)) {
        cur0.scrollBy({ top: dy * 96, behavior: 'smooth' });
        return;
      }
    }
    if (dy && cur0 && !document.querySelector('.menu')) {
      const scopeEl = cur0.closest('.dlg') || cur0.closest('.left') || cur0.closest('.right');
      if (scopeEl) {
        const inScope = all.filter((el) => scopeEl.contains(el));
        const top = (el) => (el.matches('input[type=checkbox]') ? (el.closest('label') || el) : el).getBoundingClientRect().top;
        const rows = [];
        let last = null;
        inScope.forEach((el) => {
          const nav = el.closest('.tabnav');
          const same = last && (nav ? last.nav === nav
            : !last.nav && Math.abs(last.top - top(el)) < 8);
          if (same) last.items.push(el);
          else { last = { nav, parent: el.parentElement, top: top(el), items: [el] }; rows.push(last); }
        });
        const ri = rows.findIndex((r) => r.items.includes(cur0));
        if (ri >= 0) {
          const next = rows[ri + dy];
          if (!next) return; // the first / last row: stay
          const col = rows[ri].items.indexOf(cur0);
          const chosen = next.items.length > 1 && rows[ri].items.length === 1 ? next.items.find((el) => el.classList.contains('on')) : null;
          this.navFocus(chosen || next.items[Math.min(col, next.items.length - 1)]);
          return;
        }
      }
    }
    const cur = this.navCurrent && document.contains(this.navCurrent) ? this.navCurrent : null;
    if (!cur) { this.navFocus(items[0]); return; }
    const box = (el) => (el.matches('input[type=checkbox]') ? (el.closest('label') || el) : el).getBoundingClientRect();
    const a = box(cur);
    const ac = { x: (a.left + a.right) / 2, y: (a.top + a.bottom) / 2 };
    let best = null, bestScore = Infinity;
    for (const el of items) {
      if (el === cur) continue;
      const r = box(el);
      if (r.width === 0 && r.height === 0) continue;
      const c = { x: (r.left + r.right) / 2, y: (r.top + r.bottom) / 2 };
      // How far it is in the pressed direction (edge to edge), and how far off to the side.
      const along = dx ? (dx > 0 ? r.left - a.right : a.left - r.right) : (dy > 0 ? r.top - a.bottom : a.top - r.bottom);
      const ahead = dx ? (dx > 0 ? c.x - ac.x : ac.x - c.x) : (dy > 0 ? c.y - ac.y : ac.y - c.y);
      if (ahead <= 4 || along < -Math.min(a.width, a.height, r.width, r.height) / 2) continue;
      const overlap = dx ? Math.min(a.bottom, r.bottom) - Math.max(a.top, r.top) : Math.min(a.right, r.right) - Math.max(a.left, r.left);
      const side = overlap > 0 ? 0 : (dx ? Math.abs(c.y - ac.y) : Math.abs(c.x - ac.x));
      const score = Math.max(0, along) + side * 3 + (overlap > 0 ? 0 : 400);
      if (score < bestScore) { bestScore = score; best = el; }
    }
    if (best) this.navFocus(best);
  }

  navSide(delta) {
    const el = this.navCurrent;
    if (el && el.matches('input[type=range]')) {
      const v = Math.min(Number(el.max), Math.max(Number(el.min), Number(el.value) + delta * (Number(el.step) || 1)));
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(el, String(v));
      el.dispatchEvent(new Event('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
      return;
    }
    const group = el && el.parentElement && (el.matches('.seg, .chipb') ? el.parentElement : null);
    if (group) {
      const sibs = Array.from(group.querySelectorAll('.seg, .chipb'));
      // Left / right move between the options of a row; Cross picks one (as on a console).
      const next = sibs[sibs.indexOf(el) + delta];
      if (next) { this.navFocus(next); return; }
    }
    this.navDir(delta, 0);
  }

  navBack() {
    this.hap('back');
    const x = document.querySelector('.dlg .x');
    if (x) { x.click(); setTimeout(() => this.navMove(0), 0); return; }
    if (this.state.resOpen || this.state.winOpen) {
      this.setState({ resOpen: false, winOpen: false });
      setTimeout(() => { const o = this.navOpener && document.contains(this.navOpener) ? this.navOpener : document.querySelector('.sel'); if (o) this.navFocus(o); }, 30);
      return;
    }
    if (this.ingame && this.host) this.host.postMessage('resume');
  }

  navPress() {
    this.hap('press');
    const el = this.navCurrent;
    if (!el || !document.contains(el)) { this.navMove(0); return; }
    const opener = el.matches('.sel') ? el : null;
    el.click();
    if (opener) {
      this.navOpener = opener;
      setTimeout(() => { const on = document.querySelector('.menu .opt.on') || document.querySelector('.menu .opt'); if (on) this.navFocus(on); }, 30);
      return;
    }
    // A pick in an open list closes it: back to its button.
    if (el.matches('.opt') && this.navOpener) {
      setTimeout(() => { if (document.contains(this.navOpener)) this.navFocus(this.navOpener); }, 30);
      return;
    }
    setTimeout(() => { if (!document.contains(this.navCurrent)) this.navMove(0); }, 0);
  }

  // LOCAL PATCH: the Feature Set list scrolls itself slowly and loops (pause at the end, glide back to the top).
  // Any touch pauses it (mouse over the list, wheel, keys, the controller); it carries on 4 s after the last one.
  featureAutoScroll() {
    const SPEED = 16;          // px per second
    const HOLD = 2200;         // ms held at the top and at the end
    const IDLE = 4000;         // ms after the last touch before it carries on
    this._userT = 0;
    ['wheel', 'pointerdown', 'keydown', 'touchstart'].forEach((ev) => document.addEventListener(ev, () => { this._userT = performance.now(); }, { passive: true }));
    let last = performance.now(), hold = performance.now() + 3500, pos = null, phase = 'down';
    const step = (now) => {
      requestAnimationFrame(step);
      const dt = Math.min(0.1, (now - last) / 1000);
      last = now;
      const list = document.querySelector('.flist');
      if (!list || list.scrollHeight <= list.clientHeight + 4) { pos = null; return; }
      const touched = Math.max(this._userT || 0, this._padT || 0, this._hostRsyT || 0);
      if (list.matches(':hover') || list.classList.contains('pad-focus') || now - touched < IDLE) { pos = null; hold = now + 600; return; }
      if (pos === null) pos = list.scrollTop;     // carry on from wherever the player left it
      if (now < hold) return;
      const end = list.scrollHeight - list.clientHeight;
      if (phase === 'down') {
        pos = Math.min(end, pos + SPEED * dt);
        list.scrollTop = pos;
        if (pos >= end - 0.5) { phase = 'up'; hold = now + HOLD; }
      } else {
        pos = Math.max(0, pos - Math.max(60, pos * 2.2) * dt);   // a quick glide back to the top
        list.scrollTop = pos;
        if (pos <= 0.5) { phase = 'down'; hold = now + HOLD; }
      }
    };
    requestAnimationFrame(step);
  }

  navSetup() {
    this.featureAutoScroll();
    // LOCAL PATCH (GitHub issue #20): mouse buttons can be bound on the Keyboard page. While a key is being bound, a
    // mouse button (left, middle, right, 4, 5) is taken as the binding; its click does nothing else.
    document.addEventListener('mousedown', (e) => {
      const s = this.state;
      if (!s || !s.listening || s.device !== 'keyboard') return;
      const name = ['MOUSE_LEFT', 'MOUSE_MIDDLE', 'MOUSE_RIGHT', 'MOUSE_4', 'MOUSE_5'][e.button];
      if (!name) return;
      e.preventDefault(); e.stopPropagation();
      const bind = Object.assign({}, s.bind);
      Object.keys(bind).forEach((k) => { if (bind[k] === name) bind[k] = 'None'; });
      bind[s.listening] = name;
      this._eatClick = true;
      this.setState({ bind: bind, listening: null });
    }, true);
    document.addEventListener('click', (e) => { if (this._eatClick) { this._eatClick = false; e.preventDefault(); e.stopPropagation(); } }, true);
    document.addEventListener('auxclick', (e) => { if (this._eatClick) { this._eatClick = false; e.preventDefault(); e.stopPropagation(); } }, true);
    document.addEventListener('keydown', (e) => {
      if (this.state && this.state.listening) return; // binding a key in Configure Controller
      const k = e.key;
      if (k === 'ArrowDown' || k === 'ArrowUp') { e.preventDefault(); this.navDir(0, k === 'ArrowDown' ? 1 : -1); }
      else if (k === 'ArrowLeft' || k === 'ArrowRight') {
        if (this.navCurrent && this.navCurrent.matches('input[type=range]')) return; // the slider's own keys
        e.preventDefault(); this.navSide(k === 'ArrowRight' ? 1 : -1);
      }
      else if (k === 'Escape') { e.preventDefault(); this.navBack(); }
      else if ((e.code === 'KeyQ' || e.code === 'KeyE') && !e.ctrlKey && !e.altKey && !(e.target && /^(INPUT|TEXTAREA)$/.test(e.target.tagName) && e.target.type !== 'checkbox' && e.target.type !== 'range')) {
        e.preventDefault(); this.navTab(e.code === 'KeyQ' ? -1 : 1); // settings pages (the controller's L1 / R1)
      }
      else if (k === 'F1' && this.ingame) { e.preventDefault(); if (this.host) this.host.postMessage('resume'); }
    });
    const held = {};
    const REPEAT_FIRST = 380, REPEAT_NEXT = 110;
    const edge = (name, down, fn) => {
      const now = performance.now();
      const h = held[name];
      if (!down) { held[name] = null; return; }
      // In the F1 menu the game sends its controller too (padCmd): a press counts once, from whichever source had it.
      const claim = () => { this._padT = now; if (this.ingame) { this._padSrc = 'page'; this._padSrcT = now; } };
      if (!h) { held[name] = { next: now + REPEAT_FIRST }; claim(); fn(); return; }
      if (now >= h.next) { h.next = now + REPEAT_NEXT; claim(); fn(); }
    };
    let primed = false;
    window.__nav = this;
    this.navShow = () => { if (!this.navCurrent || !document.contains(this.navCurrent)) { this.navMove(0); return true; } return false; };
    const poll = () => {
      const pads = (navigator.getGamepads ? Array.from(navigator.getGamepads()).filter(Boolean) : [])
        .filter((p) => !(this.hostPad && /054c/i.test(p.id || ''))); // a DualSense the launcher reads itself
      if (pads.length && !primed) {
        // Buttons already down when the page starts (Select+Start that opened the menu) count only after a release.
        primed = true;
        [[0, 'cross'], [1, 'circle'], [9, 'start'], [12, 'up'], [13, 'down'], [14, 'left'], [15, 'right']].forEach(([i, n]) => {
          if (pads.some((p0) => p0.buttons[i] && p0.buttons[i].pressed)) held[n] = { next: Infinity };
        });
      }
      // LOCAL PATCH: the tab hints follow the controller (L1 / R1 on PlayStation pads, LB / RB otherwise).
      if (pads.length) {
        const kind = /054c|sony|dualsense|dualshock|playstation|wireless controller/i.test(pads[0].id || '') ? 'ps' : 'xbox';
        if (this.state && this.state.padKind !== kind && !this.hostPad && !this._hostKind) this.setState({ padKind: kind });
      }
      // While the game is starting or running the controller belongs to the game: Cross would press the launcher's Stop Game button.
      const gameBusy = !this.ingame && this.state && this.state.launch !== 'idle';
      // LOCAL PATCH: in the F1 menu the game sends its controller (padCmd), but a controller switched on after the game
      // started may only reach this page: it is read here too, unless the game's has been driving the menu just now.
      const hostDriving = this.ingame && this._padSrc === 'host' && performance.now() - (this._padSrcT || 0) < 1500;
      if (pads.length && !gameBusy && !hostDriving && !(this.state && this.state.listening)) {
        const RAW = { 0: 1, 1: 2, 2: 0, 3: 3, 9: 9 }; // standard index -> raw Sony index
        const hat = (p) => { const v = p.axes[9]; if (v === undefined || v > 1.05 || v < -1.05) return -1; return Math.round((v + 1) * 3.5) % 8; };
        const press = (p, i) => {
          if (p.mapping === 'standard') return !!(p.buttons[i] && p.buttons[i].pressed);
          if (i >= 12 && i <= 15) {
            const h = hat(p);
            if (h < 0) return false;
            return i === 12 ? (h === 7 || h === 0 || h === 1) : i === 15 ? (h >= 1 && h <= 3) : i === 13 ? (h >= 3 && h <= 5) : (h >= 5 && h <= 7);
          }
          const r = RAW[i] !== undefined ? RAW[i] : i;
          return !!(p.buttons[r] && p.buttons[r].pressed);
        };
        const btn = (i) => pads.some((p) => press(p, i));
        const ax = (i) => pads.reduce((best, p) => { const v = p.axes[i] || 0; return Math.abs(v) > Math.abs(best) ? v : best; }, 0);
        const manual = document.documentElement.classList.contains('manual-open') && this._book;
        if (manual) {
          const turn = (key) => document.dispatchEvent(new KeyboardEvent('keydown', { key: key, bubbles: true, cancelable: true }));
          edge('left', btn(14) || ax(0) < -0.6, () => turn('ArrowLeft'));
          edge('right', btn(15) || ax(0) > 0.6, () => turn('ArrowRight'));
          edge('circle', btn(1), () => this.manualClose());
        } else {
        edge('up', btn(12) || ax(1) < -0.6, () => { if (!this.navShow()) this.navDir(0, -1); });
        edge('down', btn(13) || ax(1) > 0.6, () => { if (!this.navShow()) this.navDir(0, 1); });
        edge('left', btn(14) || ax(0) < -0.6, () => this.navSide(-1));
        edge('right', btn(15) || ax(0) > 0.6, () => this.navSide(1));
        edge('cross', btn(0), () => this.navPress());
        const tabStep = (d) => this.navTab(d); // LOCAL PATCH: one settings page at a time (navTab / tabGo)
        edge('l1', btn(4), () => tabStep(-1));
        edge('r1', btn(5), () => tabStep(1));
        edge('circle', btn(1), () => this.navBack());
        }
        edge('start', btn(9), () => { if (this.ingame && this.host) this.host.postMessage('resume'); });
      }
      // LOCAL PATCH: the right stick (up / down) scrolls the list in use, from this page's controllers or the host's
      // (the launcher's DualSense reader, the game in the F1 menu: "pad:rsy:<-100..100>").
      {
        let v = 0;
        if (!gameBusy && !(this.state && this.state.listening))
          pads.forEach((p) => { if (p.mapping === 'standard') { const a = p.axes[3] || 0; if (Math.abs(a) > Math.abs(v)) v = a; } });
        if (this._hostRsyT && performance.now() - this._hostRsyT < 250 && Math.abs(this._hostRsy) > Math.abs(v)) v = this._hostRsy;
        if (Math.abs(v) > 0.2) this.stickScroll(v);
      }
      this.navRestore();
      requestAnimationFrame(poll);
    };
    requestAnimationFrame(poll);
    // In game the menu starts on Resume.
    if (this.ingame && !this.manualOnly) setTimeout(() => this.navFocus(document.querySelector('.btn.go')), 150);
    if (this.manualOnly) {
      document.documentElement.classList.add('manual-only');
      setTimeout(() => { if (this.host) this.host.postMessage('manual'); }, 30);
    }
  }

  sig(s) {""")

L("""  sig(s) {
    return JSON.stringify({ res: s.res, renderer: s.renderer, ws: s.ws, vsync: s.vsync, tex: s.tex, fx: s.fx, aa: s.aa, msaa: s.msaa });
  }""",
  """  sig(s) {
    return JSON.stringify({ res: s.res, renderer: s.renderer, fps: s.fps, ws: s.ws, wsShape: s.wsShape, vsync: s.vsync, tex: s.tex, fx: s.fx, aa: s.aa, blend: s.blend, scarf: s.scarf || '', bind: s.bind, mods: s.mods });
  }

  send(cmd, extra) {
    if (!this.host) { this.toastMsg('Launcher host not connected'); return; }
    const s = this.state;
    const lines = [cmd];
    const add = (k, v) => lines.push(k + '=' + String(v));
    add('res', s.res); add('renderer', s.renderer); add('fps', s.fps); add('ws', s.ws ? 1 : 0); add('wsShape', s.wsShape);
    add('vsync', s.vsync ? 1 : 0); add('tex', s.tex); add('fx', s.fx); add('aa', s.aa); add('blend', s.blend || 'basic');
    add('scarf', s.scarf || 'original');
    Object.keys(s.mods).forEach((k) => add('mod.' + k, s.mods[k] ? 1 : 0));
    Object.keys(s.bind).forEach((k) => add('bind.' + k, s.bind[k]));
    if (extra) Object.keys(extra).forEach((k) => add(k, extra[k]));
    this.host.postMessage(lines.join('\\n'));
  }

  // The F1 menu's controller, read by the game and sent here (see ps2_ingame_menu.h padEvent).
  padCmd(name) {
    // The launcher reads a DualSense itself (launcher_dualsense.cpp): from now on the page's own Gamepad API leaves Sony pads to it.
    if (name === 'host') { this.hostPad = true; if (this.state && this.state.padKind !== 'ps') this.setState({ padKind: 'ps' }); return; }
    if (name === 'lost') { this.hostPad = false; return; }
    // While the game is starting or running the controller belongs to the game.
    if (!this.ingame && this.state && this.state.launch !== 'idle') return;
    const manual = document.documentElement.classList.contains('manual-open') && this._book;
    const key = (k) => document.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true }));
    if (manual) {
      if (name === 'left') key('ArrowLeft');
      else if (name === 'right') key('ArrowRight');
      else if (name === 'circle') this.manualClose();
      return;
    }
    if (name === 'up') { if (!this.navShow || !this.navShow()) this.navDir(0, -1); }
    else if (name === 'down') { if (!this.navShow || !this.navShow()) this.navDir(0, 1); }
    else if (name === 'left') this.navSide(-1);
    else if (name === 'right') this.navSide(1);
    else if (name === 'cross') this.navPress();
    else if (name === 'circle') this.navBack();
    else if (name === 'l1' || name === 'r1') this.navTab(name === 'l1' ? -1 : 1);
    else if (name === 'start' && this.ingame && this.host) this.host.postMessage('resume');
  }

  // LOCAL PATCH: settings pages, one at a time: L1 / R1 (LB / RB, Q / E, the arrows) step through them in order.
  navTab(d) {
    if (this.state.modal === 'mods') { this.navModTab(d); return; }
    if (document.querySelector('.dlg')) return;
    const ids = ['display', 'graphics', 'effects', 'perf', 'jukebox', 'languages', 'controller', 'features'];
    const i = Math.max(0, ids.indexOf(this.state.tab || 'display'));
    this.tabGo(ids[(i + d + ids.length) % ids.length], d);
  }

  // LOCAL PATCH: the Mod Menu's pages, stepped and animated like the settings pages.
  navModTab(d) {
    const ids = window.MOD_TABS.map((t) => t[0]);
    const i = Math.max(0, ids.indexOf(this.state.modTab || 'look'));
    this.modTabGo(ids[(i + d + ids.length) % ids.length], d);
  }

  modTabGo(id, d) {
    const ids = window.MOD_TABS.map((t) => t[0]);
    const cur = this.state.modTab || 'look';
    if (!id || id === cur) return;
    if (d === undefined) d = ids.indexOf(id) > ids.indexOf(cur) ? 1 : -1;
    this.setState({ modTab: id });
    this.hap('tab');
    requestAnimationFrame(() => {
      const nav = document.querySelector('.modnav');
      if (!nav || !nav.animate) return;
      const slide = (px, blur) => [
        { opacity: 0, transform: 'translateX(' + px + 'px)', filter: 'blur(' + blur + 'px)' },
        { opacity: 1, transform: 'translateX(0)', filter: 'blur(0)' }];
      const name = nav.querySelector('.tname');
      if (name) name.animate(slide(d * 36, 7), { duration: 300, easing: 'cubic-bezier(.2,.8,.3,1)' });
      Array.from(nav.parentElement.querySelectorAll('.mrow')).forEach((el, k) => {
        el.animate(slide(d * 26, 5), { duration: 320, delay: Math.min(k, 6) * 18, easing: 'cubic-bezier(.2,.8,.3,1)', fill: 'backwards' });
      });
      const scroller = nav.closest('.mlist, .dlg-body, .mbody') || nav.parentElement;
      if (scroller) scroller.scrollTop = 0;
    });
  }

  // Shows a settings page; the page name and the page slide in from the side it came from, with a motion blur.
  tabGo(id, d) {
    const ids = ['display', 'graphics', 'effects', 'perf', 'jukebox', 'languages', 'controller', 'features'];
    const cur = this.state.tab || 'display';
    if (!id || id === cur) return;
    if (d === undefined) d = ids.indexOf(id) > ids.indexOf(cur) ? 1 : -1;
    document.documentElement.classList.add('tabbed');
    this.setState({ tab: id, resOpen: false, winOpen: false });
    this.hap('tab');
    requestAnimationFrame(() => {
      const nav = document.querySelector('.tabnav');
      if (!nav || !nav.animate) return;
      const slide = (px, blur) => [
        { opacity: 0, transform: 'translateX(' + px + 'px)', filter: 'blur(' + blur + 'px)' },
        { opacity: 1, transform: 'translateX(0)', filter: 'blur(0)' }];
      const name = nav.querySelector('.tname');
      if (name) name.animate(slide(d * 36, 7), { duration: 300, easing: 'cubic-bezier(.2,.8,.3,1)' });
      Array.from(nav.parentElement.querySelectorAll(':scope > sc-if > *')).forEach((el, k) => {
        el.animate(slide(d * 26, 5), { duration: 320, delay: Math.min(k, 6) * 18, easing: 'cubic-bezier(.2,.8,.3,1)', fill: 'backwards' });
      });
    });
  }

  onHost(msg) {
    const m = typeof msg === 'string' ? msg : '';
    if (m.indexOf('pad:') === 0) { this.padCmd(m.slice(4)); return; }
    if (m === 'manualhide') { this.manualHide(); return; }
    if (m === 'max:1' || m === 'max:0') { document.documentElement.classList.toggle('maxed', m === 'max:1'); return; }
    if (m === 'saved') { this.setState({ saved: this.sig(this.state) }); this.toastMsg('Settings saved'); }
    else if (m === 'saved:restart') { this.setState({ saved: this.sig(this.state), restart: true }); this.toastMsg('Saved - some changes apply after a restart'); }
    else if (m === 'saved:live') { this.setState({ saved: this.sig(this.state) }); this.toastMsg('Settings applied'); }
    else if (m === 'starting') { this.setState({ launch: 'launching', progress: 10 }); this.musicOff(); }
    else if (m === 'running') { clearInterval(this.timer); this.setState({ launch: 'running', progress: 100 }); this.musicOff(); }
    else if (m === 'exited') { clearInterval(this.timer); this.setState({ launch: 'idle', progress: 0 }); this.toastMsg('Game closed'); this.musicOn(); }
    else if (m.indexOf('manual-progress:') === 0) { const a = m.slice(16).split('|'); this.toastMsg(+a[1] ? 'Preparing the manual... page ' + a[0] + ' of ' + a[1] : 'Preparing the manual...'); }
    else if (m.indexOf('manual-error:') === 0) { this.toastMsg(m.slice(13)); }
    else if (m.indexOf('manual:') === 0) { const a = m.slice(7).split('|'); this.manualShow(+a[0], a[1], a[2] || ''); }
    else if (m.indexOf('error:') === 0) { clearInterval(this.timer); this.setState({ launch: 'idle', progress: 0 }); this.toastMsg(m.slice(6)); this.musicOn(); }
  }""")

# Key names as the game's settings.ini writes them (raylib names).
L("""  keyName(e) {
    const k = e.key;
    const map = { ' ': 'Space', ArrowUp: 'Up', ArrowDown: 'Down', ArrowLeft: 'Left', ArrowRight: 'Right' };
    if (map[k]) return map[k];
    return k.length === 1 ? k.toUpperCase() : k;
  }""",
  """  keyName(e) {
    // LOCAL PATCH (GitHub issue #11): by the physical key (e.code), named as the game knows it (ps2_settings.cpp
    // keyNames). e.key gave 'SHIFT' / 'CONTROL' / ',' for Shift, Ctrl and punctuation, which the game did not know, so
    // those bindings did nothing. null = a key the game cannot use (the binding is left as it was).
    const c = e.code || '';
    const codes = { Space: 'SPACE', ArrowUp: 'UP', ArrowDown: 'DOWN', ArrowLeft: 'LEFT', ArrowRight: 'RIGHT',
      Enter: 'ENTER', NumpadEnter: 'KP_ENTER', Backspace: 'BACKSPACE', Tab: 'TAB', Escape: 'ESCAPE', Insert: 'INSERT',
      Delete: 'DELETE', Home: 'HOME', End: 'END', PageUp: 'PAGE_UP', PageDown: 'PAGE_DOWN',
      ShiftLeft: 'LEFT_SHIFT', ShiftRight: 'RIGHT_SHIFT', ControlLeft: 'LEFT_CONTROL', ControlRight: 'RIGHT_CONTROL',
      AltLeft: 'LEFT_ALT', AltRight: 'RIGHT_ALT', Comma: 'COMMA', Period: 'PERIOD', Slash: 'SLASH',
      Semicolon: 'SEMICOLON', Minus: 'MINUS', Equal: 'EQUAL', Quote: 'APOSTROPHE', Backquote: 'GRAVE',
      BracketLeft: 'LEFT_BRACKET', BracketRight: 'RIGHT_BRACKET', Backslash: 'BACKSLASH' };
    if (codes[c]) return codes[c];
    let m = /^Key([A-Z])$/.exec(c); if (m) return m[1];
    m = /^Digit([0-9])$/.exec(c); if (m) return m[1];
    m = /^Numpad([0-9])$/.exec(c); if (m) return 'KP_' + m[1];
    // no code (an old WebView): the key's text
    const k = e.key || '';
    if (k === ' ') return 'SPACE';
    if (/^[a-z0-9]$/i.test(k)) return k.toUpperCase();
    if (k === 'Shift') return e.location === 2 ? 'RIGHT_SHIFT' : 'LEFT_SHIFT';
    if (k === 'Control') return e.location === 2 ? 'RIGHT_CONTROL' : 'LEFT_CONTROL';
    return null;
  }""")
# LOCAL PATCH (GitHub issue #11): Shift, Ctrl and Alt can be bound (only the Windows key is ignored); a key the game
# cannot use leaves the binding as it was.
L("""          if (['Shift', 'Control', 'Alt', 'Meta'].indexOf(e.key) >= 0) return;
          const name = this.keyName(e);""",
  """          if (e.key === 'Meta' || e.key === 'OS') return;
          const name = this.keyName(e);
          if (!name) { this.toastMsg('That key cannot be used - try another'); return; }""")

L("""    const RES = [['720p', '1280 x 720'], ['1080p', '1920 x 1080'], ['1440p', '2560 x 1440'], ['4K', '3840 x 2160']];""",
  """    // The PCSX2 renderer draws at a whole multiple of the PS2's 448 lines.
    // The familiar sizes. Each setting draws the PS2 picture (640 x 448) at a whole multiple big enough for that size
    // (2x, 3x, 4x, 6x, 12x), then fits it to the screen.
    const RES = [['Native', '640 x 448'], ['720p', '1280 x 720'], ['1080p', '1920 x 1080'], ['1440p', '2560 x 1440'], ['4K', '3840 x 2160']]
      // 8K (12x): PCSX2's highest upscale, for very powerful graphics cards. Lowered automatically on cards with too
      // little video memory.
      .concat([['8K', '7680 x 4320']]);""")

if guide_ids:
    for key, cap, label, _ in GUIDES:
        pass
    old_sl = re.search(r"    const SL = \[\n(.*?)\n    \];", logic, re.S).group(0)
    new_sl = "    const SL = [\n" + ",\n".join(
        "      { src: '%s', cap: %s, label: %s }" % (guide_ids[k], json.dumps(c), json.dumps(l)) for k, c, l, _ in GUIDES) + "\n    ];"
    L(old_sl, new_sl)

# Scarf colour row (Mod Menu).
L("""      modRows: modRows,""",
  """      modRows: modRows,
      scarfDesc: s.scarf ? 'Hotsuma\\'s scarf in ' + s.scarf.toUpperCase() + '. Save to keep it; it changes in game straight away.' : 'Hotsuma\\'s scarf, red as in the original game. Pick a swatch or any colour.',
      scarfHex: s.scarf || '#b0161e',
      scarfSwatches: [['Original red', ''], ['Blue', '#2456d6'], ['Purple', '#7b35c9'], ['Green', '#2b9a45'],
                      ['Gold', '#d4a017'], ['White', '#ececec'], ['Black', '#1c1c1f'], ['Pink', '#e2559e']].map((c) => ({
        name: c[0], on: (s.scarf || '') === c[1],
        style: 'width:22px;height:22px;border-radius:50%;padding:0;cursor:pointer;background:' + (c[1] || '#b0161e') + ';border:' +
               ((s.scarf || '') === c[1] ? '3px solid #1b1b1f;box-shadow:0 0 0 2px #b8122b' : '2px solid rgba(0,0,0,.25)'),
        pick: () => this.setState({ scarf: c[1] })
      })),
      scarfPick: (e) => {
        const v = String(e.target.value || '').toLowerCase();
        if (/^#[0-9a-f]{6}$/.test(v)) this.setState({ scarf: v === '#b0161e' ? '' : v });
      },""")

# Mods this port has.
L("""    const MODS = [
      ['intro', 'Skip intro movies', 'Go straight to the title screen.'],
      ['fps', 'Unlocked frame rate', 'Run above the original 60 Hz cap.'],
      ['tex', 'Custom texture pack', 'Load replacement textures from the mods folder.'],
      ['dbg', 'Debug overlay', 'Show frame time and renderer info in-game.']
    ];""",
  """    const MODS = [
      ['studio', 'Twilight Orchard intro', 'Our studio\\'s opening before the game starts. Any key or button skips it.'],
      ['intro', 'Skip start-up logos', 'SEGA, Overworks and ADX logos pass in a second each. Start, Cross or Enter during them does the same.'],
      ['fastvu', 'Fast VU maths', 'A little faster; rare results can differ from the PS2.'],
      ['dbg', 'Debug overlay', 'Show the frame rate in-game.'],
      ['diag', 'Record diagnostics', 'Save a performance profile and sound trace (game/captures).'],
      ['legacy', 'Legacy sound chip', 'The older built-in sound model instead of PCSX2\\'s SPU2.'],
      ['nscale', 'Effects at high resolution', 'Blur, glow and fog drawn at the PS2\\'s size, then scaled (PCSX2 native scaling).'],
      ['rr5fix', 'Ridge Racer blur fix', 'A bridge fix made for Ridge Racer V. Off for Shinobi; on only to compare.'],
      ['dropst', 'Drop stretched triangles', 'Ridge Racer V\\'s fix. Off draws every effect layer, as PCSX2 does.'],
      ['record', 'Record drawing (effects check)', 'Saves every drawing command from boot (several GB, game/captures).']
    ];""")

# Launch: the real game.
L("""    const status = p < 25 ? 'Verifying game image...' : p < 55 ? 'Loading recompiled game...' : p < 80 ? 'Building shader cache...' : 'Starting Shinobi...';""",
  """    const status = 'Starting Shinobi...';""")
L("""      play: () => {
        const st = this.state;
        if (st.launch === 'launching') return;
        if (st.launch === 'running') { this.setState({ launch: 'idle', progress: 0 }); this.toastMsg('Game stopped'); return; }
        clearInterval(this.timer);
        this.setState({ launch: 'launching', progress: 0 });
        this.timer = setInterval(() => {
          const n = this.state.progress + 2;
          if (n >= 100) { clearInterval(this.timer); this.setState({ launch: 'running', progress: 100 }); }
          else this.setState({ progress: n });
        }, 70);
      },""",
  """      play: () => {
        if (this.ingame) { if (this.host) this.host.postMessage('resume'); return; }
        const st = this.state;
        if (st.launch === 'launching') return;
        if (st.launch === 'running') { this.send('stop'); return; }
        clearInterval(this.timer);
        this.setState({ launch: 'launching', progress: 5 });
        // The bar only shows that the game is starting; the host reports when its window is up.
        this.timer = setInterval(() => {
          const n = Math.min(this.state.progress + 3, 90);
          this.setState({ progress: n });
        }, 120);
        this.send('play');
      },""")
L("""      playLabel: launching ? status : running ? 'Stop Game' : 'Play Game (Recom)',""",
  """      playLabel: this.ingame ? 'Resume Game' : launching ? status : running ? 'Stop Game' : 'Play Game (Recom)',""")
L("""      chipText: running ? 'Running - ' + s.renderer + ' - ' + s.res : status + ' ' + p + '%',""",
  """      chipText: running ? 'Running - ' + s.renderer + ' - ' + s.res + (s.ws ? ' - ' + s.wsShape : '') : status,""")
L("""      rendererOpts: seg(['OpenGL', 'Vulkan', 'D3D12'], 'renderer', s.renderer),""",
  """      rendererOpts: seg(['D3D11', 'Vulkan'], 'renderer', s.renderer),
      fpsOpts: seg(['30', '50', '60', 'Unlocked'], 'fps', s.fps).map((o) => Object.assign(o, {
        cls: o.cls + (o.name === 'Unlocked' ? ' wide' : ''),
        tip: { '30': 'A steady, evenly paced 30', '50': '5 of every 6 frames at normal speed (for 50 Hz screens)',
               '60': "The game's own rate", 'Unlocked': 'No frame cap and no V-Sync wait (lowest lag, may tear)' }[o.name] })),
      wsName: s.wsShape,
      wsOpts: ['16:9', '21:9', '32:9', 'Auto'].map((n) => ({ name: n, on: s.wsShape === n, cls: s.wsShape === n ? 'on' : '', pick: set({ wsShape: n }) })),""")
L("""      texName: QUAL[s.tex],
      fxName: QUAL[s.fx],""",
  """      texName: ['Off', '2x', '4x', '8x', '16x'][s.tex],
      fxName: ['Off', 'Low', 'Medium', 'High', 'Max'][s.fx],""")
L("""      aaOpts: seg(['Off', 'FXAA', 'MSAA'], 'aa', s.aa),
      showMsaa: s.aa === 'MSAA',""",
  """      aaOpts: seg(['Off', 'FXAA', 'Edge AA'], 'aa', s.aa),
      showMsaa: false,""")
L("""      save: () => {
        if (this.sig(this.state) === this.state.saved) { this.toastMsg('No changes to save'); return; }
        this.setState({ saved: this.sig(this.state) });
        this.toastMsg('Settings saved');
      },""",
  """      save: () => {
        if (this.sig(this.state) === this.state.saved) { this.toastMsg('No changes to save'); return; }
        this.send('save');
      },""")
L("""      openModsFolder: () => this.toastMsg('Opening mods folder...'),""",
  """      openModsFolder: () => { this.send('openfolder'); this.toastMsg('Opening the game folder...'); },""")
L("""      hasToast: !!s.toast,""",
  """      toggleMute: () => {
        const muted = !this.state.muted;
        this.setState({ muted });
        document.documentElement.classList.toggle('muted', muted);
        if (this.music) {
          this.music.muted = muted;
          if (!muted && this.state.launch === 'idle') this.musicOn();
        }
        if (this.host) this.host.postMessage('mute\\nmuted=' + (muted ? 1 : 0));
      },
      winMin: () => { if (this.host) this.host.postMessage('minimize'); },
      winMax: () => { if (this.host) this.host.postMessage('maximize'); },
      winClose: () => { if (this.host) this.host.postMessage(this.ingame ? 'resume' : 'close'); },
      titleText: this.ingame ? 'Shinobi (PS2 Recom) - Paused' : 'Shinobi (PS2 Recom) Launcher - by TheRealAfsan',
      cardTitle: this.ingame ? 'Game paused' : 'Shinobi - PS2 Recompilation',
      cardText: this.ingame
        ? 'Change settings while you play. Most apply at once; resolution, renderer and effect switches after a restart. '
          + 'Controller: D-pad to move, Cross to choose, Circle to go back, Start to resume. Keyboard: arrows, Enter, Esc, F1.'
        : 'Ignite the fiery vengeance of Hotsuma, the last of the Oboro clan. Scale walls and slash your way through the '
          + 'demon-filled streets of post-apocalyptic Tokyo. But beware, you\\'ll have to keep your bloodthirsty sword '
          + 'satisfied or suffer its deadly curse yourself.',
      lastLabel: !this.ingame ? 'Credits' : s.quitArmed ? 'Press again to quit' : 'Quit Game',
      lastCls: this.ingame && s.quitArmed ? 'warn' : '',
      stateRowStyle: this.ingame ? 'display:flex;gap:8px' : 'display:none',
      stateSlotsStyle: this.ingame ? 'display:flex;gap:10px;align-items:center;min-width:0' : 'display:none',
      stateSlotChips: [1, 2, 3].map((n) => {
        const cur = (s.stateSlot || 1) === n, when = (this.stateSlots || [])[n - 1] || '';
        return { name: String(n), on: cur, cls: cur ? 'on' : '', tip: when ? 'Saved ' + when : 'Empty',
          pick: () => { this.setState({ stateSlot: n }); this.hap('press'); if (this.host) this.host.postMessage('stateslot\\nslot=' + n); } };
      }),
      stateSlotText: (() => { const n = s.stateSlot || 1, when = (this.stateSlots || [])[n - 1] || ''; return when || 'Empty'; })(),
      saveStateBtn: () => { if (this.host) this.host.postMessage('savestate\\nslot=' + (this.state.stateSlot || 1)); },
      loadStateBtn: () => { if (this.host) this.host.postMessage('loadstate\\nslot=' + (this.state.stateSlot || 1)); },
      lastBtn: () => {
        if (!this.ingame) { this.setState({ modal: 'credits' }); return; }
        if (this.state.quitArmed) { if (this.host) this.host.postMessage('quit'); return; }
        this.setState({ quitArmed: true });
        clearTimeout(this.quitTimer);
        this.quitTimer = setTimeout(() => this.setState({ quitArmed: false }), 3000);
      },
      resetLabel: this.ingame && s.restart ? 'Restart Game' : 'Reset to Defaults',
      resetCls: this.ingame && s.restart ? 'warn' : '',
      resetBtn: () => {
        if (this.ingame && this.state.restart) { this.send('restart'); return; }
        this.setState(Object.assign({ resOpen: false }, this.defaults));
        this.toastMsg('Defaults restored - save to keep them');
      },
      // Lighter settings for slower PCs: the game's own resolution, no extra texture filtering, sharpening or
      // anti-aliasing, PCSX2's lightest blending accuracy; frame rate, widescreen and V-Sync as the defaults.
      lowEndBtn: () => {
        this.setState(Object.assign({ resOpen: false }, this.defaults, { res: 'Native', tex: 0, fx: 0, aa: 'Off', blend: 'minimum' }));
        this.toastMsg('Low-end PC settings - save to keep them');
      },

      hasToast: !!s.toast,""")
# Game file: state, host messages, dialog, play guard.
L("""    this.state.restart = !!(init && init.restart);""",
  """    this.state.restart = !!(init && init.restart);
    // Scarf colour: '' = the original red, else '#rrggbb'.
    this.state.scarf = (init && init.settings && init.settings.scarf) || '';
    this.state.iso = (init && init.iso) || '';
    this.state.installed = init ? !!init.installed : true;
    this.state.isoMsg = '';
    this.state.isoFound = init ? !!init.isoFound : true;
    this.state.savesDir = (init && init.savesDir) || '';
    this.state.installPct = -1;
    this.state.installFile = '';""")
L("""    else if (m.indexOf('error:') === 0) {""",
  """    else if (m.indexOf('eudisc:') === 0) {
      const a = m.slice(7).split('|');
      if (a[0] === 'ok') { this.setState({ euDisc: a[1] || '' }); this.toastMsg(a.slice(2).join('|')); }
      else if (a[0] === 'bad') this.toastMsg(a.slice(2).join('|') || 'That disc image could not be used');
      else {
        // removed: the European choices fall back to English
        const eu = (c) => ['fr', 'de', 'it', 'es'].indexOf(c) >= 0;
        this.setState({ euDisc: '', lvoice: eu(this.state.lvoice) ? 'en' : this.state.lvoice, ltext: eu(this.state.ltext) ? 'en' : this.state.ltext });
        this.toastMsg('European disc removed: English is used instead');
      }
    }
    else if (m.indexOf('juke:') === 0) { const a = m.slice(5).split('|'); const j = Object.assign({}, this.state.juke); if (a[1]) j[a[0]] = a.slice(1).join('|'); else delete j[a[0]]; this.setState({ juke: j }); this.toastMsg(a[1] ? 'Jukebox: ' + a.slice(1).join('|') : 'Jukebox: original music back'); }
    else if (m.indexOf('juke-error:') === 0) { this.toastMsg(m.slice(11)); }
    else if (m.indexOf('music:') === 0) { const a = m.slice(6).split('|'); this.setState({ musicName: a.slice(1).join('|') || 'Original theme' }); this.setMusic(a[0] || null, (a.slice(1).join('|') || 'Original theme') === 'Original theme' ? 24 : 0); this.toastMsg('Launcher music: ' + (a.slice(1).join('|') || 'Original theme')); }
    else if (m.indexOf('iso:') === 0) { const a = m.slice(4).split('|'); this.setState({ iso: a[0] === 'ok' ? (a[1] || '') : this.state.iso, isoFound: a[0] === 'ok' ? true : this.state.isoFound, isoMsg: a.slice(2).join('|') }); }
    else if (m.indexOf('install:') === 0) { const a = m.slice(8).split('|'); this.setState({ installPct: parseInt(a[0], 10) || 0, installFile: a.slice(1).join('|') }); }
    else if (m === 'installed') { this.setState({ installed: true, installPct: -1, installFile: '', isoMsg: 'The game files are set up. You can close this and press Play.' }); this.toastMsg('Game files set up'); }
    else if (m.indexOf('install-error:') === 0) { this.setState({ installPct: -1, installFile: '', isoMsg: m.slice(14) }); this.toastMsg(m.slice(14)); }
    else if (m.indexOf('error:') === 0) {""")
L("""    const modalTitles = { controller: 'Configure Controller', mods: 'Mod Menu', credits: 'Credits', guides: 'Guides' };""",
  """    const modalTitles = { controller: 'Configure Controller', mods: 'Mod Menu', credits: 'Credits', guides: 'Guides', game: 'Game File' };""")
L("""        if (st.launch === 'launching') return;
        if (st.launch === 'running') { this.send('stop'); return; }""",
  """        if (!st.installed) { this.setState({ modal: 'game' }); return; }
        if (st.launch === 'launching') return;
        if (st.launch === 'running') { this.send('stop'); return; }""")
L("""      playLabel: this.ingame ? 'Resume Game' : launching ? status : running ? 'Stop Game' : 'Play Game (Recom)',""",
  """      playLabel: this.ingame ? 'Resume Game' : !s.installed ? 'Select Game File' : launching ? status : running ? 'Stop Game' : 'Play Game (Recom)',""")
L("""openMods: set({ modal: 'mods' }),""",
  """openMods: set({ modal: 'mods' }),
      openGame: set({ modal: 'game' }),
      gameBtnLabel: s.installed ? 'Game File' : 'Select Game File',
      gameBtnCls: s.installed ? '' : 'warn',
      gameCaption: s.iso
        ? (s.isoFound ? 'Game file: ' : 'Not found: ') + ((p, bs) => p.length > 38 ? p.slice(0, 3) + '...' + bs + p.split(bs).pop() : p)(s.iso, String.fromCharCode(92))
        : s.installed ? 'Game files are set up in the game folder' : 'No game file chosen yet',
      gfileCls: (s.iso && !s.isoFound) || (!s.iso && !s.installed) ? 'warn' : '',
      showGame: s.modal === 'game',
      gameIntro: 'This launcher does not contain the game. Choose your own copy of Shinobi (PlayStation 2, USA) as a disc image '
        + '(.iso, or .bin / .img). The launcher checks that it is the right game and copies its files into the game folder.',
      isoPath: s.iso || 'No file chosen',
      isoStatus: s.isoMsg || (s.installed ? 'The game files are set up.' : s.iso ? 'Press Set up to copy the game files.' : 'No game file chosen yet.'),
      installBusy: s.installPct >= 0,
      installFill: Math.max(s.installPct, 0) + '%',
      installLabel: s.installPct >= 0 ? 'Setting up... ' + s.installPct + '%' : s.installed ? 'Check / set up again' : 'Set up game files',
      installNote: s.installPct >= 0 ? (s.installFile || 'Copying...') : 'Setting up copies several GB the first time. Files already copied are skipped.',
      browseIso: () => { if (this.host) this.host.postMessage('browseiso'); },
      savesPath: (this.state.savesDir || 'saves'),
      openSaves: () => { if (this.host) this.host.postMessage('opensaves'); },
      installIso: () => {
        if (!this.state.iso) { this.toastMsg('Choose your disc image first (Browse...)'); return; }
        if (!this.ownedNow()) { this.toastMsg('Tick that you own this copy of the game first'); return; }
        this.setState({ installPct: 0, installFile: '', isoMsg: '' }); if (this.host) this.host.postMessage('installiso');
      },
      owned: this.ownedNow(),
      openManual: () => { if (this.host) this.host.postMessage('manual'); else this.toastMsg('Launcher host not connected'); },
      toggleOwned: () => {
        const owned = !this.ownedNow();
        try { localStorage.setItem('shinobi.owned', owned ? '1' : '0'); } catch (e) {}
        this.setState({ ownedTick: owned });
      },
      gameSteps: ((done) => [
        ['Disc image chosen', !!s.iso && s.isoFound],
        ['Checked: Shinobi (USA, SLUS-20459)', !!s.iso && s.isoFound && !/not|wrong|cannot|could/i.test(s.isoMsg || '')],
        ['Game files set up', !!s.installed]
      ].map((st) => ({
        label: st[0], icon: st[1] ? '\u2713' : '\u25CB',
        mark: 'display:inline-block;width:18px;text-align:center;font-weight:700;color:' + (st[1] ? '#1f8a3b' : '#9a9aa2'),
        text: 'font-weight:600;color:' + (st[1] ? '#1b1b1f' : '#7a7a82')
      })))(),""")


# The "I own this copy" tick: remembered, and taken as given once the game files are set up.
L("""  toastMsg(msg) {""",
  """  // The game manual as a book: pages drawn from the player's own PDF (game/manual), turned with StPageFlip like the
  // CS World reader: drag a page by its corner to lift, fold and turn it, or use the arrows, the arrow keys or a swipe.
  manualLib() {
    if (window.St && window.St.PageFlip) return Promise.resolve();
    if (!this._flipLib) this._flipLib = new Promise((res, rej) => {
      const sc = document.createElement('script'); sc.src = 'page-flip.browser.js';
      sc.onload = res; sc.onerror = () => { this._flipLib = null; rej(new Error('page-flip.browser.js is missing')); };
      document.head.appendChild(sc);
    });
    return this._flipLib;
  }

  // One page-turn back, animated like Next (the corner-only rule is lifted for the request).
  _bookBack(book) {
    const i = book.getCurrentPageIndex();
    if (i <= 0) return;
    const set = book.getSettings();
    const was = set.disableFlipByClick;
    set.disableFlipByClick = false;
    book.flipPrev('top');
    setTimeout(() => {
      set.disableFlipByClick = was;
      // Still not moved (the library refused): no animation, but at least the page turns.
      if (book.getCurrentPageIndex() === i && book.getState && book.getState() === 'read') book.turnToPage(Math.max(0, i - 2));
    }, 900);
  }

  manualClose() {
    if (this.manualOnly && this._bookEl && !this._manualHiding) { this.manualHide(); return; }
    this._manualTeardown();
  }

  // F4 in game: the manual slides down, then the game resumes.
  manualHide() {
    if (this._manualHiding) return;
    this._manualHiding = true;
    const el = this._bookEl;
    if (el) { el.style.transition = 'transform .3s cubic-bezier(.5,0,.75,0)'; el.style.transform = 'translateY(100%)'; }
    setTimeout(() => {
      this._manualHiding = false;
      this._manualTeardown();
      if (this.host) this.host.postMessage('resume');
    }, el ? 300 : 0);
  }

  _manualTeardown() {
    if (this._book) { try { this._book.destroy(); } catch (e) {} this._book = null; }
    if (this._bookEl) { this._bookEl.remove(); this._bookEl = null; }
    document.documentElement.classList.remove('manual-open');
    if (this._bookKeys) { window.removeEventListener('keydown', this._bookKeys, true); this._bookKeys = null; }
  }

  manualShow(count, base, stamp) {
    if (!count) { this.toastMsg('The manual has no pages'); return; }
    this.manualClose();
    const pad = (n) => String(n).padStart(3, '0');
    const urls = []; for (let i = 1; i <= count; i++) urls.push(base + pad(i) + '.png' + (stamp ? '?v=' + stamp : ''));
    const el = document.createElement('div');
    el.setAttribute('role', 'dialog'); el.setAttribute('aria-label', 'Game manual');
    // The launcher's title bar is a window drag area (app-region); the manual covers it and must not move the window.
    el.style.cssText = 'position:fixed;inset:0;z-index:99999;background:rgba(14,14,17,.94);display:flex;flex-direction:column;'
      + 'font-family:Rajdhani,sans-serif;color:#eee;user-select:none;app-region:no-drag;-webkit-app-region:no-drag';
    const btn = 'background:#2a2a30;color:#fff;border:1px solid #55555e;border-radius:4px;padding:6px 14px;font:600 14px Rajdhani,sans-serif;'
      + 'letter-spacing:.06em;text-transform:uppercase;cursor:pointer';
    if (this.manualOnly) {
      el.style.transform = 'translateY(100%)';
      requestAnimationFrame(() => requestAnimationFrame(() => { el.style.transition = 'transform .38s cubic-bezier(.2,.8,.2,1)'; el.style.transform = 'none'; }));
    }
    el.innerHTML =
      '<div style="display:flex;align-items:center;gap:12px;padding:10px 16px;border-bottom:2px solid #b3122a;background:#1b1b1f;'
      + 'app-region:no-drag;-webkit-app-region:no-drag">'
      + '<b style="font-size:18px;letter-spacing:.08em;text-transform:uppercase">Shinobi \u2013 Game Manual</b>'
      + '<span class="mpage" style="margin-left:6px;color:#aaa;font-size:14px"></span><span style="flex:1"></span>'
      + (this.manualOnly ? '<span style="color:#aaa;font-size:14px;letter-spacing:.06em">F4 TO CLOSE</span>'
         : '<button class="mpick" style="' + btn + '" title="Use a different PDF">Choose another PDF</button>'
         + '<button class="mclose" style="' + btn + ';padding:6px 12px" aria-label="Close">\u2715</button>') + '</div>'
      + '<div class="mstage" style="flex:1;min-height:0;display:flex;align-items:center;justify-content:center;padding:14px 56px;position:relative;overflow:hidden">'
      + '<div class="mflip" style="width:100%;height:100%"></div>'
      + '<div class="mzview" style="position:absolute;inset:0;display:none;cursor:grab;z-index:5;overflow:hidden;background:rgba(14,14,17,.98)">'
      + '<div class="mzimgs" style="position:absolute;left:0;top:0;display:flex;transform-origin:0 0;will-change:transform"></div></div>'
      + '<div class="mzhint" style="position:absolute;right:14px;bottom:8px;font-size:12px;color:#888;pointer-events:none">'
      + 'Mouse wheel: zoom \u00b7 Drag a page corner: turn it \u00b7 Double-click: fit</div></div>'
      + '<div style="display:flex;justify-content:center;gap:14px;padding:10px 0 14px">'
      + '<button class="mprev" style="' + btn + '">\u2039 Previous</button><button class="mnext" style="' + btn + '">Next \u203A</button></div>';
    document.body.appendChild(el);
    document.documentElement.classList.add('manual-open');
    this._bookEl = el;
    const pageText = el.querySelector('.mpage');
    if (el.querySelector('.mclose')) el.querySelector('.mclose').onclick = () => this.manualClose();
    if (el.querySelector('.mpick')) el.querySelector('.mpick').onclick = () => { this.manualClose(); if (this.host) this.host.postMessage('manualpick'); };
    this._bookKeys = (e) => {
      if (!this._book) return;
      if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); this.manualClose(); }
      else if (e.key === 'ArrowRight' || e.key === 'PageDown') { e.preventDefault(); e.stopPropagation(); if (this._bookFit) this._bookFit(); this._book.flipNext('top'); }
      else if (e.key === 'ArrowLeft' || e.key === 'PageUp') { e.preventDefault(); e.stopPropagation(); if (this._bookFit) this._bookFit(); this._bookBack(this._book); }
    };
    window.addEventListener('keydown', this._bookKeys, true);
    const first = new Image();
    first.onload = () => {
      this.manualLib().then(() => {
        if (this._bookEl !== el) return;
        const ratio = first.naturalWidth / Math.max(1, first.naturalHeight);
        const host = el.querySelector('.mflip');
        // The CS World reader's settings: a page turns only when it is dragged (or by the arrows, keys or a swipe).
        const book = new window.St.PageFlip(host, {
          width: Math.round(1000 * ratio), height: 1000, size: 'stretch', minWidth: 100, maxWidth: 3000, minHeight: 150, maxHeight: 3000,
          showCover: true, usePortrait: true, autoSize: true, drawShadow: true, maxShadowOpacity: 0.45, flippingTime: 650,
          mobileScrollSupport: false, showPageCorners: false, disableFlipByClick: true, swipeDistance: 24, clickEventForward: false
        });
        book.loadFromImages(urls);
        // Zoom with the mouse wheel, about the point under the cursor (1x to 4x). Zoomed in, a still view of the pages
        // that are open (sharp, from the full-size page images) is shown instead of the book: drag moves it around, only
        // as far as the pages' own edges, and smaller than the view it stays centred. Zooming back out to 1x (or a
        // double-click, a turn by button or key) returns to the book.
        const stage = el.querySelector('.mstage'), zv = el.querySelector('.mzview'), zi = el.querySelector('.mzimgs');
        const z = { s: 1, x: 0, y: 0, w: 0, h: 0 };
        const openPages = () => {
          const i = book.getCurrentPageIndex(), n = urls.length;
          if (!(book.getOrientation && book.getOrientation() === 'landscape')) return [i];
          if (i === 0) return [0];
          const left = i % 2 === 1 ? i : i - 1;
          return left + 1 < n ? [left, left + 1] : [left];
        };
        const layout = () => {
          // At 1x the still view has the book's size and place: the page images at the stage's inner height.
          const sr = stage.getBoundingClientRect(), cs = getComputedStyle(stage);
          const pt = parseFloat(cs.paddingTop) || 0, pb = parseFloat(cs.paddingBottom) || 0;
          const h = Math.max(50, sr.height - pt - pb), ratio = first.naturalWidth / Math.max(1, first.naturalHeight);
          zi.querySelectorAll('img').forEach((im) => { im.style.height = h + 'px'; im.style.width = Math.round(h * ratio) + 'px'; });
          z.w = zi.children.length * Math.round(h * ratio); z.h = h;
          return { vw: sr.width, vh: sr.height };
        };
        const clamp = (v) => {
          const axis = (pos, size, view) => size <= view ? (view - size) / 2 : Math.min(0, Math.max(view - size, pos));
          z.x = axis(z.x, z.w * z.s, v.vw);
          z.y = axis(z.y, z.h * z.s, v.vh);
        };
        const apply = () => {
          if (z.s <= 1) { z.s = 1; zv.style.display = 'none'; zi.innerHTML = ''; return; }
          if (zv.style.display !== 'block') {
            zi.innerHTML = openPages().map((k) => '<img draggable="false" alt="" src="' + urls[k] + '" style="display:block;user-select:none">').join('');
            zv.style.display = 'block';
          }
          clamp(layout());
          zi.style.transform = 'translate(' + z.x + 'px,' + z.y + 'px) scale(' + z.s + ')';
        };
        const zoomAt = (cx, cy, next) => {
          next = Math.max(1, Math.min(4, next));
          if (next === 1) { z.s = 1; apply(); return; }
          const r = stage.getBoundingClientRect(), px = cx - r.left, py = cy - r.top;
          if (z.s === 1) {
            // starting to zoom: the still view starts exactly where the book is
            const v = layout(); z.x = (v.vw - z.w) / 2; z.y = (v.vh - z.h) / 2;
          }
          z.x = px - (px - z.x) * (next / z.s); z.y = py - (py - z.y) * (next / z.s); z.s = next; apply();
        };
        stage.addEventListener('wheel', (e) => { e.preventDefault(); zoomAt(e.clientX, e.clientY, z.s * (e.deltaY < 0 ? 1.15 : 1 / 1.15)); }, { passive: false });
        stage.addEventListener('dblclick', () => { z.s = 1; apply(); });
        let drag = null;
        zv.addEventListener('pointerdown', (e) => { drag = { x: e.clientX - z.x, y: e.clientY - z.y }; try { zv.setPointerCapture(e.pointerId); } catch (_) {} zv.style.cursor = 'grabbing'; });
        zv.addEventListener('pointermove', (e) => { if (!drag) return; z.x = e.clientX - drag.x; z.y = e.clientY - drag.y; apply(); });
        const end = () => { drag = null; zv.style.cursor = 'grab'; };
        zv.addEventListener('pointerup', end); zv.addEventListener('pointercancel', end);
        window.addEventListener('resize', () => { if (z.s > 1) apply(); });
        // Turning a page with the buttons or keys goes back to the book first, so the turn is seen whole.
        const fit = () => { if (z.s !== 1) { z.s = 1; apply(); } };
        this._bookFit = fit;
        const show = () => { const i = book.getCurrentPageIndex(); pageText.textContent = 'Page ' + (i + 1) + ' of ' + count; };
        book.on('flip', show); show();
        el.querySelector('.mprev').onclick = () => { fit(); this._bookBack(book); };
        el.querySelector('.mnext').onclick = () => { fit(); book.flipNext('top'); };
        this._book = book; window.__manualBook = book;
      }).catch((e) => { this.toastMsg(String(e && e.message || e)); this.manualClose(); });
    };
    first.onerror = () => { this.toastMsg('The manual pages could not be loaded'); this.manualClose(); };
    first.src = urls[0];
  }

  ownedNow() {
    if (typeof this.state.ownedTick === 'boolean') return this.state.ownedTick;
    let stored = null;
    try { stored = localStorage.getItem('shinobi.owned'); } catch (e) {}
    return stored === '1' || (stored === null && !!this.state.installed);
  }

  toastMsg(msg) {""")

# Camera (the pad's right stick) on the keyboard: four more rows in Configure Controller, defaults I K J L.
L("""['start', 'Start', 'Menu'], ['select', 'Select', 'View']];""",
  """['start', 'Start', 'Menu'], ['select', 'Select', 'View'],
      ['move_up', 'Move Up', 'Left stick up'], ['move_down', 'Move Down', 'Left stick down'],
      ['move_left', 'Move Left', 'Left stick left'], ['move_right', 'Move Right', 'Left stick right'],
      ['camera_up', 'Camera Up', 'Right stick up'], ['camera_down', 'Camera Down', 'Right stick down'],
      ['camera_left', 'Camera Left', 'Right stick left'], ['camera_right', 'Camera Right', 'Right stick right']];""")
L("""start: 'ENTER', select: 'TAB' };""",
  """start: 'ENTER', select: 'TAB', move_up: 'W', move_down: 'S', move_left: 'A', move_right: 'D', camera_up: 'I', camera_down: 'K', camera_left: 'J', camera_right: 'L' };""")

# Public page: the game's back-of-box text is not used.
if PUBLIC:
    L(r"""        : 'Ignite the fiery vengeance of Hotsuma, the last of the Oboro clan. Scale walls and slash your way through the '
          + 'demon-filled streets of post-apocalyptic Tokyo. But beware, you\'ll have to keep your bloodthirsty sword '
          + 'satisfied or suffer its deadly curse yourself.',""",
      r"""        : 'A native Windows port of Shinobi (PlayStation 2, USA) by TheRealAfsan. Choose your own copy of the game under '
          + 'Game File, set up its files once, then press Play. This program contains no game files.',""")

# The Mod Menu's footer button opens the game folder.
template = must_replace(template, '<span class="lab">Open Mods Folder</span>', '<span class="lab">Open Game Folder</span>')




# ---- LOCAL PATCH: tabs, window mode, haptics (page state, saving, bindings) ----
L("""blend: 'basic' };
    this.defBinds""", """blend: 'basic', win: 'borderless', haptics: true, hapStr: 10, trig: true };
    this.defBinds""")
L("""      resOpen: false, modal: null, guide: 0,
      launch: 'idle'""", """      resOpen: false, winOpen: false, tab: (init && init.ingame) ? 'display' : 'features', modal: null, guide: 0,
      launch: 'idle'""")
L("""blend: s.blend, scarf: s.scarf || '', bind: s.bind, mods: s.mods });""",
  """blend: s.blend, scarf: s.scarf || '', win: s.win, haptics: s.haptics, hapStr: s.hapStr, trig: s.trig, bind: s.bind, mods: s.mods });""")
L("""    add('scarf', s.scarf || 'original');""", """    add('scarf', s.scarf || 'original');
    add('win', s.win || 'borderless'); add('haptics', s.haptics ? 1 : 0); add('hapStr', s.hapStr || 10); add('trig', s.trig ? 1 : 0);""")
L("""      setTex: (e) => this.setState({ tex: Number(e.target.value) }),""",
  """      setTex: (e) => this.setState({ tex: Number(e.target.value) }),
      tabOpts: [['display', 'Display'], ['graphics', 'Graphics'], ['controller', 'Controller']].map((t) => ({ name: t[1], on: s.tab === t[0], cls: s.tab === t[0] ? 'on' : '', pick: () => this.tabGo(t[0]) })),
      tabName: ({ display: 'Display', graphics: 'Graphics', effects: 'Effects', perf: 'Performance', jukebox: 'Sound & Jukebox', languages: 'Languages', controller: 'Controls', features: 'Feature Set' })[s.tab || 'display'] || 'Display',
      tabPrev: () => this.navTab(-1),
      modTabOpts: window.MOD_TABS.map((t) => ({ name: t[1], on: (s.modTab || 'look') === t[0], cls: (s.modTab || 'look') === t[0] ? 'on' : '', pick: () => this.modTabGo(t[0]) })),
      modTabName: (window.MOD_TABS.find((t) => t[0] === (s.modTab || 'look')) || window.MOD_TABS[0])[1],
      modTabPrev: () => this.navModTab(-1),
      modTabNext: () => this.navModTab(1),
      modTabLook: (s.modTab || 'look') === 'look',
      modTabPad: s.modTab === 'pad',
      modTabGameplay: s.modTab === 'gameplay',
      bossHpChips: [['100', 'Original'], ['75', '75%'], ['50', '50%'], ['33', '33%']].map((c) => ({ name: c[1], on: (s.bosshp || '50') === c[0], cls: (s.bosshp || '50') === c[0] ? 'on' : '', pick: () => { this.setState({ bosshp: c[0] }); this.hap('press'); } })),
      bossHpName: (s.bosshp || '50') === '100' ? 'the original' : (s.bosshp || '50') + '% of the original',
      tabNext: () => this.navTab(1),
      tabHintL: s.padKind === 'ps' ? 'L1' : s.padKind === 'xbox' ? 'LB' : s.padKind === 'nintendo' ? 'L' : 'Q',
      tabHintR: s.padKind === 'ps' ? 'R1' : s.padKind === 'xbox' ? 'RB' : s.padKind === 'nintendo' ? 'R' : 'E',
      tabDisplay: s.tab === 'display',
      tabGraphics: s.tab === 'graphics',
      tabController: s.tab === 'controller',
      winOpen: s.winOpen,
      winLabel: ({ windowed: 'Windowed', borderless: 'Borderless Fullscreen', fullscreen: 'Exclusive Fullscreen' })[s.win || 'borderless'],
      toggleWin: () => this.setState({ winOpen: !this.state.winOpen, resOpen: false }),
      closeWin: () => this.setState({ winOpen: false }),
      winOptions: [['borderless', 'Borderless Fullscreen', 'fills the screen, quick to alt-tab'], ['fullscreen', 'Exclusive Fullscreen', 'the screen is the game\\'s alone'], ['windowed', 'Windowed', 'a window you can move and resize']].map((w) => ({ name: w[1], sub: w[2], on: (s.win || 'borderless') === w[0], cls: (s.win || 'borderless') === w[0] ? 'on' : '', pick: () => this.setState({ win: w[0], winOpen: false }) })),
      haptics: s.haptics !== false,
      toggleHaptics: () => this.setState({ haptics: !(this.state.haptics !== false) }),
      hapStr: s.hapStr || 10,
      hapStrName: ((s.hapStr || 10) * 10) + '%',
      hapStrBg: fill(((s.hapStr || 10) - 1) / 9 * 4),
      setHapStr: (e) => this.setState({ hapStr: Number(e.target.value) }),
      trig: s.trig !== false,
      toggleTrig: () => this.setState({ trig: !(this.state.trig !== false) }),""")


# ---- LOCAL PATCH: Effects tab state, saving and bindings ----
L("""win: 'borderless', haptics: true, hapStr: 10, trig: true };""",
  """win: 'borderless', haptics: true, hapStr: 10, trig: true, texf: 'ps2', scrf: 'smooth', dith: true, mip: true, upfix: true, ovl: false, cam: false };""")
L("""win: s.win, haptics: s.haptics, hapStr: s.hapStr, trig: s.trig, bind: s.bind""",
  """win: s.win, haptics: s.haptics, hapStr: s.hapStr, trig: s.trig, texf: s.texf, scrf: s.scrf, dith: s.dith, mip: s.mip, upfix: s.upfix, ovl: s.ovl, cam: s.cam, lvoice: s.lvoice || 'en', ltext: s.ltext || 'en', bind: s.bind""")
L("""add('trig', s.trig ? 1 : 0);""",
  """add('trig', s.trig ? 1 : 0);
    add('texf', s.texf || 'ps2'); add('scrf', s.scrf || 'smooth'); add('dith', s.dith !== false ? 1 : 0); add('mip', s.mip !== false ? 1 : 0); add('upfix', s.upfix !== false ? 1 : 0); add('ovl', s.ovl ? 1 : 0); add('cam', s.cam ? 1 : 0); add('hud', s.hud ? 1 : 0); add('bars', s.bars ? 1 : 0); add('scol', s.scol !== false ? 1 : 0); add('noscarf', s.noscarf ? 1 : 0); add('nohunger', s.nohunger ? 1 : 0); add('norecenter', s.norecenter ? 1 : 0); add('unlockall', s.unlockall ? 1 : 0); add('fairboss', s.fairboss ? 1 : 0); add('bosshp', s.bosshp || '50'); add('infhealth', s.infhealth ? 1 : 0); add('vol', s.vol || '100'); add('fov', s.fov || '100'); add('lvoice', s.lvoice || 'en'); add('ltext', s.ltext || 'en'); add('hdtex', s.hdtex !== false ? 1 : 0); add('dumptex', s.dumptex ? 1 : 0); add('fg', s.fg ? 1 : 0); add('lowlat', s.lowlat ? 1 : 0); add('fgt', s.fgt || 'monitor');""")
L("""[['display', 'Display'], ['graphics', 'Graphics'], ['controller', 'Controller']]""",
  """[['display', 'Display'], ['graphics', 'Graphics'], ['effects', 'Effects'], ['perf', 'FPS'], ['jukebox', 'Jukebox'], ['languages', 'Languages'], ['controller', 'Controls'], ['features', 'Feature Set']]""")
L("""      tabController: s.tab === 'controller',""",
  """      tabController: s.tab === 'controller',
      tabEffects: s.tab === 'effects',
      tabPerf: s.tab === 'perf',
      tabJukebox: s.tab === 'jukebox',
      tabFeatures: s.tab === 'features',
      tabLanguages: s.tab === 'languages',
      langVoiceOpts: this.langOpts(s, 'lvoice'),
      langTextOpts: this.langOpts(s, 'ltext'),
      euDiscText: s.euDisc ? s.euDisc.split(String.fromCharCode(92)).join('/').split('/').pop() : 'Not added: French, German, Italian and Spanish are locked',
      euDiscPath: s.euDisc || '',
      euDiscStyle: s.euDisc ? 'color:#e8c76a' : '',
      euPick: () => { if (this.ingame) { this.toastMsg('Add the European disc from the launcher (outside the game)'); return; } if (this.host) this.host.postMessage('eudiscpick'); },
      euClear: () => { if (this.ingame) { this.toastMsg('Remove the European disc from the launcher (outside the game)'); return; } if (this.host) this.host.postMessage('eudiscclear'); },
      versionText: this.appVersion ? 'v' + this.appVersion : 'Development build',
      jukeRows: (window.JUKEBOX_TRACKS || []).map((t) => {
        const file = (s.juke || {})[t[0]];
        return { code: t[0], label: t[1], file: file ? file : 'Original', style: file ? 'color:#e8c76a' : '',
          pick: () => { if (this.host) this.host.postMessage('jukepick\\nkey=' + t[0]); },
          reset: () => { if (this.host) this.host.postMessage('jukedefault\\nkey=' + t[0]); } };
      }),
      jukeFolder: () => { if (this.host) this.host.postMessage('jukefolder'); },
      hapChips: [[3, 'Light'], [6, 'Medium'], [8, 'Strong'], [10, 'Max']].map((o, i, all) => {
        const v = +(s.hapStr || 10);
        const near = all.reduce((b, x) => Math.abs(x[0] - v) < Math.abs(b[0] - v) ? x : b, all[0])[0] === o[0];
        return { name: o[1], on: near, cls: near ? 'on' : '', pick: () => this.setState({ hapStr: o[0] }) };
      }),
      refreshLauncher: () => { if (this.host) this.host.postMessage('reload'); else location.reload(); },
      musicName: s.musicName || 'Original theme',
      pickMusic: () => { if (this.host) this.host.postMessage('musicpick'); },
      defaultMusic: () => { if (this.host) this.host.postMessage('musicdefault'); },
      fovNum: Math.max(70, Math.min(150, Number(s.fov || 100))),
      fovName: Number(s.fov || 100) === 100 ? '100% (original)' : Number(s.fov || 100) + '%',
      fovBg: fill((Math.max(70, Math.min(150, Number(s.fov || 100))) - 70) / 20),
      setFov: (e) => this.setState({ fov: String(Math.round(Number(e.target.value) / 5) * 5) }),
      volNum: Math.max(0, Math.min(100, Number(s.vol || 100))),
      volName: Number(s.vol || 100) <= 0 ? 'Mute' : Number(s.vol || 100) + '%',
      volBg: fill(Math.max(0, Math.min(100, Number(s.vol || 100))) / 25),
      setVol: (e) => this.setState({ vol: String(Math.round(Number(e.target.value) / 5) * 5) }),
      volOpts: [['100', '100%'], ['80', '80%'], ['60', '60%'], ['40', '40%'], ['20', '20%'], ['0', 'Mute']].map((o) => ({ name: o[1], on: String(s.vol || '100') === o[0], cls: String(s.vol || '100') === o[0] ? 'on' : '', pick: () => this.setState({ vol: o[0] }) })),
      fgtOpts: [['monitor', 'Auto'], ['120', '120'], ['144', '144'], ['240', '240'], ['360', '360'], ['unlimited', 'Max']].map((o) => ({ name: o[1], on: (s.fgt || 'monitor') === o[0], cls: (s.fgt || 'monitor') === o[0] ? 'on' : '', pick: () => this.setState({ fgt: o[0] }) })),
      blendOpts: [['minimum', 'Min'], ['basic', 'Basic'], ['medium', 'Medium'], ['high', 'High']].map((o) => ({ name: o[1], on: (s.blend || 'basic') === o[0], cls: (s.blend || 'basic') === o[0] ? 'on' : '', pick: () => this.setState({ blend: o[0] }) })),
      texfOpts: [['ps2', 'As on PS2'], ['bilinear', 'Bilinear']].map((o) => ({ name: o[1], on: (s.texf || 'ps2') === o[0], cls: (s.texf || 'ps2') === o[0] ? 'on' : '', pick: () => this.setState({ texf: o[0] }) })),
      scrfOpts: [['sharp', 'Sharp'], ['smooth', 'Smooth'], ['scanlines', 'Scanlines'], ['crt', 'CRT']].map((o) => ({ name: o[1], on: (s.scrf || 'smooth') === o[0], cls: (s.scrf || 'smooth') === o[0] ? 'on' : '', pick: () => this.setState({ scrf: o[0] }) })),
      dith: s.dith !== false,
      toggleDith: () => this.setState({ dith: !(this.state.dith !== false) }),
      mip: s.mip !== false,
      toggleMip: () => this.setState({ mip: !(this.state.mip !== false) }),
      upfix: s.upfix !== false,
      toggleUpfix: () => this.setState({ upfix: !(this.state.upfix !== false) }),
      ovl: !!s.ovl,
      toggleOvl: () => this.setState({ ovl: !this.state.ovl }),
      cam: !!s.cam,
      toggleCam: () => this.setState({ cam: !this.state.cam }),
      hud: !!s.hud,
      toggleHud: () => this.setState({ hud: !this.state.hud }),
      bars: !!s.bars,
      toggleBars: () => this.setState({ bars: !this.state.bars }),
      scol: s.scol !== false,
      toggleScol: () => this.setState({ scol: !(this.state.scol !== false) }),
      fg: !!s.fg,
      toggleFg: () => this.setState({ fg: !this.state.fg }),
      lowlat: !!s.lowlat,
      toggleLowlat: () => this.setState({ lowlat: !this.state.lowlat }),""")

# ---- LOCAL PATCH (GitHub issues #4 and #5): controller remapping and invert camera (Configure Controller, Gamepad) ----
L("""      bind: Object.assign({}, this.defBinds, init ? init.binds : {}),""",
  """      bind: Object.assign({}, this.defBinds, init ? init.binds : {}),
      pbind: Object.assign({}, this.defPad, init ? init.pbinds : {}),""")
L("""    const init = window.__launcherInit || null;""",
  """    this.defPad = { up: 'up', down: 'down', left: 'left', right: 'right', cross: 'cross', circle: 'circle', square: 'square', triangle: 'triangle',
      l1: 'l1', r1: 'r1', l2: 'l2', r2: 'r2', start: 'start', select: 'select', l3: 'l3', r3: 'r3' };
    const init = window.__launcherInit || null;""")
L("""bind: s.bind, mods: s.mods });""",
  """bind: s.bind, pbind: s.pbind, invX: !!s.invX, invY: !!s.invY, ovl: !!s.ovl, cam: !!s.cam, hud: !!s.hud, bars: !!s.bars, scol: s.scol !== false, noscarf: !!s.noscarf, nohunger: !!s.nohunger, norecenter: !!s.norecenter, unlockall: !!s.unlockall, fairboss: !!s.fairboss, bosshp: s.bosshp || '50', infhealth: !!s.infhealth, hdtex: s.hdtex !== false, dumptex: !!s.dumptex, fg: !!s.fg, lowlat: !!s.lowlat, fgt: s.fgt || 'monitor', vol: s.vol || '100', fov: s.fov || '100', mods: s.mods });""")
L("""add('trig', s.trig ? 1 : 0);""",
  """add('trig', s.trig ? 1 : 0);
    add('invX', s.invX ? 1 : 0); add('invY', s.invY ? 1 : 0);
    Object.keys(s.pbind || {}).forEach((k) => add('pbind.' + k, s.pbind[k]));""")
L("""    const kb = s.device === 'keyboard';
    const bindRows = BL.map((r) => {""",
  """    const kb = s.device === 'keyboard';
    // Gamepad: each game button shows the controller button that presses it. Pick one, then press the button you want;
    // two game buttons swap their controller buttons rather than share one.
    const PADN = { up: 'D-Pad Up', down: 'D-Pad Down', left: 'D-Pad Left', right: 'D-Pad Right', cross: 'Cross', circle: 'Circle',
      square: 'Square', triangle: 'Triangle', l1: 'L1', r1: 'R1', l2: 'L2', r2: 'R2', start: 'Start', select: 'Select', l3: 'L3', r3: 'R3', none: 'None' };
    const PADROWS = ['up', 'down', 'left', 'right', 'cross', 'circle', 'square', 'triangle', 'l1', 'r1', 'l2', 'r2', 'start', 'select', 'l3', 'r3'];
    const pb = s.pbind || this.defPad;
    const bindRows = !kb ? PADROWS.map((k) => {
      const listening = s.listening === k;
      const now = PADN[pb[k]] || PADN[k];
      return {
        label: PADN[k],
        key: listening ? 'Press a button...' : now,
        cls: listening ? 'listen' : '',
        aria: 'Remap ' + PADN[k] + ', currently the ' + now + ' button',
        pick: () => { this._capArm = false; this.setState({ listening: k }); },
        onKey: (e) => { if (s.listening === k && e.key === 'Escape') { e.preventDefault(); this.setState({ listening: null }); } },
        onBlur: () => {}
      };
    }) : BL.map((r) => {""")
L("""      bindNote: kb ? 'Click a binding, then press the key you want. Esc cancels. A key already in use is moved to the new action.' : 'Connected gamepads use the standard layout shown here, mapped automatically.',
      resetBinds: set({ bind: Object.assign({}, this.defBinds), listening: null }),""",
  """      bindNote: kb ? 'Click a binding, then press the key you want. Esc cancels. A key already in use is moved to the new action.' : 'Pick a button (click it or press Cross on it), then press the controller button you want for it. Two buttons swap places. Esc cancels.',
      resetBinds: () => this.setState(kb ? { bind: Object.assign({}, this.defBinds), listening: null } : { pbind: Object.assign({}, this.defPad), invX: false, invY: false, listening: null }),
      padView: !kb,
      invX: !!s.invX,
      toggleInvX: () => this.setState({ invX: !this.state.invX }),
      invY: !!s.invY,
      toggleInvY: () => this.setState({ invY: !this.state.invY }),""")
# Capture a button while a Gamepad row is waiting: the Gamepad API (Xbox and other pads) here, a DualSense the launcher
# reads itself in padCmd.
L("""      // While the game is starting or running the controller belongs to the game: Cross would press the launcher's Stop Game button.""",
  """      if (this.state && this.state.listening && this.state.device === 'gamepad') {
        const STD = ['cross', 'circle', 'square', 'triangle', 'l1', 'r1', 'l2', 'r2', 'select', 'start', 'l3', 'r3', 'up', 'down', 'left', 'right'];
        let hit = null;
        pads.forEach((p) => { if (p.mapping === 'standard') STD.forEach((n, i) => { if (!hit && p.buttons[i] && p.buttons[i].pressed) hit = n; }); });
        if (!hit) this._capArm = true;
        else if (this._capArm) {
          this._capArm = false;
          // The button that was just bound counts for the menu only after it is let go.
          ['cross', 'circle', 'start', 'up', 'down', 'left', 'right', 'l1', 'r1'].forEach((n) => { held[n] = { next: Infinity }; });
          this.padCapture(hit);
        }
      }
      // While the game is starting or running the controller belongs to the game: Cross would press the launcher's Stop Game button.""")
L("""    if (name === 'lost') { this.hostPad = false; return; }""",
  """    if (name === 'lost') { this.hostPad = false; return; }
    if (name.indexOf('rsy:') === 0) { this._hostRsy = (parseInt(name.slice(4), 10) || 0) / 100; this._hostRsyT = performance.now(); this._padT = performance.now(); return; }
    if (name !== 'host' && name !== 'lost' && name.indexOf('kind-') !== 0) this._padT = performance.now();
    if (name.indexOf('kind-') === 0) { this._hostKind = true; const k = name.slice(5); this.setState({ padKind: k === 'none' ? null : k }); return; }
    if (this.state && this.state.listening && this.state.device === 'gamepad') { this.padCapture(name); return; }
    // F1 menu: a press counts once (the page may read the same controller itself, see navSetup's poll).
    if (this.ingame) {
      const t = performance.now();
      if (this._padSrc === 'page' && t - (this._padSrcT || 0) < 1500) return;
      this._padSrc = 'host'; this._padSrcT = t;
    }""")
L("""  navTab(d) {""",
  """  padCapture(name) {
    const s = this.state;
    const key = s.listening;
    if (!key || !this.defPad[name]) return;
    const pb = Object.assign({}, s.pbind || this.defPad);
    const old = pb[key];
    Object.keys(pb).forEach((k) => { if (k !== key && pb[k] === name) pb[k] = old; });
    pb[key] = name;
    this.setState({ pbind: pb, listening: null });
  }

  navTab(d) {""")

# Resolution: the GPU warning as a notice when 4K / 8K is picked (it no longer fits in the label).
L("""pick: set({ res: r[0], resOpen: false })""",
  """pick: () => { this.setState({ res: r[0], resOpen: false }); if (r[0] === '4K') this.toastMsg('4K runs well on most graphics cards from the last few years'); else if (r[0] === '8K') this.toastMsg('8K is experimental: high-end graphics cards only'); }""")

# Mod Menu: the game's own mods first (their own settings keys), then the rest.
L("""    const modRows = MODS.map((m) => ({""",
  """    window.MOD_TABS = window.MOD_TABS || [['look', 'Look'], ['camera', 'Camera'], ['gameplay', 'Gameplay'], ['pad', 'Controller'], ['start', 'Start-up'], ['adv', 'Advanced']];
    const GAME_MODS = [
      ['cam', 'Close-up camera (F3)', 'A closer, over-the-shoulder camera for more intimate combat.', false, 'camera'],
      ['norecenter', 'No camera recentering', 'The camera keeps your angle instead of swinging behind Hotsuma as he runs. The right stick and camera reset still work.', false, 'camera'],
      ['hud', 'Hide HUD (F5)', 'Health, kunai, magic, TATE counter and boss gauges hidden for a clean screen.', false, 'look'],
      ['bars', 'IMAX mode (F7)', 'Full-screen TATE finishing moves: no black bars. Movies and story scenes keep theirs.', false, 'look'],
      ['unlockall', 'Unlock everything (Shift+F6)', 'Every difficulty, character and extra open from the start. Your save is not changed.', false, 'gameplay'],
      ['infhealth', 'Infinite health (F8)', 'Hotsuma takes no damage from hits, and Akujiki no longer drains him.', false, 'gameplay'],
      ['fairboss', 'Fair final boss (F9)', 'Hiruko without the hidden rules: quick seals, no parries, ground teleports only, steady lock-on, and self-destructing talismans count for TATE.', false, 'gameplay'],
      ['hdtex', 'HD texture packs (Shift+F5)', 'Loads HD replacement textures (an HD HUD, for example) from the textures folder. Nothing changes without a pack.', true, 'look'],
      ['dumptex', 'Save textures (pack makers)', 'Saves every texture the game uses to textures/SLUS-20459/dumps to make an HD pack from. Slows the game: leave it off to play.', false, 'look'],
      ['nohunger', 'No sword hunger (Shift+F7)', 'Akujiki no longer drains your health when it has not killed for a while.', false, 'gameplay'],
      ['noscarf', 'Remove scarf (Shift+F8)', 'Hotsuma without his scarf.', false, 'look'],
      ['scol', 'Scarf body collision (Shift+F9)', 'The scarf keeps its float and trail but no longer passes through Hotsuma.', true, 'look'],
      ['haptics', 'DualSense haptics', 'Footsteps, sword hits, shuriken, dash and ninja magic felt in a DualSense / DualSense Edge (USB or Bluetooth). Live in game.', true, 'pad'],
      ['trig', 'L2 trigger resistance', 'L2 stiffens part way down and clicks through just before the ninja magic fires. Live in game.', true, 'pad']
    ];
    const gameModRows = GAME_MODS.map((m) => {
      const on = m[3] ? s[m[0]] !== false : !!s[m[0]];
      return { cat: m[4], name: m[1], desc: m[2], on: on, toggle: () => this.setState({ [m[0]]: !on }) };
    });
    const modRows = gameModRows.concat(MODS.map((m) => ({ cat: (m[0] === 'studio' || m[0] === 'intro') ? 'start' : 'adv',""")

L("""      toggle: () => this.setState({ mods: Object.assign({}, this.state.mods, { [m[0]]: !this.state.mods[m[0]] }) })
    }));""",
  """      toggle: () => this.setState({ mods: Object.assign({}, this.state.mods, { [m[0]]: !this.state.mods[m[0]] }) })
    }))).filter((r) => r.cat === (s.modTab || 'look'));""")

# Updates: the host tells the page about a newer release; the button installs it.
L("""    if (m === 'manualhide') { this.manualHide(); return; }""",
  """    if (m === 'manualhide') { this.manualHide(); return; }
    if (m.indexOf('update:') === 0) { const a = m.slice(7).split('|'); this.setState({ updTag: a[0], updUrl: a[1], updBusy: -1 }); return; }
    if (m.indexOf('update-progress:') === 0) { this.setState({ updBusy: +m.slice(16) }); return; }
    if (m.indexOf('update-error:') === 0) { this.setState({ updBusy: -1 }); this.toastMsg(m.slice(13)); return; }
    if (m === 'update-done') { this.setState({ updBusy: 100 }); this.toastMsg('Updated to ' + (this.state.updTag || 'the new version') + ': restarting...'); return; }""")
L("""      tabPerf: s.tab === 'perf',""",
  """      tabPerf: s.tab === 'perf',
      hasUpdate: !!s.updTag && !this.ingame,
      updLabel: s.updBusy >= 0 ? (s.updBusy >= 100 ? 'Restarting...' : 'Updating ' + s.updBusy + '%') : 'Update to ' + (s.updTag || ''),
      updTitle: 'A new version (' + (s.updTag || '') + ') is out. Your saves, game files and settings are kept.',
      doUpdate: () => { if (this.state.updBusy >= 0 || !this.host) return; this.setState({ updBusy: 0 }); this.host.postMessage('update\\nurl=' + this.state.updUrl); },""")

template = template[:logic_start] + logic + template[logic_end:]

# ---- LOCAL PATCH: monochrome theme (black, greys and white; 2026-11-14). The red accents become white everywhere
# (CSS, inline styles, the page's own code); the light panels and controls are turned dark by the rules below, and a
# chosen option is white with black text. The key art, logo and guide pictures stay in colour.
template = must_replace(template,
    '<rect width="18" height="18" rx="3" fill="#b3122a"></rect><path d="M9 2.5l1.7 4.8 4.8 1.7-4.8 1.7L9 15.5l-1.7-4.8L2.5 9l4.8-1.7z" fill="#fff"></path><circle cx="9" cy="9" r="1.6" fill="#b3122a"></circle>',
    '<rect width="18" height="18" rx="3" fill="#fff"></rect><path d="M9 2.5l1.7 4.8 4.8 1.7-4.8 1.7L9 15.5l-1.7-4.8L2.5 9l4.8-1.7z" fill="#000"></path><circle cx="9" cy="9" r="1.6" fill="#fff"></circle>')
template = must_replace(template, "(st[1] ? '#1b1b1f' : '#7a7a82')", "(st[1] ? '#e6e6e6' : '#7a7a7a')")
for _red, _mono in (('#b3122a', '#ffffff'), ('#e0213d', '#ffffff'), ('#a8112a', '#ffffff'), ('#8f1023', '#ffffff'),
                    ('#b8122b', '#ffffff'), ('#ff1f3d', '#ffffff'), ('rgba(255,31,61,', 'rgba(255,255,255,'),
                    ('#e0516a', '#ffffff'), ('#d0152f', '#dddddd'), ('#6e0b1a', '#ffffff'), ('#7d0c1d', '#9a9a9a'),
                    ('#ffd9de', '#444444'), ('#ff4d6a', '#ffffff'), ('#52d88a', '#ffffff'), ('#8e8e95', '#2a2a2a'),
                    ('#1f8a3b', '#ffffff')):
    template = template.replace(_red, _mono)
MONO = (
    'html,body{background:#000!important}.stage{background:#000!important;color:#e6e6e6!important}'
    '.win{background:#000!important;border-color:#2a2a2a!important}'
    '.tb{background:#000!important;border-bottom:1px solid rgba(255,255,255,.55)!important;color:#e6e6e6!important}'
    '.tbb{color:#e6e6e6!important}.tbb:hover{background:#1c1c1c!important}.tbb.cl:hover{background:#c42b1c!important}'
    '.tbb.upd{background:#fff!important;color:#000!important}.tbb.upd:hover{background:#ddd!important}'
    '.left,.right{background:linear-gradient(180deg,#0e0e0f 0%,#060606 100%)!important}'
    '.left{box-shadow:6px 0 18px rgba(0,0,0,.85)!important}.right{box-shadow:-6px 0 18px rgba(0,0,0,.85)!important}'
    '.kanji{color:rgba(255,255,255,.05)!important}'
    '.h,.fl,.grp h2,.chk,.cap,.mrow b,.brow,.cred h4,.dlg{color:#e6e6e6!important}.fl em{color:#9a9a9a!important}'
    '.th,.gview,.gtab{border-color:#2a2a2a!important}.th:hover,.gtab.on{border-color:#fff!important}'
    '.card{background:#000!important;border-color:#262626!important;color:#bdbdbd!important;box-shadow:inset 0 1px 0 rgba(255,255,255,.06)!important}'
    '.card p{color:#a8a8a8!important}'
    '.btn{background:linear-gradient(180deg,#1b1b1c 0%,#0d0d0e 100%)!important;border-color:#333!important;color:#f0f0f0!important;'
    'box-shadow:inset 3px 0 0 #e8e8e8,inset -3px 0 0 #e8e8e8!important}'
    '.btn:hover{filter:none!important;background:linear-gradient(180deg,#262627,#141415)!important;'
    'box-shadow:inset 3px 0 0 #fff,inset -3px 0 0 #fff,0 0 0 1px #fff,0 0 16px rgba(255,255,255,.18)!important}'
    '.btn .fill{background:#fff!important;opacity:.22!important}'
    '.btn.run{background:#fff!important;color:#000!important;border-color:#fff!important}'
    '.btn.warn .lab,.gfile.warn{color:#ff7070!important}'
    '.tra-s,.studio,.mark{color:#6f6f6f!important}.tra-b,.studio b{color:#e6e6e6!important}.tra-b i{color:#9a9a9a!important}'
    '.own{color:#8a8a8a!important;border-left-color:#fff!important}.gfile{color:#8a8a8a!important}'
    '.hero{background:#000!important}'
        '.shade{background:linear-gradient(180deg,rgba(0,0,0,0),rgba(0,0,0,.75))!important}'
    '.chip{background:rgba(0,0,0,.85)!important;border-color:rgba(255,255,255,.18)!important}'
    '.grp{background:linear-gradient(180deg,#0f0f10,#080808)!important;border-color:#2a2a2a!important;'
    'box-shadow:inset 0 0 0 1px #161616,0 2px 10px rgba(0,0,0,.6)!important}'
    '.sel,.kb{background:linear-gradient(180deg,#1a1a1b,#0f0f10)!important;border-color:#333!important;color:#eee!important}'
    '.sel:hover,.kb:hover,.chipb:hover{border-color:#fff!important}.sel svg path{stroke:#ddd!important}'
    '.menu{background:#0c0c0d!important;border-color:#333!important}.opt{color:#ddd!important}.opt em{color:#7a7a7a!important}'
    '.opt:hover{background:#1c1c1d!important}.opt.on{background:#fff!important;color:#000!important}.opt.on em{color:#444!important}'
    '.segs{background:#0c0c0d!important;border-color:#333!important}'
    '.seg{background:linear-gradient(180deg,#1a1a1b,#0f0f10)!important;border-right-color:#2a2a2a!important;color:#cfcfcf!important}'
    '.seg:hover{background:linear-gradient(180deg,#262627,#151516)!important;color:#fff!important}'
    '.seg.on{background:#fff!important;color:#000!important;box-shadow:none!important}'
    '.chipb{background:#141415!important;border-color:#333!important;color:#ddd!important}'
    '.chipb.on,.kb.listen{background:#fff!important;color:#000!important;border-color:#fff!important}'
    '.kb.ro{background:#121213!important;color:#666!important;border-color:#222!important}'
    '.chk .bx{background:#0a0a0a!important;border-color:#555!important;box-shadow:none!important}.chk:hover .bx{border-color:#fff!important}'
    '.chk input:checked + .bx{background:#fff!important;border-color:#fff!important}.chk input:checked + .bx svg path{stroke:#000!important}'
    '.rng input::-webkit-slider-thumb{background:linear-gradient(180deg,#f2f2f2,#9a9a9a)!important;border-color:#000!important}'
    '.sw i{background:#2a2a2a!important}.sw input:checked + i{background:#fff!important}.sw input:checked + i::after{background:#000!important}'
    '.scrim-bg{background:rgba(0,0,0,.78)!important}'
    '.dlg{background:linear-gradient(180deg,#111112,#09090a)!important;border-color:#2a2a2a!important;'
    'box-shadow:0 24px 70px rgba(0,0,0,.85),inset 0 0 0 1px #1a1a1a!important}'
    '.dh{background:#000!important;border-bottom:1px solid rgba(255,255,255,.55)!important}.x:hover{background:#262626!important}'
    '.note,.mrow span{color:#8a8a8a!important}.brow,.mrow{border-bottom-color:#222!important}.cred p{color:#a8a8a8!important}'
    '.toast{background:#000!important;border:1px solid #333!important;border-left:4px solid #fff!important}'
    '.pathtxt{background:#0c0c0d!important;color:#ddd!important;border-color:#333!important}'
    'input[type=checkbox]{accent-color:#fff!important}'
    '::-webkit-scrollbar{width:10px;height:10px}::-webkit-scrollbar-track{background:#0a0a0a}'
    '::-webkit-scrollbar-thumb{background:#333;border-radius:5px}::-webkit-scrollbar-thumb:hover{background:#555}')
template = must_replace(template, '@media (prefers-reduced-motion:reduce)', MONO + '@media (prefers-reduced-motion:reduce)')
# LOCAL PATCH: smaller download. Every picture of 20 KB or more becomes WebP (WebView2 shows it natively): photos at
# quality 85, pictures with transparency at quality 90 with their alpha kept. Kept only where it is smaller.
_webp_saved = 0
for _uid, _res in manifest.items():
    if not isinstance(_res, dict) or _res.get('compressed') or _res.get('mime') not in ('image/jpeg', 'image/png'):
        continue
    _raw = base64.b64decode(_res['data'])
    if len(_raw) < 20000:
        continue
    _im = Image.open(io.BytesIO(_raw))
    _buf = io.BytesIO()
    if _im.mode in ('RGBA', 'LA', 'P'):
        _im.convert('RGBA').save(_buf, 'WEBP', quality=90, method=6)
    else:
        _im.convert('RGB').save(_buf, 'WEBP', quality=85, method=6)
    if _buf.tell() < len(_raw):
        _webp_saved += len(_raw) - _buf.tell()
        _res['mime'] = 'image/webp'
        _res['data'] = base64.b64encode(_buf.getvalue()).decode()
print('pictures as WebP: %d KB smaller' % (_webp_saved // 1024))
set_block('manifest', json.dumps(manifest, separators=(',', ':')))
set_block('template', json.dumps(template).replace('</', r'<\u002F'))  # keep </script> inside the block escaped
os.makedirs(os.path.dirname(out_path), exist_ok=True)
open(out_path, 'w', encoding='utf-8').write(page)
print('wrote', out_path, len(page), 'chars;', len(guide_ids), 'guide pictures')
