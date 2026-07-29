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

/// @file   c2pa_internal.hpp
/// @brief  Internal implementation details shared across c2pa_cpp source files.
/// @details This header is private to the library implementation and not installed,
///          as it is used to share code inside c2pa_cpp SDK.

#ifndef C2PA_INTERNAL_HPP
#define C2PA_INTERNAL_HPP

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <memory>

#include "c2pa.h"
#include "c2pa.hpp"

namespace c2pa {
namespace detail {

/// @brief True if the C2PA error message indicates no JUMBF / manifest in the asset (ManifestNotFound).
inline bool error_indicates_manifest_not_found(const char* message) noexcept {
    return message != nullptr && std::strstr(message, "ManifestNotFound") != nullptr;
}

/// @brief Converts a C array of C strings to a std::vector of std::string.
/// @param mime_types Pointer to an array of C strings (const char*).
/// @param count Number of elements in the array.
/// @return A std::vector containing the strings from the input array.
/// @details This function takes ownership of the input array and frees it
///          using c2pa_free_string_array().
inline std::vector<std::string> c_mime_types_to_vector(const char* const* mime_types, uintptr_t count) {
  std::vector<std::string> result;
  if (mime_types == nullptr) { return result; }

  try {
    result.reserve(count);
    for(uintptr_t i = 0; i < count; i++) {
      if (mime_types[i] != nullptr) {
        result.emplace_back(mime_types[i]);
      }
    }
  } catch (...) {
    c2pa_free_string_array(mime_types, count);
    throw;
  }

  c2pa_free_string_array(mime_types, count);
  return result;
}

/// Maps C2PA seek mode to std::ios seek direction.
constexpr std::ios_base::seekdir whence_to_seekdir(C2paSeekMode whence) noexcept {
    switch (whence) {
        case C2paSeekMode::Start:   return std::ios_base::beg;
        case C2paSeekMode::Current: return std::ios_base::cur;
        case C2paSeekMode::End:     return std::ios_base::end;
        default:                    return std::ios_base::beg;
    }
}

/// Check if stream is in valid state for I/O operations
template<typename Stream>
inline bool is_stream_usable(Stream* s) noexcept {
    return s && !s->bad();
}

/// Traits (templated): how to seek and get position for a given stream type.
template<typename Stream>
struct StreamSeekTraits;

template<>
struct StreamSeekTraits<std::istream> {
    static void seek(std::istream* s, intptr_t offset, std::ios_base::seekdir dir) {
        s->seekg(offset, dir);
    }
    static int64_t tell(std::istream* s) {
        return static_cast<int64_t>(s->tellg());
    }
};

template<>
struct StreamSeekTraits<std::ostream> {
    static void seek(std::ostream* s, intptr_t offset, std::ios_base::seekdir dir) {
        s->seekp(offset, dir);
    }
    static int64_t tell(std::ostream* s) {
        return static_cast<int64_t>(s->tellp());
    }
};

template<>
struct StreamSeekTraits<std::iostream> {
    static void seek(std::iostream* s, intptr_t offset, std::ios_base::seekdir dir) {
        s->seekg(offset, dir);
        s->seekp(offset, dir);
    }
    static int64_t tell(std::iostream* s) {
        return static_cast<int64_t>(s->tellp());
    }
};

/// Seeker impl.
/// Exceptions must not unwind into Rust/C, so any throw
/// is converted to an IoError return.
template<typename Stream>
intptr_t stream_seeker(StreamContext* context, intptr_t offset, C2paSeekMode whence) {
    try {
        auto* stream = reinterpret_cast<Stream*>(context);
        if (!is_stream_usable(stream)) {
            return stream_error_return(StreamError::IoError);
        }
        const std::ios_base::seekdir dir = whence_to_seekdir(whence);
        stream->clear();
        StreamSeekTraits<Stream>::seek(stream, offset, dir);
        if (stream->fail()) {
            return stream_error_return(StreamError::InvalidArgument);
        }
        if (stream->bad()) {
            return stream_error_return(StreamError::IoError);
        }
        const int64_t pos = StreamSeekTraits<Stream>::tell(stream);
        if (pos < 0) {
            return stream_error_return(StreamError::IoError);
        }
        return static_cast<intptr_t>(pos);
    } catch (...) {
        return stream_error_return(StreamError::IoError);
    }
}

/// Reader impl.
/// Exceptions must not unwind into Rust/C, so any throw
/// is converted to an IoError return.
template<typename Stream>
intptr_t stream_reader(StreamContext* context, uint8_t* buffer, intptr_t size) {
    if (!context || !buffer) {
        return stream_error_return(StreamError::InvalidArgument);
    }
    if (size < 0) {
        return stream_error_return(StreamError::InvalidArgument);
    }
    if (size == 0) {
        return 0;
    }
    try {
        auto* stream = reinterpret_cast<Stream*>(context);
        if (!is_stream_usable(stream)) {
            return stream_error_return(StreamError::IoError);
        }
        stream->read(reinterpret_cast<char*>(buffer), size);
        if (stream->fail()) {
            if (!stream->eof()) {
                return stream_error_return(StreamError::InvalidArgument);
            }
        }
        if (stream->bad()) {
            return stream_error_return(StreamError::IoError);
        }
        return static_cast<intptr_t>(stream->gcount());
    } catch (...) {
        return stream_error_return(StreamError::IoError);
    }
}

/// Get stream from context, used by writer and flusher.
/// Exceptions must not unwind into Rust/C, so any throw
/// is converted to an IoError return.
template<typename Stream, typename Op>
intptr_t stream_op(StreamContext* context, Op op) {
    try {
        auto* stream = reinterpret_cast<Stream*>(context);
        if (!is_stream_usable(stream)) {
            return stream_error_return(StreamError::IoError);
        }
        const intptr_t result = op(stream);
        if (stream->fail()) {
            return stream_error_return(StreamError::InvalidArgument);
        }
        if (stream->bad()) {
            return stream_error_return(StreamError::IoError);
        }
        return result;
    } catch (...) {
        return stream_error_return(StreamError::IoError);
    }
}

/// Writer impl.
template<typename Stream>
intptr_t stream_writer(StreamContext* context, const uint8_t* buffer, intptr_t size) {
    return stream_op<Stream>(context, [buffer, size](Stream* s) {
        s->write(reinterpret_cast<const char*>(buffer), size);
        return size;
    });
}

/// Flusher impl.
template<typename Stream>
intptr_t stream_flusher(StreamContext* context) {
    return stream_op<Stream>(context, [](Stream* s) {
        s->flush();
        return 0;
    });
}

/// @brief Open a binary file stream with error handling
/// @tparam StreamType std::ifstream or std::ofstream
/// @param path Path to the file
/// @return Unique pointer to opened stream
template<typename StreamType>
inline std::unique_ptr<StreamType> open_file_binary(const std::filesystem::path &path)
{
    auto stream = std::make_unique<StreamType>(
        path,
        std::ios_base::binary
    );
    if (!stream->is_open()) {
        throw C2paException("Failed to open file: " + path.string());
    }
    return stream;
}

/// @brief Extract file extension without the leading dot
/// @param path Filesystem path
/// @return Extension string (e.g., "jpg" not ".jpg")
inline std::string extract_file_extension(const std::filesystem::path &path) noexcept {
    auto ext = path.extension().string();
    return ext.empty() ? "" : ext.substr(1);
}

/// @brief Format asking the library to determine the container type from content.
/// @details The C API rejects a null format, so absent must be spelled empty.
inline constexpr const char *kDetectFormatFromContent = "";

/// @brief ASCII whitespace ignored around a format.
/// @details Formats are MIME types or extensions, so non-ASCII spaces are
///          deliberately excluded.
inline constexpr const char *kFormatWhitespace = " \t\n\r\f\v";

/// @brief The format with surrounding whitespace removed, empty when blank.
[[nodiscard]] inline std::string_view trim_format(const std::string &format) noexcept {
    const auto first = format.find_first_not_of(kFormatWhitespace);
    if (first == std::string::npos) {
        return {};  // Empty or all whitespace.
    }
    const auto last = format.find_last_not_of(kFormatWhitespace);
    return std::string_view(format).substr(first, last - first + 1);
}

/// @brief Trim and lowercase a caller-supplied format; empty when @p format is blank.
/// @details Empty means detection from content, so this alone is enough wherever
///          a blank format is allowed to request that.
[[nodiscard]] inline std::string normalize_format(const std::string &format) {
    const std::string_view trimmed = trim_format(format);
    std::string normalized(trimmed);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized;
}

/// @brief Resolve a format that must be stated explicitly.
///        These paths never infer the container type from content,
///        so a blank format is rejected.
/// @return @p format trimmed and lowercased.
/// @throws C2paException if @p format is blank.
[[nodiscard]] inline std::string resolve_format(const std::string &format) {
    std::string normalized = normalize_format(format);
    if (normalized.empty()) {
        throw C2paException("An explicit format is required.");
    }
    return normalized;
}

/// @brief Convert C string result to C++ string with cleanup
/// @param c_result Raw C string from C API
/// @return C++ string (throws if null)
template<typename T>
inline std::string c_string_to_string(T* c_result) {
    if (c_result == nullptr) {
        throw C2paException();
    }
    std::string str(c_result);
    c2pa_free(c_result);
    return str;
}

/// @brief Convert C byte array result to C++ vector
/// @param data Raw byte array from C API
/// @param size Size of the byte array (result from C API call)
/// @return Vector containing the bytes (throws if null or negative size)
/// @details This helper extracts the pattern of checking C API results,
///          copying to a vector, and freeing the C-allocated memory.
///          The C API contract is: if result < 0, the operation failed. A null
///          data pointer with size == 0 is a valid empty result (the C API
///          returns null for empty byte arrays).
inline std::vector<unsigned char> to_byte_vector(const unsigned char* data, int64_t size) {
    if (size < 0 || (data == nullptr && size > 0)) {
        c2pa_free(data);  // May be null or allocated, c2pa_free handles both
        throw C2paException();
    }
    if (size == 0) {
        c2pa_free(data);
        return {};
    }

    auto result = std::vector<unsigned char>(data, data + size);
    c2pa_free(data);
    return result;
}

} // namespace detail
} // namespace c2pa

#endif // C2PA_INTERNAL_HPP
