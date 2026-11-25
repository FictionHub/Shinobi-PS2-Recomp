// LOCAL PATCH: Languages. The US game already has a language system (GameSystem::SetLanguage / SetVoice /
// IsSubtitle, English + Japanese, files picked by letter: E1000050.ADX / J1000050.ADX, EV01_00_00E.SOT /
// EV01_00_00J.SOT, ...). The European release is the same game with four more letters (F, G, I, S) on the same file
// names, plus movies that carry five audio tracks (0 English, 1 French, 2 Italian, 3 German, 4 Spanish).
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
//            SYSMES.BDA -> _FRN/_GER/_ITA/_SPA (OPTION.BDA: US kept, v0.7.3); SYSTEM.BDA -> SYSTEM_T.BDA
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
#include <unordered_set>
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
        // LOCAL PATCH (v0.7.2, player report "Italian: the movies still play in English"): the European story movies
        // swapped in, by name, with the audio tracks they carry (5: English, French, Italian, German, Spanish; the
        // ending EV11_00_00 has 1).
        std::unordered_map<std::string, int> euMovieTracks;
        int movieTracks = 0; // the movie being set up now: its tracks when it is a European one, else 0
    };
    State g;

    // The audio tracks of an MPEG movie (Sofdec .SFD) that starts at `offset` of `f`: audio streams 0xC0, 0xC1, ...
    // seen in its first 256 KB (each track has a packet there every few frames).
    int movieAudioTracks(std::ifstream &f, uint64_t offset, uint64_t size)
    {
        std::vector<uint8_t> b(static_cast<size_t>(std::min<uint64_t>(size, 256 * 1024)));
        f.clear();
        f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(b.size()));
        b.resize(static_cast<size_t>(std::max<std::streamsize>(0, f.gcount())));
        int count[5] = {};
        for (size_t i = 0; i + 4 <= b.size(); ++i)
            if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] >= 0xC0 && b[i + 3] <= 0xC4)
                ++count[b[i + 3] - 0xC0];
        int tracks = 0;
        while (tracks < 5 && count[tracks] >= 4)
            ++tracks;
        return std::max(tracks, 1);
    }

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
        // LOCAL PATCH (v0.7.3, options screen broken in Italian): the European OPTION*.BDA belong to the European options
        // menu (subtitle and voice language lists): drawn by the US menu code its language-list sheet covered the values.
        // The US options menu keeps its own pictures (English labels); everything else follows the text language.
        if (n == "SYSMES.BDA")
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
        int swapped = 0, missing = 0, inPlace = 0;
        constexpr uint64_t kPending = ~0ull;
        std::vector<std::pair<size_t, uint64_t>> pendingRecords; // directory record -> European source offset
        // What a US file's record should point at: (target name, from the European disc).
        const auto decide = [&](const std::string &name) -> std::pair<std::string, bool>
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
                // (PS2X_LANG_US_MOVIES=1 keeps the US story movies: test hook)
                static const bool s_usMovies = std::getenv("PS2X_LANG_US_MOVIES") != nullptr;
                const bool movie = !s_usMovies && name.size() == 14 && eventStem(name) && name.compare(10, 4, ".SFD") == 0;
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
            return {target, fromEu};
        };
        // US files another record still reads (a Japanese voice in place of an English one, ...): never overwritten.
        std::unordered_set<std::string> usStillRead;
        for (const auto &[name, ref] : usFiles)
        {
            const auto [target, fromEu] = decide(name);
            if (!target.empty() && !fromEu && target != name)
                usStillRead.insert(target);
        }
        for (const auto &[name, ref] : usFiles)
        {
            const auto [target, fromEu] = decide(name);
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
                    a.sectors = (src->second.size + kSector - 1) / kSector;
                    a.srcOffset = src->second.offset;
                    a.size = src->second.size;
                    // LOCAL PATCH (RecompLauncher report: "with Italian the game will not start"): the game's file
                    // system (CRI) cannot reach past 2 GB of ROFSSMP.CVM. With the European story movies appended the
                    // archive reached ~2.2 GB: the opening movie never played (a black screen at boot) and the system
                    // messages waited ~50 s. A European file now goes into the sectors of the US file it replaces (the
                    // movies, scripts, voices, text) when it fits and no other record still reads that US file, so
                    // little is appended.
                    const uint64_t usSectorsOfName = (ref.size + kSector - 1) / kSector;
                    if (a.sectors <= usSectorsOfName && ref.offset >= kCvmIsoBase && !usStillRead.count(name))
                    {
                        a.isoLbn = (ref.offset - kCvmIsoBase) / kSector;
                        ++inPlace;
                    }
                    else
                        a.isoLbn = kPending; // placed after the loop, smallest first
                    g.appended.push_back(a);
                    appendedAt[a.srcOffset] = a.isoLbn;
                    lbn = a.isoLbn;
                }
            }
            else
                lbn = (src->second.offset - kCvmIsoBase) / kSector;
            if (src->second.size > 0xFFFFFFFFull)
                continue;
            if (lbn == kPending)
                pendingRecords.push_back({ref.record, src->second.offset});
            else if (lbn <= 0xFFFFFFFFull)
                wr32both(&dir[ref.record + 2], static_cast<uint32_t>(lbn));
            else
                continue;
            wr32both(&dir[ref.record + 10], static_cast<uint32_t>(src->second.size));
            ++swapped;
            if (fromEu && name.size() > 4 && name.compare(name.size() - 4, 4, ".SFD") == 0)
                g.euMovieTracks[name] = movieAudioTracks(g.eu, src->second.offset, src->second.size);
        }
        // The European files that did not fit in place go after the US image, smallest first: should a language ever
        // pass the 2 GB the game's file system reaches, only the biggest (movies) are past it, never text or voices.
        {
            std::vector<size_t> order;
            for (size_t i = 0; i < g.appended.size(); ++i)
                if (g.appended[i].isoLbn == kPending)
                    order.push_back(i);
            std::sort(order.begin(), order.end(), [](size_t a, size_t b) { return g.appended[a].size < g.appended[b].size; });
            for (const size_t i : order)
            {
                g.appended[i].isoLbn = nextLbn;
                appendedAt[g.appended[i].srcOffset] = nextLbn;
                nextLbn += std::max<uint64_t>(1, g.appended[i].sectors);
            }
            for (const auto &[record, srcOffset] : pendingRecords)
                wr32both(&dir[record + 2], static_cast<uint32_t>(appendedAt[srcOffset]));
        }
        if (swapped == 0)
            return;
        std::sort(g.appended.begin(), g.appended.end(), [](const Appended &a, const Appended &b) { return a.isoLbn < b.isoLbn; });
        const uint64_t isoSectors = nextLbn;
        g.totalSectors = kCvmIsoSector + isoSectors;
        if (g.totalSectors * kSector > 0x7FFFFFFFull)
            std::fprintf(stderr, "[lang] warning: the combined archive is %.0f MB, past the 2 GB the game's file system reaches;"
                                 " files beyond it will not load\n", double(g.totalSectors * kSector) / 1048576.0);
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
        // LOCAL PATCH (Italian etc. took ~50 s to start): the CVM header's own total size (big-endian 64-bit at 0x1C) is
        // checked by the game's file system (CRI GDFS) too; left at the US size, the first load of an appended European
        // file (the system messages, SYSMES) was refused and only retried after a 50-second timeout.
        std::vector<uint8_t> head(kSector);
        readAt(g.us, 0, head.data(), kSector);
        if (std::memcmp(head.data(), "CVMH", 4) == 0)
        {
            const uint64_t total = g.totalSectors * kSector;
            for (int i = 0; i < 8; ++i)
                head[0x1C + i] = static_cast<uint8_t>(total >> (56 - 8 * i));
            g.patched[0] = std::move(head);
        }
        std::vector<uint8_t> zone(kSector);
        readAt(g.us, 0x800, zone.data(), kSector);
        if (std::memcmp(zone.data(), "ZONE", 4) == 0)
        {
            const uint64_t bytes = isoSectors * kSector; // big-endian 64-bit at 0x830
            for (int i = 0; i < 8; ++i)
                zone[0x30 + i] = static_cast<uint8_t>(bytes >> (56 - 8 * i));
            // and +0x08 (big-endian 32-bit): the whole file's size less 0x80C (0x4D5007F4 on the US disc)
            const uint64_t rest = g.totalSectors * kSector - 0x80Cu;
            if (rest <= 0xFFFFFFFFull)
                for (int i = 0; i < 4; ++i)
                    zone[0x08 + i] = static_cast<uint8_t>(rest >> (24 - 8 * i));
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
        std::fprintf(stderr, "[lang] voices %s, text %s%s: %d files swapped (%d not found), %zu read from the European disc (%d in place of the US file, %.0f MB appended)\n",
                     name(g.voice), name(g.text), g.euMode ? ", European movies and scripts" : "", swapped, missing,
                     g.appended.size(), inPlace, double((isoSectors - usIsoSectors) * kSector) / 1048576.0);
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
    // 3. the European files: after the US image, or (LOCAL PATCH, 2 GB limit) in the sectors of the US file they replace
    if (end > kCvmIsoSector && !g.appended.empty())
    {
        const uint64_t isoFirst = first > kCvmIsoSector ? first - kCvmIsoSector : 0, isoEnd = end - kCvmIsoSector;
        auto it = std::upper_bound(g.appended.begin(), g.appended.end(), isoFirst,
                                   [](uint64_t v, const Appended &a) { return v < a.isoLbn; });
        if (it != g.appended.begin())
            --it;
        for (; it != g.appended.end() && it->isoLbn < isoEnd; ++it)
        {
            const uint64_t a = std::max(isoFirst, it->isoLbn), b = std::min(isoEnd, it->isoLbn + it->sectors);
            if (a >= b)
                continue;
            const size_t at = static_cast<size_t>((a + kCvmIsoSector - first) * kSector);
            if (at >= bytes)
                break;
            const size_t span = static_cast<size_t>(std::min<uint64_t>((b - a) * kSector, bytes - at));
            std::memset(dst + at, 0, span); // (in place: the rest of the US file's last sector is not shown)
            const uint64_t fileOffset = (a - it->isoLbn) * kSector;
            if (fileOffset >= it->size)
                continue;
            const size_t n = static_cast<size_t>(std::min<uint64_t>(span, it->size - fileOffset));
            readAt(g.eu, it->srcOffset + fileOffset, dst + at, n);
        }
    }
    return true;
}

