// LOCAL PATCH: bin/Shinobi.pack, the game program package (format in launcher_pack.h).
#include "launcher_pack.h"

extern "C"
{
#include "lzma/Bra.h"
#include "lzma/LzmaDec.h"
}

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace launcher::pack
{
    namespace
    {
        uint32_t crc32(const uint8_t *data, size_t size)
        {
            static const std::array<uint32_t, 256> table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                return t;
            }();
            uint32_t c = 0xFFFFFFFFu;
            for (size_t i = 0; i < size; ++i)
                c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
            return c ^ 0xFFFFFFFFu;
        }

        void *lzmaAlloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
        void lzmaFree(ISzAllocPtr, void *address) { std::free(address); }

        struct Entry
        {
            std::string name;
            uint64_t size = 0;
            uint32_t crc = 0;
        };

        struct Reader
        {
            const std::vector<uint8_t> &data;
            size_t at = 0;
            bool ok = true;
            template <typename T> T get()
            {
                T v{};
                if (at + sizeof(T) > data.size())
                {
                    ok = false;
                    return v;
                }
                std::memcpy(&v, data.data() + at, sizeof(T)); // little endian, as x64 is
                at += sizeof(T);
                return v;
            }
        };
    }

    // LOCAL PATCH (v0.7.3, player report "It won't run because avutil-59 is not found"): the first file of the package
    // that is missing from `folder` or has another size (empty: all there). Only the header is read. A DLL taken away
    // after the unpack (an antivirus quarantine, a store's update removing files that are not in the release zip) left
    // Shinobi.exe unable to start, and the launcher never unpacked again because the package itself had not changed.
    std::string firstMissing(const fs::path &pack, const fs::path &folder)
    {
        std::ifstream in(pack, std::ios::binary);
        if (!in)
            return {};
        std::vector<uint8_t> head(64 * 1024);
        in.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<size_t>(in.gcount()));
        Reader r{head};
        if (head.size() < 12 || std::memcmp(head.data(), "SHPK", 4) != 0)
            return {};
        r.at = 4;
        if (r.get<uint32_t>() != 1)
            return {};
        const uint32_t count = r.get<uint32_t>();
        if (!r.ok || count == 0 || count > 64)
            return {};
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint16_t length = r.get<uint16_t>();
            if (!r.ok || r.at + length > head.size())
                return {};
            const std::string name(reinterpret_cast<const char *>(head.data() + r.at), length);
            r.at += length;
            const uint64_t size = r.get<uint64_t>();
            (void)r.get<uint32_t>();
            if (!r.ok || name.empty() || name.find_first_of("\\:") != std::string::npos || name.find("..") != std::string::npos)
                return {};
            std::error_code ec;
            const fs::path p = folder / fs::path(std::u8string(name.begin(), name.end()));
            if (!fs::exists(p, ec) || fs::file_size(p, ec) != size)
                return name;
        }
        return {};
    }

    std::string unpack(const fs::path &pack, const fs::path &folder)
    {
        std::vector<uint8_t> file;
        {
            std::ifstream in(pack, std::ios::binary);
            if (!in)
                return "The game program package could not be opened.";
            in.seekg(0, std::ios::end);
            file.resize(static_cast<size_t>(in.tellg()));
            in.seekg(0);
            in.read(reinterpret_cast<char *>(file.data()), static_cast<std::streamsize>(file.size()));
            if (!in)
                return "The game program package could not be read.";
        }
        Reader r{file};
        if (file.size() < 12 || std::memcmp(file.data(), "SHPK", 4) != 0)
            return "The game program package is not valid.";
        r.at = 4;
        if (r.get<uint32_t>() != 1)
            return "The game program package is from a newer version: download the latest release.";
        const uint32_t count = r.get<uint32_t>();
        if (!r.ok || count == 0 || count > 64)
            return "The game program package is damaged.";
        std::vector<Entry> entries(count);
        uint64_t total = 0;
        for (Entry &e : entries)
        {
            const uint16_t length = r.get<uint16_t>();
            if (!r.ok || r.at + length > file.size())
                return "The game program package is damaged.";
            e.name.assign(reinterpret_cast<const char *>(file.data() + r.at), length);
            r.at += length;
            e.size = r.get<uint64_t>();
            e.crc = r.get<uint32_t>();
            // Plain names, or a relative path with '/' (v0.7: the graphics DLLs, pcsx2-gs-bridge/...). Nothing may land
            // outside the folder: no '\', ':', leading '/', empty or ".." parts.
            bool safe = r.ok && !e.name.empty() && e.name.find_first_of("\\:") == std::string::npos && e.name.front() != '/' &&
                        e.name.back() != '/' && e.name.find("//") == std::string::npos;
            for (size_t p = 0; safe && p <= e.name.size();)
            {
                size_t q = e.name.find('/', p);
                if (q == std::string::npos)
                    q = e.name.size();
                const std::string part = e.name.substr(p, q - p);
                safe = part != "." && part != "..";
                p = q + 1;
            }
            if (!safe)
                return "The game program package is damaged.";
            total += e.size;
        }
        uint8_t props[LZMA_PROPS_SIZE];
        if (r.at + LZMA_PROPS_SIZE > file.size())
            return "The game program package is damaged.";
        std::memcpy(props, file.data() + r.at, LZMA_PROPS_SIZE);
        r.at += LZMA_PROPS_SIZE;
        const uint64_t packed = r.get<uint64_t>();
        if (!r.ok || r.at + packed > file.size() || total == 0 || total > (2ull << 30))
            return "The game program package is damaged.";

        std::vector<uint8_t> out;
        try
        {
            out.resize(static_cast<size_t>(total));
        }
        catch (...)
        {
            return "Not enough memory to unpack the game program (about " + std::to_string(total >> 20) + " MB needed).";
        }
        SizeT outLen = static_cast<SizeT>(total), inLen = static_cast<SizeT>(packed);
        ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
        const ISzAlloc alloc{lzmaAlloc, lzmaFree};
        const SRes res = LzmaDecode(out.data(), &outLen, file.data() + r.at, &inLen, props, LZMA_PROPS_SIZE, LZMA_FINISH_END,
                                    &status, &alloc);
        if (res != SZ_OK || outLen != total ||
            (status != LZMA_STATUS_FINISHED_WITH_MARK && status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK))
            return "The game program package is damaged (unpacking failed: code " + std::to_string(res) + ", status " +
                   std::to_string(static_cast<int>(status)) + ", " + std::to_string(outLen) + " of " + std::to_string(total) +
                   " bytes).";
        std::vector<uint8_t>().swap(file); // the packed bytes are no longer needed
        UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
        z7_BranchConvSt_X86_Dec(out.data(), out.size(), 0, &state);

        size_t at = 0;
        for (const Entry &e : entries)
        {
            const uint8_t *data = out.data() + at;
            if (crc32(data, static_cast<size_t>(e.size)) != e.crc)
                return "The game program package is damaged (" + e.name + " does not check out).";
            const fs::path target = (folder / fs::path(std::u8string(e.name.begin(), e.name.end()))).make_preferred();
            const fs::path temp = target.wstring() + L".unpacking";
            {
                std::error_code dirError;
                fs::create_directories(target.parent_path(), dirError);
            }
            {
                std::ofstream o(temp, std::ios::binary | std::ios::trunc);
                o.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(e.size));
                if (!o)
                {
                    std::error_code ec;
                    o.close();
                    fs::remove(temp, ec);
                    return "Could not write " + e.name + " (is there room on the drive, and is the game closed?).";
                }
            }
            std::error_code ec;
            fs::rename(temp, target, ec);
            if (ec)
            {
                fs::remove(target, ec);
                fs::rename(temp, target, ec);
                if (ec)
                {
                    fs::remove(temp, ec);
                    return "Could not replace " + e.name + " (close the game and try again).";
                }
            }
            at += static_cast<size_t>(e.size);
        }
        return {};
    }
}
