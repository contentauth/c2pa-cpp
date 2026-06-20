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

/// @file   c2pa.hpp
/// @brief  C++ wrapper for the C2PA C library.
/// @details This is used for creating and verifying C2PA manifests.
///          This is an early version, and has not been fully tested.
///          Thread safety is not guaranteed due to the use of errno and etc.

#ifndef C2PA_H
#define C2PA_H

// Suppress unused function warning for GCC/Clang
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

// Suppress unused function warning for MSVC
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4505)
#endif

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <utility>

#include "c2pa.h"

// NOOP for now, can use later to define static library
#define C2PA_CPP_API

namespace c2pa
{
    /// @typedef SignerInfo
    /// @brief Type alias for C2paSignerInfo from the C API.
    typedef C2paSignerInfo SignerInfo;

    // Forward declarations for context types
    class Settings;
    class Context;
    class IContextProvider;
    class Signer;

    /// @brief Result codes for C API operations (matches C API return convention).
    enum class OperationResult : int {
        Success = 0,     ///< Operation succeeded
        Error = -1       ///< Operation failed (check C2paException for details)
    };

    /// @brief Stream/FFI error codes (maps to errno values used by the C layer).
    enum class StreamError : int {
        InvalidArgument = EINVAL,
        IoError = EIO,
        NoBufferSpace = ENOBUFS
    };

    /// @brief Set errno from StreamError and return error sentinel.
    /// @param e The StreamError value to convert to errno.
    /// @return OperationResult::Error (-1) for use as C API error return.
    inline int stream_error_return(StreamError e) noexcept {
        errno = static_cast<int>(e);
        return static_cast<int>(OperationResult::Error);
    }

    /// @brief Exception class for C2pa errors.
    /// This class is used to throw exceptions for errors encountered by the C2pa library via c2pa_error().
    class C2PA_CPP_API C2paException : public std::exception
    {
    public:
        /// @brief Default constructor.
        /// @details Creates an exception and retrieves the error message from the C2PA library.
        C2paException();

        /// @brief Construct an exception with a custom error message.
        /// @param message The error message.
        explicit C2paException(std::string message);

        ~C2paException() override = default;

        C2paException(const C2paException&) = default;

        C2paException& operator=(const C2paException&) = default;

        C2paException(C2paException&&) = default;

        C2paException& operator=(C2paException&&) = default;

        /// @brief Get the exception message.
        /// @return Null-terminated error message string.
        const char* what() const noexcept override;

    private:
        std::string message_;
    };

    /// @brief Interface for types that can provide C2PA context functionality.
    /// @details This interface can be implemented by external libraries to provide
    ///          custom context implementations (e.g. AdobeContext wrappers).
    ///          Reader and Builder accept a shared_ptr<IContextProvider> and extend
    ///          the provider's lifetime for as long as the Reader/Builder exists.
    ///
    /// @par Progress callback lifetime (post-0322d67)
    /// If the native C2paContext* held by the provider was configured with a progress
    /// callback, the provider MUST keep the heap-owned ProgressCallbackFunc alive at
    /// least as long as the native context. The native side stores only a raw pointer
    /// into that heap block and will call it until the context is freed. Destroy
    /// order inside the provider: free the native C2paContext* first (stops further
    /// callback invocations), then release the callback storage. The built-in
    /// Context class enforces this via member declaration order; external providers
    /// that adopt a native context built via ContextBuilder::release() must do the
    /// same.
    ///
    /// @par Move semantics
    /// Move construction and move assignment are defaulted. After move, the moved-from
    /// object is left in a valid but unspecified state: is_valid() may be false and
    /// c_context() may return nullptr. Implementations that own a C2paContext* (e.g. Context)
    /// must set the source's handle to nullptr on move to avoid double-free; callers must
    /// not use a moved-from provider without checking is_valid() first.
    ///
    /// @par Implementation Requirements for is_valid()
    /// The is_valid() method exists to support implementations that may have:
    /// - Optional or lazy context initialization
    /// - Contexts that can be invalidated or moved
    /// - A "no context" state as part of their lifecycle
    ///
    /// @par Why Both c_context() and is_valid()?
    /// While c_context() can return nullptr, is_valid() provides:
    /// 1. A boolean check without pointer inspection (yes/no answer for intialization)
    /// 2. Forward compatibility for implementations with complex context lifecycles (lazy load)
    ///
    /// @par Impact on Reader and Builder
    /// Reader and Builder constructors validate that a provider both exists and
    /// is_valid() returns true before using c_context(). This ensures that:
    /// - External implementations cannot be used in an uninitialized state
    /// - A consistent validation pattern exists across all context-using classes
    /// - Errors are caught early at construction time rather than during operations
    ///
    /// @par Standard Context Implementation
    /// The built-in Context class always returns true from is_valid() after
    /// successful construction, as it validates the context pointer in its constructor.
    /// External implementations may have different invariants.
    class C2PA_CPP_API IContextProvider {
    public:
        virtual ~IContextProvider() noexcept = default;

        /// @brief Get the underlying C2PA context pointer for FFI operations.
        /// @return Pointer to C2paContext, or nullptr if not available.
        /// @note Provider retains ownership; pointer valid for provider's lifetime.
        [[nodiscard]] virtual C2paContext* c_context() const noexcept = 0;

        /// @brief Check if this provider has a valid context.
        /// @return true if context is available, false otherwise.
        /// @note For standard Context objects, this always returns true after construction.
        ///       External implementations may return false to indicate uninitialized or
        ///       invalidated state. Reader and Builder constructors check this before use.
        /// @warning Implementations must ensure is_valid() == true implies c_context() != nullptr.
        [[nodiscard]] virtual bool is_valid() const noexcept = 0;

    protected:
        IContextProvider() = default;

        IContextProvider(const IContextProvider&) = delete;
        IContextProvider& operator=(const IContextProvider&) = delete;
    };

    /// @brief (C2PA SDK) Settings configuration object for creating contexts.
    /// @details Settings can be configured via JSON strings or programmatically
    ///          via set() and update() methods. Once passed to Context::ContextBuilder,
    ///          the settings are copied into the context and the Settings
    ///          object can be reused or discarded.
    ///
    /// @par Validity
    /// Settings uses is_valid() to indicate whether the object holds a valid underlying
    /// C settings handle. After move-from, is_valid() is false. set(), update(), and callers
    /// passing this object to the C API must ensure is_valid() is true or check before use.
    class C2PA_CPP_API Settings {
    public:
        /// @brief Create default settings.
        Settings();

        /// @brief Create settings from a configuration string.
        /// @param data Configuration data in JSON format.
        /// @param format Format of the data ("json").
        /// @throws C2paException if parsing fails.
        Settings(const std::string& data, const std::string& format);

        // Move semantics
        Settings(Settings&&) noexcept;
        Settings& operator=(Settings&&) noexcept;

        // Non-copyable
        Settings(const Settings&) = delete;
        Settings& operator=(const Settings&) = delete;

        ~Settings() noexcept;

        /// @brief Check if this Settings object is valid (holds a C settings handle).
        /// @return true if the object can be used (set, update, c_settings, or passed to Context/ContextBuilder).
        /// @return false if moved-from (or otherwise invalid). Callers must check before use.
        [[nodiscard]] bool is_valid() const noexcept;

        /// @brief Set a single configuration value by path.
        /// @param path Dot-separated path to the setting (e.g., "verify.verify_after_sign").
        /// @param json_value JSON-encoded value to set.
        /// @return Reference to this Settings for method chaining.
        /// @throws C2paException if the path or value is invalid.
        Settings& set(const std::string& path, const std::string& json_value);

        /// @brief Merge configuration from a JSON string (latest configuration wins).
        /// @param data Configuration data in JSON format.
        /// @return Reference to this Settings for method chaining.
        /// @throws C2paException if parsing fails, or if this object is invalid.
        /// @note This is the recommended overload when configuration is JSON.
        Settings& update(const std::string& data) { return update(data, "json"); }

        /// @brief Merge configuration from a std::string (latest configuration wins).
        /// @param data Configuration data in JSON or TOML format.
        /// @param format Format of the data ("json" or "toml").
        /// @return Reference to this Settings for method chaining.
        /// @throws C2paException if parsing fails, or if this object is invalid.
        Settings& update(const std::string& data, const std::string& format);

