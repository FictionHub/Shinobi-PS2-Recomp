// LOCAL PATCH: the launcher updates itself from the project's GitHub releases.
//
// At start-up (background thread) it asks GitHub's public API for the latest release of the repository named in
// recomplauncher.json and compares its tag with version.txt next to the launcher. A newer one shows an "Update" button
// in the title bar. Pressing it downloads that release's Windows zip, unpacks it in the temp folder and copies it over
// this folder: the release zip never contains saves, the game's own files, settings or launcher.ini, so those stay as
// they are. The running launcher is renamed out of the way first (Windows allows that), then it restarts itself; the
// old copy is deleted on the next start. bin\Shinobi.zip is new, so the game program is unpacked again on the next Play
// (launcher_webview.cpp unpackGameProgram). Nothing is sent to GitHub but the usual request.

#include "launcher_update.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// launcher_webview.cpp: unzip with bin's zlib1.dll (when tar.exe is missing, e.g. Wine/Proton).
bool launcherUnzipWithZlib(const fs::path &zip, const fs::path &folder, const fs::path &zlibDll);

namespace launcher::update
{
    namespace
    {
        std::wstring widen(const std::string &s)
        {
            if (s.empty())
                return std::wstring();
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
            return out;
        }

        std::string readFile(const fs::path &p)
        {
            std::ifstream in(p, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        // "v0.5.1" / "0.5.1" -> {0, 5, 1}
        std::vector<int> versionOf(std::string text)
        {
            std::vector<int> parts;
            std::string number;
            for (char c : text + ".")
            {
                if (c >= '0' && c <= '9')
                    number += c;
                else if (!number.empty())
                {
                    parts.push_back(std::atoi(number.c_str()));
                    number.clear();
                }
            }
            return parts;
        }

        bool newer(const std::string &remote, const std::string &local)
        {
            std::vector<int> a = versionOf(remote), b = versionOf(local);
            if (a.empty() || b.empty())
                return false;
            const size_t n = std::max(a.size(), b.size());
            a.resize(n, 0);
            b.resize(n, 0);
            return a > b;
        }

        // GET a URL; with `file` the body goes there (progress 0..100 reported), else into `body`. Redirects are followed
        // (GitHub's download links redirect to its file servers).
        bool get(const std::wstring &url, std::string *body, const fs::path *file, const std::function<void(int)> &progress,
                 bool api = false)
        {
            URL_COMPONENTS parts{};
            parts.dwStructSize = sizeof(parts);
            wchar_t host[256] = {}, path[2048] = {};
            parts.lpszHostName = host;
            parts.dwHostNameLength = 256;
            parts.lpszUrlPath = path;
            parts.dwUrlPathLength = 2048;
            if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
                return false;
            HINTERNET session = WinHttpOpen(L"ShinobiLauncher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                            WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session)
                return false;
            HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
            HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                             parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                        : nullptr;
            bool ok = false;
            // The GitHub API wants its own Accept header; a file download gets none (the file server may refuse it).
            if (request &&
                WinHttpSendRequest(request, api ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS,
                                   api ? static_cast<DWORD>(-1) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr))
            {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &size, WINHTTP_NO_HEADER_INDEX);
                wchar_t lengthText[32] = {};
                DWORD lengthSize = sizeof(lengthText);
                unsigned long long total = 0;
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText,
                                        &lengthSize, WINHTTP_NO_HEADER_INDEX))
                    total = _wcstoui64(lengthText, nullptr, 10);
                if (status == 200)
                {
                    std::ofstream out;
                    if (file)
                        out.open(*file, std::ios::binary | std::ios::trunc);
                    ok = !file || out.good();
                    unsigned long long got = 0;
                    int lastPercent = -1;
                    std::vector<char> buffer(1 << 16);
                    for (DWORD read = 0; ok;)
                    {
                        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
                        {
                            ok = false;
                            break;
                        }
                        if (read == 0)
                            break;
                        if (file)
                            out.write(buffer.data(), read);
                        else
                            body->append(buffer.data(), read);
                        got += read;
                        if (total && progress)
                        {
                            const int percent = static_cast<int>(got * 100 / total);
                            if (percent != lastPercent)
                                progress(lastPercent = percent);
                        }
                    }
                }
            }
            if (request)
                WinHttpCloseHandle(request);
            if (connect)
                WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return ok;
        }

