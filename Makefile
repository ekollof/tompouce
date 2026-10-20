# Makefile wrapper for the CMake/Ninja build.
# Preferred workflow (see AGENTS.md and README.md):
#   make            -> configure + build
#   make test       -> ctest with output on failures
#   make <target>   -> cmake --build build --target <target>
#
# Variables:
#   BUILD_DIR   build directory (default: build)
#   JOBS        parallel build jobs (default: 4)
#   PRESET      CMake configure/build preset (default: default)
#   TEST_FILTER ctest -R filter for `make test FILTER=...` (also TOMPOUCE_TEST_FILTER)
#   EXTRA_CMAKE extra -D flags for configure, e.g. EXTRA_CMAKE="-DWITH_CSOUND=OFF"

BUILD_DIR ?= build
JOBS ?= 4
PRESET ?= default
TEST_FILTER ?=
EXTRA_CMAKE ?=

CMAKE ?= cmake
CTEST ?= ctest
NODE ?= node
PYTHON ?= python3

# Custom CMake targets defined in cmake/Tests.cmake plus bundle helpers.
CMAKE_TARGETS := \
	test-tui \
	test-sanitize \
	test-terminal-sanitize \
	test-controls-reference-web \
	test-duck-reference-web \
	test-voice-reference-web \
	test-csound-reference-web \
	test-live-reference-web \
	test-examples \
	test-audio-backends \
	compare-audio \
	profile-prep-deadlines \
	profile-prep-backend \
	js-bundle \
	companion-bundle

.PHONY: all build configure test check clean distclean help install \
	$(CMAKE_TARGETS) \
	effects-reference-qa companion-qa csound-native-qa \
	prep-deadline-qa prep-backend-qa \
	live-change-reference-qa voice-reference-qa duck-reference-qa

all: build

configure:
	$(CMAKE) --preset $(PRESET) $(EXTRA_CMAKE)

build: configure
	$(CMAKE) --build --preset $(PRESET) --parallel $(JOBS)

# Main test entry point: native suite + PTY + reference gates registered in CTest.
test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure --parallel $(JOBS) $(if $(TEST_FILTER),-R "$(TEST_FILTER)")

# Alias kept for `make -j4 test`-style invocations from older docs.
check: test

# Forward custom CMake test/profile targets to the build tree.
$(CMAKE_TARGETS):
	$(CMAKE) --build $(BUILD_DIR) --target $@

# Standalone QA scripts that own their browser/profile/engine resources.
# These mirror the AGENTS.md focused checks.
effects-reference-qa: build
	$(NODE) scripts/effects-reference-qa.mjs

companion-qa: build
	$(NODE) scripts/companion-qa.mjs

csound-native-qa: build
	$(NODE) scripts/csound-native-qa.mjs

prep-deadline-qa: profile-prep-deadlines

prep-backend-qa: profile-prep-backend

live-change-reference-qa: test-live-reference-web

voice-reference-qa: test-voice-reference-web

duck-reference-qa: test-duck-reference-web

clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean || true

distclean:
	rm -rf $(BUILD_DIR)

install: build
	./install.sh --skip-deps --skip-banks --no-browser --jobs $(JOBS)

help:
	@echo "Tompouce make wrapper (CMake preset: $(PRESET), build dir: $(BUILD_DIR))"
	@echo ""
	@echo "  make [build]                    configure + compile"
	@echo "  make test [TEST_FILTER=regex]   run ctest (use TEST_FILTER for -R)"
	@echo "  make check                      alias for test"
	@echo ""
	@echo "  CMake test targets:"
	@echo "    test-tui test-sanitize test-terminal-sanitize test-audio-backends"
	@echo "    test-controls-reference-web test-duck-reference-web"
	@echo "    test-voice-reference-web test-csound-reference-web"
	@echo "    test-live-reference-web test-examples"
	@echo "    compare-audio profile-prep-deadlines profile-prep-backend"
	@echo "    js-bundle companion-bundle"
	@echo ""
	@echo "  QA scripts:"
	@echo "    effects-reference-qa companion-qa csound-native-qa"
	@echo ""
	@echo "  make clean / distclean          clean build tree / remove it"
	@echo "  make install                    minimal local install (no deps/banks/browser)"
	@echo "  Variables: BUILD_DIR=$(BUILD_DIR) JOBS=$(JOBS) PRESET=$(PRESET)"