        /// @brief Get the raw C FFI settings pointer.
        /// @return Pointer to C2paSettings when is_valid() is true; nullptr when invalid.
        /// @note Callers passing this to the C API should check is_valid() first and treat nullptr as invalid.
        [[nodiscard]] C2paSettings* c_settings() const noexcept;

    private:
        C2paSettings* settings_ptr;
    };

     /// @brief Phase values reported to the ProgressCallbackFunc.
    ///
    /// @details A scoped C++ mirror of `C2paProgressPhase` from c2pa.h.
    ///          Values are verified at compile time to match the C enum, so any
    ///          future divergence in c2pa-rs will be caught as a build error.
    ///
    ///          Phases emitted during a typical sign cycle (in order):
    ///            AddingIngredient → Thumbnail → Hashing → Signing → Embedding →
    ///            (if verify_after_sign) VerifyingManifest → VerifyingSignature →
    ///            VerifyingAssetHash → VerifyingIngredient
    ///
    ///          Phases emitted during reading:
    ///            Reading → VerifyingManifest → VerifyingSignature →
    ///            VerifyingAssetHash → VerifyingIngredient
    enum class ProgressPhase : uint8_t {
        Reading               = 0,
        VerifyingManifest     = 1,
        VerifyingSignature    = 2,
        VerifyingIngredient   = 3,
        VerifyingAssetHash    = 4,
        AddingIngredient      = 5,
        Thumbnail             = 6,
        Hashing               = 7,
        Signing               = 8,
        Embedding             = 9,
        FetchingRemoteManifest = 10,
        Writing               = 11,
        FetchingOCSP          = 12,
        FetchingTimestamp     = 13,
    };

    /// @brief Type alias for the progress callback passed to ContextBuilder::with_progress_callback().
    ///
    /// @details The callback is invoked at each major phase of signing and reading operations.
    ///          Returning false from the callback aborts the operation with an
    ///          OperationCancelled error (equivalent to calling Context::cancel()).
    ///
    /// @param phase  Current operation phase.
    /// @param step   1-based step index within the phase.
    ///               0 = indeterminate (use as liveness signal); resets to 1 at each new phase.
    /// @param total  0 = indeterminate; 1 = single-shot; >1 = determinate (step/total = fraction).
    /// @return true to continue the operation, false to request cancellation.
    ///
    /// @note The callback must not throw. If it throws, the implementation catches the
    ///       exception and reports cancellation to the underlying library (same as returning
    ///       false); the original exception is not propagated. Prefer returning false or
    ///       using Context::cancel() instead of throwing.
    ///
    using ProgressCallbackFunc = std::function<bool(ProgressPhase phase, uint32_t step, uint32_t total)>;

    /// @brief C2PA context implementing IContextProvider.
    /// @details Context objects manage C2PA SDK configuration and state.
    ///          Contexts can be created via direct construction or the ContextBuilder:
    ///
    ///          Direct construction:
    ///          @code
    ///          c2pa::Context ctx;              // default
    ///          c2pa::Context ctx(settings);    // from Settings
    ///          c2pa::Context ctx(json);        // from JSON string
    ///          @endcode
    ///
    ///          ContextBuilder (for multi-step configuration):
    ///          @code
    ///          auto ctx = c2pa::Context::ContextBuilder()
    ///              .with_settings(settings)
    ///              .with_json(json)
    ///              .create_context();
    ///          @endcode
    ///
    ///          Builder and Reader take the context by reference (IContextProvider&).
    ///          The context object must outlive the Builder or Reader instance.
    class C2PA_CPP_API Context : public IContextProvider {
    public:
        /// @brief ContextBuilder for creating customized Context instances.
        /// @details Provides a builder pattern for configuring contexts with multiple settings.
        ///          Note: create_context() consumes the builder.
        /// @note For most use cases, prefer direct construction via the Context constructors.
        ///       The ContextBuilder is useful when you need to layer multiple configuration
        ///       sources (e.g. with_settings() followed by with_json()).
        class C2PA_CPP_API ContextBuilder {
        public:
            ContextBuilder();
            ~ContextBuilder() noexcept;

            // Move semantics
            ContextBuilder(ContextBuilder&&) noexcept;
            ContextBuilder& operator=(ContextBuilder&&) noexcept;

            // Non-copyable
            ContextBuilder(const ContextBuilder&) = delete;
            ContextBuilder& operator=(const ContextBuilder&) = delete;

            /// @brief Check if the builder is in a valid state.
            /// @return true if the builder can be used, false if moved from.
            [[nodiscard]] bool is_valid() const noexcept;

            /// @brief Configure with Settings object.
            /// @param settings Settings to use (will be copied into the context). Must be valid (is_valid() true).
            /// @return Reference to this ContextBuilder for method chaining.
            /// @throws C2paException if settings are invalid or settings.is_valid() is false.
            ContextBuilder& with_settings(const Settings& settings);

            /// @brief Configure settings with JSON string.
            /// @param json JSON configuration string.
            /// @return Reference to this ContextBuilder for method chaining.
            /// @throws C2paException if JSON is invalid.
            ContextBuilder& with_json(const std::string& json);

            /// @brief Configure settings from a JSON settings file.
            /// @param settings_path Full path to the JSON settings file.
            /// @return Reference to this ContextBuilder for method chaining.
            /// @throws C2paException if file cannot be read or JSON is invalid.
            ContextBuilder& with_json_settings_file(const std::filesystem::path& settings_path);

            /// @brief Set a Signer on the context being built.
            /// @details After this call the source Signer object is consumed and must
            ///          not be reused, as it becomes part to the context and tied to it.
            ///          If settings also contain a signer, the programmatic signer
            ///          set through this API will be used for signing.
            /// @param signer Signer to put into the context.
            /// @return Reference to this ContextBuilder for method chaining.
            /// @throws C2paException if the builder or signer is invalid.
            ContextBuilder& with_signer(Signer&& signer);

            /// @brief Attach a progress callback to the context being built.
            ///
            /// @details The callback is invoked at each major phase of signing and
            ///          reading operations performed with the resulting context.
            ///          Return false from the callback to abort the current operation
            ///          with an OperationCancelled error.
            ///
            ///          Phases emitted during a typical sign cycle (in order):
            ///          VerifyingIngredient → VerifyingManifest → VerifyingSignature →
            ///          VerifyingAssetHash → Thumbnail → Hashing → Signing → Embedding →
            ///          (if verify_after_sign) VerifyingManifest → … → VerifyingIngredient
            ///
            ///          Phases emitted during reading:
            ///          Reading → VerifyingManifest → VerifyingSignature →
            ///          VerifyingAssetHash → VerifyingIngredient
            ///
            /// @param callback A callable matching ProgressCallbackFunc. The callback is
            ///        heap-allocated and owned by the resulting Context. Calling this method
            ///        more than once on the same builder replaces the previous callback.
            ///        The callable must not throw when invoked (see ProgressCallbackFunc).
            /// @return Reference to this ContextBuilder for method chaining.
            /// @throws C2paException if the builder is invalid or the C API call fails.
            ///
            ContextBuilder& with_progress_callback(ProgressCallbackFunc callback);

            /// @brief Set a custom HTTP resolver callback on the context being built.
            /// @details The callback is invoked synchronously whenever the SDK needs to
            ///          make an HTTP request (remote manifest fetch, OCSP, timestamp, etc.).
            ///          The callback receives a C2paHttpRequest and must fill in a C2paHttpResponse.
            ///          The response body must be allocated with malloc(); the underlying native library will call free().
            /// @param user_data Opaque user pointer passed to every callback invocation (may be nullptr).
            /// @param callback Function pointer matching C2paHttpResolverCallback.
            /// @return Reference to this ContextBuilder for method chaining.
            /// @throws C2paException if the resolver could not be set or callback is null.
            ContextBuilder& with_http_resolver(void* user_data, C2paHttpResolverCallback callback);

