// LOCAL PATCH: Languages. The US game already has a language system (GameSystem::SetLanguage / SetVoice /
// IsSubtitle, English + Japanese, files picked by letter: E1000050.ADX / J1000050.ADX, EV01_00_00E.SOT /
// EV01_00_00J.SOT, ...). The European release is the same game with four more letters (F, G, I, S) on the same file
// names, plus movies that carry five audio tracks (0 English, 1 French, 2 German, 3 Italian, 4 Spanish).
//
// The game reads all of its files from ROFSSMP.CVM (a CRI CVM: a 0x1800-byte header, then an ISO 9660 image) through
// the CD layer (Kernel/Stubs/Helpers/Support.h). When a language other than English is chosen, that file is replaced
// by a virtual one: the US image unchanged, except
//   - its root directory, where each English / Japanese file's record points at the chosen language's file instead
//     (same name, so the game's own code loads it without knowing);
//   - the chosen language's files from the player's European disc image, appended after the US image and read from
//     that disc image on demand (nothing is copied);
//   - the volume size (ISO volume descriptor, CVM ZONE record) grown to cover them.
// Japanese needs no other disc (its files are on the US disc: the records just point at them).
//
// Rules (checked against the real discs: scratchpad p0_rules.py, all 472 voice and 183 text files found per language):
//   voice V: [EJ]nnnnnnn.ADX -> Vnnnnnnn.ADX; EVaa_bb_cc[EJ].ADX -> EVaa_bb_ccV.ADX (cutscene dubs)
//   text T:  EVaa_bb_cc[EJ].SOT -> ...T.SOT (subtitles); SYSMES_[EJ].SOT -> SYSMES_T.SOT;
//            SEaa_bb_cc[J].MLT -> SEaa_bb_cc[T].MLT (stage messages; the US "_03" set is the European "_01");
//            SE_STAT_nn.MLT -> SE_STAT_nn_T.MLT; SECnnnnn.MLX -> STCnnnnn.MLX;
//            OPTION / SYSMES.BDA -> _FRN/_GER/_ITA/_SPA; SYSTEM.BDA -> SYSTEM_T.BDA
//   European mode (any European language): the European cutscene scripts (*.EVD), story movies (EVaa_bb_cc.SFD) and
//            English cutscene dubs, because the European subtitles are timed to them (5 scenes have other lines).
// Movie audio: HySfdPlayer::setSoundChannel (hook in scripts/runner_patches.py) asks ps2xLangMovieChannel.

