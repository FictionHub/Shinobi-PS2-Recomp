# Upscales PS2 texture dumps the right way for PCSX2 replacements, with the Real-ESRGAN build in _esrgan:
#  - PS2 alpha runs 0..128 (128 = solid): it is doubled to 0..255 before upscaling and halved after, so the model does
#    not see a dim grey mask (soft, dark edges) and the result never goes over 128 (bright fringes in game);
#  - colour is upscaled separately, with the colours of the visible pixels spread into the see-through ones first, so
#    no dark halos appear around shapes.
# usage: python ps2_upscale.py [model]   (model: realesrgan-x4plus (default) or realesrgan-x4plus-anime)
#        reads dumps\*.png, writes replacements\*.png (same names, 4x)
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
MODEL = sys.argv[1] if len(sys.argv) > 1 else "realesrgan-x4plus"
EXE = os.path.join(HERE, "_esrgan", "realesrgan-ncnn-vulkan.exe")
MIN_SIDE = 32  # Real-ESRGAN fails on very small pictures (and can then garble the rest of a batch): padded up to this


def pick_gpu():
    """The graphics card to use: the first one that is not a processor's built-in graphics (Real-ESRGAN's own choice
    can be the built-in one, which is slow and can fail)."""
    import re
    probe = subprocess.run([EXE, "-i", "nonexistent.png", "-o", "nonexistent_out.png"], capture_output=True, text=True)
    gpus = re.findall(r"^\[(\d+) ([^\]]+)\]\s+queueC", probe.stderr, re.M)
    for index, name in gpus:
        if not re.search(r"Radeon\(TM\) Graphics|UHD Graphics|Iris|Intel\(R\) HD|Microsoft Basic", name):
            return index, name
    return (gpus[0] if gpus else ("0", "default"))


GPU, GPU_NAME = pick_gpu()
print("Real-ESRGAN on GPU %s (%s), model %s" % (GPU, GPU_NAME.strip(), MODEL))


def pad_small(img):
    """Edge-extends a picture to at least MIN_SIDE on each side; returns it and its original size."""
    a = np.array(img)
    h, w = a.shape[:2]
    ph, pw = max(0, MIN_SIDE - h), max(0, MIN_SIDE - w)
    if ph or pw:
        a = np.pad(a, ((0, ph), (0, pw)) + ((0, 0),) * (a.ndim - 2), mode="edge")
    return Image.fromarray(a), (w, h)


def bleed(rgb, alpha, steps=64):
    """Spread the colours of visible pixels into see-through ones (repeated 4-neighbour averaging)."""
    rgb = rgb.astype(np.float32)
    known = alpha > 0
    if known.all() or not known.any():
        return rgb.astype(np.uint8)
    for _ in range(steps):
        if known.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            k = np.roll(known, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * k[..., None]
            cnt += k
        grow = (~known) & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][..., None]
        known = known | grow
    return rgb.astype(np.uint8)


