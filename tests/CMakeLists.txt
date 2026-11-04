cmake_minimum_required(VERSION 3.16)

# GoogleTest via FetchContent
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

# Platform-specific configuration variables
# Environment variables for test runtime
set(LSAN_SUPPRESSIONS_FILE "${c2pa-c_SOURCE_DIR}/ci-cd/lsan_suppressions.txt")

if(APPLE)
    set(TEST_LIBRARY_PATH_VAR "DYLD_LIBRARY_PATH")
    set(TEST_LIBRARY_PATH_VALUE "${c2pa-c_BINARY_DIR}/tests:$ENV{DYLD_LIBRARY_PATH}")
elseif(UNIX)
    set(TEST_LIBRARY_PATH_VAR "LD_LIBRARY_PATH")
    set(TEST_LIBRARY_PATH_VALUE "${c2pa-c_BINARY_DIR}/tests:$ENV{LD_LIBRARY_PATH};LSAN_OPTIONS=suppressions=${LSAN_SUPPRESSIONS_FILE}")
endif()

# Utility functions
# Function to configure platform-specific test environment
function(setup_test_environment test_name working_dir)
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${working_dir}"
    )

    # Set platform-specific library path environment
    if(DEFINED TEST_LIBRARY_PATH_VAR)
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT "${TEST_LIBRARY_PATH_VAR}=${TEST_LIBRARY_PATH_VALUE}"
        )
    endif()
endfunction()

# Function to setup runtime dependencies for any target
function(setup_c2pa_runtime_deps target_name)
    # Copy C shared library to executable directory on all platforms
    if(DEFINED C2PA_C_LIB)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${C2PA_C_LIB}"
            $<TARGET_FILE_DIR:${target_name}>
            COMMAND ${CMAKE_COMMAND} -E echo "Copied ${C2PA_C_LIB} to $<TARGET_FILE_DIR:${target_name}>"
            COMMENT "Copying c2pa_c library to ${target_name} directory"
        )

        # On Windows, copy all DLLs from the prebuilt lib directory
        if(WIN32)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "=== Windows DLL Debug Info for ${target_name} ==="
                COMMAND ${CMAKE_COMMAND} -E echo "Listing prebuilt lib directory contents:"
                COMMAND ${CMAKE_COMMAND} -E echo "Skipping directory listing due to Windows path issues"
                COMMAND ${CMAKE_COMMAND} -E echo "Target directory contents:"
                COMMAND cmd /c "dir \"$<TARGET_FILE_DIR:${target_name}>\"" || echo "Failed to list target directory"
                COMMENT "Windows DLL debugging for ${target_name}"
            )
        else()
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ls -la $<TARGET_FILE_DIR:${target_name}> || echo "Failed to list directory"
            )
        endif()
    endif()

    # When c2pa_cpp is SHARED, copy it next to the executable so loader finds it (same as consumers)
    if(TARGET c2pa_cpp)
        get_target_property(c2pa_cpp_type c2pa_cpp TYPE)
        if(c2pa_cpp_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:c2pa_cpp>
                    $<TARGET_FILE_DIR:${target_name}>
                COMMENT "Copying c2pa_cpp library to ${target_name} directory"
            )
        endif()
    endif()

    # On Unix systems (Linux/macOS), set RPATH to look in the executable's directory
    if(UNIX)
        # Platform-specific RPATH syntax
        if(APPLE)
            set(RPATH_VALUE "@executable_path")
        else()
            set(RPATH_VALUE "$ORIGIN")
        endif()

        set_target_properties(${target_name} PROPERTIES
            BUILD_RPATH "${RPATH_VALUE}"
            INSTALL_RPATH "${RPATH_VALUE}"
            BUILD_WITH_INSTALL_RPATH TRUE
            SKIP_BUILD_RPATH FALSE
        )

        # On Linux, clear any automatic RPATH from linked libraries
        if(NOT APPLE)
            set_target_properties(${target_name} PROPERTIES
                LINK_WHAT_YOU_USE FALSE
            )
        endif()

        # Force override any inherited RPATH
        set_property(TARGET ${target_name} PROPERTY BUILD_RPATH "${RPATH_VALUE}")
        set_property(TARGET ${target_name} PROPERTY INSTALL_RPATH "${RPATH_VALUE}")
    endif()
endfunction()

# C++ tests (link to shared c2pa_cpp + C lib so we test the same deployment as consumers)
file(GLOB CPP_TESTS "*.test.cpp")
add_executable(c2pa_c_tests ${CPP_TESTS})
# On Windows link to the C library import library (.lib); on Unix link to the shared lib
if(WIN32)
    target_link_libraries(c2pa_c_tests PRIVATE c2pa_cpp gtest_main "${C2PA_C_IMPLIB}")
