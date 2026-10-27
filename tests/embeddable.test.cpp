// Copyright 2026 Adobe. All rights reserved.
// This file is licensed to you under the Apache License,
// Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
// or the MIT license (http://opensource.org/licenses/MIT),
// at your option.
// Unless required by applicable law or agreed to in writing,
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

class EmbeddableTest : public ::testing::Test {
protected:
    std::vector<fs::path> temp_files;

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

    c2pa::Builder make_builder() {
        auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
        auto context = c2pa::Context::ContextBuilder()
            .with_signer(c2pa_test::create_test_signer())
            .create_context();
        return c2pa::Builder(context, manifest);
    }

};

TEST_F(EmbeddableTest, FullWorkflowWithAJpg) {
    auto builder = make_builder();
    auto placeholder = builder.placeholder("image/jpeg");
    ASSERT_FALSE(placeholder.empty());
    builder.set_data_hash_exclusions({{20, placeholder.size()}});
    std::ifstream asset(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(asset.is_open());
    builder.update_hash_from_stream("image/jpeg", asset);
    auto manifest = builder.sign_embeddable("image/jpeg");
    EXPECT_EQ(manifest.size(), placeholder.size());
}

TEST_F(EmbeddableTest, FullWorkflowWithAJpgContextSigner) {
    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .create_context();
    auto builder = c2pa::Builder(context, manifest_json);
    ASSERT_TRUE(builder.needs_placeholder("image/jpeg"));
    auto placeholder = builder.placeholder("image/jpeg");
    ASSERT_FALSE(placeholder.empty());
    builder.set_data_hash_exclusions({{20, placeholder.size()}});
    std::ifstream asset(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(asset.is_open());
    builder.update_hash_from_stream("image/jpeg", asset);
    auto manifest = builder.sign_embeddable("image/jpeg");
    EXPECT_EQ(manifest.size(), placeholder.size());
}

TEST_F(EmbeddableTest, FullWorkflowWithCJpg) {
    auto builder = make_builder();
    auto placeholder = builder.placeholder("image/jpeg");
    ASSERT_FALSE(placeholder.empty());
    builder.set_data_hash_exclusions({{20, placeholder.size()}});
    std::ifstream asset(c2pa_test::get_fixture_path("C.jpg"), std::ios::binary);
    ASSERT_TRUE(asset.is_open());
    builder.update_hash_from_stream("image/jpeg", asset);
    auto manifest = builder.sign_embeddable("image/jpeg");
    EXPECT_EQ(manifest.size(), placeholder.size());
}

TEST_F(EmbeddableTest, PreCalculatedHash) {
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .create_context();
    auto builder = c2pa::Builder(context, c2pa_test::prehashed_training_manifest());
    // A precomputed hard binding uses direct mode, with no asset stream.
    auto manifest = builder.sign_embeddable("image/jpeg");
    EXPECT_FALSE(manifest.empty());
}

TEST_F(EmbeddableTest, AutoCalculatedHash) {
    auto builder = make_builder();
    auto placeholder = builder.placeholder("image/jpeg");
    builder.set_data_hash_exclusions({{20, placeholder.size()}});
    std::ifstream asset(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(asset.is_open());
    builder.update_hash_from_stream("image/jpeg", asset);
    auto manifest = builder.sign_embeddable("application/c2pa");
    EXPECT_FALSE(manifest.empty());
}

TEST_F(EmbeddableTest, FormatEmbeddableRoundTrip) {
    auto builder = make_builder();
    auto placeholder = builder.placeholder("image/jpeg");
    builder.set_data_hash_exclusions({{20, placeholder.size()}});
    std::ifstream asset(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(asset.is_open());
    builder.update_hash_from_stream("image/jpeg", asset);
    auto raw_jumbf = builder.sign_embeddable("application/c2pa");
    auto jpeg_formatted = builder.sign_embeddable("image/jpeg");
    EXPECT_GT(jpeg_formatted.size(), raw_jumbf.size());
    EXPECT_EQ(jpeg_formatted.size(), placeholder.size());
}

TEST_F(EmbeddableTest, PlaceholderSizeMatchesFinalInvariant) {
    auto builder = make_builder();
    auto placeholder = builder.placeholder("image/jpeg");
    builder.set_data_hash_exclusions({{20, placeholder.size()}});
    std::ifstream asset(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(asset.is_open());
    builder.update_hash_from_stream("image/jpeg", asset);
    auto manifest = builder.sign_embeddable("image/jpeg");
    EXPECT_EQ(manifest.size(), placeholder.size());
}

TEST_F(EmbeddableTest, PlaceholderSizeMatchesFinalInvariantWithMetadata) {
    auto builder = make_builder();
    auto placeholder = builder.placeholder("image/jpeg");
    builder.set_data_hash_exclusions({{20, placeholder.size()}});
    std::ifstream asset(c2pa_test::get_fixture_path("C.jpg"), std::ios::binary);
    ASSERT_TRUE(asset.is_open());
    builder.update_hash_from_stream("image/jpeg", asset);
    auto manifest = builder.sign_embeddable("image/jpeg");
    EXPECT_EQ(manifest.size(), placeholder.size());
}

TEST_F(EmbeddableTest, ContextSettingsPropagation) {
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .with_json(R"({"builder":{"thumbnail":{"enabled":false}}})")
        .create_context();
    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto builder = c2pa::Builder(context, manifest_json);
    auto placeholder = builder.placeholder("image/jpeg");
    ASSERT_FALSE(placeholder.empty());
    builder.set_data_hash_exclusions({{20, placeholder.size()}});
    std::ifstream asset(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(asset.is_open());
    builder.update_hash_from_stream("image/jpeg", asset);
    EXPECT_FALSE(builder.sign_embeddable("image/jpeg").empty());
}

TEST_F(EmbeddableTest, MultipleFormats) {
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .create_context();
    for (const auto& format : {"image/jpeg", "image/png"}) {
        auto placeholder_builder = make_builder();
        EXPECT_FALSE(placeholder_builder.placeholder(format).empty());
        auto builder = c2pa::Builder(context, c2pa_test::prehashed_training_manifest());
        EXPECT_FALSE(builder.sign_embeddable(format).empty());
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

TEST_F(EmbeddableTest, DirectEmbeddingWithFormat) {
    auto builder = make_builder();
    auto placeholder = builder.placeholder("image/jpeg");
    builder.set_data_hash_exclusions({{20, placeholder.size()}});
    std::ifstream asset(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(asset.is_open());
    builder.update_hash_from_stream("image/jpeg", asset);
    auto manifest = builder.sign_embeddable("image/jpeg");
    ASSERT_GT(manifest.size(), 2u);
    EXPECT_EQ(manifest[0], 0xff);
    EXPECT_EQ(manifest[1], 0xeb);
    EXPECT_EQ(manifest.size(), placeholder.size());
}

TEST_F(EmbeddableTest, PlaceholderRequiresContextSigner) {
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, "{}");
    EXPECT_THROW(builder.placeholder("image/jpeg"), c2pa::C2paException);
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
