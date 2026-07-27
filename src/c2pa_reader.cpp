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

/// @file   c2pa_reader.cpp
/// @brief  Reader class implementation.

#include <system_error>

#include "c2pa.hpp"
#include "c2pa_internal.hpp"

namespace {

template <typename F>
std::optional<c2pa::Reader> reader_from_asset_impl(F&& construct_reader) {
    try {
        return construct_reader();
    } catch (const c2pa::C2paException& e) {
        if (c2pa::detail::error_indicates_manifest_not_found(e.what())) {
            return std::nullopt;
        }
        throw;
    }
}

} // namespace

namespace c2pa
{
    /// Reader class for reading manifests

    // Shared initialization from any IContextProvider, used by both the
    // overloads, so neither calls the other.
    void Reader::init_from_context(IContextProvider& context, const std::string &format, std::istream &stream)
    {
        if (!context.is_valid()) {
            throw C2paException("Invalid Context provider IContextProvider");
        }

        // Validate before acquiring anything.
        const std::string resolved = detail::resolve_reader_format(format);

        // Create the stream wrapper before the reader handle
        cpp_stream = std::make_unique<CppIStream>(stream);

        c2pa_reader = c2pa_reader_from_context(context.c_context());
        if (c2pa_reader == nullptr) {
            throw C2paException("Failed to create reader from context");
        }

        // Update reader with stream.
        // Note: c2pa_reader_with_stream consumes the reader pointer.
        C2paReader* updated = c2pa_reader_with_stream(c2pa_reader, resolved.c_str(), cpp_stream->c_stream);
        c2pa_reader = nullptr;
        if (updated == nullptr) {
            throw C2paException();
        }
        c2pa_reader = updated;
    }

    // Shared initialization from any IContextProvider, used by both the
    // overloads, so neither calls the other.
    void Reader::init_from_context(IContextProvider& context, const std::filesystem::path &source_path)
    {
        if (!context.is_valid()) {
            throw C2paException("Invalid Context provider IContextProvider");
        }

        // Create the streams before the reader handle.
        // Create owned stream that will live as long as the Reader.
        owned_stream = std::make_unique<std::ifstream>(source_path, std::ios::binary);
        if (!owned_stream->is_open()) {
            throw std::system_error(errno, std::system_category(), "Failed to open file: " + source_path.string());
        }

        // The extension describes the filename, so it is normalized and left unchecked.
        // Content type detection overrides it anyway if wrong.
        std::string extension = detail::normalize_derived_extension(detail::extract_file_extension(source_path));

        // CppIStream stores reference to owned_stream, which lives as long as Reader
        cpp_stream = std::make_unique<CppIStream>(*owned_stream);

        c2pa_reader = c2pa_reader_from_context(context.c_context());
        if (c2pa_reader == nullptr) {
            throw C2paException("Failed to create reader from context");
        }

        // Note: c2pa_reader_with_stream consumes the reader pointer.
        C2paReader* updated = c2pa_reader_with_stream(c2pa_reader, extension.c_str(), cpp_stream->c_stream);
        c2pa_reader = nullptr;
        if (updated == nullptr) {
            throw C2paException();
        }
        c2pa_reader = updated;
    }

    void Reader::init_from_manifest_data_and_stream(
        IContextProvider& context,
        const std::string& format,
        std::istream& image_stream,
        const std::vector<uint8_t>& manifest_jumbf)
    {
        if (!context.is_valid()) {
            throw C2paException("Invalid Context provider IContextProvider");
        }
        if (manifest_jumbf.empty()) {
            throw C2paException("manifest_jumbf must not be empty");
        }

        // The container type decides how the asset is hashed, so it is required.
        const std::string resolved = detail::require_explicit_format(format);

        cpp_stream = std::make_unique<CppIStream>(image_stream);

        c2pa_reader = c2pa_reader_from_context(context.c_context());
        if (c2pa_reader == nullptr) {
            throw C2paException("Failed to create reader from context");
        }

        // c2pa_reader_with_manifest_data_and_stream always consumes c2pa_reader.
        C2paReader* updated = c2pa_reader_with_manifest_data_and_stream(
            c2pa_reader,
            resolved.c_str(),
            cpp_stream->c_stream,
            manifest_jumbf.data(),
            manifest_jumbf.size());
        c2pa_reader = nullptr;
        if (updated == nullptr) {
            throw C2paException();
        }
        c2pa_reader = updated;

        // Stream not retained by C FFI
        cpp_stream.reset();
    }

    Reader::Reader(IContextProvider& context, const std::string &format, std::istream &stream)
        : c2pa_reader(nullptr)
    {
        init_from_context(context, format, stream);
    }

    Reader::Reader(IContextProvider& context, const std::filesystem::path &source_path)
        : c2pa_reader(nullptr)
    {
        init_from_context(context, source_path);
    }

    Reader::Reader(std::shared_ptr<IContextProvider> context, const std::string &format, std::istream &stream)
        : c2pa_reader(nullptr)
    {
        if (!context) {
            throw C2paException("context must not be null");
        }
        init_from_context(*context, format, stream);
        context_ref = std::move(context);
    }

    Reader::Reader(std::shared_ptr<IContextProvider> context, std::istream &stream)
        : Reader(std::move(context), detail::kDetectFormatFromContent, stream)
    {
    }

    Reader::Reader(std::shared_ptr<IContextProvider> context, const std::filesystem::path &source_path)
        : c2pa_reader(nullptr)
    {
        if (!context) {
            throw C2paException("context must not be null");
        }
        init_from_context(*context, source_path);
        context_ref = std::move(context);
    }

