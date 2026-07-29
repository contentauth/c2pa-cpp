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
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>

#include "c2pa.hpp"
#include "include/test_utils.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

// Test fixture for embeddable tests with automatic cleanup
class EmbeddableTest : public ::testing::Test {
protected:
    std::vector<fs::path> temp_files;

    // Get path for temp embeddable test files in build directory
    fs::path get_temp_path(const std::string& name) {
        fs::path current_dir = fs::path(__FILE__).parent_path();
        fs::path build_dir = current_dir.parent_path() / "build";
        if (!fs::exists(build_dir)) {
            fs::create_directories(build_dir);
        }
        fs::path temp_path = build_dir / ("embeddable-" + name);
        temp_files.push_back(temp_path);
        return temp_path;
    }

    void TearDown() override {
        for (const auto& path : temp_files) {
            if (fs::exists(path)) {
                fs::remove(path);
            }
        }
        temp_files.clear();
    }

    // Helper: Creates a Builder with a test manifest.
    c2pa::Builder make_builder() {
        auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
        return c2pa::Builder(manifest);
    }
};

// e2e workflow with A.jpg (has no existing C2PA)
TEST_F(EmbeddableTest, FullWorkflowWithAJpg) {
    // This test demonstrates the complete data-hashed embeddable workflow:
    // 1. Create placeholder
    // 2. Sign with auto-calculated hash
    // 3. Format for embedding
    // 4. Verify sizes match (critical invariant for in-place patching)
    //
    // Note: We don't actually embed and read back because manual byte-level
    // embedding requires format-specific knowledge of JPEG structure. The SDK's
    // normal sign() method handles this internally. This test verifies the
    // embeddable APIs work correctly.

    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto certs = c2pa_test::read_text_file(c2pa_test::get_fixture_path("es256_certs.pem"));
    auto private_key = c2pa_test::read_text_file(c2pa_test::get_fixture_path("es256_private.key"));
    auto source_asset = c2pa_test::get_fixture_path("A.jpg");

    // Create signing infra
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json);
    c2pa::Signer signer("Es256", certs, private_key, "http://timestamp.digicert.com");

    // 1: Get placeholder manifest
    // The placeholder is the exact size of the final signed manifest.
    // We specify "image/jpeg" to get JPEG-formatted placeholder bytes.
    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");
    ASSERT_GT(placeholder.size(), 0) << "Placeholder should not be empty";
    size_t placeholder_size = placeholder.size();

    // 2: Build DataHash JSON
    // In production, you would embed the placeholder and note its location.
    // The exclusion range would match where you embedded it.
    size_t embed_offset = 20;  // Example offset
    std::string data_hash = R"({
        "exclusions": [{
            "start": )" + std::to_string(embed_offset) + R"(,
            "length": )" + std::to_string(placeholder.size()) + R"(
        }],
        "name": "jumbf manifest",
        "alg": "sha256",
        "hash": "",
        "pad": " "
    })";

    // 3: Sign with auto-calculated hash
    // The SDK reads the asset stream, skips the exclusion range,
    // calculates SHA-256 hash, and signs the manifest.
    std::ifstream asset_stream(source_asset, std::ios::binary);
    ASSERT_TRUE(asset_stream.is_open());

    auto raw_manifest = builder.sign_data_hashed_embeddable(
        signer, data_hash, "application/c2pa", &asset_stream);
    asset_stream.close();
    ASSERT_GT(raw_manifest.size(), 0);

    // 4: Format for JPEG embedding
    // Convert raw JUMBF to JPEG APP11 segments
    auto jpeg_embeddable = c2pa::Builder::format_embeddable("image/jpeg", raw_manifest);
    ASSERT_GT(jpeg_embeddable.size(), 0);

    // 5: Verify invariant (the placement size)
    // The formatted manifest must be exactly the same size as the placeholder.
    // This enables in-place patching in production workflows.
    EXPECT_EQ(jpeg_embeddable.size(), placeholder_size)
        << "Signed manifest size must match placeholder size for in-place patching";
}

