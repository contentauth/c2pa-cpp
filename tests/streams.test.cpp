// Copyright 2026 Adobe. All rights reserved.
// This file is licensed to you under the Apache License,
// Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
// or the MIT license (http://opensource.org/licenses/MIT),
// at your option.

#include <c2pa.hpp>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <type_traits>

static_assert(std::is_base_of_v<c2pa::C2paStream, c2pa::CppIStream>);
static_assert(std::is_base_of_v<c2pa::C2paStream, c2pa::CppOStream>);
static_assert(std::is_base_of_v<c2pa::C2paStream, c2pa::CppIOStream>);
static_assert(std::has_virtual_destructor_v<c2pa::C2paStream>);
static_assert(!std::is_copy_constructible_v<c2pa::C2paStream>);
static_assert(!std::is_move_constructible_v<c2pa::C2paStream>);
static_assert(std::is_same_v<decltype(c2pa::C2paStream::c_stream), ::C2paStream*>);

TEST(StreamWrappers, OwnHandlesThroughCommonBase)
{
    std::istringstream input("input");
    std::ostringstream output;
    std::stringstream bidirectional("both");

    std::unique_ptr<c2pa::C2paStream> wrappers[] = {
        std::make_unique<c2pa::CppIStream>(input),
        std::make_unique<c2pa::CppOStream>(output),
        std::make_unique<c2pa::CppIOStream>(bidirectional)
    };

    for (auto& wrapper : wrappers) {
        ASSERT_NE(wrapper->c_stream, nullptr);
        wrapper.reset();
    }

    input.clear();
    EXPECT_EQ(input.get(), 'i');
    output << "still open";
    EXPECT_EQ(output.str(), "still open");
    EXPECT_EQ(bidirectional.get(), 'b');
}
