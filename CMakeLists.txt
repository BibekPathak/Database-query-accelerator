# ===========================================================================
#  DBQA -- FPGA Database Query Accelerator
#
#  Canonical build system for the C++20 / Verilator testbenches.
#
#  Each simulation testbench is a small C++ program driving a Verilated DUT.
#  Verilator compiles the SystemVerilog RTL to C++, then g++/clang++ compiles
#  and links the testbench against it into a self-checking executable that is
#  registered with CTest.
#
#  TBs that need RTL (DBQA_TBS) are only generated once RTL sources exist.
#  The plain-C++ toolchain smoke test (tb_smoke) is always built.
# ===========================================================================

cmake_minimum_required(VERSION 3.20)

project(dbqa_accelerator
    VERSION 0.1.0
    DESCRIPTION "FPGA Database Query Accelerator"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

find_program(VERILATOR_BIN verilator REQUIRED)

# ---------------------------------------------------------------------------
# RTL sources.
# Append each module as it lands (Phases 1-7). Order matters: packages and
# interfaces must precede the modules that consume them.
# ---------------------------------------------------------------------------
set(RTL_SOURCES
    # Phase 1: core primitives
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/common/db_pkg.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/interfaces/axis_fifo.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/interfaces/axis_register.sv
    # Phase 2: columnar memory
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/memory/column_memory.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/memory/column_reader.sv
    # Phase 3: predicates
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/operators/predicate_engine.sv
    # Phase 4: projection
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/operators/projection_engine.sv
    # Phase 5: aggregation
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/operators/count_engine.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/operators/sum_engine.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/operators/min_engine.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/operators/max_engine.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/operators/avg_engine.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/operators/aggregation_top.sv
    # Phase 6: group by
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/operators/groupby_engine.sv
    # Phase 7: scheduler + top
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/scheduler/scheduler.sv
)

# ---------------------------------------------------------------------------
# Simulation testbenches.
# Format: "<tb_name>;<verilated_top_module>"
# ---------------------------------------------------------------------------
set(DBQA_TBS
    "tb_fifo\;axis_fifo_multi"
    "tb_reader\;column_reader_tb_top"
    "tb_predicate\;predicate_engine"
    "tb_projection\;projection_engine"
    "tb_aggregation\;aggregation_top"
    "tb_groupby\;groupby_engine"
    "tb_scheduler\;scheduler"
)

# Testbench-specific extra SystemVerilog sources (e.g. test wrappers).
# Format: set(DBQA_TB_EXTRA_<tb_name> <file>...)
set(DBQA_TB_EXTRA_tb_fifo ${CMAKE_CURRENT_SOURCE_DIR}/sim/axis_fifo_multi.sv)
set(DBQA_TB_EXTRA_tb_reader ${CMAKE_CURRENT_SOURCE_DIR}/sim/column_reader_tb_top.sv)

# ---------------------------------------------------------------------------
# Common Verilator flags for every simulation testbench.
# ---------------------------------------------------------------------------
set(DBQA_VERILATOR_FLAGS
    --cc --exe --build   # Verilate, compile and link the executable
    --timing        # enable $time / delays
    --assert        # elaborate SVA assertions in the simulation
    -DDBQA_ASSERT   # compile assertion blocks guarded by DBQA_ASSERT
    -Wall           # enable all lint warnings
    -Wno-fatal      # do not treat warnings as build failures
)

set(DBQA_CXXFLAGS "-std=c++20")

# ---------------------------------------------------------------------------
# Add one Verilator-based simulation testbench.
#
#   dbqa_add_tb(<name> <top_module>)
# ---------------------------------------------------------------------------
function(dbqa_add_tb name top)
    if(NOT name)
        message(FATAL_ERROR "dbqa_add_tb: missing testbench name")
    endif()
    if(NOT top)
        message(FATAL_ERROR "dbqa_add_tb(${name}): missing top module")
    endif()

    set(mdir "${CMAKE_BINARY_DIR}/${name}")
    set(exe "${mdir}/V${name}")
    set(srcs ${RTL_SOURCES} ${DBQA_TB_EXTRA_${name}}
             "${CMAKE_CURRENT_SOURCE_DIR}/sim/${name}.cpp")

    add_custom_command(
        OUTPUT "${exe}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${mdir}"
        COMMAND "${VERILATOR_BIN}" ${DBQA_VERILATOR_FLAGS}
                --Mdir "${mdir}"
                --top-module "${top}"
                -CFLAGS "${DBQA_CXXFLAGS}"
                ${srcs}
                --exe -o "V${name}"
        DEPENDS ${srcs}
        COMMENT "Building testbench ${name}"
        VERBATIM)

    add_custom_target(${name} ALL DEPENDS "${exe}")
    add_test(NAME ${name} COMMAND "${exe}")
endfunction()

# ---------------------------------------------------------------------------
# Toolchain smoke test: pure C++20, no RTL. Proves the compiler, C++20 mode,
# and CTest plumbing work before any RTL exists.
# ---------------------------------------------------------------------------
add_executable(tb_smoke "${CMAKE_CURRENT_SOURCE_DIR}/sim/tb_smoke.cpp")
add_test(NAME tb_smoke COMMAND tb_smoke)

# ---------------------------------------------------------------------------
# Verilated testbenches. Only added once RTL sources exist.
# ---------------------------------------------------------------------------
if(RTL_SOURCES)
    foreach(pair IN LISTS DBQA_TBS)
        list(GET pair 0 tb_name)
        list(GET pair 1 tb_top)
        dbqa_add_tb(${tb_name} ${tb_top})
    endforeach()
else()
    message(STATUS "No RTL sources yet; Verilated testbenches will be added in Phase 1+")
endif()

enable_testing()
