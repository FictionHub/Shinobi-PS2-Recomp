#include "launcher_iso.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#include <libchdr/chd.h>

namespace fs = std::filesystem;

namespace launcher::iso
{
    namespace
    {
        // A CHD stores CD frames of 2352 raw bytes + 96 subcode bytes. Mode 1's 2048 ISO bytes start 16 bytes into the
        // raw frame (12 sync + 3 address + 1 mode); Mode 2 Form 1 puts them at 24.
        constexpr uint32_t kCdFrameBytes = 2448;
        constexpr uint32_t kMode1DataOffset = 16;
        constexpr uint32_t kMode2Form1DataOffset = 24;

        struct Image
        {
            std::ifstream file;  // plain .iso / .bin / .img
            FILE *raw = nullptr; // the FILE handed to libchdr (libchdr does not take ownership)
            chd_file *chd = nullptr;
            std::vector<uint8_t> hunk; // one decoded CHD hunk, reused while extracting
            uint32_t cachedHunk = std::numeric_limits<uint32_t>::max();
            uint32_t framesPerHunk = 0;
            uint64_t totalFrames = 0;
            uint32_t sectorSize = 2048; // bytes per sector in the file (plain images)
            uint32_t dataOffset = 0;    // where the 2048 data bytes start inside a sector (plain images)
            uint64_t sectors = 0;

            ~Image()
            {
                if (chd)
                    chd_close(chd);
                if (raw)
                    std::fclose(raw);
            }

            Image() = default;
            Image(const Image &) = delete;
            Image &operator=(const Image &) = delete;

            bool readSector(uint32_t lba, uint8_t *out2048)
            {
                if (chd)
                    return readChdSector(lba, out2048);
                file.clear();
                file.seekg(static_cast<std::streamoff>(static_cast<uint64_t>(lba) * sectorSize + dataOffset));
                file.read(reinterpret_cast<char *>(out2048), 2048);
                return static_cast<bool>(file) && file.gcount() == 2048;
            }

        private:
            bool readChdSector(uint32_t lba, uint8_t *out2048)
            {
                const uint64_t frame = lba; // a single-track CD: disc LBA 0 is CHD frame 0
                if (frame >= totalFrames)
                    return false;
                const uint32_t number = static_cast<uint32_t>(frame / framesPerHunk);
                if (number != cachedHunk)
                {
                    if (chd_read(chd, number, hunk.data()) != CHDERR_NONE)
                        return false;
                    cachedHunk = number;
                }
                const uint8_t *sector = hunk.data() + static_cast<size_t>(frame % framesPerHunk) * kCdFrameBytes;
                // Byte 15 of a raw sector is its mode. Only Mode 1 and Mode 2 Form 1 carry 2048 ISO bytes.
                uint32_t offset = 0;
                if (sector[15] == 1)
                    offset = kMode1DataOffset;
                else if (sector[15] == 2 && sector[23] == 1)
                    offset = kMode2Form1DataOffset;
                else
                    return false;
                if (offset + 2048 > kCdFrameBytes)
                    return false;
                std::memcpy(out2048, sector + offset, 2048);
                return true;
            }
        };

        struct Entry
        {
            std::string path; // inside the image, '/' separated, no version suffix
            uint32_t lba = 0;
            uint32_t size = 0;
            bool directory = false;
        };

        uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

        // CHDs are recognised by their magic, not by the file name, just like the ISO layouts below. The FILE stays open
        // for libchdr's lifetime; Image's destructor closes both it and the chd_file.
        bool openChd(Image &image, const fs::path &path, std::string &error)
        {
            const chd_error err = chd_open_file(image.raw, CHD_OPEN_READ, nullptr, &image.chd);
            if (err != CHDERR_NONE)
            {
                error = std::string("This CHD image could not be opened: ") + chd_error_string(err);
                return false;
            }
            const chd_header *header = chd_get_header(image.chd);
            if (header->unitbytes != kCdFrameBytes || header->hunkbytes == 0 ||
                header->hunkbytes % header->unitbytes != 0 || header->hunkbytes > 4u * 1024u * 1024u)
            {
                error = "This CHD is not a CD image with 2448-byte frames (make it from a disc with chdman createcd).";
                return false;
            }
            image.framesPerHunk = header->hunkbytes / header->unitbytes;
            image.totalFrames = header->unitcount;
            image.sectors = header->unitcount;
            image.hunk.resize(header->hunkbytes);
            image.cachedHunk = std::numeric_limits<uint32_t>::max();
            return true;
        }

