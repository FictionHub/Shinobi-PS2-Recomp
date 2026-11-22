#pragma once

#include <cstdint>
#include <memory>
#include <optional>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopCdvd
    {
    public:
        struct CompletionCallback
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            uint32_t reason = 0u;
        };

        IopCdvd(IopHost &host, IopMemory &memory, IopKernel &kernel);
        ~IopCdvd();

        IopCdvd(const IopCdvd &) = delete;
        IopCdvd &operator=(const IopCdvd &) = delete;

        void reset() noexcept;

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] std::optional<CompletionCallback> takeCompletionCallback() noexcept;

        // LOCAL PATCH (save states, GitHub #16): the driver's state that changes while the game plays (the disc
        // image and its directory are the same in every session).
        struct SavedState
        {
            uint32_t callbackAddress = 0u, callbackGp = 0u;
            bool hasCompletion = false;
            CompletionCallback completion{};
            bool initialized = false;
            uint32_t mediaMode = 0u, currentLsn = 0u, lastError = 0u, streamFlag = 0u, lastReadTimeout = 0u;
            int interruptEventFlagId = 0;
        };
        [[nodiscard]] SavedState saveState() const;
        void loadState(const SavedState &state);

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
