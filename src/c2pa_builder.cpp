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

/// @file   c2pa_builder.cpp
/// @brief  Builder class implementation.

#include <filesystem>
#include <fstream>

#include "c2pa.hpp"
#include "c2pa_internal.hpp"

namespace c2pa
{
    Builder::Builder(C2paBuilder *builder)
        : builder(builder)
    {
        if (this->builder == nullptr)
        {
            throw C2paException("Invalid builder pointer");
        }
    }

    // Shared initialization from any IContextProvider, used by both the
    // overloads, so neither calls the other.
    void Builder::init_from_context(IContextProvider& context)
    {
        if (!context.is_valid()) {
            throw C2paException("Invalid Context provider IContextProvider");
        }

        builder = c2pa_builder_from_context(context.c_context());
        if (builder == nullptr) {
            throw C2paException("Failed to create builder from context");
        }
    }

    // Shared initialization from any IContextProvider, used by both the
    // overloads, so neither calls the other.
    void Builder::init_from_context(IContextProvider& context, const std::string &manifest_json)
    {
        init_from_context(context);

        // Apply the manifest definition to the Builder.
        // Note: c2pa_builder_with_definition always consumes the builder pointer,
        // so the original pointer is invalid after this call regardless of success/error.
        C2paBuilder* updated = c2pa_builder_with_definition(builder, manifest_json.c_str());
        builder = nullptr;
        if (updated == nullptr) {
            throw C2paException();
        }
        builder = updated;
    }

    Builder::Builder(IContextProvider& context)
        : builder(nullptr)
    {
        init_from_context(context);
    }

    Builder::Builder(IContextProvider& context, const std::string &manifest_json)
        : builder(nullptr)
    {
        init_from_context(context, manifest_json);
    }

    Builder::Builder(std::shared_ptr<IContextProvider> context)
        : builder(nullptr)
    {
        init_from_context(*context);
        context_ref = std::move(context);
    }

    Builder::Builder(std::shared_ptr<IContextProvider> context, const std::string &manifest_json)
        : builder(nullptr)
    {
        init_from_context(*context, manifest_json);
        context_ref = std::move(context);
    }

    Builder::Builder(const std::string &manifest_json)
        : builder(nullptr)
    {
        builder = c2pa_builder_from_json(manifest_json.c_str());
        if (builder == nullptr)
        {
            throw C2paException();
        }
    }

    /// @brief Create a Builder from an archive.
    /// @param archive  The input stream to read the archive from.
    /// @throws C2pa::C2paException for errors encountered by the C2PA library.
    Builder::Builder(std::istream &archive)
        : builder(nullptr)
    {
        CppIStream c_archive(archive);

        C2paContext* ctx = c2pa_context_new();
        if (ctx == nullptr) {
            throw C2paException();
        }

        C2paBuilder* base = c2pa_builder_from_context(ctx);
        c2pa_free(ctx);
        if (base == nullptr) {
            throw C2paException();
        }

        builder = c2pa_builder_with_archive(base, c_archive.c_stream);
        if (builder == nullptr) {
            throw C2paException();
        }
    }

    Builder::~Builder()
    {
        c2pa_free(builder);
    }

    C2paBuilder *Builder::c2pa_builder() const noexcept
    {
        return builder;
    }

    void Builder::set_no_embed()
    {
        c2pa_builder_set_no_embed(builder);
    }

    void Builder::set_remote_url(const std::string &remote_url)
    {
        int result = c2pa_builder_set_remote_url(builder, remote_url.c_str());
        if (result < 0)
        {
            throw C2paException();
        }
    }

    Builder& Builder::with_definition(const std::string &manifest_json)
    {
        // c2pa_builder_with_definition consumes the builder pointer,
        // so the original pointer is invalid after the call.
        C2paBuilder* updated = c2pa_builder_with_definition(builder, manifest_json.c_str());
        builder = nullptr;
        if (updated == nullptr) {
            throw C2paException("Failed to set builder definition");
        }
        builder = updated;
        return *this;
    }

    void Builder::set_base_path(const std::string &base_path)
    {
        int result = c2pa_builder_set_base_path(builder, base_path.c_str());
        if (result < 0)
        {
            throw C2paException();
        }
    }

    void Builder::add_resource(const std::string &uri, std::istream &source)
    {
        CppIStream c_source(source);
        int result = c2pa_builder_add_resource(builder, uri.c_str(), c_source.c_stream);
        if (result < 0)
        {
            throw C2paException();
        }
    }

    void Builder::add_resource(const std::string &uri, const std::filesystem::path &source_path)
    {
        auto stream = detail::open_file_binary<std::ifstream>(source_path);
        add_resource(uri, *stream);
    }

    void Builder::add_ingredient(const std::string &ingredient_json, const std::string &format, std::istream &source)
    {
        CppIStream c_source(source);
        int result = c2pa_builder_add_ingredient_from_stream(builder, ingredient_json.c_str(), format.c_str(), c_source.c_stream);
        if (result < 0)
        {
            throw C2paException();
        }
    }

