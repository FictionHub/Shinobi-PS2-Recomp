# Writes bin/Shinobi.pack: the game programs in one LZMA stream with the x86 branch filter (format: launcher_pack.h).
# usage: python make_pack.py <out.pack> <name>=<file> [<name>=<file> ...]
#   e.g. python make_pack.py Shinobi.pack Shinobi.exe=...\ps2EntryRunner.exe Shinobi-SSE4.exe=...\build-sse4\...\ps2EntryRunner.exe
import lzma
import struct
import sys
import zlib

LC, LP, PB, DICT = 3, 0, 2, 384 << 20  # the decoder reads these from the 5 property bytes

out = sys.argv[1]
files = []
for arg in sys.argv[2:]:
    name, path = arg.split("=", 1)
    data = open(path, "rb").read()
    files.append((name, data))

header = b"SHPK" + struct.pack("<II", 1, len(files))
for name, data in files:
    n = name.encode("utf-8")
    header += struct.pack("<H", len(n)) + n + struct.pack("<QI", len(data), zlib.crc32(data) & 0xFFFFFFFF)

filters = [{"id": lzma.FILTER_X86},
           {"id": lzma.FILTER_LZMA1, "preset": 9 | lzma.PRESET_EXTREME, "dict_size": DICT, "lc": LC, "lp": LP, "pb": PB}]
packed = lzma.compress(b"".join(d for _, d in files), format=lzma.FORMAT_RAW, filters=filters)
props = bytes([(PB * 5 + LP) * 9 + LC]) + struct.pack("<I", DICT)

with open(out + ".new", "wb") as f:
    f.write(header + props + struct.pack("<Q", len(packed)) + packed)
import os
os.replace(out + ".new", out)
print("%s: %d files, %.1f MB -> %.1f MB" % (out, len(files), sum(len(d) for _, d in files) / 2**20, os.path.getsize(out) / 2**20))
