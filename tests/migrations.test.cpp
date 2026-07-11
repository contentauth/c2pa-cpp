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
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

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
// Current API: there is no single-call drop-in, but the whole behavior is a short
//   reimplementation on top of Builder/Reader:
//     1. add_ingredient(json, source_path) forms the ingredient in a working store,
//     2. write_ingredient_archive(id, buf) archives just that ingredient,
//     3. Reader(ctx, "application/c2pa", buf) reads it back, and
//        Reader::get_resource(uri, path) writes the thumbnail / manifest_data to disk,
//     4. Builder(ctx, {"ingredients": [...]}) + set_base_path(dir) + sign() reuses the
//        extracted directory to sign a carrier.
//   This is the flow the ingredient-from-file example demonstrates, and the two tests
//   below exercise it end to end.
// Notes: The identifier fields in the recovered ingredient JSON point at internal JUMBF
//   URIs. To make the directory self-contained (and loadable via set_base_path) we
//   rewrite each identifier to the bare file name it was written under. File names are
//   derived from the ingredient's instance_id so multiple ingredients can share one
//   output directory without their thumbnail / manifest_data files colliding, which is
//   what the second (adversarial) test verifies. set_base_path is slated for a future
//   deprecation (see add_resource); the example uses it because it is the terser path.
TEST_F(LegacyApiMigrationTest, ReadIngredientFile_ExtractToDirThenSignCarrier) {
    auto context = std::make_shared<c2pa::Context>();

    // Collision-resistant filename stem from an instance_id (the trailing UUID),
    // and a mime -> extension mapping, inline as in the example.
    auto uuid_stem = [](const std::string& instance_id) {
        auto colon = instance_id.rfind(':');
        std::string stem = (colon != std::string::npos) ? instance_id.substr(colon + 1)
                                                         : instance_id;
        return stem.empty() ? std::string("ingredient") : stem;
    };
    auto ext_for_format = [](const std::string& mime) {
        std::string ext = mime.substr(mime.find('/') + 1);
        return ext == "jpeg" ? std::string("jpg") : ext;
    };

    auto output_dir = get_temp_path("read_ingredient_extract_dir");
    fs::create_directories(output_dir);

    // --- Extract phase: form C.jpg as an ingredient, archive it, read it back, and
    //     write its JSON + thumbnail + nested manifest_data to output_dir. C.jpg carries
    //     its own manifest store, so it exercises the manifest_data path too.
    json ingredient;
    {
        const std::string ingredient_id = "my-ingredient";
        auto builder = c2pa::Builder(context, "{}");
        builder.add_ingredient(
            R"({"label": ")" + ingredient_id +
                R"(", "title": "C.jpg", "relationship": "componentOf"})",
            c2pa_test::get_fixture_path("C.jpg"));

        std::stringstream archive_buf(std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_NO_THROW(builder.write_ingredient_archive(ingredient_id, archive_buf));
        archive_buf.seekg(0);

        c2pa::Reader reader(context, "application/c2pa", archive_buf);
        auto manifest_store = json::parse(reader.json());
        std::string active = manifest_store["active_manifest"];
        auto& ingredients = manifest_store["manifests"][active]["ingredients"];
        ASSERT_FALSE(ingredients.empty());
        ingredient = ingredients[0];

        std::string stem = uuid_stem(ingredient.value("instance_id", std::string()));

        ASSERT_TRUE(ingredient.contains("thumbnail"));
        std::string thumb_uri = ingredient["thumbnail"]["identifier"];
        std::string thumb_name = stem + "." +
            ext_for_format(ingredient["thumbnail"]["format"]);
        ASSERT_GT(reader.get_resource(thumb_uri, output_dir / thumb_name), 0);
        ingredient["thumbnail"]["identifier"] = thumb_name;

        ASSERT_TRUE(ingredient.contains("manifest_data"));
        std::string md_uri = ingredient["manifest_data"]["identifier"];
        std::string md_name = stem + ".c2pa";
        ASSERT_GT(reader.get_resource(md_uri, output_dir / md_name), 0);
        ingredient["manifest_data"]["identifier"] = md_name;

        // Drop the archive-only assertion label so the ingredient keys cleanly on reuse.
        ingredient.erase("label");
        std::ofstream(output_dir / (stem + ".json")) << ingredient.dump(2);

        // The data_dir side effect: JSON + thumbnail + manifest_data all on disk.
        EXPECT_TRUE(fs::exists(output_dir / (stem + ".json")));
        EXPECT_GT(fs::file_size(output_dir / thumb_name), 0u);
        EXPECT_GT(fs::file_size(output_dir / md_name), 0u);
    }

    // --- Sign phase: load the extracted ingredient from output_dir and embed it into a
    //     carrier asset, resolving the thumbnail / manifest_data files via set_base_path.
    json sign_manifest = {{"ingredients", json::array({ingredient})}};
    auto builder = c2pa::Builder(context, sign_manifest.dump());
    builder.set_base_path(output_dir.string());

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("read_ingredient_signed.jpg");
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(c2pa_test::get_fixture_path("A.jpg"),
                                                 output_path, signer));
    EXPECT_FALSE(manifest_data.empty());

    auto reader = c2pa::Reader::from_asset(context, output_path);
    ASSERT_TRUE(reader.has_value());
    auto parsed = json::parse(reader->json());
    std::string active = parsed["active_manifest"];
    auto& signed_ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(signed_ingredients.size(), 1u);
    EXPECT_EQ(signed_ingredients[0]["title"], "C.jpg");

    fs::remove_all(output_dir);
}

