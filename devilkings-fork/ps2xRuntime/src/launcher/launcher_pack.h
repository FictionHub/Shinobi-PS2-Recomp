#pragma once
// LOCAL PATCH: the game program package (bin/Shinobi.pack). Both game programs (Shinobi.exe for AVX2 CPUs and
// Shinobi-SSE4.exe) in one LZMA stream with the x86 branch filter, the way 7-Zip packs programs: about half the size of
// the old Shinobi.zip, because the two programs are compressed together. Decoded here with the LZMA SDK (public domain,
// src/launcher/lzma), so it needs no tar.exe or zlib1.dll and works the same under Wine/Proton.
//
// Format (little endian): "SHPK", u32 version (1), u32 file count; per file: u16 name length, name (UTF-8),
// u64 size, u32 CRC-32; then 5 bytes of LZMA properties, u64 packed size, the raw LZMA stream of all files one after
// another (x86 filter applied over the whole stream from position 0). Written by make_release.py (scratchpad).
#include <filesystem>
#include <string>

namespace launcher::pack
{
    // Unpacks every file of `pack` into `folder` (each written under a temporary name, CRC checked, then renamed).
    // Empty on success, else what went wrong.
    std::string unpack(const std::filesystem::path &pack, const std::filesystem::path &folder);
}