    Reader::Reader(std::shared_ptr<IContextProvider> context,
                   const std::string& format,
                   std::istream& image_stream,
                   const std::vector<uint8_t>& manifest_jumbf)
        : c2pa_reader(nullptr)
    {
        if (!context) {
            throw C2paException("context must not be null");
        }
        init_from_manifest_data_and_stream(*context, format, image_stream, manifest_jumbf);
        context_ref = std::move(context);
    }

    Reader& Reader::with_fragment(const std::string& format, std::istream& stream, std::istream& fragment)
    {
        ensure_initialized();

        const std::string resolved = detail::require_explicit_format(format);

        CppIStream main_wrapper(stream);
        CppIStream fragment_wrapper(fragment);

        // c2pa_reader_with_fragment consumes the existing reader and returns a new one.
        // *this is returned for chaining so reading can go through all segments.
        C2paReader* updated = c2pa_reader_with_fragment(
            c2pa_reader,
            resolved.c_str(),
            main_wrapper.c_stream,
            fragment_wrapper.c_stream);
        c2pa_reader = nullptr;
        if (updated == nullptr) {
            throw C2paException();
        }
        c2pa_reader = updated;

        return *this;
    }

    Reader::Reader(const std::string &format, std::istream &stream)
    {
        const std::string resolved = detail::resolve_reader_format(format);

        cpp_stream = std::make_unique<CppIStream>(stream);
        c2pa_reader = c2pa_reader_from_stream(resolved.c_str(), cpp_stream->c_stream);
        if (c2pa_reader == nullptr)
        {
            throw C2paException();
        }
    }

    Reader::Reader(const std::filesystem::path &source_path)
    {
        // Create owned stream that will live as long as the Reader
        owned_stream = std::make_unique<std::ifstream>(source_path, std::ios::binary);
        if (!owned_stream->is_open()) {
            throw std::system_error(errno, std::system_category(), "Failed to open file: " + source_path.string());
        }

        std::string extension = detail::normalize_derived_extension(detail::extract_file_extension(source_path));

        // CppIStream stores reference to owned_stream, which lives as long as Reader
        cpp_stream = std::make_unique<CppIStream>(*owned_stream);
        c2pa_reader = c2pa_reader_from_stream(extension.c_str(), cpp_stream->c_stream);
        if (c2pa_reader == nullptr)
        {
            throw C2paException();
        }
    }

    Reader::~Reader()
    {
        c2pa_free(c2pa_reader);
        // cpp_stream and owned_stream are cleaned up by unique_ptr
    }

    std::string Reader::json() const
    {
        ensure_initialized();
        return detail::c_string_to_string(c2pa_reader_json(c2pa_reader));
    }

    std::string Reader::detailed_json() const
    {
        ensure_initialized();
        return detail::c_string_to_string(c2pa_reader_detailed_json(c2pa_reader));
    }

    std::string Reader::crjson() const
    {
        ensure_initialized();
        return detail::c_string_to_string(c2pa_reader_crjson(c2pa_reader));
    }

    [[nodiscard]] std::optional<std::string> Reader::remote_url() const {
        auto url = c2pa_reader_remote_url(c2pa_reader);
        if (url == nullptr) { return std::nullopt; }
        // The C2PA library returns a `const char*` that needs to be released.
        // The underlying `char*` is mutable; however, to indicate the value
        // shouldn't be modified, it's returned as a const char*.
        //
        // TODO: Revisit after determining how we want c2pa-rs to handle
        //       strings that shouldn't be modified by our bindings.
        try {
            std::string url_str(url);
            c2pa_free(url);
            return url_str;
        } catch (...) {
            c2pa_free(url);
            throw;
        }
    }

    int64_t Reader::get_resource(const std::string &uri, const std::filesystem::path &path)
    {
        auto file_stream = detail::open_file_binary<std::ofstream>(path);
        return get_resource(uri.c_str(), *file_stream);
    }

    int64_t Reader::get_resource(const std::string &uri, std::ostream &stream)
    {
        CppOStream output_stream(stream);
        int64_t result = c2pa_reader_resource_to_stream(c2pa_reader, uri.c_str(), output_stream.c_stream);
        if (result < 0)
        {
            throw C2paException();
        }
        return result;
    }

    std::vector<std::string> Reader::supported_mime_types() {
      return detail::supported_reader_formats();
    }

    std::optional<Reader> Reader::from_asset(IContextProvider& context, const std::filesystem::path& source_path) {
        return reader_from_asset_impl([&]() {
            Reader r;
            r.init_from_context(context, source_path);
            return r;
        });
    }

    std::optional<Reader> Reader::from_asset(IContextProvider& context, const std::string& format, std::istream& stream) {
        return reader_from_asset_impl([&]() {
            Reader r;
            r.init_from_context(context, format, stream);
            return r;
        });
    }

    std::optional<Reader> Reader::from_asset(std::shared_ptr<IContextProvider> context, const std::filesystem::path& source_path) {
        return reader_from_asset_impl([&]() { return Reader(std::move(context), source_path); });
    }

    std::optional<Reader> Reader::from_asset(std::shared_ptr<IContextProvider> context, const std::string& format, std::istream& stream) {
        return reader_from_asset_impl([&]() { return Reader(std::move(context), format, stream); });
    }

    std::optional<Reader> Reader::from_asset(std::shared_ptr<IContextProvider> context, std::istream& stream) {
        return reader_from_asset_impl([&]() {
            return Reader(std::move(context), detail::kDetectFormatFromContent, stream);
        });
    }
} // namespace c2pa