else()
    target_link_libraries(c2pa_c_tests PRIVATE c2pa_cpp gtest_main "${C2PA_C_LIB}")
endif()
target_include_directories(c2pa_c_tests PRIVATE
    ${nlohmann_json_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(NOT MSVC)
    target_compile_options(c2pa_c_tests PRIVATE -Wno-deprecated-declarations)
endif()
if(MSVC)
    target_compile_options(c2pa_c_tests PRIVATE /wd4996)
endif()
setup_c2pa_runtime_deps(c2pa_c_tests)

include(GoogleTest)

# Add debug commands to check what's happening on Linux
if(UNIX AND NOT APPLE)
    add_test(NAME debug_rpath_info
        COMMAND bash -c "echo '=== RPATH Debug Info ===' && echo 'Working dir:' && pwd && echo 'Library files in tests dir:' && ls -la ${c2pa-c_BINARY_DIR}/tests/ && echo 'RPATH of c2pa_c_tests:' && readelf -d ${c2pa-c_BINARY_DIR}/tests/c2pa_c_tests | grep -E '(RPATH|RUNPATH)' && echo 'RPATH of ctest:' && readelf -d ${c2pa-c_BINARY_DIR}/tests/ctest | grep -E '(RPATH|RUNPATH)' && echo 'ldd output for c2pa_c_tests:' && ldd ${c2pa-c_BINARY_DIR}/tests/c2pa_c_tests"
    )

    if(NOT C2PA_BUILD_FROM_SOURCE)
        add_test(NAME debug_library_location
            COMMAND bash -c "echo '=== Library Location Debug ===' && echo 'Current dir:' && pwd && echo 'Contents of build/debug:' && ls -la ${CMAKE_BINARY_DIR}/ && echo 'Contents of _deps:' && ls -la ${CMAKE_BINARY_DIR}/_deps/ && echo 'Contents of prebuilt lib:' && ls -la ${CMAKE_BINARY_DIR}/_deps/c2pa_prebuilt-src/lib/"
        )
    endif()
endif()

# For better reliability, especially on Linux, manually add tests instead of auto-discovery
# This avoids the working directory and library path issues during discovery
if(WIN32)
    # On Windows, just run the test executable directly - CTest handles it properly
    add_test(NAME cpp_tests COMMAND c2pa_c_tests.exe)
else()
    add_test(NAME cpp_tests COMMAND c2pa_c_tests)
endif()

# Set platform-specific environment and working directory
setup_test_environment(cpp_tests "${c2pa-c_BINARY_DIR}/tests")

# C tests
add_executable(ctest c-app-test/test.c)

# If building from source, ensure ctest waits for the Rust build to complete
if(C2PA_BUILD_FROM_SOURCE AND TARGET build_c2pa_c)
    add_dependencies(ctest build_c2pa_c)
endif()

if(WIN32)
    # On Windows, link to the import library (.lib file)
    target_link_libraries(ctest PRIVATE "${C2PA_C_IMPLIB}")

    # Add MSVC-specific compile options for C code to handle prebuilt library issues
    if(MSVC)
        # The prebuilt library has empty structs which are invalid in C but valid in C++
        # Compile as C++ to work around this prebuilt library limitation
        set_target_properties(ctest PROPERTIES
            COMPILE_OPTIONS "/TP"  # Force compile as C++
            CXX_STANDARD 20       # Use C++20 for designated initializers
            CXX_STANDARD_REQUIRED ON
        )
        target_compile_options(ctest PRIVATE
            /wd4996  # Disable deprecated function warnings (strerror, strncpy, etc.)
            /wd4244  # Disable type conversion warnings
            /wd4267  # Disable size_t conversion warnings
        )
    endif()
else()
    # On Unix systems, link directly to the shared library
    target_link_libraries(ctest PRIVATE "${C2PA_C_LIB}")
endif()
if(NOT MSVC)
    target_compile_options(ctest PRIVATE -Wno-deprecated-declarations)
endif()
target_include_directories(ctest PRIVATE
    "${C2PA_PREBUILT_INCLUDE_DIR}"
    ${CMAKE_CURRENT_SOURCE_DIR}/c-app-test
)
setup_c2pa_runtime_deps(ctest)

# Register tests with CTest
add_test(NAME c_test COMMAND ctest)
# Set platform-specific environment and working directory
setup_test_environment(c_test "${c2pa-c_SOURCE_DIR}")
