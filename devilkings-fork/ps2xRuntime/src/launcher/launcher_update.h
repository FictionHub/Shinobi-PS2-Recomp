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
}
