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

#include <c2pa.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "include/test_utils.hpp"

using nlohmann::json;
namespace fs = std::filesystem;

class LegacyApiMigrationTest : public ::testing::Test {
protected:
    std::vector<fs::path> temp_files;
    bool cleanup_temp_files = true;  // Set to false to keep files for debugging

    // Path for a temp file under the build directory
    fs::path get_temp_path(const std::string& name) {
        fs::path current_dir = fs::path(__FILE__).parent_path();
        fs::path build_dir = current_dir.parent_path() / "build";
        if (!fs::exists(build_dir)) {
            fs::create_directories(build_dir);
        }
        fs::path temp_path = build_dir / ("migration-" + name);
        temp_files.push_back(temp_path);
        return temp_path;
    }

    void TearDown() override {
        if (cleanup_temp_files) {
            for (const auto& path : temp_files) {
                if (fs::exists(path)) {
                    fs::remove(path);
                }
            }
        }
        temp_files.clear();
    }
};

// Legacy API:  std::optional<std::string> c2pa::read_file(const fs::path& source_path,
//                                                       std::optional<fs::path> data_dir = nullopt)
// What it did: read a file and returned the manifest-store JSON, or an empty
//   optional when the asset had no C2PA manifest.
// Current API: Reader::from_asset(ctx, format, stream) returns std::optional<Reader>
//   (std::nullopt when no manifest), then Reader::json() yields the manifest store.
// Context use: the old functions took no context and ran on thread-local
//   default settings. The current APIs take an explicit Context. We pass a
//   shared_ptr<Context>, so the shared_ptr keeps the context alive for the
//   Reader/Builder that holds it.
TEST_F(LegacyApiMigrationTest, ReadFile_NoManifest_ReturnsEmptyOptional) {
    auto test_file = c2pa_test::get_fixture_path("A.jpg");  // A.jpg has no manifest
    std::ifstream stream(test_file, std::ios::binary);
    ASSERT_TRUE(stream.is_open());

    auto context = std::make_shared<c2pa::Context>();
    auto reader = c2pa::Reader::from_asset(context, "image/jpeg", stream);

    EXPECT_FALSE(reader.has_value());
}

// Legacy API:  c2pa::read_file(source_path) for an asset that has a manifest.
// What it did: returned the manifest-store JSON (with "manifests" and
//   "active_manifest" keys); the format was derived from the path internally.
// Current API: the path overload Reader::from_asset(ctx, source_path) is an
//   equivalent. It opens the file and infers the format from the extension,
//   like the old path-based read_file.
//   With a stream there is no path to infer from, so the caller owns
//   format detection and must pass the MIME type, which happenes when
//   the stream overload, Reader::from_asset(ctx, format, stream), is used).
class ReadFileWithManifestMigrationTest
    : public ::testing::TestWithParam<std::string> {};

INSTANTIATE_TEST_SUITE_P(ReadFileWithManifestMigrationTest,
                         ReadFileWithManifestMigrationTest,
                         ::testing::Values("C.jpg",
                                           "video1.mp4",
                                           "C.dng",
                                           "sample1_signed.wav"));

TEST_P(ReadFileWithManifestMigrationTest, ReadFile_WithManifest_ReturnsManifestJson) {
    auto filename = GetParam();
    auto test_file = c2pa_test::get_fixture_path(filename);

    auto context = std::make_shared<c2pa::Context>();
    auto reader = c2pa::Reader::from_asset(context, test_file);
    ASSERT_TRUE(reader.has_value());

    auto parsed = json::parse(reader->json());
    EXPECT_TRUE(parsed.contains("manifests"));
    EXPECT_TRUE(parsed.contains("active_manifest"));
}

// Legacy API:  c2pa::read_file(source_path, data_dir)
// What it did: returned the manifest-store JSON AND, as a side effect, wrote
//   manifest_store.json plus every binary resource (thumbnails, etc.) into data_dir.
// Current API: there is no folder-dump equivalent. The caller gets the JSON from
//   Reader::json() and pulls resources individually with Reader::get_resource(uri, dest),
//   choosing the destination per resource.
// Notes: The JSON is similar, the implicit "write everything to a
//   directory" behavior is gone. This example reproduces the following: read the
//   JSON, then extract one resource to a path of the caller's choosing. The caller
//   now controls the destination instead of receiving a directory dump.
TEST_F(LegacyApiMigrationTest, ReadFile_WithDataDir_ExtractResources) {
    auto test_file = c2pa_test::get_fixture_path("C.jpg");
    std::ifstream stream(test_file, std::ios::binary);
    ASSERT_TRUE(stream.is_open());

    auto context = std::make_shared<c2pa::Context>();
    // Stream overload: no path to infer from, so the caller supplies the MIME type.
    auto reader = c2pa::Reader::from_asset(context, "image/jpeg", stream);
    ASSERT_TRUE(reader.has_value());

    auto parsed = json::parse(reader->json());
    ASSERT_TRUE(parsed.contains("active_manifest"));

    // Extract the active manifest's claim thumbnail to a file the caller picks,
    // standing in for the old data_dir resource dump.
    std::string thumbnail_uri = "self#jumbf=c2pa.assertions/c2pa.thumbnail.claim.jpeg";
    fs::path output_file = get_temp_path("read_file_thumbnail.jpg");

    auto byte_count = reader->get_resource(thumbnail_uri, output_file);
    EXPECT_GT(byte_count, 0);
    EXPECT_TRUE(fs::exists(output_file));
    EXPECT_GT(fs::file_size(output_file), 0u);
}

