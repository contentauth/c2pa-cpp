// Copyright 2026 Adobe. All rights reserved.
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

#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <sstream>
#include <type_traits>

#include "include/test_utils.hpp"
#include "../src/c2pa_internal.hpp"

namespace {

std::string manifest_definition()
{
    return c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
}

std::string signing_certs()
{
    return c2pa_test::read_text_file(c2pa_test::get_fixture_path("ed25519.pub"));
}

const unsigned char* native_bytes()
{
    const auto key = c2pa_test::read_text_file(c2pa_test::get_fixture_path("ed25519.pem"));
    const unsigned char message[] = {1, 2, 3};
    auto* bytes = c2pa_ed25519_sign(message, sizeof(message), key.c_str());
    if (!bytes) {
        throw c2pa::C2paException();
    }
    return bytes;
}

c2pa::Signer direct_signer()
{
    return c2pa::Signer("Ed25519", signing_certs(),
        c2pa_test::read_text_file(c2pa_test::get_fixture_path("ed25519.pem")));
}

c2pa::Signer callback_signer(std::shared_ptr<int> calls)
{
    auto key = c2pa_test::read_text_file(c2pa_test::get_fixture_path("ed25519.pem"));
    return c2pa::Signer(
        [key, calls](c2pa::ConstBytes data, c2pa::MutableBytes dest) -> intptr_t {
            ++*calls;
            auto signature = c2pa::detail::to_byte_buffer(
                c2pa_ed25519_sign(data.data, data.size, key.c_str()), 64);
            if (dest.size < signature.size()) {
                return -1;
            }
            std::copy(signature.begin(), signature.end(), dest.data);
            return static_cast<intptr_t>(signature.size());
        }, C2paSigningAlg::Ed25519, signing_certs(), "");
}

std::stringstream asset()
{
    return std::stringstream(c2pa_test::read_text_file(c2pa_test::get_fixture_path("A.jpg")),
                             std::ios::in | std::ios::out | std::ios::binary);
}

void expect_valid(std::iostream& signed_asset)
{
    signed_asset.clear();
    signed_asset.seekg(0);
    auto context = std::make_shared<c2pa::Context>();
    auto reader = c2pa::Reader(context, "image/jpeg", signed_asset);
    const auto report = nlohmann::json::parse(reader.json());
    const auto state = report.at("validation_state");
    EXPECT_TRUE(state == "Valid" || state == "Trusted") << report.dump(2);
}

static_assert(!std::is_copy_constructible<c2pa::ByteBuffer>::value);
static_assert(!std::is_copy_assignable<c2pa::ByteBuffer>::value);
static_assert(std::is_nothrow_move_constructible<c2pa::ByteBuffer>::value);
static_assert(std::is_nothrow_move_assignable<c2pa::ByteBuffer>::value);

TEST(ByteBuffer, EmptyAndInvalidResults)
{
    c2pa::ByteBuffer empty;
    EXPECT_EQ(empty.data(), nullptr);
    EXPECT_EQ(empty.begin(), empty.end());
    EXPECT_EQ(empty.size(), 0u);
    EXPECT_TRUE(empty.empty());
    EXPECT_TRUE(empty.to_vector().empty());
    EXPECT_TRUE(c2pa::detail::to_byte_buffer(nullptr, 0).empty());
    EXPECT_THROW(c2pa::detail::to_byte_buffer(nullptr, 1), c2pa::C2paException);
    EXPECT_THROW(c2pa::detail::to_byte_buffer(nullptr, -1), c2pa::C2paException);

    auto* bytes = native_bytes();
    EXPECT_THROW(c2pa::detail::to_byte_buffer(bytes, -1), c2pa::C2paException);
    EXPECT_NE(c2pa_free(bytes), 0);
    bytes = native_bytes();
    EXPECT_TRUE(c2pa::detail::to_byte_buffer(bytes, 0).empty());
    EXPECT_NE(c2pa_free(bytes), 0);
}

TEST(ByteBuffer, IdentityMovesAndExactlyOnceRelease)
{
    auto* bytes = native_bytes();
    const std::vector<unsigned char> expected(bytes, bytes + 64);
    {
        auto original = c2pa::detail::to_byte_buffer(bytes, 64);
        EXPECT_EQ(original.data(), bytes);
        EXPECT_EQ(original.view().data, bytes);
        EXPECT_EQ(original.view().size, 64u);
        EXPECT_EQ(original.to_vector(), expected);
        EXPECT_EQ(c2pa::detail::to_byte_vector(native_bytes(), 64), expected);
        EXPECT_EQ(std::vector<unsigned char>(original.begin(), original.end()), expected);
        c2pa::ByteBuffer moved(std::move(original));
        EXPECT_TRUE(original.empty());
        EXPECT_EQ(original.data(), nullptr);
        EXPECT_EQ(moved.data(), bytes);
        auto* replaced = native_bytes();
        auto assigned = c2pa::detail::to_byte_buffer(replaced, 64);
        assigned = std::move(moved);
        EXPECT_NE(c2pa_free(replaced), 0);
        EXPECT_TRUE(moved.empty());
        auto& self = assigned;
        assigned = std::move(self);
        EXPECT_EQ(assigned.data(), bytes);
        EXPECT_EQ(assigned.to_vector(), expected);
    }
    EXPECT_NE(c2pa_free(bytes), 0);
}

TEST(ZeroCopyBuilder, AllStreamOverloadsRoundTrip)
{
    auto context = std::make_shared<c2pa::Context>(
        c2pa::Context::ContextBuilder().with_signer(direct_signer()).create_context());
    for (int mode = 0; mode < 3; ++mode) {
        auto source = asset();
        std::stringstream dest(std::ios::in | std::ios::out | std::ios::binary);
        auto signer = direct_signer();
        c2pa::ByteBuffer bytes;
        {
            c2pa::Builder builder(context, manifest_definition());
            if (mode == 0) {
                bytes = builder.sign_to_buffer("image/jpeg", source, dest, signer);
            } else if (mode == 1) {
                std::ostream& output = dest;
                bytes = builder.sign_to_buffer("image/jpeg", source, output, signer);
            } else {
                bytes = builder.sign_to_buffer("image/jpeg", source, dest);
            }
        }
        ASSERT_FALSE(bytes.empty());
        EXPECT_EQ(bytes.to_vector(), std::vector<unsigned char>(bytes.begin(), bytes.end()));
        expect_valid(dest);
    }
}

TEST(ZeroCopyBuilder, FileOverloadsRoundTrip)
{
    auto context = std::make_shared<c2pa::Context>(
        c2pa::Context::ContextBuilder().with_signer(direct_signer()).create_context());
    for (bool use_context : {false, true}) {
        auto path = std::filesystem::temp_directory_path() /
            ("c2pa-zero-copy-" + std::to_string(
                reinterpret_cast<uintptr_t>(context.get())) +
             (use_context ? "-context.jpg" : "-explicit.jpg"));
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); }
        } cleanup{path};
        c2pa::Builder builder(context, manifest_definition());
        auto signer = direct_signer();
        auto bytes = use_context
            ? builder.sign_to_buffer(c2pa_test::get_fixture_path("A.jpg"), path)
            : builder.sign_to_buffer(c2pa_test::get_fixture_path("A.jpg"), path, signer);
        EXPECT_FALSE(bytes.empty());
        std::fstream result(path, std::ios::in | std::ios::out | std::ios::binary);
        expect_valid(result);
    }
}