        bool openImage(Image &image, const fs::path &path, std::string &error)
        {
#if defined(_WIN32)
            image.raw = _wfopen(path.wstring().c_str(), L"rb");
#else
            image.raw = std::fopen(path.string().c_str(), "rb");
#endif
            if (!image.raw)
            {
                error = "The file could not be opened.";
                return false;
            }
            char magic[8] = {};
            const bool isChd = std::fread(magic, 1, sizeof(magic), image.raw) == sizeof(magic) &&
                               std::memcmp(magic, "MComprHD", sizeof(magic)) == 0;
            if (isChd)
            {
                std::rewind(image.raw);
                return openChd(image, path, error);
            }
            std::fclose(image.raw);
            image.raw = nullptr;

            image.file.open(path, std::ios::binary);
            if (!image.file)
            {
                error = "The file could not be opened.";
                return false;
            }
            image.file.seekg(0, std::ios::end);
            const uint64_t length = static_cast<uint64_t>(image.file.tellg());
            // The primary volume descriptor ("CD001") is at sector 16: in a plain .iso its data starts at the sector, in a raw
            // 2352-byte image after a 16 byte (mode 1) or 24 byte (mode 2) header.
            struct Layout { uint32_t size, offset; };
            for (const Layout layout : {Layout{2048, 0}, Layout{2352, 16}, Layout{2352, 24}})
            {
                image.sectorSize = layout.size;
                image.dataOffset = layout.offset;
                uint8_t sector[2048];
                if (image.readSector(16, sector) && sector[0] == 1 && std::memcmp(sector + 1, "CD001", 5) == 0)
                {
                    image.sectors = length / layout.size;
                    return true;
                }
            }
            error = "This is not a disc image (no ISO 9660 volume found).";
            return false;
        }

        // Every file and folder below directory (extent lba, size bytes).
        bool listDirectory(Image &image, uint32_t lba, uint32_t size, const std::string &prefix, std::vector<Entry> &out, int depth)
        {
            if (depth > 16)
                return true;
            std::vector<uint8_t> data(((size + 2047u) / 2048u) * 2048u);
            for (uint32_t s = 0; s * 2048u < data.size(); ++s)
                if (!image.readSector(lba + s, data.data() + s * 2048u))
                    return false;
            uint32_t at = 0;
            while (at < size)
            {
                const uint8_t length = data[at];
                if (length == 0)
                {
                    at = (at / 2048u + 1u) * 2048u; // records do not cross a sector: the rest is padding
                    continue;
                }
                if (at + length > data.size() || length < 34)
                    break;
                const uint8_t *record = data.data() + at;
                const uint8_t nameLength = record[32];
                if (!(nameLength == 1 && (record[33] == 0 || record[33] == 1)) && 33u + nameLength <= length)
                {
                    std::string name(reinterpret_cast<const char *>(record + 33), nameLength);
                    const size_t version = name.find(';');
                    if (version != std::string::npos)
                        name.resize(version);
                    if (!name.empty() && name.back() == '.')
                        name.pop_back();
                    Entry entry;
                    entry.path = prefix.empty() ? name : prefix + "/" + name;
                    entry.lba = le32(record + 2);
                    entry.size = le32(record + 10);
                    entry.directory = (record[25] & 2) != 0;
                    out.push_back(entry);
                    if (entry.directory && !listDirectory(image, entry.lba, entry.size, entry.path, out, depth + 1))
                        return false;
                }
                at += length;
            }
            return true;
        }

        bool listImage(Image &image, std::vector<Entry> &out, std::string &error)
        {
            uint8_t pvd[2048];
            if (!image.readSector(16, pvd))
            {
                error = "The disc image is cut short.";
                return false;
            }
            const uint8_t *root = pvd + 156;
            if (!listDirectory(image, le32(root + 2), le32(root + 10), "", out, 0))
            {
                error = "The disc image's file table could not be read.";
                return false;
            }
            return true;
        }