            /// @brief Create a Context from the current builder configuration.
            /// @return A new Context instance.
            /// @throws C2paException if context creation fails.
            /// @note This consumes the builder. After calling this, is_valid() returns false.
            [[nodiscard]] Context create_context();

            /// @brief Result of ContextBuilder::release(): the raw native builder handle
            ///        paired with the heap-owned progress callback (if any).
            /// @details The caller takes joint ownership. If `callback_owner` is non-null,
            ///          the native side stores a raw pointer into it and will invoke the
            ///          callback until the native context built from `builder` is freed.
            ///          Therefore `callback_owner` must remain alive at least until the
            ///          native context returned by c2pa_context_builder_build(builder) is
            ///          freed via c2pa_free(). Release order: native context first, then
            ///          `callback_owner` goes out of scope.
            struct ReleasedBuilder {
                C2paContextBuilder* builder;
                std::unique_ptr<ProgressCallbackFunc> callback_owner;
            };

            /// @brief Release ownership of the underlying C2paContextBuilder handle and
            ///        its progress-callback heap block (if any) to the caller.
            /// @details After this call is_valid() returns false. The previous
            ///          C2paContextBuilder* overload was unsound when a progress callback
            ///          was pending: the C++ builder destructor would free the heap
            ///          ProgressCallbackFunc while the native side still held its pointer,
            ///          yielding a use-after-free on the first progress tick.
            ///          Callers must keep ReleasedBuilder::callback_owner alive for as
            ///          long as the native context built from ReleasedBuilder::builder
            ///          exists.
            /// @return ReleasedBuilder holding both the native builder and (optionally)
            ///         the owning unique_ptr for the progress callback. Fields are null
            ///         when moved-from.
            [[nodiscard]] ReleasedBuilder release() noexcept;

        private:
            // pending_callback_ is declared before context_builder
            // so destruction (reverse of declaration) frees context_builder first, while
            // the callback heap block is still live, then releases pending_callback_.
            std::unique_ptr<ProgressCallbackFunc> pending_callback_;
            C2paContextBuilder* context_builder;
        };

        // Direct construction
        /// @brief Create a Context with default settings.
        /// @throws C2paException if context creation fails.
        Context();

        /// @brief Create a Context configured with a Settings object.
        /// @param settings Settings configuration to apply. Must be valid (settings.is_valid() true).
        /// @throws C2paException if settings are invalid, settings.is_valid() is false, or context creation fails.
        explicit Context(const Settings& settings);

        /// @brief Create a Context configured with a JSON string.
        /// @param json JSON configuration string.
        /// @throws C2paException if JSON is invalid or context creation fails.
        explicit Context(const std::string& json);

        /// @brief Create a Context with a Settings object and a Signer.
        /// @param settings Settings configuration to apply.
        /// @param signer Signer to move into the context. Consumed after this call.
        ///        The programmatic Signer from the signer parameter
        ///        takes priority over the Signer in settings, so use this API
        ///        when wanting to explicitly set a Signer (or override the Signer in settings).
        /// @throws C2paException if settings or signer are invalid, or context creation fails.
        Context(const Settings& settings, Signer&& signer);

        // Non-copyable, moveable
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        Context(Context&&) noexcept;
        Context& operator=(Context&&) noexcept;

        ~Context() noexcept override;

        // IContextProvider implementation
        /// @brief Get the underlying C2PA context pointer.
        /// @return C2paContext pointer when is_valid() is true; nullptr when moved-from (is_valid() false).
        /// @note Callers must check is_valid() before using the result; do not pass nullptr to the C API.
        [[nodiscard]] C2paContext* c_context() const noexcept override;

        /// @brief Check if this Context has a valid context (validity check for context-like types).
        /// @return true when the object holds a valid C context; false when moved-from.
        /// @note After move, is_valid() is false and c_context() returns nullptr.
        [[nodiscard]] bool is_valid() const noexcept override;

        /// @brief Internal constructor from raw FFI pointer (prefer public constructors).
        /// @param ctx Raw C2paContext pointer — Context takes ownership.
        /// @throws C2paException if ctx is nullptr.
        explicit Context(C2paContext* ctx);

        /// @brief "Adopt" a native context together with its heap-owned progress callback.
        /// @param ctx Native context pointer. Context takes ownership.
        /// @param callback Heap-owned progress callback whose raw pointer the native
        ///        context may hold. May be null.
        Context(C2paContext* ctx,
                std::unique_ptr<ProgressCallbackFunc> callback) noexcept;

        /// @brief Request cancellation of any in-progress operation on this context.
        ///
        /// @details Safe to call from another thread while this Context remains valid
        ///          and is not being destroyed or moved concurrently with this call.
        ///          While a signing or reading operation is running on a valid Context,
        ///          the operation is aborted with an OperationCancelled error at the
        ///          next progress checkpoint. Has no effect if no operation is currently
        ///          in progress, or if this object is moved-from (is_valid() is false).
        ///
        void cancel() noexcept;

    private:
        // Member order matters: `context` is declared before `callback_owner_` so
        // destruction (reverse of declaration) frees the heap callback block after
        // the native context has been freed.
        C2paContext* context;

        /// Heap-owned ProgressCallbackFunc, non-null only when the Context was built
        /// with a progress callback via ContextBuilder::with_progress_callback().
        /// Destroyed after `context`.
        std::unique_ptr<ProgressCallbackFunc> callback_owner_;
    };

    /// @brief Get the version of the C2PA library.
    /// @return Version string.
    std::string C2PA_CPP_API version();

    /// @brief Load C2PA settings from a string in a given format.
    /// @param data The configuration data to load.
    /// @param format The mimetype of the string.
    /// @throws C2paException for errors encountered by the C2PA library.
    /// @deprecated Use Context constructors or Context::ContextBuilder instead for better thread safety.
    [[deprecated("Use Context::from_json() or Context::from_settings() instead")]]
    void C2PA_CPP_API load_settings(const std::string& data, const std::string& format);

    /// @defgroup StreamWrappers Stream wrappers for C2PA C API
    /// @brief C++ stream types that adapt stream types to C2paStream.
    ///
    /// The C2PA C API expects a C2paStream with four callbacks: reader, writer, seeker, flusher.
    /// The contract for each callback is:
    /// - reader(context, buffer, size): read up to size bytes into buffer; return bytes read, or -1 on error (set errno).
    /// - writer(context, buffer, size): write size bytes from buffer; return bytes written, or -1 on error (set errno).
    /// - seeker(context, offset, whence): seek to offset (whence = Start/Current/End); return new position or -1 (set errno).
    /// - flusher(context): flush; return 0 on success, -1 on error (set errno).

    /// @brief Input stream IStream wrapper for C2paStream.
    /// @details This class is used to wrap an input stream for use with the C2PA library.
    class C2PA_CPP_API CppIStream : public C2paStream
    {
    public:
        /// @brief Pointer to the underlying C2paStream.
        C2paStream *c_stream;

        /// @brief Construct an input stream wrapper from a std::istream-derived object.
        /// @tparam IStream Type derived from std::istream.
        /// @param istream The input stream to wrap (must be open and valid).
        /// @throws C2paException if stream wrapper creation fails.
        template <typename IStream>
        explicit CppIStream(IStream &istream) {
            static_assert(std::is_base_of<std::istream, IStream>::value,
                      "Stream must be derived from std::istream");
            // Upcast to std::istream* before type erasure; the callbacks cast the context back to std::istream*
            c_stream = c2pa_create_stream(reinterpret_cast<StreamContext *>(static_cast<std::istream *>(&istream)), reader, seeker, writer, flusher);
            if (c_stream == nullptr) {
                throw C2paException("Failed to create input stream wrapper: is stream open and valid?");
            }
        }

        CppIStream(const CppIStream &) = delete;

        CppIStream &operator=(const CppIStream &) = delete;

        CppIStream(CppIStream &&) = delete;

        CppIStream &operator=(CppIStream &&) = delete;

        ~CppIStream();

    private:
        /// @brief Reader callback implementation.
        /// @param context Stream context pointer.
        /// @param buffer Buffer to read into.
        /// @param size Number of bytes to read.
        /// @return Number of bytes read, or -1 on error (sets errno).
        static intptr_t reader(StreamContext *context, uint8_t *buffer, intptr_t size);

