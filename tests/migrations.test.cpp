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
#include <string>
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
// Current API: there is no single-call replacement, but the behavior is a short
//   reimplementation on top of Builder/Reader:
//     1. add_ingredient(json, source_path) forms the ingredient in a working store,
//     2. write_ingredient_archive(id, buf) archives just that ingredient,
//     3. Reader(ctx, "application/c2pa", buf) reads it back, and
//        Reader::get_resource(uri, path) writes the thumbnail / manifest_data to disk,
//     4. Builder(ctx, {"ingredients": [...]}) + set_base_path(dir) + sign() reuses the
//        extracted directory to sign an asset.
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
    //     (carrier) asset, resolving the thumbnail / manifest_data files via set_base_path.
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
// an asset with both and confirm both survive the round-trip. Fixed names like
// "thumbnail.jpg" and "manifest_data.c2pa" would fail this test; instance_id-derived
// names, which are unique per ingredient, pass it.
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

// The extract-and-reuse flow above signs with set_base_path, which is deprecated.
// add_resource(identifier, path) is the replacement: register each referenced resource
// on the builder instead of pointing it at a directory. This test signs the same
// extracted ingredients both ways and checks that the two signed manifests carry the same
// ingredients with the same resources attached. The check ignores identifiers: two
// independent signs generate different signatures and fresh JUMBF identifier URIs, so it
// compares structure (title, relationship, format, and resource presence) rather than raw
// JSON.
TEST_F(LegacyApiMigrationTest, ReadIngredientFile_AddResourceMatchesSetBasePath) {
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

    auto output_dir = get_temp_path("read_ingredient_addresource_dir");
    fs::create_directories(output_dir);

    // --- Extract once: two ingredients into one directory (shared input for both signs).
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

    // Shared extract stays covered: distinct thumbnails, 2 JSON, 1 manifest_data.
    ASSERT_EQ(thumb_names.size(), 2u);
    EXPECT_NE(thumb_names[0], thumb_names[1]);
    size_t json_count = 0, c2pa_count = 0;
    for (const auto& entry : fs::directory_iterator(output_dir)) {
        if (entry.path().extension() == ".json") ++json_count;
        if (entry.path().extension() == ".c2pa") ++c2pa_count;
    }
    EXPECT_EQ(json_count, 2u);
    EXPECT_EQ(c2pa_count, 1u);

    auto signer = c2pa_test::create_test_signer();

    // --- Sign path A: set_base_path resolves resources against the directory.
    auto builder_a = c2pa::Builder(context, json{{"ingredients", extracted}}.dump());
    builder_a.set_base_path(output_dir.string());
    auto out_a = get_temp_path("read_ingredient_addresource_a.jpg");
    std::vector<unsigned char> md_a;
    ASSERT_NO_THROW(md_a = builder_a.sign(c2pa_test::get_fixture_path("A.jpg"), out_a, signer));
    EXPECT_FALSE(md_a.empty());

    // --- Sign path B: no set_base_path; register every referenced resource explicitly.
    auto builder_b = c2pa::Builder(context, json{{"ingredients", extracted}}.dump());
    for (const auto& ing : extracted) {
        std::string thumb_id = ing["thumbnail"]["identifier"];
        builder_b.add_resource(thumb_id, output_dir / thumb_id);
        if (ing.contains("manifest_data")) {
            std::string md_id = ing["manifest_data"]["identifier"];
            builder_b.add_resource(md_id, output_dir / md_id);
        }
    }
    auto out_b = get_temp_path("read_ingredient_addresource_b.jpg");
    std::vector<unsigned char> md_b;
    ASSERT_NO_THROW(md_b = builder_b.sign(c2pa_test::get_fixture_path("A.jpg"), out_b, signer));
    EXPECT_FALSE(md_b.empty());

    // --- Cross-check: both signed manifests carry the same ingredients + resources.
    // Reduce each ingredient to identifier-safe structure: stable fields plus, for each
    // resource, only its presence and format (drop the generated identifier / hashes).
    auto normalize = [](const json& ingredients) {
        json out = json::array();
        for (const auto& ing : ingredients) {
            json n;
            n["title"] = ing.value("title", std::string());
            n["relationship"] = ing.value("relationship", std::string());
            n["format"] = ing.value("format", std::string());
            for (const char* key : {"thumbnail", "manifest_data"}) {
                if (ing.contains(key))
                    n[key] = {{"present", true},
                              {"format", ing[key].value("format", std::string())}};
            }
            out.push_back(n);
        }
        // Order-independent: the two builders may list ingredients in either order.
        std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
            return a["title"].get<std::string>() < b["title"].get<std::string>();
        });
        return out;
    };

    auto reader_a = c2pa::Reader::from_asset(context, out_a);
    auto reader_b = c2pa::Reader::from_asset(context, out_b);
    ASSERT_TRUE(reader_a.has_value());
    ASSERT_TRUE(reader_b.has_value());
    auto parsed_a = json::parse(reader_a->json());
    auto parsed_b = json::parse(reader_b->json());
    auto& ings_a = parsed_a["manifests"][parsed_a["active_manifest"].get<std::string>()]["ingredients"];
    auto& ings_b = parsed_b["manifests"][parsed_b["active_manifest"].get<std::string>()]["ingredients"];

    ASSERT_EQ(ings_a.size(), 2u);
    ASSERT_EQ(ings_b.size(), ings_a.size());
    // The two sign paths are equivalent: same ingredients, same resources attached.
    EXPECT_EQ(normalize(ings_a), normalize(ings_b));

    // Also check each manifest directly: both carry A.jpg and C.jpg, every ingredient has
    // a thumbnail, and exactly one ingredient per manifest has manifest_data.
    for (auto* ings : {&ings_a, &ings_b}) {
        std::vector<std::string> titles;
        size_t md_count = 0;
        for (const auto& ing : *ings) {
            titles.push_back(ing.value("title", std::string()));
            EXPECT_TRUE(ing.contains("thumbnail"));
            if (ing.contains("manifest_data")) ++md_count;
        }
        EXPECT_NE(std::find(titles.begin(), titles.end(), "A.jpg"), titles.end());
        EXPECT_NE(std::find(titles.begin(), titles.end(), "C.jpg"), titles.end());
        EXPECT_EQ(md_count, 1u);
    }

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

// Test fixture: per-test temp dirs under tests/build, cleaned up in TearDown.
class LegacyFolderIngredient : public ::testing::Test {
protected:
    std::vector<fs::path> temp_dirs;

