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


def esrgan(src_dir, dst_dir):
    subprocess.run([EXE, "-i", src_dir, "-o", dst_dir, "-n", MODEL, "-s", "4", "-f", "png"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


work = tempfile.mkdtemp(prefix="ps2up_")
try:
    for d in ("rgb", "a", "rgb_up", "a_up"):
        os.makedirs(os.path.join(work, d))
    names = sorted(f for f in os.listdir(os.path.join(HERE, "dumps")) if f.lower().endswith(".png"))
    for f in names:
        im = np.array(Image.open(os.path.join(HERE, "dumps", f)).convert("RGBA"))
        a = im[..., 3].astype(np.int32)
        Image.fromarray(bleed(im[..., :3], a)).save(os.path.join(work, "rgb", f))
        a255 = np.clip(a * 2, 0, 255).astype(np.uint8)
        Image.fromarray(np.dstack([a255] * 3)).save(os.path.join(work, "a", f))
    esrgan(os.path.join(work, "rgb"), os.path.join(work, "rgb_up"))
    esrgan(os.path.join(work, "a"), os.path.join(work, "a_up"))
    os.makedirs(os.path.join(HERE, "replacements"), exist_ok=True)
    for f in names:
        orig_a = np.array(Image.open(os.path.join(HERE, "dumps", f)).convert("RGBA"))[..., 3]
        rgb = np.array(Image.open(os.path.join(work, "rgb_up", f)).convert("RGB"))
        if orig_a.min() == orig_a.max():
            a = np.full(rgb.shape[:2], orig_a.min(), np.uint8)  # one opacity everywhere: kept exactly
        else:
            a_up = np.array(Image.open(os.path.join(work, "a_up", f)).convert("L")).astype(np.int32)
            a = np.clip((a_up + 1) // 2, 0, 128).astype(np.uint8)
        Image.fromarray(np.dstack([rgb, a])).save(os.path.join(HERE, "replacements", f))
        print("%-52s %s -> %s, alpha %d..%d" % (f, orig_a.shape[::-1], rgb.shape[1::-1], a.min(), a.max()))
finally:
    shutil.rmtree(work, ignore_errors=True)