        /// @brief Writer callback implementation (not used for input streams).
        /// @param context Stream context pointer.
        /// @param buffer Buffer to write from.
        /// @param size Number of bytes to write.
        /// @return -1 (always fails for input streams).
        static intptr_t writer(StreamContext *context, const uint8_t *buffer, intptr_t size);

        /// @brief Seeker callback implementation.
        /// @param context Stream context pointer.
        /// @param offset Offset to seek to.
        /// @param whence Seek mode (Start/Current/End).
        /// @return New stream position, or -1 on error (sets errno).
        static intptr_t seeker(StreamContext *context, intptr_t offset, C2paSeekMode whence);

        /// @brief Flusher callback implementation (no-op for input streams).
        /// @param context Stream context pointer.
        /// @return 0 on success.
        static intptr_t flusher(StreamContext *context);

        friend class Reader;
    };

    /// @brief Output stream OStream wrapper for C2paStream.
    /// @details This class is used to wrap an output stream for use with the C2PA library.
    class C2PA_CPP_API CppOStream : public C2paStream
    {
    public:
        /// @brief Pointer to the underlying C2paStream.
        C2paStream *c_stream;

        /// @brief Construct an output stream wrapper from a std::ostream-derived object.
        /// @tparam OStream Type derived from std::ostream.
        /// @param ostream The output stream to wrap (must be open and valid).
        /// @throws C2paException if stream wrapper creation fails.
        template <typename OStream>
        explicit CppOStream(OStream &ostream) {
            static_assert(std::is_base_of<std::ostream, OStream>::value, "Stream must be derived from std::ostream");
            // Upcast to std::ostream* before type erasure; the callbacks cast the context back to std::ostream*
            c_stream = c2pa_create_stream(reinterpret_cast<StreamContext *>(static_cast<std::ostream *>(&ostream)), reader, seeker, writer, flusher);
            if (c_stream == nullptr) {
                throw C2paException("Failed to create output stream wrapper: is stream open and valid?");
            }
        }

        CppOStream(const CppOStream &) = delete;

        CppOStream &operator=(const CppOStream &) = delete;

        CppOStream(CppOStream &&) = delete;

        CppOStream &operator=(CppOStream &&) = delete;

        ~CppOStream();

    private:
        /// @brief Reader callback implementation (not used for output streams).
        /// @param context Stream context pointer.
        /// @param buffer Buffer to read into.
        /// @param size Number of bytes to read.
        /// @return -1 (always fails for output streams).
        static intptr_t reader(StreamContext *context, uint8_t *buffer, intptr_t size);

        /// @brief Writer callback implementation.
        /// @param context Stream context pointer.
        /// @param buffer Buffer to write from.
        /// @param size Number of bytes to write.
        /// @return Number of bytes written, or -1 on error (sets errno).
        static intptr_t writer(StreamContext *context, const uint8_t *buffer, intptr_t size);

        /// @brief Seeker callback implementation.
        /// @param context Stream context pointer.
        /// @param offset Offset to seek to.
        /// @param whence Seek mode (Start/Current/End).
        /// @return New stream position, or -1 on error (sets errno).
        static intptr_t seeker(StreamContext *context, intptr_t offset, C2paSeekMode whence);

        /// @brief Flusher callback implementation.
        /// @param context Stream context pointer.
        /// @return 0 on success, -1 on error (sets errno).
        static intptr_t flusher(StreamContext *context);
    };

    /// @brief IOStream Class wrapper for C2paStream.
    /// @details This class is used to wrap an input/output stream for use with the C2PA library.
    class C2PA_CPP_API CppIOStream : public C2paStream
    {
    public:
        /// @brief Pointer to the underlying C2paStream.
        C2paStream *c_stream;

        /// @brief Construct an I/O stream wrapper from a std::iostream-derived object.
        /// @tparam IOStream Type derived from std::iostream.
        /// @param iostream The I/O stream to wrap (must be open and valid).
        /// @throws C2paException if stream wrapper creation fails.
        template <typename IOStream>
        CppIOStream(IOStream &iostream) {
            static_assert(std::is_base_of<std::iostream, IOStream>::value, "Stream must be derived from std::iostream");
            // Upcast to std::iostream* before type erasure; the callbacks cast the context back to std::iostream*
            c_stream = c2pa_create_stream(reinterpret_cast<StreamContext *>(static_cast<std::iostream *>(&iostream)), reader, seeker, writer, flusher);
            if (c_stream == nullptr) {
                throw C2paException("Failed to create I/O stream wrapper: is stream open and valid?");
            }
        }

        CppIOStream(const CppIOStream &) = delete;

        CppIOStream &operator=(const CppIOStream &) = delete;

        CppIOStream(CppIOStream &&) = delete;

        CppIOStream &operator=(CppIOStream &&) = delete;

        ~CppIOStream();

    private:
        /// @brief Reader callback implementation.
        /// @param context Stream context pointer.
        /// @param buffer Buffer to read into.
        /// @param size Number of bytes to read.
        /// @return Number of bytes read, or -1 on error (sets errno).
        static intptr_t reader(StreamContext *context, uint8_t *buffer, intptr_t size);

        /// @brief Writer callback implementation.
        /// @param context Stream context pointer.
        /// @param buffer Buffer to write from.
        /// @param size Number of bytes to write.
        /// @return Number of bytes written, or -1 on error (sets errno).
        static intptr_t writer(StreamContext *context, const uint8_t *buffer, intptr_t size);

        /// @brief Seeker callback implementation.
        /// @param context Stream context pointer.
        /// @param offset Offset to seek to.
        /// @param whence Seek mode (Start/Current/End).
        /// @return New stream position, or -1 on error (sets errno).
        static intptr_t seeker(StreamContext *context, intptr_t offset, C2paSeekMode whence);

        /// @brief Flusher callback implementation.
        /// @param context Stream context pointer.
        /// @return 0 on success, -1 on error (sets errno).
        static intptr_t flusher(StreamContext *context);
    };

    /// @brief Reader class for reading a manifest.
    /// @details This class is used to read and validate a manifest from a stream or file.
    ///          Resources are managed using RAII; member order ensures cpp_stream (which
    ///          holds a C stream pointing at the ifstream) is destroyed before owned_stream.
    class C2PA_CPP_API Reader
    {
    private:
        C2paReader *c2pa_reader;
        std::unique_ptr<std::ifstream> owned_stream;       // Owns file stream when created from path
        std::unique_ptr<CppIStream> cpp_stream;            // Wraps stream for C API; destroyed before owned_stream
        std::unique_ptr<CppIStream> fragment_main_stream;  // Main/init stream wrapper for with_fragment()
        std::unique_ptr<CppIStream> fragment_stream;       // Fragment stream wrapper for with_fragment()
        std::shared_ptr<IContextProvider> context_ref;

        void init_from_context(IContextProvider& context, const std::string &format, std::istream &stream);
        void init_from_context(IContextProvider& context, const std::filesystem::path &source_path);
        void init_from_manifest_data_and_stream(IContextProvider& context,
                                                 const std::string& format,
                                                 std::istream& image_stream,
                                                 const std::vector<uint8_t>& manifest_jumbf);

        /// @brief Throw if the Reader holds no valid native handle.
        void ensure_initialized() const {
            if (c2pa_reader == nullptr) {
                throw C2paException("Reader is not initialized");
            }
        }

        Reader() : c2pa_reader(nullptr) {}

    public:
        /// @brief Create a Reader from a context and stream.
        /// @param context Context provider; used at construction to configure settings.
        /// @param format The mime format of the stream.
        /// @param stream The input stream to read from.
        /// @throws C2paException if context.is_valid() returns false,
        ///         or for other errors encountered by the C2PA library.
        /// @deprecated Use Reader(std::shared_ptr<IContextProvider>, format, stream) instead.
        ///             The reference overload does not extend the lifetime of the context, which can
        ///             be problematic when progress callbacks fire after the context is destroyed.
        [[deprecated("Use Reader(std::shared_ptr<IContextProvider>, format, stream) instead. "
                     "The reference overload does not extend the lifetime of the context, which can "
                     "be problematic when progress callbacks fire after the context is destroyed.")]]
        Reader(IContextProvider& context, const std::string &format, std::istream &stream);