def esrgan(src, dst):
    subprocess.run([EXE, "-i", src, "-o", dst, "-n", MODEL, "-s", "4", "-f", "png", "-g", GPU], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def matches(src_path, up_path):
    """True when the upscaled picture, shrunk back, looks like its source. Real-ESRGAN's folder mode sometimes saves a
    picture under another picture's name when they differ in size: that is caught here and done again on its own."""
    if not os.path.exists(up_path):
        return False
    src = Image.open(src_path).convert("RGB")
    up = Image.open(up_path).convert("RGB")
    if up.size != (src.width * 4, src.height * 4):  # (both still padded here)
        return False
    small = np.array(up.resize(src.size, Image.BOX), np.float32).ravel()
    orig = np.array(src, np.float32).ravel()
    if float(np.abs(small - orig).mean()) < 12.0:
        return True
    # busy, noisy pictures change more when upscaled, but still follow their source closely (a mixed-up one does not)
    return bool(orig.std() > 0 and small.std() > 0 and np.corrcoef(small, orig)[0, 1] > 0.6)


def esrgan_checked(src_dir, dst_dir, names):
    esrgan(src_dir, dst_dir)  # the whole folder at once (fast) ...
    redo = [f for f in names if not matches(os.path.join(src_dir, f), os.path.join(dst_dir, f))]
    for f in redo:            # ... and any picture that came out wrong, again on its own
        esrgan(os.path.join(src_dir, f), os.path.join(dst_dir, f))
        if not matches(os.path.join(src_dir, f), os.path.join(dst_dir, f)):
            os.remove(os.path.join(dst_dir, f))  # still wrong: a plain resize is used for it (below)
    if redo:
        print("%d pictures came out mixed up in the batch and were done again one by one" % len(redo))


work = tempfile.mkdtemp(prefix="ps2up_")
try:
    for d in ("rgb", "a", "rgb_up", "a_up"):
        os.makedirs(os.path.join(work, d))
    names = sorted(f for f in os.listdir(os.path.join(HERE, "dumps")) if f.lower().endswith(".png"))
    for f in names:
        im = np.array(Image.open(os.path.join(HERE, "dumps", f)).convert("RGBA"))
        a = im[..., 3].astype(np.int32)
        pad_small(Image.fromarray(bleed(im[..., :3], a)))[0].save(os.path.join(work, "rgb", f))
        # alpha usually runs 0..128 (doubled for the model); a few textures use the full 0..255 (kept as they are)
        a255 = np.clip(a * (2 if a.max() <= 128 else 1), 0, 255).astype(np.uint8)
        pad_small(Image.fromarray(np.dstack([a255] * 3)))[0].save(os.path.join(work, "a", f))
    esrgan_checked(os.path.join(work, "rgb"), os.path.join(work, "rgb_up"), names)
    esrgan_checked(os.path.join(work, "a"), os.path.join(work, "a_up"), names)
    os.makedirs(os.path.join(HERE, "replacements"), exist_ok=True)
    for f in names:
        orig_a = np.array(Image.open(os.path.join(HERE, "dumps", f)).convert("RGBA"))[..., 3]
        oh, ow = orig_a.shape
        size4 = (ow * 4, oh * 4)
        src_rgb = Image.open(os.path.join(HERE, "dumps", f)).convert("RGBA")
        plain_rgb = np.array(Image.fromarray(bleed(np.array(src_rgb)[..., :3], orig_a.astype(np.int32))).resize(size4, Image.LANCZOS))
        notes = []
        up_path = os.path.join(work, "rgb_up", f)
        rgb = np.array(Image.open(up_path).convert("RGB"))[:oh * 4, :ow * 4] if os.path.exists(up_path) else None
        visible = orig_a > 8
        if rgb is not None and visible.sum() > 3:
            small = np.array(Image.fromarray(rgb).resize((ow, oh), Image.BOX), np.float32)[visible]
            orig = np.array(src_rgb, np.float32)[..., :3][visible]
            off = float(np.abs(small - orig).mean())
            corr = np.corrcoef(small.ravel(), orig.ravel())[0, 1] if orig.std() > 0 and small.std() > 0 else 1.0
            if off > 20 and corr < 0.6:
                rgb = None  # faint or busy picture the model changed too much
        if rgb is None:
            rgb, notes = plain_rgb, notes + ["colour: plain resize"]
        if orig_a.min() == orig_a.max():
            a = np.full(rgb.shape[:2], orig_a.min(), np.uint8)  # one opacity everywhere: kept exactly
        else:
            top = 128 if orig_a.max() <= 128 else 255
            plain_a = np.clip(np.array(Image.fromarray(orig_a).resize(size4, Image.LANCZOS)), 0, top).astype(np.uint8)
            a_path = os.path.join(work, "a_up", f)
            a = None
            if os.path.exists(a_path):
                a_up = np.array(Image.open(a_path).convert("L")).astype(np.int32)[:oh * 4, :ow * 4]
                a = (np.clip((a_up + 1) // 2, 0, 128) if top == 128 else np.clip(a_up, 0, 255)).astype(np.uint8)
                # the see-through shape must keep its coverage (thin strokes, like a font's, can come out thinner)
                if abs(float(a.mean()) - float(orig_a.mean())) > 4.0:
                    a = None
            if a is None:
                a, notes = plain_a, notes + ["transparency: plain resize"]
        Image.fromarray(np.dstack([rgb, a])).save(os.path.join(HERE, "replacements", f))
        print("%-52s %s -> %s, alpha %d..%d%s" % (f, orig_a.shape[::-1], rgb.shape[1::-1], a.min(), a.max(),
                                                   ("  (" + ", ".join(notes) + ")") if notes else ""))
finally:
    shutil.rmtree(work, ignore_errors=True)
