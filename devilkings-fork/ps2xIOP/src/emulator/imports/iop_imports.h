#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory;

    struct IopImportCall
    {
        std::string library;
        uint16_t ordinal = 0;
        uint16_t version = 0;
    };

    class IopImportRegistry
    {
    public:
        explicit IopImportRegistry(IopMemory &memory) noexcept;

        void reset();
        [[nodiscard]] std::optional<IopImportCall> decode(uint32_t pc) const;
        [[nodiscard]] bool registerExportTable(uint32_t address);
        [[nodiscard]] bool releaseExportTable(uint32_t address);
        [[nodiscard]] uint32_t findTable(std::string_view library, std::optional<uint16_t> version = std::nullopt) const;
        [[nodiscard]] uint32_t resolve(std::string_view library, uint16_t ordinal, std::optional<uint16_t> version = std::nullopt) const;
        [[nodiscard]] int32_t setRebootTimeLibraryHandlingMode(uint32_t address, uint32_t mode);
        void eraseRange(uint32_t base, uint32_t size);

    private:
        struct ExportLibrary
        {
            uint32_t tableAddress = 0;
            uint16_t version = 0;
            std::string name;
            std::vector<uint32_t> functions;
        };

        [[nodiscard]] const ExportLibrary *findLibrary(std::string_view name, std::optional<uint16_t> version) const;

        void clearCaches() const;
        [[nodiscard]] std::optional<IopImportCall> decodeUncached(uint32_t pc, uint32_t delay) const;

        IopMemory &m_memory;
        std::map<uint32_t, ExportLibrary> m_libraries;
        // LOCAL PATCH (performance): every import call decoded its stub (a backward search for the table) and looked the
        // library up by name. Both answers are kept until a library is added or removed.
        mutable std::unordered_map<uint32_t, std::pair<uint32_t, IopImportCall>> m_decodeCache; // stub pc -> (delay word, call)
        mutable std::map<std::pair<std::string, uint32_t>, uint32_t> m_resolveCache;            // (name, ordinal|version) -> target
    };
}