        /// @brief Create a Reader from a context and file path.
        /// @param context Context provider; used at construction only to configure settings.
        /// @param source_path The path to the file to read.
        /// @throws C2paException if context.is_valid() returns false,
        ///         or for other errors encountered by the C2PA library.
        /// @note Prefer using the streaming APIs if possible.
        /// @deprecated Use Reader(std::shared_ptr<IContextProvider>, source_path) instead.
        ///             The reference overload does not extend the lifetime of the context, which can
        ///             be problematic when progress callbacks fire after the context is destroyed.
        [[deprecated("Use Reader(std::shared_ptr<IContextProvider>, source_path) instead. "
                     "The reference overload does not extend the lifetime of the context, which can "
                     "be problematic when progress callbacks fire after the context is destroyed.")]]
        Reader(IContextProvider& context, const std::filesystem::path &source_path);

        /// @brief Create a Reader from a shared context and stream.
        /// @details The Reader retains a shared reference to the context, keeping it
        ///          alive for the lifetime of the Reader.
        /// @param context Shared context provider.
        /// @param format The mime format of the stream.
        /// @param stream The input stream to read from.
        /// @throws C2paException if context is null or context->is_valid() returns false.
        Reader(std::shared_ptr<IContextProvider> context, const std::string &format, std::istream &stream);

        /// @brief Create a Reader from a shared context and file path.
        /// @details The Reader retains a shared reference to the context, keeping it
        ///          alive for the lifetime of the Reader.
        /// @param context Shared context provider.
        /// @param source_path The path to the file to read.
        /// @throws C2paException if context is null or context->is_valid() returns false.
        Reader(std::shared_ptr<IContextProvider> context, const std::filesystem::path &source_path);

        /// @brief Create a Reader from a shared context, image stream, and external JUMBF manifest.
        /// @details Performs full C2PA binding verification between @p image_stream and the provided
        ///          JUMBF manifest bytes without requiring the manifest to be embedded in the asset.
        ///          Typical use: sidecar .c2pa files, database-stored manifests, in-memory pipelines.
        ///          The Reader retains a shared reference to the context for its lifetime.
        /// @param context Shared context provider (trust anchors, verification policy).
        /// @param format MIME type of the image stream (e.g., "image/jpeg").
        /// @param image_stream Asset data. Must support seeking (used for exclusion-range hashing).
        /// @param manifest_jumbf Raw JUMBF manifest bytes (e.g., contents of a .c2pa sidecar file).
        /// @note @p image_stream, @p manifest_jumbf, and @p format need only remain valid for the
        ///       duration of the construction call. The Reader does not retain their references.
        /// @throws C2paException if context is null,
        ///         context->is_valid() is false,
        ///         manifest_jumbf is empty, or the C2PA library reports an error.
        Reader(std::shared_ptr<IContextProvider> context,
               const std::string& format,
               std::istream& image_stream,
               const std::vector<uint8_t>& manifest_jumbf);

        /// @brief Create a Reader from a stream (will use global settings if any loaded).
        /// @details The validation_status field in the JSON contains validation results.
        /// @param format The mime format of the stream.
        /// @param stream The input stream to read from.
        /// @throws C2paException for errors encountered by the C2PA library.
        /// @deprecated Use Reader(IContextProvider& context, format, stream) instead.
        [[deprecated("Use Reader(IContextProvider& context, format, stream) instead")]]
        Reader(const std::string &format, std::istream &stream);

        /// @brief Create a Reader from a file path (will use global settings if any loaded).
        /// @param source_path The path to the file to read.
        /// @throws C2paException for errors encountered by the C2PA library.
        /// @deprecated Use Reader(IContextProvider& context, source_path) instead.
        /// @note Prefer using the streaming APIs if possible.
        [[deprecated("Use Reader(IContextProvider& context, source_path) instead")]]
        Reader(const std::filesystem::path &source_path);

        /// @brief Try to open a Reader from a context and file path when the asset may lack C2PA data.
        /// @return A Reader if JUMBF (c2pa/manifest) data is present; std::nullopt if none.
        /// @throws C2paException for errors other than a missing manifest (e.g. invalid asset).
        /// @throws std::system_error if the file cannot be opened.
        /// @deprecated Use from_asset(std::shared_ptr<IContextProvider>, source_path) instead.
        ///             The reference overload does not extend the lifetime of the context, which can
        ///             cause a use-after-free crash when progress callbacks fire after the context is
        ///             destroyed.
        [[deprecated("Use from_asset(std::shared_ptr<IContextProvider>, source_path) instead. "
                     "The reference overload does not extend the lifetime of the context, which can "
                     "be problematic when progress callbacks fire after the context is destroyed.")]]
        static std::optional<Reader> from_asset(IContextProvider& context, const std::filesystem::path &source_path);

        /// @brief Try to create a Reader from a context and stream when the asset may lack C2PA data.
        /// @return A Reader if JUMBF (c2pa/manifest) data is present; std::nullopt if none.
        /// @throws C2paException for errors other than a missing manifest.
        /// @deprecated Use from_asset(std::shared_ptr<IContextProvider>, format, stream) instead.
        ///             The reference overload does not extend the lifetime of the context, which can
        ///             cause a use-after-free crash when progress callbacks fire after the context is
        ///             destroyed.
        [[deprecated("Use from_asset(std::shared_ptr<IContextProvider>, format, stream) instead. "
                     "The reference overload does not extend the lifetime of the context, which can "
                     "be problematic when progress callbacks fire after the context is destroyed.")]]
        static std::optional<Reader> from_asset(IContextProvider& context, const std::string &format, std::istream &stream);

        /// @brief Try to open a Reader from a shared context and file path when the asset may lack C2PA data.
        /// @details The Reader retains a shared reference to the context if C2PA data is found.
        /// @return A Reader if JUMBF (c2pa/manifest) data is present; std::nullopt if none.
        /// @throws C2paException for errors other than a missing manifest.
        static std::optional<Reader> from_asset(std::shared_ptr<IContextProvider> context, const std::filesystem::path &source_path);

        /// @brief Try to create a Reader from a shared context and stream when the asset may lack C2PA data.
        /// @details The Reader retains a shared reference to the context if C2PA data is found.
        /// @return A Reader if JUMBF (c2pa/manifest) data is present; std::nullopt if none.
        /// @throws C2paException for errors other than a missing manifest.
        static std::optional<Reader> from_asset(std::shared_ptr<IContextProvider> context, const std::string &format, std::istream &stream);

        // Non-copyable
        Reader(const Reader&) = delete;

        Reader& operator=(const Reader&) = delete;

        Reader(Reader&& other) noexcept
            : c2pa_reader(std::exchange(other.c2pa_reader, nullptr)),
              owned_stream(std::move(other.owned_stream)),
              cpp_stream(std::move(other.cpp_stream)),
              fragment_main_stream(std::move(other.fragment_main_stream)),
              fragment_stream(std::move(other.fragment_stream)),
              context_ref(std::move(other.context_ref)) {
        }

        Reader& operator=(Reader&& other) noexcept {
            if (this != &other) {
                c2pa_free(c2pa_reader);
                c2pa_reader = std::exchange(other.c2pa_reader, nullptr);
                owned_stream = std::move(other.owned_stream);
                cpp_stream = std::move(other.cpp_stream);
                fragment_main_stream = std::move(other.fragment_main_stream);
                fragment_stream = std::move(other.fragment_stream);
                context_ref = std::move(other.context_ref);
            }
            return *this;
        }

        ~Reader();

        /// @brief Check if the reader was created from an embedded manifest.
        /// @return true if the manifest was embedded in the asset, false if external.
        /// @throws C2paException if the Reader holds no valid handle (e.g. it was moved
        ///         from, or a prior with_fragment() failed and consumed it), or for other
        ///         errors encountered by the C2PA library.
        [[nodiscard]] inline bool is_embedded() const {
            ensure_initialized();
            return c2pa_reader_is_embedded(c2pa_reader);
        }