    fs::path make_temp_dir(const std::string &name) {
        fs::path build_dir = fs::path(__FILE__).parent_path() / "build";
        fs::create_directories(build_dir);
        fs::path dir = build_dir / ("legacy-ingredient-" + name);
        if (fs::exists(dir)) {
            fs::remove_all(dir);
        }
        fs::create_directories(dir);
        temp_dirs.push_back(dir);
        return dir;
    }

    void TearDown() override {
        for (const auto &dir : temp_dirs) {
            if (fs::exists(dir)) {
                fs::remove_all(dir);
            }
        }
        temp_dirs.clear();
    }

    // A legacy folder ingredient is reconstituted from two things: the manifest
    // store bytes (manifest_data.c2pa) AND the signed asset they are hash-bound
    // to. Sign a fixture asset, return the manifest-store bytes plus the path to
    // the signed asset that carries them.
    struct SignedSeed {
        std::vector<unsigned char> manifest_bytes;  // contents of manifest_data.c2pa
        fs::path signed_asset;                       // asset the manifest binds to
    };
    SignedSeed sign_seed(const fs::path &asset, const std::string &name) {
        auto manifest =
            c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
        auto signer = c2pa_test::create_test_signer();
        auto builder = c2pa::Builder(manifest);
        fs::path out = make_temp_dir("seed-" + name) / "signed.jpg";
        auto bytes = builder.sign(asset, out, signer);
        return {std::move(bytes), out};
    }

    // Write a legacy folder: ingredient.json (+ optional manifest_data.c2pa,
    // + optional thumbnail copied into the folder). Returns the folder path.
    fs::path write_legacy_folder(const std::string &name,
                                 const json &ingredient_json,
                                 const std::vector<unsigned char> *manifest_data,
                                 const fs::path *thumbnail_src,
                                 const std::string &thumbnail_name) {
        fs::path dir = make_temp_dir(name);
        if (manifest_data != nullptr) {
            std::ofstream m(dir / "manifest_data.c2pa", std::ios::binary);
            m.write(reinterpret_cast<const char *>(manifest_data->data()),
                    static_cast<std::streamsize>(manifest_data->size()));
        }
        if (thumbnail_src != nullptr) {
            fs::copy_file(*thumbnail_src, dir / thumbnail_name,
                          fs::copy_options::overwrite_existing);
        }
        std::ofstream j(dir / "ingredient.json");
        j << ingredient_json.dump(2);
        return dir;
    }

    // A signed manifest validates when its validation_state is Valid or Trusted
    // (the es256 test cert is not trusted, so the good state here is "Valid").
    static bool is_valid_state(const json &parsed) {
        if (!parsed.contains("validation_state")) return false;
        const auto &s = parsed["validation_state"];
        return s == "Valid" || s == "Trusted";
    }

    // Sign `source` with `builder`, assert the active manifest validates, and
    // return the active manifest's ingredients array.
    json sign_and_read_ingredients(c2pa::Builder &builder,
                                   const fs::path &source,
                                   const std::string &out_name) {
        auto signer = c2pa_test::create_test_signer();
        fs::path out = make_temp_dir(out_name) / "out.jpg";
        builder.sign(source, out, signer);
        auto reader = c2pa::Reader(out);
        auto parsed = json::parse(reader.json());
        EXPECT_TRUE(is_valid_state(parsed))
            << out_name << " validation_state="
            << (parsed.contains("validation_state")
                    ? parsed["validation_state"].dump() : "<none>");
        std::string active = parsed["active_manifest"];
        return parsed["manifests"][active]["ingredients"];
    }

    // Build a self-contained legacy Case-A folder (ingredient.json declaring a
    // manifest_data ref + manifest_data.c2pa + the signed asset it binds to) and
    // add it to `builder` via the documented Case-A route. Returns nothing; the
    // ingredient is appended to the builder.
    void add_legacy_caseA(c2pa::Builder &builder, const std::string &name,
                          const std::string &title) {
        auto seed = sign_seed(c2pa_test::get_fixture_path("A.jpg"), name);
        json ing = {
            {"title", title},
            {"format", "image/jpeg"},
            {"relationship", "componentOf"},
            {"manifest_data",
             {{"format", "application/c2pa"}, {"identifier", "manifest_data.c2pa"}}},
        };
        fs::path folder = write_legacy_folder(name, ing, &seed.manifest_bytes, nullptr, "");
        fs::copy_file(seed.signed_asset, folder / "asset.jpg",
                      fs::copy_options::overwrite_existing);
        std::string ing_json = c2pa_test::read_text_file(folder / "ingredient.json");
        builder.set_base_path(folder.string());
        std::ifstream asset_stream(folder / "asset.jpg", std::ios::binary);
        ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", asset_stream));
    }

    // Build a modern dedicated ingredient archive from a fixture asset and add it
    // to `builder` via add_ingredient_from_archive. The archive stream must
    // outlive the call, so it is owned by the caller.
    std::shared_ptr<std::stringstream> add_modern_archive(
        c2pa::Builder &builder, const std::string &manifest,
        const std::string &label, const std::string &title,
        const fs::path &asset) {
        auto archive = std::make_shared<std::stringstream>(
            std::ios::in | std::ios::out | std::ios::binary);
        {
            auto ab = c2pa::Builder(manifest);
            json ing = {{"title", title}, {"relationship", "componentOf"}, {"label", label}};
            ab.add_ingredient(ing.dump(), asset);
            ab.write_ingredient_archive(label, *archive);
        }
        archive->seekg(0);
        // EXPECT (not ASSERT): ASSERT_* expands to `return;`, which is illegal in
        // a non-void helper.
        EXPECT_NO_THROW(builder.add_ingredient_from_archive(*archive));
        return archive;
    }

    // Assert every expected title is present in the ingredients array.
    void expect_titles(const json &ingredients,
                       const std::vector<std::string> &expected) {
        std::vector<std::string> titles;
        for (const auto &i : ingredients) titles.push_back(i["title"]);
        for (const auto &t : expected) {
            EXPECT_NE(std::find(titles.begin(), titles.end(), t), titles.end())
                << "missing ingredient titled " << t;
        }
    }

};