// Extract two ingredients from the same working store into the SAME directory
// and confirm their thumbnail and manifest_data files do not overwrite each other, then sign
// a carrier with both and confirm both survive the round-trip. Fixed names like
// "thumbnail.jpg" and "manifest_data.c2pa" would fail this test,
// instance_id-derived names to ensure name unicity pass it.
TEST_F(LegacyApiMigrationTest, ReadIngredientFile_MultipleIngredientsSameStoreNoCollision) {
    auto context = std::make_shared<c2pa::Context>();

    auto uuid_stem = [](const std::string& instance_id) {
        auto colon = instance_id.rfind(':');
        std::string stem = (colon != std::string::npos) ? instance_id.substr(colon + 1)
                                                         : instance_id;
        return stem.empty() ? std::string("ingredient") : stem;
    };
    auto ext_for_format = [](const std::string& mime) {
        std::string ext = mime.substr(mime.find('/') + 1);
        return ext == "jpeg" ? std::string("jpg") : ext;
    };

    auto output_dir = get_temp_path("read_ingredient_multi_dir");
    fs::create_directories(output_dir);

    // Two ingredients in one working store, each keyed by a distinct instance_id.
    // A.jpg has no nested manifest; C.jpg carries one, so only C.jpg writes a .c2pa file.
    struct Spec { std::string id; std::string fixture; std::string title; };
    const std::vector<Spec> specs = {
        {"mig:ingredient-A", "A.jpg", "A.jpg"},
        {"mig:ingredient-C", "C.jpg", "C.jpg"},
    };

    auto builder = c2pa::Builder(context, "{}");
    for (const auto& s : specs) {
        builder.add_ingredient(
            R"({"instance_id": ")" + s.id + R"(", "title": ")" + s.title +
                R"(", "relationship": "componentOf"})",
            c2pa_test::get_fixture_path(s.fixture));
    }

    std::vector<json> extracted;
    std::vector<std::string> thumb_names;
    for (const auto& s : specs) {
        std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_NO_THROW(builder.write_ingredient_archive(s.id, buf));
        buf.seekg(0);

        c2pa::Reader reader(context, "application/c2pa", buf);
        auto store = json::parse(reader.json());
        std::string active = store["active_manifest"];
        auto& ings = store["manifests"][active]["ingredients"];
        ASSERT_FALSE(ings.empty());
        json ingredient = ings[0];

        std::string stem = uuid_stem(s.id);
        ASSERT_TRUE(ingredient.contains("thumbnail"));
        std::string thumb_name = stem + "." +
            ext_for_format(ingredient["thumbnail"]["format"]);
        ASSERT_GT(reader.get_resource(ingredient["thumbnail"]["identifier"],
                                      output_dir / thumb_name), 0);
        ingredient["thumbnail"]["identifier"] = thumb_name;
        thumb_names.push_back(thumb_name);

        if (ingredient.contains("manifest_data")) {
            std::string md_name = stem + ".c2pa";
            ASSERT_GT(reader.get_resource(ingredient["manifest_data"]["identifier"],
                                          output_dir / md_name), 0);
            ingredient["manifest_data"]["identifier"] = md_name;
        }

        ingredient.erase("label");
        std::ofstream(output_dir / (stem + ".json")) << ingredient.dump(2);
        extracted.push_back(ingredient);
    }

    // Collision guard: the two ingredients wrote distinct, non-empty thumbnail files.
    ASSERT_EQ(thumb_names.size(), 2u);
    EXPECT_NE(thumb_names[0], thumb_names[1]);
    EXPECT_GT(fs::file_size(output_dir / thumb_names[0]), 0u);
    EXPECT_GT(fs::file_size(output_dir / thumb_names[1]), 0u);

    size_t json_count = 0, c2pa_count = 0;
    for (const auto& entry : fs::directory_iterator(output_dir)) {
        if (entry.path().extension() == ".json") ++json_count;
        if (entry.path().extension() == ".c2pa") ++c2pa_count;
    }
    EXPECT_EQ(json_count, 2u);
    EXPECT_EQ(c2pa_count, 1u);

    // Sign an asset with both extracted ingredients, resolving resources from the dir.
    json sign_manifest = {{"ingredients", extracted}};
    auto sign_builder = c2pa::Builder(context, sign_manifest.dump());
    sign_builder.set_base_path(output_dir.string());

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("read_ingredient_multi_signed.jpg");
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = sign_builder.sign(c2pa_test::get_fixture_path("A.jpg"),
                                                      output_path, signer));
    EXPECT_FALSE(manifest_data.empty());

    // Both ingredients survived the round-trip into the signed asset.
    auto reader = c2pa::Reader::from_asset(context, output_path);
    ASSERT_TRUE(reader.has_value());
    auto parsed = json::parse(reader->json());
    std::string active = parsed["active_manifest"];
    auto& signed_ings = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(signed_ings.size(), 2u);
    std::vector<std::string> titles;
    for (const auto& ing : signed_ings) titles.push_back(ing.value("title", std::string()));
    EXPECT_NE(std::find(titles.begin(), titles.end(), "A.jpg"), titles.end());
    EXPECT_NE(std::find(titles.begin(), titles.end(), "C.jpg"), titles.end());

    fs::remove_all(output_dir);
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
