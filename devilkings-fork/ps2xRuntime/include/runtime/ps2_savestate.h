#pragma once
// LOCAL PATCH (GitHub #16): save states. A small binary writer / reader used by every part of the machine that has state
// changing while the game plays (see shinobi_port/reference/savestates.md). Values are written in host order (a state
// file is only ever loaded by the same build: the header holds a build id).

#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

class PS2Runtime;

namespace ps2x::savestate
{
    class Writer
    {
    public:
        void raw(const void *data, size_t size)
        {
            const auto *p = static_cast<const uint8_t *>(data);
            m_bytes.insert(m_bytes.end(), p, p + size);
        }
        template <typename T>
        void pod(const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "pod() needs a trivially copyable type");
            raw(&value, sizeof(T));
        }
        void u32(uint32_t v) { pod(v); }
        void u64(uint64_t v) { pod(v); }
        void str(const std::string &s)
        {
            u32(static_cast<uint32_t>(s.size()));
            raw(s.data(), s.size());
        }
        void bytes(const std::vector<uint8_t> &v)
        {
            u32(static_cast<uint32_t>(v.size()));
            raw(v.data(), v.size());
        }
        template <typename T>
        void podVector(const std::vector<T> &v)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            u32(static_cast<uint32_t>(v.size()));
            raw(v.data(), v.size() * sizeof(T));
        }
        template <typename T>
        void podDeque(const std::deque<T> &v)
        {
            u32(static_cast<uint32_t>(v.size()));
            for (const T &x : v)
                pod(x);
        }
        // A named section: a reader can check it is reading the part it expects.
        void section(const char *name) { str(name); }
        [[nodiscard]] const std::vector<uint8_t> &data() const { return m_bytes; }

    private:
        std::vector<uint8_t> m_bytes;
    };

    class Reader
    {
    public:
        Reader(const uint8_t *data, size_t size) : m_data(data), m_size(size) {}
        bool raw(void *out, size_t size)
        {
            if (m_failed || size > m_size - m_pos)
            {
                fail("read past the end");
                if (out && size)
                    std::memset(out, 0, size);
                return false;
            }
            if (size)
                std::memcpy(out, m_data + m_pos, size);
            m_pos += size;
            return true;
        }
        template <typename T>
        T pod()
        {
            static_assert(std::is_trivially_copyable_v<T>);
            T v{};
            raw(&v, sizeof(T));
            return v;
        }
        template <typename T>
        void pod(T &out)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            raw(&out, sizeof(T));
        }
        uint32_t u32() { return pod<uint32_t>(); }
        uint64_t u64() { return pod<uint64_t>(); }
        std::string str()
        {
            const uint32_t n = u32();
            if (m_failed || n > m_size - m_pos)
            {
                fail("bad string");
                return {};
            }
            std::string s(reinterpret_cast<const char *>(m_data + m_pos), n);
            m_pos += n;
            return s;
        }
        std::vector<uint8_t> bytes()
        {
            const uint32_t n = u32();
            std::vector<uint8_t> v;
            if (m_failed || n > m_size - m_pos)
            {
                fail("bad byte block");
                return v;
            }
            v.assign(m_data + m_pos, m_data + m_pos + n);
            m_pos += n;
            return v;
        }
        template <typename T>
        void podVector(std::vector<T> &v)
        {
            const uint32_t n = u32();
            if (m_failed || static_cast<uint64_t>(n) * sizeof(T) > m_size - m_pos)
            {
                fail("bad vector");
                v.clear();
                return;
            }
            v.resize(n);
            raw(v.data(), n * sizeof(T));
        }
        template <typename T>
        void podDeque(std::deque<T> &v)
        {
            const uint32_t n = u32();
            v.clear();
            for (uint32_t i = 0; i < n && !m_failed; ++i)
                v.push_back(pod<T>());
        }
        bool section(const char *name)
        {
            const std::string got = str();
            if (!m_failed && got != name)
                fail(std::string("expected section ") + name + ", found " + got);
            return !m_failed;
        }
        void fail(const std::string &why)
        {
            if (!m_failed)
                m_error = why;
            m_failed = true;
        }
        [[nodiscard]] bool failed() const { return m_failed; }
        [[nodiscard]] const std::string &error() const { return m_error; }

    private:
        const uint8_t *m_data;
        size_t m_size;
        size_t m_pos = 0;
        bool m_failed = false;
        std::string m_error;
    };

    // Requests from the window / hotkeys / menu (any thread); the EE executor carries them out at its next clean point.
    void requestSave(int slot);
    void requestLoad(int slot);
    // Called by EeScheduler::run() at the top of every loop iteration (cheap when nothing is requested).
    void servicePending(PS2Runtime &runtime);
    // The last result, for the on-screen message: "Saved to slot 1", "Load failed: ...". Empty when none is new.
    std::string takeStatus();
    // Three slots for players (Ctrl+1/2/3, the pause menu); Ctrl+F1 / F2 and the menu buttons use the current one.
    constexpr int kSlots = 3;
    int currentSlot();
    void setCurrentSlot(int slot); // 1..kSlots, anything else is ignored
    // When the slot was last saved ("22 Nov 13:20"), or "" when it holds nothing.
    std::string slotLabel(int slot);
}
