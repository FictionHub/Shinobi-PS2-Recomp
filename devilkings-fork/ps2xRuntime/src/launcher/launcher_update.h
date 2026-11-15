#pragma once
// LOCAL PATCH: the launcher updates itself from the project's GitHub releases (launcher_update.cpp).
#include <filesystem>
#include <functional>
#include <string>

namespace launcher::update
{
    // Deletes what a previous update left behind (the renamed old launcher).
    void cleanup(const std::filesystem::path &root);
    // In the background: when GitHub has a newer release than version.txt, post("update:<tag>|<zip url>").
    void startCheck(const std::filesystem::path &root, std::function<void(const std::wstring &)> post);
    // In the background: downloads and installs that zip over `root`, reporting update-progress:N, update-error:..., or
    // update-done (then calls restart).
    void install(const std::filesystem::path &root, const std::string &url, std::function<void(const std::wstring &)> post,
                 std::function<void()> restart);

    // LOCAL PATCH (smaller download): the game program for processors without AVX2 (bin/Shinobi-SSE4.exe) is no longer
    // in the release zip. On those PCs it comes from the same GitHub release (Shinobi-SSE4-v<version>.pack), once per
    // version; a Shinobi-SSE4.pack put in bin by hand (offline) is used instead of downloading.
    enum class OldCpuBuild
    {
        Ready,       // bin/Shinobi-SSE4.exe of this version is there
        Downloading, // being fetched now (`percent`)
        Failed,      // the download or unpacking failed (`error`); asking again tries again
    };
    // Starts the fetch when needed (in the background) and says where it is.
    OldCpuBuild oldCpuBuild(const std::filesystem::path &root, int *percent = nullptr, std::string *error = nullptr);
}
