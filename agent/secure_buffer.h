#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <span>
#include <utility>

#include <sodium.h>

#if defined(__linux__)
#include <sys/mman.h>
#endif

// RAII secure memory allocation via libsodium.
// - sodium_malloc provides guard pages, canary, mlock/VirtualLock.
// - MADV_DONTDUMP on Linux keeps the region out of core dumps.
// - sodium_memzero in destructor prevents compiler-elided zeroing.
// - Copy is deleted; move transfers sole ownership.
class SecureBuffer {
    void*  ptr_  = nullptr;
    size_t size_ = 0;

    void advise_no_dump() noexcept {
#if defined(__linux__)
        if (ptr_) madvise(ptr_, size_, MADV_DONTDUMP);
#endif
    }

public:
    explicit SecureBuffer(size_t n) : ptr_(sodium_malloc(n)), size_(n) {
        if (!ptr_) throw std::bad_alloc();
        advise_no_dump();
    }

    SecureBuffer() = default;

    ~SecureBuffer() {
        if (ptr_) {
            sodium_memzero(ptr_, size_);
            sodium_free(ptr_);
        }
    }

    SecureBuffer(const SecureBuffer&)            = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;

    SecureBuffer(SecureBuffer&& o) noexcept
        : ptr_(std::exchange(o.ptr_, nullptr)),
          size_(std::exchange(o.size_, 0)) {}

    SecureBuffer& operator=(SecureBuffer&& o) noexcept {
        if (this != &o) {
            if (ptr_) { sodium_memzero(ptr_, size_); sodium_free(ptr_); }
            ptr_  = std::exchange(o.ptr_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }

    void*       data()  noexcept       { return ptr_; }
    const void* data()  const noexcept { return ptr_; }
    size_t      size()  const noexcept { return size_; }
    bool        empty() const noexcept { return size_ == 0; }

    std::byte*       bytes()       noexcept { return static_cast<std::byte*>(ptr_); }
    const std::byte* bytes() const noexcept { return static_cast<const std::byte*>(ptr_); }

    std::span<std::byte>       span()       noexcept { return {bytes(), size_}; }
    std::span<const std::byte> span() const noexcept { return {bytes(), size_}; }

    // Allocate-copy-zero-free resize: never realloc proper.
    void resize(size_t n) {
        if (n == size_) return;
        void* np = sodium_malloc(n);
        if (!np) throw std::bad_alloc();
        if (ptr_) {
            std::memcpy(np, ptr_, std::min(n, size_));
            sodium_memzero(ptr_, size_);
            sodium_free(ptr_);
        }
        ptr_  = np;
        size_ = n;
        advise_no_dump();
    }

    void zero() noexcept { if (ptr_) sodium_memzero(ptr_, size_); }
};

// Type-system marker preventing accidental logging.
// operator<< is intentionally not defined — formatting a Secret is a compile error.
struct Secret {
    SecureBuffer buf;
    explicit Secret(size_t n) : buf(n) {}
    Secret() = default;
};

template<class C, class T>
std::basic_ostream<C,T>& operator<<(std::basic_ostream<C,T>&, const Secret&) = delete;
