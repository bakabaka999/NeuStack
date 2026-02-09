#include <catch2/catch_test_macros.hpp>
#include "neustack/common/spsc_queue.hpp"
#include <cstdint>

using namespace neustack;

TEST_CASE("SPSCQueue basic operations", "[common][spsc_queue]") {
    // N=4 is a power of 2, satisfying the API requirement
    SPSCQueue<int, 4> queue;

    SECTION("Initial state") {
        CHECK(queue.empty() == true);
        CHECK(queue.size() == 0);
    }

    SECTION("Push and pop a single element") {
        int value = 42;
        CHECK(queue.try_push(value) == true);
        CHECK(queue.empty() == false);
        CHECK(queue.size() == 1);

        int out = 0;
        CHECK(queue.try_pop(out) == true);
        CHECK(out == 42);
        CHECK(queue.empty() == true);
        CHECK(queue.size() == 0);
    }

    SECTION("FIFO order preservation") {
        queue.try_push(1);
        queue.try_push(2);
        queue.try_push(3);

        int out = 0;
        queue.try_pop(out);
        CHECK(out == 1);
        queue.try_pop(out);
        CHECK(out == 2);
        queue.try_pop(out);
        CHECK(out == 3);
    }

    SECTION("Empty queue pop failure") {
        int out = -1;
        CHECK(queue.try_pop(out) == false);
        CHECK(out == -1); // Value should remain unchanged
    }
}

TEST_CASE("SPSCQueue capacity and wrap-around", "[common][spsc_queue]") {
    SPSCQueue<int, 4> queue;

    SECTION("Full queue behavior") {
        CHECK(queue.try_push(10) == true);
        CHECK(queue.try_push(20) == true);
        CHECK(queue.try_push(30) == true);
        CHECK(queue.try_push(40) == true);
        
        CHECK(queue.size() == 4);
        // 5th push should fail as capacity is 4
        CHECK(queue.try_push(50) == false);
        CHECK(queue.size() == 4);
    }

    SECTION("Pop from full and push again") {
        for (int i = 1; i <= 4; ++i) queue.try_push(i);
        
        int out = 0;
        CHECK(queue.try_pop(out) == true);
        CHECK(out == 1);
        CHECK(queue.size() == 3);

        // Now we should be able to push one more
        CHECK(queue.try_push(5) == true);
        CHECK(queue.size() == 4);
    }

    SECTION("Multiple rounds of push/pop") {
        int push_count = 0;
        int pop_count = 0;
        int out = 0;

        // Round 1: push 2, pop 1
        queue.try_push(++push_count); // 1
        queue.try_push(++push_count); // 2
        queue.try_pop(out);           // pops 1
        CHECK(out == 1);

        // Round 2: push 2 more (total in queue: 2, 3, 4)
        queue.try_push(++push_count); // 3
        queue.try_push(++push_count); // 4
        
        // Round 3: pop all
        queue.try_pop(out); CHECK(out == 2);
        queue.try_pop(out); CHECK(out == 3);
        queue.try_pop(out); CHECK(out == 4);
        
        CHECK(queue.empty() == true);
    }
}

TEST_CASE("SPSCQueue trivial copyable requirement", "[common][spsc_queue]") {
    struct Pod {
        uint32_t a;
        uint64_t b;
    };

    SPSCQueue<Pod, 2> pod_queue;

    SECTION("Push and pop POD struct") {
        Pod p1{100, 200};
        CHECK(pod_queue.try_push(p1) == true);
        
        Pod p2{0, 0};
        CHECK(pod_queue.try_pop(p2) == true);
        CHECK(p2.a == 100);
        CHECK(p2.b == 200);
    }
}