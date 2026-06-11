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

/// @file   c2pa_signer.cpp
/// @brief  Signer class implementation.

#include <vector>

#include "c2pa.hpp"
#include "c2pa_internal.hpp"

namespace {

intptr_t signer_passthrough(const void *context, const unsigned char *data, uintptr_t len, unsigned char *signature, uintptr_t sig_max_len)
{
  if (data == nullptr || signature == nullptr)
  {
    return c2pa::stream_error_return(c2pa::StreamError::InvalidArgument);
  }
  try
  {
    // the context is a pointer to the C++ callback function
    auto* callback = reinterpret_cast<c2pa::SignerFunc*>(const_cast<void*>(context));
    std::vector<uint8_t> data_vec(data, data + len);
    std::vector<uint8_t> signature_vec = (callback)(data_vec);
    if (signature_vec.size() > sig_max_len)
    {
      return c2pa::stream_error_return(c2pa::StreamError::NoBufferSpace);
    }
    std::copy(signature_vec.begin(), signature_vec.end(), signature);
    return signature_vec.size();
  }
  catch (std::exception const &e)
  {
    // todo pass exceptions to Rust error handling
    (void)e;
    // printf("Error: signer_passthrough - %s\n", e.what());
    return static_cast<intptr_t>(c2pa::OperationResult::Error);
  }
  catch (...)
  {
    // printf("Error: signer_passthrough - unknown C2paException\n");
    return static_cast<intptr_t>(c2pa::OperationResult::Error);
  }
}

} // anonymous namespace

namespace c2pa
{
    const char *Signer::validate_tsa_uri(const std::string &tsa_uri)
    {
        return tsa_uri.empty() ? nullptr : tsa_uri.c_str();
    }

    const char *Signer::validate_tsa_uri(const std::optional<std::string> &tsa_uri)
    {
        return (tsa_uri && !tsa_uri->empty()) ? tsa_uri->c_str() : nullptr;
    }

    Signer::Signer(SignerFunc *callback, C2paSigningAlg alg, const std::string &sign_cert, const std::string &tsa_uri)
    {
        // Pass the C++ callback as a context to our static callback wrapper.
        signer = c2pa_signer_create((const void *)callback, &signer_passthrough, alg, sign_cert.c_str(), validate_tsa_uri(tsa_uri));
        if (signer == nullptr)
        {
            throw C2paException();
        }
    }

    Signer::Signer(const std::string &alg, const std::string &sign_cert, const std::string &private_key, const std::optional<std::string> &tsa_uri)
    {
        auto info = C2paSignerInfo { alg.c_str(), sign_cert.c_str(), private_key.c_str(), validate_tsa_uri(tsa_uri) };
        signer = c2pa_signer_from_info(&info);
        if (signer == nullptr)
        {
            throw C2paException();
        }
    }

    Signer::~Signer()
    {
        c2pa_free(signer);
    }

    /// @brief  Get the C2paSigner
    C2paSigner *Signer::c2pa_signer() const noexcept
    {
        return signer;
    }

    /// @brief  Get the size to reserve for a signature for this signer.
    uintptr_t Signer::reserve_size()
    {
        int64_t result = c2pa_signer_reserve_size(signer);
        if (result < 0) {
            throw C2paException();
        }
        return static_cast<uintptr_t>(result);
    }
} // namespace c2pa
