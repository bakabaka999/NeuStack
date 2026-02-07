#include <catch2/catch_test_macros.hpp>
#include "neustack/common/ring_buffer.hpp"
#include <vector>
#include <cstring>

using namespace neustack;

TEST_CASE("StreamBuffer basic state and lifecycle", "[common][ring_buffer]") {
    SECTION("Initial state") {
        StreamBuffer buffer(16);
        CHECK(buffer.capacity() == 16);
        CHECK(buffer.size() == 0);
        CHECK(buffer.available() == 16);
        CHECK(buffer.empty() == true);
        CHECK(buffer.full() == false);
    }

    SECTION("Default constructor") {
        StreamBuffer buffer;
        CHECK(buffer.capacity() == 65536); // 64KB
    }

    SECTION("Clear state") {
        StreamBuffer buffer(10);
        uint8_t data[] = {1, 2, 3};
        buffer.write(data, 3);
        buffer.clear();
        CHECK(buffer.empty() == true);
        CHECK(buffer.size() == 0);
        CHECK(buffer.available() == 10);
    }
}

TEST_CASE("StreamBuffer read/write operations", "[common][ring_buffer]") {
    SECTION("Standard write and read") {
        StreamBuffer buffer(16);
        const char* msg = "hello";
        size_t written = buffer.write(reinterpret_cast<const uint8_t*>(msg), 5);
        CHECK(written == 5);
        CHECK(buffer.size() == 5);

        uint8_t out[5];
        size_t read_bytes = buffer.read(out, 5);
        CHECK(read_bytes == 5);
        CHECK(std::memcmp(out, "hello", 5) == 0);
        CHECK(buffer.empty() == true);
    }

    SECTION("Full buffer behavior") {
        StreamBuffer buffer(4);
        uint8_t data[] = {1, 2, 3, 4};
        CHECK(buffer.write(data, 4) == 4);
        CHECK(buffer.full() == true);
        CHECK(buffer.available() == 0);

        // Overwrite attempt
        uint8_t extra = 5;
        CHECK(buffer.write(&extra, 1) == 0);
    }

    SECTION("Partial reading") {
        StreamBuffer buffer(10);
        buffer.write(reinterpret_cast<const uint8_t*>("0123456789"), 10);
        
        uint8_t out[5];
        CHECK(buffer.read(out, 5) == 5);
        CHECK(buffer.size() == 5);
        CHECK(std::memcmp(out, "01234", 5) == 0);
    }
}

TEST_CASE("StreamBuffer wraparound logic", "[common][ring_buffer]") {
    SECTION("Circular wraparound write and read") {
        StreamBuffer buffer(8);
        
        // 1. Write 6 bytes [0,1,2,3,4,5,_,_]
        buffer.write(reinterpret_cast<const uint8_t*>("012345"), 6);
        
        // 2. Read 4 bytes, leaves 2 [_,_,_,_,4,5,_,_]
        uint8_t out1[4];
        buffer.read(out1, 4);
        CHECK(buffer.size() == 2);
        CHECK(buffer.available() == 6);

        // 3. Write 5 bytes (this must wrap)
        // [C,D,E,_,4,5,A,B] (assuming tail wraps to index 0)
        buffer.write(reinterpret_cast<const uint8_t*>("ABCDE"), 5);
        CHECK(buffer.size() == 7);

        // 4. Read 7 bytes across the boundary
        uint8_t out2[7];
        buffer.read(out2, 7);
        CHECK(std::memcmp(out2, "45ABCDE", 7) == 0);
        CHECK(buffer.empty() == true);
    }
}

TEST_CASE("StreamBuffer advanced access", "[common][ring_buffer]") {
    SECTION("Peek and Consume") {
        StreamBuffer buffer(10);
        buffer.write(reinterpret_cast<const uint8_t*>("abcde"), 5);

        uint8_t p[3];
        size_t peeked = buffer.peek(p, 3);
        CHECK(peeked == 3);
        CHECK(std::memcmp(p, "abc", 3) == 0);
        CHECK(buffer.size() == 5); // Size shouldn't change

        size_t consumed = buffer.consume(2);
        CHECK(consumed == 2);
        CHECK(buffer.size() == 3);
        
        uint8_t remaining[3];
        buffer.read(remaining, 3);
        CHECK(std::memcmp(remaining, "cde", 3) == 0);
    }

    SECTION("Peek at specific offset") {
        StreamBuffer buffer(10);
        buffer.write(reinterpret_cast<const uint8_t*>("abcdef"), 6);

        uint8_t out[4];
        // Peek 4 bytes starting from index 2 ('c')
        size_t ret = buffer.peek_at(2, out, 4);
        CHECK(ret == 4);
        CHECK(std::memcmp(out, "cdef", 4) == 0);
        CHECK(buffer.size() == 6);
    }
}

TEST_CASE("StreamBuffer zero-copy API", "[common][ring_buffer]") {
    SECTION("Write contiguous and commit") {
        StreamBuffer buffer(10);
        
        auto span = buffer.write_contiguous();
        // Even if capacity is 10, the first contiguous chunk might be smaller 
        // depending on head/tail, but here it's an empty buffer.
        size_t to_write = (span.len > 4) ? 4 : span.len;
        
        std::memcpy(span.data, "zero", to_write);
        buffer.commit_write(to_write);

        CHECK(buffer.size() == to_write);
        
        uint8_t out[4];
        buffer.read(out, to_write);
        CHECK(std::memcmp(out, "zero", to_write) == 0);
    }

    SECTION("Peek contiguous") {
        StreamBuffer buffer(10);
        buffer.write(reinterpret_cast<const uint8_t*>("data"), 4);

        auto span = buffer.peek_contiguous();
        CHECK(span.len >= 4);
        CHECK(std::memcmp(span.data, "data", 4) == 0);
    }
}