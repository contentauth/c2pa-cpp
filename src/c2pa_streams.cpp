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

/// @file   c2pa_streams.cpp
/// @brief  Stream wrapper implementations (CppIStream, CppOStream, CppIOStream).

#include "c2pa.hpp"
#include "c2pa_internal.hpp"

namespace c2pa
{
    CppStream::~CppStream()
    {
        if (c_stream != nullptr) {
            c2pa_release_stream(c_stream);
        }
    }

    CppIStream::~CppIStream() = default;

    intptr_t CppIStream::reader(StreamContext *context, uint8_t *buffer, intptr_t size)
    {
        return detail::stream_reader<std::istream>(context, buffer, size);
    }

    intptr_t CppIStream::seeker(StreamContext *context, intptr_t offset, C2paSeekMode whence)
    {
        return detail::stream_seeker<std::istream>(context, offset, whence);
    }

    intptr_t CppIStream::writer(StreamContext *context, const uint8_t *buffer, intptr_t size)
    {
        (void)context;
        (void)buffer;
        (void)size;
        return stream_error_return(StreamError::InvalidArgument);
    }

    intptr_t CppIStream::flusher(StreamContext *context)
    {
        (void)context;
        return stream_error_return(StreamError::InvalidArgument);
    }

    CppOStream::~CppOStream() = default;

    intptr_t CppOStream::reader(StreamContext *context, uint8_t *buffer, intptr_t size)
    {
        (void) context;
        (void) buffer;
        (void) size;
        return stream_error_return(StreamError::InvalidArgument);
    }

    intptr_t CppOStream::seeker(StreamContext *context, intptr_t offset, C2paSeekMode whence)
    {
        return detail::stream_seeker<std::ostream>(context, offset, whence);
    }

    intptr_t CppOStream::writer(StreamContext *context, const uint8_t *buffer, intptr_t size)
    {
        return detail::stream_writer<std::ostream>(context, buffer, size);
    }

    intptr_t CppOStream::flusher(StreamContext *context)
    {
        return detail::stream_flusher<std::ostream>(context);
    }

    CppIOStream::~CppIOStream() = default;

    intptr_t CppIOStream::reader(StreamContext *context, uint8_t *buffer, intptr_t size)
    {
        return detail::stream_reader<std::iostream>(context, buffer, size);
    }

    intptr_t CppIOStream::seeker(StreamContext *context, intptr_t offset, C2paSeekMode whence)
    {
        return detail::stream_seeker<std::iostream>(context, offset, whence);
    }

    intptr_t CppIOStream::writer(StreamContext *context, const uint8_t *buffer, intptr_t size)
    {
        return detail::stream_writer<std::iostream>(context, buffer, size);
    }

    intptr_t CppIOStream::flusher(StreamContext *context)
    {
        return detail::stream_flusher<std::iostream>(context);
    }
} // namespace c2pa