    void Builder::add_ingredient(const std::string &ingredient_json, const std::filesystem::path &source_path)
    {
        auto stream = detail::open_file_binary<std::ifstream>(source_path);
        auto format = detail::extract_file_extension(source_path);
        add_ingredient(ingredient_json, format, *stream);
    }

    void Builder::add_action(const std::string &action_json)
    {
        int result = c2pa_builder_add_action(builder, action_json.c_str());
        if (result < 0)
        {
            throw C2paException();
        }
    }

    void Builder::set_intent(C2paBuilderIntent intent, C2paDigitalSourceType digital_source_type)
    {
        int result = c2pa_builder_set_intent(builder, intent, digital_source_type);
        if (result < 0)
        {
            throw C2paException();
        }
    }

    std::vector<unsigned char> Builder::sign(const std::string &format, std::istream &source, std::ostream &dest, Signer &signer)
    {
        // Caller's source/dest streams must outlive this call
        // Stream wrappers are stack locals that wrap the caller's streams
        CppIStream c_source(source);
        CppOStream c_dest(dest);
        const unsigned char *c2pa_manifest_bytes = nullptr;

        // c2pa_builder_sign() uses streams synchronously and completes before returning
        auto result = c2pa_builder_sign(builder, format.c_str(), c_source.c_stream, c_dest.c_stream, signer.c2pa_signer(), &c2pa_manifest_bytes);
        return detail::to_byte_vector(c2pa_manifest_bytes, result);
    }

    std::vector<unsigned char> Builder::sign(const std::string &format, std::istream &source, std::iostream &dest, Signer &signer)
    {
        // Caller's source/dest streams must outlive this call
        // Stream wrappers are stack locals that wrap the caller's streams
        CppIStream c_source(source);
        CppIOStream c_dest(dest);
        const unsigned char *c2pa_manifest_bytes = nullptr;

        // c2pa_builder_sign() uses streams synchronously and completes before returning
        auto result = c2pa_builder_sign(builder, format.c_str(), c_source.c_stream, c_dest.c_stream, signer.c2pa_signer(), &c2pa_manifest_bytes);
        return detail::to_byte_vector(c2pa_manifest_bytes, result);
    }

    /// @brief Sign a file and write the signed data to an output file.
    /// @param source_path The path to the file to sign.
    /// @param dest_path The path to write the signed file to.
    /// @param signer A signer object to use when signing.
    /// @return A vector containing the signed manifest bytes.
    /// @throws C2pa::C2paException for errors encountered by the C2PA library.
    std::vector<unsigned char> Builder::sign(const std::filesystem::path &source_path, const std::filesystem::path &dest_path, Signer &signer)
    {
        auto source = detail::open_file_binary<std::ifstream>(source_path);
        // Ensure the destination directory exists
        auto dest_dir = dest_path.parent_path();
        if (!std::filesystem::exists(dest_dir))
        {
            std::filesystem::create_directories(dest_dir);
        }

        std::fstream dest(dest_path,
                          std::ios_base::binary | std::ios_base::trunc |
                              std::ios_base::in | std::ios_base::out);

        if (!dest.is_open())
        {
            throw std::runtime_error("Failed to open destination file: " + dest_path.string());
        }
        auto format = detail::extract_file_extension(dest_path);
        auto result = sign(format, *source, dest, signer);
        return result;
    }

    std::vector<unsigned char> Builder::sign(const std::string &format, std::istream &source, std::iostream &dest)
    {
        CppIStream c_source(source);
        CppIOStream c_dest(dest);
        const unsigned char *c2pa_manifest_bytes = nullptr;

        auto result = c2pa_builder_sign_context(builder, format.c_str(), c_source.c_stream, c_dest.c_stream, &c2pa_manifest_bytes);
        return detail::to_byte_vector(c2pa_manifest_bytes, result);
    }

    std::vector<unsigned char> Builder::sign(const std::filesystem::path &source_path, const std::filesystem::path &dest_path)
    {
        auto source = detail::open_file_binary<std::ifstream>(source_path);
        auto dest_dir = dest_path.parent_path();
        if (!std::filesystem::exists(dest_dir))
        {
            std::filesystem::create_directories(dest_dir);
        }

        std::fstream dest(dest_path,
                          std::ios_base::binary | std::ios_base::trunc |
                              std::ios_base::in | std::ios_base::out);

        if (!dest.is_open())
        {
            throw std::runtime_error("Failed to open destination file: " + dest_path.string());
        }
        auto format = detail::extract_file_extension(dest_path);
        auto result = sign(format, *source, dest);
        return result;
    }

    /// @brief Create a Builder from an archive stream.
    /// @param archive The input stream to read the archive from.
    /// @throws C2pa::C2paException for errors encountered by the C2PA library.
    Builder Builder::from_archive(std::istream &archive)
    {
        return Builder(archive);
    }

    /// @brief Create a Builder from an archive file.
    /// @param archive The input path to read the archive from.
    /// @throws C2pa::C2paException for errors encountered by the C2PA library.
    Builder Builder::from_archive(const std::filesystem::path &archive_path)
    {
        auto path = detail::open_file_binary<std::ifstream>(archive_path);
        return from_archive(*path);
    }