        /// @brief Returns the remote url of the manifest if this `Reader`
        ///        obtained the manifest remotely
        /// @return Optional string containing the remote URL, or std::nullopt if manifest was embedded.
        /// @throws C2paException for errors encountered by the C2PA library.
        [[nodiscard]] std::optional<std::string> remote_url() const;

        /// @brief Process a BMFF fragment stream with this Reader instance.
        /// @details Used for fragmented BMFF media (DASH/HLS streaming) where content is split
        ///          into an init/main segment and separate fragment files. The Reader is
        ///          created from the init segment (e.g. Reader(context, "video/mp4", init)),
        ///          then goes through one fragment at a time via this method.
        ///          The underlying native SDK consumes the current reader and
        ///          returns a new one configured with the fragment.
        ///          This method swaps the handle in place and returns *this for chaining
        ///          across fragments as the reading steps go through fragments.
        /// @param format MIME type of the media (e.g., "video/mp4").
        /// @param stream The main/init segment.
        /// @param fragment The current fragment to process.
        /// @return *this, for chaining across multiple fragments.
        /// @note @p stream and @p fragment need only remain valid for the duration of this call.
        ///       The Reader does not retain a reference to either after returning.
        /// @throws C2paException if the C2PA library reports an error.
        ///         On failure the underlying reader handle is consumed and
        ///         this Reader must not be used further.
        Reader& with_fragment(const std::string& format, std::istream& stream, std::istream& fragment);

        /// @brief Get the manifest as a JSON string.
        /// @return The manifest as a JSON string.
        /// @throws C2paException for errors encountered by the C2PA library.
        std::string json() const;

        /// @brief Get the manifest as a detailed JSON string.
        /// @details The detailed view more closely resembles the underlying JUMBF
        ///          manifest store. Unlike json(), it exposes the full
        ///          `assertion_store` map for each manifest, including assertions
        ///          that the regular view filters out (such as `c2pa.thumbnail.*`,
        ///          `c2pa.actions.*`, and `c2pa.hash.*`). Use this when building
        ///          redaction URIs or otherwise needing the canonical assertion
        ///          labels embedded in the manifest.
        /// @return The detailed manifest as a JSON string.
        /// @throws C2paException for errors encountered by the C2PA library.
        std::string detailed_json() const;

        /// @brief Get the manifest store as a pretty-printed crJSON string.
        /// @details crJSON is a standardized JSON format for C2PA manifest data.
        ///          This call is infallible at yields valid empty JSON ("{}") if
        ///          there are no Content Credentials.
        /// @return The manifest store as a crJSON string.
        /// @throws C2paException if the underlying C call returns null.
        std::string crjson() const;

        /// @brief Get a resource from the reader and write it to a file.
        /// @param uri The URI of the resource.
        /// @param path The file path to write the resource to.
        /// @return The number of bytes written.
        /// @throws C2paException for errors encountered by the C2PA library.
        /// @note Prefer using the streaming APIs if possible.
        int64_t get_resource(const std::string &uri, const std::filesystem::path &path);

        /// @brief Get a resource from the reader and write it to an output stream.
        /// @param uri The URI of the resource.
        /// @param stream The output stream to write the resource to.
        /// @return The number of bytes written.
        /// @throws C2paException for errors encountered by the C2PA library.
        int64_t get_resource(const std::string &uri, std::ostream &stream);

        /// @brief Get the raw C2paReader pointer.
        /// @return The raw C2paReader pointer.
        /// @note This is intended for internal API use and compatibility with C APIs.
        C2paReader* get_api_internal_raw_reader() const { return c2pa_reader; }

        /// @brief Get a list of mime types that the SDK can read manifests from.
        /// @return Vector of supported MIME type strings.
        static std::vector<std::string> supported_mime_types();
    };

    /// @brief Signer callback function type.
    /// @details This function type is used to create a callback function for signing.
    ///          The callback receives data to sign and returns the signature.
    /// @param data The data to sign.
    /// @return The signature as a vector of bytes.
    using SignerFunc = std::vector<unsigned char>(const std::vector<unsigned char> &);

    /// @brief Signer class for creating a Signer
    /// @details This class is used to create a signer from a signing algorithm, certificate, and TSA URI.
    ///          Supports both callback-based and direct signing methods.
    class C2PA_CPP_API Signer
    {
        friend class Context::ContextBuilder;

    private:
        C2paSigner *signer;

        /// @brief Transfers ownership of the underlying C2paSigner pointer out
        ///        of this wrapper, without freeing it.
        /// @details Used by ContextBuilder::with_signer() to pass the raw pointer
        ///          to c2pa_context_builder_set_signer(), which takes ownership on
        ///          the Rust side via Box::from_raw. After this call the Signer
        ///          wrapper holds nullptr and its destructor is a no-op.
        ///          This is not the same as c2pa_signer_free(), which destroys
        ///          the signer. Similar to std::unique_ptr::release().
        /// @return Raw C2paSigner pointer, or nullptr if already released.
        C2paSigner* release() noexcept {
            return std::exchange(signer, nullptr);
        }

        /// @brief Validate a TSA URI string.
        /// @param tsa_uri The TSA URI to validate.
        /// @return Validated C-string pointer.
        static const char *validate_tsa_uri(const std::string &tsa_uri);

        /// @brief Validate an optional TSA URI.
        /// @param tsa_uri The optional TSA URI to validate.
        /// @return Validated C-string pointer, or nullptr if nullopt.
        static const char *validate_tsa_uri(const std::optional<std::string> &tsa_uri);

    public:
        /// @brief Create a Signer from a callback function.
        /// @param callback The callback function to use for signing.
        /// @param alg The signing algorithm to use (e.g., C2paSigningAlg::PS256).
        /// @param sign_cert The signing certificate in PEM format.
        /// @param tsa_uri The timestamp authority URI for time-stamping.
        /// @throws C2paException if signer creation fails.
        Signer(SignerFunc *callback, C2paSigningAlg alg, const std::string &sign_cert, const std::string &tsa_uri);

        /// @brief Create a signer from a Signer pointer and take ownership of that pointer.
        /// @param c_signer The C2paSigner pointer (must be non-null).
        Signer(C2paSigner *c_signer) : signer(c_signer) {
            if (!c_signer) {
                throw C2paException("Signer can not be null");
            }
        }

        /// @brief Create a Signer from signing credentials.
        /// @param alg Signing algorithm name (e.g., "ps256", "es256").
        /// @param sign_cert Signing certificate in PEM format.
        /// @param private_key Private key in PEM format.
        /// @param tsa_uri Optional timestamp authority URI.
        /// @throws C2paException if signer creation fails.
        Signer(const std::string &alg, const std::string &sign_cert, const std::string &private_key, const std::optional<std::string> &tsa_uri = std::nullopt);

        Signer(const Signer&) = delete;

        Signer& operator=(const Signer&) = delete;

        /// @brief Move constructor.
        /// @param other Signer to move from.
        Signer(Signer&& other) noexcept : signer(std::exchange(other.signer, nullptr)) {
        }

        Signer& operator=(Signer&& other) noexcept {
            if (this != &other) {
                c2pa_free(signer);
                signer = std::exchange(other.signer, nullptr);
            }
            return *this;
        }

        ~Signer();

        /// @brief Get the size to reserve for a signature for this Signer.
        /// @return Reserved size for the signature in bytes.
        uintptr_t reserve_size();

        /// @brief Get the underlying C2paSigner pointer.
        /// @return Pointer to the C2paSigner object.
        C2paSigner *c2pa_signer() const noexcept;
    };

    /// @brief Builder class for creating a manifest.
    /// @details This class is used to create a manifest from a json std::string and add resources and ingredients to the manifest.
    class C2PA_CPP_API Builder
    {
    private:
        C2paBuilder *builder;
        std::shared_ptr<IContextProvider> context_ref;

        void init_from_context(IContextProvider& context);
        void init_from_context(IContextProvider& context, const std::string &manifest_json);

