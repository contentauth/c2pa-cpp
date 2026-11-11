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
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
        return c2pa::Builder(manifest);
    }

    std::vector<unsigned char> read_bytes(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
            throw std::runtime_error("Could not open " + path.string());
        }
        return std::vector<unsigned char>(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    // Replace the fixtures' existing JUMBF APP11 segments, retaining the other
    // metadata and the complete scan data. A second manifest store is not valid.
    std::vector<unsigned char> jpeg_without_jumbf(const std::vector<unsigned char>& source,
                                                size_t& removed_segments) {
        if (source.size() < 2 || source[0] != 0xff || source[1] != 0xd8) {
            throw std::runtime_error("Expected a JPEG fixture");
        }
        std::vector<unsigned char> result(source.begin(), source.begin() + 2);
        removed_segments = 0;
        size_t offset = 2;
        while (offset + 4 <= source.size()) {
            if (source[offset] != 0xff) {
                throw std::runtime_error("Expected a JPEG segment");
            }
            const auto marker = source[offset + 1];
            if (marker == 0xda || marker == 0xd9) {
                result.insert(result.end(), source.begin() + offset, source.end());
                return result;
            }
            const size_t length = (static_cast<size_t>(source[offset + 2]) << 8)
                                | source[offset + 3];
            if (length < 2 || length > source.size() - offset - 2) {
                throw std::runtime_error("Invalid JPEG segment length");
            }
            const size_t end = offset + 2 + length;
            const bool is_jumbf = marker == 0xeb && length >= 4
                              && source[offset + 4] == 'J' && source[offset + 5] == 'P';
            if (is_jumbf) {
                ++removed_segments;
            } else {
                result.insert(result.end(), source.begin() + offset, source.begin() + end);
            }
            offset = end;
        }
        throw std::runtime_error("JPEG fixture has no scan data");
    }

    void expect_valid_manifest(const json& parsed) {
        ASSERT_TRUE(parsed.contains("active_manifest")) << parsed.dump(2);
        const auto active = parsed["active_manifest"].get<std::string>();
        ASSERT_TRUE(parsed.contains("manifests"));
        ASSERT_TRUE(parsed["manifests"].contains(active));
        const auto state = parsed.value("validation_state", std::string());
        EXPECT_TRUE(state == "Valid" || state == "Trusted") << parsed.dump(2);

        const auto& manifest = parsed["manifests"][active];
        ASSERT_TRUE(manifest.contains("assertions"));
        EXPECT_TRUE(std::any_of(manifest["assertions"].begin(), manifest["assertions"].end(),
            [](const json& assertion) {
                return assertion.value("label", std::string()) == "cawg.training-mining";
            })) << manifest.dump(2);
    }

    void jpeg_round_trip(c2pa::Context& context, const std::string& fixture,
                         bool format_raw = false, bool check_tampering = false) {
        SCOPED_TRACE(fixture);
        auto manifest = json::parse(c2pa_test::read_text_file(
            c2pa_test::get_fixture_path("training.json")));
        manifest["title"] = "Embeddable " + fixture;
        manifest["format"] = "image/jpeg";
        auto builder = c2pa::Builder(context, manifest.dump());
        ASSERT_TRUE(builder.needs_placeholder("image/jpeg"));
        auto placeholder = builder.placeholder("image/jpeg");
        ASSERT_FALSE(placeholder.empty());

        size_t removed_segments = 0;
        auto asset = jpeg_without_jumbf(
            read_bytes(c2pa_test::get_fixture_path(fixture)), removed_segments);
        if (fixture == "C.jpg") {
            EXPECT_GT(removed_segments, 0u) << "C.jpg must exercise manifest replacement";
        } else {
            EXPECT_EQ(removed_segments, 0u);
        }

        // A valid COM segment gives the tampering check a non-structural byte
        // outside the excluded manifest slot, without corrupting JPEG parsing.
        const std::vector<unsigned char> comment = {0xff, 0xfe, 0x00, 0x06, 't', 'e', 's', 't'};
        asset.insert(asset.begin() + 2, comment.begin(), comment.end());
        const size_t embed_offset = 2 + comment.size();
        asset.insert(asset.begin() + embed_offset, placeholder.begin(), placeholder.end());
        const auto asset_with_placeholder = asset;
        ASSERT_LE(embed_offset + placeholder.size(), asset.size());
        builder.set_data_hash_exclusions({{embed_offset, placeholder.size()}});

        std::istringstream asset_stream(
            std::string(asset.begin(), asset.end()), std::ios::in | std::ios::binary);
        builder.update_hash_from_stream("image/jpeg", asset_stream);

        auto signed_manifest = builder.sign_embeddable(
            format_raw ? "application/c2pa" : "image/jpeg");
        ASSERT_FALSE(signed_manifest.empty());
        if (format_raw) {
            auto formatted = c2pa::Builder::format_embeddable("image/jpeg", signed_manifest);
            EXPECT_GT(formatted.size(), signed_manifest.size());
            signed_manifest = std::move(formatted);
        }
        ASSERT_EQ(signed_manifest.size(), placeholder.size())
            << "In-place patching must not shift any asset bytes";
        std::copy(signed_manifest.begin(), signed_manifest.end(), asset.begin() + embed_offset);
        EXPECT_EQ(asset.size(), asset_with_placeholder.size());
        EXPECT_TRUE(std::equal(asset.begin(), asset.begin() + embed_offset,
                               asset_with_placeholder.begin()));
        EXPECT_TRUE(std::equal(asset.begin() + embed_offset + placeholder.size(), asset.end(),
                               asset_with_placeholder.begin() + embed_offset + placeholder.size()));

        std::istringstream signed_stream(
            std::string(asset.begin(), asset.end()), std::ios::in | std::ios::binary);
        auto reader = c2pa::Reader(context, "image/jpeg", signed_stream);
        auto parsed = json::parse(reader.json());
        expect_valid_manifest(parsed);
        ASSERT_TRUE(parsed.contains("active_manifest"));
        auto active = parsed["active_manifest"].get<std::string>();
        EXPECT_EQ(parsed["manifests"][active]["title"], manifest["title"]);

        if (check_tampering) {
            asset[6] ^= 1; // COM payload, outside the manifest exclusion.
            std::istringstream tampered_stream(
                std::string(asset.begin(), asset.end()), std::ios::in | std::ios::binary);
            auto tampered_reader = c2pa::Reader(context, "image/jpeg", tampered_stream);
            auto tampered = json::parse(tampered_reader.json());
            EXPECT_EQ(tampered.value("validation_state", std::string()), "Invalid");
            EXPECT_NE(tampered.dump().find("assertion.dataHash.mismatch"), std::string::npos);
        }
    }
};

