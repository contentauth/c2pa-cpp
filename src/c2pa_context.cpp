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

/// @file   c2pa_context.cpp
/// @brief  Context and ContextBuilder implementation.

#include <exception>
#include <fstream>
#include <utility>

#include "c2pa.hpp"
#include "c2pa_internal.hpp"

namespace c2pa
{
    Context::Context(C2paContext* ctx) : context(ctx) {
        if (!context) {
            throw C2paException("Invalid context pointer");
        }
    }

    Context::Context() : context(c2pa_context_new()) {
        if (!context) {
            throw C2paException("Failed to create Context");
        }
    }

    Context::Context(const Settings& settings)
        : Context(ContextBuilder().with_settings(settings).create_context()) {
    }

    Context::Context(Context&& other) noexcept
        : context(std::exchange(other.context, nullptr)),
          callback_owner_(std::move(other.callback_owner_)) {
    }

    Context& Context::operator=(Context&& other) noexcept {
        if (this != &other) {
            // Free the native context first so it stops invoking the callback,
            // then release the heap block the callback lived in (move).
            if (context) {
                c2pa_free(context);
            }
            context = std::exchange(other.context, nullptr);
            callback_owner_ = std::move(other.callback_owner_);
        }
        return *this;
    }

    Context::~Context() noexcept {
        // Free native context first, then let callback_owner_ destruct.
        if (context) {
            c2pa_free(context);
        }
    }

    Context::Context(C2paContext* ctx,
                     std::unique_ptr<ProgressCallbackFunc> callback) noexcept
        : context(ctx), callback_owner_(std::move(callback)) {
    }

    void Context::cancel() noexcept {
        if (context) {
            c2pa_context_cancel(context);
        }
    }

    C2paContext* Context::c_context() const noexcept {
        return context;
    }

    bool Context::is_valid() const noexcept {
        return context != nullptr;
    }

    Context::Context(const std::string& json) : Context(Settings(json, "json")) {
    }

    Context::Context(const Settings& settings, Signer&& signer)
        : Context(ContextBuilder()
            .with_settings(settings)
            .with_signer(std::move(signer))
            .create_context()) {
    }

    // Context::ContextBuilder

    Context::ContextBuilder::ContextBuilder() : context_builder(c2pa_context_builder_new()) {
        if (!context_builder) {
            throw C2paException("Failed to create Context builder");
        }
    }

    Context::ContextBuilder::ContextBuilder(ContextBuilder&& other) noexcept
        : pending_callback_(std::move(other.pending_callback_)),
          context_builder(std::exchange(other.context_builder, nullptr)) {
    }

    Context::ContextBuilder& Context::ContextBuilder::operator=(ContextBuilder&& other) noexcept {
        if (this != &other) {
            // Free the native builder before moving in the new pending_callback_ so
            // the old native builder cannot touch its (about-to-be-replaced) callback
            // block after we've dropped the old one.
            if (context_builder) {
                c2pa_free(context_builder);
            }
            context_builder = std::exchange(other.context_builder, nullptr);
            pending_callback_ = std::move(other.pending_callback_);
        }
        return *this;
    }

    Context::ContextBuilder::~ContextBuilder() noexcept {
        // Member destruction order is reverse of declaration:
        // context_builder freed first (stops FFI from calling the callback),
        // then pending_callback_ is released.
        if (context_builder) {
            c2pa_free(context_builder);
        }
    }

    bool Context::ContextBuilder::is_valid() const noexcept {
        return context_builder != nullptr;
    }

    Context::ContextBuilder& Context::ContextBuilder::with_settings(const Settings& settings) {
        if (!is_valid()) {
            throw C2paException("ContextBuilder is invalid (moved from)");
        }
        if (!settings.is_valid()) {
            throw C2paException("Settings object is invalid");
        }
        if (c2pa_context_builder_set_settings(context_builder, settings.c_settings()) != 0) {
            throw C2paException();
        }
        return *this;
    }

    Context::ContextBuilder& Context::ContextBuilder::with_json(const std::string& json) {
        if (!is_valid()) {
            throw C2paException("ContextBuilder is invalid (moved from)");
        }
        return with_settings(Settings(json, "json"));
    }

    Context::ContextBuilder& Context::ContextBuilder::with_json_settings_file(const std::filesystem::path& settings_path) {
        if (!is_valid()) {
            throw C2paException("ContextBuilder is invalid (moved from)");
        }

        auto file = detail::open_file_binary<std::ifstream>(settings_path);

        file->seekg(0, std::ios::end);
        auto file_size = file->tellg();
        file->seekg(0, std::ios::beg);
        if (file_size < 0) {
            throw C2paException("Settings file is not readable");
        }
        constexpr std::streamoff max_settings_size = 1024 * 1024; // 1 MB max, similar to c2pa-rs
        if (file_size > max_settings_size) {
            throw C2paException("Settings file is too large (>1MB)");
        }

        std::string json_content((std::istreambuf_iterator<char>(*file)), std::istreambuf_iterator<char>());

        return with_json(json_content);
    }