    public:
        /// @brief Create a Builder from a context with an empty manifest.
        /// @param context Context provider; used at construction to configure settings.
        /// @throws C2paException if context.is_valid() returns false,
        ///         or for other errors encountered by the C2PA library.
        /// @deprecated Use Builder(std::shared_ptr<IContextProvider>) instead.
        ///             The reference overload does not extend the lifetime of the context, which can
        ///             be problematic when progress callbacks fire after the context is destroyed.
        [[deprecated("Use Builder(std::shared_ptr<IContextProvider>) instead. "
                     "The reference overload does not extend the lifetime of the context, which can "
                     "be problematic when progress callbacks fire after the context is destroyed.")]]
        explicit Builder(IContextProvider& context);

        /// @brief Create a Builder from a context and manifest JSON string.
        /// @param context Context provider; used at construction to configure settings.
        /// @param manifest_json The manifest JSON string.
        /// @throws C2paException if context.is_valid() returns false,
        ///         or for other errors encountered by the C2PA library.
        /// @deprecated Use Builder(std::shared_ptr<IContextProvider>, manifest_json) instead.
        ///             The reference overload does not extend the lifetime of the context, which can
        ///             be problematic when progress callbacks fire after the context is destroyed.
        [[deprecated("Use Builder(std::shared_ptr<IContextProvider>, manifest_json) instead. "
                     "The reference overload does not extend the lifetime of the context, which can "
                     "be problematic when progress callbacks fire after the context is destroyed.")]]
        Builder(IContextProvider& context, const std::string &manifest_json);

        /// @brief Create a Builder from a shared context with an empty manifest.
        /// @details The Builder retains a shared reference to the context, keeping it
        ///          alive for the lifetime of the Builder. This is the preferred
        ///          constructor when the Context may be destroyed before the Builder.
        /// @param context Shared context provider.
        /// @throws C2paException if context is null or context->is_valid() returns false.
        explicit Builder(std::shared_ptr<IContextProvider> context);

        /// @brief Create a Builder from a shared context and manifest JSON string.
        /// @details The Builder retains a shared reference to the context, keeping it
        ///          alive for the lifetime of the Builder.
        /// @param context Shared context provider.
        /// @param manifest_json The manifest JSON string.
        /// @throws C2paException if context is null or context->is_valid() returns false.
        Builder(std::shared_ptr<IContextProvider> context, const std::string &manifest_json);

        /// @brief Create a Builder from a manifest JSON string (will use global settings if any loaded).
        /// @param manifest_json The manifest JSON string.
        /// @throws C2paException for errors encountered by the C2PA library.
        /// @deprecated Use Builder(IContextProvider& context, manifest_json) instead.
        [[deprecated("Use Builder(IContextProvider& context, manifest_json) instead")]]
        Builder(const std::string &manifest_json);

        /// @brief Create a Builder from a raw C FFI builder.
        /// @param builder Raw C2paBuilder pointer to wrap.
        /// @throws C2paException if builder is nullptr.
        explicit Builder(C2paBuilder *builder);

        Builder(const Builder&) = delete;

        Builder& operator=(const Builder&) = delete;

        Builder(Builder&& other) noexcept
            : builder(std::exchange(other.builder, nullptr)),
              context_ref(std::move(other.context_ref)) {
        }

        Builder& operator=(Builder&& other) noexcept {
            if (this != &other) {
                c2pa_free(builder);
                builder = std::exchange(other.builder, nullptr);
                context_ref = std::move(other.context_ref);
            }
            return *this;
        }

        ~Builder();

        /// @brief  Get the underlying C2paBuilder pointer.
        /// @return Pointer managed by this wrapper.
        C2paBuilder *c2pa_builder() const noexcept;

        /// @brief Set or update the manifest definition.
        /// @param manifest_json The manifest JSON string.
        /// @return Reference to this Builder for method chaining.
        /// @throws C2pa::C2paException for errors encountered by the C2PA library.
        Builder& with_definition(const std::string &manifest_json);

        /// @brief Set the no-embed flag to prevent embedding the manifest in the asset.
        /// @details When set, the manifest will be stored externally rather than embedded.
        void set_no_embed();

        /// @brief Set the remote URL.
        /// @param remote_url The remote URL to set.
        /// @throws C2paException for errors encountered by the C2PA library.
        void set_remote_url(const std::string &remote_url);

        /// @brief Set the base path for loading resources from files.
        /// @details When set, resources are loaded from files relative to this path.
        ///          If not set, resources are loaded from memory.
        /// @param base_path The base directory path.
        /// @throws C2paException for errors encountered by the C2PA library.
        /// @deprecated This method is planned to be deprecated in a future release.
        ///             Usage should be limited and temporary. Use add_resource instead.
        void set_base_path(const std::string &base_path);

        /// @brief Add a resource to the builder from a stream.
        /// @param uri The URI identifier for the resource.
        /// @param source The input stream to read the resource from.
        /// @throws C2paException for errors encountered by the C2PA library.
        void add_resource(const std::string &uri, std::istream &source);

        /// @brief Add a resource to the builder from a file.
        /// @param uri The URI identifier for the resource.
        /// @param source_path The path to the resource file.
        /// @throws C2paException for errors encountered by the C2PA library.
        /// @note Prefer using the streaming APIs if possible.
        void add_resource(const std::string &uri, const std::filesystem::path &source_path);

        /// @brief Add an ingredient to the builder from a stream.
        /// @param ingredient_json Any fields of the ingredient you want to define.
        /// @param format The mime format of the ingredient.
        /// @param source The input stream to read the ingredient from.
        /// @throws C2paException for errors encountered by the C2PA library.
        void add_ingredient(const std::string &ingredient_json, const std::string &format, std::istream &source);

        /// @brief Add an ingredient to the builder from a file.
        /// @param ingredient_json Any fields of the ingredient you want to define.
        /// @param source_path The path to the ingredient file.
        /// @throws C2paException for errors encountered by the C2PA library.
        /// @note Prefer using the streaming APIs if possible.
        void add_ingredient(const std::string &ingredient_json, const std::filesystem::path &source_path);

        /// @brief Add an action to the manifest.
        /// @param action_json JSON string containing the action data.
        /// @throws C2paException for errors encountered by the C2PA library.
        void add_action(const std::string &action_json);

        /// @brief Set the intent for this Builder, controlling what kind of manifest to create.
        /// @param intent The intent type: Create, Edit, or Update.
        /// @param digital_source_type Required for Create intent. Describes how the asset was produced.
        ///        Defaults to Empty.
        /// @throws C2paException if the intent cannot be set.
        void set_intent(C2paBuilderIntent intent, C2paDigitalSourceType digital_source_type = Empty);

        /// @brief Sign an input stream and write the signed data to an output stream.
        /// @param format The mime format of the output stream.
        /// @param source The input stream to sign.
        /// @param dest The output stream to write the signed data to.
        /// @param signer The Signer object to use for signing.
        /// @return A vector containing the signed manifest bytes.
        /// @throws C2paException for errors encountered by the C2PA library.
        /// @deprecated Use sign(const string&, std::istream&, std::iostream&, Signer&) instead.
        std::vector<unsigned char> sign(const std::string &format, std::istream &source, std::ostream &dest, Signer &signer);

        /// @brief Sign an input stream and write the signed data to an I/O stream.
        /// @param format The mime format of the output.
        /// @param source The input stream to sign.
        /// @param dest The I/O stream to write the signed data to.
        /// @param signer The Signer object to use for signing.
        /// @return A vector containing the signed manifest bytes.
        /// @throws C2paException for errors encountered by the C2PA library.
        std::vector<unsigned char> sign(const std::string &format, std::istream &source, std::iostream &dest, Signer &signer);

        /// @brief Sign a file and write the signed data to an output file.
        /// @param source_path The path to the file to sign.
        /// @param dest_path The path to write the signed file to.
        /// @param signer The signer object to use for signing.
        /// @return A vector containing the signed manifest bytes.
        /// @throws C2paException for errors encountered by the C2PA library.
        /// @note Prefer using the streaming APIs if possible.
        std::vector<unsigned char> sign(const std::filesystem::path &source_path, const std::filesystem::path &dest_path, Signer &signer);