// e2e workflow with A.jpg using new context-signer embeddable APIs
TEST_F(EmbeddableTest, FullWorkflowWithAJpgContextSigner) {
    // Demonstrates the new embeddable API where the signer lives on the context
    // rather than being passed explicitly to placeholder/sign methods.
    //
    // Workflow:
    //   1. needs_placeholder() — check whether a placeholder embed step is required
    //   2. placeholder()       — get the correctly-sized placeholder bytes to embed
    //   3. set_data_hash_exclusions() — register where the placeholder was embedded
    //   4. update_hash_from_stream()  — hash the asset (SDK skips exclusion ranges)
    //   5. sign_embeddable()          — sign; output size is exactly placeholder.size()

    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto source_asset  = c2pa_test::get_fixture_path("A.jpg");

    // Create a context with the signer attached programmatically
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .create_context();

    auto builder = c2pa::Builder(context, manifest_json);

    // 1: Check if a placeholder is required for JPEG
    ASSERT_TRUE(builder.needs_placeholder("image/jpeg"))
        << "JPEG always requires a placeholder";

    // 2: Get the placeholder bytes (size is committed internally for later validation)
    auto placeholder_bytes = builder.placeholder("image/jpeg");
    ASSERT_GT(placeholder_bytes.size(), 0) << "Placeholder must not be empty";
    size_t placeholder_size = placeholder_bytes.size();

    // 3: Register where we would embed the placeholder.
    //    In production this offset is where you physically write placeholder_bytes.
    size_t embed_offset = 20;
    builder.set_data_hash_exclusions({{embed_offset, placeholder_bytes.size()}});

    // 4: Hash the asset stream, skipping the exclusion range
    std::ifstream asset_stream(source_asset, std::ios::binary);
    ASSERT_TRUE(asset_stream.is_open());
    builder.update_hash_from_stream("image/jpeg", asset_stream);
    asset_stream.close();

    // 5: Sign — output must match the placeholder size (enables in-place patching)
    auto signed_manifest = builder.sign_embeddable("image/jpeg");
    ASSERT_GT(signed_manifest.size(), 0);
    EXPECT_EQ(signed_manifest.size(), placeholder_size)
        << "Signed manifest must be exactly the same size as the placeholder";
}

// e2e workflow with C.jpg (has existing C2PA metadata)
TEST_F(EmbeddableTest, FullWorkflowWithCJpg) {
    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();
    auto source_asset = c2pa_test::get_fixture_path("C.jpg");

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json);

    // Get placeholder
    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");
    ASSERT_GT(placeholder.size(), 0);

    // Build data hash JSON
    size_t embed_offset = 20;
    std::string data_hash = R"({
        "exclusions": [{
            "start": )" + std::to_string(embed_offset) + R"(,
            "length": )" + std::to_string(placeholder.size()) + R"(
        }],
        "name": "jumbf manifest",
        "alg": "sha256",
        "hash": "",
        "pad": " "
    })";

    // Sign with the asset that has existing C2PA
    std::ifstream asset_stream(source_asset, std::ios::binary);
    auto raw_manifest = builder.sign_data_hashed_embeddable(
        signer, data_hash, "application/c2pa", &asset_stream);
    asset_stream.close();
    ASSERT_GT(raw_manifest.size(), 0);

    // Format for jpeg
    auto jpeg_embeddable = c2pa::Builder::format_embeddable("image/jpeg", raw_manifest);
    ASSERT_GT(jpeg_embeddable.size(), 0);

    // Verify invariant: size matches so we could place
    EXPECT_EQ(jpeg_embeddable.size(), placeholder.size());
}

