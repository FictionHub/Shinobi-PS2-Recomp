// LOCAL PATCH (GitHub #16): save states. Design and the list of what is kept: shinobi_port/reference/savestates.md.
//
// A request (hotkey, menu, test hook) is carried out by the EE executor at the top of EeScheduler::run(), where no guest
// code is on the host stack, once the machine is at a "clean" point: no interrupt handler or callback half way, no wait
// whose completion cannot be rebuilt, the VIF1 queue drained, no DMA transfer held back and the IOP between batches.
// That is nearly every frame of play; a request that cannot find one within 3 seconds (a movie, a loading screen)
// reports why.
//
// A state is loaded into a game that is already running (the boot-time setup - loaded modules, RPC servers, the disc's
// file table - is the same in every session and is not written).

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "ps2x/iop/iop_subsystem.h"
#include "runtime/ps2_savestate.h"
#include "runtime/ps2_settings.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Kernel/Syscalls/Helpers/State.h"
#include "runtime/gs/rrv_pcsx2_gs_bridge.h"

namespace ps2x::pcsx2gs
{
    bool readVram(uint8_t *dst, uint32_t sizeBytes);
    bool restoreVram(const uint8_t *src, uint32_t sizeBytes);
}
namespace ps2_stubs
{
    void ps2xCdSaveState(ps2x::savestate::Writer &out);
    bool ps2xCdLoadState(ps2x::savestate::Reader &in);
}
void ps2xCriRnaSaveState(ps2x::savestate::Writer &out);
bool ps2xCriRnaLoadState(ps2x::savestate::Reader &in);

namespace
{
    constexpr char kMagic[8] = {'S', 'H', 'N', 'B', 'S', 'T', 'A', 'T'};
    constexpr uint32_t kVersion = 1u;

    std::atomic<int> g_saveSlot{-1};
    std::atomic<int> g_loadSlot{-1};
    std::mutex g_statusMutex;
    std::string g_status;
    std::chrono::steady_clock::time_point g_requestStart{};
    bool g_waiting = false;
    std::string g_lastWhy;

    void setStatus(const std::string &text)
    {
        std::fprintf(stderr, "[savestate] %s\n", text.c_str());
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status = text;
    }

    std::filesystem::path slotPath(int slot)
    {
        return std::filesystem::path("..") / "saves" / "states" / ("slot" + std::to_string(slot) + ".state");
    }

    double secondsSinceStart()
    {
        static const auto s_start = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
    }
}

struct PS2SaveStateAccess
{
    using Writer = ps2x::savestate::Writer;
    using Reader = ps2x::savestate::Reader;