// Legacy API:  std::string c2pa::read_ingredient_file(const fs::path& source_path,
//                                                   const fs::path& data_dir)
// What it did: read an asset, returned the formed ingredient JSON string, and
//   wrote the ingredient's binary resources (thumbnail, and any manifest data) to
//   data_dir.
// Current API: Builder::add_ingredient(ingredient_json, source_path). The Builder
//   reads the source, forms the ingredient, and embeds its resources in the working
//   store; the call returns void. Forming an ingredient and recovering its
//   JSON + resources are both available through the working store. add_ingredient
//   forms it; archiving the working store and reading it back yields the same
//   ingredient JSON, and Reader::get_resource pulls the resources. A dedicated
//   "file -> ingredient JSON + thumbnail on disk" wrapper is redundant.
// Notes: This example reconstructs the old return value and side effect
//   as closely as possible using the read-filter-rebuild pattern: add the
//   ingredient, archive the working store, read the archive back, and pull the
//   ingredient JSON + thumbnail. Two differences worth noting:
//     - read_ingredient_file derived "title" from the file name (it read a path
//       and ran extension_to_mime/title logic). add_ingredient only sets "title"
//       if the caller supplies it in the ingredient JSON, so we pass it
//       explicitly to reproduce the old output.
//     - The old top-level "format" was the MIME type "image/jpeg" (read_ingredient_file
//       went through extension_to_mime). The current working-store ingredient stores
//       the short extension "jpg" at top level, but the MIME type is still present on
//       the thumbnail ("image/jpeg"). The old test only did a substring search for
//       "image/jpeg" over the whole ingredient JSON, which still matches today via
//       the thumbnail. We assert both the new top-level "jpg" and the thumbnail MIME
//       so the difference is explicit rather than hidden behind a substring match.
//   Generated identifiers (instance_id, the thumbnail JUMBF URI) are not pinned.
TEST_F(LegacyApiMigrationTest, ReadIngredientFile_ReconstructIngredientJsonAndResources) {
    auto context = std::make_shared<c2pa::Context>();
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // Case 1: A.jpg, a plain ingredient with no embedded manifest store.
    {
        auto builder = c2pa::Builder(context, manifest);
        builder.add_ingredient(R"({"title": "A.jpg", "relationship": "componentOf"})",
                               c2pa_test::get_fixture_path("A.jpg"));

        auto archive_path = get_temp_path("ingredient_a.c2pa");
        ASSERT_NO_THROW(builder.to_archive(archive_path));

        std::ifstream archive_in(archive_path, std::ios::binary);
        ASSERT_TRUE(archive_in.is_open());
        c2pa::Reader archive_reader(context, "application/c2pa", archive_in);
        auto parsed = json::parse(archive_reader.json());

        ASSERT_TRUE(parsed.contains("active_manifest"));
        std::string active = parsed["active_manifest"];
        auto ingredients = parsed["manifests"][active]["ingredients"];
        ASSERT_EQ(ingredients.size(), 1u);

        // The reconstructed ingredient carries the same fields the old
        // read_ingredient_file return value did (title, format, thumbnail,
        // relationship).
        auto ingredient = ingredients[0];
        EXPECT_EQ(ingredient["title"], "A.jpg");
        EXPECT_EQ(ingredient["relationship"], "componentOf");
        // Top-level format is the short extension now, the old MIME "image/jpeg"
        // still appears on the thumbnail.
        EXPECT_EQ(ingredient["format"], "jpg");
        ASSERT_TRUE(ingredient.contains("thumbnail"));
        EXPECT_EQ(ingredient["thumbnail"]["format"], "image/jpeg");
        ASSERT_TRUE(ingredient["thumbnail"].contains("identifier"));

        // The data_dir side effect, now caller-driven: pull the thumbnail resource.
        std::string thumbnail_id = ingredient["thumbnail"]["identifier"];
        auto thumb_path = get_temp_path("ingredient_a_thumb.jpg");
        auto byte_count = archive_reader.get_resource(thumbnail_id, thumb_path);
        EXPECT_GT(byte_count, 0);
        EXPECT_TRUE(fs::exists(thumb_path));
        EXPECT_GT(fs::file_size(thumb_path), 0u);
    }

    // Case 2: C.jpg, which carries a manifest store. The old
    // ReadIngredientFileWhoHasAManifestStore checked that the ingredient additionally
    // exposed provenance. The reconstructed ingredient does the same.
    {
        auto builder = c2pa::Builder(context, manifest);
        builder.add_ingredient(R"({"title": "C.jpg", "relationship": "componentOf"})",
                               c2pa_test::get_fixture_path("C.jpg"));

        auto archive_path = get_temp_path("ingredient_c.c2pa");
        ASSERT_NO_THROW(builder.to_archive(archive_path));

        std::ifstream archive_in(archive_path, std::ios::binary);
        ASSERT_TRUE(archive_in.is_open());
        c2pa::Reader archive_reader(context, "application/c2pa", archive_in);
        auto parsed = json::parse(archive_reader.json());

        std::string active = parsed["active_manifest"];
        auto ingredients = parsed["manifests"][active]["ingredients"];
        ASSERT_EQ(ingredients.size(), 1u);
        auto ingredient = ingredients[0];

        EXPECT_EQ(ingredient["title"], "C.jpg");
        EXPECT_EQ(ingredient["relationship"], "componentOf");
        // Provenance markers present because C.jpg has a manifest store. These are
        // the extra fields the old ReadIngredientFileWhoHasAManifestStore checked.
        EXPECT_TRUE(ingredient.contains("active_manifest"));
        ASSERT_TRUE(ingredient.contains("manifest_data"));
        EXPECT_EQ(ingredient["manifest_data"]["format"], "application/c2pa");
        EXPECT_TRUE(ingredient.contains("validation_results"));
    }
}

