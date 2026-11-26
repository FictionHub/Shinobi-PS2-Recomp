"""Make synthetic disc images for the launcher's ISO/CHD reader tests.

No game data is used: just a tiny ISO 9660 image with a SYSTEM.CNF that names
SLUS_204.59, written out as a plain .iso, a raw-sector .bin, a createcd-style
CHD v5 (2448-byte CD frames) and a createdvd-style CHD v5 (2048-byte DVD
sectors). The launcher_iso_check.cpp companion verifies that the launcher reads
all five the same way.

Usage: python make_test_images.py <output folder>
"""

import os
import struct
import sys

SECTOR = 2048
FRAME = 2448          # 2352 raw bytes + 96 subcode bytes in a CHD CD frame
HUNK_FRAMES = 8
SECTORS = 25
BOOT = b"BOOT2 = cdrom0:\\SLUS_204.59;1\r\n"
TEST_FILE = bytes(range(256)) * 12


def put32(data, offset, value):
    struct.pack_into("<I", data, offset, value)
    struct.pack_into(">I", data, offset + 4, value)


def directory_record(lba, size, name, directory=False):
    name = name.encode("ascii") + b";1"
    length = 33 + len(name)
    if length % 2:
        length += 1
    record = bytearray(length)
    record[0] = length
    put32(record, 2, lba)
    put32(record, 10, size)
    record[25] = 2 if directory else 0
    record[32] = len(name)
    record[33 : 33 + len(name)] = name
    return bytes(record)


def make_iso():
    image = bytearray(SECTOR * SECTORS)
    pvd = bytearray(SECTOR)
    pvd[0:6] = b"\x01CD001"
    pvd[6] = 1
    put32(pvd, 80, SECTORS)
    root = bytearray(34)
    root[0] = 34
    put32(root, 2, 20)          # root directory at LBA 20
    put32(root, 10, SECTOR)
    root[25] = 2
    root[32] = 1                # name length 1: directory name "\0"
    pvd[156:190] = root
    image[16 * SECTOR : 17 * SECTOR] = pvd

    listing = directory_record(22, len(BOOT), "SYSTEM.CNF") + directory_record(23, len(TEST_FILE), "TEST.TXT")
    image[20 * SECTOR : 21 * SECTOR] = listing.ljust(SECTOR, b"\0")
    image[22 * SECTOR : 22 * SECTOR + len(BOOT)] = BOOT
    image[23 * SECTOR : 23 * SECTOR + len(TEST_FILE)] = TEST_FILE
    return bytes(image)


def raw_frame(data, lba):
    """One CHD CD frame: 12 sync bytes, 3 address bytes, mode 1, 2048 data, ECC, subcode."""
    frame = bytearray(FRAME)
    frame[0:12] = b"\x00" + b"\xff" * 11
    frame[12:15] = lba.to_bytes(3, "big")
    frame[15] = 1
    frame[16 : 16 + SECTOR] = data
    return bytes(frame)


def make_chd(iso):
    frames = [raw_frame(iso[n * SECTOR : (n + 1) * SECTOR], n) for n in range(SECTORS)]
    hunkbytes = HUNK_FRAMES * FRAME
    hunkcount = (len(frames) + HUNK_FRAMES - 1) // HUNK_FRAMES
    # chdman pads the last hunk to a full hunk too: libchdr always reads whole hunks.
    hunks = b"".join(b"".join(frames[i : i + HUNK_FRAMES]).ljust(hunkbytes, b"\0")
                     for i in range(0, len(frames), HUNK_FRAMES))

    header = bytearray(124)
    header[0:8] = b"MComprHD"
    struct.pack_into(">I", header, 8, len(header))
    struct.pack_into(">I", header, 12, 5)      # CHD v5
    struct.pack_into(">Q", header, 32, len(frames) * FRAME)
    struct.pack_into(">Q", header, 40, len(header))  # map offset
    struct.pack_into(">Q", header, 48, 0)       # no metadata
    struct.pack_into(">I", header, 56, hunkbytes)
    struct.pack_into(">I", header, 60, FRAME)   # CD frame size
    # An uncompressed v5 map is one big-endian hunk number per hunk (offset / hunkbytes), so the hunks
    # themselves must start on a hunk boundary like real chdman output does.
    data_offset = ((len(header) + 4 * hunkcount + hunkbytes - 1) // hunkbytes) * hunkbytes
    # libchdr treats a zero map entry as an empty hunk, so entry n points at data_offset / hunkbytes + n.
    mapping = b"".join(struct.pack(">I", data_offset // hunkbytes + n) for n in range(hunkcount))
    result = bytearray(data_offset + len(hunks))
    result[: len(header)] = header
    result[len(header) : len(header) + len(mapping)] = mapping
    result[data_offset:] = hunks
    return bytes(result)


def make_chd_dvd(iso, units_per_hunk=16):
    """A chdman createdvd-style CHD v5: plain 2048-byte sectors, no sync/mode/ECC header."""
    units = [iso[n * SECTOR : (n + 1) * SECTOR] for n in range(SECTORS)]
    hunkbytes = units_per_hunk * SECTOR
    hunkcount = (len(units) + units_per_hunk - 1) // units_per_hunk
    hunks = b"".join(b"".join(units[i : i + units_per_hunk]).ljust(hunkbytes, b"\0")
                     for i in range(0, len(units), units_per_hunk))

    header = bytearray(124)
    header[0:8] = b"MComprHD"
    struct.pack_into(">I", header, 8, len(header))
    struct.pack_into(">I", header, 12, 5)
    struct.pack_into(">Q", header, 32, len(units) * SECTOR)
    struct.pack_into(">Q", header, 40, len(header))
    struct.pack_into(">Q", header, 48, 0)
    struct.pack_into(">I", header, 56, hunkbytes)
    struct.pack_into(">I", header, 60, SECTOR)  # DVD sector size: the case chdman createdvd writes
    data_offset = ((len(header) + 4 * hunkcount + hunkbytes - 1) // hunkbytes) * hunkbytes
    mapping = b"".join(struct.pack(">I", data_offset // hunkbytes + n) for n in range(hunkcount))
    result = bytearray(data_offset + len(hunks))
    result[: len(header)] = header
    result[len(header) : len(header) + len(mapping)] = mapping
    result[data_offset:] = hunks
    return bytes(result)


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)
    iso = make_iso()
    with open(os.path.join(folder, "test.iso"), "wb") as f:
        f.write(iso)
    with open(os.path.join(folder, "test.bin"), "wb") as f:
        for n in range(SECTORS):
            f.write(raw_frame(iso[n * SECTOR : (n + 1) * SECTOR], n)[:2352])
    with open(os.path.join(folder, "test.chd"), "wb") as f:
        f.write(make_chd(iso))
    with open(os.path.join(folder, "test-dvd.chd"), "wb") as f:
        f.write(make_chd_dvd(iso))
    print("Wrote test.iso, test.bin, test.chd and test-dvd.chd to", folder)
    return 0


if __name__ == "__main__":
    sys.exit(main())