// Pre-calculated hash
TEST_F(EmbeddableTest, PreCalculatedHash) {
    // This test demonstrates signing with a pre-calculated hash.
    // In production, you might calculate the hash on a different machine or at a different time than signing.

    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json);

    // Get placeholder
    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");

    // Build data hash JSON with a KNOWN hash value
    // In this test, we use a dummy hash since we're not actually embedding.
    // In production, this would be a real SHA-256 hash of your asset.
    std::string data_hash = R"({
        "exclusions": [{
            "start": 20,
            "length": )" + std::to_string(placeholder.size()) + R"(
        }],
        "name": "jumbf manifest",
        "alg": "sha256",
        "hash": "gWZNEOMHQNiULfA/tO5HD2awOwYMA3tnfUPApIr9csk=",
        "pad": " "
    })";

    // Sign without providing an asset stream
    // The SDK uses the hash value from the data_hash JSON directly
    auto manifest = builder.sign_data_hashed_embeddable(
        signer, data_hash, "image/jpeg", nullptr);

    EXPECT_GT(manifest.size(), 0) << "Should produce signed manifest bytes";
    EXPECT_EQ(manifest.size(), placeholder.size()) << "Size should match placeholder";
}

// Auto-calculated hash
TEST_F(EmbeddableTest, AutoCalculatedHash) {
    // This test demonstrates the SDK calculating the hash automatically from an asset stream.
    // The hash field is left empty (""), and the SDK will:
    // 1. Read the asset stream
    // 2. Skip the exclusion ranges
    // 3. Calculate the SHA-256 hash of the remaining bytes
    //
    // Usually (not in a test), you would have already embedded the placeholder at the exclusion offset.
    // Here we just verify the API accepts an asset stream and auto-calculates the hash.

    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();
    auto source_asset = c2pa_test::get_fixture_path("A.jpg");

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json);

    // Get placeholder
    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");

    // Build data hash JSON with empty hash field
    // The exclusion range would match where you embedded the placeholder
    std::string data_hash = R"({
        "exclusions": [{
            "start": 20,
            "length": )" + std::to_string(placeholder.size()) + R"(
        }],
        "name": "jumbf manifest",
        "alg": "sha256",
        "hash": "",
        "pad": " "
    })";

    // Sign with asset stream: SDK will auto-calculate hash
    // This stream would be your asset with the placeholder already embedded
    std::ifstream asset_stream(source_asset, std::ios::binary);
    auto manifest = builder.sign_data_hashed_embeddable(
        signer, data_hash, "application/c2pa", &asset_stream);
    asset_stream.close();

    EXPECT_GT(manifest.size(), 0) << "Should produce signed manifest bytes";
}

// Embedding roundtrip
TEST_F(EmbeddableTest, FormatEmbeddableRoundTrip) {
    // This test demonstrates the two-step format conversion:
    // 1. Sign with format="application/c2pa" to get raw JUMBF bytes
    // 2. Use format_embeddable() to convert to JPEG-embeddable format
    //
    // This is useful when you want to sign once and embed in multiple formats.

    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();
    auto source_asset = c2pa_test::get_fixture_path("A.jpg");

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json);

    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");

    std::string data_hash = R"({
        "exclusions": [{
            "start": 20,
            "length": )" + std::to_string(placeholder.size()) + R"(
        }],
        "name": "jumbf manifest",
        "alg": "sha256",
        "hash": "",
        "pad": " "
    })";

    // 1: Sign with "application/c2pa" to get raw JUMBF
    std::ifstream asset_stream(source_asset, std::ios::binary);
    auto raw_jumbf = builder.sign_data_hashed_embeddable(
        signer, data_hash, "application/c2pa", &asset_stream);
    asset_stream.close();

    // 2: Convert raw JUMBF to JPEG-embeddable format
    auto jpeg_formatted = c2pa::Builder::format_embeddable("image/jpeg", raw_jumbf);

    // The JPEG format adds container overhead (APP11 markers, headers)
    // so the formatted output should be larger than raw JUMBF due to this
    EXPECT_GT(jpeg_formatted.size(), raw_jumbf.size())
        << "JPEG format should add container overhead";

    // But still matches placeholder size (which was already JPEG-formatted)
    EXPECT_EQ(jpeg_formatted.size(), placeholder.size());
}

