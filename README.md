# CAI SDK C++ library

The [c2pa-cpp repository](https://github.com/contentauth/c2pa-cpp) implements C++ APIs that:
- Read and validate C2PA data from media files in supported formats.
- Add signed manifests to media files in supported formats.

Although this library works for plain C applications, the documentation assumes you're using C++, since that's most common for modern applications.

<div class="hide-doxygen" >
<div style={{display: 'none'}}>

For the best experience, read the docs on the [CAI Open Source SDK documentation website](https://opensource.contentauthenticity.org/docs/c2pa-c).

If you want to view the documentation in GitHub, see:
- [Using the C++ library](docs/usage.md)
- [Supported formats](https://github.com/contentauth/c2pa-rs/blob/main/docs/supported-formats.md)
- [Configuring the SDK using `Context` and `Settings`](docs/context-settings.md)
- [Using Builder intents](docs/intents.md) to ensure spec-compliant manifests
- Using [working stores and archvies](docs/working-stores.md)
- Selectively constructing manifests by [filtering actions and ingredients](docs/selective-manifests.md)
- Using the [embeddable API for low-level control over embedding manifests](docs/embeddable-api.md)
- [Frequently-asked questions (FAQs)](docs/faqs.md)
- [Release notes](docs/release-notes.md)

</div>
</div>

## Using c2pa_cpp

The recommended way to use this library in your own CMake project is with [FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html):

```cmake
include(FetchContent)

FetchContent_Declare(
    c2pa_cpp
    GIT_REPOSITORY https://github.com/contentauth/c2pa-cpp.git
    GIT_TAG main  # Or use a specific release tag
)
FetchContent_MakeAvailable(c2pa_cpp)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE c2pa_cpp)
```

This will automatically fetch, build, and link the `c2pa_cpp` library and its dependencies.

> **Note:**
> This project uses pre-built dynamic libraries from the [c2pa-rs](https://github.com/contentauth/c2pa-rs) repository. It should select the correct library for your platform. If your platform is not supported, you can build your own library using the c2pa_rs repo.

### Example usage

See the [`examples/`](examples/) directory for sample applications that demonstrate how to use the library in practice.

## Development

This project has been tested on macOS and should also work on common Linux distributions.

You must install the [Ninja](https://github.com/ninja-build/ninja/wiki/Pre-built-Ninja-packages) build system to run the unit tests.

### Building using pre-built C FFI libraries

Building the library holding the C++ SDK requires [GNU make](https://www.gnu.org/software/make/), which is installed on most macOS systems.

Enter this command to build the SDK:

```sh
make release
```

This will download the [pre-build libraries published with c2pa releases](https://github.com/contentauth/c2pa-rs/releases), build and link the C++ code.

The Makefile has a number of other targets; for example:
- `test` to run unit tests
- `examples` to build and run the C++ examples.
- `emscripten-example` to build the Emscripten/WebAssembly example (see below).
- `all` to build and run everything.

Results are saved in the `build` directory.

### Building using local sources

This project can also be built entirely from source (without pre-built library download), with the pre-requisite that you will also need [c2pa-rs](https://github.com/contentauth/c2pa-rs) on the local machine, as well as the [Rust toolchain](https://rust-lang.org/tools/install/).

To build in this case, the build scripts need to be able to locate the `c2pa-rs` sources as well as the library this builds for linking. This is done by setting environment variables in the terminal where the builds will run.

```sh
# Enable local c2pa-rs build
export C2PA_BUILD_FROM_SOURCE=ON

# If local build is enabled, set this environment variable to contain the path to c2pa-rs sources
export C2PA_RS_PATH=path_to_c2pa_rs_sources

# Since this is going to build Rust code, the build system needs to locate cargo, the tool to build Rust code
# Add Rust cargo to PATH if not already there
export PATH="$HOME/.cargo/bin:$PATH"

# macOs: Set built library path for running tests
export DYLD_LIBRARY_PATH="$(pwd)/build/release/tests:$DYLD_LIBRARY_PATH"

# Linux: Set built library path for running tests
export LD_LIBRARY_PATH="$(pwd)/build/release/tests:$LD_LIBRARY_PATH"
```

### Building the Emscripten/WebAssembly example

The [`examples/emscripten_example.cpp`](examples/emscripten_example.cpp) file demonstrates using the c2pa C++ library compiled to WebAssembly via Emscripten. It includes reading manifests from files, streams, and using a custom HTTP resolver with `emscripten_fetch`.

Prerequisites: Install the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) (4.x or later recommended) and activate it in your shell:

```sh
source /path/to/emsdk/emsdk_env.sh
```

Build and run:

```sh
make emscripten-example
node build/emscripten-example/c2pa_example.js path/to/image.jpg
```

This downloads prebuilt wasm libraries from the c2pa-rs release, compiles the C++ sources with `emcc`, and produces a Node.js-runnable output. The HTTP resolver example requires a Web Worker in the browser but works without restriction under Node.js.

### Testing

Build the [unit tests](https://github.com/contentauth/c2pa-cpp/tree/main/tests) by entering this `make` command:

```
make test
```

The Rust `c2pa_c` library does not need to be sanitizer-instrumented for this to work.

### Troubleshooting

#### Sanitizer test builds fail on macOS

`make test-san` builds the tests with AddressSanitizer/UBSan. On some recent macOS versions the
AddressSanitizer runtime shipped with Xcode's AppleClang can abort at process start with a message like:

```sh
AddressSanitizer: CHECK failed: sanitizer_malloc_mac.inc:189 "((!asan_init_is_running)) != (0)"
```

This is an incompatibility between an older AppleClang sanitizer runtime and the host
malloc. The fix is to build the sanitizer target with a newer LLVM/Clang (for example, Homebrew's `llvm`).

To do so, install (or update) an `llvm` version. For instance, using homebrew::

```sh
brew install llvm
```

Then, build and run the tests with the sanitizers activated. First, configure the build:

```sh
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON \
  -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" \
  -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++"
```

Note that the llvm compiler from `brew --prefix llvm` must properly resolve for this command to work.

Then run the build scripts:

```sh
cmake --build build/debug
```

And finally run the test executable:

```sh
ctest --test-dir build/debug --output-on-failure
```


### Building API documentation

API documentation generated by Doxygen is automatically built on each PR.

To generate API docs locally, these are the main files:

- Configuration file: `c2pa-cpp/Doxyfile`
- Script: `c2pa-cpp/scripts/generate_api_docs.sh`
- Output directory: `docs/_build/html`

Install Doxygen if needed:

```sh
macOS: brew install doxygen
Ubuntu/Debian: sudo apt-get install doxygen
```

To generate docs, enter the command:

```sh
./scripts/generate_api_docs.sh
```

Or run `make -C docs`.

Open `_build/html/index.html` to see the results.

## License

This package is distributed under the terms of both the [MIT license](https://github.com/contentauth/c2pa-cpp/blob/main/LICENSE-MIT) and the [Apache License (Version 2.0)](https://github.com/contentauth/c2pa-cpp/blob/main/LICENSE-APACHE).

Note that some components and dependent crates are licensed under different terms; please check the license terms for each crate and component for details.

### Contributions and feedback

We welcome contributions to this project.  For information on contributing, providing feedback, and about ongoing work, see [Contributing](https://github.com/contentauth/c2pa-cpp/blob/main/CONTRIBUTING.md).
