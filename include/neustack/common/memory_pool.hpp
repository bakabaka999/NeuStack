#ifndef NEUSTACK_COMMON_MEMORY_POOL_HPP
#define NEUSTACK_COMMON_MEMORY_POOL_HPP

#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>

namespace neustack {

/**
 * @brief A generic, fixed-size object pool with zero dynamic allocation.
 *
 * Characteristics:
 * - Compile-time fixed size (N).
 * - Intrusive free-list (zero memory overhead per slot).
 * - Placement new construction / Explicit destruction.
 * - NOT thread-safe (designed for single-threaded data plane or external locking).
 *
 * @tparam T The type of object to pool.
 * @tparam N The capacity of the pool.
 */
template <typename T, size_t N>
class FixedPool {
    static_assert(N > 0, "FixedPool capacity must be greater than 0");

    // Intrusive list node overlay
    struct FreeNode {
        FreeNode* next;
    };

    // Each slot must be large enough to hold both T and FreeNode (for the free list)
    static constexpr size_t SLOT_SIZE = sizeof(T) > sizeof(FreeNode) ? sizeof(T) : sizeof(FreeNode);
    static constexpr size_t SLOT_ALIGN = alignof(T) > alignof(FreeNode) ? alignof(T) : alignof(FreeNode);

public:
    // Custom deleter for RAII support
    struct Deleter {
        FixedPool<T, N>* pool;
        void operator()(T* ptr) const {
            if (pool && ptr) {
                pool->release(ptr);
            }
        }
    };

    // Smart pointer type alias
    using Ptr = std::unique_ptr<T, Deleter>;

    FixedPool() : _free_head(nullptr), _in_use_count(0) {
        reset();
    }

    // Disable copy/move to prevent pointer invalidation issues
    FixedPool(const FixedPool&) = delete;
    FixedPool& operator=(const FixedPool&) = delete;
    FixedPool(FixedPool&&) = delete;
    FixedPool& operator=(FixedPool&&) = delete;

    ~FixedPool() {
        // Warning: If objects are still in use, they are leaked (destructors not called)
        // logic assumes the owner manages lifecycle correctly or uses smart pointers.
        assert(_in_use_count == 0 && "FixedPool destroyed with objects still in use!");
    }

    /**
     * @brief Acquire an object from the pool.
     * Constructs the object using placement new with provided arguments.
     *
     * @param args Constructor arguments for T.
     * @return T* Pointer to the constructed object, or nullptr if pool is full.
     */
    template <typename... Args>
    T* acquire(Args&&... args) {
        if (_free_head == nullptr) {
            return nullptr;
        }

        // Pop from free list
        FreeNode* node = _free_head;
        _free_head = node->next;

        void* mem = static_cast<void*>(node);
        
        // Construct object
        T* obj = new (mem) T(std::forward<Args>(args)...);
        
        _in_use_count++;
        return obj;
    }

    /**
     * @brief Acquire an object wrapped in a RAII smart pointer.
     *
     * @param args Constructor arguments for T.
     * @return Ptr unique_ptr with custom deleter, or empty unique_ptr if full.
     */
    template <typename... Args>
    Ptr acquire_ptr(Args&&... args) {
        T* obj = acquire(std::forward<Args>(args)...);
        if (!obj) {
            return Ptr(nullptr, Deleter{this});
        }
        return Ptr(obj, Deleter{this});
    }

    /**
     * @brief Release an object back to the pool.
     * Calls the destructor and adds the memory slot back to the free list.
     *
     * @param obj Pointer to the object to release. Must be owned by this pool.
     */
    void release(T* obj) {
        if (!obj) return;

        // Security check: ensure pointer belongs to this pool
        assert(owns(obj) && "Attempting to release pointer not owned by this pool");

        // 1. Destruct
        obj->~T();

        // 2. Add to free list (LIFO)
        FreeNode* node = reinterpret_cast<FreeNode*>(obj);
        node->next = _free_head;
        _free_head = node;

        _in_use_count--;
    }

    // ─── Status Queries ───

    constexpr size_t capacity() const { return N; }

    size_t available() const { return N - _in_use_count; }

    size_t in_use() const { return _in_use_count; }

    bool empty() const { return available() == 0; }

    /**
     * @brief Check if a pointer lies within the pool's memory range and alignment.
     */
    bool owns(const T* obj) const {
        if (!obj) return false;

        const std::byte* ptr_byte = reinterpret_cast<const std::byte*>(obj);
        const std::byte* start = _storage.data();
        const std::byte* end = start + (SLOT_SIZE * N);

        if (ptr_byte < start || ptr_byte >= end) {
            return false;
        }

        // Check alignment relative to start
        size_t offset = static_cast<size_t>(ptr_byte - start);
        return (offset % SLOT_SIZE == 0);
    }

private:
    /**
     * @brief Initialize the free list.
     */
    void reset() {
        _in_use_count = 0;
        _free_head = nullptr;

        // Link all slots in reverse order so head points to index 0 (cache friendly)
        for (size_t i = N; i > 0; --i) {
            size_t index = i - 1;
            void* mem = &_storage[index * SLOT_SIZE];
            FreeNode* node = static_cast<FreeNode*>(mem);
            node->next = _free_head;
            _free_head = node;
        }
    }

    // Storage: Raw bytes with correct alignment for T and FreeNode
    alignas(SLOT_ALIGN) std::array<std::byte, SLOT_SIZE * N> _storage;

    // Head of the intrusive free list
    FreeNode* _free_head;

    size_t _in_use_count;
};
    
} // namespace neustack


#endif // NEUSTACK_COMMON_MEMORY_POOL_HPP