// LOCAL PATCH (v0.7.3, "subtitles never show in the movies"): every line of a .SOT text file is tagged with its language
// letter (E, J, F, G, I, S), and StringsDat::GetString(letter, id) only returns a line with the letter asked for. The US
// game asks for E or J, so the European files swapped in (tagged F / G / I / S) gave no text at all: no subtitles. With a
// European text language, a request for E or J is answered from that language (runner hook, runner_patches.py 13d).
int ps2xLangSotLetter(int letter)
{
    if (g.active && european(g.text) && (letter == 'E' || letter == 'J'))
        return g.text;
    return letter;
}

// HySfdPlayer::setSoundChannel (runner hook): which audio track of a story movie to play. US movies: 0 Japanese,
// 1 English. European movies (European mode): 0 English, 1 French, 2 Italian, 3 German, 4 Spanish.
int ps2xLangMovieChannel(int requested)
{
    // (a European movie's track was chosen in movieTask::actSetup: ps2xLangMovieTrack)
    if (!g.active || g.movieTracks || (requested != 0 && requested != 1))
        return requested;
    // (PS2X_LANG_US_MOVIES keeps the US movies in European mode: then their two tracks, as above)
    if (!g.euMode || std::getenv("PS2X_LANG_US_MOVIES"))
        return g.voice == 'J' ? 0 : 1;
    switch (g.voice)
    {
    case 'F': return 1;
    case 'I': return 2;
    case 'G': return 3;
    case 'S': return 4;
    default: return 0; // English (the European movies have no Japanese track)
    }
}

