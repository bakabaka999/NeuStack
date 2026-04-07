# ============================================================================
# NeuStack CLI Tools
# ============================================================================

# neustack-stat: standalone HTTP client for live telemetry
add_executable(neustack_stat neustack_stat.cpp)
target_compile_features(neustack_stat PRIVATE cxx_std_20)

# Windows needs Winsock
if(NEUSTACK_PLATFORM_WINDOWS)
    target_link_libraries(neustack_stat PRIVATE ws2_32)
endif()

# Output name with hyphen
set_target_properties(neustack_stat PROPERTIES OUTPUT_NAME "neustack-stat")

install(TARGETS neustack_stat RUNTIME DESTINATION bin)

# udp_flood: high-speed UDP packet generator for benchmarking
if(NEUSTACK_PLATFORM_LINUX)
    add_executable(udp_flood udp_flood.cpp)
endif()

# ============================================================================
# Dashboard HTML embedding
# ============================================================================
set(DASHBOARD_SRC "${CMAKE_CURRENT_SOURCE_DIR}/dashboard/index.html")
if(EXISTS "${DASHBOARD_SRC}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
    file(READ "${DASHBOARD_SRC}" DASHBOARD_CONTENT)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/dashboard/dashboard_embed.hpp.in"
        "${CMAKE_BINARY_DIR}/generated/dashboard_embed.hpp"
        @ONLY
    )
endif()
