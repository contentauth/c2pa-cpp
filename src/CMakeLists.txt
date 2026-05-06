cmake_minimum_required(VERSION 3.27)
project(c2pa_cpp)

include(FetchContent)

# Important note: Building from source has only been tested for Linux systems.
# Windows and macOS users should use prebuilt binaries unless they are comfortable
# troubleshooting potential build issues with Rust toolchains and CMake integration.

# Download json library (used in tests), use 3.11.3 which works with C++17 (3.12+ requires C++20/char8_t)
# This ensures compatibility with older platforms too
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Set DLL export for Windows
add_definitions(-DC2PA_DLL)

# Option to build c2pa_c from source instead of using prebuilt binaries
# Useful for platforms where prebuilt binaries are not compatible
# (eg. older glibc that what prebuilt libs target)
#
# Use this *only* if you understand the implications of building from source
# (and if you have all the needed tools)
option(C2PA_BUILD_FROM_SOURCE "Build c2pa_c Rust library from source instead of using prebuilt" OFF)
set(C2PA_RS_PATH "" CACHE PATH "Path to c2pa-rs repository (required if C2PA_BUILD_FROM_SOURCE is ON)")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(C2PA_PROCESSOR "aarch64")
else()
    set(C2PA_PROCESSOR "x86_64")
endif()

# Detect platform and set library name
if(APPLE)
    set(C2PA_PREBUILT_ARCH "${C2PA_PROCESSOR}-apple-darwin")
    set(C2PA_LIB_NAME "libc2pa_c.dylib")
elseif(WIN32)
    set(C2PA_PREBUILT_ARCH "${C2PA_PROCESSOR}-pc-windows-msvc")
    set(C2PA_LIB_NAME "c2pa_c.dll")
    set(C2PA_IMPLIB_NAME "c2pa_c.dll.lib")

    # Warn about potential ABI issues with MinGW
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(WARNING "Using MinGW with MSVC-compiled DLL may cause ABI compatibility issues. Consider using MSVC compiler on Windows.")
    endif()
else()
    set(C2PA_PREBUILT_ARCH "${C2PA_PROCESSOR}-unknown-linux-gnu")
    set(C2PA_LIB_NAME "libc2pa_c.so")
endif()
set(C2PA_C_LIB "${C2PA_LIB_NAME}" PARENT_SCOPE)
if(WIN32)
    # Export path to C library import library so tests/examples can link (MSVC needs .lib, not .dll)
    set(C2PA_C_IMPLIB "" PARENT_SCOPE)
endif()