TEST(ZeroCopyBuilder, EmbeddableAndVectorEquivalence)
{
    auto context = std::make_shared<c2pa::Context>(
        c2pa::Context::ContextBuilder().with_signer(direct_signer()).create_context());
    c2pa::Builder builder(context, c2pa_test::prehashed_training_manifest());
    auto bytes = builder.sign_embeddable_to_buffer("application/c2pa");
    auto vector = builder.sign_embeddable("application/c2pa");
    auto reading_context = std::make_shared<c2pa::Context>(
        R"({"verify":{"verify_after_reading":false}})");
    auto assertions = [&](const unsigned char* data, size_t size) {
        std::istringstream input(std::string(reinterpret_cast<const char*>(data), size));
        c2pa::Reader reader(reading_context, "application/c2pa", input);
        auto report = nlohmann::json::parse(reader.json());
        return report["manifests"][report["active_manifest"].get<std::string>()]["assertions"];
    };
    // Independent signing calls generate fresh labels and assertion salts.
    // Compare decoded assertions here; the adapter test checks exact byte equality.
    EXPECT_EQ(assertions(bytes.data(), bytes.size()), assertions(vector.data(), vector.size()));
    EXPECT_THROW(builder.sign_embeddable_to_buffer(""), c2pa::C2paException);

    for (const auto* format : {"image/jpeg", "image/png"}) {
        c2pa::Builder buffered(context, manifest_definition());
        c2pa::Builder copied(context, manifest_definition());
        auto placeholder = buffered.placeholder_to_buffer(format);
        auto vector_placeholder = copied.placeholder(format);
        EXPECT_EQ(placeholder.size(), vector_placeholder.size());
        ASSERT_GT(placeholder.size(), 8u);
        EXPECT_TRUE(std::equal(placeholder.begin(), placeholder.begin() + 8,
                               vector_placeholder.begin()));
        buffered.set_data_hash_exclusions({{20, placeholder.size()}});
        auto source = asset();
        buffered.update_hash_from_stream(format, source);
        auto final = buffered.sign_embeddable_to_buffer(format);
        EXPECT_EQ(final.size(), placeholder.size());
    }
    c2pa::Builder no_signer(std::make_shared<c2pa::Context>(), manifest_definition());
    EXPECT_THROW(no_signer.placeholder_to_buffer("image/jpeg"), c2pa::C2paException);
    auto source = asset();
    std::stringstream dest;
    EXPECT_THROW(no_signer.sign_to_buffer("image/jpeg", source, dest), c2pa::C2paException);
}

