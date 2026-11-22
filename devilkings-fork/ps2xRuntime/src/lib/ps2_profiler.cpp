#include "runtime/ps2_profiler.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace
{
    struct ProfileRequest
    {
        double delaySeconds = 0.0;
        double durationSeconds = 0.0;
        std::string path;
        bool enabled = false;
    };

    ProfileRequest parseProfileRequest()
    {
        ProfileRequest request;
        const char *value = std::getenv("PS2X_PROFILE");
        if (!value || !*value)
            return request;
        char *end = nullptr;
        request.delaySeconds = std::strtod(value, &end);
        if (!end || *end != ':')
            return request;
        request.durationSeconds = std::strtod(end + 1, &end);
        if (!end || *end != ':' || request.durationSeconds <= 0.0)
            return request;
        request.path = end + 1;
        request.enabled = !request.path.empty();
        return request;
    }

    struct SampledThread
    {
        HANDLE handle = nullptr;
        uint64_t samples = 0;
        std::unordered_map<DWORD64, uint32_t> ripCounts;
        std::vector<std::array<DWORD64, 6>> stacks; // LOCAL PATCH: PS2X_PROFILE_STACKS=1, [0] = rip
    };

    bool stacksWanted()
    {
        static const bool s_on = std::getenv("PS2X_PROFILE_STACKS") != nullptr;
        return s_on;
    }

    // Up to 6 frames of a suspended thread (x64 unwind data; no allocation).
    void unwindFramesUnsafe(CONTEXT &context, DWORD64 *frames)
    {
        for (size_t i = 0; i < 6u && context.Rip >= 0x10000u && context.Rsp >= 0x10000u; ++i)
        {
            frames[i] = context.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (!function)
            {
                context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            void *handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
        }
    }

    // A frame walk can run off the stack (a frame with no unwind data): stop there instead of crashing.
    void unwindFrames(CONTEXT context, std::array<DWORD64, 6> &frames)
    {
        frames.fill(0);
        __try
        {
            unwindFramesUnsafe(context, frames.data());
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // Opens every other thread of this process that is not tracked yet. Toolhelp snapshots
    // are slow, so this runs only every few hundred samples.
    void refreshThreads(DWORD selfId, std::map<DWORD, SampledThread> &threads)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        const DWORD pid = GetCurrentProcessId();
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == selfId ||
                threads.count(entry.th32ThreadID) != 0u)
                continue;
            HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                       FALSE, entry.th32ThreadID);
            if (handle)
                threads[entry.th32ThreadID].handle = handle;
        }
        CloseHandle(snapshot);
    }

    std::string describeAddress(HANDLE process, DWORD64 address)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol))
            return std::string(symbol->Name, symbol->NameLen);

        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        if (SymGetModuleInfo64(process, address, &module))
            return std::string(module.ModuleName) + "!?";
        return "?";
    }

    // file:line of an instruction (the innermost inlined source line), or "?".
    std::string describeLine(HANDLE process, DWORD64 address)
    {
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD displacement = 0;
        if (!SymGetLineFromAddr64(process, address, &displacement, &line) || !line.FileName)
            return "?";
        std::string file(line.FileName);
        const size_t slash = file.find_last_of("/\\");
        if (slash != std::string::npos)
            file.erase(0, slash + 1u);
        return file + ":" + std::to_string(line.LineNumber);
    }

    // Source lines listed per busy function: PS2X_PROFILE_LINES=<n> (default 12).
    size_t linesShown()
    {
        static const size_t s_lines = []() -> size_t
        {
            const char *value = std::getenv("PS2X_PROFILE_LINES");
            const long parsed = value ? std::strtol(value, nullptr, 10) : 0L;
            return parsed > 0 ? static_cast<size_t>(parsed) : 12u;
        }();
        return s_lines;
    }

    bool isWaitFunction(const std::string &name)
    {
        return name.rfind("Zw", 0) == 0 || name.rfind("Nt", 0) == 0 || name.rfind("Rtl", 0) == 0 || name == "?";
    }

    void writeReport(const ProfileRequest &request, std::map<DWORD, SampledThread> &threads, uint64_t rounds,
                     double seconds)
    {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        // LOCAL PATCH: the system-stats code may have initialised DbgHelp already (then SymInitialize fails but the symbols
        // are loaded); only a session started here is cleaned up afterwards.
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string exeDir = exePath;
        if (const size_t slash = exeDir.find_last_of("\\/"); slash != std::string::npos)
            exeDir.resize(slash);
        const bool ownSymbols = SymInitialize(process, exeDir.c_str(), TRUE) != FALSE;
        SymRefreshModuleList(process);
        const bool symbols = true;

        std::unordered_map<DWORD64, std::string> names;
        auto nameOf = [&](DWORD64 rip) -> const std::string &
        {
            auto it = names.find(rip);
            if (it == names.end())
                it = names.emplace(rip, symbols ? describeAddress(process, rip) : std::string("?")).first;
            return it->second;
        };

        std::vector<std::pair<DWORD, SampledThread *>> ordered;
        for (auto &[id, thread] : threads)
        {
            if (thread.samples != 0u)
                ordered.emplace_back(id, &thread);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto &a, const auto &b) { return a.second->samples > b.second->samples; });

        FILE *out = std::fopen(request.path.c_str(), "w");
        if (!out)
        {
            std::fprintf(stderr, "[profile] cannot write %s\n", request.path.c_str());
            if (ownSymbols)
                SymCleanup(process);
            return;
        }

        std::fprintf(out, "sampling rounds=%llu seconds=%.1f threads=%zu\n",
                     static_cast<unsigned long long>(rounds), seconds, ordered.size());
        for (const auto &[id, thread] : ordered)
        {
            std::unordered_map<std::string, uint64_t> byFunction;
            for (const auto &[rip, count] : thread->ripCounts)
                byFunction[nameOf(rip)] += count;
            std::vector<std::pair<std::string, uint64_t>> functions(byFunction.begin(), byFunction.end());
            std::sort(functions.begin(), functions.end(),
                      [](const auto &a, const auto &b) { return a.second > b.second; });

            std::fprintf(out, "\nthread %lu: %llu samples\n", static_cast<unsigned long>(id),
                         static_cast<unsigned long long>(thread->samples));
            const size_t shown = std::min<size_t>(functions.size(), 40u);
            for (size_t i = 0; i < shown; ++i)
            {
                std::fprintf(out, "  %6.2f%%  %7llu  %s\n",
                             100.0 * static_cast<double>(functions[i].second) / static_cast<double>(thread->samples),
                             static_cast<unsigned long long>(functions[i].second), functions[i].first.c_str());
            }

            // Source lines of the busiest functions (not counting waits).
            if (!symbols)
                continue;
            size_t described = 0;
            for (size_t i = 0; i < functions.size() && described < 10u; ++i)
            {
                if (isWaitFunction(functions[i].first) ||
                    functions[i].second * 200u < thread->samples) // under 0.5%
                    continue;
                ++described;
                std::unordered_map<std::string, uint64_t> byLine;
                for (const auto &[rip, count] : thread->ripCounts)
                {
                    if (nameOf(rip) == functions[i].first)
                        byLine[describeLine(process, rip)] += count;
                }
                std::vector<std::pair<std::string, uint64_t>> lines(byLine.begin(), byLine.end());
                std::sort(lines.begin(), lines.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                if (!thread->stacks.empty())
                {
                    std::unordered_map<std::string, uint64_t> byCaller;
                    for (const auto &frames : thread->stacks)
                        if (nameOf(frames[0]) == functions[i].first)
                        {
                            std::string chain;
                            for (size_t k = 1; k < 5 && frames[k]; ++k)
                                chain += (k > 1 ? " < " : "") + nameOf(frames[k]);
                            byCaller[chain] += 1;
                        }
                    std::vector<std::pair<std::string, uint64_t>> callers(byCaller.begin(), byCaller.end());
                    std::sort(callers.begin(), callers.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                    std::fprintf(out, "  callers of %s:\n", functions[i].first.c_str());
                    for (size_t j = 0; j < callers.size() && j < 6u; ++j)
                        std::fprintf(out, "      %6llu  %s\n", static_cast<unsigned long long>(callers[j].second), callers[j].first.c_str());
                }
                std::fprintf(out, "  lines of %s:\n", functions[i].first.c_str());
                for (size_t j = 0; j < lines.size() && j < linesShown(); ++j)
                {
                    std::fprintf(out, "      %6.2f%%  %s\n",
                                 100.0 * static_cast<double>(lines[j].second) / static_cast<double>(functions[i].second),
                                 lines[j].first.c_str());
                }
            }
        }
        std::fclose(out);
        if (ownSymbols)
            SymCleanup(process);
        std::fprintf(stderr, "[profile] wrote %s\n", request.path.c_str());
    }

    void profileThreadMain(ProfileRequest request)
    {
        std::this_thread::sleep_for(std::chrono::duration<double>(request.delaySeconds));
        std::fprintf(stderr, "[profile] sampling for %.1f s\n", request.durationSeconds);

        const DWORD selfId = GetCurrentThreadId();
        std::map<DWORD, SampledThread> threads;
        refreshThreads(selfId, threads);

        timeBeginPeriod(1);
        const auto start = std::chrono::steady_clock::now();
        const auto stop = start + std::chrono::duration<double>(request.durationSeconds);
        uint64_t rounds = 0;
        while (std::chrono::steady_clock::now() < stop)
        {
            if ((rounds % 256u) == 255u)
                refreshThreads(selfId, threads);
            for (auto &[id, thread] : threads)
            {
                if (SuspendThread(thread.handle) == static_cast<DWORD>(-1))
                    continue;
                CONTEXT context{};
                context.ContextFlags = stacksWanted() ? CONTEXT_FULL : CONTEXT_CONTROL;
                const BOOL ok = GetThreadContext(thread.handle, &context);
                std::array<DWORD64, 6> frames{};
                if (ok && stacksWanted())
                    unwindFrames(context, frames);
                ResumeThread(thread.handle);
                if (ok && stacksWanted())
                    thread.stacks.push_back(frames);
                // Record only after resuming: the sampled thread may hold the heap lock.
                if (ok)
                {
                    ++thread.samples;
                    ++thread.ripCounts[context.Rip];
                }
            }
            ++rounds;
            Sleep(1);
        }
        timeEndPeriod(1);

        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        writeReport(request, threads, rounds, seconds);
        for (auto &[id, thread] : threads)
            CloseHandle(thread.handle);
    }
}

void ps2StartSamplingProfilerFromEnv()
{
    const ProfileRequest request = parseProfileRequest();
    if (!request.enabled)
        return;
    std::thread(profileThreadMain, request).detach();
}

#else

void ps2StartSamplingProfilerFromEnv()
{
}

#endif