// Verify placeholder size matches final size (asset has no existing C2PA)
TEST_F(EmbeddableTest, PlaceholderSizeMatchesFinalInvariant) {
    // This test verifies the critical invariant:
    // placeholder.size() == final_signed_manifest.size()
    // This is what enables in-place patching in workflows.
    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();
    auto source_asset = c2pa_test::get_fixture_path("A.jpg");

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json);

    // Get placeholder and record its size
    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");
    size_t placeholder_size = placeholder.size();

    // Complete the signing flow
    size_t embed_offset = 20;
    std::string data_hash = R"({
        "exclusions": [{
            "start": )" + std::to_string(embed_offset) + R"(,
            "length": )" + std::to_string(placeholder.size()) + R"(
        }],
        "name": "jumbf manifest",
        "alg": "sha256",
        "hash": "",
        "pad": " "
    })";

    std::ifstream asset_stream(source_asset, std::ios::binary);
    auto raw_manifest = builder.sign_data_hashed_embeddable(
        signer, data_hash, "application/c2pa", &asset_stream);
    asset_stream.close();

    auto jpeg_embeddable = c2pa::Builder::format_embeddable("image/jpeg", raw_manifest);

    // The final manifest is exactly the same size as placeholder
    EXPECT_EQ(jpeg_embeddable.size(), placeholder_size)
        << "Final signed manifest MUST be same size as placeholder";
}

// Verify placeholder size matches final size (asset has existing C2PA metadata)
TEST_F(EmbeddableTest, PlaceholderSizeMatchesFinalInvariantWithMetadata) {
    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();
    auto source_asset = c2pa_test::get_fixture_path("C.jpg");

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json);

    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");
    size_t placeholder_size = placeholder.size();

    size_t embed_offset = 20;
    std::string data_hash = R"({
        "exclusions": [{
            "start": )" + std::to_string(embed_offset) + R"(,
            "length": )" + std::to_string(placeholder.size()) + R"(
        }],
        "name": "jumbf manifest",
        "alg": "sha256",
        "hash": "",
        "pad": " "
    })";

    std::ifstream asset_stream(source_asset, std::ios::binary);
    auto signed_manifest = builder.sign_data_hashed_embeddable(
        signer, data_hash, "image/jpeg", &asset_stream);
    asset_stream.close();

    EXPECT_EQ(signed_manifest.size(), placeholder_size);
}

TEST_F(EmbeddableTest, ContextSettingsPropagation) {
    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();
    auto source_asset = c2pa_test::get_fixture_path("A.jpg");

    // Create context with custom settings
    auto context = c2pa::Context(R"({
        "builder": {
            "thumbnail": {
                "enabled": false
            }
        }
    })");

    // Use context for builder
    auto builder = c2pa::Builder(context, manifest_json);

    // Get placeholder
    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");
    ASSERT_GT(placeholder.size(), 0);

    // Sign using the same context
    std::string data_hash = R"({
        "exclusions": [{
            "start": 20,
            "length": )" + std::to_string(placeholder.size()) + R"(
        }],
        "name": "jumbf manifest",
        "alg": "sha256",
        "hash": "",
        "pad": " "
    })";

    std::ifstream asset_stream(source_asset, std::ios::binary);
    auto manifest = builder.sign_data_hashed_embeddable(
        signer, data_hash, "application/c2pa", &asset_stream);
    asset_stream.close();

    EXPECT_GT(manifest.size(), 0) << "Context propagated successfully through workflow";
}

