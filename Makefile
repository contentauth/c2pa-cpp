OS := $(shell uname)

# Build directories
BUILD_DIR = build
DEBUG_BUILD_DIR = build/debug
RELEASE_BUILD_DIR = build/release

# CMake options (can be overridden via environment)
CMAKE_OPTS :=
ifdef C2PA_BUILD_FROM_SOURCE
	CMAKE_OPTS += -DC2PA_BUILD_FROM_SOURCE=$(C2PA_BUILD_FROM_SOURCE)
endif
ifdef C2PA_RS_PATH
	CMAKE_OPTS += -DC2PA_RS_PATH="$(C2PA_RS_PATH)"
endif

# Sanitizer builds need a compiler-rt that matches the running OS. On macOS 26+
# the Apple clang (Xcode 16) AddressSanitizer runtime can abort at process startup
# with "sanitizer_malloc_mac.inc CHECK failed: ((!asan_init_is_running))"
# before main() runs. If a Homebrew LLVM clang is actually installed, use it for
# sanitizer builds; otherwise fall back to the default toolchain (fine on older
# macOS and on Linux). Override with SAN_CC / SAN_CXX.
# Note: `brew --prefix llvm` prints a path even when llvm is NOT installed, so the
# binary's existence must be verified (-x) before using it.
SAN_CMAKE_OPTS :=
ifeq ($(OS),Darwin)
SAN_CC ?= $(shell c="$$(brew --prefix llvm 2>/dev/null)/bin/clang"; [ -x "$$c" ] && echo "$$c")
SAN_CXX ?= $(shell c="$$(brew --prefix llvm 2>/dev/null)/bin/clang++"; [ -x "$$c" ] && echo "$$c")
ifneq ($(SAN_CC),)
SAN_CMAKE_OPTS += -DCMAKE_C_COMPILER=$(SAN_CC) -DCMAKE_CXX_COMPILER=$(SAN_CXX)
endif
endif

# Default target
all: clean test examples

clean:
	rm -rf $(BUILD_DIR)

# Debug build
debug:
	cmake -S . -B $(DEBUG_BUILD_DIR) -G "Ninja" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON $(CMAKE_OPTS)
	cmake --build $(DEBUG_BUILD_DIR)
	@ln -sf $(DEBUG_BUILD_DIR)/compile_commands.json compile_commands.json

# Release build
release:
	cmake -S . -B $(RELEASE_BUILD_DIR) -G "Ninja" -DCMAKE_BUILD_TYPE=Release $(CMAKE_OPTS)
	cmake --build $(RELEASE_BUILD_DIR)

# Legacy cmake target (uses release build)
cmake: release

# Test targets
test: clean debug
	cd $(DEBUG_BUILD_DIR) && ctest --output-on-failure

test-release: clean release
	cd $(RELEASE_BUILD_DIR) && ctest --output-on-failure

# Test with sanitizers (ASAN + UBSAN)
test-san: clean
	@if [ -n "$(SAN_CC)" ]; then \
		echo "Sanitizer build using $(SAN_CC)"; \
	elif [ "$(OS)" = "Darwin" ]; then \
		echo "NOTE: using the default toolchain for sanitizers. If tests abort at startup with"; \
		echo "      'asan_init_is_running' (Apple clang on macOS 26+), run 'brew install llvm'"; \
		echo "      or set SAN_CC/SAN_CXX to a clang whose compiler-rt supports this OS."; \
	fi
	cmake -S . -B $(DEBUG_BUILD_DIR) -G "Ninja" -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON $(CMAKE_OPTS) $(SAN_CMAKE_OPTS)
	cmake --build $(DEBUG_BUILD_DIR)
	cd $(DEBUG_BUILD_DIR) && ctest --output-on-failure

# Run only the C test (test.c)
test-c: clean release
	@echo "Running C test only..."
ifeq ($(OS),Darwin)
	DYLD_LIBRARY_PATH=$(RELEASE_BUILD_DIR)/tests:$$DYLD_LIBRARY_PATH ./$(RELEASE_BUILD_DIR)/tests/ctest