// LOCAL PATCH (v0.7.2, player report "Italian: the movies still play in English"). movieTask::actSetup (runner hook,
// scripts/runner_patches.py section 13b) asks here, with the movie's file name, which audio track to play, just before
// it calls HySfdPlayer::setSoundChannel only when that track is not 0. Two faults: the tracks were taken as En, Fr, De,
// It, Es, but the European game's order is En, Fr, It, De, Es (Italian played German), and the game asks for track 1
// only when its own voice setting is English and the scene says so (a scene asking for 0 played track 0, English). A
// European story movie now always gets the chosen language's track (0 English: the call is skipped and track 0 plays);
// every other movie keeps the game's choice (ps2xLangMovieChannel). Checked against a recording of the game's audio
// (PS2X_AUDIO_DUMP): it matches the movie's Italian track sample for sample.
int ps2xLangMovieTrack(const char *path, int requested)
{
    g.movieTracks = 0;
    if (!g.active || !g.euMode || !path)
        return requested;
    std::string name(path);
    const size_t slash = name.find_last_of("\/:");
    if (slash != std::string::npos)
        name.erase(0, slash + 1);
    const size_t semi = name.find(';');
    if (semi != std::string::npos)
        name.erase(semi);
    const auto it = g.euMovieTracks.find(upper(name));
    if (it == g.euMovieTracks.end())
        return requested;
    g.movieTracks = it->second;
    int track = 0;
    switch (g.voice)
    {
    case 'F': track = 1; break;
    case 'I': track = 2; break; // (the European game's own order: its language table reads ENG, FRN, ITL, GER, SPA)
    case 'G': track = 3; break;
    case 'S': track = 4; break;
    default: break;
    }
    if (track >= it->second)
        track = 0; // (the ending has its one track only)
    static int s_logged = 0;
    if (s_logged < 40)
    {
        ++s_logged;
        std::fprintf(stderr, "[lang] movie %s: audio track %d of %d (the game asked for %d)\n", name.c_str(), track,
                     it->second, requested);
    }
    return track;
}

// LOCAL PATCH (v0.7.3, "subtitles need to be on by default"): GameSystem::IsSubtitle (runner hook, runner_patches.py
// section 13c) reads the game's option word (+0x270, bit 31 = subtitles). With [language] subtitles on (the default)
// that bit reads as set, so story movies and cutscenes show subtitles from the start in every language; off, the game's
// own Subtitles option decides.
bool ps2xSubtitlesForced()
{
    static const bool s_on = ps2x::settings::getBool("language", "subtitles");
    return s_on;
}
