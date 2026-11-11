// Copyright 2024 Adobe. All rights reserved.
// This file is licensed to you under the Apache License,
// Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
// or the MIT license (http://opensource.org/licenses/MIT),
// at your option.
// Unless required by applicable law or agreed to in writing,
// this software is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR REPRESENTATIONS OF ANY KIND, either express or
// implied. See the LICENSE-MIT and LICENSE-APACHE files for the
// specific language governing permissions and limitations under
// each license.

/// @file   c2pa_core.cpp
/// @brief  Core C2PA exception class and free functions.

#include <string.h>
#include <utility>

#include "c2pa.hpp"
#include "c2pa_internal.hpp"

namespace c2pa
{
    /// C2paException class for C2PA errors.
    /// This class is used to throw exceptions for errors encountered by the C2PA library via c2pa_error().

    C2paException::C2paException()
        : message_([]{
            auto result = c2pa_error();
            std::string msg = result ? std::string(result) : std::string();
            c2pa_free(result);
            return msg;
        }())
    {
    }

    C2paException::C2paException(std::string message) : message_(std::move(message))
    {
    }

    const char* C2paException::what() const noexcept
    {
        return message_.c_str();
    }

    /// Returns the version of the C2PA library.
    std::string version()
    {
        return detail::c_string_to_string(c2pa_version());
    }

} // namespace c2pa
