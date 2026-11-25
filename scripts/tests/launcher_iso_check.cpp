// Standalone check for launcher_iso.cpp: reads the images made by make_test_images.py and verifies that the plain ISO,
// raw-sector BIN and CHD paths all find the same SYSTEM.CNF and extract the same file bytes.
//
// Build (MinGW, after configuring libchdr with CMake):
//   g++ -std=c++20 -I devilkings-fork/ps2xRuntime/src/launcher -I third_party/libchdr/include \
//       scripts/tests/launcher_iso_check.cpp devilkings-fork/ps2xRuntime/src/launcher/launcher_iso.cpp \
//       <build>/libchdr-static.a <build>/deps/miniz-3.1.1/libminiz.a \
//       <build>/deps/lzma-25.01/libchdr-lzma.a <build>/deps/zstd-1.5.7/libzstd.a -o launcher_iso_check.exe
// Run: launcher_iso_check.exe <folder with test.iso, test.bin, test.chd>

#include "launcher_iso.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: launcher_iso_check <folder with test.iso, test.bin, test.chd>\n";
        return 2;
    }
    const fs::path folder = argv[1];
    std::vector<unsigned char> pattern;
    for (int n = 0; n < 256; ++n)
        pattern.push_back(static_cast<unsigned char>(n));
    std::vector<unsigned char> wanted;
    for (int n = 0; n < 12; ++n)
        wanted.insert(wanted.end(), pattern.begin(), pattern.end());

    for (const char *name : {"test.iso", "test.bin", "test.chd"})
    {
        const fs::path image = folder / name;
        const launcher::iso::Check check = launcher::iso::check(image, "SLUS_204.59");
        std::cout << name << ": " << check.message << "\n";
        if (!check.ok || check.bootFile != "SLUS_204.59")
            return 1;

        const fs::path destination = folder / (std::string(name) + ".out");
        std::error_code ec;
        fs::remove_all(destination, ec);
        fs::create_directories(destination, ec);
        const std::string error = launcher::iso::extractAll(image, destination, [](int, const std::string &) { return true; });
        if (!error.empty())
        {
            std::cerr << name << ": extractAll: " << error << "\n";
            return 1;
        }
        const fs::path file = destination / "TEST.TXT";
        std::ifstream in(file, std::ios::binary);
        const std::vector<unsigned char> got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (got != wanted)
        {
            std::cerr << name << ": TEST.TXT differs (" << got.size() << " bytes)\n";
            return 1;
        }
        std::cout << name << ": extracted " << got.size() << " bytes correctly\n";
    }
    std::cout << "All synthetic images passed.\n";
    return 0;
}
