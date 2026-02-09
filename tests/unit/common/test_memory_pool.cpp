#include <catch2/catch_test_macros.hpp>
#include "neustack/common/memory_pool.hpp"
#include <string>
#include <vector>

using namespace neustack;

// Helper struct for testing construction/destruction
struct ComplexObject {
    int id;
    std::string name;
    static int construct_count;
    static int destruct_count;

    ComplexObject(int i, std::string n) : id(i), name(std::move(n)) {
        construct_count++;
    }
    ~ComplexObject() {
        destruct_count++;
    }
};

int ComplexObject::construct_count = 0;
int ComplexObject::destruct_count = 0;

TEST_CASE("FixedPool: Basic Operations", "[common][memory_pool]") {
    FixedPool<int, 4> pool;

    SECTION("Initial state") {
        CHECK(pool.capacity() == 4);
        CHECK(pool.available() == 4);
        CHECK(pool.in_use() == 0);
        CHECK(pool.empty() == false);
    }

    SECTION("Acquire and Release") {
        int* p1 = pool.acquire(10);
        REQUIRE(p1 != nullptr);
        CHECK(*p1 == 10);
        CHECK(pool.in_use() == 1);
        CHECK(pool.available() == 3);
        CHECK(pool.owns(p1));

        int* p2 = pool.acquire(20);
        REQUIRE(p2 != nullptr);
        CHECK(*p2 == 20);
        CHECK(pool.in_use() == 2);

        // Slots are contiguous; distance = max(sizeof(int), sizeof(void*))
        constexpr size_t SLOT = sizeof(int) > sizeof(void*) ? sizeof(int) : sizeof(void*);
        CHECK(std::abs((std::byte*)p2 - (std::byte*)p1) == SLOT);

        pool.release(p1);
        CHECK(pool.in_use() == 1);
        
        // Re-acquire should likely get p1 back (LIFO)
        int* p3 = pool.acquire(30);
        CHECK(p3 == p1);
        CHECK(*p3 == 30);

        pool.release(p2);
        pool.release(p3);
    }
}

TEST_CASE("FixedPool: Exhaustion", "[common][memory_pool]") {
    FixedPool<double, 2> pool;

    double* d1 = pool.acquire(1.1);
    double* d2 = pool.acquire(2.2);

    CHECK(pool.in_use() == 2);
    CHECK(pool.empty() == true);

    // Pool is full
    double* d3 = pool.acquire(3.3);
    CHECK(d3 == nullptr);

    pool.release(d1);
    CHECK(pool.empty() == false);
    
    // Now valid
    d3 = pool.acquire(3.3);
    CHECK(d3 != nullptr);
    CHECK(*d3 == 3.3);

    // Cleanup
    pool.release(d2);
    pool.release(d3);
}

TEST_CASE("FixedPool: Constructor and Destructor calls", "[common][memory_pool]") {
    ComplexObject::construct_count = 0;
    ComplexObject::destruct_count = 0;

    FixedPool<ComplexObject, 5> pool;

    SECTION("Lifecycle management") {
        auto* obj = pool.acquire(1, "test");
        CHECK(obj->id == 1);
        CHECK(obj->name == "test");
        CHECK(ComplexObject::construct_count == 1);
        CHECK(ComplexObject::destruct_count == 0);

        pool.release(obj);
        CHECK(ComplexObject::construct_count == 1);
        CHECK(ComplexObject::destruct_count == 1);
    }
}

TEST_CASE("FixedPool: Ownership Check", "[common][memory_pool]") {
    FixedPool<int, 2> pool;
    int* p1 = pool.acquire(1);
    int outside_stack = 1;
    int* outside_heap = new int(1);

    CHECK(pool.owns(p1) == true);
    CHECK(pool.owns(&outside_stack) == false);
    CHECK(pool.owns(outside_heap) == false);
    CHECK(pool.owns(nullptr) == false);

    pool.release(p1);
    delete outside_heap;
}

TEST_CASE("FixedPool: RAII Smart Pointer", "[common][memory_pool]") {
    ComplexObject::construct_count = 0;
    ComplexObject::destruct_count = 0;
    FixedPool<ComplexObject, 3> pool;

    {
        auto ptr = pool.acquire_ptr(99, "raii");
        CHECK(ptr != nullptr);
        CHECK(ptr->id == 99);
        CHECK(pool.in_use() == 1);
        CHECK(ComplexObject::construct_count == 1);
        
        // Move semantics
        auto ptr2 = std::move(ptr);
        CHECK(ptr == nullptr);
        CHECK(ptr2 != nullptr);
        CHECK(pool.in_use() == 1);
    } // ptr2 goes out of scope here

    CHECK(pool.in_use() == 0);
    CHECK(ComplexObject::destruct_count == 1);
}

TEST_CASE("FixedPool: Full Cycle Integrity", "[common][memory_pool]") {
    constexpr size_t N = 10;
    FixedPool<size_t, N> pool;
    std::vector<size_t*> ptrs;

    // 1. Fill
    for (size_t i = 0; i < N; ++i) {
        auto* p = pool.acquire(i);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }
    CHECK(pool.empty());

    // 2. Empty
    for (auto* p : ptrs) {
        pool.release(p);
    }
    CHECK(pool.in_use() == 0);

    // 3. Fill again
    ptrs.clear();
    for (size_t i = 0; i < N; ++i) {
        auto* p = pool.acquire(i);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }
    CHECK(pool.empty());

    // 4. Release all so destructor doesn't assert
    for (auto* p : ptrs) {
        pool.release(p);
    }
}