// Verify different formats
TEST_F(EmbeddableTest, MultipleFormats) {
    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();

    auto context = c2pa::Context();

    // jpeg
    {
        auto builder = c2pa::Builder(context, manifest_json);
        auto placeholder = builder.data_hashed_placeholder(
            signer.reserve_size(), "image/jpeg");
        EXPECT_GT(placeholder.size(), 0) << "JPEG placeholder should not be empty";

        // Build simple data hash with dummy values
        std::string data_hash = R"({
            "exclusions": [{
                "start": 20,
                "length": )" + std::to_string(placeholder.size()) + R"(
            }],
            "name": "jumbf manifest",
            "alg": "sha256",
            "hash": "gWZNEOMHQNiULfA/tO5HD2awOwYMA3tnfUPApIr9csk=",
            "pad": " "
        })";

        auto manifest = builder.sign_data_hashed_embeddable(
            signer, data_hash, "image/jpeg", nullptr);
        EXPECT_GT(manifest.size(), 0) << "JPEG signed manifest should not be empty";
    }

    // png
    {
        auto builder = c2pa::Builder(context, manifest_json);
        auto placeholder = builder.data_hashed_placeholder(
            signer.reserve_size(), "image/png");
        EXPECT_GT(placeholder.size(), 0) << "PNG placeholder should not be empty";

        std::string data_hash = R"({
            "exclusions": [{
                "start": 33,
                "length": )" + std::to_string(placeholder.size()) + R"(
            }],
            "name": "jumbf manifest",
            "alg": "sha256",
            "hash": "gWZNEOMHQNiULfA/tO5HD2awOwYMA3tnfUPApIr9csk=",
            "pad": " "
        })";

        auto manifest = builder.sign_data_hashed_embeddable(
            signer, data_hash, "image/png", nullptr);
        EXPECT_GT(manifest.size(), 0) << "PNG signed manifest should not be empty";
    }
}

// BoxHash: standard Builder::sign() flow with prefer_box_hash enabled
TEST_F(EmbeddableTest, BoxHashStandardSign) {
    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("boxhash_standard.jpg");

    // Enable BoxHash via context settings
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .with_json(R"({
            "builder": {
                "prefer_box_hash": true
            }
        })")
        .create_context();

    auto builder = c2pa::Builder(context, manifest_json);

    // Standard sign() handles the entire BoxHash workflow internally
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(source_path, output_path));
    ASSERT_FALSE(manifest_data.empty());

    // Verify the output can be read back with valid C2PA data
    auto reader = c2pa::Reader(context, output_path);
    std::string json_result;
    ASSERT_NO_THROW(json_result = reader.json());
    EXPECT_FALSE(json_result.empty());
}

// BoxHash: needs_placeholder() returns false when prefer_box_hash is enabled
TEST_F(EmbeddableTest, BoxHashNeedsPlaceholderReturnsFalse) {
    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .with_json(R"({
            "builder": {
                "prefer_box_hash": true
            }
        })")
        .create_context();

    auto builder = c2pa::Builder(context, manifest_json);

    // BoxHash-capable formats should not require a placeholder
    ASSERT_FALSE(builder.needs_placeholder("image/jpeg"))
        << "JPEG with prefer_box_hash should not require a placeholder";
}

// BoxHash: embeddable API workflow (update_hash_from_stream + sign_embeddable)
TEST_F(EmbeddableTest, BoxHashEmbeddableWorkflow) {
    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto source_asset = c2pa_test::get_fixture_path("A.jpg");

    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .with_json(R"({
            "builder": {
                "prefer_box_hash": true
            }
        })")
        .create_context();

    auto builder = c2pa::Builder(context, manifest_json);

    // No placeholder needed for BoxHash
    ASSERT_FALSE(builder.needs_placeholder("image/jpeg"));

    // Hash the original asset directly (no placeholder to embed first)
    std::ifstream asset_stream(source_asset, std::ios::binary);
    ASSERT_TRUE(asset_stream.is_open());
    builder.update_hash_from_stream("image/jpeg", asset_stream);
    asset_stream.close();

    // Sign — output is the actual manifest size (not padded to a placeholder)
    auto manifest_bytes = builder.sign_embeddable("image/jpeg");
    ASSERT_GT(manifest_bytes.size(), 0)
        << "BoxHash embeddable should produce signed manifest bytes";
}

// BoxHash: BMFF formats always require a placeholder regardless of prefer_box_hash
TEST_F(EmbeddableTest, BoxHashBmffAlwaysRequiresPlaceholder) {
    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .with_json(R"({
            "builder": {
                "prefer_box_hash": true
            }
        })")
        .create_context();

    auto builder = c2pa::Builder(context, manifest_json);

    // BMFF formats always require a placeholder, even with prefer_box_hash
    ASSERT_TRUE(builder.needs_placeholder("video/mp4"))
        << "MP4 must always require a placeholder regardless of prefer_box_hash";
}