        /// @brief Sign using the signer from the Builder's Context.
        /// @details The Signer may have been set programmatically via
        ///          ContextBuilder::with_signer(), or configured in settings JSON.
        ///          If both programmatic and settings signers are present,
        ///          the programmatic signer takes priority.
        /// @param format The mime format of the output.
        /// @param source The input stream to sign.
        /// @param dest The I/O stream to write the signed data to.
        /// @return A vector containing the signed manifest bytes.
        /// @throws C2paException if the context has no signer or on other errors.
        std::vector<unsigned char> sign(const std::string &format, std::istream &source, std::iostream &dest);

        /// @brief Sign a file using the signer from the Builder's Context.
        /// @details The signer may have been set programmatically via
        ///          ContextBuilder::with_signer(), or configured in settings JSON.
        ///          If both programmatic and settings signers are present,
        ///          the programmatic signer takes priority.
        /// @param source_path The path to the file to sign.
        /// @param dest_path The path to write the signed file to.
        /// @return A vector containing the signed manifest bytes.
        /// @throws C2paException if the context has no signer or on other errors.
        std::vector<unsigned char> sign(const std::filesystem::path &source_path, const std::filesystem::path &dest_path);

        /// @brief Create a Builder from an archived Builder stream.
        /// @param archive The input stream to read the archive from.
        /// @return A new Builder instance loaded from the archive.
        /// @throws C2paException for errors encountered by the C2PA library.
        static Builder from_archive(std::istream &archive);

        /// @brief Create a Builder from an archive.
        /// @param archive_path The path to the archive file.
        /// @return A new Builder instance loaded from the archive.
        /// @throws C2paException for errors encountered by the C2PA library.
        /// @note Prefer using the streaming APIs if possible.
        static Builder from_archive(const std::filesystem::path &archive_path);

        /// @brief Load an archive into this builder.
        /// @details Replaces the current definition with the archived builder state.
        /// @param archive The input stream to read the archive from.
        /// @return Reference to this builder for method chaining.
        /// @throws C2paException for errors encountered by the C2PA library.
        /// @note This allows setting a context before loading the archive, preserving context settings.
        Builder& with_archive(std::istream &archive);

        /// @brief Write the builder to an archive stream.
        /// @param dest The output stream to write the archive to.
        /// @throws C2paException for errors encountered by the C2PA library.
        void to_archive(std::ostream &dest);

        /// @brief Write the builder to an archive file.
        /// @param dest_path The path to write the archive file to.
        /// @throws C2paException for errors encountered by the C2PA library.
        /// @note Prefer using the streaming APIs if possible.
        void to_archive(const std::filesystem::path &dest_path);

        /// @brief Write a single-ingredient archive for the named ingredient.
        /// @param ingredient_id The ingredient's `label` if set, otherwise its `instance_id`,
        ///        as supplied to add_ingredient.
        /// @param dest The output stream to write the ingredient archive to.
        /// @note Requires the `generate_c2pa_archive` context setting to be enabled
        ///       (enabled by default).
        /// @throws C2paException for errors encountered by the C2PA library.
        void write_ingredient_archive(const std::string &ingredient_id, std::ostream &dest);

        /// @brief Add an ingredient to this builder from a per-ingredient archive stream.
        /// @param archive The input stream containing the archive produced by write_ingredient_archive.
        /// @throws C2paException for errors encountered by the C2PA library.
        void add_ingredient_from_archive(std::istream &archive);

        /// @brief Create a hashed placeholder from the builder.
        /// @param reserved_size The size required for a signature from the intended signer (in bytes).
        /// @param format The mime format or extension of the asset.
        /// @return A vector containing the hashed placeholder bytes.
        /// @throws C2paException for errors encountered by the C2PA library.
        std::vector<unsigned char> data_hashed_placeholder(uintptr_t reserved_size, const std::string &format);

        /// @brief Sign a Builder using the specified signer and data hash.
        /// @param signer The signer to use for signing.
        /// @param data_hash The data hash ranges to sign (must contain hashes unless an asset is provided).
        /// @param format The mime format for embedding. Use "c2pa" for an unformatted result.
        /// @param asset Optional asset to hash according to the data_hash information.
        /// @return A vector containing the signed embeddable data.
        /// @throws C2paException for errors encountered by the C2PA library.
        std::vector<unsigned char> sign_data_hashed_embeddable(Signer &signer, const std::string &data_hash, const std::string &format, std::istream *asset = nullptr);

        /// @brief Convert unformatted manifest data to an embeddable format.
        /// @param format The format for embedding.
        /// @param data Unformatted manifest data from sign_data_hashed_embeddable using "c2pa" format.
        /// @return A formatted copy of the data.
        static std::vector<unsigned char> format_embeddable(const std::string &format, std::vector<unsigned char> &data);

        /// @brief Check if the given format requires a placeholder embedding step.
        /// @details Returns false for BoxHash-capable formats when prefer_box_hash is enabled in
        ///          the context settings (no placeholder needed — hash covers the full asset).
        ///          Always returns true for BMFF formats (MP4, etc.) regardless of settings.
        /// @param format The MIME type or extension of the asset (e.g. "image/jpeg", "video/mp4").
        /// @return true if placeholder() must be called and embedded before sign_embeddable(); false otherwise.
        /// @throws C2paException on error.
        bool needs_placeholder(const std::string &format);

        /// @brief Create a composed placeholder manifest to embed in the asset.
        /// @details The signer (and its reserve size) are obtained from the Builder's Context.
        ///          For BMFF assets, the placeholder includes a BmffHash assertion with
        ///          default exclusions for the manifest UUID box.
        ///          Returns empty bytes for formats that do not need a placeholder (BoxHash).
        ///          The placeholder size is stored internally so sign_embeddable() returns bytes
        ///          of exactly the same size, enabling in-place patching.
        /// @param format The MIME type or extension of the asset (e.g. "image/jpeg", "video/mp4").
        /// @return Composed placeholder bytes ready to embed into the asset.
        /// @throws C2paException on error.
        std::vector<unsigned char> placeholder(const std::string &format);

        /// @brief Register the byte ranges where the placeholder was embedded (DataHash workflow).
        /// @details Call this after embedding the placeholder bytes into the asset and before
        ///          update_hash_from_stream(). The exclusions replace the dummy ranges set by
        ///          placeholder() so the asset hash covers all bytes except the manifest slot.
        ///          Exclusions are (start, length) pairs in asset byte coordinates.
        /// @param exclusions Vector of (start, length) pairs describing the embedded placeholder region.
        /// @throws C2paException if no DataHash assertion exists or on other error.
        void set_data_hash_exclusions(const std::vector<std::pair<uint64_t, uint64_t>> &exclusions);

        /// @brief Compute and store the asset hash by reading a stream.
        /// @details Automatically detects the hard binding type from the builder state:
        ///          - DataHash: uses exclusion ranges already registered via set_data_hash_exclusions().
        ///          - BmffHash: uses path-based exclusions from the BMFF assertion (UUID box, mdat).
        ///          - BoxHash: hashes each format-specific box individually.
        ///          Call set_data_hash_exclusions() before this for DataHash workflows.
        /// @param format The MIME type or extension of the asset (e.g. "image/jpeg", "video/mp4").
        /// @param stream The asset stream to hash. Must include the embedded placeholder bytes.
        /// @throws C2paException on error.
        void update_hash_from_stream(const std::string &format, std::istream &stream);

        /// @brief Sign and return the final manifest bytes, ready for embedding.
        /// @details Operates in two modes:
        ///          - Placeholder mode (after placeholder()): zero-pads the signed manifest to the
        ///            pre-committed placeholder size, enabling in-place patching of the asset.
        ///          - Direct mode (no placeholder): returns the actual signed manifest size.
        ///            Requires a valid hard binding assertion (set via update_hash_from_stream()).
        ///          The signer is obtained from the Builder's Context.
        /// @param format The MIME type or extension of the asset (e.g. "image/jpeg", "video/mp4").
        /// @return Signed manifest bytes ready to embed into the asset.
        /// @throws C2paException on error.
        std::vector<unsigned char> sign_embeddable(const std::string &format);

        /// @brief Get a list of mime types that the Builder supports.
        /// @return Vector of supported MIME type strings.
        static std::vector<std::string> supported_mime_types();

    private:
        explicit Builder(std::istream &archive);
    };
}

// Restore warnings
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // C2PA_H
