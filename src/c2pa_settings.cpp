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

/// @file   c2pa_settings.cpp
/// @brief  Settings configuration object implementation.

#include <utility>

#include "c2pa.hpp"
#include "c2pa_internal.hpp"

namespace c2pa
{
    // Settings Implementation

    Settings::Settings() : settings_ptr(c2pa_settings_new()) {
        if (!settings_ptr) {
            throw C2paException("Failed to create Settings");
        }
    }

    Settings::Settings(const std::string& data, const std::string& format) : settings_ptr(c2pa_settings_new()) {
        if (!settings_ptr) {
            throw C2paException("Failed to create Settings");
        }
        if (c2pa_settings_update_from_string(settings_ptr, data.c_str(), format.c_str()) != 0) {
            c2pa_free(settings_ptr);
            settings_ptr = nullptr;
            throw C2paException();
        }
    }

    Settings::Settings(Settings&& other) noexcept
        : settings_ptr(std::exchange(other.settings_ptr, nullptr)) {
    }

    Settings& Settings::operator=(Settings&& other) noexcept {
        if (this != &other) {
            c2pa_free(settings_ptr);
            settings_ptr = std::exchange(other.settings_ptr, nullptr);
        }
        return *this;
    }

    Settings::~Settings() noexcept {
        c2pa_free(settings_ptr);
    }

    bool Settings::is_valid() const noexcept {
        return settings_ptr != nullptr;
    }

    Settings& Settings::set(const std::string& path, const std::string& json_value) {
        if (!settings_ptr) {
            throw C2paException("Settings object is invalid");
        }
        if (c2pa_settings_set_value(settings_ptr, path.c_str(), json_value.c_str()) != 0) {
            throw C2paException();
        }
        return *this;
    }

    Settings& Settings::update(const std::string& data, const std::string& format) {
        if (!settings_ptr) {
            throw C2paException("Settings object is invalid");
        }
        if (c2pa_settings_update_from_string(settings_ptr, data.c_str(), format.c_str()) != 0) {
            throw C2paException();
        }
        return *this;
    }

    C2paSettings* Settings::c_settings() const noexcept {
        return settings_ptr;
    }
} // namespace c2pa