if(C2PA_BUILD_FROM_SOURCE)
    # Build from source path
    message(STATUS "Building c2pa_c from source")

    if(NOT C2PA_RS_PATH)
        message(FATAL_ERROR "C2PA_RS_PATH must be set when C2PA_BUILD_FROM_SOURCE is ON")
    endif()

    if(NOT EXISTS "${C2PA_RS_PATH}/c2pa_c_ffi/Cargo.toml")
        message(FATAL_ERROR "c2pa_c_ffi not found at ${C2PA_RS_PATH}/c2pa_c_ffi")
    endif()

    # Set build directory for Rust artifacts
    set(C2PA_RUST_BUILD_DIR "${CMAKE_BINARY_DIR}/c2pa_rust_build")
    file(MAKE_DIRECTORY "${C2PA_RUST_BUILD_DIR}")

    # Find cargo executable
    find_program(CARGO_EXECUTABLE cargo PATHS "$ENV{HOME}/.cargo/bin" REQUIRED)
    message(STATUS "Found cargo at: ${CARGO_EXECUTABLE}")

    # Build the Rust c2pa_c_ffi library
    add_custom_command(
        OUTPUT "${C2PA_RUST_BUILD_DIR}/${C2PA_LIB_NAME}"
        COMMAND ${CARGO_EXECUTABLE} build --release --package c2pa-c-ffi --no-default-features --features "rust_native_crypto, file_io, http, add_thumbnails"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${C2PA_RS_PATH}/target/release/${C2PA_LIB_NAME}"
            "${C2PA_RUST_BUILD_DIR}/${C2PA_LIB_NAME}"
        WORKING_DIRECTORY "${C2PA_RS_PATH}"
        COMMENT "Building c2pa_c from source using Cargo"
        VERBATIM
    )

    # Create a custom target that depends on the library
    add_custom_target(build_c2pa_c ALL
        DEPENDS "${C2PA_RUST_BUILD_DIR}/${C2PA_LIB_NAME}"
    )

    # Copy the generated c2pa.h header
    add_custom_command(
        TARGET build_c2pa_c POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${C2PA_RS_PATH}/target/release/c2pa.h"
            "${C2PA_RUST_BUILD_DIR}/c2pa.h"
        COMMENT "Copying c2pa.h header"
    )

    set(C2PA_C_LIB "${C2PA_RUST_BUILD_DIR}/${C2PA_LIB_NAME}")
    set(C2PA_C_LIB "${C2PA_C_LIB}" PARENT_SCOPE)
    if(WIN32)
        set(C2PA_C_IMPLIB "${C2PA_RUST_BUILD_DIR}/${C2PA_IMPLIB_NAME}")
        set(C2PA_C_IMPLIB "${C2PA_C_IMPLIB}" PARENT_SCOPE)
    endif()
    set(C2PA_INCLUDE_DIR "${C2PA_RUST_BUILD_DIR}")
    set(C2PA_PREBUILT_INCLUDE_DIR "${C2PA_RUST_BUILD_DIR}" PARENT_SCOPE)

    message(STATUS "c2pa_c will be built from source at: ${C2PA_C_LIB}")