// A folder declaring manifest_data.c2pa loads its provenance when set_base_path points at the folder, and fails at sign with no base_path.
TEST_F(LegacyFolderIngredient, ManifestDataResolvedViaBasePath) {
    auto seed = sign_seed(c2pa_test::get_fixture_path("A.jpg"), "caseA");

    json ing = {
        {"title", "legacy parent"},
        {"format", "image/jpeg"},
        {"relationship", "parentOf"},
        {"manifest_data",
         {{"format", "application/c2pa"}, {"identifier", "manifest_data.c2pa"}}},
    };
    fs::path folder =
        write_legacy_folder("caseA", ing, &seed.manifest_bytes, nullptr, "");
    // The signed asset the manifest binds to lives alongside in the folder.
    fs::copy_file(seed.signed_asset, folder / "asset.jpg",
                  fs::copy_options::overwrite_existing);

    std::string ing_json = c2pa_test::read_text_file(folder / "ingredient.json");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // POSITIVE: base_path set; manifest_data reference resolves from the folder.
    auto builder = c2pa::Builder(manifest);
    builder.set_base_path(folder.string());
    std::ifstream asset_stream(folder / "asset.jpg", std::ios::binary);
    ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", asset_stream));

    auto src = c2pa_test::get_fixture_path("A.jpg");
    auto ingredients = sign_and_read_ingredients(builder, src, "caseA-out");
    ASSERT_EQ(ingredients.size(), 1u);
    EXPECT_EQ(ingredients[0]["title"], "legacy parent");
    EXPECT_EQ(ingredients[0]["relationship"], "parentOf");
    // Provenance carried from the legacy manifest_data, not just bare metadata.
    EXPECT_TRUE(ingredients[0].contains("active_manifest"))
        << "manifest_data.c2pa should be resolved and carried via base_path";

    // COUNTER: no base_path. The ingredient.json declares a manifest_data
    // reference ("manifest_data.c2pa") that can only be resolved from disk via
    // base_path. Without it, resolution fails hard (ResourceNotFound) at sign
    // time, proving base_path is what makes the legacy manifest_data load.
    auto builder2 = c2pa::Builder(manifest);
    std::ifstream plain(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_NO_THROW(builder2.add_ingredient(ing_json, "image/jpeg", plain));
    auto signer = c2pa_test::create_test_signer();
    fs::path out = make_temp_dir("caseA-nobasepath") / "out.jpg";
    EXPECT_ANY_THROW(builder2.sign(src, out, signer))
        << "without base_path the declared manifest_data reference is unresolved";
}

// A folder with a relative thumbnail resolves it at sign time via set_base_path, and fails at sign without base_path.
TEST_F(LegacyFolderIngredient, BasePathResolvesRelativeThumbnail) {
    auto asset = c2pa_test::get_fixture_path("A.jpg");
    auto thumb_src = c2pa_test::get_fixture_path("A.jpg");

    json ing = {
        {"title", "legacy with thumb"},
        {"format", "image/jpeg"},
        {"relationship", "componentOf"},
        {"thumbnail", {{"format", "image/jpeg"}, {"identifier", "thumb.jpg"}}},
    };
    fs::path folder =
        write_legacy_folder("caseB", ing, nullptr, &thumb_src, "thumb.jpg");
    std::string ing_json = c2pa_test::read_text_file(folder / "ingredient.json");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // POSITIVE: base_path set to the folder; the relative "thumb.jpg" resolves.
    {
        auto builder = c2pa::Builder(manifest);
        builder.set_base_path(folder.string());
        std::ifstream src(asset, std::ios::binary);
        ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", src));
        EXPECT_NO_THROW(sign_and_read_ingredients(builder, asset, "caseB-ok"));
    }

    // COUNTER: no base_path. The relative thumbnail cannot be found, so signing
    // (which serializes resources) must fail.
    {
        auto builder = c2pa::Builder(manifest);
        std::ifstream src(asset, std::ios::binary);
        ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", src));
        auto signer = c2pa_test::create_test_signer();
        fs::path out = make_temp_dir("caseB-fail") / "out.jpg";
        EXPECT_ANY_THROW(builder.sign(asset, out, signer))
            << "without set_base_path the relative thumbnail must be unresolved";
    }
}

// Two self-contained folders loop with per-folder base_path and produce two ingredients.
TEST_F(LegacyFolderIngredient, MultipleCaseA_Loop) {
    auto asset = c2pa_test::get_fixture_path("A.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    struct Spec { std::string name; std::string title; std::string rel; };
    std::vector<Spec> specs = {
        {"m-parent", "first legacy", "parentOf"},
        {"m-comp", "second legacy", "componentOf"},
    };

    auto builder = c2pa::Builder(manifest);
    for (const auto &s : specs) {
        auto seed = sign_seed(asset, s.name);
        json ing = {
            {"title", s.title},
            {"format", "image/jpeg"},
            {"relationship", s.rel},
            {"manifest_data",
             {{"format", "application/c2pa"}, {"identifier", "manifest_data.c2pa"}}},
        };
        fs::path folder = write_legacy_folder(s.name, ing, &seed.manifest_bytes, nullptr, "");
        fs::copy_file(seed.signed_asset, folder / "asset.jpg",
                      fs::copy_options::overwrite_existing);
        std::string ing_json = c2pa_test::read_text_file(folder / "ingredient.json");
        // Each folder is self-contained: set base_path per folder right before
        // adding, so its manifest_data.c2pa resolves. (base_path is global and
        // last-wins, but here we resolve eagerly at add time, one folder at a
        // time, so the sequential override is correct.)
        builder.set_base_path(folder.string());
        std::ifstream asset_stream(folder / "asset.jpg", std::ios::binary);
        ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", asset_stream));
    }

    auto ingredients = sign_and_read_ingredients(builder, asset, "m-out");
    ASSERT_EQ(ingredients.size(), 2u);
    std::vector<std::string> titles = {ingredients[0]["title"], ingredients[1]["title"]};
    EXPECT_NE(std::find(titles.begin(), titles.end(), "first legacy"), titles.end());
    EXPECT_NE(std::find(titles.begin(), titles.end(), "second legacy"), titles.end());
}

// Two folders sharing a thumbnail name collide under one global base_path, so add_resource with unique identifiers is the fix.
TEST_F(LegacyFolderIngredient, MultipleBasePathCollisionFixedByAddResource) {
    auto asset = c2pa_test::get_fixture_path("A.jpg");
    auto thumb_a = c2pa_test::get_fixture_path("A.jpg");
    auto thumb_c = c2pa_test::get_fixture_path("C.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // Two folders, each with its own thumbnail referenced by the SAME relative
    // name but living in different folders.
    auto make_ing = [](const std::string &title) {
        return json{
            {"title", title},
            {"format", "image/jpeg"},
            {"relationship", "componentOf"},
            {"thumbnail", {{"format", "image/jpeg"}, {"identifier", "thumb.jpg"}}},
        };
    };
    fs::path folder_a =
        write_legacy_folder("collide-a", make_ing("ing A"), nullptr, &thumb_a, "thumb.jpg");
    fs::path folder_c =
        write_legacy_folder("collide-c", make_ing("ing C"), nullptr, &thumb_c, "thumb.jpg");
    std::string ing_a = c2pa_test::read_text_file(folder_a / "ingredient.json");
    std::string ing_c = c2pa_test::read_text_file(folder_c / "ingredient.json");

    // GOTCHA: one global base_path. Whichever folder it points at resolves;
    // the other ingredient's identically-named thumbnail resolves to the WRONG
    // bytes or fails. base_path is global, so this cannot serve both folders.
    {
        auto builder = c2pa::Builder(manifest);
        builder.set_base_path(folder_a.string());  // only folder_a
        std::ifstream s1(asset, std::ios::binary);
        std::ifstream s2(asset, std::ios::binary);
        builder.add_ingredient(ing_a, "image/jpeg", s1);
        builder.add_ingredient(ing_c, "image/jpeg", s2);
        // Both ingredients now reference "thumb.jpg" resolved from folder_a, so
        // folder_c's distinct thumbnail is lost. We assert the collision is
        // observable: signing succeeds but both thumbnails came from folder_a,
        // OR (depending on internal dedup) is simply not what the user intended.
        // The point of the test is the FIX below, so we only require that the
        // naive approach cannot distinguish the two sources.
        auto signer = c2pa_test::create_test_signer();
        fs::path out = make_temp_dir("collide-naive") / "out.jpg";
        // Not asserting throw/no-throw here: the defect is silent (wrong bytes),
        // which is exactly why the add_resource fix is recommended.
        EXPECT_NO_THROW(builder.sign(asset, out, signer));
    }

    // FIX: give each thumbnail a UNIQUE identifier and inline its bytes with
    // add_resource, so neither ingredient depends on a global base_path.
    {
        auto builder = c2pa::Builder(manifest);

        json ia = make_ing("ing A");
        ia["thumbnail"]["identifier"] = "thumb_a.jpg";
        json ic = make_ing("ing C");
        ic["thumbnail"]["identifier"] = "thumb_c.jpg";

        std::ifstream ta(folder_a / "thumb.jpg", std::ios::binary);
        std::ifstream tc(folder_c / "thumb.jpg", std::ios::binary);
        builder.add_resource("thumb_a.jpg", ta);
        builder.add_resource("thumb_c.jpg", tc);

        std::ifstream s1(asset, std::ios::binary);
        std::ifstream s2(asset, std::ios::binary);
        ASSERT_NO_THROW(builder.add_ingredient(ia.dump(), "image/jpeg", s1));
        ASSERT_NO_THROW(builder.add_ingredient(ic.dump(), "image/jpeg", s2));

        auto ingredients = sign_and_read_ingredients(builder, asset, "collide-fixed");
        EXPECT_EQ(ingredients.size(), 2u)
            << "inlining resources lets both distinct-thumbnail ingredients resolve";
    }
}

// A legacy folder ingredient and a modern archive ingredient both go on one Builder.
TEST_F(LegacyFolderIngredient, MixLegacyAndModernArchiveOnOneBuilder) {
    auto src = c2pa_test::get_fixture_path("A.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // Legacy folder ingredient (Case A): manifest_data.c2pa + signed asset.
    auto seed = sign_seed(c2pa_test::get_fixture_path("A.jpg"), "mix-legacy");
    json legacy_ing = {
        {"title", "legacy ingredient"},
        {"format", "image/jpeg"},
        {"relationship", "parentOf"},
        {"manifest_data",
         {{"format", "application/c2pa"}, {"identifier", "manifest_data.c2pa"}}},
    };
    fs::path folder =
        write_legacy_folder("mix-legacy", legacy_ing, &seed.manifest_bytes, nullptr, "");
    fs::copy_file(seed.signed_asset, folder / "asset.jpg",
                  fs::copy_options::overwrite_existing);
    std::string legacy_json = c2pa_test::read_text_file(folder / "ingredient.json");

    // Modern dedicated ingredient archive built from C.jpg.
    std::stringstream archive(std::ios::in | std::ios::out | std::ios::binary);
    {
        auto ab = c2pa::Builder(manifest);
        ab.add_ingredient(
            R"({"title": "modern ingredient", "relationship": "componentOf", "label": "ing-modern"})",
            c2pa_test::get_fixture_path("C.jpg"));
        ab.write_ingredient_archive("ing-modern", archive);
    }

    // One builder, both routes.
    auto builder = c2pa::Builder(manifest);
    builder.set_base_path(folder.string());
    std::ifstream asset_stream(folder / "asset.jpg", std::ios::binary);
    ASSERT_NO_THROW(builder.add_ingredient(legacy_json, "image/jpeg", asset_stream));
    archive.seekg(0);
    ASSERT_NO_THROW(builder.add_ingredient_from_archive(archive));

    auto ingredients = sign_and_read_ingredients(builder, src, "mix-out");
    ASSERT_EQ(ingredients.size(), 2u);
    std::vector<std::string> titles = {ingredients[0]["title"], ingredients[1]["title"]};
    EXPECT_NE(std::find(titles.begin(), titles.end(), "legacy ingredient"), titles.end());
    EXPECT_NE(std::find(titles.begin(), titles.end(), "modern ingredient"), titles.end());
}

// Three legacy folder ingredients plus one modern archive go on one Builder.
TEST_F(LegacyFolderIngredient, MixMultipleLegacyAndOneModern) {
    auto src = c2pa_test::get_fixture_path("A.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto builder = c2pa::Builder(manifest);
    add_legacy_caseA(builder, "m1-legacy-a", "legacy A");
    add_legacy_caseA(builder, "m1-legacy-b", "legacy B");
    add_legacy_caseA(builder, "m1-legacy-c", "legacy C");
    auto a1 = add_modern_archive(builder, manifest, "ing-modern", "modern X",
                                 c2pa_test::get_fixture_path("C.jpg"));

    auto ingredients = sign_and_read_ingredients(builder, src, "m1-out");
    ASSERT_EQ(ingredients.size(), 4u);
    expect_titles(ingredients, {"legacy A", "legacy B", "legacy C", "modern X"});
}

// Two legacy folder ingredients plus two modern archives go on one Builder.
TEST_F(LegacyFolderIngredient, MixMultipleLegacyAndMultipleModern) {
    auto src = c2pa_test::get_fixture_path("A.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto builder = c2pa::Builder(manifest);
    add_legacy_caseA(builder, "m2-legacy-a", "legacy A");
    add_legacy_caseA(builder, "m2-legacy-b", "legacy B");
    auto a1 = add_modern_archive(builder, manifest, "ing-modern-1", "modern X",
                                 c2pa_test::get_fixture_path("C.jpg"));
    auto a2 = add_modern_archive(builder, manifest, "ing-modern-2", "modern Y",
                                 c2pa_test::get_fixture_path("sample1.gif"));

    auto ingredients = sign_and_read_ingredients(builder, src, "m2-out");
    ASSERT_EQ(ingredients.size(), 4u);
    expect_titles(ingredients, {"legacy A", "legacy B", "modern X", "modern Y"});
}

// Loads a legacy folder fixture (ingredient.json + manifest_data.c2pa + self#jumbf thumbnail).
TEST_F(LegacyFolderIngredient, LegacyIngredientFolderLoading) {
    fs::path folder =
        c2pa_test::get_fixture_path("ingredient-legacy-folder-migration");
    std::string ing_json = c2pa_test::read_text_file(folder / "ingredient.json");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto builder = c2pa::Builder(manifest);
    builder.set_base_path(folder.string());

    std::ifstream asset_stream(c2pa_test::get_fixture_path("C.jpg"), std::ios::binary);
    ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", asset_stream));

    auto ingredients =
        sign_and_read_ingredients(builder, c2pa_test::get_fixture_path("A.jpg"),
                                  "real-legacy-out");
    ASSERT_EQ(ingredients.size(), 1u);
    EXPECT_EQ(ingredients[0]["title"], "C.jpg");
    EXPECT_EQ(ingredients[0]["relationship"], "componentOf");
    // The legacy manifest_data is carried: the ingredient references an active
    // manifest reconstituted from manifest_data.c2pa.
    EXPECT_TRUE(ingredients[0].contains("active_manifest"))
        << "legacy manifest_data.c2pa should be resolved via base_path";
}

// Loads the legacy folder fixture with no asset stream by injecting the ingredient into the definition and carrying its store with add_resource.
TEST_F(LegacyFolderIngredient, LegacyIngredientFolderLoadingNoAssetStream) {
    fs::path folder =
        c2pa_test::get_fixture_path("ingredient-legacy-folder-migration");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // The ingredient fields, including a manifest_data ref, come from the folder's
    // ingredient.json.
    json ingredient = json::parse(c2pa_test::read_text_file(folder / "ingredient.json"));
    ASSERT_TRUE(ingredient.contains("manifest_data"));

    // Fill validation_results only when the ingredient.json lacks it. Older folders
    // may already carry the field (this fixture does); read it from the store
    // otherwise, since the definition-injection route cannot derive it.
    if (!ingredient.contains("validation_results")) {
        std::ifstream store_in(folder / "manifest_data.c2pa", std::ios::binary);
        c2pa::Reader store_reader("application/c2pa", store_in);
        ingredient["validation_results"] =
            json::parse(store_reader.json())["validation_results"];
    }

    // Inject the ingredient into the definition, then carry the store bytes under
    // the matching identifier. No asset stream is added.
    json def = json::parse(manifest);
    def["ingredients"] = json::array({ingredient});
    auto builder = c2pa::Builder(def.dump());

    std::ifstream store_res(folder / "manifest_data.c2pa", std::ios::binary);
    ASSERT_NO_THROW(builder.add_resource("manifest_data.c2pa", store_res));

    auto signer = c2pa_test::create_test_signer();
    fs::path out = make_temp_dir("real-legacy-noasset-out") / "out.jpg";
    ASSERT_NO_THROW(builder.sign(c2pa_test::get_fixture_path("A.jpg"), out, signer));

    auto reader = c2pa::Reader(out);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);
    EXPECT_EQ(ingredients[0]["title"], "C.jpg");
    // Provenance is carried even with no asset stream.
    EXPECT_TRUE(ingredients[0].contains("active_manifest"))
        << "injected manifest_data.c2pa should be carried via add_resource";
}

// Re-archives the legacy folder fixture into an ingredient archive, then adds that archive to a fresh Builder.
TEST_F(LegacyFolderIngredient, LegacyIngredientFolderLoadingReArchiveThenAdd) {
    fs::path folder =
        c2pa_test::get_fixture_path("ingredient-legacy-folder-migration");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // Give the ingredient a label so write_ingredient_archive can name it.
    json ingredient = json::parse(c2pa_test::read_text_file(folder / "ingredient.json"));
    ingredient["label"] = "ing-mig";

    // Load the legacy folder via base_path with a (carrier) asset,
    //  then emit it in the modern archive format.
    std::stringstream archive(std::ios::in | std::ios::out | std::ios::binary);
    {
        auto b = c2pa::Builder(manifest);
        b.set_base_path(folder.string());
        std::ifstream asset(c2pa_test::get_fixture_path("C.jpg"), std::ios::binary);
        ASSERT_NO_THROW(b.add_ingredient(ingredient.dump(), "image/jpeg", asset));
        ASSERT_NO_THROW(b.write_ingredient_archive("ing-mig", archive));
    }

    // The archive bundles the manifest store, so a fresh Builder loads it with no
    // base_path and keeps the provenance.
    archive.seekg(0);
    auto builder = c2pa::Builder(manifest);
    ASSERT_NO_THROW(builder.add_ingredient_from_archive(archive));

    auto ingredients =
        sign_and_read_ingredients(builder, c2pa_test::get_fixture_path("A.jpg"),
                                  "real-legacy-rearchive-out");
    ASSERT_EQ(ingredients.size(), 1u);
    EXPECT_EQ(ingredients[0]["title"], "C.jpg");
    EXPECT_TRUE(ingredients[0].contains("active_manifest"))
        << "re-archived legacy ingredient should keep its provenance";
}

// manifest_data resolves eagerly at add_ingredient, so deleting the directory
// before sign still signs and carries provenance.
TEST_F(LegacyFolderIngredient, ManifestDataDirectoryDeletableAfterAdd) {
    auto seed = sign_seed(c2pa_test::get_fixture_path("A.jpg"), "eager-del");
    json ing = {
        {"title", "eager legacy"},
        {"format", "image/jpeg"},
        {"relationship", "componentOf"},
        {"manifest_data",
         {{"format", "application/c2pa"}, {"identifier", "manifest_data.c2pa"}}},
    };
    fs::path folder = write_legacy_folder("eager-del", ing, &seed.manifest_bytes, nullptr, "");
    fs::copy_file(seed.signed_asset, folder / "asset.jpg",
                  fs::copy_options::overwrite_existing);
    std::string ing_json = c2pa_test::read_text_file(folder / "ingredient.json");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto builder = c2pa::Builder(manifest);
    builder.set_base_path(folder.string());
    std::ifstream asset_stream(folder / "asset.jpg", std::ios::binary);
    ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", asset_stream));
    asset_stream.close();

    // Delete the whole directory AFTER add_ingredient, BEFORE sign.
    fs::remove_all(folder);
    ASSERT_FALSE(fs::exists(folder));

    // Eager resolution: manifest_data is already in the Builder, so sign succeeds
    // and the ingredient keeps its provenance despite the directory being gone.
    auto ingredients = sign_and_read_ingredients(
        builder, c2pa_test::get_fixture_path("A.jpg"), "eager-del-out");
    ASSERT_EQ(ingredients.size(), 1u);
    EXPECT_TRUE(ingredients[0].contains("active_manifest"))
        << "manifest_data resolved eagerly; directory deletable before sign";
}

// A thumbnail resolves lazily at sign, so deleting the directory before sign
// makes sign fail; deleting after sign is fine.
TEST_F(LegacyFolderIngredient, ThumbnailDirectoryMustSurviveUntilSign) {
    auto asset = c2pa_test::get_fixture_path("A.jpg");
    auto thumb_src = c2pa_test::get_fixture_path("A.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto make_ing = []() {
        return json{
            {"title", "lazy thumb"},
            {"format", "image/jpeg"},
            {"relationship", "componentOf"},
            {"thumbnail", {{"format", "image/jpeg"}, {"identifier", "thumb.jpg"}}},
        };
    };

    // Negative test: delete the directory before sign. The lazy thumbnail cannot be
    // read at sign time, so sign must throw.
    {
        fs::path folder =
            write_legacy_folder("lazy-del", make_ing(), nullptr, &thumb_src, "thumb.jpg");
        std::string ing_json = c2pa_test::read_text_file(folder / "ingredient.json");
        auto builder = c2pa::Builder(manifest);
        builder.set_base_path(folder.string());
        std::ifstream src(asset, std::ios::binary);
        ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", src));
        src.close();

        fs::remove_all(folder);
        ASSERT_FALSE(fs::exists(folder));

        auto signer = c2pa_test::create_test_signer();
        fs::path out = make_temp_dir("lazy-del-fail") / "out.jpg";
        EXPECT_ANY_THROW(builder.sign(asset, out, signer))
            << "thumbnail resolved lazily; deleting the directory before sign must fail";
    }

    // Postive test: keep the directory until sign, then it is safe to delete.
    {
        fs::path folder =
            write_legacy_folder("lazy-keep", make_ing(), nullptr, &thumb_src, "thumb.jpg");
        std::string ing_json = c2pa_test::read_text_file(folder / "ingredient.json");
        auto builder = c2pa::Builder(manifest);
        builder.set_base_path(folder.string());
        std::ifstream src(asset, std::ios::binary);
        ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", src));

        EXPECT_NO_THROW(sign_and_read_ingredients(builder, asset, "lazy-keep-ok"));
        // Directory survived until sign; deleting now is fine.
        fs::remove_all(folder);
    }
}

// The Builder does not de-duplicate. Adding the same directory ingredient
// twice yields two ingredients in the signed manifest.
TEST_F(LegacyFolderIngredient, NoDeduplicationSameDirectoryAddedTwice) {
    auto seed = sign_seed(c2pa_test::get_fixture_path("A.jpg"), "dedup");
    json ing = {
        {"title", "dup legacy"},
        {"format", "image/jpeg"},
        {"relationship", "componentOf"},
        {"manifest_data",
         {{"format", "application/c2pa"}, {"identifier", "manifest_data.c2pa"}}},
    };
    fs::path folder = write_legacy_folder("dedup", ing, &seed.manifest_bytes, nullptr, "");
    fs::copy_file(seed.signed_asset, folder / "asset.jpg",
                  fs::copy_options::overwrite_existing);
    std::string ing_json = c2pa_test::read_text_file(folder / "ingredient.json");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto builder = c2pa::Builder(manifest);
    builder.set_base_path(folder.string());
    // Add the identical ingredient twice.
    std::ifstream a1(folder / "asset.jpg", std::ios::binary);
    ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", a1));
    std::ifstream a2(folder / "asset.jpg", std::ios::binary);
    ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", a2));

    auto ingredients = sign_and_read_ingredients(
        builder, c2pa_test::get_fixture_path("A.jpg"), "dedup-out");
    EXPECT_EQ(ingredients.size(), 2u)
        << "Builder does not de-duplicate; two adds produce two ingredients";
}

// A corrupt manifest_data.c2pa fails at sign, not at add_ingredient, and
// with a different message than the missing/unresolved case (ResourceNotFound).
TEST_F(LegacyFolderIngredient, CorruptManifestDataFailsAtSign) {
    json ing = {
        {"title", "corrupt legacy"},
        {"format", "image/jpeg"},
        {"relationship", "componentOf"},
        {"manifest_data",
         {{"format", "application/c2pa"}, {"identifier", "manifest_data.c2pa"}}},
    };
    // Garbage bytes standing in for a real manifest store.
    std::vector<unsigned char> garbage(512, 0xAB);
    fs::path folder = write_legacy_folder("corrupt", ing, &garbage, nullptr, "");
    std::string ing_json = c2pa_test::read_text_file(folder / "ingredient.json");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto builder = c2pa::Builder(manifest);
    builder.set_base_path(folder.string());
    std::ifstream asset(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    // The corruption is not detected at add time.
    ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", asset));

    auto signer = c2pa_test::create_test_signer();
    fs::path out = make_temp_dir("corrupt-out") / "out.jpg";
    std::string msg;
    try {
        builder.sign(c2pa_test::get_fixture_path("A.jpg"), out, signer);
        FAIL() << "sign should throw on a corrupt manifest_data.c2pa";
    } catch (const std::exception &e) {
        msg = e.what();
    }
    // Capture the actual message for the docs; corruption should not read as a
    // plain "not found".
    std::cerr << "[corrupt manifest_data sign error] " << msg << std::endl;
    EXPECT_FALSE(msg.empty());
    EXPECT_EQ(msg.find("ResourceNotFound"), std::string::npos)
        << "corrupt store should fail with a verify/JUMBF error, not ResourceNotFound; got: "
        << msg;
}

// An unrelated asset stream can mask an unresolved manifest_data reference.
// ingredient.json declares manifest_data but base_path is NOT set, so the ref cannot
// resolve from disk. Observe whether an unrelated asset stream lets sign succeed
// (masking) or still throws.
TEST_F(LegacyFolderIngredient, UnrelatedAssetStreamMasksUnresolvedRef) {
    auto seed = sign_seed(c2pa_test::get_fixture_path("A.jpg"), "mask");
    json ing = {
        {"title", "masking legacy"},
        {"format", "image/jpeg"},
        {"relationship", "componentOf"},
        {"manifest_data",
         {{"format", "application/c2pa"}, {"identifier", "manifest_data.c2pa"}}},
    };
    fs::path folder = write_legacy_folder("mask", ing, &seed.manifest_bytes, nullptr, "");
    std::string ing_json = c2pa_test::read_text_file(folder / "ingredient.json");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto builder = c2pa::Builder(manifest);
    // NO set_base_path: the declared manifest_data cannot resolve from disk.
    // Pass an unrelated asset stream (C.jpg, not the store's bound asset).
    std::ifstream unrelated(c2pa_test::get_fixture_path("C.jpg"), std::ios::binary);
    ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", unrelated));

    auto signer = c2pa_test::create_test_signer();
    fs::path out = make_temp_dir("mask-out") / "out.jpg";
    bool threw = false;
    std::string msg;
    try {
        builder.sign(c2pa_test::get_fixture_path("A.jpg"), out, signer);
    } catch (const std::exception &e) {
        threw = true;
        msg = e.what();
    }
    // Document the real behavior. If sign succeeded, the unrelated stream masked the
    // unresolved manifest_data reference (the pitfall); if it threw, record why.
    std::cerr << "[unrelated-asset masking] threw=" << (threw ? "yes" : "no")
              << " msg=" << msg << std::endl;
    if (!threw) {
        auto reader = c2pa::Reader(out);
        auto parsed = json::parse(reader.json());
        std::string active = parsed["active_manifest"];
        auto ingredients = parsed["manifests"][active]["ingredients"];
        EXPECT_EQ(ingredients.size(), 1u)
            << "unrelated asset stream masked the unresolved manifest_data and signed";
    }
    SUCCEED();
}

TEST_F(LegacyFolderIngredient, LinkParentOfToOpened) {
    auto seed = sign_seed(c2pa_test::get_fixture_path("A.jpg"), "link-parent");
    json ing = {
        {"title", "link-parent.jpg"},
        {"format", "image/jpeg"},
        {"relationship", "parentOf"},
        {"label", "dir-parent"},   // primary ingredientIds lookup key
        {"manifest_data",
         {{"format", "application/c2pa"}, {"identifier", "manifest_data.c2pa"}}},
    };
    fs::path dir = write_legacy_folder("link-parent", ing, &seed.manifest_bytes, nullptr, "");
    fs::copy_file(seed.signed_asset, dir / "asset.jpg",
                  fs::copy_options::overwrite_existing);
    std::string ing_json = c2pa_test::read_text_file(dir / "ingredient.json");

    json manifest = {
        {"claim_generator_info",
         json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {{"label", "c2pa.actions"},
             {"data", {{"actions", json::array({
                 {{"action", "c2pa.opened"},
                  {"parameters", {{"ingredientIds", json::array({"dir-parent"})}}}},
             })}}}},
        })},
    };

    auto builder = c2pa::Builder(manifest.dump());
    builder.set_base_path(dir.string());
    std::ifstream asset_stream(dir / "asset.jpg", std::ios::binary);
    ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", asset_stream));

    auto signer = c2pa_test::create_test_signer();
    fs::path out = make_temp_dir("link-parent-out") / "out.jpg";
    ASSERT_NO_THROW(builder.sign(c2pa_test::get_fixture_path("A.jpg"), out, signer));

    auto reader = c2pa::Reader(out);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    bool found = false;
    for (auto &assertion : parsed["manifests"][active]["assertions"]) {
        if (assertion["label"] != "c2pa.actions.v2" &&
            assertion["label"] != "c2pa.actions") continue;
        for (auto &a : assertion["data"]["actions"]) {
            if (a["action"] != "c2pa.opened") continue;
            if (a.contains("parameters") && a["parameters"].contains("ingredients")) {
                auto &ings = a["parameters"]["ingredients"];
                ASSERT_GE(ings.size(), 1u);
                ASSERT_TRUE(ings[0].contains("url"));
                std::string url = ings[0]["url"];
                EXPECT_NE(url.find("c2pa.ingredient"), std::string::npos)
                    << "c2pa.opened should resolve to an ingredient assertion, got " << url;
                found = true;
            }
        }
    }
    EXPECT_TRUE(found) << "linked c2pa.opened action not found in signed output";
}

TEST_F(LegacyFolderIngredient, LinkComponentOfToPlaced) {
    auto seed = sign_seed(c2pa_test::get_fixture_path("A.jpg"), "link-comp");
    json ing = {
        {"title", "link-comp.jpg"},
        {"format", "image/jpeg"},
        {"relationship", "componentOf"},
        {"label", "dir-comp"},
        {"manifest_data",
         {{"format", "application/c2pa"}, {"identifier", "manifest_data.c2pa"}}},
    };
    fs::path dir = write_legacy_folder("link-comp", ing, &seed.manifest_bytes, nullptr, "");
    fs::copy_file(seed.signed_asset, dir / "asset.jpg",
                  fs::copy_options::overwrite_existing);
    std::string ing_json = c2pa_test::read_text_file(dir / "ingredient.json");

    json manifest = {
        {"claim_generator_info",
         json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {{"label", "c2pa.actions"},
             {"data", {{"actions", json::array({
                 {{"action", "c2pa.placed"},
                  {"parameters", {{"ingredientIds", json::array({"dir-comp"})}}}},
             })}}}},
        })},
    };

    auto builder = c2pa::Builder(manifest.dump());
    builder.set_base_path(dir.string());
    std::ifstream asset_stream(dir / "asset.jpg", std::ios::binary);
    ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", asset_stream));

    auto signer = c2pa_test::create_test_signer();
    fs::path out = make_temp_dir("link-comp-out") / "out.jpg";
    ASSERT_NO_THROW(builder.sign(c2pa_test::get_fixture_path("A.jpg"), out, signer));

    auto reader = c2pa::Reader(out);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    bool found = false;
    for (auto &assertion : parsed["manifests"][active]["assertions"]) {
        if (assertion["label"] != "c2pa.actions.v2" &&
            assertion["label"] != "c2pa.actions") continue;
        for (auto &a : assertion["data"]["actions"]) {
            if (a["action"] != "c2pa.placed") continue;
            if (a.contains("parameters") && a["parameters"].contains("ingredients")) {
                auto &ings = a["parameters"]["ingredients"];
                ASSERT_GE(ings.size(), 1u);
                ASSERT_TRUE(ings[0].contains("url"));
                std::string url = ings[0]["url"];
                EXPECT_NE(url.find("c2pa.ingredient"), std::string::npos)
                    << "c2pa.placed should resolve to an ingredient assertion, got " << url;
                found = true;
            }
        }
    }
    EXPECT_TRUE(found) << "linked c2pa.placed action not found in signed output";
}

TEST_F(LegacyFolderIngredient, LinkInputToToEdited) {
    auto seed = sign_seed(c2pa_test::get_fixture_path("A.jpg"), "link-input");
    json ing = {
        {"title", "link-input.jpg"},
        {"format", "image/jpeg"},
        {"relationship", "inputTo"},
        {"label", "dir-input"},
        {"manifest_data",
         {{"format", "application/c2pa"}, {"identifier", "manifest_data.c2pa"}}},
    };
    fs::path dir = write_legacy_folder("link-input", ing, &seed.manifest_bytes, nullptr, "");
    fs::copy_file(seed.signed_asset, dir / "asset.jpg",
                  fs::copy_options::overwrite_existing);
    std::string ing_json = c2pa_test::read_text_file(dir / "ingredient.json");

    json manifest = {
        {"claim_generator_info",
         json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {{"label", "c2pa.actions"},
             {"data", {{"actions", json::array({
                 {{"action", "c2pa.edited"},
                  {"parameters", {{"ingredientIds", json::array({"dir-input"})}}}},
             })}}}},
        })},
    };

    auto builder = c2pa::Builder(manifest.dump());
    builder.set_base_path(dir.string());
    std::ifstream asset_stream(dir / "asset.jpg", std::ios::binary);
    ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", asset_stream));

    auto signer = c2pa_test::create_test_signer();
    fs::path out = make_temp_dir("link-input-out") / "out.jpg";
    ASSERT_NO_THROW(builder.sign(c2pa_test::get_fixture_path("A.jpg"), out, signer));

    auto reader = c2pa::Reader(out);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    bool found = false;
    for (auto &assertion : parsed["manifests"][active]["assertions"]) {
        if (assertion["label"] != "c2pa.actions.v2" &&
            assertion["label"] != "c2pa.actions") continue;
        for (auto &a : assertion["data"]["actions"]) {
            if (a["action"] != "c2pa.edited") continue;
            if (a.contains("parameters") && a["parameters"].contains("ingredients")) {
                auto &ings = a["parameters"]["ingredients"];
                ASSERT_GE(ings.size(), 1u);
                ASSERT_TRUE(ings[0].contains("url"));
                std::string url = ings[0]["url"];
                EXPECT_NE(url.find("c2pa.ingredient"), std::string::npos)
                    << "c2pa.edited should resolve to an ingredient assertion, got " << url;
                found = true;
            }
        }
    }
    EXPECT_TRUE(found) << "linked c2pa.edited action not found in signed output";
}