else
	LD_LIBRARY_PATH=$(RELEASE_BUILD_DIR)/tests:$$LD_LIBRARY_PATH ./$(RELEASE_BUILD_DIR)/tests/ctest
endif

# Run only the C++ tests
test-cpp: clean release
	@echo "Running C++ tests only..."
ifeq ($(OS),Darwin)
	DYLD_LIBRARY_PATH=$(RELEASE_BUILD_DIR)/tests:$$DYLD_LIBRARY_PATH ./$(RELEASE_BUILD_DIR)/tests/c2pa_c_tests
else
	LD_LIBRARY_PATH=$(RELEASE_BUILD_DIR)/tests:$$LD_LIBRARY_PATH ./$(RELEASE_BUILD_DIR)/tests/c2pa_c_tests
endif

# Run a single test by fully qualified name
# Like this: make run-single-test TEST=BuilderTest.MergingBuildersThenSignMerged
run-single-test: release
	@if [ -z "$(TEST)" ]; then echo "Usage: make run-single-test TEST=SuiteName.TestName"; exit 1; fi
	@echo "Running single test: $(TEST)"
ifeq ($(OS),Darwin)
	DYLD_LIBRARY_PATH=$(RELEASE_BUILD_DIR)/tests:$$DYLD_LIBRARY_PATH ./$(RELEASE_BUILD_DIR)/tests/c2pa_c_tests --gtest_filter="$(TEST)"
else
	LD_LIBRARY_PATH=$(RELEASE_BUILD_DIR)/tests:$$LD_LIBRARY_PATH ./$(RELEASE_BUILD_DIR)/tests/c2pa_c_tests --gtest_filter="$(TEST)"
endif

# Test with coverage reporting
# THis verifies necessary tooling for coverage check is also installed
test-coverage: clean
	cmake -S . -B build/coverage -G "Ninja" -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON $(CMAKE_OPTS)
	cmake --build build/coverage
	cd build/coverage && ctest --output-on-failure
	@echo ""
	@echo "Generating coverage report..."
	@lcov --capture --directory build/coverage --output-file build/coverage/coverage.info \
		--ignore-errors mismatch,inconsistent,unsupported,format 2>&1 \
		| grep -v "WARNING:" || { echo "Error: lcov capture failed"; exit 1; }
	@lcov --remove build/coverage/coverage.info \
		'/usr/*' '*/googletest/*' '*/json-src/*' '*/c2pa_prebuilt-src/*' '*/tests/*' \
		--output-file build/coverage/coverage_filtered.info \
		--ignore-errors unused,mismatch,inconsistent,format 2>&1 \
		| grep -v "WARNING:" || { echo "Error: lcov filter failed"; exit 1; }
	@echo ""
	@echo "=== Coverage Summary ==="
	@lcov --summary build/coverage/coverage_filtered.info \
		--ignore-errors inconsistent,format 2>&1 \
		| grep -E "(lines|functions|branches)" || true
	@echo "========================"
	@echo ""
	@if command -v genhtml > /dev/null 2>&1; then \
		genhtml build/coverage/coverage_filtered.info --output-directory build/coverage/html \
			--ignore-errors inconsistent,corrupt,unsupported,format,category 2>&1 \
			| grep -v "WARNING:" || true; \
		if [ -f build/coverage/html/index.html ]; then \
			echo "HTML report: build/coverage/html/index.html"; \
		else \
			echo "Warning: HTML report was not generated (genhtml may have failed)"; \
		fi; \
	else \
		echo "Note: genhtml not found, skipping HTML report (install lcov for HTML reports)"; \
	fi

# Demo targets
demo: release
	cmake --build $(RELEASE_BUILD_DIR) --target demo
	$(RELEASE_BUILD_DIR)/examples/demo

training: release
	cmake --build $(RELEASE_BUILD_DIR) --target training
	$(RELEASE_BUILD_DIR)/examples/training

