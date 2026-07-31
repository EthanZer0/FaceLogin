#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>

namespace facelogin {

// RAII buffer that securely zeros memory on destruction.
// Uses HeapAlloc(HEAP_ZERO_MEMORY) for allocation and
// SecureZeroMemory before freeing.
class SecureBuffer {
public:
    SecureBuffer() = default;

    explicit SecureBuffer(size_t size) {
        Allocate(size);
    }

    ~SecureBuffer() {
        Free();
    }

    // Non-copyable
    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;

    // Movable
    SecureBuffer(SecureBuffer&& other) noexcept
        : m_data(other.m_data)
        , m_size(other.m_size) {
        other.m_data = nullptr;
        other.m_size = 0;
    }

    SecureBuffer& operator=(SecureBuffer&& other) noexcept {
        if (this != &other) {
            Free();
            m_data = other.m_data;
            m_size = other.m_size;
            other.m_data = nullptr;
            other.m_size = 0;
        }
        return *this;
    }

    bool Allocate(size_t size) {
        Free();
        m_data = static_cast<uint8_t*>(HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, size));
        if (m_data) {
            m_size = size;
            return true;
        }
        return false;
    }

    void Free() {
        if (m_data) {
            SecureZeroMemory(m_data, m_size);
            HeapFree(GetProcessHeap(), 0, m_data);
            m_data = nullptr;
            m_size = 0;
        }
    }

    uint8_t* Data() { return m_data; }
    const uint8_t* Data() const { return m_data; }
    size_t Size() const { return m_size; }
    bool IsValid() const { return m_data != nullptr; }

    // Wipe contents without freeing
    void Wipe() {
        if (m_data && m_size > 0) {
            SecureZeroMemory(m_data, m_size);
        }
    }

private:
    uint8_t* m_data = nullptr;
    size_t m_size = 0;
};

// Stack-based equivalent for small fixed-size sensitive data.
// Zeroes on destruction.
template <size_t N>
class SecureStackBuffer {
public:
    SecureStackBuffer() {
        SecureZeroMemory(m_data, N);
    }

    ~SecureStackBuffer() {
        SecureZeroMemory(m_data, N);
    }

    uint8_t* Data() { return m_data; }
    const uint8_t* Data() const { return m_data; }
    constexpr size_t Size() const { return N; }

private:
    uint8_t m_data[N];
};

} // namespace facelogin