#include "runtime/ps2_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr uint64_t kSector = 2048;
    constexpr uint64_t kCvmIsoBase = 0x1800; // the CVM header; the ISO image starts here (3 sectors)
    constexpr uint64_t kCvmIsoSector = kCvmIsoBase / kSector;

    uint32_t rd32le(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
    void wr32both(uint8_t *p, uint32_t v) // ISO 9660 both-endian 32-bit field
    {
        p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24);
        p[4] = uint8_t(v >> 24), p[5] = uint8_t(v >> 16), p[6] = uint8_t(v >> 8), p[7] = uint8_t(v);
    }

    struct FileRef
    {
        uint64_t offset = 0; // bytes, from the start of its host file
        uint64_t size = 0;
        size_t record = 0;   // where its record sits in the root directory (US image only)
    };

    struct Appended
    {
        uint64_t isoLbn = 0;  // where the game sees it (ISO sector of the virtual image)
        uint64_t sectors = 0;
        uint64_t srcOffset = 0; // in the European disc image
        uint64_t size = 0;
    };

    struct State
    {
        bool tried = false, active = false, euMode = false;
        char voice = 'E', text = 'E';
        fs::path usPath, euPath;
        uint64_t usBytes = 0, usSectors = 0, totalSectors = 0;
        std::map<uint64_t, std::vector<uint8_t>> patched; // CVM sector -> its new 2048 bytes
        std::vector<Appended> appended;                   // by isoLbn
        std::ifstream us, eu;
        std::mutex lock;
    };
    State g;

    bool readAt(std::ifstream &f, uint64_t offset, void *dst, size_t bytes)
    {
        f.clear();
        f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        f.read(static_cast<char *>(dst), static_cast<std::streamsize>(bytes));
        return f.gcount() == static_cast<std::streamsize>(bytes);
    }

    std::string upper(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        return s;
    }

    // The root directory of an ISO 9660 image that starts at byte `base` of `f`: files by upper-case name.
    bool isoRoot(std::ifstream &f, uint64_t base, std::unordered_map<std::string, FileRef> &out, uint32_t &dirLbn,
                 uint32_t &dirSize, std::vector<uint8_t> *dirBytes = nullptr)
    {
        uint8_t pvd[kSector];
        if (!readAt(f, base + 16 * kSector, pvd, kSector) || std::memcmp(pvd + 1, "CD001", 5) != 0)
            return false;
        dirLbn = rd32le(pvd + 156 + 2);
        dirSize = rd32le(pvd + 156 + 10);
        if (dirSize == 0 || dirSize > (8u << 20))
            return false;
        std::vector<uint8_t> dir(dirSize);
        if (!readAt(f, base + uint64_t(dirLbn) * kSector, dir.data(), dir.size()))
            return false;
        for (size_t i = 0; i < dir.size();)
        {
            const uint8_t len = dir[i];
            if (len == 0)
            {
                i = (i / kSector + 1) * kSector; // records never cross a sector: the rest is padding
                continue;
            }
            if (len < 34 || i + len > dir.size())
                break;
            const uint8_t flags = dir[i + 25], nameLen = dir[i + 32];
            if (!(flags & 2) && nameLen > 0 && 33u + nameLen <= len && !(nameLen == 1 && dir[i + 33] <= 1))
            {
                std::string name(reinterpret_cast<const char *>(&dir[i + 33]), nameLen);
                const size_t semi = name.find(';');
                if (semi != std::string::npos)
                    name.resize(semi);
                FileRef ref;
                ref.offset = base + uint64_t(rd32le(&dir[i + 2])) * kSector;
                ref.size = rd32le(&dir[i + 10]);
                ref.record = i;
                out[upper(name)] = ref;
            }
            i += len;
        }
        if (dirBytes)
            *dirBytes = std::move(dir);
        return true;
    }

    char letterFor(const std::string &code)
    {
        static const std::pair<const char *, char> kCodes[] = {{"en", 'E'}, {"ja", 'J'}, {"fr", 'F'}, {"de", 'G'}, {"it", 'I'}, {"es", 'S'}};
        for (const auto &c : kCodes)
            if (code == c.first)
                return c.second;
        return 'E';
    }

    bool european(char l) { return l == 'F' || l == 'G' || l == 'I' || l == 'S'; }

    bool digits(const std::string &s, size_t at, size_t n)
    {
        if (at + n > s.size())
            return false;
        for (size_t i = 0; i < n; ++i)
            if (!std::isdigit(static_cast<unsigned char>(s[at + i])))
                return false;
        return true;
    }

    // "EVaa_bb_cc" at the start of s
    bool eventStem(const std::string &s) { return s.size() >= 10 && s.compare(0, 2, "EV") == 0 && digits(s, 2, 2) && s[4] == '_' && digits(s, 5, 2) && s[7] == '_' && digits(s, 8, 2); }

    // The file name the chosen voice language wants instead of `n` (empty: unchanged).
    std::string voiceName(const std::string &n, char v)
    {
        if (n.size() == 12 && (n[0] == 'E' || n[0] == 'J') && digits(n, 1, 7) && n.compare(8, 4, ".ADX") == 0)
            return std::string(1, v) + n.substr(1);
        if (n.size() == 15 && eventStem(n) && (n[10] == 'E' || n[10] == 'J') && n.compare(11, 4, ".ADX") == 0)
            return n.substr(0, 10) + v + ".ADX";
        return {};
    }

    std::string textName(const std::string &n, char t)
    {
        static const std::unordered_map<char, std::string> kBda = {{'F', "FRN"}, {'G', "GER"}, {'I', "ITA"}, {'S', "SPA"}};
        if (n.size() == 15 && eventStem(n) && (n[10] == 'E' || n[10] == 'J') && n.compare(11, 4, ".SOT") == 0)
            return n.substr(0, 10) + t + ".SOT";
        if (n == "SYSMES_E.SOT" || n == "SYSMES_J.SOT")
            return std::string("SYSMES_") + t + ".SOT";
        // stage messages SEaa_bb_cc.MLT / SEaa_bb_ccJ.MLT
        if ((n.size() == 14 || n.size() == 15) && n.compare(0, 2, "SE") == 0 && digits(n, 2, 2) && n[4] == '_' && digits(n, 5, 2) &&
            n[7] == '_' && digits(n, 8, 2) && n.compare(n.size() - 4, 4, ".MLT") == 0 && (n.size() == 14 || n[10] == 'J'))
        {
            const std::string stem = n.substr(0, 10);
            if (t == 'J')
                return stem + "J.MLT";
            if (t == 'E')
                return stem + ".MLT";
            const std::string set = stem.substr(8, 2) == "03" ? stem.substr(0, 8) + "01" : stem; // US "_03" = EU "_01"
            return set + t + ".MLT";
        }
        if (!european(t))
            return {}; // the rest exists in English only on the US disc
        if (n.size() == 14 && n.compare(0, 8, "SE_STAT_") == 0 && digits(n, 8, 2) && n.compare(10, 4, ".MLT") == 0)
            return n.substr(0, 10) + "_" + t + ".MLT";
        if (n.size() == 12 && n.compare(0, 3, "SEC") == 0 && digits(n, 3, 5) && n.compare(8, 4, ".MLX") == 0)
            return std::string("S") + t + "C" + n.substr(3);
        if (n == "OPTION.BDA" || n == "SYSMES.BDA")
            return n.substr(0, n.size() - 4) + "_" + kBda.at(t) + ".BDA";
        if (n == "SYSTEM.BDA")
            return std::string("SYSTEM_") + t + ".BDA";
        return {};
    }

    // The European disc image: the files of its ROFSSMP.CVM (offsets in the disc image).
    bool euFiles(std::ifstream &f, std::unordered_map<std::string, FileRef> &out)
    {
        std::unordered_map<std::string, FileRef> top;
        uint32_t lbn = 0, size = 0;
        if (!isoRoot(f, 0, top, lbn, size))
            return false;
        if (top.find("SCES_514.28") == top.end())
        {
            std::fprintf(stderr, "[lang] the European disc image is not Shinobi (Europe) (no SCES_514.28)\n");
            return false;
        }
        const auto cvm = top.find("ROFSSMP.CVM");
        if (cvm == top.end())
            return false;
        return isoRoot(f, cvm->second.offset + kCvmIsoBase, out, lbn, size);
    }

    void build(const fs::path &usCvm)
    {
        g.voice = letterFor(ps2x::settings::get("language", "voice"));
        g.text = letterFor(ps2x::settings::get("language", "text"));
        if (g.voice == 'E' && g.text == 'E')
            return; // the game as it is
        g.euMode = european(g.voice) || european(g.text);
        g.usPath = usCvm;
        g.us.open(usCvm, std::ios::binary);
        if (!g.us)
            return;
        std::error_code ec;
        g.usBytes = fs::file_size(usCvm, ec);
        if (ec || g.usBytes < kCvmIsoBase + 32 * kSector || g.usBytes % kSector)
            return;
        g.usSectors = g.usBytes / kSector;

        std::unordered_map<std::string, FileRef> usFiles, euFilesByName;
        uint32_t dirLbn = 0, dirSize = 0;
        std::vector<uint8_t> dir;
        if (!isoRoot(g.us, kCvmIsoBase, usFiles, dirLbn, dirSize, &dir))
        {
            std::fprintf(stderr, "[lang] %s is not a CVM image: languages off\n", reinterpret_cast<const char *>(usCvm.u8string().c_str()));
            return;
        }
        if (g.euMode)
        {
            const std::string disc = ps2x::settings::get("language", "eu_disc");
            const std::u8string u8(disc.begin(), disc.end());
            g.euPath = fs::path(u8);
            if (disc.empty() || !fs::exists(g.euPath, ec))
            {
                std::fprintf(stderr, "[lang] French / German / Italian / Spanish need the European disc image (none set): English kept\n");
                if (european(g.voice))
                    g.voice = 'E';
                if (european(g.text))
                    g.text = 'E';
                g.euMode = false;
                if (g.voice == 'E' && g.text == 'E')
                    return;
            }
            else
            {
                g.eu.open(g.euPath, std::ios::binary);
                if (!g.eu || !euFiles(g.eu, euFilesByName))
                {
                    std::fprintf(stderr, "[lang] the European disc image could not be read: English kept\n");
                    return;
                }
            }
        }

        // What each US file becomes.
        const uint64_t usIsoSectors = g.usSectors - kCvmIsoSector;
        uint64_t nextLbn = usIsoSectors;
        std::unordered_map<uint64_t, uint64_t> appendedAt; // European source offset -> its ISO sector (shared targets)
        int swapped = 0, missing = 0;
        for (const auto &[name, ref] : usFiles)
        {
            std::string target;
            char lang = 'E';
            if (!(target = voiceName(name, g.voice)).empty())
                lang = g.voice;
            else if (!(target = textName(name, g.text)).empty())
                lang = g.text;
            bool fromEu = european(lang);
            if (g.euMode && target.empty())
            {
                // the European scripts / story movies / English cutscene dubs, timed with the European subtitles
                const bool script = name.size() > 4 && name.compare(name.size() - 4, 4, ".EVD") == 0;
                const bool movie = name.size() == 14 && eventStem(name) && name.compare(10, 4, ".SFD") == 0;
                if (script || movie)
                    target = name, fromEu = true;
            }
            else if (g.euMode && lang == 'E' && !target.empty())
            {
                // English in European mode: the European disc's own English cutscene dubs and subtitles / system text,
                // timed to (and numbered like) its cutscene scripts. (Its system messages are the US ones plus five.)
                const bool dub = target.size() == 15 && eventStem(target) && target[10] == 'E' && target.compare(11, 4, ".ADX") == 0;
                const bool sub = target.size() > 4 && target.compare(target.size() - 4, 4, ".SOT") == 0;
                if (dub || sub)
                    fromEu = true;
            }
            if (target.empty() || (target == name && !fromEu))
                continue;
            const auto &table = fromEu ? euFilesByName : usFiles;
            const auto src = table.find(target);
            if (src == table.end())
            {
                ++missing;
                continue;
            }
            uint64_t lbn = 0;
            if (fromEu)
            {
                const auto shared = appendedAt.find(src->second.offset);
                if (shared != appendedAt.end())
                    lbn = shared->second;
                else
                {
                    Appended a;
                    a.isoLbn = nextLbn;
                    a.sectors = (src->second.size + kSector - 1) / kSector;
                    a.srcOffset = src->second.offset;
                    a.size = src->second.size;
                    g.appended.push_back(a);
                    appendedAt[a.srcOffset] = a.isoLbn;
                    lbn = a.isoLbn;
                    nextLbn += std::max<uint64_t>(1, a.sectors);
                }
            }
            else
                lbn = (src->second.offset - kCvmIsoBase) / kSector;
            if (lbn > 0xFFFFFFFFull || src->second.size > 0xFFFFFFFFull)
                continue;
            wr32both(&dir[ref.record + 2], static_cast<uint32_t>(lbn));
            wr32both(&dir[ref.record + 10], static_cast<uint32_t>(src->second.size));
            ++swapped;
        }
        if (swapped == 0)
            return;
        std::sort(g.appended.begin(), g.appended.end(), [](const Appended &a, const Appended &b) { return a.isoLbn < b.isoLbn; });
        const uint64_t isoSectors = nextLbn;
        g.totalSectors = kCvmIsoSector + isoSectors;
        if (g.totalSectors * kSector > 0xFFFFFFFFull)
        {
            std::fprintf(stderr, "[lang] the combined image would pass 4 GB: languages off\n");
            g.appended.clear();
            return;
        }

        // The sectors that change: the root directory, the volume descriptor (size), the CVM ZONE record (size).
        for (uint64_t i = 0; i < (dir.size() + kSector - 1) / kSector; ++i)
        {
            std::vector<uint8_t> s(kSector, 0);
            std::memcpy(s.data(), dir.data() + i * kSector, std::min<size_t>(kSector, dir.size() - i * kSector));
            g.patched[kCvmIsoSector + dirLbn + i] = std::move(s);
        }
        std::vector<uint8_t> pvd(kSector);
        readAt(g.us, kCvmIsoBase + 16 * kSector, pvd.data(), kSector);
        wr32both(&pvd[80], static_cast<uint32_t>(isoSectors)); // volume space size
        g.patched[kCvmIsoSector + 16] = std::move(pvd);
        std::vector<uint8_t> zone(kSector);
        readAt(g.us, 0x800, zone.data(), kSector);
        if (std::memcmp(zone.data(), "ZONE", 4) == 0)
        {
            const uint64_t bytes = isoSectors * kSector; // big-endian 64-bit at 0x830
            for (int i = 0; i < 8; ++i)
                zone[0x30 + i] = static_cast<uint8_t>(bytes >> (56 - 8 * i));
            g.patched[1] = std::move(zone);
        }
        g.active = true;
        const auto name = [](char l) {
            switch (l)
            {
            case 'J': return "Japanese";
            case 'F': return "French";
            case 'G': return "German";
            case 'I': return "Italian";
            case 'S': return "Spanish";
            default: return "English";
            }
        };
        std::fprintf(stderr, "[lang] voices %s, text %s%s: %d files swapped (%d not found), %zu read from the European disc (%.0f MB)\n",
                     name(g.voice), name(g.text), g.euMode ? ", European movies and scripts" : "", swapped, missing,
                     g.appended.size(), double((isoSectors - usIsoSectors) * kSector) / 1048576.0);
    }
}