# Build the Emscripten example (requires emsdk)
# Downloads prebuilt wasm library from c2pa-rs releases.
C2PA_VERSION := $(shell grep 'set.C2PA_VERSION' CMakeLists.txt | sed 's/[^"]*"//;s/".*//')
EMSCRIPTEN_PREBUILT_URL = https://github.com/contentauth/c2pa-rs/releases/download/c2pa-v$(C2PA_VERSION)/c2pa-v$(C2PA_VERSION)-wasm32-unknown-emscripten.zip
EMSCRIPTEN_PREBUILT_DIR = $(BUILD_DIR)/emscripten-prebuilt
EMSCRIPTEN_OUTPUT_DIR = $(BUILD_DIR)/emscripten-example

emscripten-example:
	@command -v emcc >/dev/null 2>&1 || { echo "Error: emcc not found. Install the Emscripten SDK and source emsdk_env.sh."; exit 1; }
	@mkdir -p $(EMSCRIPTEN_PREBUILT_DIR) $(EMSCRIPTEN_OUTPUT_DIR)
	@if [ ! -f "$(EMSCRIPTEN_PREBUILT_DIR)/lib/libc2pa_c.a" ]; then \
		echo "Downloading prebuilt wasm library (c2pa v$(C2PA_VERSION))..."; \
		curl -sL "$(EMSCRIPTEN_PREBUILT_URL)" -o "$(EMSCRIPTEN_PREBUILT_DIR)/c2pa-wasm.zip" || \
			{ echo "Error: failed to download $(EMSCRIPTEN_PREBUILT_URL)"; exit 1; }; \
		unzip -qo "$(EMSCRIPTEN_PREBUILT_DIR)/c2pa-wasm.zip" -d "$(EMSCRIPTEN_PREBUILT_DIR)"; \
		rm "$(EMSCRIPTEN_PREBUILT_DIR)/c2pa-wasm.zip"; \
	fi
	emcc src/c2pa_core.cpp src/c2pa_context.cpp src/c2pa_reader.cpp src/c2pa_streams.cpp \
		src/c2pa_settings.cpp src/c2pa_signer.cpp src/c2pa_builder.cpp \
		examples/emscripten_example.cpp \
		-I include -I "$(EMSCRIPTEN_PREBUILT_DIR)/include" \
		"$(EMSCRIPTEN_PREBUILT_DIR)/lib/libc2pa_c.a" \
		-o $(EMSCRIPTEN_OUTPUT_DIR)/c2pa_example.js \
		-pthread -fwasm-exceptions \
		-s WASM=1 -s USE_PTHREADS=1 -s ALLOW_MEMORY_GROWTH=1 \
		-s INITIAL_MEMORY=256MB -s MAXIMUM_MEMORY=2GB \
		-s EXPORTED_FUNCTIONS='["_main"]' \
		-s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","FS"]' \
		-s FETCH=1 -s ENVIRONMENT='node,worker' -s NODERAWFS=1 \
		-std=c++17 -O3
	@echo ""
	@echo "Build complete: $(EMSCRIPTEN_OUTPUT_DIR)/c2pa_example.js"
	@echo "Run with: node $(EMSCRIPTEN_OUTPUT_DIR)/c2pa_example.js path/to/image.jpg"
	@# Append emscripten example to compile_commands.json for LSP support
	@if [ -f compile_commands.json ]; then \
		EMSYSROOT=$$(emcc --cflags 2>/dev/null | grep -o '\-\-sysroot=[^ ]*' | cut -d= -f2); \
		if [ -n "$$EMSYSROOT" ]; then \
			python3 -c " \
import json, os; \
ccdb = json.load(open('compile_commands.json')); \
src = os.path.abspath('examples/emscripten_example.cpp'); \
ccdb = [e for e in ccdb if e.get('file') != src]; \
ccdb.append({'directory': os.getcwd(), 'file': src, \
  'command': 'c++ -std=c++17 -I include -I $(EMSCRIPTEN_PREBUILT_DIR)/include -isysroot $$EMSYSROOT -I $$EMSYSROOT/include examples/emscripten_example.cpp'}); \
json.dump(ccdb, open('compile_commands.json','w'), indent=2)"; \
		fi; \
	fi

examples: training demo

.PHONY: all debug release cmake test test-release test-san test-c test-cpp test-coverage demo training examples emscripten-example clean

# Build C API docs with Doxygen
docs:
	./scripts/generate_api_docs.sh