TEST_F(EmbeddableTest, FullWorkflowWithAJpgContextSigner) {
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .create_context();
    jpeg_round_trip(context, "A.jpg", false, true);
}

TEST_F(EmbeddableTest, FullWorkflowWithCJpg) {
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .create_context();
    jpeg_round_trip(context, "C.jpg");
}

TEST_F(EmbeddableTest, FormatEmbeddableRoundTrip) {
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .create_context();
    jpeg_round_trip(context, "A.jpg", true);
}

TEST_F(EmbeddableTest, ContextSettingsPropagation) {
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .with_json(R"({
            "builder": {
                "thumbnail": {
                    "enabled": false
                }
            }
        })")
        .create_context();
    jpeg_round_trip(context, "A.jpg");
}

TEST_F(EmbeddableTest, MultipleFormats) {
    auto raw_manifest = read_bytes(c2pa_test::get_fixture_path("cloud_manifest.c2pa"));
    ASSERT_FALSE(raw_manifest.empty());
    const auto original = raw_manifest;
    auto jpeg = c2pa::Builder::format_embeddable("image/jpeg", raw_manifest);
    auto png = c2pa::Builder::format_embeddable("image/png", raw_manifest);
    ASSERT_GT(jpeg.size(), raw_manifest.size());
    EXPECT_EQ(jpeg[0], 0xff);
    EXPECT_EQ(jpeg[1], 0xeb);
    ASSERT_EQ(png.size(), raw_manifest.size() + 12);
    EXPECT_EQ(std::string(png.begin() + 4, png.begin() + 8), "caBX");
    EXPECT_TRUE(std::equal(raw_manifest.begin(), raw_manifest.end(), png.begin() + 8));
    EXPECT_EQ(raw_manifest, original) << "Formatting must not modify the raw manifest";
}