        int run(const std::wstring &commandLine)
        {
            std::wstring cmd = commandLine;
            STARTUPINFOW info{};
            info.cb = sizeof(info);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &info, &process))
                return -1;
            WaitForSingleObject(process.hProcess, 600000);
            DWORD code = 1;
            GetExitCodeProcess(process.hProcess, &code);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return static_cast<int>(code);
        }
    }

    void cleanup(const fs::path &root)
    {
        std::error_code ec;
        fs::remove(root / "Shinobi Launcher.exe.old", ec);
    }

    void startCheck(const fs::path &root, std::function<void(const std::wstring &)> post)
    {
        std::thread([root, post]() {
            std::string local = readFile(root / "version.txt");
            local.erase(std::remove_if(local.begin(), local.end(), [](char c) { return c == '\r' || c == '\n' || c == ' '; }),
                        local.end());
            const std::string manifest = readFile(root / "recomplauncher.json");
            std::smatch m;
            if (local.empty() || !std::regex_search(manifest, m, std::regex("\"repo\"\\s*:\\s*\"([^\"]+)\"")))
                return;
            const std::string repo = m[1].str();
            std::string json;
            if (!get(widen("https://api.github.com/repos/" + repo + "/releases/latest"), &json, nullptr, nullptr, true))
                return;
            std::smatch tag, asset;
            if (!std::regex_search(json, tag, std::regex("\"tag_name\"\\s*:\\s*\"([^\"]+)\"")))
                return;
            if (!std::regex_search(json, asset, std::regex("\"browser_download_url\"\\s*:\\s*\"([^\"]+Windows-x64\\.zip)\"")))
                return;
            if (!newer(tag[1].str(), local))
                return;
            std::fprintf(stderr, "[update] %s available (this is %s)\n", tag[1].str().c_str(), local.c_str());
            post(widen("update:" + tag[1].str() + "|" + asset[1].str()));
        }).detach();
    }

    void install(const fs::path &root, const std::string &url, std::function<void(const std::wstring &)> post,
                 std::function<void()> restart)
    {
        std::thread([root, url, post, restart]() {
            wchar_t tempText[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, tempText);
            const fs::path work = fs::path(tempText) / "ShinobiUpdate";
            std::error_code ec;
            fs::remove_all(work, ec);
            fs::create_directories(work / "x", ec);
            const fs::path zip = work / "update.zip";
            if (!get(widen(url), nullptr, &zip, [&](int percent) { post(L"update-progress:" + std::to_wstring(percent)); }))
            {
                post(L"update-error:The download failed. Check your internet connection and try again.");
                return;
            }
            wchar_t system[MAX_PATH] = {};
            GetSystemDirectoryW(system, MAX_PATH);
            // tar.exe first (Windows 10 1803+); without it (Wine/Proton) or if it fails, bin's zlib1.dll.
            const bool untarred = !_wgetenv(L"SHINOBI_LAUNCHER_NO_TAR") &&
                                  run(L"\"" + std::wstring(system) + L"\\tar.exe\" -xf \"" + zip.wstring() + L"\" -C \"" +
                                      (work / "x").wstring() + L"\"") == 0;
            if (!untarred && !launcherUnzipWithZlib(zip, work / "x", root / "bin" / "zlib1.dll"))
            {
                post(L"update-error:The update could not be unpacked.");
                return;
            }
            fs::path source = work / "x" / "Shinobi-PS2-Recomp";
            if (!fs::exists(source / "Shinobi Launcher.exe", ec))
                source = work / "x";
            if (!fs::exists(source / "Shinobi Launcher.exe", ec))
            {
                post(L"update-error:The download does not look like a Shinobi release.");
                return;
            }
            // The running launcher out of the way (Windows allows renaming a running program), then everything copied.
            fs::remove(root / "Shinobi Launcher.exe.old", ec);
            fs::rename(root / "Shinobi Launcher.exe", root / "Shinobi Launcher.exe.old", ec);
            fs::copy(source, root, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                // Put the launcher back so the folder still works.
                std::error_code ec2;
                if (!fs::exists(root / "Shinobi Launcher.exe", ec2))
                    fs::rename(root / "Shinobi Launcher.exe.old", root / "Shinobi Launcher.exe", ec2);
                post(widen("update-error:Some files could not be replaced (" + ec.message() +
                           "). Close the game and try again."));
                return;
            }
            fs::remove_all(work, ec);
            post(L"update-done");
            Sleep(1200);
            restart();
        }).detach();
    }
}