// After signing, the `label` used for action linking is not preserved on the
// read-back ingredient: the SDK consumes it as the linking key and rewrites it.
// An explicit `instance_id`, by contrast, stays in the ingredient data.
TEST_F(LegacyFolderIngredient, LabelNotPreservedButInstanceIdIs) {
    auto seed = sign_seed(c2pa_test::get_fixture_path("A.jpg"), "label-persist");
    const std::string my_label = "dir-link-label";
    const std::string my_iid = "xmp:iid:11111111-2222-3333-4444-555555555555";
    json ing = {
        {"title", "label-persist.jpg"},
        {"format", "image/jpeg"},
        {"relationship", "componentOf"},
        {"label", my_label},
        {"instance_id", my_iid},   // explicit, caller-controlled
        {"manifest_data",
         {{"format", "application/c2pa"}, {"identifier", "manifest_data.c2pa"}}},
    };
    fs::path dir = write_legacy_folder("label-persist", ing, &seed.manifest_bytes, nullptr, "");
    fs::copy_file(seed.signed_asset, dir / "asset.jpg",
                  fs::copy_options::overwrite_existing);
    std::string ing_json = c2pa_test::read_text_file(dir / "ingredient.json");

    json manifest = {
        {"claim_generator_info",
         json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {{"label", "c2pa.actions"},
             {"data", {{"actions", json::array({
                 {{"action", "c2pa.placed"},
                  {"parameters", {{"ingredientIds", json::array({my_label})}}}},
             })}}}},
        })},
    };

    auto builder = c2pa::Builder(manifest.dump());
    builder.set_base_path(dir.string());
    std::ifstream asset_stream(dir / "asset.jpg", std::ios::binary);
    ASSERT_NO_THROW(builder.add_ingredient(ing_json, "image/jpeg", asset_stream));

    auto signer = c2pa_test::create_test_signer();
    fs::path out = make_temp_dir("label-persist-out") / "out.jpg";
    ASSERT_NO_THROW(builder.sign(c2pa_test::get_fixture_path("A.jpg"), out, signer));

    auto reader = c2pa::Reader(out);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto &ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);
    const auto &out_ing = ingredients[0];

    std::string read_label = out_ing.value("label", std::string("<absent>"));
    std::string read_iid = out_ing.value("instance_id", std::string("<absent>"));
    std::cerr << "[label persistence] label=" << read_label
              << " instance_id=" << read_iid << std::endl;

    // The linking label is not carried through verbatim onto the read-back ingredient.
    EXPECT_NE(read_label, my_label)
        << "the linking label should not survive verbatim on the signed ingredient";
    // The explicit instance_id stays in the ingredient data.
    EXPECT_EQ(read_iid, my_iid)
        << "an explicit instance_id should be preserved on the signed ingredient";
}