// Legacy API:  std::string c2pa::read_ingredient_file(const fs::path& source_path,
//                                                   const fs::path& data_dir)
// What it did: read an asset, returned the formed ingredient JSON string, and
//   wrote the ingredient's binary resources (thumbnail, and any manifest data) to
//   data_dir.
// Current API (preferred): the dedicated ingredient-archive APIs
//   Builder::write_ingredient_archive(id, dest) on the producer and
//   Builder::add_ingredient_from_archive(src) on the consumer.
// Notes: This is an example of a replacement for moving a formed ingredient between
//   builders. No JSON parsing and no add_resource loop. The formed ingredient
//   travels as a self-contained .c2pa archive rather than a JSON string plus loose
//   thumbnail files on disk. See docs/selective-manifests.md, "Extracting
//   ingredients from a working store".
TEST_F(LegacyApiMigrationTest, ReadIngredientFile_ViaDedicatedIngredientArchive) {
    auto context = std::make_shared<c2pa::Context>();
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // Producer: form an ingredient keyed by instance_id, archive it.
    auto producer = c2pa::Builder(context, manifest);
    producer.add_ingredient(
        R"({"relationship": "componentOf", "instance_id": "mig:ingredient-A"})",
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream archive(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(producer.write_ingredient_archive("mig:ingredient-A", archive));

    // Consumer: load only that ingredient, no JSON parsing.
    auto consumer = c2pa::Builder(context, manifest);
    archive.seekg(0);
    ASSERT_NO_THROW(consumer.add_ingredient_from_archive(archive));

    // Sign and confirm the signed output lists the ingredient.
    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("ingredient_archive_signed.jpg");
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = consumer.sign(c2pa_test::get_fixture_path("A.jpg"),
                                                  output_path, signer));
    EXPECT_FALSE(manifest_data.empty());

    auto reader = c2pa::Reader::from_asset(context, output_path);
    ASSERT_TRUE(reader.has_value());
    auto parsed = json::parse(reader->json());
    std::string active = parsed["active_manifest"];
    EXPECT_GE(parsed["manifests"][active]["ingredients"].size(), 1u);
}

// Legacy API:  void c2pa::sign_file(const fs::path& source_path, const fs::path& dest_path,
//                                const char* manifest, SignerInfo* signer_info,
//                                std::optional<fs::path> data_dir = nullopt)
// What it did: added the manifest and signed source_path into dest_path, using a
//   SignerInfo struct, optionally resolving resources from data_dir.
// Current API: build with Builder(ctx, manifest), construct a Signer, then
//   Builder::sign(source_path, dest_path, signer).
// Notes: The free sign_file function is gone, along with its SignerInfo* and
//   data_dir parameters. Signing now goes through a Signer object and
//   Builder::sign, which has no data_dir; resources are added to the builder (or
//   via a base path) before signing rather than at sign time. The SignerInfo type
//   alias itself still exists (include/c2pa.hpp), it is just no longer how you sign.
//   The behavior, sign an asset to a readable signed output, is reproduced.
TEST_F(LegacyApiMigrationTest, SignFile_SignsAssetToOutput) {
    auto context = std::make_shared<c2pa::Context>();
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto builder = c2pa::Builder(context, manifest);
    auto signer = c2pa_test::create_test_signer();

    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("sign_file_output.jpg");

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(source_path, output_path, signer));
    EXPECT_FALSE(manifest_data.empty());

    auto reader = c2pa::Reader::from_asset(context, output_path);
    ASSERT_TRUE(reader.has_value());
    auto parsed = json::parse(reader->json());
    EXPECT_TRUE(parsed.contains("active_manifest"));
}