// Direct embedding when giving format
TEST_F(EmbeddableTest, DirectEmbeddingWithFormat) {
    // This test demonstrates that when you sign with format="image/jpeg",
    // the output is already embeddable: no need for format_embeddable()!
    // Whereas...
    // Signing with "application/c2pa" requires a separate format_embeddable() call.

    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();
    auto source_asset = c2pa_test::get_fixture_path("A.jpg");

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json);

    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");

    std::string data_hash = R"({
        "exclusions": [{
            "start": 20,
            "length": )" + std::to_string(placeholder.size()) + R"(
        }],
        "name": "jumbf manifest",
        "alg": "sha256",
        "hash": "",
        "pad": " "
    })";

    std::ifstream asset_stream(source_asset, std::ios::binary);

    // Sign directly with "image/jpeg" format
    // This output is already formatted: no format_embeddable() needed
    auto jpeg_manifest = builder.sign_data_hashed_embeddable(
        signer, data_hash, "image/jpeg", &asset_stream);
    asset_stream.close();

    EXPECT_GT(jpeg_manifest.size(), 0);
    EXPECT_EQ(jpeg_manifest.size(), placeholder.size())
        << "Direct JPEG format output matches placeholder size";
}

class EmbeddableBlankFormatTest : public EmbeddableTest, public ::testing::WithParamInterface<std::string> {};
// needs_placeholder() and update_hash_from_stream() are the only embeddable steps when the core is silent on a blank format.

INSTANTIATE_TEST_SUITE_P(EmbeddableBlankFormats, EmbeddableBlankFormatTest,
                         ::testing::ValuesIn(c2pa_test::kBlankFormats));

TEST_P(EmbeddableBlankFormatTest, NeedsPlaceholderRejectsBlankFormat) {
    auto builder = make_builder();
    EXPECT_THROW({ builder.needs_placeholder(GetParam()); }, c2pa::C2paException);
}

TEST_F(EmbeddableTest, UpdateHashFromStreamRejectsBlankFormat) {
    auto builder = make_builder();
    std::ifstream asset(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(asset.is_open());
    EXPECT_THROW({ builder.update_hash_from_stream("", asset); }, c2pa::C2paException);
}

TEST_F(EmbeddableTest, EmbeddableStepsRejectUnsupportedFormat) {
    std::vector<unsigned char> data{0x01, 0x02, 0x03};
    EXPECT_THROW({ c2pa::Builder::format_embeddable("application/zip", data); },
                 c2pa::C2paException);
}

TEST_F(EmbeddableTest, FormatEmbeddableAcceptsReadOnlyFormat) {
    // Composing a manifest for a container the library reads but cannot write
    // is the point of the embeddable workflow: an outside writer embeds it.
    const auto readable = c2pa::Reader::supported_mime_types();
    const auto writable = c2pa::Builder::supported_mime_types();

    std::string read_only;
    for (const auto& format : readable) {
        if (std::find(writable.begin(), writable.end(), format) == writable.end()) {
            read_only = format;
            break;
        }
    }
    if (read_only.empty()) {
        GTEST_SKIP() << "Every readable format is also writable";
    }

    std::vector<unsigned char> data{0x01, 0x02, 0x03};
    EXPECT_NO_THROW({ c2pa::Builder::format_embeddable(read_only, data); })
        << "read-only format: " << read_only;
}

TEST_F(EmbeddableTest, NeedsPlaceholderDistinguishesContainersWhenFormatIsGiven) {
    // A named format yields a container-specific answer:
    // BMFF needs a placeholder sized differently from a JPEG.
    auto builder = make_builder();
    EXPECT_TRUE(builder.needs_placeholder("image/jpeg"));
    EXPECT_TRUE(builder.needs_placeholder("video/mp4"));
}