TEST_F(EmbeddableTest, PngPlaceholderRoundTrip) {
    // A complete 1x1 grayscale/alpha PNG keeps this test independent of extra fixtures.
    std::vector<unsigned char> asset = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xb5, 0x1c, 0x0c, 0x02,
        0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54,
        0x78, 0xda, 0x63, 0x64, 0xf8, 0x0f, 0x00, 0x01,
        0x05, 0x01, 0x01, 0x27, 0x18, 0xe3, 0x66,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
        0xae, 0x42, 0x60, 0x82
    };
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .create_context();
    auto manifest = json::parse(c2pa_test::read_text_file(
        c2pa_test::get_fixture_path("training.json")));
    manifest["format"] = "image/png";
    auto builder = c2pa::Builder(context, manifest.dump());
    ASSERT_TRUE(builder.needs_placeholder("image/png"));
    auto placeholder = builder.placeholder("image/png");
    ASSERT_FALSE(placeholder.empty());

    // PNG ancillary chunks may be inserted immediately after IHDR.
    const size_t embed_offset = 33;
    asset.insert(asset.begin() + embed_offset, placeholder.begin(), placeholder.end());
    builder.set_data_hash_exclusions({{embed_offset, placeholder.size()}});
    std::istringstream asset_stream(
        std::string(asset.begin(), asset.end()), std::ios::in | std::ios::binary);
    builder.update_hash_from_stream("image/png", asset_stream);
    auto signed_manifest = builder.sign_embeddable("image/png");
    ASSERT_EQ(signed_manifest.size(), placeholder.size());
    std::copy(signed_manifest.begin(), signed_manifest.end(), asset.begin() + embed_offset);

    std::istringstream signed_stream(
        std::string(asset.begin(), asset.end()), std::ios::in | std::ios::binary);
    auto reader = c2pa::Reader(context, "image/png", signed_stream);
    expect_valid_manifest(json::parse(reader.json()));
}

TEST_F(EmbeddableTest, SetDataHashExclusionsRequiresPlaceholder) {
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .create_context();
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto builder = c2pa::Builder(context, manifest);
    EXPECT_THROW(builder.set_data_hash_exclusions({{0, 1}}), c2pa::C2paException);
}

// BoxHash: standard Builder::sign() flow with prefer_box_hash enabled.
TEST_F(EmbeddableTest, BoxHashStandardSign) {
    auto manifest_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("boxhash_standard.jpg");

    auto context = c2pa::Context::ContextBuilder()
        .with_signer(c2pa_test::create_test_signer())
        .with_json(R"({
            "builder": {
                "prefer_box_hash": true
            }
        })")
        .create_context();
    auto builder = c2pa::Builder(context, manifest_json);

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(source_path, output_path));
    ASSERT_FALSE(manifest_data.empty());

    auto reader = c2pa::Reader(context, output_path);
    expect_valid_manifest(json::parse(reader.json()));
}

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

    ASSERT_FALSE(builder.needs_placeholder("image/jpeg"));
}

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

    ASSERT_FALSE(builder.needs_placeholder("image/jpeg"));
    std::ifstream asset_stream(source_asset, std::ios::binary);
    ASSERT_TRUE(asset_stream.is_open());
    builder.update_hash_from_stream("image/jpeg", asset_stream);
    auto manifest_bytes = builder.sign_embeddable("image/jpeg");
    ASSERT_FALSE(manifest_bytes.empty());

    auto asset = read_bytes(source_asset);
    asset.insert(asset.begin() + 2, manifest_bytes.begin(), manifest_bytes.end());
    std::istringstream signed_stream(
        std::string(asset.begin(), asset.end()), std::ios::in | std::ios::binary);
    auto reader = c2pa::Reader(context, "image/jpeg", signed_stream);
    expect_valid_manifest(json::parse(reader.json()));
}

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

    ASSERT_TRUE(builder.needs_placeholder("video/mp4"));
}

class EmbeddableBlankFormatTest : public EmbeddableTest,
                                  public ::testing::WithParamInterface<std::string> {};

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