else()
    # Use prebuilt binaries (usually the path to use)
    message(STATUS "Using prebuilt c2pa_c binaries")

    set(C2PA_PREBUILT_ZIP "c2pa-v${C2PA_VERSION}-${C2PA_PREBUILT_ARCH}.zip")
    set(C2PA_PREBUILT_URL "https://github.com/contentauth/c2pa-rs/releases/download/c2pa-v${C2PA_VERSION}/${C2PA_PREBUILT_ZIP}")

    message(STATUS "fetching c2pa_prebuilt url: ${C2PA_PREBUILT_URL}")

    FetchContent_Declare(
        c2pa_prebuilt
        URL ${C2PA_PREBUILT_URL}
    )
    FetchContent_MakeAvailable(c2pa_prebuilt)

    set(C2PA_C_LIB "${c2pa_prebuilt_SOURCE_DIR}/lib/${C2PA_LIB_NAME}")
    set(C2PA_C_LIB "${C2PA_C_LIB}" PARENT_SCOPE)
    if(WIN32)
        set(C2PA_C_IMPLIB "${c2pa_prebuilt_SOURCE_DIR}/lib/${C2PA_IMPLIB_NAME}")
        set(C2PA_C_IMPLIB "${C2PA_C_IMPLIB}" PARENT_SCOPE)
    endif()
    set(C2PA_INCLUDE_DIR "${c2pa_prebuilt_SOURCE_DIR}/include")
    set(C2PA_PREBUILT_INCLUDE_DIR "${c2pa_prebuilt_SOURCE_DIR}/include" PARENT_SCOPE)

    # Check if we got our prebuilt library
    if(NOT EXISTS "${C2PA_C_LIB}")
        message(FATAL_ERROR
            "The c2pa prebuilt library was not downloaded or does not exist for this platform/version:\n"
            "  ${C2PA_PREBUILT_URL}\n"
            "Please check https://github.com/contentauth/c2pa-rs/releases for available binaries."
        )
    endif()

    # Fix hardcoded install name on macOS
    if(APPLE)
        find_program(INSTALL_NAME_TOOL install_name_tool REQUIRED)

        # Check current install name before fixing
        execute_process(
            COMMAND otool -D "${C2PA_C_LIB}"
            OUTPUT_VARIABLE CURRENT_INSTALL_NAME
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        message(STATUS "Current install name: ${CURRENT_INSTALL_NAME}")

        # Fix the install name to use @rpath
        execute_process(
            COMMAND ${INSTALL_NAME_TOOL} -id "@rpath/${C2PA_LIB_NAME}" "${C2PA_C_LIB}"
            RESULT_VARIABLE INSTALL_NAME_RESULT
            OUTPUT_QUIET
            ERROR_QUIET
        )

        if(INSTALL_NAME_RESULT EQUAL 0)
            message(STATUS "Successfully fixed install name for ${C2PA_LIB_NAME}")
        else()
            message(WARNING "Failed to fix install name for ${C2PA_LIB_NAME} (result: ${INSTALL_NAME_RESULT})")
        endif()
    endif()
endif()

# -----
# Amalgamation: generate a single, complete, compilable c2pa.cpp at build time.
# Parts are concatenated into one file so downstream users can
# compile it with only c2pa.hpp in the include path.
# -----
set(C2PA_CPP_PARTS
    c2pa_core.cpp
    c2pa_context.cpp
    c2pa_builder.cpp
    c2pa_reader.cpp
    c2pa_settings.cpp
    c2pa_signer.cpp
    c2pa_streams.cpp
)
set(C2PA_AMALGAM_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/c2pa.cpp")
# Path to script in this repo (works when built standalone or via FetchContent)
set(C2PA_AMALGAM_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/amalgamate.cmake")
set(C2PA_AMALGAM_PARTS_FILE "${CMAKE_CURRENT_BINARY_DIR}/amalgam_parts.txt")

# Write part list to a file so the script gets it reliably (avoids command-line list mangling)
list(JOIN C2PA_CPP_PARTS "\n" C2PA_CPP_PARTS_LINES)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
file(WRITE "${C2PA_AMALGAM_PARTS_FILE}" "${C2PA_CPP_PARTS_LINES}\n")

# Regenerate c2pa.cpp when any part or header changes
set(C2PA_AMALGAM_DEPENDS
    ${C2PA_AMALGAM_SCRIPT}
    ${C2PA_AMALGAM_PARTS_FILE}
    ${CMAKE_CURRENT_SOURCE_DIR}/c2pa_internal.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/c2pa.hpp
)
foreach(part ${C2PA_CPP_PARTS})
    list(APPEND C2PA_AMALGAM_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${part}")
endforeach()

add_custom_command(
    OUTPUT "${C2PA_AMALGAM_OUTPUT}"
    COMMAND ${CMAKE_COMMAND}
        -DC2PA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DC2PA_AMALGAM_OUTPUT=${C2PA_AMALGAM_OUTPUT}
        -DC2PA_CPP_PARTS_FILE=${C2PA_AMALGAM_PARTS_FILE}
        -P "${C2PA_AMALGAM_SCRIPT}"
    DEPENDS ${C2PA_AMALGAM_DEPENDS}
    COMMENT "Generating amalgamated c2pa.cpp (complete, compilable single file)"
    VERBATIM
)

# Define the C++ library using the generated unity source (one TU).
add_library(c2pa_cpp SHARED "${C2PA_AMALGAM_OUTPUT}")

# On Windows, export all symbols so MSVC generates the c2pa_cpp.lib
# import library that test/example targets need to link against.
# Without this, MSVC silently skips .lib generation when no __declspec(dllexport)
# annotations are present, causing LNK1104 at link time.
if(WIN32)
    set_target_properties(c2pa_cpp PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# Set compiler-specific flags for the C++ library
if(MSVC)
    target_compile_options(c2pa_cpp PRIVATE
        /WX
        /sdl
        /guard:cf
        /GS
        /DYNAMICBASE
        /wd4996)

    target_link_options(c2pa_cpp PRIVATE
        /DYNAMICBASE
        /GUARD:CF
    )
else()
    target_compile_options(c2pa_cpp PRIVATE
        -Werror
        -Wformat=2
        -Wformat-security
        -Wnull-dereference
        -Wstack-protector
        -fstack-protector-strong
        -fPIC
        -Wno-deprecated-declarations
    )

    if(APPLE)
        target_link_options(c2pa_cpp PRIVATE
            -Wl,-bind_at_load
        )
    elseif(UNIX)
        target_compile_options(c2pa_cpp PRIVATE -D_FORTIFY_SOURCE=2)
        # Linux-only hardening flags
        target_link_options(c2pa_cpp PRIVATE
            -Wl,-z,relro
            -Wl,-z,now
        )
    endif()

    # Some compiler versions require explicit linking with stdc++fs for std::filesystem...
    # This ensures compatibility if built with older compiler versions
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
        target_link_libraries(c2pa_cpp PUBLIC stdc++fs)
        message(STATUS "Linking with stdc++fs for filesystem support explicitly")
    endif()
endif()

# Expose public headers and C API headers to clients
target_include_directories(c2pa_cpp
    PUBLIC
        $<BUILD_INTERFACE:${c2pa-c_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${C2PA_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        # So generated c2pa.cpp can find part files (c2pa_core.cpp etc.)
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# Add dependency on build_c2pa_c if building from source
if(C2PA_BUILD_FROM_SOURCE)
    add_dependencies(c2pa_cpp build_c2pa_c)
endif()

# Link the C++ library to the C library.
# C2PA_C_LIB_OVERRIDE lets a parent project substitute its own library that already
# embeds the c2pa_c symbols.
# Without this, two independent copies of the tracked-pointer registry can exist in
# the same process, causing UntrackedPointer errors when a pointer allocated through
# one registry is freed or accessed through the other.
if(DEFINED C2PA_C_LIB_OVERRIDE)
    target_link_libraries(c2pa_cpp PRIVATE "${C2PA_C_LIB_OVERRIDE}")
elseif(WIN32)
    # On Windows, link to the import library (.lib file)
    if(C2PA_BUILD_FROM_SOURCE)
        target_link_libraries(c2pa_cpp PRIVATE "${C2PA_RUST_BUILD_DIR}/${C2PA_IMPLIB_NAME}")
    else()
        target_link_libraries(c2pa_cpp PRIVATE "${c2pa_prebuilt_SOURCE_DIR}/lib/${C2PA_IMPLIB_NAME}")
    endif()
else()
    # On Unix systems, link directly to the shared library
    target_link_libraries(c2pa_cpp PRIVATE "${C2PA_C_LIB}")
endif()
target_include_directories(c2pa_cpp PUBLIC
    $<BUILD_INTERFACE:${C2PA_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# Optionally, install rules for consumers
install(TARGETS c2pa_cpp
    EXPORT c2pa_cppTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES ${c2pa-c_SOURCE_DIR}/include/c2pa.hpp DESTINATION include)

# Install the export set (for build-tree and install-tree usage) - added from main
install(EXPORT c2pa_cppTargets
    FILE c2pa_cppTargets.cmake
    NAMESPACE c2pa_cpp::
    DESTINATION lib/cmake/c2pa_cpp
)

# Install headers and libraries
install(DIRECTORY "${C2PA_INCLUDE_DIR}/"
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)
if(APPLE)
    install(FILES "${C2PA_C_LIB}" DESTINATION lib)
elseif(UNIX)
    install(FILES "${C2PA_C_LIB}" DESTINATION lib)
endif()
if(WIN32)
    install(FILES
        "${C2PA_C_LIB}"
        DESTINATION bin
    )
    if(C2PA_BUILD_FROM_SOURCE)
        install(FILES
            "${C2PA_RUST_BUILD_DIR}/${C2PA_IMPLIB_NAME}"
            DESTINATION lib
        )
    else()
        install(FILES
            "${c2pa_prebuilt_SOURCE_DIR}/lib/${C2PA_IMPLIB_NAME}"
            DESTINATION lib
        )
    endif()
endif()