    static bool clean(PS2Runtime &rt, std::string &why)
    {
        if (!rt.m_eeScheduler->saveStateClean(why))
            return false;
        PS2Memory &m = rt.m_memory;
        if (m.vif1Async())
            m.syncVif1();
        if (!m.m_pendingGifTransfers.empty() || !m.m_pendingVif0Transfers.empty() || !m.m_pendingVif1Transfers.empty() ||
            !m.m_path3MaskedFifo.empty())
        {
            why = "a DMA transfer is in progress";
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m.m_completedDmacMutex);
            if (!m.m_completedDmacCauses.empty())
            {
                why = "a DMA interrupt is pending";
                return false;
            }
        }
        if (rt.m_iopSubsystem && !rt.m_iopSubsystem->saveStateClean(why))
            return false;
        return true;
    }

    static void saveMemory(PS2Runtime &rt, Writer &out)
    {
        PS2Memory &m = rt.m_memory;
        out.section("ee-memory");
        out.raw(m.m_rdram, PS2_RAM_SIZE);
        out.raw(m.m_scratchpad, PS2_SCRATCHPAD_SIZE);
        out.u32(static_cast<uint32_t>(m.m_ioRegisters.size()));
        for (const auto &[a, v] : m.m_ioRegisters)
        {
            out.u32(a);
            out.u32(v);
        }
        out.raw(&m.gs_regs, sizeof(m.gs_regs)); // 20 x 64-bit (two are lock-free atomics)
        out.pod(m.vif0_regs);
        out.pod(m.vif1_regs);
        out.pod(m.dma_regs);
        out.podVector(m.m_tlbEntries);
        out.pod(m.m_eeTimers);
        out.pod(m.m_path3Masked);
        out.u32(m.m_vif1PendingPath2ImageQwc);
        out.pod(m.m_vif1PendingPath2DirectHl);
        out.pod(m.m_lastChainBytes);
        out.raw(m.m_vu0Code, PS2_VU0_CODE_SIZE);
        out.raw(m.m_vu0Data, PS2_VU0_DATA_SIZE);
        out.raw(m.m_vu1Code, PS2_VU1_CODE_SIZE);
        out.raw(m.m_vu1Data, PS2_VU1_DATA_SIZE);
        out.pod(rt.m_vu0.state());
        out.pod(rt.m_vu1.state());
    }

    static bool loadMemory(PS2Runtime &rt, Reader &in)
    {
        PS2Memory &m = rt.m_memory;
        if (!in.section("ee-memory"))
            return false;
        in.raw(m.m_rdram, PS2_RAM_SIZE);
        in.raw(m.m_scratchpad, PS2_SCRATCHPAD_SIZE);
        // Updated in place (no clear): other parts may hold pointers to entries.
        const uint32_t n = in.u32();
        for (uint32_t i = 0; i < n && !in.failed(); ++i)
        {
            const uint32_t a = in.u32();
            m.m_ioRegisters[a] = in.u32();
        }
        in.raw(&m.gs_regs, sizeof(m.gs_regs));
        in.pod(m.vif0_regs);
        in.pod(m.vif1_regs);
        in.pod(m.dma_regs);
        in.podVector(m.m_tlbEntries);
        in.pod(m.m_eeTimers);
        in.pod(m.m_path3Masked);
        m.m_vif1PendingPath2ImageQwc = in.u32();
        in.pod(m.m_vif1PendingPath2DirectHl);
        in.pod(m.m_lastChainBytes);
        in.raw(m.m_vu0Code, PS2_VU0_CODE_SIZE);
        in.raw(m.m_vu0Data, PS2_VU0_DATA_SIZE);
        in.raw(m.m_vu1Code, PS2_VU1_CODE_SIZE);
        in.raw(m.m_vu1Data, PS2_VU1_DATA_SIZE);
        in.pod(rt.m_vu0.state());
        in.pod(rt.m_vu1.state());
        // Code caches built from the old microprograms must be rebuilt.
        m.markVU0CodeModified();
        m.markVU1CodeModified();
        return !in.failed();
    }

    static void saveRuntime(PS2Runtime &rt, Writer &out)
    {
        out.section("ee-runtime");
        out.pod(rt.m_cpuContext);
        out.podVector(rt.m_guestHeapBlocks);
        out.u32(rt.m_guestHeapBase);
        out.u32(rt.m_guestHeapEnd);
        out.u32(rt.m_guestHeapLimit);
        out.u32(rt.m_guestHeapSuggestedBase);
        out.pod(rt.m_guestHeapConfigured);
        std::lock_guard<std::mutex> lock(rt.m_eeKernelStateMutex);
        out.u32(static_cast<uint32_t>(rt.m_eeExitHandlers.size()));
        for (const auto &[id, list] : rt.m_eeExitHandlers)
        {
            out.pod(id);
            out.podVector(list);
        }
        out.section("ee-rpc");
        std::lock_guard<std::mutex> rpcLock(g_rpc_mutex);
        out.u32(static_cast<uint32_t>(g_rpc_clients.size()));
        for (const auto &[addr, c] : g_rpc_clients)
        {
            out.u32(addr);
            out.pod(c);
        }
        out.u32(g_rpc_next_id);
        out.u32(g_rpc_packet_index);
        out.u32(g_rpc_server_index);
        out.u32(g_rpc_active_queue);
    }

    static bool loadRuntime(PS2Runtime &rt, Reader &in)
    {
        if (!in.section("ee-runtime"))
            return false;
        in.pod(rt.m_cpuContext);
        in.podVector(rt.m_guestHeapBlocks);
        rt.m_guestHeapBase = in.u32();
        rt.m_guestHeapEnd = in.u32();
        rt.m_guestHeapLimit = in.u32();
        rt.m_guestHeapSuggestedBase = in.u32();
        in.pod(rt.m_guestHeapConfigured);
        {
            std::lock_guard<std::mutex> lock(rt.m_eeKernelStateMutex);
            rt.m_eeExitHandlers.clear();
            const uint32_t n = in.u32();
            for (uint32_t i = 0; i < n && !in.failed(); ++i)
            {
                const int id = in.pod<int>();
                in.podVector(rt.m_eeExitHandlers[id]);
            }
        }
        if (!in.section("ee-rpc"))
            return false;
        std::lock_guard<std::mutex> rpcLock(g_rpc_mutex);
        g_rpc_clients.clear();
        const uint32_t n = in.u32();
        for (uint32_t i = 0; i < n && !in.failed(); ++i)
        {
            const uint32_t addr = in.u32();
            g_rpc_clients[addr] = in.pod<RpcClientState>();
        }
        g_rpc_next_id = in.u32();
        g_rpc_packet_index = in.u32();
        g_rpc_server_index = in.u32();
        g_rpc_active_queue = in.u32();
        return !in.failed();
    }

    static bool save(PS2Runtime &rt, int slot)
    {
        const auto t0 = std::chrono::steady_clock::now();
        Writer out;
        out.raw(kMagic, sizeof(kMagic));
        out.u32(kVersion);
        saveMemory(rt, out);
        rt.m_eeScheduler->saveState(out);
        saveRuntime(rt, out);
        std::vector<uint8_t> iop;
        if (rt.m_iopSubsystem)
            rt.m_iopSubsystem->saveState(iop);
        out.section("iop-blob");
        out.bytes(iop);
        out.section("gs");
        std::vector<uint8_t> vram(RRV_PCSX2_GS_BRIDGE_LOCAL_MEMORY_BYTES);
        const bool haveVram = ps2x::pcsx2gs::readVram(vram.data(), static_cast<uint32_t>(vram.size()));
        out.pod(haveVram);
        if (haveVram)
            out.bytes(vram);
        ps2_stubs::ps2xCdSaveState(out);
        ps2xCriRnaSaveState(out);
        out.section("end");

        const std::filesystem::path path = slotPath(slot);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const std::filesystem::path temp = path.string() + ".tmp";
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char *>(out.data().data()), static_cast<std::streamsize>(out.data().size()));
            if (!file)
            {
                setStatus("Save failed: could not write " + path.string());
                return false;
            }
        }
        std::filesystem::rename(temp, path, ec);
        if (ec)
        {
            setStatus("Save failed: " + ec.message());
            return false;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        char text[160];
        std::snprintf(text, sizeof(text), "Saved state to slot %d (%.1f MB, %.0f ms, frame %llu)%s", slot,
                      out.data().size() / 1048576.0, ms,
                      static_cast<unsigned long long>(rt.m_eeScheduler->currentVSyncTick()),
                      haveVram ? "" : " - graphics memory not saved");
        setStatus(text);
        return true;
    }

    static bool load(PS2Runtime &rt, int slot)
    {
        const auto t0 = std::chrono::steady_clock::now();
        const std::filesystem::path path = slotPath(slot);
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            setStatus("No saved state in slot " + std::to_string(slot));
            return false;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        Reader in(bytes.data(), bytes.size());
        char magic[8] = {};
        in.raw(magic, sizeof(magic));
        if (std::memcmp(magic, kMagic, sizeof(magic)) != 0 || in.u32() != kVersion)
        {
            setStatus("Slot " + std::to_string(slot) + " holds a state from another version");
            return false;
        }
        bool ok = loadMemory(rt, in) && rt.m_eeScheduler->loadState(in) && loadRuntime(rt, in) && in.section("iop-blob");
        std::string error;
        if (ok)
        {
            const std::vector<uint8_t> iop = in.bytes();
            if (rt.m_iopSubsystem && !iop.empty() && !rt.m_iopSubsystem->loadState(iop.data(), iop.size(), error))
                ok = false;
        }
        if (ok && in.section("gs"))
        {
            if (in.pod<bool>())
            {
                const std::vector<uint8_t> vram = in.bytes();
                if (!ps2x::pcsx2gs::restoreVram(vram.data(), static_cast<uint32_t>(vram.size())))
                    std::fprintf(stderr, "[savestate] graphics memory could not be restored\n");
            }
            ok = ps2_stubs::ps2xCdLoadState(in) && ps2xCriRnaLoadState(in) && in.section("end");
        }
        if (!ok)
        {
            setStatus("Load failed (" + (error.empty() ? in.error() : error) + "): restart the game");
            return false;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        char text[128];
        std::snprintf(text, sizeof(text), "Loaded state from slot %d (%.0f ms, frame %llu)", slot, ms,
                      static_cast<unsigned long long>(rt.m_eeScheduler->currentVSyncTick()));
        setStatus(text);
        return true;
    }
};