TEST(SignerInto, AdapterForwardsExactPointersAndLengths)
{
    std::array<unsigned char, 5> payload{1, 2, 3, 4, 5};
    std::array<unsigned char, 9> signature{};
    c2pa::SignerIntoFunc callback = [&](c2pa::ConstBytes data, c2pa::MutableBytes dest) {
        EXPECT_EQ(data.data, payload.data() + 1);
        EXPECT_EQ(data.size, 3u);
        EXPECT_EQ(dest.data, signature.data() + 2);
        EXPECT_EQ(dest.size, 6u);
        dest.data[0] = 42;
        return 1;
    };
    EXPECT_EQ(c2pa::detail::signer_into_passthrough(
        &callback, payload.data() + 1, 3, signature.data() + 2, 6), 1);
    EXPECT_EQ(signature[2], 42);
}

TEST(SignerInto, AdapterRejectsOversizeExceptionsAndInvalidArguments)
{
    unsigned char byte = 0;
    c2pa::SignerIntoFunc callback = [](c2pa::ConstBytes, c2pa::MutableBytes dest) {
        return static_cast<intptr_t>(dest.size + 1);
    };
    EXPECT_EQ(c2pa::detail::signer_into_passthrough(&callback, &byte, 1, &byte, 1), -1);
    EXPECT_EQ(errno, ENOBUFS);
    callback = [](c2pa::ConstBytes, c2pa::MutableBytes) -> intptr_t { throw 42; };
    EXPECT_EQ(c2pa::detail::signer_into_passthrough(&callback, &byte, 1, &byte, 1), -1);
    EXPECT_EQ(errno, EIO);
    callback = [](c2pa::ConstBytes, c2pa::MutableBytes) { return -2; };
    EXPECT_EQ(c2pa::detail::signer_into_passthrough(&callback, &byte, 1, &byte, 1), -1);
    EXPECT_EQ(errno, EIO);
    EXPECT_EQ(c2pa::detail::signer_into_passthrough(nullptr, &byte, 1, &byte, 1), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(SignerInto, MovesContextTransferAndSharedBuilderLifetime)
{
    auto calls = std::make_shared<int>(0);
    std::weak_ptr<int> lifetime = calls;
    auto signer = callback_signer(calls);
    c2pa::Signer moved(std::move(signer));
    auto assigned = direct_signer();
    assigned = std::move(moved);
    auto& self = assigned;
    assigned = std::move(self);
    ASSERT_EQ(signer.c2pa_signer(), nullptr);
    ASSERT_EQ(moved.c2pa_signer(), nullptr);
    c2pa::Context::ContextBuilder config;
    config.with_signer(std::move(assigned));
    auto moved_config = std::move(config);
    c2pa::Context::ContextBuilder final_config;
    final_config = std::move(moved_config);
    auto context = final_config.create_context();
    auto moved_context = std::move(context);
    c2pa::Context final_context;
    final_context = std::move(moved_context);
    auto shared = std::make_shared<c2pa::Context>(std::move(final_context));
    {
        c2pa::Builder builder(shared, manifest_definition());
        shared.reset();
        auto source = asset();
        std::stringstream dest;
        EXPECT_FALSE(builder.sign_to_buffer("image/jpeg", source, dest).empty());
        expect_valid(dest);
        EXPECT_GT(*calls, 0);
        calls.reset();
        EXPECT_FALSE(lifetime.expired());
    }
    EXPECT_TRUE(lifetime.expired());
}

TEST(SignerInto, ReleaseCarriesStableCallbackOwnerAndReplacementCleansUp)
{
    auto calls = std::make_shared<int>(0);
    std::weak_ptr<int> lifetime = calls;
    {
        c2pa::Context::ContextBuilder builder;
        builder.with_signer(callback_signer(calls));
        calls.reset();
        auto released = builder.release();
        ASSERT_NE(released.signer_callback_owner, nullptr);
        auto* address = released.signer_callback_owner.get();
        auto moved = std::move(released);
        EXPECT_EQ(moved.signer_callback_owner.get(), address);
        auto* native = c2pa_context_builder_build(moved.builder);
        ASSERT_NE(native, nullptr);
        c2pa::Context context(native, std::move(moved.callback_owner),
                              std::move(moved.signer_callback_owner));
        auto source = asset();
        std::stringstream dest;
        c2pa::Builder manifest(context, manifest_definition());
        EXPECT_FALSE(manifest.sign_to_buffer("image/jpeg", source, dest).empty());
        expect_valid(dest);
    }
    EXPECT_TRUE(lifetime.expired());

    calls = std::make_shared<int>(0);
    lifetime = calls;
    c2pa::Context::ContextBuilder builder;
    builder.with_signer(callback_signer(calls));
    calls.reset();
    builder.with_signer(direct_signer());
    EXPECT_TRUE(lifetime.expired());
}

TEST(SignerInto, FailuresReleaseCapturesAndPropagate)
{
    EXPECT_THROW(c2pa::Signer(c2pa::SignerIntoFunc{}, C2paSigningAlg::Ed25519,
                              signing_certs(), ""), c2pa::C2paException);
    for (int failure = 0; failure < 4; ++failure) {
        auto marker = std::make_shared<int>(0);
        std::weak_ptr<int> lifetime = marker;
        {
            c2pa::Signer signer([marker, failure](c2pa::ConstBytes, c2pa::MutableBytes dest) -> intptr_t {
                ++*marker;
                if (failure == 0) { return static_cast<intptr_t>(dest.size + 1); }
                if (failure == 1) { throw std::runtime_error("signing failed"); }
                if (failure == 2) { return -1; }
                dest.data[0] = 0;
                return 1;
            }, C2paSigningAlg::Ed25519, signing_certs(), "");
            auto context = std::make_shared<c2pa::Context>();
            c2pa::Builder builder(context, manifest_definition());
            auto source = asset();
            std::stringstream dest;
            EXPECT_THROW(builder.sign_to_buffer("image/jpeg", source, dest, signer), c2pa::C2paException);
            EXPECT_GT(*marker, 0);
            marker.reset();
        }
        EXPECT_TRUE(lifetime.expired());
    }
}

TEST(SignerInto, StandaloneMoveReplacementAndConfigurationFailureReleaseCaptures)
{
    auto old_calls = std::make_shared<int>(0);
    auto new_calls = std::make_shared<int>(0);
    std::weak_ptr<int> old_lifetime = old_calls;
    std::weak_ptr<int> new_lifetime = new_calls;
    {
        auto signer = callback_signer(old_calls);
        auto replacement = callback_signer(new_calls);
        old_calls.reset();
        signer = std::move(replacement);
        EXPECT_TRUE(old_lifetime.expired());
        c2pa::Builder builder(std::make_shared<c2pa::Context>(), manifest_definition());
        auto source = asset();
        std::stringstream dest;
        EXPECT_FALSE(builder.sign_to_buffer("image/jpeg", source, dest, signer).empty());
        expect_valid(dest);
        EXPECT_GT(*new_calls, 0);
        new_calls.reset();
    }
    EXPECT_TRUE(new_lifetime.expired());

    auto calls = std::make_shared<int>(0);
    std::weak_ptr<int> lifetime = calls;
    auto signer = callback_signer(calls);
    calls.reset();
    EXPECT_THROW(c2pa::Context::ContextBuilder()
        .with_signer(std::move(signer)).with_json("not-json"), c2pa::C2paException);
    EXPECT_TRUE(lifetime.expired());
}

TEST(SignerInto, ThrowingCallableCopyDoesNotLeakDuringConstruction)
{
    struct Captured {
        int* destructions;
        ~Captured() { ++*destructions; }
    };
    struct Callable {
        std::shared_ptr<Captured> capture;
        std::shared_ptr<bool> reject_copy;
        Callable(std::shared_ptr<Captured> state, std::shared_ptr<bool> reject)
            : capture(std::move(state)), reject_copy(std::move(reject)) {}
        Callable(const Callable& other)
            : capture(other.capture), reject_copy(other.reject_copy) {
            if (*reject_copy) {
                throw std::runtime_error("callable copy failed");
            }
        }
        intptr_t operator()(c2pa::ConstBytes, c2pa::MutableBytes) const { return -1; }
    };
    int destructions = 0;
    auto capture = std::make_shared<Captured>();
    capture->destructions = &destructions;
    auto reject = std::make_shared<bool>(false);
    c2pa::SignerIntoFunc callback = Callable(capture, reject);
    capture.reset();
    *reject = true;
    EXPECT_THROW(c2pa::Signer(callback, C2paSigningAlg::Ed25519, signing_certs(), ""),
                 std::runtime_error);
    EXPECT_EQ(destructions, 0);
    callback = nullptr;
    EXPECT_EQ(destructions, 1);
}

class RecordingStreambuf : public std::streambuf {
public:
    char* read_pointer = nullptr;
    const char* write_pointer = nullptr;
    std::streamsize read_size = 0;
    std::streamsize write_size = 0;
protected:
    std::streamsize xsgetn(char* data, std::streamsize size) override {
        read_pointer = data;
        read_size = size;
        std::memset(data, 7, static_cast<size_t>(size));
        return size;
    }
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        write_pointer = data;
        write_size = size;
        return size;
    }
};

TEST(ZeroCopyStreams, SharedAdaptersPreserveCallbackPointers)
{
    RecordingStreambuf buffer;
    std::array<unsigned char, 7> bytes{};
    std::istream input(&buffer);
    std::ostream output(&buffer);
    std::iostream io(&buffer);
    auto read = [&](auto& stream) {
        using Stream = std::decay_t<decltype(stream)>;
        EXPECT_EQ(c2pa::detail::stream_reader<Stream>(
            reinterpret_cast<StreamContext*>(&stream), bytes.data() + 1, 5), 5);
        EXPECT_EQ(buffer.read_pointer, reinterpret_cast<char*>(bytes.data() + 1));
        EXPECT_EQ(buffer.read_size, 5);
        EXPECT_EQ(bytes[1], 7);
    };
    auto write = [&](auto& stream) {
        using Stream = std::decay_t<decltype(stream)>;
        EXPECT_EQ(c2pa::detail::stream_writer<Stream>(
            reinterpret_cast<StreamContext*>(&stream), bytes.data() + 2, 3), 3);
        EXPECT_EQ(buffer.write_pointer, reinterpret_cast<const char*>(bytes.data() + 2));
        EXPECT_EQ(buffer.write_size, 3);
    };
    read(input);
    write(output);
    read(io);
    write(io);
}

} // namespace
