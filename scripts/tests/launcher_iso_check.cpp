// Standalone check for launcher_iso.cpp: reads the images made by make_test_images.py and verifies that the plain ISO,
// raw-sector BIN, createcd-style CHD and createdvd-style CHD paths all find the same SYSTEM.CNF and extract the same
// file bytes.
//
// Build after configuring devilkings-fork/ps2xRuntime/cmake/libchdr (target shinobi-chdr plus its three static
// dependencies), e.g. with cl /std:c++20 /utf-8 /EHsc /MD /I<launcher src> /I<third_party/libchdr/include>
// launcher_iso_check.cpp launcher_iso.cpp <libs> legacy_stdio_definitions.lib /Fe:launcher_iso_check.exe
// Run: launcher_iso_check.exe <folder with test.iso, test.bin, test.chd, test-dvd.chd>

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
        std::cerr << "Usage: launcher_iso_check <folder with test.iso, test.bin, test.chd, test-dvd.chd>\n";
        return 2;
    }
    const fs::path folder = argv[1];
    std::vector<unsigned char> pattern;
    for (int n = 0; n < 256; ++n)
        pattern.push_back(static_cast<unsigned char>(n));
    std::vector<unsigned char> wanted;
    for (int n = 0; n < 12; ++n)
        wanted.insert(wanted.end(), pattern.begin(), pattern.end());

    for (const char *name : {"test.iso", "test.bin", "test.chd", "test-dvd.chd"})
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
