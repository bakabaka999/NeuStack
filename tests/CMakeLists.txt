# ============================================================================
# NeuStack Tests
# ============================================================================

include(Catch)

# Unit tests
# add_executable(test_checksum unit/test_checksum.cpp)
# target_link_libraries(test_checksum PRIVATE neustack_lib Catch2::Catch2WithMain)
# catch_discover_tests(test_checksum)

# add_executable(test_ipv4 unit/test_ipv4.cpp)
# target_link_libraries(test_ipv4 PRIVATE neustack_lib Catch2::Catch2WithMain)
# catch_discover_tests(test_ipv4)

# Integration tests (require root/admin privileges)
# add_executable(test_ping integration/test_ping.cpp)
# target_link_libraries(test_ping PRIVATE neustack_lib Catch2::Catch2WithMain)
