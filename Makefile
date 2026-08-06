# ===========================================================================
#  DBQA -- FPGA Database Query Accelerator
#
#  Top-level build orchestration.
#
#  The canonical build system for the C++/Verilator testbenches is CMake;
#  this Makefile is the human-facing front door that wires together:
#
#    * configuration / build / run        (CMake + CTest)
#    * RTL lint                           (verilator --lint-only)
#    * formal verification                (SymbiYosys, Phase 10)
#    * synthesis                          (Vivado, Phase 11)
#
#  Usage: make help
# ===========================================================================

SHELL := /bin/bash

CMAKE     ?= cmake
CTEST     ?= ctest
VERILATOR ?= verilator

BUILD_DIR ?= build

RTL_DIR := rtl
SIM_DIR := sim

# ---------------------------------------------------------------------------
# RTL sources.
#
# Append each module here as it lands in Phases 1-7 so it is picked up by
# `make lint` and every Verilator testbench automatically.
# ---------------------------------------------------------------------------
RTL_SRCS := \
	rtl/common/db_pkg.sv \
	rtl/interfaces/axis_fifo.sv \
	rtl/interfaces/axis_register.sv

# ---------------------------------------------------------------------------
# Simulation testbenches. The canonical list lives in CMakeLists.txt
# (DBQA_TBS); TBS here drives the top-level "sim" aggregate only.
# ---------------------------------------------------------------------------
TBS := tb_smoke tb_fifo

# ---------------------------------------------------------------------------
.PHONY: help all configure build sim test lint formal synth clean tb_%

help:
	@echo 'Usage: make [target]'
	@echo ''
	@echo 'Build and test'
	@echo '  configure     Generate the CMake build tree (./build)'
	@echo '  build         Compile all testbenches'
	@echo '  sim           Build and run every testbench (ctest)'
	@echo '  test          Alias for "sim"'
	@echo '  tb_<name>     Build and run a single testbench, e.g. "make tb_fifo"'
	@echo '  lint          Verilator --lint-only over all RTL sources'
	@echo ''
	@echo 'Formal and synthesis'
	@echo '  formal        Run SymbiYosys formal proofs  (enabled in Phase 10)'
	@echo '  synth         Run the Vivado synthesis flow (enabled in Phase 11)'
	@echo ''
	@echo 'Housekeeping'
	@echo '  clean         Remove build artifacts and generated results'
	@echo '  help          Show this help message'

all: lint sim

configure:
	$(CMAKE) -B $(BUILD_DIR) -S . -DCMAKE_BUILD_TYPE=Release

build: configure
	$(CMAKE) --build $(BUILD_DIR) --parallel

sim: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

test: sim

# Pattern target: make tb_<name> -> build CMake target tb_<name>, then run it.
# Verilated TBs produce build/<tb>/V<tb>; plain-C++ TBs produce build/<tb>.
tb_%: configure
	$(CMAKE) --build $(BUILD_DIR) --target $@
	@if [ -x "$(BUILD_DIR)/$@/V$@" ]; then \
		"$(BUILD_DIR)/$@/V$@"; \
	elif [ -x "$(BUILD_DIR)/$@" ]; then \
		"$(BUILD_DIR)/$@"; \
	else \
		echo "error: no runnable artifact found for '$@'"; exit 1; \
	fi

# ---------------------------------------------------------------------------
# RTL lint. Degenerates gracefully while RTL_SRCS is still empty.
# ---------------------------------------------------------------------------
ifneq ($(strip $(RTL_SRCS)),)
# -Wno-UNUSEDPARAM: package localparams are a public API surface; a given
# module may legitimately use only a subset, so unused-package-parameter
# warnings are expected noise, not defects.
# -Wno-MULTITOP: the RTL set is a library of independent modules; multiple
# top candidates are normal until the full pipeline instantiates them.
lint:
	$(VERILATOR) --lint-only --timing -Wall -Wno-UNUSEDPARAM -Wno-MULTITOP $(RTL_SRCS)
else
lint:
	@echo "No RTL sources registered yet (RTL_SRCS is empty); nothing to lint."
endif

# ---------------------------------------------------------------------------
# Formal verification and synthesis are wired up in later phases.
# ---------------------------------------------------------------------------
formal:
	@echo "Formal verification (SymbiYosys) is enabled in Phase 10; skipping."

synth:
	@echo "Vivado synthesis flow is enabled in Phase 11; skipping."

clean:
	rm -rf $(BUILD_DIR)
	find results -mindepth 1 -not -name '.gitkeep' -exec rm -rf {} +
