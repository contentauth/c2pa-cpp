# CAI SDK C++ library

The [c2pa-cpp repository](https://github.com/contentauth/c2pa-cpp) implements C++ APIs that:
- Read and validate C2PA data from media files in supported formats.
- Add signed manifests to media files in supported formats.

Although this library works for plain C applications, the documentation assumes you're using C++, since that's most common for modern applications.

<div class="hide-doxygen" >
<div class="github-only">

For the best experience, read the docs on the [CAI Open Source SDK documentation website](https://opensource.contentauthenticity.org/docs/c2pa-cpp).

If you want to view the documentation in GitHub, see:
- [Using the C++ library](docs/usage.md)
- [Supported formats](https://github.com/contentauth/c2pa-rs/blob/main/docs/supported-formats.md)
- [Configuring the SDK using `Context` and `Settings`](docs/context-settings.md)
- [Using Builder intents](docs/intents.md) to ensure spec-compliant manifests
- Using [working stores and archives](docs/working-stores.md)
- Selectively constructing manifests by [filtering actions and ingredients](docs/selective-manifests.md)
- Using the [embeddable API for low-level control over embedding manifests](docs/embeddable-api.md)
- [Frequently asked questions (FAQs)](docs/faqs.md)
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

> [!NOTE]
> This project uses pre-built dynamic libraries from the [c2pa-rs](https://github.com/contentauth/c2pa-rs) repository. It should select the correct library for your platform. If your platform is not supported, you can build your own library using the c2pa_rs repo.

### Example usage

See the [`examples/`](https://github.com/contentauth/c2pa-cpp/tree/main/examples) directory for sample applications that demonstrate how to use the library in practice.

## Development

This project has been tested on macOS and should also work on common Linux distributions.

You must install the [Ninja](https://github.com/ninja-build/ninja/wiki/Pre-built-Ninja-packages) build system to run the unit tests.

### Building using pre-built C FFI libraries

Building the library holding the C++ SDK requires [GNU make](https://www.gnu.org/software/make/), which is installed on most macOS systems.

Enter this command to build the SDK:

```sh
make release
```

This will download the [prebuilt libraries published with c2pa releases](https://github.com/contentauth/c2pa-rs/releases), build and link the C++ code.

The Makefile has a number of other targets; for example:
- `test` to run unit tests
- `examples` to build and run the C++ examples.
- `emscripten-example` to build the Emscripten/WebAssembly example (see below).
- `all` to build and run everything.

Results are saved in the `build` directory.

### Downloading prebuilt binaries through a mirror

By default the prebuilt `c2pa_c` libraries are downloaded from the [c2pa-rs GitHub releases](https://github.com/contentauth/c2pa-rs/releases). To use a mirror instead, set `C2PA_PREBUILT_BASE_URL` to the mirror's equivalent of `https://github.com/contentauth/c2pa-rs/releases/download`:

```sh
cmake -S . -B build -DC2PA_PREBUILT_BASE_URL=https://mirror.example.com/github/contentauth/c2pa-rs/releases/download
```

### Building using local sources

This project can also be built entirely from source (without pre-built library download), with the prerequisite that you will also need [c2pa-rs](https://github.com/contentauth/c2pa-rs) on the local machine, as well as the [Rust toolchain](https://rust-lang.org/tools/install/).

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

The [`examples/emscripten_example.cpp`](https://github.com/contentauth/c2pa-cpp/blob/main/examples/emscripten_example.cpp) file demonstrates using the c2pa C++ library compiled to WebAssembly via Emscripten. It includes reading manifests from files, streams, and using a custom HTTP resolver with `emscripten_fetch`.

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

To do so, install (or update) an `llvm` version. For instance, using homebrew:

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

The [API documentation](https://contentauth.github.io/c2pa-cpp/) is built with
Doxygen from selected published stable releases. The selector displays major/minor
lines (`X.Y`); each page displays the exact `vX.Y.Z` patch that supplied its source.
`api-docs/versions.json` retains one highest stable patch per selected line,
sorted by numeric semver descending. The highest line is the default at the
existing root URL; canonical line pages are under
`https://contentauth.github.io/c2pa-cpp/versions/X.Y/`.

#### Preview the versioned site

Install Doxygen if needed:

```sh
macOS: brew install doxygen
Ubuntu/Debian: sudo apt-get install doxygen
```

Install Python 3.10 or newer and the
[GitHub CLI](https://cli.github.com/), authenticate with `gh auth login` (or
set `GH_TOKEN`), and ensure the selected release tags are available locally
with `git fetch --tags`. Then run:

```sh
python3 scripts/generate_versioned_api_docs.py
```

Open `api-docs/_build/versioned/index.html` to preview the default release,
including the version selector.
Line-specific pages are under `api-docs/_build/versioned/versions/X.Y/`.
The build verifies that each configured tag has a published stable GitHub release and
reads that tag's headers, C sources, and README without running release code.
It uses the current branch's Doxygen configuration and styling.

#### Preview unreleased changes

The versioned site uses published release sources, not working-tree API changes.
To preview local changes before release, run:

```sh
./scripts/generate_api_docs.sh
```

Open `api-docs/_build/html/index.html`. This Doxygen-only build uses the working
tree and does not include a version selector or require GitHub authentication.

#### Publishing

`promote-docs.yml` reconciles published stable GitHub releases on release
publication, manual dispatch, and hourly recovery. It verifies release metadata,
updates retained lines only to higher patches, and adds newly released lines
above the highest known active or retired line. It never backfills unselected
historical lines or restores retired lines. Older maintenance releases cannot
regress a patch or move the default. Drafts and prereleases are excluded.

The dependency-free Python updater writes **only `api-docs/versions.json` on
`main`** through the GitHub contents API. Each commit is guarded by the current
file SHA; conflicts trigger a fresh manifest read and full release reconciliation,
up to six attempts, without force pushes. Repeated runs make no empty commits.
Promotion runs have no Actions concurrency group: pending concurrency runs are
not a durable release queue. Full paginated reconciliation and hourly recovery
also catch missed events or exhausted retries. Failed runs remain visible and
can be rerun manually.

After a successful promotion workflow, `publish-docs.yml` explicitly rebuilds
current `main` through `workflow_run`, even if promotion made no change; it does
not rely on a bot push. Main pushes and manual publishing dispatches remain
supported. A single workflow-wide Pages lock serializes build/deploy pairs.
Every admitted run checks out current `main`, rather than the triggering SHA,
so replacing a pending run does not discard release updates. Deployment skips
an artifact if `main` advanced during its build; the pending current-main run
rebuilds it. Builds have read-only contents access and no App credentials;
only the deployment job has Pages and OIDC write permissions. No release
workflow is triggered by manifest commits.

##### GitHub App setup (administrator action)

Use the shared organization documentation GitHub App with **Contents: read and
write** (plus implicit metadata read). Its installation must include
`contentauth/c2pa-cpp`. Expose the organization variable `C2PA_DOCS_APP_ID` and
organization secret `C2PA_DOCS_APP_PRIVATE_KEY` to this repository through their
repository-access policies. The workflow consumes them as
`vars.C2PA_DOCS_APP_ID` and `secrets.C2PA_DOCS_APP_PRIVATE_KEY`; no repository-local
copies are required. Configure the `main` ruleset or
branch protection to allow this App's manifest commits, including any required
pull-request bypass. Organization App provisioning alone does not establish
installation access, credential visibility, or permission to bypass protections;
administrators must verify these separately. The tooling does not change them.
Keep the installation narrowly scoped to the intended documentation repositories:
GitHub does not provide
file-level Contents permissions, so the updater's fixed contents endpoint is
the manifest-only write allowlist.

Promotion mints a short-lived repository-scoped installation token requesting
only Contents write, revokes it at job end, and never passes it to dependency
installation, Doxygen, or deployment. Missing App configuration fails with an
explicit error; there is no ordinary-token fallback. Leave Pages configured
for GitHub Actions and authorize the `github-pages` deployment environment.
Local tests/builds do not verify App installation, ruleset bypass, or live Pages
deployment; confirm those separately after administrator setup.

##### Manifest maintenance and pruning

The manifest has exactly three fields:

- `versions`: non-empty exact stable `vX.Y.Z` tags, one per active line, in
  descending numeric semver order. Index zero is always the default.
- `aliases`: historical exact tag/path identifiers mapped to an active unprefixed
  `X.Y`. Include the selected exact tags too. Identifiers must be single safe URL
  path segments (not reserved canonical `X.Y` names), and exact-tag aliases must
  point to their own line.
- `retiredLines`: unique unprefixed `X.Y` lines disjoint from active lines.

The migration consolidates the previously listed patches without adding
historical lines. It retains all their exact-tag URLs as redirect HTML to the
current patch for that line. Redirects preserve equivalent HTML page paths,
query strings and fragments; removed pages or symbols cannot be guaranteed.
These redirects are compatibility links, not immutable historical snapshots.
Automation preserves existing aliases and adds aliases for newly selected tags.

To prune a line, open a **human-reviewed PR** removing its tag from `versions`
and all aliases targeting that line, and adding the line to `retiredLines`.
Keep at least one active line. Do not remove the retirement marker: automation
must never resurrect it. To explicitly retain an otherwise unselected historical
line, use a human PR adding its highest published stable patch and exact-tag
alias. All builds and promotions validate the contract before proceeding.

Run the documentation tooling tests with:

```sh
python3 -m unittest discover -s tests -p 'test_*docs*.py'
node --test tests/test_version_selector.js
```

The selector tests require Node.js 18 or newer. CI runs these tests and builds
the versioned documentation on pull requests.

## License

This package is distributed under the terms of both the [MIT license](https://github.com/contentauth/c2pa-cpp/blob/main/LICENSE-MIT) and the [Apache License (Version 2.0)](https://github.com/contentauth/c2pa-cpp/blob/main/LICENSE-APACHE).

Note that some components and dependent crates are licensed under different terms; please check the license terms for each crate and component for details.

### Contributions and feedback

We welcome contributions to this project.  For information on contributing, providing feedback, and about ongoing work, see [Contributing](https://github.com/contentauth/c2pa-cpp/blob/main/CONTRIBUTING.md).