    Context::ContextBuilder& Context::ContextBuilder::with_signer(Signer&& signer) {
        if (!is_valid()) {
            throw C2paException("ContextBuilder is invalid (moved from)");
        }
        C2paSigner* raw = signer.release();
        if (!raw) {
            throw C2paException("Signer is not valid");
        }
        // On error the signer may not have been consumed by the C API,
        // surface an error
        if (c2pa_context_builder_set_signer(context_builder, raw) != 0) {
            throw C2paException();
        }
        return *this;
    }

    Context::ContextBuilder& Context::ContextBuilder::with_http_resolver(
        void* user_data, C2paHttpResolverCallback callback) {
        if (!is_valid()) {
            throw C2paException("ContextBuilder is invalid (moved from)");
        }
        if (!callback) {
            throw C2paException("HTTP resolver callback must not be null");
        }
        C2paHttpResolver* resolver = c2pa_http_resolver_create(user_data, callback);
        if (!resolver) {
            throw C2paException("Failed to create HTTP resolver");
        }
        if (c2pa_context_builder_set_http_resolver(context_builder, resolver) != 0) {
            c2pa_free(resolver);
            throw C2paException();
        }
        return *this;
    }

    // Verify our C++ enum class stays in sync with the C enum from c2pa.h.
    // If c2pa-rs adds or reorders variants, these will catch it at compile time.
    static_assert(static_cast<uint8_t>(ProgressPhase::Reading)               == Reading,               "ProgressPhase::Reading mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::VerifyingManifest)     == VerifyingManifest,     "ProgressPhase::VerifyingManifest mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::VerifyingSignature)    == VerifyingSignature,    "ProgressPhase::VerifyingSignature mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::VerifyingIngredient)   == VerifyingIngredient,   "ProgressPhase::VerifyingIngredient mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::VerifyingAssetHash)    == VerifyingAssetHash,    "ProgressPhase::VerifyingAssetHash mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::AddingIngredient)      == AddingIngredient,      "ProgressPhase::AddingIngredient mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::Thumbnail)             == Thumbnail,             "ProgressPhase::Thumbnail mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::Hashing)               == Hashing,               "ProgressPhase::Hashing mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::Signing)               == Signing,               "ProgressPhase::Signing mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::Embedding)             == Embedding,             "ProgressPhase::Embedding mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::FetchingRemoteManifest)== FetchingRemoteManifest,"ProgressPhase::FetchingRemoteManifest mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::Writing)               == Writing,               "ProgressPhase::Writing mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::FetchingOCSP)          == FetchingOCSP,          "ProgressPhase::FetchingOCSP mismatch");
    static_assert(static_cast<uint8_t>(ProgressPhase::FetchingTimestamp)     == FetchingTimestamp,     "ProgressPhase::FetchingTimestamp mismatch");

    // C trampoline: bridges the C callback ABI to the stored std::function.
    // Returns non-zero to continue, zero to cancel (matching ProgressCCallback convention).
    // Exceptions must not unwind into Rust/C: treat any throw like cancellation (return 0).
    // Callers should not throw from the callback; a future c2pa-rs API may surface errors explicitly.
    static int progress_callback_trampoline(const void* user_data,
                                            C2paProgressPhase phase,
                                            uint32_t step,
                                            uint32_t total) {
        try {
            const auto* cb = static_cast<const ProgressCallbackFunc*>(user_data);
            return (*cb)(static_cast<ProgressPhase>(phase), step, total) ? 1 : 0;
        } catch (const std::exception&) {
            return 0;
        } catch (...) {
            return 0;
        }
    }

    Context::ContextBuilder& Context::ContextBuilder::with_progress_callback(ProgressCallbackFunc callback) {
        if (!is_valid()) {
            throw C2paException("ContextBuilder is invalid (moved from)");
        }

        // Two-stage swap: create the new heap block, register it with the FFI, and
        // only after drop any previous pending_callback_.
        auto fresh = std::make_unique<ProgressCallbackFunc>(std::move(callback));
        if (c2pa_context_builder_set_progress_callback(
                context_builder,
                fresh.get(),
                progress_callback_trampoline) != 0) {
            throw C2paException();
        }
        pending_callback_ = std::move(fresh);
        return *this;
    }

    Context::ContextBuilder::ReleasedBuilder Context::ContextBuilder::release() noexcept {
        // Transfer both the native builder handle and the callback heap block to
        // the caller atomically.
        return ReleasedBuilder{
            std::exchange(context_builder, nullptr),
            std::move(pending_callback_),
        };
    }

    Context Context::ContextBuilder::create_context() {
        if (!is_valid()) {
            throw C2paException("ContextBuilder is invalid (moved from)");
        }

        // The C API consumes the context builder on build (success or failure).
        C2paContext* ctx = c2pa_context_builder_build(context_builder);
        context_builder = nullptr;
        if (!ctx) {
            throw C2paException("Failed to build context");
        }

        return Context(ctx, std::move(pending_callback_));
    }
} // namespace c2pa