// The CD layer (Support.h registerCdFile) calls this for ROFSSMP.CVM; true: the file is served by ps2xLangOverlayRead,
// `virtualBytes` long.
bool ps2xLangOverlayInit(const std::filesystem::path &usCvm, uint64_t &virtualBytes)
{
    std::lock_guard<std::mutex> guard(g.lock);
    if (!g.tried)
    {
        g.tried = true;
        build(usCvm);
    }
    if (!g.active)
        return false;
    virtualBytes = g.totalSectors * kSector;
    return true;
}

// Sectors of the virtual ROFSSMP.CVM, `sector` counted from its start. Bytes past the end read as zero.
bool ps2xLangOverlayRead(uint64_t sector, uint64_t sectors, uint8_t *dst, size_t bytes)
{
    if (!dst)
        return false;
    std::lock_guard<std::mutex> guard(g.lock);
    std::memset(dst, 0, bytes);
    const uint64_t first = sector, end = sector + (bytes + kSector - 1) / kSector;
    (void)sectors;
    // 1. the US image part, in one read
    if (first < g.usSectors)
    {
        const uint64_t usEnd = std::min(end, g.usSectors);
        const size_t n = static_cast<size_t>(std::min<uint64_t>((usEnd - first) * kSector, bytes));
        g.us.clear();
        g.us.seekg(static_cast<std::streamoff>(first * kSector), std::ios::beg);
        g.us.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(n));
    }
    // 2. the changed sectors
    for (auto it = g.patched.lower_bound(first); it != g.patched.end() && it->first < end; ++it)
    {
        const size_t at = static_cast<size_t>((it->first - first) * kSector);
        std::memcpy(dst + at, it->second.data(), std::min<size_t>(kSector, bytes - at));
    }
    // 3. the European files after the US image
    if (end > g.usSectors)
    {
        const uint64_t isoFirst = std::max(first, g.usSectors) - kCvmIsoSector, isoEnd = end - kCvmIsoSector;
        auto it = std::upper_bound(g.appended.begin(), g.appended.end(), isoFirst,
                                   [](uint64_t v, const Appended &a) { return v < a.isoLbn; });
        if (it != g.appended.begin())
            --it;
        for (; it != g.appended.end() && it->isoLbn < isoEnd; ++it)
        {
            const uint64_t a = std::max(isoFirst, it->isoLbn), b = std::min(isoEnd, it->isoLbn + it->sectors);
            if (a >= b)
                continue;
            const uint64_t fileOffset = (a - it->isoLbn) * kSector;
            if (fileOffset >= it->size)
                continue;
            const size_t at = static_cast<size_t>((a + kCvmIsoSector - first) * kSector);
            if (at >= bytes)
                break;
            const size_t n = static_cast<size_t>(std::min<uint64_t>({(b - a) * kSector, it->size - fileOffset, bytes - at}));
            readAt(g.eu, it->srcOffset + fileOffset, dst + at, n);
        }
    }
    return true;
}

// HySfdPlayer::setSoundChannel (runner hook): which audio track of a story movie to play. US movies: 0 Japanese,
// 1 English. European movies (European mode): 0 English, 1 French, 2 German, 3 Italian, 4 Spanish.
int ps2xLangMovieChannel(int requested)
{
    if (!g.active || (requested != 0 && requested != 1))
        return requested;
    if (!g.euMode)
        return g.voice == 'J' ? 0 : 1;
    switch (g.voice)
    {
    case 'F': return 1;
    case 'G': return 2;
    case 'I': return 3;
    case 'S': return 4;
    default: return 0; // English (the European movies have no Japanese track)
    }
}
