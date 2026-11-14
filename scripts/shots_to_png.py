# Turns the game's raw dumps (PS2X_SCREEN_DUMP_DIR: the whole window with the overlay; or PS2X_FRAME_DUMP_DIR +
# PS2X_FRAME_DUMP_RAW: the game's picture only; both .ppm) into numbered PNGs as they arrive,
# deleting each .ppm once its PNG is written. Runs until the game (Shinobi.exe) has closed and every dump is converted.
# usage: python shots_to_png.py <raw folder> <png folder>
import os
import re
import subprocess
import sys
import time

from PIL import Image

raw, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)


def game_running():
    r = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq Shinobi.exe', '/NH'], capture_output=True, text=True)
    return 'Shinobi.exe' in r.stdout


def index(name):
    m = re.search(r'_(\d+)', name)  # frame_<n>_t... or screen_<n>.ppm
    return int(m.group(1)) if m else 0


seen_game = False
idle = 0
done = 0
while True:
    files = sorted((f for f in os.listdir(raw) if f.endswith('.ppm')), key=index)
    running = game_running()
    seen_game = seen_game or running
    # the newest file may still be being written while the game runs: leave it for the next pass
    ready = files[:-1] if running else files
    for f in ready:
        src = os.path.join(raw, f)
        try:
            img = Image.open(src)
            img.load()
        except Exception:
            continue
        done += 1
        img.save(os.path.join(out, 'shot_%04d.png' % done), optimize=False, compress_level=3)
        img.close()
        os.remove(src)
        print('shot_%04d.png' % done, '%dx%d' % img.size, flush=True)
    if not running:
        idle += 1
        if (seen_game and not files) or (not seen_game and idle > 600):
            break
    time.sleep(1)
print('converted', done, flush=True)