namespace ps2x::savestate
{
    void requestSave(int slot) { g_saveSlot.store(slot, std::memory_order_release); }
    void requestLoad(int slot) { g_loadSlot.store(slot, std::memory_order_release); }

    namespace
    {
        std::atomic<int> g_currentSlot{1};
    }
    int currentSlot() { return g_currentSlot.load(std::memory_order_relaxed); }
    void setCurrentSlot(int slot)
    {
        if (slot >= 1 && slot <= kSlots)
            g_currentSlot.store(slot, std::memory_order_relaxed);
    }
    std::string slotLabel(int slot)
    {
        std::error_code ec;
        const auto when = std::filesystem::last_write_time(slotPath(slot), ec);
        if (ec)
            return {};
        const std::time_t t = std::chrono::system_clock::to_time_t(
            std::chrono::clock_cast<std::chrono::system_clock>(when));
        std::tm local{};
        localtime_s(&local, &t);
        char text[32];
        std::strftime(text, sizeof(text), "%d %b %H:%M", &local);
        return text[0] == '0' ? std::string(text + 1) : std::string(text);
    }

    std::string takeStatus()
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        std::string s;
        s.swap(g_status);
        return s;
    }

    void servicePending(PS2Runtime &runtime)
    {
        // Test hooks: PS2X_SAVESTATE_AT=<seconds> saves slot 9 then, PS2X_LOADSTATE_AT=<seconds> loads it.
        static const double s_saveAt = std::getenv("PS2X_SAVESTATE_AT") ? std::atof(std::getenv("PS2X_SAVESTATE_AT")) : -1.0;
        static const double s_loadAt = std::getenv("PS2X_LOADSTATE_AT") ? std::atof(std::getenv("PS2X_LOADSTATE_AT")) : -1.0;
        static bool s_savedByHook = false, s_loadedByHook = false;
        if ((s_saveAt >= 0.0 && !s_savedByHook) || (s_loadAt >= 0.0 && !s_loadedByHook))
        {
            const double now = secondsSinceStart();
            if (s_saveAt >= 0.0 && !s_savedByHook && now >= s_saveAt)
            {
                s_savedByHook = true;
                requestSave(9);
            }
            if (s_loadAt >= 0.0 && !s_loadedByHook && now >= s_loadAt)
            {
                s_loadedByHook = true;
                requestLoad(9);
            }
        }

        const int save = g_saveSlot.load(std::memory_order_acquire);
        const int load = g_loadSlot.load(std::memory_order_acquire);
        if (save < 0 && load < 0)
            return;
        if (!g_waiting)
        {
            g_waiting = true;
            g_requestStart = std::chrono::steady_clock::now();
        }
        std::string why;
        if (!PS2SaveStateAccess::clean(runtime, why))
        {
            g_lastWhy = why;
            if (std::chrono::steady_clock::now() - g_requestStart > std::chrono::seconds(3))
            {
                setStatus(std::string(save >= 0 ? "Could not save" : "Could not load") + " now: " + why);
                g_saveSlot.store(-1, std::memory_order_release);
                g_loadSlot.store(-1, std::memory_order_release);
                g_waiting = false;
            }
            return;
        }
        g_waiting = false;
        if (save >= 0)
        {
            g_saveSlot.store(-1, std::memory_order_release);
            PS2SaveStateAccess::save(runtime, save);
        }
        else
        {
            g_loadSlot.store(-1, std::memory_order_release);
            PS2SaveStateAccess::load(runtime, load);
        }
    }
}