        std::string readFile(Image &image, const Entry &entry, uint32_t limit = 1u << 20)
        {
            std::string text;
            const uint32_t size = std::min(entry.size, limit);
            std::vector<uint8_t> sector(2048);
            for (uint32_t done = 0; done < size; done += 2048)
            {
                if (!image.readSector(entry.lba + done / 2048u, sector.data()))
                    break;
                text.append(reinterpret_cast<const char *>(sector.data()), std::min<uint32_t>(2048u, size - done));
            }
            return text;
        }

        std::string upper(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }
    }

    Check check(const fs::path &path, const std::string &expectedBootFile)
    {
        Check result;
        Image image;
        std::string error;
        if (!openImage(image, path, error))
        {
            result.message = error;
            return result;
        }
        std::vector<Entry> entries;
        if (!listImage(image, entries, error))
        {
            result.message = error;
            return result;
        }
        for (const Entry &entry : entries)
            if (upper(entry.path) == "SYSTEM.CNF")
            {
                const std::string text = upper(readFile(image, entry, 4096));
                const size_t boot = text.find("BOOT2");
                if (boot != std::string::npos)
                {
                    size_t start = text.find('\\', boot);
                    start = start == std::string::npos ? text.find('=', boot) : start;
                    const size_t end = text.find_first_of(";\r\n", start == std::string::npos ? boot : start);
                    if (start != std::string::npos && end != std::string::npos && end > start)
                        result.bootFile = text.substr(start + 1, end - start - 1);
                }
            }
        if (result.bootFile.empty())
        {
            result.message = "This disc image has no PlayStation 2 boot file (SYSTEM.CNF).";
            return result;
        }
        if (upper(result.bootFile) != upper(expectedBootFile))
        {
            result.message = "This is a different game (" + result.bootFile + "). This launcher needs Shinobi (USA), " + expectedBootFile + ".";
            return result;
        }
        result.ok = true;
        result.message = "Shinobi (USA), " + result.bootFile + ". Ready to set up.";
        return result;
    }

    std::string extractAll(const fs::path &path, const fs::path &destination, const std::function<bool(int, const std::string &)> &progress)
    {
        Image image;
        std::string error;
        if (!openImage(image, path, error))
            return error;
        std::vector<Entry> entries;
        if (!listImage(image, entries, error))
            return error;
        uint64_t total = 0, done = 0;
        for (const Entry &entry : entries)
            if (!entry.directory)
                total += entry.size;
        total = std::max<uint64_t>(total, 1);
        std::vector<uint8_t> buffer(2048u * 512u); // 1 MB at a time
        for (const Entry &entry : entries)
        {
            const fs::path target = destination / fs::path(entry.path);
            std::error_code ec;
            if (entry.directory)
            {
                fs::create_directories(target, ec);
                continue;
            }
            fs::create_directories(target.parent_path(), ec);
            if (fs::exists(target, ec) && fs::file_size(target, ec) == entry.size)
            {
                done += entry.size; // already there
                if (progress && !progress(static_cast<int>(done * 100 / total), entry.path))
                    return "Cancelled.";
                continue;
            }
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out)
                return "Could not write " + target.string() + " (is there room on the drive?).";
            uint32_t remaining = entry.size;
            uint32_t lba = entry.lba;
            std::vector<uint8_t> sector(2048);
            while (remaining > 0)
            {
                const uint32_t sectorsNow = std::min<uint32_t>(512u, (remaining + 2047u) / 2048u);
                for (uint32_t s = 0; s < sectorsNow; ++s)
                    if (!image.readSector(lba + s, buffer.data() + static_cast<size_t>(s) * 2048u))
                        return "The disc image is damaged near " + entry.path + ".";
                const uint32_t bytes = std::min<uint32_t>(remaining, sectorsNow * 2048u);
                out.write(reinterpret_cast<const char *>(buffer.data()), bytes);
                if (!out)
                    return "Could not write " + target.string() + " (is there room on the drive?).";
                remaining -= bytes;
                lba += sectorsNow;
                done += bytes;
                if (progress && !progress(static_cast<int>(std::min<uint64_t>(done * 100 / total, 100)), entry.path))
                    return "Cancelled.";
            }
        }
        if (progress)
            progress(100, "");
        return {};
    }
}