    /// @brief Load an archive into a Builder instance, replacing its current manifest definition.
    /// @param archive The input stream to read the archive from.
    /// @return Reference to this builder for method chaining.
    /// @throws C2pa::C2paException for errors encountered by the C2PA library.
    Builder& Builder::with_archive(std::istream &archive)
    {
        CppIStream c_archive(archive);

        // c2pa_builder_with_archive consumes the builder pointer and returns a new one
        C2paBuilder* updated = c2pa_builder_with_archive(builder, c_archive.c_stream);
        builder = nullptr;
        if (updated == nullptr) {
            throw C2paException();
        }
        builder = updated;
        return *this;
    }

    /// @brief Write the builder to an archive stream.
    /// @param dest The output stream to write the archive to.
    /// @throws C2pa::C2paException for errors encountered by the C2PA library.
    void Builder::to_archive(std::ostream &dest)
    {
        CppOStream c_dest = CppOStream(dest);
        int result = c2pa_builder_to_archive(builder, c_dest.c_stream);
        if (result < 0)
        {
            throw C2paException();
        }
    }

    /// @brief Write the builder to an archive file.
    /// @param dest_path The path to write the archive file to.
    /// @throws C2pa::C2paException for errors encountered by the C2PA library.
    void Builder::to_archive(const std::filesystem::path &dest_path)
    {
        auto dest = detail::open_file_binary<std::ofstream>(dest_path);
        to_archive(*dest);
    }

    std::vector<unsigned char> Builder::data_hashed_placeholder(uintptr_t reserve_size, const std::string &format)
    {
        const unsigned char *c2pa_manifest_bytes = nullptr;
        auto result = c2pa_builder_data_hashed_placeholder(builder, reserve_size, format.c_str(), &c2pa_manifest_bytes);
        return detail::to_byte_vector(c2pa_manifest_bytes, result);
    }

    std::vector<unsigned char> Builder::sign_data_hashed_embeddable(Signer &signer, const std::string &data_hash, const std::string &format, std::istream *asset)
    {
        int64_t result;
        const unsigned char *c2pa_manifest_bytes = nullptr;
        if (asset)
        {
            CppIStream c_asset(*asset);
            result = c2pa_builder_sign_data_hashed_embeddable(builder, signer.c2pa_signer(), data_hash.c_str(), format.c_str(), c_asset.c_stream, &c2pa_manifest_bytes);
        }
        else
        {
            result = c2pa_builder_sign_data_hashed_embeddable(builder, signer.c2pa_signer(), data_hash.c_str(), format.c_str(), nullptr, &c2pa_manifest_bytes);
        }
        return detail::to_byte_vector(c2pa_manifest_bytes, result);
    }

    bool Builder::needs_placeholder(const std::string &format)
    {
        int result = c2pa_builder_needs_placeholder(builder, format.c_str());
        if (result < 0)
        {
            throw C2paException();
        }
        return result == 1;
    }

    std::vector<unsigned char> Builder::placeholder(const std::string &format)
    {
        const unsigned char *c2pa_manifest_bytes = nullptr;
        auto result = c2pa_builder_placeholder(builder, format.c_str(), &c2pa_manifest_bytes);
        return detail::to_byte_vector(c2pa_manifest_bytes, result);
    }

    void Builder::set_data_hash_exclusions(const std::vector<std::pair<uint64_t, uint64_t>> &exclusions)
    {
        std::vector<uint64_t> flat;
        flat.reserve(exclusions.size() * 2);
        for (const auto &[start, len] : exclusions)
        {
            flat.push_back(start);
            flat.push_back(len);
        }
        int result = c2pa_builder_set_data_hash_exclusions(
            builder, flat.data(), exclusions.size());
        if (result < 0)
        {
            throw C2paException();
        }
    }

    void Builder::update_hash_from_stream(const std::string &format, std::istream &stream)
    {
        CppIStream c_stream(stream);
        int result = c2pa_builder_update_hash_from_stream(builder, format.c_str(), c_stream.c_stream);
        if (result < 0)
        {
            throw C2paException();
        }
    }

    std::vector<unsigned char> Builder::sign_embeddable(const std::string &format)
    {
        const unsigned char *c2pa_manifest_bytes = nullptr;
        auto result = c2pa_builder_sign_embeddable(builder, format.c_str(), &c2pa_manifest_bytes);
        return detail::to_byte_vector(c2pa_manifest_bytes, result);
    }

    std::vector<unsigned char> Builder::format_embeddable(const std::string &format, std::vector<unsigned char> &data)
    {
        const unsigned char *c2pa_manifest_bytes = nullptr;
        auto result = c2pa_format_embeddable(format.c_str(), data.data(), data.size(), &c2pa_manifest_bytes);
        return detail::to_byte_vector(c2pa_manifest_bytes, result);
    }

    std::vector<std::string> Builder::supported_mime_types() {
      uintptr_t count = 0;
      auto ptr = c2pa_builder_supported_mime_types(&count);
      return detail::c_mime_types_to_vector(ptr, count);
    }
} // namespace c2pa
