# cmake/BPFCompile.cmake
#
# Detects clang + libbpf and provides neustack_add_bpf() for compiling
# BPF/XDP programs as part of the normal build.

# ── 1. Find clang ──────────────────────────────────────────────────────────

find_program(BPF_CLANG clang)

if(NOT BPF_CLANG)
    message(WARNING "clang not found — BPF/XDP programs will NOT be compiled")
    set(NEUSTACK_BPF_SUPPORTED OFF)
    return()
endif()

message(STATUS "BPF clang: ${BPF_CLANG}")

# ── 2. Find libbpf (headers + library) ────────────────────────────────────

find_path(LIBBPF_INCLUDE_DIR
    NAMES bpf/bpf.h
    PATH_SUFFIXES include
)

find_library(LIBBPF_LIBRARY
    NAMES bpf
)

if(NOT LIBBPF_INCLUDE_DIR OR NOT LIBBPF_LIBRARY)
    message(WARNING "libbpf not found — BPF/XDP programs will NOT be compiled\n"
                    "  Install: apt install libbpf-dev  (or dnf install libbpf-devel)")
    set(NEUSTACK_BPF_SUPPORTED OFF)
    return()
endif()

message(STATUS "libbpf include: ${LIBBPF_INCLUDE_DIR}")
message(STATUS "libbpf library: ${LIBBPF_LIBRARY}")

set(NEUSTACK_BPF_SUPPORTED ON)

# ── 3. BPF output directory ───────────────────────────────────────────────

set(BPF_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bpf")
file(MAKE_DIRECTORY "${BPF_OUTPUT_DIR}")

# ── 4. neustack_add_bpf(name source) ─────────────────────────────────────
#
# Compiles a .c BPF source into a .o ELF object using clang -target bpf.
# The resulting .o is placed in ${BPF_OUTPUT_DIR}/<name>.o.

function(neustack_add_bpf name source)
    get_filename_component(_src_abs "${source}" ABSOLUTE)
    set(_output "${BPF_OUTPUT_DIR}/${name}.o")

    # Kernel headers typically live under /usr/include; libbpf helpers
    # (bpf_helpers.h) are found via LIBBPF_INCLUDE_DIR.
    add_custom_command(
        OUTPUT  "${_output}"
        COMMAND "${BPF_CLANG}"
                -O2
                -target bpf
                -g
                -I "${LIBBPF_INCLUDE_DIR}"
                -c "${_src_abs}"
                -o "${_output}"
        DEPENDS "${_src_abs}"
        COMMENT "BPF  ${name}.o"
        VERBATIM
    )

    # Per-program target so other targets can depend on it.
    add_custom_target(bpf_${name} DEPENDS "${_output}")
endfunction()
