# Copyright 2024 Adobe. All rights reserved.
# This file is licensed to you under the Apache License,
# Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
# or the MIT license (http://opensource.org/licenses/MIT),
# at your option.
#
# Unless required by applicable law or agreed to in writing,
# this software is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR REPRESENTATIONS OF ANY KIND, either express or
# implied. See the LICENSE-MIT and LICENSE-APACHE files for the
# specific language governing permissions and limitations under
# each license.

cmake_minimum_required(VERSION 3.27)

# This is the current version of this C++ project
project(c2pa-c VERSION 0.24.0)

# Set the version of the c2pa_rs library used
set(C2PA_VERSION "0.87.0")

set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3")

if(MSVC)
add_compile_options(/utf-8)
endif()

# Set RPATH settings for shared library discovery
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
# Disable automatic RPATH from link paths since we copy libraries to executable directories
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)

# Note: We don't set CMAKE_BUILD_RPATH to the prebuilt directory
# because we copy libraries to each executable's directory instead

# Set compiler-specific warning flags
if(MSVC)
    # MSVC warning flags
    add_compile_options(/W4 /WX)
else()
    # GCC/Clang warning flags
    add_compile_options(-Wall -Wextra -Werror)
endif()

# Sanitizers (for debug/verification builds)
option(ENABLE_SANITIZERS "Enable AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer" OFF)
option(ENABLE_MSAN "Enable MemorySanitizer (mutually exclusive with ASAN)" OFF)

if(ENABLE_SANITIZERS AND ENABLE_MSAN)
    message(FATAL_ERROR "ENABLE_SANITIZERS and ENABLE_MSAN are mutually exclusive - choose one")
endif()

if(ENABLE_SANITIZERS)
    if(MSVC)
        message(WARNING "Sanitizers are not supported with MSVC in this configuration")
    elseif(WIN32)
        # MinGW on Windows doesn't have sanitizer libraries available
        message(WARNING "Sanitizers are not available on Windows with MinGW - skipping sanitizer build")
    else()
        # AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer for GCC/Clang
        # Note: LeakSanitizer is not supported on macOS (both x86_64 and ARM64)
        # On Linux, LeakSanitizer is integrated with AddressSanitizer
        if(APPLE)
            add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
            add_link_options(-fsanitize=address,undefined)
            message(STATUS "Sanitizers enabled on macOS: ASAN and UBSAN (LSan not supported on macOS)")
        else()
            add_compile_options(-fsanitize=address,undefined,leak -fno-omit-frame-pointer -g)
            add_link_options(-fsanitize=address,undefined,leak)
            message(STATUS "Sanitizers enabled: ASAN, UBSAN, and LSan")
        endif()
        # Allow macro redefinition for _FORTIFY_SOURCE which conflicts with sanitizers
        add_compile_options(-Wno-macro-redefined)
    endif()
endif()

if(ENABLE_MSAN)
    if(NOT MSVC)
        # MemorySanitizer for detecting uninitialized memory reads
        # WARNING: Requires all dependencies to be built with MSan
        add_compile_options(-fsanitize=memory -fno-omit-frame-pointer -g)
        add_compile_options(-Wno-macro-redefined)
        add_link_options(-fsanitize=memory)
        message(STATUS "MemorySanitizer enabled (MSan)")
    else()
        message(WARNING "MemorySanitizer is not supported with MSVC")
    endif()
endif()

# Code coverage instrumentation
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)

if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
        add_link_options(--coverage)
        message(STATUS "Code coverage enabled")
    else()
        message(WARNING "Code coverage is only supported with GCC or Clang")
    endif()
endif()

enable_testing()

ADD_SUBDIRECTORY(src)
ADD_SUBDIRECTORY(tests)
ADD_SUBDIRECTORY(examples)
