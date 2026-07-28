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

#include <c2pa.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <filesystem>
#include <thread>

#include "include/test_utils.hpp"

using namespace std;
namespace fs = std::filesystem;
using nlohmann::json;

// Test fixture for builder tests with cleanup
class BuilderTest : public ::testing::Test {
protected:
    std::vector<fs::path> temp_files;
    std::vector<fs::path> temp_dirs;
    bool cleanup_temp_files = true;  // Set to false to keep temp files for debugging

    // Get path for temp builder test files in build directory.
    fs::path get_temp_path(const fs::path& name) {
        fs::path current_dir = fs::path(__FILE__).parent_path();
        fs::path build_dir = current_dir.parent_path() / "build";
        if (!fs::exists(build_dir)) {
            fs::create_directories(build_dir);
        }
        fs::path temp_path = build_dir / (fs::path("builder-") += name);
        temp_files.push_back(temp_path);
        return temp_path;
    }

    // Get path for temp builder test directories in build directory
    fs::path get_temp_dir(const std::string& name) {
        fs::path current_dir = fs::path(__FILE__).parent_path();
        fs::path build_dir = current_dir.parent_path() / "build";
        if (!fs::exists(build_dir)) {
            fs::create_directories(build_dir);
        }
        fs::path temp_path = build_dir / ("builder-" + name);
        // Remove and recreate the temp dir
        if (fs::exists(temp_path)) {
            fs::remove_all(temp_path);
        }
        fs::create_directories(temp_path);
        temp_dirs.push_back(temp_path);
        return temp_path;
    }

    void TearDown() override {
        if (cleanup_temp_files) {
            for (const auto& path : temp_files) {
                if (fs::exists(path)) {
                    fs::remove(path);
                }
            }
            for (const auto& dir : temp_dirs) {
                if (fs::exists(dir)) {
                    fs::remove_all(dir);
                }
            }
        }
        temp_files.clear();
        temp_dirs.clear();
    }

    // Helper: Creates a Builder with a test manifest.
    c2pa::Builder make_builder() {
        auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
        return c2pa::Builder(manifest);
    }

    // Helper: Creates an ingredient archive (.c2pa) from a single ingredient.
    void create_ingredient_archive(
        const fs::path& archive_path,
        const std::string& archive_ingredient_json)
    {
        auto context = c2pa::Context();
        auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
        auto builder = c2pa::Builder(context, manifest_str);
        builder.add_ingredient(archive_ingredient_json, c2pa_test::get_fixture_path("A.jpg"));
        builder.to_archive(archive_path);
    }

    // Helper: Builds a manifest JSON with one action that references one ingredient for linking,
    // with valid c2pa.created first action, used to prefix inline signing manifests.
    json created_first_action()
    {
        return {
            {"action", "c2pa.created"},
            {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"}
        };
    }

    json make_manifest_with_action(const std::string& action_name,
                                   const std::string& linking_key,
                                   const std::string& digital_source_type = "")
    {
        json action_obj = {{"action", action_name}};
        if (!digital_source_type.empty()) {
            action_obj["digitalSourceType"] = digital_source_type;
        }
        action_obj["parameters"] = {{"ingredientIds", json::array({linking_key})}};

        // A signed manifest must start with a c2pa.created or c2pa.opened action
        json actions = json::array();
        if (action_name != "c2pa.created" && action_name != "c2pa.opened") {
            actions.push_back({
                {"action", "c2pa.created"},
                {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"}
            });
        }
        actions.push_back(action_obj);

        return {
            {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
            {"assertions", json::array({
                {
                    {"label", "c2pa.actions.v2"},
                    {"data", {{"actions", actions}}}
                }
            })}
        };
    }

    // Helper: Verifies an ingredient linked to an action.
    bool verify_ingredient_linked(
        c2pa::Builder& builder,
        const fs::path& output_path,
        c2pa::Signer& signer,
        const std::string& expected_action,
        const std::string& expected_ingredient_label = "c2pa.ingredient.v3")
    {
        auto source_path = c2pa_test::get_fixture_path("A.jpg");
        builder.sign(source_path, output_path, signer);

        auto context = c2pa::Context();
        auto reader = c2pa::Reader(context, output_path);
        auto parsed = json::parse(reader.json());
        std::string active = parsed["active_manifest"];
        auto& manifest = parsed["manifests"][active];

        for (auto& assertion : manifest["assertions"]) {
            if (assertion["label"] != "c2pa.actions.v2") continue;
            for (auto& action : assertion["data"]["actions"]) {
                if (action["action"] != expected_action) continue;
                if (!action.contains("parameters")) return false;
                auto& params = action["parameters"];
                if (!params.contains("ingredients")) return false;
                auto& ingredients = params["ingredients"];
                if (!ingredients.is_array() || ingredients.empty()) return false;
                std::string url = ingredients[0]["url"];
                std::string expected_url = "self#jumbf=c2pa.assertions/" + expected_ingredient_label;
                return url == expected_url;
            }
        }
        return false;
    }
};

TEST_F(BuilderTest, supported_mime_types_returns_types) {
  auto supported_types = c2pa::Builder::supported_mime_types();
  auto begin = supported_types.begin();
  auto end = supported_types.end();
  EXPECT_TRUE(std::find(begin, end, "image/jpeg") != end);
  EXPECT_TRUE(std::find(begin, end, "application/c2pa") != end);
}

TEST_F(BuilderTest, exposes_raw_pointer) {
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    auto manifest = c2pa_test::read_text_file(manifest_path);
    c2pa::Builder builder(manifest);
    ASSERT_NE(builder.c2pa_builder(), nullptr);
}

TEST_F(BuilderTest, wraps_c_ffi_builder) {
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    auto manifest = c2pa_test::read_text_file(manifest_path);

    C2paBuilder* ffi_builder = c2pa_builder_from_json(manifest.c_str());
    ASSERT_NE(ffi_builder, nullptr);

    auto builder = c2pa::Builder(ffi_builder);
    EXPECT_EQ(builder.c2pa_builder(), ffi_builder);
}

TEST_F(BuilderTest, wraps_null_c_ffi_builder_throws) {
    EXPECT_THROW(c2pa::Builder(nullptr), c2pa::C2paException);
}

// Test fixture for basic signature validations with automatic cleanup
class BuilderSmokeSignTest : public BuilderTest, public ::testing::WithParamInterface<std::string> {
public:
  static std::string get_mime_type_from_extension(const std::string& filename) {
    std::string ext = fs::path(filename).extension().string();
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".dng") return "image/dng";
    if (ext == ".mp4") return "video/mp4";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".wav") return "audio/wav";
    return "application/octet-stream";
  }
};

INSTANTIATE_TEST_SUITE_P(
    BuilderSignCallToVerifyMiscFileTypes,
    BuilderSmokeSignTest,
    ::testing::Values(
        "A.jpg",
        "C.jpg",
        "C.dng",
        "C_with_CAWG_data.jpg",
        "sample1.gif",
        "sample1.mp3",
        "sample1.wav",
        "sample1.webp",
        "sample2.svg",
        "video1.mp4"
        )
);

TEST_P(BuilderSmokeSignTest, SignsFileTypes) {
  fs::path current_dir = fs::path(__FILE__).parent_path();

  // Construct the paths relative to the current directory
  fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
  fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
  fs::path asset_path = current_dir / "../tests/fixtures" / GetParam();

  // Use temp path with automatic cleanup
  fs::path output_path = get_temp_path(GetParam());

  auto manifest = c2pa_test::read_text_file(manifest_path);
  auto certs = c2pa_test::read_text_file(certs_path);
  auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

  // create a signer
  auto signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");
  auto builder = c2pa::Builder(manifest);

  std::vector<unsigned char> manifest_data;
  ASSERT_NO_THROW(manifest_data = builder.sign(asset_path, output_path, signer));
  ASSERT_FALSE(manifest_data.empty());

  ASSERT_TRUE(std::filesystem::exists(output_path));

  auto reader = c2pa::Reader(output_path);
  ASSERT_NO_THROW(reader.json());
}

TEST_P(BuilderSmokeSignTest, SignsStreamTypes) {
  fs::path current_dir = fs::path(__FILE__).parent_path();

  // Construct the paths relative to the current directory
  fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
  fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
  fs::path asset_path = current_dir / "../tests/fixtures" / GetParam();

  auto manifest = c2pa_test::read_text_file(manifest_path);
  auto certs = c2pa_test::read_text_file(certs_path);
  auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

  // Get mimetype from file extension
  std::string mime_type = get_mime_type_from_extension(GetParam());

  // Create a signer
  auto signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");
  auto builder = c2pa::Builder(manifest);

  // Open input stream
  std::ifstream source(asset_path, std::ios::binary);
  ASSERT_TRUE(source) << "Failed to open source file: " << asset_path;

  // Create output stream
  std::stringstream output_buffer(std::ios::in | std::ios::out | std::ios::binary);
  std::iostream& dest = output_buffer;

  // Sign
  std::vector<unsigned char> manifest_data;
  ASSERT_NO_THROW(manifest_data = builder.sign(mime_type, source, dest, signer));
  ASSERT_FALSE(manifest_data.empty());

  source.close();

  // Rewind output stream and verify with Reader
  dest.flush();
  dest.seekg(0, std::ios::beg);

  auto reader = c2pa::Reader(mime_type, dest);
  std::string json_result;
  ASSERT_NO_THROW(json_result = reader.json());
  ASSERT_FALSE(json_result.empty());
}

TEST(BuilderErrorHandling, EmptyManifestJsonReturnsError)
{
    EXPECT_THROW(c2pa::Builder(""), c2pa::C2paException);
}

TEST(BuilderErrorHandling, MalformedJsonManifestReturnsError)
{
    EXPECT_THROW(c2pa::Builder("{ invalid json"), c2pa::C2paException);
}

TEST(BuilderErrorHandling, EmptyManifestJsonReturnsErrorWithContext)
{
    auto context = c2pa::Context();
    EXPECT_THROW(c2pa::Builder(context, ""), c2pa::C2paException);
}

TEST(BuilderErrorHandling, MalformedJsonManifestReturnsErrorWithContext)
{
    auto context = c2pa::Context();
    EXPECT_THROW(c2pa::Builder(context, "{ invalid json"), c2pa::C2paException);
}

TEST(BuilderErrorHandling, JsonErrorsBehaveSameWithAndWithoutContext)
{
    std::vector<std::string> bad_inputs = {
        "",
        "null",
        "[]",
        "{",
        "{ invalid }",
        "{\"key\": }",
    };

    for (const auto& bad_input : bad_inputs) {
        // Without context
        EXPECT_THROW({
            c2pa::Builder builder(bad_input);
        }, c2pa::C2paException)
            << "Without context, should throw for: " << bad_input;

        // With context
        auto ctx = c2pa::Context();
        EXPECT_THROW({
            c2pa::Builder builder(ctx, bad_input);
        }, c2pa::C2paException)
            << "With context, should throw for: " << bad_input;
    }
}

TEST(BuilderErrorHandling, ValidJsonWorksWithAndWithoutContext)
{
    std::string valid_json = R"({"claim_generator": "test"})";

    // Without context
    EXPECT_NO_THROW({
        c2pa::Builder builder(valid_json);
    });

    // With context
    auto ctx = c2pa::Context();
    EXPECT_NO_THROW({
        c2pa::Builder builder(ctx, valid_json);
    });
}

TEST(BuilderErrorHandling, FailedConstructionWithAndWithoutContext)
{
    for (int i = 0; i < 100; i++) {
        // Without context
        try {
            c2pa::Builder("");
            FAIL() << "Should have thrown";
        } catch (const c2pa::C2paException&) {
            // Expected
        }

        // With context
        try {
            auto ctx = c2pa::Context();
            c2pa::Builder(ctx, "");
            FAIL() << "Should have thrown";
        } catch (const c2pa::C2paException&) {
            // Expected
        }
    }
}

TEST(BuilderErrorHandling, ErrorMessagesWithAndWithoutContext)
{
    // Without context
    try {
        c2pa::Builder("");
        FAIL() << "Should have thrown";
    } catch (const c2pa::C2paException& e) {
        std::string msg = e.what();
        EXPECT_FALSE(msg.empty()) << "Error message should be present";
    }

    // With context
    try {
        auto ctx = c2pa::Context();
        c2pa::Builder(ctx, "");
        FAIL() << "Should have thrown";
    } catch (const c2pa::C2paException& e) {
        std::string msg = e.what();
        EXPECT_FALSE(msg.empty()) << "Error message should be present when using context API";
    }
}

TEST_F(BuilderTest, AddAnActionAndSign)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("image_with_one_action.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    auto builder = c2pa::Builder(manifest);

    // Add an action to the builder
    string action_json = R"({
        "action": "c2pa.color_adjustments",
        "parameters": {
            "name": "brightnesscontrast"
        }
    })";
    builder.add_action(action_json);

    // Sign with the added actions. The Builder returns manifest bytes
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    // Read to verify
    auto reader = c2pa::Reader(output_path);
    string json_result;
    ASSERT_NO_THROW(json_result = reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));

    // Parse the JSON result for structured validation
    json manifest_json;
    ASSERT_NO_THROW(manifest_json = json::parse(json_result));

    // Get the active manifest
    ASSERT_TRUE(manifest_json.contains("manifests"));
    ASSERT_TRUE(manifest_json.contains("active_manifest"));
    string active_manifest_label = manifest_json["active_manifest"];
    ASSERT_TRUE(manifest_json["manifests"].contains(active_manifest_label));
    json active_manifest = manifest_json["manifests"][active_manifest_label];
    ASSERT_TRUE(active_manifest.contains("assertions"));
    ASSERT_TRUE(active_manifest["assertions"].is_array());

    // Find the c2pa.actions assertion
    json actions_assertion;
    bool found_actions_assertion = false;
    for (const auto& assertion : active_manifest["assertions"]) {
        if (assertion.contains("label") && assertion["label"] == "c2pa.actions.v2") {
            actions_assertion = assertion;
            found_actions_assertion = true;
            break;
        }
    }
    ASSERT_TRUE(found_actions_assertion);

    // Verify the actions assertion has the expected structure
    ASSERT_TRUE(actions_assertion.contains("data"));
    ASSERT_TRUE(actions_assertion["data"].contains("actions"));
    ASSERT_TRUE(actions_assertion["data"]["actions"].is_array());
    json actions_array = actions_assertion["data"]["actions"];
    ASSERT_FALSE(actions_array.empty());

    // Verify the action we added is here...
    json our_action;
    bool found_our_action = false;
    for (const auto& action : actions_array) {
        if (action.contains("action") && action["action"] == "c2pa.color_adjustments") {
            our_action = action;
            found_our_action = true;
            break;
        }
    }
    ASSERT_TRUE(found_our_action);

    // Verify the action structure
    ASSERT_TRUE(our_action.contains("action"));
    ASSERT_EQ(our_action["action"], "c2pa.color_adjustments");
    ASSERT_TRUE(our_action.contains("parameters"));
    ASSERT_TRUE(our_action["parameters"].contains("name"));
    ASSERT_EQ(our_action["parameters"]["name"], "brightnesscontrast");
};

TEST_F(BuilderTest, AddAnActionAndSignUsingContext)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("image_with_one_action_context.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Create a Context and pass it to the Builder
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest);

    // Add an action to the builder
    string action_json = R"({
        "action": "c2pa.color_adjustments",
        "parameters": {
            "name": "brightnesscontrast"
        }
    })";
    builder.add_action(action_json);

    // Sign with the added actions. The Builder returns manifest bytes
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    // Read to verify (also using context)
    auto reader = c2pa::Reader(context, output_path);
    string json_result;
    ASSERT_NO_THROW(json_result = reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));

    // Parse the JSON result for structured validation
    json manifest_json;
    ASSERT_NO_THROW(manifest_json = json::parse(json_result));

    // Get the active manifest
    ASSERT_TRUE(manifest_json.contains("manifests"));
    ASSERT_TRUE(manifest_json.contains("active_manifest"));
    string active_manifest_label = manifest_json["active_manifest"];
    ASSERT_TRUE(manifest_json["manifests"].contains(active_manifest_label));
    json active_manifest = manifest_json["manifests"][active_manifest_label];
    ASSERT_TRUE(active_manifest.contains("assertions"));
    ASSERT_TRUE(active_manifest["assertions"].is_array());

    // Find the c2pa.actions assertion
    json actions_assertion;
    bool found_actions_assertion = false;
    for (const auto& assertion : active_manifest["assertions"]) {
        if (assertion.contains("label") && assertion["label"] == "c2pa.actions.v2") {
            actions_assertion = assertion;
            found_actions_assertion = true;
            break;
        }
    }
    ASSERT_TRUE(found_actions_assertion);

    // Verify the actions assertion has the expected structure
    ASSERT_TRUE(actions_assertion.contains("data"));
    ASSERT_TRUE(actions_assertion["data"].contains("actions"));
    ASSERT_TRUE(actions_assertion["data"]["actions"].is_array());
    json actions_array = actions_assertion["data"]["actions"];
    ASSERT_FALSE(actions_array.empty());

    // Verify the action we added is here...
    json our_action;
    bool found_our_action = false;
    for (const auto& action : actions_array) {
        if (action.contains("action") && action["action"] == "c2pa.color_adjustments") {
            our_action = action;
            found_our_action = true;
            break;
        }
    }
    ASSERT_TRUE(found_our_action);

    // Verify the action structure
    ASSERT_TRUE(our_action.contains("action"));
    ASSERT_EQ(our_action["action"], "c2pa.color_adjustments");
    ASSERT_TRUE(our_action.contains("parameters"));
    ASSERT_TRUE(our_action["parameters"].contains("name"));
    ASSERT_EQ(our_action["parameters"]["name"], "brightnesscontrast");
};

TEST_F(BuilderTest, AddMultipleActionsAndSign)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("image_with_multiple_actions.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    auto builder = c2pa::Builder(manifest);

    // Add multiple actions to the builder
    string action_json_1 = R"({
        "action": "c2pa.color_adjustments",
        "parameters": {
            "name": "brightnesscontrast"
        }
    })";
    builder.add_action(action_json_1);
    string action_json_2 = R"({
        "action": "c2pa.filtered",
        "parameters": {
            "name": "A filter"
        },
        "description": "Filtering applied"
    })";
    builder.add_action(action_json_2);

    // Sign with the added actions
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    // Read to verify
    auto reader = c2pa::Reader(output_path);
    string json_result;
    ASSERT_NO_THROW(json_result = reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));

    // Parse the JSON result for structured validation
    json manifest_json;
    ASSERT_NO_THROW(manifest_json = json::parse(json_result));

    // Get the active manifest
    ASSERT_TRUE(manifest_json.contains("manifests"));
    ASSERT_TRUE(manifest_json.contains("active_manifest"));
    string active_manifest_label = manifest_json["active_manifest"];
    ASSERT_TRUE(manifest_json["manifests"].contains(active_manifest_label));
    json active_manifest = manifest_json["manifests"][active_manifest_label];
    ASSERT_TRUE(active_manifest.contains("assertions"));
    ASSERT_TRUE(active_manifest["assertions"].is_array());

    // Find the c2pa.actions assertion
    json actions_assertion;
    bool found_actions_assertion = false;
    for (const auto& assertion : active_manifest["assertions"]) {
        if (assertion.contains("label") && assertion["label"] == "c2pa.actions.v2") {
            actions_assertion = assertion;
            found_actions_assertion = true;
            break;
        }
    }
    ASSERT_TRUE(found_actions_assertion);

    // Verify the actions assertion structure
    ASSERT_TRUE(actions_assertion.contains("data"));
    ASSERT_TRUE(actions_assertion["data"].contains("actions"));
    ASSERT_TRUE(actions_assertion["data"]["actions"].is_array());

    // Verify both actions we added are in the actions array
    json actions_array = actions_assertion["data"]["actions"];
    ASSERT_FALSE(actions_array.empty());
    ASSERT_GE(actions_array.size(), 2);

    // Find our added actions...
    json color_adjustments_action;
    json filtered_action;
    bool found_color_adjustments = false;
    bool found_filtered = false;

    for (const auto& action : actions_array) {
        if (action.contains("action")) {
            if (action["action"] == "c2pa.color_adjustments") {
                color_adjustments_action = action;
                found_color_adjustments = true;
            } else if (action["action"] == "c2pa.filtered") {
                filtered_action = action;
                found_filtered = true;
            }
        }
    }

    ASSERT_TRUE(found_color_adjustments);
    ASSERT_TRUE(found_filtered);

    // Verify the color_adjustments action structure
    ASSERT_TRUE(color_adjustments_action.contains("action"));
    ASSERT_EQ(color_adjustments_action["action"], "c2pa.color_adjustments");
    ASSERT_TRUE(color_adjustments_action.contains("parameters"));
    ASSERT_TRUE(color_adjustments_action["parameters"].contains("name"));
    ASSERT_EQ(color_adjustments_action["parameters"]["name"], "brightnesscontrast");

    // Verify the filtered action structure
    ASSERT_TRUE(filtered_action.contains("action"));
    ASSERT_EQ(filtered_action["action"], "c2pa.filtered");
    ASSERT_TRUE(filtered_action.contains("parameters"));
    ASSERT_TRUE(filtered_action["parameters"].contains("name"));
    ASSERT_EQ(filtered_action["parameters"]["name"], "A filter");
    ASSERT_TRUE(filtered_action.contains("description"));
    ASSERT_EQ(filtered_action["description"], "Filtering applied");
};

TEST_F(BuilderTest, AddMultipleActionsAndSignUsingContext)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("image_with_multiple_actions_context.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Create a Context and pass it to the Builder
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest);

    // Add multiple actions to the builder
    string action_json_1 = R"({
        "action": "c2pa.color_adjustments",
        "parameters": {
            "name": "brightnesscontrast"
        }
    })";
    builder.add_action(action_json_1);
    string action_json_2 = R"({
        "action": "c2pa.filtered",
        "parameters": {
            "name": "A filter"
        },
        "description": "Filtering applied"
    })";
    builder.add_action(action_json_2);

    // Sign with the added actions
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    // Read to verify (also using context)
    auto reader = c2pa::Reader(context, output_path);
    string json_result;
    ASSERT_NO_THROW(json_result = reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));

    // Parse the JSON result for structured validation
    json manifest_json;
    ASSERT_NO_THROW(manifest_json = json::parse(json_result));

    // Get the active manifest
    ASSERT_TRUE(manifest_json.contains("manifests"));
    ASSERT_TRUE(manifest_json.contains("active_manifest"));
    string active_manifest_label = manifest_json["active_manifest"];
    ASSERT_TRUE(manifest_json["manifests"].contains(active_manifest_label));
    json active_manifest = manifest_json["manifests"][active_manifest_label];
    ASSERT_TRUE(active_manifest.contains("assertions"));
    ASSERT_TRUE(active_manifest["assertions"].is_array());

    // Find the c2pa.actions assertion
    json actions_assertion;
    bool found_actions_assertion = false;
    for (const auto& assertion : active_manifest["assertions"]) {
        if (assertion.contains("label") && assertion["label"] == "c2pa.actions.v2") {
            actions_assertion = assertion;
            found_actions_assertion = true;
            break;
        }
    }
    ASSERT_TRUE(found_actions_assertion);

    // Verify the actions assertion structure
    ASSERT_TRUE(actions_assertion.contains("data"));
    ASSERT_TRUE(actions_assertion["data"].contains("actions"));
    ASSERT_TRUE(actions_assertion["data"]["actions"].is_array());

    // Verify both actions we added are in the actions array
    json actions_array = actions_assertion["data"]["actions"];
    ASSERT_FALSE(actions_array.empty());
    ASSERT_GE(actions_array.size(), 2);

    // Find our added actions...
    json color_adjustments_action;
    json filtered_action;
    bool found_color_adjustments = false;
    bool found_filtered = false;

    for (const auto& action : actions_array) {
        if (action.contains("action")) {
            if (action["action"] == "c2pa.color_adjustments") {
                color_adjustments_action = action;
                found_color_adjustments = true;
            } else if (action["action"] == "c2pa.filtered") {
                filtered_action = action;
                found_filtered = true;
            }
        }
    }

    ASSERT_TRUE(found_color_adjustments);
    ASSERT_TRUE(found_filtered);

    // Verify the color_adjustments action structure
    ASSERT_TRUE(color_adjustments_action.contains("action"));
    ASSERT_EQ(color_adjustments_action["action"], "c2pa.color_adjustments");
    ASSERT_TRUE(color_adjustments_action.contains("parameters"));
    ASSERT_TRUE(color_adjustments_action["parameters"].contains("name"));
    ASSERT_EQ(color_adjustments_action["parameters"]["name"], "brightnesscontrast");

    // Verify the filtered action structure
    ASSERT_TRUE(filtered_action.contains("action"));
    ASSERT_EQ(filtered_action["action"], "c2pa.filtered");
    ASSERT_TRUE(filtered_action.contains("parameters"));
    ASSERT_TRUE(filtered_action["parameters"].contains("name"));
    ASSERT_EQ(filtered_action["parameters"]["name"], "A filter");
    ASSERT_TRUE(filtered_action.contains("description"));
    ASSERT_EQ(filtered_action["description"], "Filtering applied");
};

TEST_F(BuilderTest, SignImageFileOnly)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("training_image_only.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    auto builder = c2pa::Builder(manifest);
    // the Builder returns manifest bytes
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    // read to verify
    auto reader = c2pa::Reader(output_path);
    ASSERT_NO_THROW(reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));
};

TEST_F(BuilderTest, SignImageFileNoThumbnailAutoGenThreadLocalSettings)
{
    // Run in separate thread for complete test isolation (thread-local settings won't leak)
    fs::path temp_output = get_temp_path("training_image_only_thread_local.jpg");
    std::thread test_thread([temp_output]() {
        fs::path current_dir = fs::path(__FILE__).parent_path();

        // Construct the paths relative to the current directory
        fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
        fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
        fs::path image_path = current_dir / "../tests/fixtures/A.jpg";
        fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
        fs::path output_path = temp_output;

        auto manifest = c2pa_test::read_text_file(manifest_path);
        auto certs = c2pa_test::read_text_file(certs_path);
        auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

        // set settings to not generate thumbnails (thread-local)
        c2pa::load_settings("{\"builder\": { \"thumbnail\": {\"enabled\": false}}}", "json");

        // create a signer
        c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

        auto builder = c2pa::Builder(manifest);
        // the Builder returns manifest bytes
        std::vector<unsigned char> manifest_data;
        ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

        // read to verify
        ASSERT_TRUE(std::filesystem::exists(output_path));
        auto reader = c2pa::Reader(output_path);
        ASSERT_NO_THROW(reader.json());

        // No need to reset settings - thread-local settings destroyed with thread
    });

    // Wait for thread to complete
    test_thread.join();
};

TEST_F(BuilderTest, SignImageFileNoThumbnailAutoGen)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path_with_context = get_temp_path("settings_no_thumbnails.jpg");
    fs::path output_path_no_context = get_temp_path("settings_with_thumbnails.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Test 1: Create context with specific settings via JSON
    auto context = c2pa::Context("{\"builder\": { \"thumbnail\": {\"enabled\": false}}}");

    // Create builder using context containing settings
    auto builder_with_context = c2pa::Builder(context, manifest);
    std::vector<unsigned char> manifest_data_context;
    ASSERT_NO_THROW(manifest_data_context = builder_with_context.sign(signed_image_path, output_path_with_context, signer));

    // Verify the signed file with context exists and is readable
    ASSERT_TRUE(std::filesystem::exists(output_path_with_context));
    auto reader_with_context = c2pa::Reader(output_path_with_context);
    std::string json_with_context;
    ASSERT_NO_THROW(json_with_context = reader_with_context.json());
    EXPECT_FALSE(json_with_context.empty());

    // Test 2: Create builder WITHOUT context (uses default behavior)
    auto builder_no_context = c2pa::Builder(manifest);
    std::vector<unsigned char> manifest_data_no_context;
    ASSERT_NO_THROW(manifest_data_no_context = builder_no_context.sign(signed_image_path, output_path_no_context, signer));

    // Verify the signed file without context exists and is readable
    ASSERT_TRUE(std::filesystem::exists(output_path_no_context));
    auto reader_no_context = c2pa::Reader(output_path_no_context);
    std::string json_no_context;
    ASSERT_NO_THROW(json_no_context = reader_no_context.json());
    EXPECT_FALSE(json_no_context.empty());

    // Both builders should successfully create valid manifests,
    // but one should have thumbnails whereas the other shouldn't
    json parsed_context = json::parse(json_with_context);
    json parsed_no_context = json::parse(json_no_context);

    // Both should have valid structure
    EXPECT_TRUE(parsed_context.contains("active_manifest"));
    EXPECT_TRUE(parsed_no_context.contains("active_manifest"));

    std::string active_manifest_value_context = parsed_context["active_manifest"];
    EXPECT_FALSE(parsed_context["manifests"][active_manifest_value_context].contains("thumbnail"));
    std::string active_manifest_value_no_context = parsed_no_context["active_manifest"];
    EXPECT_TRUE(parsed_no_context["manifests"][active_manifest_value_no_context].contains("thumbnail"));
};

TEST_F(BuilderTest, SignImageThumbnailSettingsFileJson)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path_with_context = get_temp_path("image_context_settings_json.jpg");
    fs::path output_path_no_context = get_temp_path("image_no_context_json.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Create context with specific settings via JSON, by loading the JSON file with the settings
    fs::path settings_path = current_dir / "../tests/fixtures/settings/test_settings_no_thumbnail.json";
    auto settings_json = c2pa_test::read_text_file(settings_path);
    auto context = c2pa::Context(settings_json);

    // Create builder using context containing settings (does not generate thumbnails)
    auto builder_no_thumbnail = c2pa::Builder(context, manifest);
    std::vector<unsigned char> manifest_data_no_thumbnail;
    ASSERT_NO_THROW(manifest_data_no_thumbnail = builder_no_thumbnail.sign(signed_image_path, output_path_with_context, signer));

    // Verify the signed file with context exists and is readable
    ASSERT_TRUE(std::filesystem::exists(output_path_with_context));
    auto reader_without_thumbnails = c2pa::Reader(output_path_with_context);
    std::string json_without_thumbnails;
    ASSERT_NO_THROW(json_without_thumbnails = reader_without_thumbnails.json());
    EXPECT_FALSE(json_without_thumbnails.empty());

    // Now, create builder with another context (settings generate a thumbnail)
    fs::path settings_path2 = current_dir / "../tests/fixtures/settings/test_settings_with_thumbnail.json";
    auto settings_json2 = c2pa_test::read_text_file(settings_path2);
    auto context2 = c2pa::Context(settings_json2);

    auto builder_with_thumbnail = c2pa::Builder(context2, manifest);
    std::vector<unsigned char> manifest_data_with_thumbnail;
    ASSERT_NO_THROW(manifest_data_with_thumbnail = builder_with_thumbnail.sign(signed_image_path, output_path_no_context, signer));

    // Verify the signed file without context exists and is readable
    ASSERT_TRUE(std::filesystem::exists(output_path_no_context));
    auto reader_with_thumbnails = c2pa::Reader(output_path_no_context);
    std::string json_with_thumbnails;
    ASSERT_NO_THROW(json_with_thumbnails = reader_with_thumbnails.json());
    EXPECT_FALSE(json_with_thumbnails.empty());

    // Both builders should successfully create valid manifests,
    // but one should have thumbnails whereas the other shouldn't
    json parsed_no_thumbnail = json::parse(json_without_thumbnails);
    json parsed_with_thumbnail = json::parse(json_with_thumbnails);

    // Both should have valid structure
    EXPECT_TRUE(parsed_no_thumbnail.contains("active_manifest"));
    EXPECT_TRUE(parsed_with_thumbnail.contains("active_manifest"));

    std::string active_manifest_value_context = parsed_no_thumbnail["active_manifest"];
    EXPECT_FALSE(parsed_no_thumbnail["manifests"][active_manifest_value_context].contains("thumbnail"));
    std::string active_manifest_value_no_context = parsed_with_thumbnail["active_manifest"];
    EXPECT_TRUE(parsed_with_thumbnail["manifests"][active_manifest_value_no_context].contains("thumbnail"));
};

TEST_F(BuilderTest, SignImageThumbnailSettingsObject)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path_no_thumbnail = get_temp_path("image_no_thumbnail_incremental.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Here we are going to build settings, field by field
    // Start with default settings
    // If setting the same value multiple times, last one wins
    c2pa::Settings settings;
    settings
        .set("builder.thumbnail.enabled", "true")
        .set("builder.thumbnail.enabled", "false");

    // Build context from Settings object we just did
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(settings)
        .create_context();

    auto builder = c2pa::Builder(context, manifest);
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path_no_thumbnail, signer));

    // Verify the signed file exists and is readable
    ASSERT_TRUE(std::filesystem::exists(output_path_no_thumbnail));
    auto reader = c2pa::Reader(output_path_no_thumbnail);
    std::string json_result;
    ASSERT_NO_THROW(json_result = reader.json());
    EXPECT_FALSE(json_result.empty());

    // Verify NO thumbnail is present
    auto parsed = json::parse(json_result);
    EXPECT_TRUE(parsed.contains("active_manifest"));
    std::string active_manifest = parsed["active_manifest"];
    EXPECT_FALSE(parsed["manifests"][active_manifest].contains("thumbnail"));
}

TEST_F(BuilderTest, SignImageThumbnailSettingsIncrementalObject)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path_no_thumbnail = get_temp_path("image_no_thumbnail_incremental2.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Here we are going to update a settings object we just build
    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "true");
    std::string updated_config = R"({
        "builder": {
            "thumbnail": {
                "enabled": false
            }
        }
    })";
    settings.update(updated_config, "json");

    // Build context from Settings object we just did
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(settings)
        .create_context();

    auto builder = c2pa::Builder(context, manifest);
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path_no_thumbnail, signer));

    // Verify the signed file exists and is readable
    ASSERT_TRUE(std::filesystem::exists(output_path_no_thumbnail));
    auto reader = c2pa::Reader(output_path_no_thumbnail);
    std::string json_result;
    ASSERT_NO_THROW(json_result = reader.json());
    EXPECT_FALSE(json_result.empty());

    // Verify NO thumbnail is present
    auto parsed = json::parse(json_result);
    EXPECT_TRUE(parsed.contains("active_manifest"));
    std::string active_manifest = parsed["active_manifest"];
    EXPECT_FALSE(parsed["manifests"][active_manifest].contains("thumbnail"));
}

TEST_F(BuilderTest, SignImageFileWithResource)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("training_resource_only.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    auto builder = c2pa::Builder(manifest);
    // add a resource: a thumbnail
    builder.add_resource("thumbnail", image_path);

    // sign
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    // read to verify signature
    auto reader = c2pa::Reader(output_path);
    ASSERT_NO_THROW(reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));
};

TEST_F(BuilderTest, SignImageFileWithResourceUsingContext)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("training_resource_only_context.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Create a Context and pass it to the Builder
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest);
    // add a resource: a thumbnail
    builder.add_resource("thumbnail", image_path);

    // sign
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    // read to verify signature (also using context)
    auto reader = c2pa::Reader(context, output_path);
    ASSERT_NO_THROW(reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));
};

TEST_F(BuilderTest, SignWithMultipleResources)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path image_resource = current_dir / "../tests/fixtures/C.jpg";
    fs::path image_resource_other = current_dir / "../tests/fixtures/sample1.gif";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("multiple_resources.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    auto builder = c2pa::Builder(manifest);

    // add multiple resources
    builder.add_resource("thumbnail1", image_path);
    builder.add_resource("thumbnail2", image_resource);
    builder.add_resource("thumbnail3", image_resource_other);

    // sign
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    // read to verify signature
    auto reader = c2pa::Reader(output_path);
    ASSERT_NO_THROW(reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));
}

TEST_F(BuilderTest, SignImageFileWithIngredient)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("training_ingredient_only.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    auto builder = c2pa::Builder(manifest);

    // add an ingredient
    string ingredient_json = "{\"title\":\"Test Ingredient\"}";
    // the signed image can be used as an ingredient too
    builder.add_ingredient(ingredient_json, signed_image_path);

    // sign
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    // read to verify signature
    auto reader = c2pa::Reader(output_path);
    ASSERT_NO_THROW(reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));
};

TEST_F(BuilderTest, SignImageFileWithResourceAndIngredient)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("training.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");
    auto builder = c2pa::Builder(manifest);
    // add a resource: a thumbnail
    builder.add_resource("thumbnail", image_path);

    // add an ingredient
    string ingredient_json = "{\"title\":\"Test Ingredient\"}";
    // the signed image can be used as an ingredient too
    builder.add_ingredient(ingredient_json, signed_image_path);

    // sign
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    // read to verify signature
    auto reader = c2pa::Reader(output_path);
    ASSERT_NO_THROW(reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));
};

TEST_F(BuilderTest, SignVideoFileWithMultipleIngredients)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_video_path = current_dir / "../tests/fixtures/video1.mp4";
    fs::path audio_ingredient = current_dir / "../tests/fixtures/sample1_signed.wav";
    fs::path image_ingredient = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("video1_signed_multi_ingredients.mp4");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // create the builder
    auto builder = c2pa::Builder(manifest);

    // add the ingredients for the video
    string ingredient_video_json = "{\"title\":\"Test Video Ingredient\", \"relationship\": \"parentOf\"}";
    builder.add_ingredient(ingredient_video_json, signed_video_path);
    string ingredient_audio_json = "{\"title\":\"Test Audio Ingredient\", \"relationship\": \"componentOf\"}";
    builder.add_ingredient(ingredient_audio_json, audio_ingredient);
    string ingredient_image_json = "{\"title\":\"Test Image Ingredient\", \"relationship\": \"componentOf\"}";
    builder.add_ingredient(ingredient_image_json, image_ingredient);

    // sign
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_video_path, output_path, signer));

    // read to verify signature
    auto reader = c2pa::Reader(output_path);
    ASSERT_NO_THROW(reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));
};

TEST_F(BuilderTest, SignVideoFileWithMultipleIngredientsUsingContext)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_video_path = current_dir / "../tests/fixtures/video1.mp4";
    fs::path audio_ingredient = current_dir / "../tests/fixtures/sample1_signed.wav";
    fs::path image_ingredient = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("video1_signed_context.mp4");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // create the builder with context
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest);

    // add the ingredients for the video
    string ingredient_video_json = "{\"title\":\"Test Video Ingredient\", \"relationship\": \"parentOf\"}";
    builder.add_ingredient(ingredient_video_json, signed_video_path);
    string ingredient_audio_json = "{\"title\":\"Test Audio Ingredient\", \"relationship\": \"componentOf\"}";
    builder.add_ingredient(ingredient_audio_json, audio_ingredient);
    string ingredient_image_json = "{\"title\":\"Test Image Ingredient\", \"relationship\": \"componentOf\"}";
    builder.add_ingredient(ingredient_image_json, image_ingredient);

    // sign
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_video_path, output_path, signer));

    // read to verify signature (also using context)
    auto reader = c2pa::Reader(context, output_path);
    ASSERT_NO_THROW(reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));
};

TEST_F(BuilderTest, SignVideoFileWithMultipleIngredientsAndResources)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_video_path = current_dir / "../tests/fixtures/video1.mp4";
    fs::path audio_ingredient = current_dir / "../tests/fixtures/sample1_signed.wav";
    fs::path image_ingredient = current_dir / "../tests/fixtures/A.jpg";
    fs::path image_resource = current_dir / "../tests/fixtures/C.jpg";
    fs::path image_resource_other = current_dir / "../tests/fixtures/sample1.gif";
    fs::path output_path = get_temp_path("video1_signed_with_ingredients_and_resources.mp4");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // create the builder
    auto builder = c2pa::Builder(manifest);

    // add the ingredients for the video
    string ingredient_video_json = "{\"title\":\"Test Video Ingredient\", \"relationship\": \"parentOf\"}";
    builder.add_ingredient(ingredient_video_json, signed_video_path);
    string ingredient_audio_json = "{\"title\":\"Test Audio Ingredient\", \"relationship\": \"componentOf\"}";
    builder.add_ingredient(ingredient_audio_json, audio_ingredient);
    string ingredient_image_json = "{\"title\":\"Test Image Ingredient\", \"relationship\": \"componentOf\"}";
    builder.add_ingredient(ingredient_image_json, image_ingredient);

    // add multiple resources
    builder.add_resource("thumbnail1", image_ingredient);
    builder.add_resource("thumbnail2", image_resource);
    builder.add_resource("thumbnail3", image_resource_other);

    // sign
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_video_path, output_path, signer));

    // read to verify signature
    auto reader = c2pa::Reader(output_path);
    ASSERT_NO_THROW(reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));
};

TEST_F(BuilderTest, SignVideoFileWithMultipleIngredientsAndResourcesInterleaved)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_video_path = current_dir / "../tests/fixtures/video1.mp4";
    fs::path audio_ingredient = current_dir / "../tests/fixtures/sample1_signed.wav";
    fs::path image_ingredient = current_dir / "../tests/fixtures/A.jpg";
    fs::path image_resource = current_dir / "../tests/fixtures/C.jpg";
    fs::path image_resource_other = current_dir / "../tests/fixtures/sample1.gif";
    fs::path output_path = get_temp_path("video1_signed_interleaved.mp4");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");
    // create the builder
    auto builder = c2pa::Builder(manifest);

    // add ingredients and resources
    string ingredient_video_json = "{\"title\":\"Test Video Ingredient\", \"relationship\": \"parentOf\"}";
    builder.add_ingredient(ingredient_video_json, signed_video_path);
    builder.add_resource("thumbnail1", image_ingredient);
    string ingredient_audio_json = "{\"title\":\"Test Audio Ingredient\", \"relationship\": \"componentOf\"}";
    builder.add_ingredient(ingredient_audio_json, audio_ingredient);
    builder.add_resource("thumbnail2", image_resource);
    builder.add_resource("thumbnail3", image_resource_other);
    string ingredient_image_json = "{\"title\":\"Test Image Ingredient\", \"relationship\": \"componentOf\"}";
    builder.add_ingredient(ingredient_image_json, image_ingredient);

    // sign
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_video_path, output_path, signer));

    // read to verify signature
    auto reader = c2pa::Reader(output_path);
    ASSERT_NO_THROW(reader.json());
    ASSERT_TRUE(std::filesystem::exists(output_path));
};

TEST_F(BuilderTest, SignImageStreamWithoutContext)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    auto builder = c2pa::Builder(manifest);

    std::ifstream source(signed_image_path, std::ios::binary);
    if (!source)
    {
        FAIL() << "Failed to open file: " << signed_image_path << std::endl;
    }

    // Create a memory buffer
    std::stringstream memory_buffer(std::ios::in | std::ios::out | std::ios::binary);
    std::iostream &dest = memory_buffer;
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign("image/jpeg", source, dest, signer));
    source.close();

    // Rewind dest to the start
    dest.flush();
    dest.seekp(0, std::ios::beg);
    auto reader = c2pa::Reader("image/jpeg", dest);
    std::string json;
    ASSERT_NO_THROW(json = reader.json());
    ASSERT_TRUE(json.find("cawg.training-mining") != std::string::npos);
}

TEST_F(BuilderTest, SignImageStream)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Create a default context (no custom settings)
    auto context = c2pa::Context();

    // Create builder with context
    auto builder = c2pa::Builder(context, manifest);

    std::ifstream source(signed_image_path, std::ios::binary);
    if (!source)
    {
        FAIL() << "Failed to open file: " << signed_image_path << std::endl;
    }

    // Create a memory buffer for the destination stream
    std::stringstream memory_buffer(std::ios::in | std::ios::out | std::ios::binary);
    std::iostream &dest = memory_buffer;

    // Sign using stream APIs
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign("image/jpeg", source, dest, signer));
    source.close();

    // Rewind dest to the start for reading
    dest.flush();
    dest.seekp(0, std::ios::beg);
    dest.seekg(0, std::ios::beg);

    // Create reader with same context to read from stream
    auto reader = c2pa::Reader(context, "image/jpeg", dest);
    std::string json;
    ASSERT_NO_THROW(json = reader.json());

    // Verify the manifest contains expected data
    ASSERT_TRUE(json.find("cawg.training-mining") != std::string::npos);
    ASSERT_FALSE(manifest_data.empty());
}

TEST_F(BuilderTest, SignImageStreamBuilderReaderDifferentContext)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Create a default context (no custom settings, defaults to SDK default settings)
    auto write_context = c2pa::Context();

    // Create builder with context
    auto builder = c2pa::Builder(write_context, manifest);

    std::ifstream source(signed_image_path, std::ios::binary);
    if (!source)
    {
        FAIL() << "Failed to open file: " << signed_image_path << std::endl;
    }

    // Create a memory buffer for the destination stream
    std::stringstream memory_buffer(std::ios::in | std::ios::out | std::ios::binary);
    std::iostream &dest = memory_buffer;

    // Sign using stream APIs
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign("image/jpeg", source, dest, signer));
    source.close();

    // Rewind dest to the start for reading
    dest.flush();
    dest.seekp(0, std::ios::beg);
    dest.seekg(0, std::ios::beg);

    // Create a reader. The Reader can have a different context than the Builder,
    // which could enable different settings for reading vs writing!
    // (Here for this test we use a demo empty context again)

    // Create a default context (no custom settings)
    auto read_context = c2pa::Context();

    auto reader = c2pa::Reader(read_context, "image/jpeg", dest);
    std::string json;
    ASSERT_NO_THROW(json = reader.json());

    // Verify the manifest contains expected data
    ASSERT_TRUE(json.find("cawg.training-mining") != std::string::npos);
    ASSERT_FALSE(manifest_data.empty());
}

TEST_F(BuilderTest, SignImageWithIngredientHavingManifestStream)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path ingredient_image_path = current_dir / "../tests/fixtures/C.jpg";

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    auto builder = c2pa::Builder(manifest);

    // there can only be one parent ingredient
    string ingredient_json = "{\"title\":\"Test Ingredient\", \"relationship\": \"parentOf\"}";
    builder.add_ingredient(ingredient_json, signed_image_path);
    // other ingredients can be components
    string ingredient_json_2 = "{\"title\":\"Test Ingredient 2\", \"relationship\": \"componentOf\"}";
    builder.add_ingredient(ingredient_json_2, ingredient_image_path);

    std::ifstream source(signed_image_path, std::ios::binary);
    if (!source)
    {
        FAIL() << "Failed to open file: " << signed_image_path << std::endl;
    }

    // Create a memory buffer
    std::stringstream memory_buffer(std::ios::in | std::ios::out | std::ios::binary);
    std::iostream &dest = memory_buffer;
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign("image/jpeg", source, dest, signer));
    source.close();

    // Rewind dest to the start
    dest.flush();
    dest.seekp(0, std::ios::beg);
    auto reader = c2pa::Reader("image/jpeg", dest);
    std::string json;
    ASSERT_NO_THROW(json = reader.json());
    ASSERT_TRUE(json.find("cawg.training-mining") != std::string::npos);
}

TEST_F(BuilderTest, SignStreamCloudUrl)
{
    try
    {
        fs::path current_dir = fs::path(__FILE__).parent_path();

        // Construct the paths relative to the current directory
        fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
        fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
        fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";

        auto manifest = c2pa_test::read_text_file(manifest_path);
        auto certs = c2pa_test::read_text_file(certs_path);
        auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

        // create a signer
        c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

        auto builder = c2pa::Builder(manifest);

        // very important to use a URL that does not exist, otherwise you may get a JumbfParseError or JumbfNotFound
        builder.set_remote_url("http://this_does_not_exist/foo.jpg");
        builder.set_no_embed();

        // auto manifest_data = builder.sign(signed_image_path, "build/dest.jpg", signer);
        std::ifstream source(signed_image_path, std::ios::binary);
        if (!source)
        {
            FAIL() << "Failed to open file: " << signed_image_path << std::endl;
        }

        // Create a memory buffer
        std::stringstream memory_buffer(std::ios::in | std::ios::out | std::ios::binary);
        std::iostream &dest = memory_buffer;
        auto manifest_data = builder.sign("image/jpeg", source, dest, signer);
        source.close();

        // Rewind dest to the start
        dest.flush();
        dest.seekp(0, std::ios::beg);

        auto reader = c2pa::Reader("image/jpeg", dest);
        auto json = reader.json();
    }
    catch (c2pa::C2paException const &e)
    {
        std::string error_message = e.what();
        if (error_message.rfind("Remote:", 0) == 0)
        {
            SUCCEED();
        }
        else
        {
            FAIL() << "Failed: C2pa::Builder: " << e.what() << endl;
        }
    };
}

TEST_F(BuilderTest, SignDataHashedEmbedded)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    // fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    auto builder = c2pa::Builder(manifest);

    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");

    std::string data_hash = R"({
      "exclusions": [
        {
          "start": 20,
          "length": 45884
        }
      ],
      "name": "jumbf manifest",
      "alg": "sha256",
      "hash": "gWZNEOMHQNiULfA/tO5HD2awOwYMA3tnfUPApIr9csk=",
      "pad": " "
    })";
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign_data_hashed_embeddable(signer, data_hash, "image/jpeg"));
}

TEST_F(BuilderTest, SignDataHashedEmbeddedUsingContext)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    // fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Create a Context and pass it to the Builder
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest);

    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");

    std::string data_hash = R"({
      "exclusions": [
        {
          "start": 20,
          "length": 45884
        }
      ],
      "name": "jumbf manifest",
      "alg": "sha256",
      "hash": "gWZNEOMHQNiULfA/tO5HD2awOwYMA3tnfUPApIr9csk=",
      "pad": " "
    })";
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign_data_hashed_embeddable(signer, data_hash, "image/jpeg"));
}

TEST_F(BuilderTest, SignDataHashedEmbeddedWithAsset)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    auto builder = c2pa::Builder(manifest);

    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");

    std::string data_hash = R"({
      "exclusions": [
        {
          "start": 20,
          "length": 45884
        }
      ],
      "name": "jumbf manifest",
      "alg": "sha256",
      "hash": "",
      "pad": " "
    })";

    std::ifstream asset(image_path, std::ios::binary);
    if (!asset)
    {
        FAIL() << "Failed to open file: " << image_path << std::endl;
    }

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign_data_hashed_embeddable(signer, data_hash, "application/c2pa", &asset));

    auto embeddable_data = c2pa::Builder::format_embeddable("image/jpeg", manifest_data);

    ASSERT_TRUE(embeddable_data.size() > manifest_data.size());
}

TEST_F(BuilderTest, SignDataHashedEmbeddedWithAssetUsingContext)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Create a Context and pass it to the Builder
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest);

    auto placeholder = builder.data_hashed_placeholder(signer.reserve_size(), "image/jpeg");

    std::string data_hash = R"({
      "exclusions": [
        {
          "start": 20,
          "length": 45884
        }
      ],
      "name": "jumbf manifest",
      "alg": "sha256",
      "hash": "",
      "pad": " "
    })";

    std::ifstream asset(image_path, std::ios::binary);
    if (!asset)
    {
        FAIL() << "Failed to open file: " << image_path << std::endl;
    }

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign_data_hashed_embeddable(signer, data_hash, "application/c2pa", &asset));

    auto embeddable_data = c2pa::Builder::format_embeddable("image/jpeg", manifest_data);

    ASSERT_TRUE(embeddable_data.size() > manifest_data.size());
}

TEST_F(BuilderTest, SignWithInvalidStream)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    auto builder = c2pa::Builder(manifest);

    // create an empty/invalid stream
    std::stringstream empty_stream;

    std::stringstream dest;

    // expect the sign operation to fail due to invalid/empty stream
    EXPECT_THROW(builder.sign("image/jpeg", empty_stream, dest, signer), c2pa::C2paException);
}

TEST_F(BuilderTest, SignWithoutTimestamping)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the paths relative to the current directory
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer without tsa uri
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key);

    auto builder = c2pa::Builder(manifest);

    std::ifstream source(signed_image_path, std::ios::binary);
    if (!source)
    {
        FAIL() << "Failed to open file: " << signed_image_path << std::endl;
    }

    // Create a memory buffer
    std::stringstream memory_buffer(std::ios::in | std::ios::out | std::ios::binary);
    std::iostream &dest = memory_buffer;
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign("image/jpeg", source, dest, signer));
    source.close();

    // Rewind dest to the start
    dest.flush();
    dest.seekp(0, std::ios::beg);
    auto reader = c2pa::Reader("image/jpeg", dest);
    string json_result;
    ASSERT_NO_THROW(json_result = reader.json());

    // Parse the JSON result for validation
    json result_json = json::parse(json_result);

    // Get the active manifest signature info
    ASSERT_TRUE(result_json.contains("active_manifest"));
    ASSERT_TRUE(result_json.contains("manifests"));
    string active_manifest_label = result_json["active_manifest"];
    ASSERT_TRUE(result_json["manifests"].contains(active_manifest_label));
    json active_manifest = result_json["manifests"][active_manifest_label];
    ASSERT_TRUE(active_manifest.contains("signature_info"));
    ASSERT_TRUE(active_manifest["signature_info"].is_object());

    // Expect no signature timestamp when tsa uri is not provided
    ASSERT_FALSE(active_manifest["signature_info"].contains("time"));
}

TEST_F(BuilderTest, AddIngredientAsResourceToBuilder)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";

    // Construct the path to the test fixture
    fs::path ingredient_source_path = current_dir / "../tests/fixtures/A.jpg";

    // Create the builder using a manifest JSON
    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto builder = c2pa::Builder(manifest);

    // Add the ingredient from its source file. The Builder reads the file,
    // generates the thumbnail resource, and adds the ingredient to the manifest.
    builder.add_ingredient("{}", ingredient_source_path);

    // Create a signer
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    std::vector<unsigned char> manifest_data;
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("signed_with_ingredient_and_resource_1.jpg");
    manifest_data = builder.sign(signed_image_path, output_path, signer);

    auto reader = c2pa::Reader(output_path);
    ASSERT_NO_THROW(reader.json());
}

TEST_F(BuilderTest, LinkIngredientsAndSign)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";

    // Construct the path to the test fixture
    fs::path ingredient_source_path = current_dir / "../tests/fixtures/A.jpg";
    std::string ingredient_source_path_str = ingredient_source_path.string();

    fs::path temp_dir = get_temp_dir("ingredient_linked_resource");

    // Create the builder using a manifest JSON
    auto manifest = c2pa_test::read_text_file(manifest_path);

    // Parse the manifest and modify it
    json manifest_json = json::parse(manifest);

    // Find the c2pa.actions assertion and add ingredientIds to c2pa.created action
    // The values in ingredientIds are going to be used to link ingredients to actions
    // Those values should be valid UUIDs
    for (auto& assertion : manifest_json["assertions"]) {
        if (assertion["label"] == "c2pa.actions") {
            for (auto& action : assertion["data"]["actions"]) {
                if (action["action"] == "c2pa.created") {
                    action["parameters"]["ingredientIds"] = json::array({"test:iid:939a4c48-0dff-44ec-8f95-61f52b11618f"});
                    break;
                }
            }
            break;
        }
    }

    auto builder = c2pa::Builder(manifest_json.dump());

    // add an ingredient
    // instance_id, if found in an action's ingredientIds array,
    // will be used to link the ingredient to the action
    json ingredient_obj = {
        {"title", "Test Ingredient"},
        {"relationship", "parentOf"},
        {"instance_id", "test:iid:939a4c48-0dff-44ec-8f95-61f52b11618f"}
    };
    string ingredient_json = ingredient_obj.dump();
    builder.add_ingredient(ingredient_json, ingredient_source_path);

    // Create a signer
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("signed_with_ingredient_and_resource_2.jpg");

    std::vector<unsigned char> manifest_data;
    manifest_data = builder.sign(signed_image_path, output_path, signer);

    auto reader = c2pa::Reader(output_path);
    string json_result;
    ASSERT_NO_THROW(json_result = reader.json());

    // Parse the JSON result for validation
    json result_json = json::parse(json_result);

    // Get the active manifest
    ASSERT_TRUE(result_json.contains("active_manifest"));
    ASSERT_TRUE(result_json.contains("manifests"));
    string active_manifest_label = result_json["active_manifest"];
    ASSERT_TRUE(result_json["manifests"].contains(active_manifest_label));
    json active_manifest = result_json["manifests"][active_manifest_label];

    // Verify ingredients array has exactly one ingredient with label "c2pa.ingredient.v3"
    ASSERT_TRUE(active_manifest.contains("ingredients"));
    ASSERT_TRUE(active_manifest["ingredients"].is_array());
    ASSERT_EQ(active_manifest["ingredients"].size(), 1);
    ASSERT_TRUE(active_manifest["ingredients"][0].contains("label"));
    ASSERT_EQ(active_manifest["ingredients"][0]["label"], "c2pa.ingredient.v3");

    // Find the c2pa.actions assertion
    ASSERT_TRUE(active_manifest.contains("assertions"));
    ASSERT_TRUE(active_manifest["assertions"].is_array());

    json actions_assertion;
    bool found_actions = false;
    for (const auto& assertion : active_manifest["assertions"]) {
        if (assertion.contains("label") && assertion["label"] == "c2pa.actions.v2") {
            actions_assertion = assertion;
            found_actions = true;
            break;
        }
    }
    ASSERT_TRUE(found_actions);

    // Find the c2pa.created action
    ASSERT_TRUE(actions_assertion.contains("data"));
    ASSERT_TRUE(actions_assertion["data"].contains("actions"));
    ASSERT_TRUE(actions_assertion["data"]["actions"].is_array());

    json created_action;
    bool found_created = false;
    for (const auto& action : actions_assertion["data"]["actions"]) {
        if (action.contains("action") && action["action"] == "c2pa.created") {
            created_action = action;
            found_created = true;
            break;
        }
    }
    ASSERT_TRUE(found_created);

    // Verify the c2pa.created action has an ingredients array with exactly one object
    // that has url: "self#jumbf=c2pa.assertions/c2pa.ingredient.v3"
    ASSERT_TRUE(created_action.contains("parameters"));
    ASSERT_TRUE(created_action["parameters"].contains("ingredients"));
    ASSERT_TRUE(created_action["parameters"]["ingredients"].is_array());
    ASSERT_EQ(created_action["parameters"]["ingredients"].size(), 1);
    ASSERT_TRUE(created_action["parameters"]["ingredients"][0].contains("url"));
    ASSERT_EQ(created_action["parameters"]["ingredients"][0]["url"], "self#jumbf=c2pa.assertions/c2pa.ingredient.v3");
}

TEST_F(BuilderTest, LinkIngredientsAndSignUsingContext)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";

    // Construct the path to the test fixture
    fs::path ingredient_source_path = current_dir / "../tests/fixtures/A.jpg";
    std::string ingredient_source_path_str = ingredient_source_path.string();

    fs::path temp_dir = get_temp_dir("ingredient_linked_resource_context");

    // Create the builder using a manifest JSON
    auto manifest = c2pa_test::read_text_file(manifest_path);

    // Parse the manifest and modify it
    json manifest_json = json::parse(manifest);

    // Find the c2pa.actions assertion and add ingredientIds to c2pa.created action
    // The values in ingredientIds are going to be used to link ingredients to actions
    // Those values should be valid UUIDs
    for (auto& assertion : manifest_json["assertions"]) {
        if (assertion["label"] == "c2pa.actions") {
            for (auto& action : assertion["data"]["actions"]) {
                if (action["action"] == "c2pa.created") {
                    action["parameters"]["ingredientIds"] = json::array({"test:iid:939a4c48-0dff-44ec-8f95-61f52b11618f"});
                    break;
                }
            }
            break;
        }
    }

    // Create a Context and pass it to the Builder
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json.dump());

    // add an ingredient
    // instance_id, if found in an action's ingredientIds array,
    // will be used to link the ingredient to the action
    json ingredient_obj = {
        {"title", "Test Ingredient"},
        {"relationship", "parentOf"},
        {"instance_id", "test:iid:939a4c48-0dff-44ec-8f95-61f52b11618f"}
    };
    string ingredient_json = ingredient_obj.dump();
    builder.add_ingredient(ingredient_json, ingredient_source_path);

    // Create a signer
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("signed_with_ingredient_and_resource_context.jpg");

    std::vector<unsigned char> manifest_data;
    manifest_data = builder.sign(signed_image_path, output_path, signer);

    // Read to verify (also using context)
    auto reader = c2pa::Reader(context, output_path);
    string json_result;
    ASSERT_NO_THROW(json_result = reader.json());

    // Parse the JSON result for validation
    json result_json = json::parse(json_result);

    // Get the active manifest
    ASSERT_TRUE(result_json.contains("active_manifest"));
    ASSERT_TRUE(result_json.contains("manifests"));
    string active_manifest_label = result_json["active_manifest"];
    ASSERT_TRUE(result_json["manifests"].contains(active_manifest_label));
    json active_manifest = result_json["manifests"][active_manifest_label];

    // Verify ingredients array has exactly one ingredient with label "c2pa.ingredient.v3"
    ASSERT_TRUE(active_manifest.contains("ingredients"));
    ASSERT_TRUE(active_manifest["ingredients"].is_array());
    ASSERT_EQ(active_manifest["ingredients"].size(), 1);
    ASSERT_TRUE(active_manifest["ingredients"][0].contains("label"));
    ASSERT_EQ(active_manifest["ingredients"][0]["label"], "c2pa.ingredient.v3");

    // Find the c2pa.actions assertion
    ASSERT_TRUE(active_manifest.contains("assertions"));
    ASSERT_TRUE(active_manifest["assertions"].is_array());

    json actions_assertion;
    bool found_actions = false;
    for (const auto& assertion : active_manifest["assertions"]) {
        if (assertion.contains("label") && assertion["label"] == "c2pa.actions.v2") {
            actions_assertion = assertion;
            found_actions = true;
            break;
        }
    }
    ASSERT_TRUE(found_actions);

    // Find the c2pa.created action
    ASSERT_TRUE(actions_assertion.contains("data"));
    ASSERT_TRUE(actions_assertion["data"].contains("actions"));
    ASSERT_TRUE(actions_assertion["data"]["actions"].is_array());

    json created_action;
    bool found_created = false;
    for (const auto& action : actions_assertion["data"]["actions"]) {
        if (action.contains("action") && action["action"] == "c2pa.created") {
            created_action = action;
            found_created = true;
            break;
        }
    }
    ASSERT_TRUE(found_created);

    // Verify the c2pa.created action has an ingredients array with exactly one object
    // that has url: "self#jumbf=c2pa.assertions/c2pa.ingredient.v3"
    ASSERT_TRUE(created_action.contains("parameters"));
    ASSERT_TRUE(created_action["parameters"].contains("ingredients"));
    ASSERT_TRUE(created_action["parameters"]["ingredients"].is_array());
    ASSERT_EQ(created_action["parameters"]["ingredients"].size(), 1);
    ASSERT_TRUE(created_action["parameters"]["ingredients"][0].contains("url"));
    ASSERT_EQ(created_action["parameters"]["ingredients"][0]["url"], "self#jumbf=c2pa.assertions/c2pa.ingredient.v3");
}

TEST_F(BuilderTest, AddIngredientToBuilderUsingBasePath)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";

    // Construct the path to the test fixture
    fs::path ingredient_source_path = current_dir / "../tests/fixtures/A.jpg";

    // Create the builder using a manifest JSON
    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto builder = c2pa::Builder(manifest);

    // Add the ingredient from its source file. The Builder reads the file and
    // embeds the ingredient resources directly (no separate data dir / base path).
    builder.add_ingredient("{}", ingredient_source_path);

    // Create a signer
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    std::vector<unsigned char> manifest_data;
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("signed_with_ingredient_and_resource_3.jpg");
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    auto reader = c2pa::Reader(output_path);
    ASSERT_NO_THROW(reader.json());
}

TEST_F(BuilderTest, AddIngredientToBuilderUsingBasePathPlacedActionThreadLocalSettings)
{
    // Run in separate thread for complete test isolation (thread-local settings won't leak)
    std::thread test_thread([this]() {
        fs::path current_dir = fs::path(__FILE__).parent_path();

        // Construct the path to the test fixture
        fs::path ingredient_source_path = current_dir / "../tests/fixtures/A.jpg";
        std::string ingredient_source_path_str = ingredient_source_path.string();

        // Use temp dir for ingredient data
        fs::path build_dir = current_dir.parent_path() / "build";
        if (!fs::exists(build_dir)) {
            fs::create_directories(build_dir);
        }
        fs::path temp_dir = build_dir / "builder-ingredient_placed_as_resource";

        // Remove and recreate the temp data dir folder before using it
        if (fs::exists(temp_dir)) {
            fs::remove_all(temp_dir);
        }
        fs::create_directories(temp_dir);

        // set settings to not auto-add a placed action (thread-local)
        c2pa::load_settings("{\"builder\": { \"actions\": {\"auto_placed_action\": {\"enabled\": false}}}}", "json");

        // The instance_id we assign to the ingredient and reference from the placed action.
        std::string instance_id = "test:iid:939a4c48-0dff-44ec-8f95-61f52b11618f";

        // Create the manifest JSON structure with the placed action
        json manifest_json = {
            {"vendor", "a-vendor"},
            {"claim_generator_info", json::array({
                {
                    {"name", "c2pa-c test"},
                    {"version", "1.0.0"}
                }
            })},
            {"assertions", json::array({
                {
                    {"label", "c2pa.actions"},
                    {"data", {
                        {"actions", json::array({
                            {
                                {"action", "c2pa.created"},
                                {"description", "Created a new file or content"},
                                {"parameters", {
                                    {"com.vendor.tool", "new"}
                                }},
                                {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"}
                            },
                            {
                                {"action", "c2pa.placed"},
                                {"description", "Added pre-existing content to this file"},
                                {"parameters", {
                                    {"com.vendor.tool", "place_embedded_object"},
                                    {"ingredientIds", json::array({instance_id})}
                                }}
                            }
                        })},
                        {"metadata", {
                            {"dateTime", "2025-09-25T20:59:48.262Z"}
                        }}
                    }}
                }
            })}
        };

        // Now we can create a Builder with the manifest
        auto builder = c2pa::Builder(manifest_json.dump());

        // Add the ingredient with the matching instance_id so the placed action links to it.
        // The Builder reads the source file and embeds the ingredient resources directly.
        json ingredient_obj = {
            {"instance_id", instance_id},
            {"relationship", "componentOf"}
        };
        builder.add_ingredient(ingredient_obj.dump(), ingredient_source_path);

        // Create a signer
        fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
        auto certs = c2pa_test::read_text_file(certs_path);
        auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");
        c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

        std::vector<unsigned char> manifest_data;
        fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
        fs::path output_path = get_temp_path("signed_with_ingredient_and_resource_4.jpg");
        ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

        auto reader = c2pa::Reader(output_path);
        ASSERT_NO_THROW(reader.json());

        // Cleanup temp dir if needed
        if (cleanup_temp_files) {
            if (fs::exists(temp_dir)) {
                fs::remove_all(temp_dir);
            }
        }

        // No need to reset settings - thread-local settings destroyed with thread
    });

    // Wait for thread to complete
    test_thread.join();
}

TEST_F(BuilderTest, AddIngredientToBuilderUsingBasePathWithManifestContainingPlacedAction)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // Construct the path to the test fixture
    fs::path ingredient_source_path = current_dir / "../tests/fixtures/A.jpg";
    std::string ingredient_source_path_str = ingredient_source_path.string();

    // Use temp dir for ingredient data
    fs::path temp_dir = get_temp_dir("ingredient_placed_context");

    // Create context with auto_placed_action disabled via JSON settings
    auto context = c2pa::Context("{\"builder\": { \"actions\": {\"auto_placed_action\": {\"enabled\": false}}}}");

    // The instance_id we assign to the ingredient and reference from the placed action.
    std::string instance_id = "test:iid:939a4c48-0dff-44ec-8f95-61f52b11618f";

    // Create the manifest JSON structure with the placed action
    json manifest_json = {
        {"vendor", "a-vendor"},
        {"claim_generator_info", json::array({
            {
                {"name", "c2pa-c test with context"},
                {"version", "1.0.0"}
            }
        })},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions"},
                {"data", {
                    {"actions", json::array({
                        {
                            {"action", "c2pa.created"},
                            {"description", "Created a new file or content"},
                            {"parameters", {
                                {"com.vendor.tool", "new"}
                            }},
                            {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"}
                        },
                        {
                            {"action", "c2pa.placed"},
                            {"description", "Added pre-existing content to this file"},
                            {"parameters", {
                                {"com.vendor.tool", "place_embedded_object"},
                                {"ingredientIds", json::array({instance_id})}
                            }}
                        }
                    })},
                    {"metadata", {
                        {"dateTime", "2025-09-25T20:59:48.262Z"}
                    }}
                }}
            }
        })}
    };

    // Create a Builder with the context and manifest
    auto builder = c2pa::Builder(context, manifest_json.dump());

    // Add the ingredient with the matching instance_id so the placed action links to it.
    // The Builder reads the source file and embeds the ingredient resources directly.
    json ingredient_obj = {
        {"instance_id", instance_id},
        {"relationship", "componentOf"}
    };
    builder.add_ingredient(ingredient_obj.dump(), ingredient_source_path);

    // Create a signer
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Sign the image
    std::vector<unsigned char> manifest_data;
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("signed_with_ingredient_context.jpg");
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    // Read and verify
    auto reader = c2pa::Reader(output_path);
    std::string reader_json;
    ASSERT_NO_THROW(reader_json = reader.json());

    // Verify the placed action is in the manifest
    ASSERT_TRUE(reader_json.find("c2pa.placed") != std::string::npos);
    ASSERT_TRUE(reader_json.find(instance_id) != std::string::npos);
}

TEST_F(BuilderTest, AddIngredientWithProvenanceDataToBuilderUsingBasePath)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";

    // Construct the path to the test fixture
    fs::path ingredient_source_path = current_dir / "../tests/fixtures/C.jpg";

    // Create the builder using a manifest JSON
    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto builder = c2pa::Builder(manifest);

    // Add the ingredient from its source file. C.jpg has a manifest store, so the
    // Builder reads its provenance/manifest data and embeds the ingredient resources.
    builder.add_ingredient("{}", ingredient_source_path);

    // Create a signer
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    std::vector<unsigned char> manifest_data;
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("signed_with_ingredient_and_resource_5.jpg");
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    auto reader = c2pa::Reader(output_path);
    ASSERT_NO_THROW(reader.json());
}

TEST_F(BuilderTest, MultipleBuildersDifferentThumbnailSettingsInterleaved)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // All the test fixtures
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path_no_thumbnail = current_dir / "../build/examples/no_thumbnail_interleaved_1.jpg";
    fs::path output_path_with_thumbnails = current_dir / "../build/examples/with_thumbnails_interleaved_1.jpg";
    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Create one context with specific settings via JSON, by loading the JSON file with the settings
    fs::path settings_path = current_dir / "../tests/fixtures/settings/test_settings_no_thumbnail.json";
    auto settings_json = c2pa_test::read_text_file(settings_path);
    auto context_without_thumbnails = c2pa::Context(settings_json);

    // Now, create anothetrcontext, that sets thumbnails to be generated
    fs::path settings_path2 = current_dir / "../tests/fixtures/settings/test_settings_with_thumbnail.json";
    auto settings_json2 = c2pa_test::read_text_file(settings_path2);
    auto context_with_thumbnails = c2pa::Context(settings_json2);

    // Create builder using context containing settings that does not generate thumbnails
    auto builder_no_thumbnail = c2pa::Builder(context_without_thumbnails, manifest);
    std::vector<unsigned char> manifest_data_no_thumbnail;
    ASSERT_NO_THROW(manifest_data_no_thumbnail = builder_no_thumbnail.sign(signed_image_path, output_path_no_thumbnail, signer));

    // Create builder using context containing settings that does generate thumbnails
    auto builder_with_thumbnail = c2pa::Builder(context_with_thumbnails, manifest);
    std::vector<unsigned char> manifest_data_with_thumbnail;
    ASSERT_NO_THROW(manifest_data_with_thumbnail = builder_with_thumbnail.sign(signed_image_path, output_path_with_thumbnails, signer));

    // Verify the signed file with context exists and is readable
    ASSERT_TRUE(std::filesystem::exists(output_path_no_thumbnail));
    auto reader_without_thumbnails = c2pa::Reader(output_path_no_thumbnail);
    std::string json_without_thumbnails;
    ASSERT_NO_THROW(json_without_thumbnails = reader_without_thumbnails.json());
    EXPECT_FALSE(json_without_thumbnails.empty());

    // Verify the signed file without context exists and is readable
    ASSERT_TRUE(std::filesystem::exists(output_path_with_thumbnails));
    auto reader_with_thumbnails = c2pa::Reader(output_path_with_thumbnails);
    std::string json_with_thumbnails;
    ASSERT_NO_THROW(json_with_thumbnails = reader_with_thumbnails.json());
    EXPECT_FALSE(json_with_thumbnails.empty());

    // Both builders should successfully create valid manifests,
    // but one should have thumbnails whereas the other shouldn't,
    // and we can even have different settings per Builder, as they trickle down!
    json parsed_no_thumbnail = json::parse(json_without_thumbnails);
    json parsed_with_thumbnail = json::parse(json_with_thumbnails);

    // Both should have valid structure
    EXPECT_TRUE(parsed_no_thumbnail.contains("active_manifest"));
    EXPECT_TRUE(parsed_with_thumbnail.contains("active_manifest"));

    std::string active_manifest_value_context = parsed_no_thumbnail["active_manifest"];
    EXPECT_FALSE(parsed_no_thumbnail["manifests"][active_manifest_value_context].contains("thumbnail"));
    std::string active_manifest_value_no_context = parsed_with_thumbnail["active_manifest"];
    EXPECT_TRUE(parsed_with_thumbnail["manifests"][active_manifest_value_no_context].contains("thumbnail"));
};

TEST_F(BuilderTest, MultipleBuildersDifferentThumbnailSettingsInterleaved2)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // All the test fixtures
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path_no_thumbnail = current_dir / "../build/examples/no_thumbnail_interleaved_2.jpg";
    fs::path output_path_with_thumbnails = current_dir / "../build/examples/with_thumbnails_interleaved_2.jpg";
    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create a signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Create one context with specific settings via JSON, by loading the JSON file with the settings
    fs::path settings_path = current_dir / "../tests/fixtures/settings/test_settings_no_thumbnail.json";
    auto settings_json = c2pa_test::read_text_file(settings_path);
    auto context_without_thumbnails = c2pa::Context(settings_json);

    // Now, create another context, that sets thumbnails to be generated
    fs::path settings_path2 = current_dir / "../tests/fixtures/settings/test_settings_with_thumbnail.json";
    auto settings_json2 = c2pa_test::read_text_file(settings_path2);
    auto context_with_thumbnails = c2pa::Context(settings_json2);

    // Create builder using context containing settings that does generate thumbnails
    auto builder_with_thumbnail = c2pa::Builder(context_with_thumbnails, manifest);
    std::vector<unsigned char> manifest_data_with_thumbnail;
    ASSERT_NO_THROW(manifest_data_with_thumbnail = builder_with_thumbnail.sign(signed_image_path, output_path_with_thumbnails, signer));

    // Create builder using context containing settings that does not generate thumbnails
    auto builder_no_thumbnail = c2pa::Builder(context_without_thumbnails, manifest);
    std::vector<unsigned char> manifest_data_no_thumbnail;
    ASSERT_NO_THROW(manifest_data_no_thumbnail = builder_no_thumbnail.sign(signed_image_path, output_path_no_thumbnail, signer));

    // Verify the signed file with context exists and is readable
    ASSERT_TRUE(std::filesystem::exists(output_path_no_thumbnail));
    auto reader_without_thumbnails = c2pa::Reader(output_path_no_thumbnail);
    std::string json_without_thumbnails;
    ASSERT_NO_THROW(json_without_thumbnails = reader_without_thumbnails.json());
    EXPECT_FALSE(json_without_thumbnails.empty());

    // Verify the signed file without context exists and is readable
    ASSERT_TRUE(std::filesystem::exists(output_path_with_thumbnails));
    auto reader_with_thumbnails = c2pa::Reader(output_path_with_thumbnails);
    std::string json_with_thumbnails;
    ASSERT_NO_THROW(json_with_thumbnails = reader_with_thumbnails.json());
    EXPECT_FALSE(json_with_thumbnails.empty());

    // Both builders should successfully create valid manifests,
    // but one should have thumbnails whereas the other shouldn't,
    // and we can even have different settings per Builder, as they trickle down!
    json parsed_no_thumbnail = json::parse(json_without_thumbnails);
    json parsed_with_thumbnail = json::parse(json_with_thumbnails);

    // Both should have valid structure
    EXPECT_TRUE(parsed_no_thumbnail.contains("active_manifest"));
    EXPECT_TRUE(parsed_with_thumbnail.contains("active_manifest"));

    std::string active_manifest_value_context = parsed_no_thumbnail["active_manifest"];
    EXPECT_FALSE(parsed_no_thumbnail["manifests"][active_manifest_value_context].contains("thumbnail"));
    std::string active_manifest_value_no_context = parsed_with_thumbnail["active_manifest"];
    EXPECT_TRUE(parsed_with_thumbnail["manifests"][active_manifest_value_no_context].contains("thumbnail"));
};

TEST_F(BuilderTest, TrustHandling)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();

    // All the test fixtures
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path signed_image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("trust_handling_test.jpg");
    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");

    // Create our very own signer
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    // Trust is based on a chain of trusted certificates. When signing, we may need to know
    // if the ingredients are trusted at time of signing, so we benefit from having a context
    // already configured with that trust to use with our Builder and Reader.
    fs::path settings_path = current_dir / "../tests/fixtures/settings/test_settings_example.json";
    auto settings = c2pa_test::read_text_file(settings_path);
    auto trusted_context = c2pa::Context::ContextBuilder().with_settings(c2pa::Settings(settings, "json")).create_context();

    // Create builder using context containing settings that does generate thumbnails
    auto builder = c2pa::Builder(trusted_context, manifest);
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(signed_image_path, output_path, signer));

    // Verify the signed file with context exists and is readable
    ASSERT_TRUE(std::filesystem::exists(output_path));

    // When reading, the Reader also needs to know about trust, to determine the manifest validation state
    // If there is a valid trust chain, the manifest will be in validation_state Trusted.
    auto reader = c2pa::Reader(trusted_context, output_path);
    std::string read_json_manifest;
    ASSERT_NO_THROW(read_json_manifest = reader.json());
    ASSERT_FALSE(read_json_manifest.empty());

    // Both builders should successfully create valid manifests,
    // but one should have thumbnails whereas the other shouldn't,
    // and we can even have different settings per Builder, as they trickle down!
    json parsed_manifest_json = json::parse(read_json_manifest);

    // Smoke check that the manifest has a valid structure
    ASSERT_TRUE(parsed_manifest_json["validation_state"] == "Trusted");

    // But if we don't know about the whole trust chain,
    // for instance it is not configured on the Reader's context,
    // then our manifest is "only" Valid.

    // If the Reader doesn't know about trust, we can only be "Valid" in the best case
    // For instance, a Reader without context won't know about trust
    auto reader2 = c2pa::Reader(output_path);
    std::string read_json_manifest2;
    ASSERT_NO_THROW(read_json_manifest2 = reader2.json());
    ASSERT_FALSE(read_json_manifest2.empty());

    // Both builders should successfully create valid manifests,
    // but one should have thumbnails whereas the other shouldn't,
    // and we can even have different settings per Builder, as they trickle down!
    json parsed_manifest_json2 = json::parse(read_json_manifest2);

    // Smoke check that the manifest has a valid structure
    ASSERT_TRUE(parsed_manifest_json2["validation_state"] == "Valid");

    // It is also important to make sure the proper trust chain is configured in settings...
    // If not, we won't be able to read the manifest as trusted!
    fs::path settings_without_trust_path = current_dir / "../tests/fixtures/settings/test_settings_no_thumbnail.json";
    auto settings_without_trust = c2pa_test::read_text_file(settings_without_trust_path);
    auto no_trust_context = c2pa::Context(settings_without_trust);

    auto reader3 = c2pa::Reader(no_trust_context, output_path);
    std::string read_json_manifest3;
    ASSERT_NO_THROW(read_json_manifest3 = reader3.json());
    ASSERT_FALSE(read_json_manifest3.empty());

    // Both builders should successfully create valid manifests,
    // but one should have thumbnails whereas the other shouldn't,
    // and we can even have different settings per Builder, as they trickle down!
    json parsed_manifest_json3 = json::parse(read_json_manifest3);
    // Smoke check that the manifest has a valid structure
    ASSERT_TRUE(parsed_manifest_json3["validation_state"] == "Valid");
};

TEST_F(BuilderTest, SignWithIStreamAndOStream_RoundTrip)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";
    fs::path output_path = get_temp_path("stream_ostream_roundtrip.jpg");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    std::filesystem::create_directories(output_path.parent_path());

    auto builder = c2pa::Builder(manifest);
    std::ifstream source(image_path, std::ios::binary);
    ASSERT_TRUE(source) << "Failed to open " << image_path;

    std::ofstream dest(output_path, std::ios::binary);
    ASSERT_TRUE(dest) << "Failed to open " << output_path;

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign("image/jpeg", source, dest, signer));
    source.close();
    dest.close();

    ASSERT_FALSE(manifest_data.empty());
    ASSERT_TRUE(std::filesystem::exists(output_path));

    c2pa::Reader reader(output_path);
    std::string json_result;
    ASSERT_NO_THROW(json_result = reader.json());
    ASSERT_TRUE(json_result.find("cawg.training-mining") != std::string::npos);
}

TEST_F(BuilderTest, SignWithIStreamAndIOStream_RoundTrip)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path manifest_path = current_dir / "../tests/fixtures/training.json";
    fs::path certs_path = current_dir / "../tests/fixtures/es256_certs.pem";
    fs::path image_path = current_dir / "../tests/fixtures/A.jpg";

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto certs = c2pa_test::read_text_file(certs_path);
    auto p_key = c2pa_test::read_text_file(current_dir / "../tests/fixtures/es256_private.key");
    c2pa::Signer signer = c2pa::Signer("Es256", certs, p_key, "http://timestamp.digicert.com");

    auto builder = c2pa::Builder(manifest);
    std::ifstream source(image_path, std::ios::binary);
    ASSERT_TRUE(source) << "Failed to open " << image_path;

    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    std::iostream& dest = buffer;

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign("image/jpeg", source, dest, signer));
    source.close();

    dest.flush();
    dest.seekg(0, std::ios::beg);
    dest.seekp(0, std::ios::beg);

    c2pa::Reader reader("image/jpeg", dest);
    std::string json_result;
    ASSERT_NO_THROW(json_result = reader.json());
    ASSERT_FALSE(manifest_data.empty());
    ASSERT_TRUE(json_result.find("cawg.training-mining") != std::string::npos);
}

TEST_F(BuilderTest, ArchiveRoundTrip)
{
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto context = c2pa::Context();
    auto builder1 = c2pa::Builder(context, manifest);

    // Export to archive
    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        builder1.to_archive(archive_stream);
    });

    // Re-import from archive
    archive_stream.seekg(0);
    auto builder2 = c2pa::Builder::from_archive(archive_stream);

    // Sign with the restored builder
    auto signer = c2pa_test::create_test_signer();
    std::ifstream source(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(source.is_open());
    std::stringstream dest(std::ios::in | std::ios::out | std::ios::binary);

    EXPECT_NO_THROW({
        builder2.sign("image/jpeg", source, dest, signer);
    });

    // Verify signed output has data
    dest.seekg(0, std::ios::end);
    EXPECT_GT(dest.tellg(), 0);
}

TEST_F(BuilderTest, ArchiveRoundTripSettingsBehavior)
{
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // Create context with settings that disable thumbnail generation
    auto settings_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("settings/test_settings_no_thumbnail.json"));
    auto context_no_thumbnail = c2pa::Context(settings_json);

    // Verify the setting works when set on builder with context (baseline)
    auto builder_direct = c2pa::Builder(context_no_thumbnail, manifest);
    auto signer = c2pa_test::create_test_signer();
    std::ifstream source1(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(source1.is_open());
    std::stringstream dest_direct(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
      builder_direct.sign("image/jpeg", source1, dest_direct, signer);
    });
    // Verify no thumbnail in direct sign
    dest_direct.seekg(0);
    auto ctx_reader_direct = c2pa::Context();
    auto reader_direct = c2pa::Reader(ctx_reader_direct, "image/jpeg", dest_direct);
    auto json_direct = json::parse(reader_direct.json());
    std::string active_direct = json_direct["active_manifest"];
    EXPECT_FALSE(json_direct["manifests"][active_direct].contains("thumbnail"))
        << "Direct sign with thumbnail=false should not generate thumbnail";

    // from_archive does not preserve settings of the archived builder,
    // They would need to be restored... So here we expect a manifest with thumbnails then!
    auto builder1 = c2pa::Builder(context_no_thumbnail, manifest);
    // Export to archive
    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        builder1.to_archive(archive_stream);
    });
    // Re-import from archive (creates builder with DEFAULT context/settings)
    archive_stream.seekg(0);
    auto builder2 = c2pa::Builder::from_archive(archive_stream);
    // Sign with the restored builder (reuse the same signer from above)
    std::ifstream source2(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(source2.is_open());
    std::stringstream dest_archive(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        builder2.sign("image/jpeg", source2, dest_archive, signer);
    });
    // Verify manifest structure is preserved
    dest_archive.seekg(0);
    auto ctx_reader_archive = c2pa::Context();
    auto reader_archive = c2pa::Reader(ctx_reader_archive, "image/jpeg", dest_archive);
    auto json_archive = json::parse(reader_archive.json());
    EXPECT_TRUE(json_archive.contains("active_manifest"));

    // Verify that the archive round-trip manifest HAS a thumbnail
    // (because from_archive() uses default settings which enable thumbnails)
    std::string active_archive = json_archive["active_manifest"];
    EXPECT_TRUE(json_archive["manifests"][active_archive].contains("thumbnail"))
        << "Archive round-trip (default context on Builder) should generate thumbnail with default settings";
}

TEST_F(BuilderTest, ArchiveRoundTripSettingsBehaviorRestoredContext)
{
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // Create context with settings that disable thumbnail generation
    auto settings_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("settings/test_settings_no_thumbnail.json"));
    auto context_no_thumbnail = c2pa::Context(settings_json);

    // Verify the setting works when set on builder with context (baseline)
    auto builder_direct = c2pa::Builder(context_no_thumbnail, manifest);
    auto signer = c2pa_test::create_test_signer();
    std::ifstream source1(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(source1.is_open());
    std::stringstream dest_direct(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
      builder_direct.sign("image/jpeg", source1, dest_direct, signer);
    });
    // Verify no thumbnail is here (aka setting was applied)
    dest_direct.seekg(0);
    auto ctx_reader_direct = c2pa::Context();
    auto reader_direct = c2pa::Reader(ctx_reader_direct, "image/jpeg", dest_direct);
    auto json_direct = json::parse(reader_direct.json());
    std::string active_direct = json_direct["active_manifest"];
    EXPECT_FALSE(json_direct["manifests"][active_direct].contains("thumbnail"))
        << "Direct sign with thumbnail=false should not generate thumbnail";

    // from_archive does not preserve settings of the archived builder,
    // They would need to be restored... So here we expect a manifest with thumbnails then!
    auto builder1 = c2pa::Builder(context_no_thumbnail, manifest);
    // Export to archive
    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        builder1.to_archive(archive_stream);
    });
    // Re-import from archive (creates builder with DEFAULT context/settings)
    archive_stream.seekg(0);
    auto builder2 = c2pa::Builder::from_archive(archive_stream);
    // Sign with the restored builder (reuse the same signer from above)
    std::ifstream source2(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(source2.is_open());
    std::stringstream dest_archive(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        builder2.sign("image/jpeg", source2, dest_archive, signer);
    });
    // Verify manifest structure is preserved
    dest_archive.seekg(0);
    auto ctx_reader_archive = c2pa::Context();
    auto reader_archive = c2pa::Reader(ctx_reader_archive, "image/jpeg", dest_archive);
    auto json_archive = json::parse(reader_archive.json());
    EXPECT_TRUE(json_archive.contains("active_manifest"));

    // Verify that the archive round-trip manifest HAS a thumbnail
    // (because from_archive() uses default settings which enable thumbnails)
    std::string active_archive = json_archive["active_manifest"];
    EXPECT_TRUE(json_archive["manifests"][active_archive].contains("thumbnail"))
        << "Archive round-trip (default context on Builder) should generate thumbnail with default settings";
}

TEST_F(BuilderTest, LoadArchiveWithContext)
{
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // Create a context with thumbnail generation disabled
    auto context = c2pa::Context(R"({"builder": {"thumbnail": {"enabled": false}}})");

    // Create a builder with the custom context
    auto builder1 = c2pa::Builder(context, manifest);

    // Add an ingredient
    std::string ingredient_json = R"({"title": "C.jpg Ingredient"})";
    builder1.add_ingredient(ingredient_json, c2pa_test::get_fixture_path("C.jpg"));

    // Export to archive
    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        builder1.to_archive(archive_stream);
    });

    // Create a new builder with a context, then load the archive into it
    // This preserves the context's settings (thumbnail=false)
    auto context2 = c2pa::Context(R"({"builder": {"thumbnail": {"enabled": false}}})");

    archive_stream.seekg(0);
    c2pa::Builder builder2(context2);  // Create a builder with context
    EXPECT_NO_THROW({
        builder2.to_archive(archive_stream);  // Load an archive into the builder
    });

    builder2.add_action(R"({"action": "c2pa.created", "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"})");

    // Sign with the restored builder (which has thumbnail=false from context2)
    auto signer = c2pa_test::create_test_signer();

    std::ifstream source2(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(source2.is_open());
    std::stringstream dest_stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        builder2.sign("image/jpeg", source2, dest_stream, signer);
    });

    // Verify the signed asset has NO thumbnail (because context2 had thumbnail=false)
    dest_stream.seekg(0);
    auto ctx_reader = c2pa::Context();
    auto reader = c2pa::Reader(ctx_reader, "image/jpeg", dest_stream);
    auto json_result = json::parse(reader.json());
    EXPECT_TRUE(json_result.contains("active_manifest"));

    std::string active_manifest = json_result["active_manifest"];
    EXPECT_FALSE(json_result["manifests"][active_manifest].contains("thumbnail"))
        << "Builder loaded with custom context should respect thumbnail=false setting";
}

// Test adding multiple builder archives that got signed as assets, back as ingredients
TEST_F(BuilderTest, MultipleArchivesAsIngredients)
{
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();

    // Create first builder and export to archive
    auto builder1 = c2pa::Builder(manifest);
    std::stringstream archive1_stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        builder1.to_archive(archive1_stream);
    });

    // Create second builder and export to archive
    auto builder2 = c2pa::Builder(manifest);
    std::stringstream archive2_stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        builder2.to_archive(archive2_stream);
    });

    // Create third builder and export to archive
    auto builder3 = c2pa::Builder(manifest);
    std::stringstream archive3_stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        builder3.to_archive(archive3_stream);
    });

    // Load each archive and sign to create assets that can be reused as ingredients
    // Sign first archive: signed asset 1
    archive1_stream.seekg(0);
    auto loaded_builder1 = c2pa::Builder::from_archive(archive1_stream);
    std::ifstream source1(c2pa_test::get_fixture_path("C.jpg"), std::ios::binary);
    ASSERT_TRUE(source1.is_open());
    std::stringstream signed_asset1(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        loaded_builder1.sign("image/jpeg", source1, signed_asset1, signer);
    });

    // Sign second archive: signed asset 2
    archive2_stream.seekg(0);
    auto loaded_builder2 = c2pa::Builder::from_archive(archive2_stream);
    std::ifstream source2(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(source2.is_open());
    std::stringstream signed_asset2(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        loaded_builder2.sign("image/jpeg", source2, signed_asset2, signer);
    });

    // Sign third archive: signed asset 3
    archive3_stream.seekg(0);
    auto loaded_builder3 = c2pa::Builder::from_archive(archive3_stream);
    std::ifstream source3(c2pa_test::get_fixture_path("sample1.gif"), std::ios::binary);
    ASSERT_TRUE(source3.is_open());
    std::stringstream signed_asset3(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        loaded_builder3.sign("image/gif", source3, signed_asset3, signer);
    });

    // Create a final builder and add all three signed assets as ingredients with different relationships
    auto final_builder = c2pa::Builder(manifest);

    // Add first signed asset as parentOf ingredient
    signed_asset1.seekg(0);
    std::string ingredient1_json = R"({"title": "Archive 1 Ingredient", "relationship": "parentOf"})";
    EXPECT_NO_THROW({
        final_builder.add_ingredient(ingredient1_json, "image/jpeg", signed_asset1);
    });

    // Add second signed asset as componentOf ingredient
    signed_asset2.seekg(0);
    std::string ingredient2_json = R"({"title": "Archive 2 Ingredient", "relationship": "componentOf"})";
    EXPECT_NO_THROW({
        final_builder.add_ingredient(ingredient2_json, "image/jpeg", signed_asset2);
    });

    // Add third signed asset as componentOf ingredient
    signed_asset3.seekg(0);
    std::string ingredient3_json = R"({"title": "Archive 3 Ingredient", "relationship": "componentOf"})";
    EXPECT_NO_THROW({
        final_builder.add_ingredient(ingredient3_json, "image/gif", signed_asset3);
    });

    // Sign the final builder
    std::ifstream final_source(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(final_source.is_open());
    std::stringstream dest_stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_NO_THROW({
        final_builder.sign("image/jpeg", final_source, dest_stream, signer);
    });

    // Verify all three ingredients are present with correct relationships
    dest_stream.seekg(0);
    auto ctx_reader = c2pa::Context();
    auto reader = c2pa::Reader(ctx_reader, "image/jpeg", dest_stream);
    auto json_result = json::parse(reader.json());
    EXPECT_TRUE(json_result.contains("active_manifest"));

    std::string active = json_result["active_manifest"];
    ASSERT_TRUE(json_result["manifests"][active].contains("ingredients"));

    auto ingredients = json_result["manifests"][active]["ingredients"];
    EXPECT_EQ(ingredients.size(), 3) << "Should have exactly 3 ingredients";

    // Verify ingredient titles and relationships are preserved
    std::map<std::string, std::string> ingredient_relationships;
    for (const auto& ingredient : ingredients) {
        EXPECT_TRUE(ingredient.contains("title"));
        EXPECT_TRUE(ingredient.contains("relationship"));
        ingredient_relationships[ingredient["title"]] = ingredient["relationship"];
    }

    EXPECT_EQ(ingredient_relationships["Archive 1 Ingredient"], "parentOf")
        << "First ingredient should have parentOf relationship";
    EXPECT_EQ(ingredient_relationships["Archive 2 Ingredient"], "componentOf")
        << "Second ingredient should have componentOf relationship";
    EXPECT_EQ(ingredient_relationships["Archive 3 Ingredient"], "componentOf")
        << "Third ingredient should have componentOf relationship";
}

TEST_F(BuilderTest, WithDefinitionUpdatesManifest) {
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path manifest_path = current_dir / "fixtures/training.json";

    auto manifest = c2pa_test::read_text_file(manifest_path);
    auto context = c2pa::Context();

    c2pa::Builder builder(context);
    builder.with_definition(manifest);

    fs::path asset_path = current_dir / "fixtures/A.jpg";
    fs::path dest_path = get_temp_path("test_with_definition_output.jpg");
    fs::path cert_path = current_dir / "fixtures/es256_certs.pem";
    fs::path key_path = current_dir / "fixtures/es256_private.key";

    auto certs = c2pa_test::read_text_file(cert_path);
    auto private_key = c2pa_test::read_text_file(key_path);
    c2pa::Signer signer("es256", certs, private_key);

    builder.sign(asset_path, dest_path, signer);

    c2pa::Reader reader(context, dest_path);
    auto json_result = json::parse(reader.json());

    std::string active = json_result["active_manifest"];
    auto claim_generator_info = json_result["manifests"][active]["claim_generator_info"];

    // Verify the manifest definition from training.json was applied
    ASSERT_TRUE(claim_generator_info.is_array());
    ASSERT_GT(claim_generator_info.size(), 0);
    EXPECT_EQ(claim_generator_info[0]["name"], "c2pa-c test");
}

TEST_F(BuilderTest, WithDefinitionChaining) {
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path manifest_path = current_dir / "fixtures/training.json";

    auto initial_manifest = R"({
      "claim_generator_info": [
        {
          "name": "initial-value",
          "version": "0.1"
        }
      ]
    })";
    auto updated_manifest = c2pa_test::read_text_file(manifest_path);
    auto context = c2pa::Context();

    c2pa::Builder builder(context, initial_manifest);
    builder.with_definition(updated_manifest);

    // Sign and verify the updated definition was applied
    fs::path asset_path = current_dir / "fixtures/A.jpg";
    fs::path dest_path = get_temp_path("test_with_definition_chaining_output.jpg");
    fs::path cert_path = current_dir / "fixtures/es256_certs.pem";
    fs::path key_path = current_dir / "fixtures/es256_private.key";

    auto certs = c2pa_test::read_text_file(cert_path);
    auto private_key = c2pa_test::read_text_file(key_path);
    c2pa::Signer signer("es256", certs, private_key);

    builder.sign(asset_path, dest_path, signer);

    c2pa::Reader reader(context, dest_path);
    auto json_result = json::parse(reader.json());

    std::string active = json_result["active_manifest"];
    auto claim_generator_info = json_result["manifests"][active]["claim_generator_info"];

    // Verify updated definition (training.json) was applied, not initial
    ASSERT_TRUE(claim_generator_info.is_array());
    ASSERT_GT(claim_generator_info.size(), 0);
    EXPECT_EQ(claim_generator_info[0]["name"], "c2pa-c test");
    EXPECT_NE(claim_generator_info[0]["name"], "initial-value");
}

TEST_F(BuilderTest, WithDefinitionInvalidJsonThrows) {
    auto context = c2pa::Context();
    c2pa::Builder builder(context);

    // Invalid JSON should throw C2paException
    auto invalid_manifest = "{ invalid json ]";
    EXPECT_THROW({
        builder.with_definition(invalid_manifest);
    }, c2pa::C2paException);

    // Builder should still be usable after failed with_definition
    // (the fix ensures old pointer is nulled before checking result,
    // preventing double-free in destructor)
}

TEST_F(BuilderTest, ArchiveToFilePath) {
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path manifest_path = current_dir / "fixtures/training.json";
    fs::path archive_path = get_temp_path("test_archive.c2pa");

    auto manifest = c2pa_test::read_text_file(manifest_path);
    c2pa::Builder builder(manifest);

    builder.to_archive(archive_path);

    EXPECT_TRUE(fs::exists(archive_path));
    EXPECT_GT(fs::file_size(archive_path), 0);
}

TEST_F(BuilderTest, ExtractIngredientsFromArchive) {
  auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

  // Archive each ingredient individually using the dedicated ingredient archive API.
  // Each ingredient is registered under a label, then written to its own archive stream.
  auto archive_ingredient = [&](const std::string& ingredient_json,
                                const std::string& ingredient_id,
                                const fs::path& asset_path) -> std::stringstream {
      auto b = c2pa::Builder(manifest);
      b.add_ingredient(ingredient_json, asset_path);
      std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
      b.write_ingredient_archive(ingredient_id, ss);
      return ss;
  };

  auto archive1 = archive_ingredient(
      R"({"title": "A.jpg", "relationship": "parentOf", "label": "ing-a"})",
      "ing-a",
      c2pa_test::get_fixture_path("A.jpg"));
  auto archive2 = archive_ingredient(
      R"({"title": "C.jpg", "relationship": "componentOf", "label": "ing-c"})",
      "ing-c",
      c2pa_test::get_fixture_path("C.jpg"));
  auto archive3 = archive_ingredient(
      R"({"title": "sample.gif", "relationship": "componentOf", "label": "ing-gif"})",
      "ing-gif",
      c2pa_test::get_fixture_path("sample1.gif"));

  // Add each archived ingredient to the new builder using the archive API.
  auto merged_builder = c2pa::Builder(manifest);
  archive1.seekg(0);
  EXPECT_NO_THROW(merged_builder.add_ingredient_from_archive(archive1));
  archive2.seekg(0);
  EXPECT_NO_THROW(merged_builder.add_ingredient_from_archive(archive2));
  archive3.seekg(0);
  EXPECT_NO_THROW(merged_builder.add_ingredient_from_archive(archive3));

  // Sign and verify
  auto signer = c2pa_test::create_test_signer();
  auto source_path = c2pa_test::get_fixture_path("A.jpg");
  auto output_path = get_temp_path("merged_output.jpg");

  std::vector<unsigned char> manifest_data;
  EXPECT_NO_THROW({
      manifest_data = merged_builder.sign(source_path, output_path, signer);
  });
  ASSERT_FALSE(manifest_data.empty());

  // Read and log the merged builder's manifest JSON
  auto merged_reader = c2pa::Reader(output_path);
  // Verify all 3 ingredients are present in the merged builder
  auto merged_parsed = json::parse(merged_reader.json());
  std::string merged_active = merged_parsed["active_manifest"];
  auto merged_ingredients = merged_parsed["manifests"][merged_active]["ingredients"];
  EXPECT_EQ(merged_ingredients.size(), 3) << "Merged builder should have all 3 ingredients";
}

TEST_F(BuilderTest, ExtractIngredientsFromArchiveToBuilder) {
  auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

  // Helper: register a single ingredient under a label and write its dedicated
  // ingredient archive to a stream.
  auto make_ingredient_archive = [&](const std::string& ingredient_json,
                                     const std::string& ingredient_id,
                                     const fs::path& asset_path) -> std::stringstream {
      auto b = c2pa::Builder(manifest);
      b.add_ingredient(ingredient_json, asset_path);
      std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
      b.write_ingredient_archive(ingredient_id, ss);
      return ss;
  };

  // Archive 1 contains A.jpg and C.jpg (as separate per-ingredient archives)
  auto archive_a = make_ingredient_archive(
      R"({"title": "A.jpg", "relationship": "parentOf", "label": "ing-a"})",
      "ing-a",
      c2pa_test::get_fixture_path("A.jpg"));
  auto archive_c = make_ingredient_archive(
      R"({"title": "C.jpg", "relationship": "componentOf", "label": "ing-c"})",
      "ing-c",
      c2pa_test::get_fixture_path("C.jpg"));

  // Archive 2 contains sample.gif
  auto archive_gif = make_ingredient_archive(
      R"({"title": "sample.gif", "relationship": "componentOf", "label": "ing-gif"})",
      "ing-gif",
      c2pa_test::get_fixture_path("sample1.gif"));

  // Build merged builder by loading each archived ingredient via the archive API.
  // Load twice for "archive 1" group, once for "archive 2" group.
  auto merged_builder = c2pa::Builder(manifest);

  archive_a.seekg(0);
  EXPECT_NO_THROW(merged_builder.add_ingredient_from_archive(archive_a));

  archive_c.seekg(0);
  EXPECT_NO_THROW(merged_builder.add_ingredient_from_archive(archive_c));

  archive_gif.seekg(0);
  EXPECT_NO_THROW(merged_builder.add_ingredient_from_archive(archive_gif));

  // Sign and verify
  auto signer = c2pa_test::create_test_signer();
  auto source_path = c2pa_test::get_fixture_path("A.jpg");
  auto output_path = get_temp_path("merged_output2.jpg");

  std::vector<unsigned char> manifest_data;
  EXPECT_NO_THROW({
      manifest_data = merged_builder.sign(source_path, output_path, signer);
  });
  ASSERT_FALSE(manifest_data.empty());

  auto merged_reader = c2pa::Reader(output_path);
  auto merged_parsed = json::parse(merged_reader.json());
  std::string merged_active = merged_parsed["active_manifest"];
  auto merged_ingredients = merged_parsed["manifests"][merged_active]["ingredients"];
  EXPECT_EQ(merged_ingredients.size(), 3) << "Merged builder should have all 3 ingredients from both archives";
}

// An ingredient's validation state is carried over with the archive
TEST_F(BuilderTest, IngredientArchivePreservesIngredientValidationState) {
  auto read_bytes = [](const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
  };

  // Force a dataHash mismatch in the ingredient
  std::string corrupted = read_bytes(c2pa_test::get_fixture_path("C.jpg"));
  ASSERT_GT(corrupted.size(), 4096u);
  size_t flip_at = corrupted.size() - 1024;
  corrupted[flip_at] = static_cast<char>(corrupted[flip_at] ^ 0xFF);

  auto manifest = read_bytes(c2pa_test::get_fixture_path("training.json"));

  // Archive the failing ingredient as-is
  std::stringstream archive(std::ios::in | std::ios::out | std::ios::binary);
  {
    auto src_builder = c2pa::Builder(manifest);
    std::stringstream corrupt_stream(corrupted,
        std::ios::in | std::ios::out | std::ios::binary);
    src_builder.add_ingredient(
        R"({"title": "C.jpg", "relationship": "componentOf", "label": "ing-bad"})",
        "image/jpeg", corrupt_stream);
    src_builder.write_ingredient_archive("ing-bad", archive);
  }

  auto builder = c2pa::Builder(manifest);
  archive.seekg(0);
  ASSERT_NO_THROW(builder.add_ingredient_from_archive(archive));

  auto signer = c2pa_test::create_test_signer();
  auto output_path = get_temp_path("archive_mismatch_output.jpg");
  ASSERT_NO_THROW(builder.sign(
      c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

  // Verify ingredient state is still here
  auto reader = c2pa::Reader(output_path);
  auto parsed = json::parse(reader.json());
  std::string active = parsed["active_manifest"];
  auto ingredients = parsed["manifests"][active]["ingredients"];
  ASSERT_EQ(ingredients.size(), 1u);

  EXPECT_NE(ingredients[0].dump().find("assertion.dataHash.mismatch"), std::string::npos)
      << "ingredient state was lost during the ingredient-archive round-trip";
}

// An ingredient's validation state is carried over with the archive
TEST_F(BuilderTest, LegacyIngredientArchivePreservesIngredientValidationState) {
  auto read_bytes = [](const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
  };

  // Force a dataHash mismatch in the ingredient
  std::string corrupted = read_bytes(c2pa_test::get_fixture_path("C.jpg"));
  ASSERT_GT(corrupted.size(), 4096u);
  size_t flip_at = corrupted.size() - 1024;
  corrupted[flip_at] = static_cast<char>(corrupted[flip_at] ^ 0xFF);

  auto manifest = read_bytes(c2pa_test::get_fixture_path("training.json"));

  // Archive a builder holding the single failing ingredient (legacy whole-builder archive)
  std::stringstream archive(std::ios::in | std::ios::out | std::ios::binary);
  {
    auto src_builder = c2pa::Builder(manifest);
    std::stringstream corrupt_stream(corrupted,
        std::ios::in | std::ios::out | std::ios::binary);
    src_builder.add_ingredient(
        R"({"title": "C.jpg", "relationship": "componentOf", "label": "ing-bad"})",
        "image/jpeg", corrupt_stream);
    src_builder.to_archive(archive);
  }

  // Add the archive as an ingredient on a new builder, then sign
  auto builder = c2pa::Builder(manifest);
  archive.seekg(0);
  ASSERT_NO_THROW(builder.add_ingredient(
      R"({"title": "C.jpg", "relationship": "componentOf", "label": "ing-bad"})",
      "application/c2pa", archive));

  auto signer = c2pa_test::create_test_signer();
  auto output_path = get_temp_path("legacy_archive_mismatch_output.jpg");
  ASSERT_NO_THROW(builder.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

  // Verify ingredient state is still here
  auto reader = c2pa::Reader(output_path);
  auto parsed = json::parse(reader.json());
  std::string active = parsed["active_manifest"];
  auto ingredients = parsed["manifests"][active]["ingredients"];
  ASSERT_EQ(ingredients.size(), 1u);

  EXPECT_NE(ingredients[0].dump().find("assertion.dataHash.mismatch"), std::string::npos)
      << "ingredient state was lost during the ingredient-archive round-trip";
}

TEST_F(BuilderTest, ExtractIngredientsFromArchives) {
  auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

  // Helper: register a single asset under a label and write its dedicated
  // per-ingredient archive.
  auto make_ingredient_archive = [&](const std::string& ingredient_json,
                                     const std::string& ingredient_id,
                                     const fs::path& asset_path) -> std::stringstream {
      auto b = c2pa::Builder(manifest);
      b.add_ingredient(ingredient_json, asset_path);
      std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
      b.write_ingredient_archive(ingredient_id, ss);
      return ss;
  };

  // "Archive group 1": A.jpg and C.jpg — each gets its own per-ingredient archive
  auto archive_a = make_ingredient_archive(
      R"({"title": "A.jpg", "relationship": "parentOf", "label": "ing-a"})",
      "ing-a",
      c2pa_test::get_fixture_path("A.jpg"));
  auto archive_c = make_ingredient_archive(
      R"({"title": "C.jpg", "relationship": "componentOf", "label": "ing-c"})",
      "ing-c",
      c2pa_test::get_fixture_path("C.jpg"));

  // "Archive group 2": sample1.gif — per-ingredient archive
  auto archive_gif = make_ingredient_archive(
      R"({"title": "sample.gif", "relationship": "componentOf", "label": "ing-gif"})",
      "ing-gif",
      c2pa_test::get_fixture_path("sample1.gif"));

  // Merge all three ingredients into one builder via the dedicated archive API
  auto merged_builder = c2pa::Builder(manifest);

  archive_a.seekg(0);
  EXPECT_NO_THROW(merged_builder.add_ingredient_from_archive(archive_a));

  archive_c.seekg(0);
  EXPECT_NO_THROW(merged_builder.add_ingredient_from_archive(archive_c));

  archive_gif.seekg(0);
  EXPECT_NO_THROW(merged_builder.add_ingredient_from_archive(archive_gif));

  // Sign the merged builder
  auto signer = c2pa_test::create_test_signer();
  auto source_path = c2pa_test::get_fixture_path("A.jpg");
  auto output_path = get_temp_path("merged_from_archives.jpg");

  std::vector<unsigned char> manifest_data;
  EXPECT_NO_THROW({
      manifest_data = merged_builder.sign(source_path, output_path, signer);
  });
  ASSERT_FALSE(manifest_data.empty());

  // Read and verify the merged output
  auto merged_reader = c2pa::Reader(output_path);
  auto merged_json = merged_reader.json();

  // Verify all 3 ingredients are present from both archive groups
  auto merged_parsed = json::parse(merged_json);
  std::string merged_active = merged_parsed["active_manifest"];
  auto merged_ingredients = merged_parsed["manifests"][merged_active]["ingredients"];
  EXPECT_EQ(merged_ingredients.size(), 3) << "Merged builder should have all 3 ingredients from both archives";
}


TEST_F(BuilderTest, ExtractIngredientsFromArchiveLegacy) {
  // Helper function to transfer ingredients from an archive to a new builder
  auto create_builder_with_ingredients_from_archive = [](
      std::istream& archive_stream,
      const std::string& base_manifest_json) -> c2pa::Builder {

      // Create a Reader from the archive
      c2pa::Reader reader("application/c2pa", archive_stream);
      auto json_result = reader.json();

      // Parse the archive JSON to extract ingredient information
      auto parsed = json::parse(json_result);
      std::string active = parsed["active_manifest"];
      auto ingredients = parsed["manifests"][active]["ingredients"];

      // Create a builder with the ingredients injected into the manifest definition
      json manifest_json = json::parse(base_manifest_json);
      manifest_json["ingredients"] = ingredients;
      c2pa::Builder builder(manifest_json.dump());

      // Add all referenced resources for each ingredient
      for (size_t i = 0; i < ingredients.size(); i++) {
          const auto& ingredient = ingredients[i];
          std::string title = ingredient["title"];

          // Copy thumbnail resource if present
          if (ingredient.contains("thumbnail") && ingredient["thumbnail"].contains("identifier")) {
              std::string thumbnail_id = ingredient["thumbnail"]["identifier"];

              std::stringstream thumbnail_stream(std::ios::in | std::ios::out | std::ios::binary);
              reader.get_resource(thumbnail_id, thumbnail_stream);

              thumbnail_stream.seekg(0);
              builder.add_resource(thumbnail_id, thumbnail_stream);
          }

          // Copy manifest_data resource if present (C2PA-signed ingredients)
          if (ingredient.contains("manifest_data") &&
              ingredient["manifest_data"].is_object() &&
              ingredient["manifest_data"].contains("identifier")) {
              std::string manifest_data_id = ingredient["manifest_data"]["identifier"];

              std::stringstream manifest_data_stream(std::ios::in | std::ios::out | std::ios::binary);
              reader.get_resource(manifest_data_id, manifest_data_stream);

              manifest_data_stream.seekg(0);
              builder.add_resource(manifest_data_id, manifest_data_stream);
          }
      }

      return builder;
  };
  auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

  // Create a builder with 3 ingredients: A.jpg, C.jpg, and sample1.gif
  auto builder = c2pa::Builder(manifest);

  std::string ingredient1_json = R"({"title": "A.jpg", "relationship": "parentOf"})";
  builder.add_ingredient(ingredient1_json, c2pa_test::get_fixture_path("A.jpg"));

  std::string ingredient2_json = R"({"title": "C.jpg", "relationship": "componentOf"})";
  builder.add_ingredient(ingredient2_json, c2pa_test::get_fixture_path("C.jpg"));

  std::string ingredient3_json = R"({"title": "sample.gif", "relationship": "componentOf"})";
  builder.add_ingredient(ingredient3_json, c2pa_test::get_fixture_path("sample1.gif"));

  // Archive the builder
  std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
  EXPECT_NO_THROW({
      builder.to_archive(archive_stream);
  });

  // Skip verification since archives have placeholder signatures
  c2pa::load_settings(R"({"verify": {"verify_after_reading": false}})", "json");

  // Use the helper function to create a builder with ingredients from the archive
  archive_stream.seekg(0);
  auto merged_builder = create_builder_with_ingredients_from_archive(archive_stream, manifest);

  // Sign the merged builder
  auto signer = c2pa_test::create_test_signer();
  auto source_path = c2pa_test::get_fixture_path("A.jpg");
  auto output_path = get_temp_path("merged_output_legacy.jpg");

  std::vector<unsigned char> manifest_data;
  EXPECT_NO_THROW({
      manifest_data = merged_builder.sign(source_path, output_path, signer);
  });
  ASSERT_FALSE(manifest_data.empty());

  // Read and log the merged builder's manifest JSON
  auto merged_reader = c2pa::Reader(output_path);
  auto merged_json = merged_reader.json();

  // Verify all 3 ingredients are present in the merged builder
  auto merged_parsed = json::parse(merged_json);
  std::string merged_active = merged_parsed["active_manifest"];
  auto merged_ingredients = merged_parsed["manifests"][merged_active]["ingredients"];
  EXPECT_EQ(merged_ingredients.size(), 3) << "Merged builder should have all 3 ingredients";

  // Reset settings
  c2pa::load_settings(R"({"verify": {"verify_after_reading": true}})", "json");
}

TEST_F(BuilderTest, ExtractIngredientsFromArchiveToBuilderLegacy) {
  auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

  // Helper function that adds ingredients from an archived builder into an existing builder.
  // Can be called multiple times on the same builder to accumulate ingredients from multiple archives.
  auto create_builder_with_archived_ingredients = [&manifest](
      c2pa::Builder& builder,
      std::istream& archive_stream) {

      // Archive the current builder to capture its existing ingredients and resources
      std::stringstream builder_archive(std::ios::in | std::ios::out | std::ios::binary);
      builder.to_archive(builder_archive);
      builder_archive.seekg(0);

      c2pa::Reader builder_reader("application/c2pa", builder_archive);
      auto builder_parsed = json::parse(builder_reader.json());
      std::string builder_active = builder_parsed["active_manifest"];
      json current_ingredients = builder_parsed["manifests"][builder_active].value("ingredients", json::array());

      // Read the incoming archive to get its ingredients
      c2pa::Reader archive_reader("application/c2pa", archive_stream);
      auto archive_parsed = json::parse(archive_reader.json());
      std::string archive_active = archive_parsed["active_manifest"];
      auto new_ingredients = archive_parsed["manifests"][archive_active]["ingredients"];

      // Collect all resource identifiers already in use by current ingredients
      std::set<std::string> used_ids;
      for (const auto& ing : current_ingredients) {
          if (ing.contains("thumbnail") && ing["thumbnail"].contains("identifier"))
              used_ids.insert(ing["thumbnail"]["identifier"].get<std::string>());
          if (ing.contains("manifest_data") && ing["manifest_data"].is_object() && ing["manifest_data"].contains("identifier"))
              used_ids.insert(ing["manifest_data"]["identifier"].get<std::string>());
      }

      // Helper to generate a unique identifier by appending/incrementing a __N suffix
      int suffix_counter = static_cast<int>(current_ingredients.size());
      auto make_unique_id = [&](const std::string& original_id) -> std::string {
          if (used_ids.find(original_id) == used_ids.end()) {
              used_ids.insert(original_id);
              return original_id;
          }
          // Strip any existing __N suffix to get the base identifier
          std::string base = original_id;
          auto pos = base.rfind("__");
          if (pos != std::string::npos && pos + 2 < base.size()) {
              bool all_digits = std::all_of(base.begin() + static_cast<long>(pos) + 2, base.end(), ::isdigit);
              if (all_digits) base = base.substr(0, pos);
          }
          std::string new_id;
          do {
              new_id = base + "__" + std::to_string(suffix_counter++);
          } while (used_ids.find(new_id) != used_ids.end());
          used_ids.insert(new_id);
          return new_id;
      };

      // Remap conflicting identifiers in new_ingredients and collect (source_id, dest_id) pairs
      // for resource copying. source_id is the original ID in the archive reader, dest_id is the
      // (possibly remapped) ID that goes into the new builder.
      struct ResourceCopy { std::string source_id; std::string dest_id; };
      std::vector<ResourceCopy> new_resource_copies;

      for (auto& ingredient : new_ingredients) {
          if (ingredient.contains("thumbnail") && ingredient["thumbnail"].contains("identifier")) {
              std::string old_id = ingredient["thumbnail"]["identifier"].get<std::string>();
              std::string new_id = make_unique_id(old_id);
              if (new_id != old_id) {
                  ingredient["thumbnail"]["identifier"] = new_id;
              }
              new_resource_copies.push_back({old_id, new_id});
          }
          if (ingredient.contains("manifest_data") &&
              ingredient["manifest_data"].is_object() &&
              ingredient["manifest_data"].contains("identifier")) {
              std::string old_id = ingredient["manifest_data"]["identifier"].get<std::string>();
              std::string new_id = make_unique_id(old_id);
              if (new_id != old_id) {
                  ingredient["manifest_data"]["identifier"] = new_id;
              }
              new_resource_copies.push_back({old_id, new_id});
          }
      }

      // Merge all ingredients (current + remapped new)
      json all_ingredients = current_ingredients;
      for (const auto& ing : new_ingredients) {
          all_ingredients.push_back(ing);
      }

      // Create a new builder with the merged ingredients injected into the manifest definition
      json manifest_json = json::parse(manifest);
      manifest_json["ingredients"] = all_ingredients;
      c2pa::Builder new_builder(manifest_json.dump());

      // Copy resources from the current builder's archive (existing ingredients, no remapping needed)
      for (const auto& ingredient : current_ingredients) {
          if (ingredient.contains("thumbnail") && ingredient["thumbnail"].contains("identifier")) {
              std::string id = ingredient["thumbnail"]["identifier"];
              std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
              builder_reader.get_resource(id, stream);
              stream.seekg(0);
              new_builder.add_resource(id, stream);
          }
          if (ingredient.contains("manifest_data") &&
              ingredient["manifest_data"].is_object() &&
              ingredient["manifest_data"].contains("identifier")) {
              std::string id = ingredient["manifest_data"]["identifier"];
              std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
              builder_reader.get_resource(id, stream);
              stream.seekg(0);
              new_builder.add_resource(id, stream);
          }
      }

      // Copy resources from the new archive (using source_id to read, dest_id to write)
      for (const auto& rc : new_resource_copies) {
          std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
          archive_reader.get_resource(rc.source_id, stream);
          stream.seekg(0);
          new_builder.add_resource(rc.dest_id, stream);
      }

      // Replace the original builder with the new one via move assignment
      builder = std::move(new_builder);
  };

  // Archive 1: A.jpg and C.jpg
  auto builder1 = c2pa::Builder(manifest);
  builder1.add_ingredient(R"({"title": "A.jpg", "relationship": "parentOf"})",
                          c2pa_test::get_fixture_path("A.jpg"));
  builder1.add_ingredient(R"({"title": "C.jpg", "relationship": "componentOf"})",
                          c2pa_test::get_fixture_path("C.jpg"));
  std::stringstream archive1_stream(std::ios::in | std::ios::out | std::ios::binary);
  EXPECT_NO_THROW({
      builder1.to_archive(archive1_stream);
  });

  // Archive 2: sample1.gif
  auto builder2 = c2pa::Builder(manifest);
  builder2.add_ingredient(R"({"title": "sample.gif", "relationship": "componentOf"})",
      c2pa_test::get_fixture_path("sample1.gif"));
  std::stringstream archive2_stream(std::ios::in | std::ios::out | std::ios::binary);
  EXPECT_NO_THROW({
      builder2.to_archive(archive2_stream);
  });

  // Skip verification since archives have placeholder signatures
  c2pa::load_settings(R"({"verify": {"verify_after_reading": false}})", "json");

  auto merged_builder = c2pa::Builder(manifest);

  // First call: add ingredients from archive 1 (A.jpg, C.jpg)
  archive1_stream.seekg(0);
  create_builder_with_archived_ingredients(merged_builder, archive1_stream);

  // Second call: add ingredients from archive 2 (sample.gif) to the same builder
  archive2_stream.seekg(0);
  create_builder_with_archived_ingredients(merged_builder, archive2_stream);

  // Sign the merged builder
  auto signer = c2pa_test::create_test_signer();
  auto source_path = c2pa_test::get_fixture_path("A.jpg");
  auto output_path = get_temp_path("merged_output2_legacy.jpg");

  std::vector<unsigned char> manifest_data;
  EXPECT_NO_THROW({
      manifest_data = merged_builder.sign(source_path, output_path, signer);
  });
  ASSERT_FALSE(manifest_data.empty());

  // Read and log the merged builder's manifest JSON
  auto merged_reader = c2pa::Reader(output_path);
  auto merged_json = merged_reader.json();

  // Verify all 3 ingredients are present from both archives
  auto merged_parsed = json::parse(merged_json);
  std::string merged_active = merged_parsed["active_manifest"];
  auto merged_ingredients = merged_parsed["manifests"][merged_active]["ingredients"];
  EXPECT_EQ(merged_ingredients.size(), 3) << "Merged builder should have all 3 ingredients from both archives";

  // Reset settings
  c2pa::load_settings(R"({"verify": {"verify_after_reading": true}})", "json");
}

TEST_F(BuilderTest, ExtractIngredientsFromArchivesLegacy) {
  auto create_builder_with_ingredients_from_archives = [](
      std::vector<std::reference_wrapper<std::istream>>& archive_streams,
      const std::string& base_manifest_json) -> c2pa::Builder {

      // Collect ingredients and their associated readers from all archives.
      // Each entry pairs an ingredient JSON (possibly with remapped identifiers)
      // with a pointer to the Reader it came from, so we can copy resources later.
      struct IngredientSource {
          json ingredient;          // ingredient JSON (with remapped IDs if needed)
          std::string source_id;    // original resource ID in the archive (for thumbnail)
          std::string dest_id;      // destination resource ID in the builder (possibly remapped)
          std::string md_source_id; // original manifest_data ID (empty if none)
          std::string md_dest_id;   // destination manifest_data ID (possibly remapped)
      };

      // Keep readers alive for the duration of resource copying
      std::vector<std::unique_ptr<c2pa::Reader>> readers;
      std::vector<std::vector<IngredientSource>> per_archive_sources;
      json all_ingredients = json::array();

      // Track used resource identifiers across all archives to avoid collisions
      std::set<std::string> used_ids;
      int suffix_counter = 0;

      auto make_unique_id = [&](const std::string& original_id) -> std::string {
          if (used_ids.find(original_id) == used_ids.end()) {
              used_ids.insert(original_id);
              return original_id;
          }
          std::string base = original_id;
          auto pos = base.rfind("__");
          if (pos != std::string::npos && pos + 2 < base.size()) {
              bool all_digits = std::all_of(base.begin() + static_cast<long>(pos) + 2, base.end(), ::isdigit);
              if (all_digits) base = base.substr(0, pos);
          }
          std::string new_id;
          do {
              new_id = base + "__" + std::to_string(suffix_counter++);
          } while (used_ids.find(new_id) != used_ids.end());
          used_ids.insert(new_id);
          return new_id;
      };

      // Process each archive: extract ingredients, remap conflicting IDs
      for (auto& stream_ref : archive_streams) {
          auto reader = std::make_unique<c2pa::Reader>("application/c2pa", stream_ref.get());
          auto parsed = json::parse(reader->json());
          std::string active = parsed["active_manifest"];
          auto ingredients = parsed["manifests"][active]["ingredients"];

          std::vector<IngredientSource> sources;
          for (auto& ingredient : ingredients) {
              IngredientSource src;

              // Remap thumbnail identifier if it conflicts
              if (ingredient.contains("thumbnail") && ingredient["thumbnail"].contains("identifier")) {
                  std::string old_id = ingredient["thumbnail"]["identifier"].get<std::string>();
                  std::string new_id = make_unique_id(old_id);
                  if (new_id != old_id) {
                      ingredient["thumbnail"]["identifier"] = new_id;
                  }
                  src.source_id = old_id;
                  src.dest_id = new_id;
              }

              // Remap manifest_data identifier if it conflicts
              if (ingredient.contains("manifest_data") &&
                  ingredient["manifest_data"].is_object() &&
                  ingredient["manifest_data"].contains("identifier")) {
                  std::string old_id = ingredient["manifest_data"]["identifier"].get<std::string>();
                  std::string new_id = make_unique_id(old_id);
                  if (new_id != old_id) {
                      ingredient["manifest_data"]["identifier"] = new_id;
                  }
                  src.md_source_id = old_id;
                  src.md_dest_id = new_id;
              }

              src.ingredient = ingredient;
              all_ingredients.push_back(ingredient);
              sources.push_back(std::move(src));
          }

          per_archive_sources.push_back(std::move(sources));
          readers.push_back(std::move(reader));
      }

      // Create the builder with all ingredients injected into the manifest definition
      json manifest_json = json::parse(base_manifest_json);
      manifest_json["ingredients"] = all_ingredients;
      c2pa::Builder builder(manifest_json.dump());

      // Copy all resources from each archive into the builder
      for (size_t a = 0; a < readers.size(); a++) {
          for (const auto& src : per_archive_sources[a]) {
              // Copy thumbnail
              if (!src.source_id.empty()) {
                  std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
                  readers[a]->get_resource(src.source_id, stream);
                  stream.seekg(0);
                  builder.add_resource(src.dest_id, stream);
              }
              // Copy manifest_data
              if (!src.md_source_id.empty()) {
                  std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
                  readers[a]->get_resource(src.md_source_id, stream);
                  stream.seekg(0);
                  builder.add_resource(src.md_dest_id, stream);
              }
          }
      }

      return builder;
  };

  auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

  // Archive 1: A.jpg and C.jpg
  auto builder1 = c2pa::Builder(manifest);
  builder1.add_ingredient(R"({"title": "A.jpg", "relationship": "parentOf"})", c2pa_test::get_fixture_path("A.jpg"));
  builder1.add_ingredient(R"({"title": "C.jpg", "relationship": "componentOf"})", c2pa_test::get_fixture_path("C.jpg"));
  std::stringstream archive1_stream(std::ios::in | std::ios::out | std::ios::binary);
  EXPECT_NO_THROW({ builder1.to_archive(archive1_stream); });

  // Archive 2: sample1.gif
  auto builder2 = c2pa::Builder(manifest);
  builder2.add_ingredient(R"({"title": "sample.gif", "relationship": "componentOf"})", c2pa_test::get_fixture_path("sample1.gif"));
  std::stringstream archive2_stream(std::ios::in | std::ios::out | std::ios::binary);
  EXPECT_NO_THROW({
      builder2.to_archive(archive2_stream);
  });

  // Skip verification since archives have placeholder signatures
  c2pa::load_settings(R"({"verify": {"verify_after_reading": false}})", "json");

  // Use the helper to create a builder from both archives at once
  archive1_stream.seekg(0);
  archive2_stream.seekg(0);
  std::vector<std::reference_wrapper<std::istream>> archives = { archive1_stream, archive2_stream };
  auto merged_builder = create_builder_with_ingredients_from_archives(archives, manifest);

  // Sign the merged builder
  auto signer = c2pa_test::create_test_signer();
  auto source_path = c2pa_test::get_fixture_path("A.jpg");
  auto output_path = get_temp_path("merged_from_archives_legacy.jpg");

  std::vector<unsigned char> manifest_data;
  EXPECT_NO_THROW({
      manifest_data = merged_builder.sign(source_path, output_path, signer);
  });
  ASSERT_FALSE(manifest_data.empty());

  // Read and verify the merged output
  auto merged_reader = c2pa::Reader(output_path);
  auto merged_json = merged_reader.json();

  // Verify all 3 ingredients are present from both archives
  auto merged_parsed = json::parse(merged_json);
  std::string merged_active = merged_parsed["active_manifest"];
  auto merged_ingredients = merged_parsed["manifests"][merged_active]["ingredients"];
  EXPECT_EQ(merged_ingredients.size(), 3) << "Merged builder should have all 3 ingredients from both archives";

  // Reset settings
  c2pa::load_settings(R"({"verify": {"verify_after_reading": true}})", "json");
}

TEST_F(BuilderTest, AddMultipleActions)
{
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest);

    builder.add_action(R"({
        "action": "c2pa.color_adjustments",
        "parameters": { "name": "brightnesscontrast" }
    })");

    builder.add_action(R"({
        "action": "c2pa.filtered",
        "parameters": { "name": "A filter" },
        "description": "Filtering applied"
    })");

    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("multiple_actions.jpg");

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(source_path, output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    string active = parsed["active_manifest"];

    bool found_color = false, found_filter = false;
    for (auto& assertion : parsed["manifests"][active]["assertions"]) {
        if (assertion["label"] == "c2pa.actions.v2") {
            for (auto& action : assertion["data"]["actions"]) {
                if (action["action"] == "c2pa.color_adjustments") found_color = true;
                if (action["action"] == "c2pa.filtered") {
                    found_filter = true;
                    EXPECT_EQ(action["description"], "Filtering applied");
                }
            }
        }
    }
    EXPECT_TRUE(found_color) << "c2pa.color_adjustments not found";
    EXPECT_TRUE(found_filter) << "c2pa.filtered not found";
}

TEST_F(BuilderTest, LinkActionToIngredient)
{
    auto context = c2pa::Context();

    std::string instance_id = "xmp:iid:939a4c48-0dff-44ec-8f95-61f52b11618f";

    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions"},
                {"data", {
                    {"actions", json::array({
                        {
                            {"action", "c2pa.created"},
                            {"digitalSourceType", "http://c2pa.org/digitalsourcetype/compositeCapture"}
                        },
                        {
                            {"action", "c2pa.placed"},
                            {"description", "Placed image into composition"},
                            {"parameters", {
                                {"ingredientIds", json::array({instance_id})}
                            }}
                        }
                    })}
                }}
            }
        })}
    };

    auto builder = c2pa::Builder(context, manifest_json.dump());

    json ingredient = {
        {"title", "source_asset.jpg"},
        {"relationship", "componentOf"},
        {"instance_id", instance_id}
    };
    builder.add_ingredient(ingredient.dump(), c2pa_test::get_fixture_path("A.jpg"));

    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("link_action.jpg");

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(source_path, output_path, signer));

    // Read back and verify the ingredientIds were resolved to JUMBF URLs
    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    string active = parsed["active_manifest"];

    bool found_linked_action = false;
    for (auto& assertion : parsed["manifests"][active]["assertions"]) {
        if (assertion["label"] == "c2pa.actions.v2") {
            for (auto& action : assertion["data"]["actions"]) {
                if (action["action"] == "c2pa.placed" &&
                    action.contains("parameters") &&
                    action["parameters"].contains("ingredients")) {
                    auto& ings = action["parameters"]["ingredients"];
                    ASSERT_GE(ings.size(), 1u);
                    EXPECT_TRUE(ings[0].contains("url"));
                    // URL should be a JUMBF path
                    std::string url = ings[0]["url"];
                    EXPECT_TRUE(url.find("c2pa.ingredient") != std::string::npos)
                        << "URL should reference an ingredient assertion, got: " << url;
                    found_linked_action = true;
                }
            }
        }
    }
    EXPECT_TRUE(found_linked_action) << "Linked c2pa.placed action not found";
}

TEST_F(BuilderTest, ExtractIngredientsFromArchiveAndReuse)
{
    auto context = c2pa::Context();
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // Step 1: Build a working store with ingredients and archive it
    auto builder = c2pa::Builder(context, manifest);
    builder.add_ingredient(R"({"title": "A.jpg", "relationship": "componentOf"})",
                           c2pa_test::get_fixture_path("A.jpg"));
    builder.add_ingredient(R"({"title": "C.jpg", "relationship": "componentOf"})",
                           c2pa_test::get_fixture_path("C.jpg"));

    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(builder.to_archive(archive_stream));

    // Step 2: Read the archive and extract only one of the 2 ingredients
    archive_stream.seekg(0);
    c2pa::Reader reader(context, "application/c2pa", archive_stream);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto ingredients = parsed["manifests"][active]["ingredients"];

    ASSERT_GE(ingredients.size(), 2u) << "Archive should have 2 ingredients";

    // Pick only ingredient with title A.jpg (this is one way of filtering)
    json selected = json::array();
    for (auto& ingredient : ingredients) {
        if (ingredient["title"] == "A.jpg") {
            selected.push_back(ingredient);
        }
    }

    // Step 3: Create a new Builder with only selected ingredients
    json new_manifest = json::parse(manifest);
    new_manifest["ingredients"] = selected;
    auto new_builder = c2pa::Builder(context, new_manifest.dump());

    // Transfer binary resources
    for (auto& ingredient : selected) {
        if (ingredient.contains("thumbnail") && ingredient["thumbnail"].contains("identifier")) {
            std::string id = ingredient["thumbnail"]["identifier"];
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            reader.get_resource(id, stream);
            stream.seekg(0);
            new_builder.add_resource(id, stream);
        }
        if (ingredient.contains("manifest_data") &&
            ingredient["manifest_data"].is_object() &&
            ingredient["manifest_data"].contains("identifier")) {
            std::string id = ingredient["manifest_data"]["identifier"];
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            reader.get_resource(id, stream);
            stream.seekg(0);
            new_builder.add_resource(id, stream);
        }
    }

    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("extracted_ingredient.jpg");

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = new_builder.sign(source_path, output_path, signer));

    // Verify the output has only 1 ingredient
    auto out_reader = c2pa::Reader(context, output_path);
    auto out_parsed = json::parse(out_reader.json());
    std::string out_active = out_parsed["active_manifest"];
    auto out_ingredients = out_parsed["manifests"][out_active]["ingredients"];
    EXPECT_EQ(out_ingredients.size(), 1u) << "Output should have exactly 1 ingredient";
    EXPECT_EQ(out_ingredients[0]["title"], "A.jpg");
}

TEST_F(BuilderTest, FilterActionsWithLinkedIngredients)
{
    // When filtering, we keep c2pa.created (required by spec)
    // and c2pa.placed (wanted) + its ingredient
    auto context = c2pa::Context();

    std::string instance_id = "xmp:iid:abcd1234-ef56-7890-abcd-ef1234567890";

    // Build a manifest with c2pa.created (spec mandatory),
    // c2pa.placed (linked),
    // and c2pa.filtered (to be dropped)
    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions"},
                {"data", {
                    {"actions", json::array({
                        {
                            {"action", "c2pa.created"},
                            {"digitalSourceType", "http://c2pa.org/digitalsourcetype/compositeCapture"}
                        },
                        {
                            {"action", "c2pa.placed"},
                            {"description", "Placed an image"},
                            {"parameters", {
                                {"ingredientIds", json::array({instance_id})}
                            }}
                        },
                        {
                            {"action", "c2pa.filtered"},
                            {"parameters", {{"name", "blur"}}}
                        }
                    })}
                }}
            }
        })}
    };

    auto builder = c2pa::Builder(context, manifest_json.dump());

    json ingredient = {
        {"title", "asset.jpg"},
        {"relationship", "componentOf"},
        {"instance_id", instance_id}
    };
    builder.add_ingredient(ingredient.dump(), c2pa_test::get_fixture_path("A.jpg"));

    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto signed_path = get_temp_path("linked_source.jpg");

    ASSERT_NO_THROW(builder.sign(source_path, signed_path, signer));

    // Read the signed asset and filter:
    // Keep c2pa.created (required),
    // keep c2pa.placed (wanted with ingredient),
    // drop c2pa.filtered
    auto reader = c2pa::Reader(context, signed_path);
    auto parsed = json::parse(reader.json());
    string active = parsed["active_manifest"];
    auto source_manifest = parsed["manifests"][active];

    // Filter actions and track needed ingredients
    json kept_actions = json::array();
    std::set<std::string> needed_labels;

    for (auto& assertion : source_manifest["assertions"]) {
        if (assertion["label"] == "c2pa.actions.v2") {
            for (auto& action : assertion["data"]["actions"]) {
                std::string action_type = action["action"];
                // Always keep required actions (c2pa.created / c2pa.opened)
                if (action_type == "c2pa.created" || action_type == "c2pa.opened") {
                    kept_actions.push_back(action);
                }
                // Keep c2pa.placed (wanted) and track its linked ingredients
                else if (action_type == "c2pa.placed") {
                    kept_actions.push_back(action);
                    if (action.contains("parameters") &&
                        action["parameters"].contains("ingredients")) {
                        for (auto& ref : action["parameters"]["ingredients"]) {
                            std::string url = ref["url"];
                            std::string label = url.substr(url.rfind('/') + 1);
                            needed_labels.insert(label);
                        }
                    }
                }
                // Drop everything else (c2pa.filtered, etc.)
            }
        }
    }

    ASSERT_GE(kept_actions.size(), 2u) << "Should have kept c2pa.created + c2pa.placed";
    ASSERT_FALSE(needed_labels.empty()) << "c2pa.placed should reference at least one ingredient";

    // Keep only ingredients referenced by kept actions
    json kept_ingredients = json::array();
    for (auto& ing : source_manifest["ingredients"]) {
        if (ing.contains("label") && needed_labels.count(ing["label"])) {
            kept_ingredients.push_back(ing);
        }
    }
    ASSERT_FALSE(kept_ingredients.empty()) << "Should have kept at least one ingredient";

    // Build the new manifest
    json new_manifest = json::parse(R"({
        "claim_generator_info": [{"name": "c2pa-test", "version": "1.0"}]
    })");
    new_manifest["ingredients"] = kept_ingredients;
    new_manifest["assertions"] = json::array({
        {
            {"label", "c2pa.actions"},
            {"data", {{"actions", kept_actions}}}
        }
    });

    auto new_builder = c2pa::Builder(context, new_manifest.dump());

    // Transfer binary resources for kept ingredients
    for (auto& ing : kept_ingredients) {
        if (ing.contains("thumbnail") && ing["thumbnail"].contains("identifier")) {
            std::string id = ing["thumbnail"]["identifier"];
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            reader.get_resource(id, stream);
            stream.seekg(0);
            new_builder.add_resource(id, stream);
        }
        if (ing.contains("manifest_data") &&
            ing["manifest_data"].is_object() &&
            ing["manifest_data"].contains("identifier")) {
            std::string id = ing["manifest_data"]["identifier"];
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            reader.get_resource(id, stream);
            stream.seekg(0);
            new_builder.add_resource(id, stream);
        }
    }

    auto output_path = get_temp_path("filtered_linked.jpg");
    ASSERT_NO_THROW(new_builder.sign(source_path, output_path, signer));

    // Verify: output has c2pa.created + c2pa.placed but NOT c2pa.filtered
    auto out_reader = c2pa::Reader(context, output_path);
    auto out_parsed = json::parse(out_reader.json());
    string out_active = out_parsed["active_manifest"];

    bool found_created = false, found_placed = false;
    for (auto& assertion : out_parsed["manifests"][out_active]["assertions"]) {
        if (assertion["label"] == "c2pa.actions.v2") {
            for (auto& action : assertion["data"]["actions"]) {
                if (action["action"] == "c2pa.created") found_created = true;
                if (action["action"] == "c2pa.placed") found_placed = true;
                EXPECT_NE(action["action"], "c2pa.filtered")
                    << "c2pa.filtered should have been filtered out";
            }
        }
    }
    EXPECT_TRUE(found_created) << "c2pa.created must be present (required for valid v2)";
    EXPECT_TRUE(found_placed) << "c2pa.placed should be in the output";

    // Verify the output has the linked ingredient
    auto out_ingredients = out_parsed["manifests"][out_active]["ingredients"];
    EXPECT_GE(out_ingredients.size(), 1u) << "Should have at least 1 ingredient";
}

TEST_F(BuilderTest, WalkTree)
{
    auto context = c2pa::Context();
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");

    // Layer 1: Create a new asset with c2pa.created + c2pa.color_adjustments
    auto builder1 = c2pa::Builder(context, manifest);
    builder1.add_action(R"({"action": "c2pa.color_adjustments", "parameters": {"name": "contrast"}})");
    auto signed1_path = get_temp_path("tree_layer1.jpg");
    ASSERT_NO_THROW(builder1.sign(source_path, signed1_path, signer));

    // Layer 2: Open layer1 (c2pa.opened + parentOf ingredient) and add c2pa.filtered
    std::string instance_id = "tree-layer1";
    json layer2_manifest = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions"},
                {"data", {
                    {"actions", json::array({
                        {
                            {"action", "c2pa.opened"},
                            {"parameters", {
                                {"ingredientIds", json::array({instance_id})}
                            }}
                        }
                    })}
                }}
            }
        })}
    };
    auto builder2 = c2pa::Builder(context, layer2_manifest.dump());
    builder2.add_action(R"({"action": "c2pa.filtered", "parameters": {"name": "sharpen"}})");
    builder2.add_ingredient(
        json({{"title", "layer1.jpg"}, {"relationship", "parentOf"}, {"instance_id", instance_id}}).dump(),
        signed1_path);
    auto signed2_path = get_temp_path("tree_layer2.jpg");
    ASSERT_NO_THROW(builder2.sign(source_path, signed2_path, signer));

    // Read the final asset and walk the tree
    auto reader = c2pa::Reader(context, signed2_path);
    auto parsed = json::parse(reader.json());
    string active = parsed["active_manifest"];
    auto active_manifest = parsed["manifests"][active];

    // The active manifest should have c2pa.opened and c2pa.filtered
    bool found_opened = false, found_filtered = false;
    for (auto& assertion : active_manifest["assertions"]) {
        if (assertion["label"] == "c2pa.actions.v2") {
            for (auto& action : assertion["data"]["actions"]) {
                if (action["action"] == "c2pa.opened") found_opened = true;
                if (action["action"] == "c2pa.filtered") found_filtered = true;
            }
        }
    }
    EXPECT_TRUE(found_opened) << "Active manifest should have c2pa.opened";
    EXPECT_TRUE(found_filtered) << "Active manifest should have c2pa.filtered";

    // Walk into the ingredient's manifest (the tree)
    ASSERT_TRUE(active_manifest.contains("ingredients"));
    ASSERT_FALSE(active_manifest["ingredients"].empty());

    bool found_ingredient_with_manifest = false;
    for (auto& ingredient : active_manifest["ingredients"]) {
        if (ingredient.contains("active_manifest")) {
            found_ingredient_with_manifest = true;
            std::string ing_label = ingredient["active_manifest"];

            // The ingredient's manifest should be in the flat manifests dictionary
            ASSERT_TRUE(parsed["manifests"].contains(ing_label))
                << "Ingredient manifest should be in the manifests dictionary";

            auto ing_manifest = parsed["manifests"][ing_label];

            // The ingredient's manifest should have c2pa.created and c2pa.color_adjustments
            bool found_created = false, found_contrast = false;
            for (auto& assertion : ing_manifest["assertions"]) {
                if (assertion["label"] == "c2pa.actions.v2") {
                    for (auto& action : assertion["data"]["actions"]) {
                        if (action["action"] == "c2pa.created") found_created = true;
                        if (action["action"] == "c2pa.color_adjustments") {
                            found_contrast = true;
                            EXPECT_EQ(action["parameters"]["name"], "contrast");
                        }
                    }
                }
            }
            EXPECT_TRUE(found_created)
                << "Ingredient manifest should have c2pa.created from layer 1";
            EXPECT_TRUE(found_contrast)
                << "Ingredient manifest should have c2pa.color_adjustments from layer 1";
        }
    }
    EXPECT_TRUE(found_ingredient_with_manifest)
        << "Should have at least one ingredient with its own manifest (tree structure)";
}

TEST_F(BuilderTest, AddIngredientsUsingLabelParentOfComponentOf)
{
    auto context = c2pa::Context();
    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");

    // The labels used for linking may not be the ones that will be in the
    // manifest. They are just indicators on which ingredient to link with what.
    auto manifest_json = R"(
    {
        "claim_generator_info": [{ "name": "TestAddIngredientsUsingLabelParentOfComponentOf", "version": "0.1" }],
        "assertions": [
            {
                "label": "c2pa.actions.v2",
                "data": {
                    "actions": [
                        {
                            "action": "c2pa.opened",
                            "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation",
                            "parameters": {
                                "ingredientIds": ["c2pa.ingredient.v3_1"]
                            }
                        },
                        {
                            "action": "c2pa.placed",
                            "parameters": {
                                "ingredientIds": ["c2pa.ingredient.v3_2"]
                            }
                        }
                    ]
                }
            }
        ]
    }
    )";

    auto builder = c2pa::Builder(context, manifest_json);

    auto ingredient_json_1 = R"(
    {
        "title": "C.jpg",
        "format": "image/jpeg",
        "relationship": "parentOf",
        "label": "c2pa.ingredient.v3_1"
    }
    )";

    auto ingredient_json_2 = R"(
    {
        "title": "A.jpg",
        "format": "image/jpeg",
        "relationship": "componentOf",
        "label": "c2pa.ingredient.v3_2"
    }
    )";

    builder.add_ingredient(ingredient_json_1, c2pa_test::get_fixture_path("C.jpg"));
    builder.add_ingredient(ingredient_json_2, c2pa_test::get_fixture_path("A.jpg"));

    auto output_path = get_temp_path("signed_labelled_ingredient_3.jpg");
    ASSERT_NO_THROW(builder.sign(source_path, output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto reader_json = reader.json();
    ASSERT_TRUE(reader_json.find("TestAddIngredientsUsingLabelParentOfComponentOf") != std::string::npos);

    // Verify both ingredients are present with correct relationships
    auto parsed = json::parse(reader_json);
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_GE(ingredients.size(), 2u);

    // Find each ingredient by title and check relationship
    bool found_parent = false, found_component = false;
    for (auto& ing : ingredients) {
        if (ing["title"] == "C.jpg") {
            EXPECT_EQ(ing["relationship"], "parentOf");
            found_parent = true;
        }
        if (ing["title"] == "A.jpg") {
            EXPECT_EQ(ing["relationship"], "componentOf");
            found_component = true;
        }
    }
    EXPECT_TRUE(found_parent) << "Should have parentOf ingredient (C.jpg)";
    EXPECT_TRUE(found_component) << "Should have componentOf ingredient (A.jpg)";

    // Verify both actions resolved their ingredientIds
    for (auto& assertion : parsed["manifests"][active]["assertions"]) {
        if (assertion["label"] == "c2pa.actions.v2") {
            for (auto& action : assertion["data"]["actions"]) {
                if (action["action"] == "c2pa.opened" || action["action"] == "c2pa.placed") {
                    ASSERT_TRUE(action.contains("parameters"));
                    ASSERT_TRUE(action["parameters"].contains("ingredients"))
                        << action["action"].get<std::string>()
                        << " should have resolved ingredientIds to ingredients[] URLs";
                }
            }
        }
    }
}

TEST_F(BuilderTest, AddIngredientFromArchiveWithOverride)
{
    auto context = c2pa::Context();
    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");

    // Step 1: Create a Builder with an ingredient titled "my-first-title",
    //         then archive it to a .c2pa file (ingredient archive)
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto builder1 = c2pa::Builder(context, manifest_str);
    builder1.add_ingredient(
        R"({"title": "my-first-title", "relationship": "componentOf"})",
        c2pa_test::get_fixture_path("A.jpg"));

    auto archive_path = get_temp_path("example-ingredient.c2pa");
    ASSERT_NO_THROW(builder1.to_archive(archive_path));
    ASSERT_TRUE(fs::exists(archive_path));

    // Step 2: Create a new Builder and add the ingredient from the .c2pa archive,
    // but override the title to "overridden-title"
    auto builder2 = c2pa::Builder(context, manifest_str);
    json ingredient_override = {
        {"title", "overridden-title"},
        {"relationship", "parentOf"}
    };
    builder2.add_ingredient(ingredient_override.dump(), archive_path);

    auto output_path = get_temp_path("archive_override_result.jpg");
    ASSERT_NO_THROW(builder2.sign(source_path, output_path, signer));

    // Step 3: Read back and verify which title ended up in the signed manifest
    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto ingredients = parsed["manifests"][active]["ingredients"];

    ASSERT_GE(ingredients.size(), 1u);
    auto& ing = ingredients[0];

    // Check whether the override title or the original archive title won
    std::string actual_title = ing["title"];
    std::string actual_relationship = ing["relationship"];

    // Verify the override title is what ends up in the signed manifest
    EXPECT_EQ(actual_title, "overridden-title")
        << "Title from add_ingredient JSON should override the archive's original title";
    EXPECT_EQ(actual_relationship, "parentOf")
        << "Relationship from add_ingredient JSON should override the archive's original relationship";
}

TEST_F(BuilderTest, AddIngredientFromArchiveWithCustomProperties)
{
    auto context = c2pa::Context();
    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");

    // Step 1: Create an ingredient archive with a basic ingredient
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto builder1 = c2pa::Builder(context, manifest_str);
    builder1.add_ingredient(
        R"({"title": "original.jpg", "relationship": "componentOf"})",
        c2pa_test::get_fixture_path("A.jpg"));

    auto archive_path = get_temp_path("custom-props-ingredient.c2pa");
    ASSERT_NO_THROW(builder1.to_archive(archive_path));

    // Step 2: Add the archived ingredient with overridden + additional custom properties
    std::string custom_instance = "tracking:proj-7:asset-42";
    json ingredient_with_custom_props = {
        {"title", "renamed-asset.jpg"},
        {"relationship", "parentOf"},
        {"instance_id", custom_instance},
        {"description", "Overridden ingredient with extra properties"},
        {"informational_URI", "https://example.com/assets/42"}
    };

    auto builder2 = c2pa::Builder(context, manifest_str);
    builder2.add_ingredient(ingredient_with_custom_props.dump(), archive_path);

    auto output_path = get_temp_path("archive_custom_props_result.jpg");
    ASSERT_NO_THROW(builder2.sign(source_path, output_path, signer));

    // Step 3: Verify all properties survived signing
    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto ingredients = parsed["manifests"][active]["ingredients"];

    ASSERT_GE(ingredients.size(), 1u);
    auto& ing = ingredients[0];

    EXPECT_EQ(ing["title"], "renamed-asset.jpg")
        << "Title override should be preserved";
    EXPECT_EQ(ing["relationship"], "parentOf")
        << "Relationship override should be preserved";

    // Check which additional custom properties survived
    if (ing.contains("instance_id")) {
        EXPECT_EQ(ing["instance_id"], custom_instance);
    }
    if (ing.contains("description")) {
        EXPECT_EQ(ing["description"], "Overridden ingredient with extra properties");
    }
    if (ing.contains("informational_URI")) {
        EXPECT_EQ(ing["informational_URI"], "https://example.com/assets/42");
    }
}

TEST_F(BuilderTest, LinkArchiveInstanceIdFromArchiveOnSigningBuilder)
{
    // Verify that instance_id does NOT work as a linking key for ingredient
    // archives, even when the instance_id is read from the archive and set
    // on the signing builder's add_ingredient call.
    auto context = c2pa::Context();
    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");

    // Create archive with ingredient carrying instance_id
    auto archive_path = get_temp_path("iid_from_archive_linking.c2pa");
    create_ingredient_archive(archive_path,
        R"({"title": "photo.jpg", "relationship": "componentOf", "instance_id": "xmp:iid:test-archive-link"})");

    // Read archive, extract instance_id
    auto reader = c2pa::Reader(context, archive_path);
    auto archive_parsed = json::parse(reader.json());
    std::string active = archive_parsed["active_manifest"];
    auto& archive_ingredient = archive_parsed["manifests"][active]["ingredients"][0];
    ASSERT_TRUE(archive_ingredient.contains("instance_id"));
    std::string instance_id = archive_ingredient["instance_id"];

    // Build manifest with ingredientIds referencing that instance_id
    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions.v2"},
                {"data", {{"actions", json::array({
                    {
                        {"action", "c2pa.placed"},
                        {"parameters", {{"ingredientIds", json::array({instance_id})}}}
                    }
                })}}}
            }
        })}
    };

    auto builder = c2pa::Builder(context, manifest_json.dump());

    // Set instance_id on the signing builder's add_ingredient
    std::ifstream archive_stream(archive_path, std::ios::binary);
    builder.add_ingredient(
        json({
            {"title", archive_ingredient["title"]},
            {"relationship", "componentOf"},
            {"instance_id", instance_id}
        }).dump(),
        "application/c2pa",
        archive_stream);
    archive_stream.close();

    auto output_path = get_temp_path("iid_from_archive_linking_result.jpg");

    // Signing throws because instance_id cannot be used as a linking key
    // for ingredient archives. Use label instead.
    EXPECT_THROW(builder.sign(source_path, output_path, signer), c2pa::C2paException);
}

TEST_F(BuilderTest, IngredientFieldsSurviveArchive)
{
    auto context = c2pa::Context();

    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto builder = c2pa::Builder(context, manifest_str);
    json ingredient_json = {
        {"title", "tracked-asset.jpg"},
        {"relationship", "componentOf"},
        {"instance_id", "tracking:project-7:asset-42"},
        {"description", "A tracked ingredient"},
        {"informational_URI", "https://example.com/assets/42"}
    };
    builder.add_ingredient(ingredient_json.dump(), c2pa_test::get_fixture_path("A.jpg"));

    auto archive_path = get_temp_path("fields_survive_archive.c2pa");
    ASSERT_NO_THROW(builder.to_archive(archive_path));

    // Read the archive back and check which fields survived
    auto reader = c2pa::Reader(context, archive_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];

    ASSERT_GE(ingredients.size(), 1u);
    auto& ing = ingredients[0];

    EXPECT_EQ(ing["title"], "tracked-asset.jpg");
    EXPECT_EQ(ing["relationship"], "componentOf");
}

TEST_F(BuilderTest, IngredientFieldsSurviveArchiveThenSign)
{
    auto context = c2pa::Context();
    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");

    // Create an archive with an ingredient carrying identifying fields
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto builder1 = c2pa::Builder(context, manifest_str);
    json ingredient_json = {
        {"title", "tracked-asset.jpg"},
        {"relationship", "componentOf"},
        {"instance_id", "tracking:project-7:asset-42"},
        {"description", "A tracked ingredient"},
        {"informational_URI", "https://example.com/assets/42"}
    };
    builder1.add_ingredient(ingredient_json.dump(), c2pa_test::get_fixture_path("A.jpg"));

    auto archive_path = get_temp_path("fields_survive_sign.c2pa");
    ASSERT_NO_THROW(builder1.to_archive(archive_path));

    // Add the archive as ingredient to a second builder, no overrides
    auto builder2 = c2pa::Builder(context, manifest_str);
    builder2.add_ingredient(
        R"({"title": "tracked-asset.jpg", "relationship": "componentOf"})",
        archive_path);

    auto output_path = get_temp_path("fields_survive_sign_result.jpg");
    ASSERT_NO_THROW(builder2.sign(source_path, output_path, signer));

    // Read signed asset and check ingredient fields
    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];

    ASSERT_GE(ingredients.size(), 1u);
}

TEST_F(BuilderTest, InstanceIdAsIngredientIdentifierInCatalog)
{
    auto context = c2pa::Context();

    // Create an archive with two ingredients, each with a different instance_id
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto builder = c2pa::Builder(context, manifest_str);

    builder.add_ingredient(
        json({
            {"title", "photo-A.jpg"},
            {"relationship", "componentOf"},
            {"instance_id", "catalog:photo-A"}
        }).dump(),
        c2pa_test::get_fixture_path("A.jpg"));

    builder.add_ingredient(
        json({
            {"title", "photo-B.jpg"},
            {"relationship", "componentOf"},
            {"instance_id", "catalog:photo-B"}
        }).dump(),
        c2pa_test::get_fixture_path("A.jpg"));

    auto archive_path = get_temp_path("catalog_instance_id.c2pa");
    ASSERT_NO_THROW(builder.to_archive(archive_path));

    // Read the archive and pick an ingredient by instance_id
    auto reader = c2pa::Reader(context, archive_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];

    ASSERT_EQ(ingredients.size(), 2u);

    // Find the ingredient with instance_id "catalog:photo-B"
    json* found = nullptr;
    for (auto& ing : ingredients) {
        if (ing.contains("instance_id") && ing["instance_id"] == "catalog:photo-B") {
            found = &ing;
            break;
        }
    }

    ASSERT_NE(found, nullptr)
        << "Should find ingredient by instance_id 'catalog:photo-B' in archive";
    EXPECT_EQ((*found)["title"], "photo-B.jpg");
}

TEST_F(BuilderTest, LinkArchiveLabelOnSigningBuilderPlaced)
{
    // No label on the archive ingredient
    auto archive_path = get_temp_path("label_on_signing_placed.c2pa");
    create_ingredient_archive(archive_path,
        R"({"title": "photo.jpg", "relationship": "componentOf"})");

    auto manifest_json = make_manifest_with_action("c2pa.placed", "my-ingredient");
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json.dump());

    // Label set on the signing builder's add_ingredient call
    builder.add_ingredient(
        R"({"title": "photo.jpg", "relationship": "componentOf", "label": "my-ingredient"})",
        archive_path);

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("link_label_on_signing_placed.jpg");

    bool linked = verify_ingredient_linked(builder, output_path, signer, "c2pa.placed");
    EXPECT_TRUE(linked);
}

TEST_F(BuilderTest, LinkArchiveLabelOnSigningBuilderOpened)
{
    auto archive_path = get_temp_path("label_on_signing_opened.c2pa");
    create_ingredient_archive(archive_path,
        R"({"title": "photo.jpg", "relationship": "parentOf"})");

    auto manifest_json = make_manifest_with_action("c2pa.opened", "my-ingredient",
        "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation");
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json.dump());

    builder.add_ingredient(
        R"({"title": "photo.jpg", "relationship": "parentOf", "label": "my-ingredient"})",
        archive_path);

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("link_label_on_signing_opened.jpg");

    bool linked = verify_ingredient_linked(builder, output_path, signer, "c2pa.opened");
    EXPECT_TRUE(linked);
}

TEST_F(BuilderTest, LinkArchiveLabelOnSigningBuilderOpenedFromStream)
{
    // Test scenario:
    // 1. Create the .c2pa ingredient archive.
    // 2. Build a signing manifest whose action references a chosen label.
    // 3. Open the archive as std::ifstream and add it via the stream
    // overload, setting the same label on the ingredient JSON.
    // 4. Sign, then read back and verify the action linked to the ingredient.
    auto context = c2pa::Context();
    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto fixture_path = c2pa_test::get_fixture_path("A.jpg");

    auto archive_path = get_temp_path("link_label_opened_stream_full_flow.c2pa");
    {
        auto archive_manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
        auto archive_builder = c2pa::Builder(context, archive_manifest);
        archive_builder.add_ingredient(
            R"({"title": "photo.jpg", "relationship": "parentOf"})",
            fixture_path);
        archive_builder.to_archive(archive_path);
    }

    json signing_manifest = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions.v2"},
                {"data", {{"actions", json::array({
                    {
                        {"action", "c2pa.opened"},
                        {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"},
                        {"parameters", {{"ingredientIds", json::array({"my-ingredient"})}}}
                    }
                })}}}
            }
        })}
    };
    auto builder = c2pa::Builder(context, signing_manifest.dump());

    std::ifstream archive_stream(archive_path, std::ios::binary);
    builder.add_ingredient(
        R"({"title": "photo.jpg", "relationship": "parentOf", "label": "my-ingredient"})",
        "application/c2pa",
        archive_stream);
    archive_stream.close();

    auto output_path = get_temp_path("link_label_opened_stream_full_flow_result.jpg");
    ASSERT_NO_THROW(builder.sign(source_path, output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& manifest = parsed["manifests"][active];

    bool found_action = false;
    std::string resolved_url;
    for (auto& assertion : manifest["assertions"]) {
        if (assertion["label"] != "c2pa.actions.v2") continue;
        for (auto& action : assertion["data"]["actions"]) {
            if (action["action"] != "c2pa.opened") continue;
            found_action = true;
            ASSERT_TRUE(action.contains("parameters"));
            ASSERT_TRUE(action["parameters"].contains("ingredients"));
            auto& ingredients = action["parameters"]["ingredients"];
            ASSERT_TRUE(ingredients.is_array());
            ASSERT_EQ(ingredients.size(), 1u);
            resolved_url = ingredients[0]["url"];
        }
    }
    ASSERT_TRUE(found_action) << "c2pa.opened action not found in signed manifest";
    EXPECT_EQ(resolved_url, "self#jumbf=c2pa.assertions/c2pa.ingredient.v3")
        << "Action did not resolve to the linked ingredient";
}

TEST_F(BuilderTest, LinkArchiveLabelOnSigningBuilderPlacedFromStream)
{
    auto archive_path = get_temp_path("label_on_signing_placed_stream.c2pa");
    create_ingredient_archive(archive_path,
        R"({"title": "photo.jpg", "relationship": "componentOf"})");

    auto manifest_json = make_manifest_with_action("c2pa.placed", "my-ingredient");
    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json.dump());

    std::ifstream archive_stream(archive_path, std::ios::binary);
    builder.add_ingredient(
        R"({"title": "photo.jpg", "relationship": "componentOf", "label": "my-ingredient"})",
        "application/c2pa",
        archive_stream);
    archive_stream.close();

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("link_label_on_signing_placed_stream.jpg");

    bool linked = verify_ingredient_linked(builder, output_path, signer, "c2pa.placed");
    EXPECT_TRUE(linked);
}

TEST_F(BuilderTest, LinkArchiveInstanceIdOnSigningBuilderFromStreamFails)
{
    auto context = c2pa::Context();
    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");

    auto archive_path = get_temp_path("iid_from_archive_linking_stream.c2pa");
    create_ingredient_archive(archive_path,
        R"({"title": "photo.jpg", "relationship": "parentOf", "instance_id": "xmp:iid:test-archive-link-stream"})");

    auto reader = c2pa::Reader(context, archive_path);
    auto archive_parsed = json::parse(reader.json());
    std::string active = archive_parsed["active_manifest"];
    auto& archive_ingredient = archive_parsed["manifests"][active]["ingredients"][0];
    ASSERT_TRUE(archive_ingredient.contains("instance_id"));
    std::string instance_id = archive_ingredient["instance_id"];

    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions.v2"},
                {"data", {{"actions", json::array({
                    {
                        {"action", "c2pa.opened"},
                        {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"},
                        {"parameters", {{"ingredientIds", json::array({instance_id})}}}
                    }
                })}}}
            }
        })}
    };

    auto builder = c2pa::Builder(context, manifest_json.dump());

    std::ifstream archive_stream(archive_path, std::ios::binary);
    builder.add_ingredient(
        json({
            {"title", archive_ingredient["title"]},
            {"relationship", "parentOf"},
            {"instance_id", instance_id}
        }).dump(),
        "application/c2pa",
        archive_stream);
    archive_stream.close();

    auto output_path = get_temp_path("iid_from_archive_linking_stream_result.jpg");

    EXPECT_THROW(builder.sign(source_path, output_path, signer), c2pa::C2paException);
}

TEST_F(BuilderTest, LinkArchiveTwoIngredientsUsingLabels)
{
    auto archive1 = get_temp_path("two_labels_archive1.c2pa");
    create_ingredient_archive(archive1,
        R"({"title": "photo-placed.jpg", "relationship": "componentOf"})");
    auto archive2 = get_temp_path("two_labels_archive2.c2pa");
    create_ingredient_archive(archive2,
        R"({"title": "photo-opened.jpg", "relationship": "parentOf"})");

    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions.v2"},
                {"data", {{"actions", json::array({
                    created_first_action(),
                    {
                        {"action", "c2pa.placed"},
                        {"parameters", {{"ingredientIds", json::array({"ingredient-for-placed"})}}}
                    },
                    {
                        {"action", "c2pa.opened"},
                        {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"},
                        {"parameters", {{"ingredientIds", json::array({"ingredient-for-opened"})}}}
                    }
                })}}}
            }
        })}
    };

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json.dump());

    builder.add_ingredient(
        R"({"title": "photo-placed.jpg", "relationship": "componentOf", "label": "ingredient-for-placed"})",
        archive1);
    builder.add_ingredient(
        R"({"title": "photo-opened.jpg", "relationship": "parentOf", "label": "ingredient-for-opened"})",
        archive2);

    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("two_ingredients_labels.jpg");
    ASSERT_NO_THROW(builder.sign(source_path, output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& manifest = parsed["manifests"][active];

    json placed_action, opened_action;
    bool found_placed = false, found_opened = false;
    for (auto& assertion : manifest["assertions"]) {
        if (assertion["label"] != "c2pa.actions.v2") continue;
        for (auto& action : assertion["data"]["actions"]) {
            if (action["action"] == "c2pa.placed") { placed_action = action; found_placed = true; }
            if (action["action"] == "c2pa.opened") { opened_action = action; found_opened = true; }
        }
    }
    ASSERT_TRUE(found_placed) << "c2pa.placed action not found";
    ASSERT_TRUE(found_opened) << "c2pa.opened action not found";

    ASSERT_TRUE(placed_action.contains("parameters"));
    ASSERT_TRUE(placed_action["parameters"].contains("ingredients"));
    ASSERT_EQ(placed_action["parameters"]["ingredients"].size(), 1u);
    std::string placed_url = placed_action["parameters"]["ingredients"][0]["url"];

    ASSERT_TRUE(opened_action.contains("parameters"));
    ASSERT_TRUE(opened_action["parameters"].contains("ingredients"));
    ASSERT_EQ(opened_action["parameters"]["ingredients"].size(), 1u);
    std::string opened_url = opened_action["parameters"]["ingredients"][0]["url"];

    EXPECT_NE(placed_url, opened_url);
}

TEST_F(BuilderTest, LinkArchiveMultipleIngredientsInOnePlacedAction)
{
    // c2pa.placed supports multiple ingredientIds — all composited at once.
    auto archive1 = get_temp_path("multi_in_one_archive1.c2pa");
    create_ingredient_archive(archive1,
        R"({"title": "base-layer.jpg", "relationship": "componentOf"})");
    auto archive2 = get_temp_path("multi_in_one_archive2.c2pa");
    create_ingredient_archive(archive2,
        R"({"title": "overlay-layer.jpg", "relationship": "componentOf"})");

    // One c2pa.placed action referencing two ingredients
    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions.v2"},
                {"data", {{"actions", json::array({
                    created_first_action(),
                    {
                        {"action", "c2pa.placed"},
                        {"parameters", {{"ingredientIds", json::array({"base-layer", "overlay-layer"})}}}
                    }
                })}}}
            }
        })}
    };

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest_json.dump());

    builder.add_ingredient(
        R"({"title": "base-layer.jpg", "relationship": "componentOf", "label": "base-layer"})",
        archive1);
    builder.add_ingredient(
        R"({"title": "overlay-layer.jpg", "relationship": "componentOf", "label": "overlay-layer"})",
        archive2);

    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("multi_in_one_placed.jpg");
    ASSERT_NO_THROW(builder.sign(source_path, output_path, signer));

    // Read back and verify the single placed action has two resolved ingredients
    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];

    json placed_action;
    bool found = false;
    for (auto& assertion : parsed["manifests"][active]["assertions"]) {
        if (assertion["label"] != "c2pa.actions.v2") continue;
        for (auto& action : assertion["data"]["actions"]) {
            if (action["action"] == "c2pa.placed") {
                placed_action = action;
                found = true;
            }
        }
    }
    ASSERT_TRUE(found) << "c2pa.placed action not found";
    ASSERT_TRUE(placed_action.contains("parameters"));
    ASSERT_TRUE(placed_action["parameters"].contains("ingredients"));
    auto& ingredients = placed_action["parameters"]["ingredients"];
    ASSERT_EQ(ingredients.size(), 2u)
        << "c2pa.placed should reference both ingredients";

    // The two URLs should be different
    std::string url0 = ingredients[0]["url"];
    std::string url1 = ingredients[1]["url"];
    EXPECT_NE(url0, url1);
}

TEST_F(BuilderTest, CustomParamsInActions)
{
    auto context = c2pa::Context();
    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");

    std::string instance_id = "custom-param-test-1";

    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions"},
                {"data", {
                    {"actions", json::array({
                        {
                            {"action", "c2pa.created"},
                            {"digitalSourceType", "http://c2pa.org/digitalsourcetype/compositeCapture"},
                            {"parameters", {
                                {"test.example.tool", "my-tool"},
                                {"test.example.session_id", "session-abc-123"}
                            }}
                        },
                        {
                            {"action", "c2pa.placed"},
                            {"description", "Placed an image"},
                            {"parameters", {
                                {"test.example.layer_id", "layer-42"},
                                {"ingredientIds", json::array({instance_id})}
                            }}
                        }
                    })}
                }}
            }
        })}
    };

    auto builder = c2pa::Builder(context, manifest_json.dump());
    builder.add_ingredient(
        json({{"title", "photo.jpg"}, {"relationship", "componentOf"}, {"instance_id", instance_id}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));

    auto output_path = get_temp_path("vendor_params.jpg");
    ASSERT_NO_THROW(builder.sign(source_path, output_path, signer));

    // Read back and verify custom params survive signing
    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    string active = parsed["active_manifest"];

    bool found_created_params = false;
    bool found_placed_params = false;
    for (auto& assertion : parsed["manifests"][active]["assertions"]) {
        if (assertion["label"] == "c2pa.actions.v2") {
            for (auto& action : assertion["data"]["actions"]) {
                if (action["action"] == "c2pa.created" && action.contains("parameters")) {
                    auto& params = action["parameters"];
                    if (params.contains("test.example.tool")) {
                        EXPECT_EQ(params["test.example.tool"], "my-tool");
                        EXPECT_EQ(params["test.example.session_id"], "session-abc-123");
                        found_created_params = true;
                    }
                }
                if (action["action"] == "c2pa.placed" && action.contains("parameters")) {
                    auto& params = action["parameters"];
                    if (params.contains("test.example.layer_id")) {
                        EXPECT_EQ(params["test.example.layer_id"], "layer-42");
                        found_placed_params = true;
                    }
                    // Also verify ingredientIds resolved to URL
                    EXPECT_TRUE(params.contains("ingredients"))
                        << "ingredientIds should be resolved to ingredients[] URLs";
                }
            }
        }
    }
    EXPECT_TRUE(found_created_params)
        << "Custom params on c2pa.created should survive signing";
    EXPECT_TRUE(found_placed_params)
        << "Custom params on c2pa.placed should survive signing";
}

TEST_F(BuilderTest, RebuildFromArchiveWithUpdatedProperties)
{
    auto context = c2pa::Context();
    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");

    // Create a builder with ingredient and multiple actions, then archive.
    json phase1_manifest = {
        {"claim_generator_info", json::array({{{"name", "phase1-app"}, {"version", "0.1.0"}}})},
        {"title", "original-title.jpg"},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions"},
                {"data", {{"actions", json::array({
                    {{"action", "c2pa.created"},
                     {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"}},
                    {{"action", "c2pa.color_adjustments"},
                     {"parameters", {{"name", "brightnesscontrast"}}}}
                })}}}
            }
        })}
    };

    auto builder1 = c2pa::Builder(context, phase1_manifest.dump());
    builder1.add_ingredient(
        R"({"title": "parent.jpg", "relationship": "parentOf", "instance_id": "tracking:parent-1"})",
        source_path);

    auto archive_path = get_temp_path("issue176_rebuild_archive.c2pa");
    builder1.to_archive(archive_path);

    // Read the archive, extract everything, rebuild with updated properties.
    auto reader = c2pa::Reader(context, archive_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& archived_manifest = parsed["manifests"][active];

    // Extract ingredients and assertions from the archive.
    auto ingredients = archived_manifest["ingredients"];
    auto assertions = archived_manifest["assertions"];

    // Build a new manifest with updated properties but same ingredients/assertions.
    json new_manifest = {
        {"claim_generator_info", json::array({{{"name", "final-app"}, {"version", "0.2.0"}}})},
        {"title", "renamed-and-converted.png"},
        {"ingredients", ingredients},
        {"assertions", assertions}
    };

    auto builder2 = c2pa::Builder(context, new_manifest.dump());

    // Transfer binary resources for the ingredients.
    for (auto& ing : ingredients) {
        if (ing.contains("thumbnail")) {
            std::string id = ing["thumbnail"]["identifier"];
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            reader.get_resource(id, stream);
            stream.seekg(0);
            builder2.add_resource(id, stream);
        }
        if (ing.contains("manifest_data")) {
            std::string id = ing["manifest_data"]["identifier"];
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            reader.get_resource(id, stream);
            stream.seekg(0);
            builder2.add_resource(id, stream);
        }
    }

    auto output_path = get_temp_path("issue176_rebuild_result.jpg");
    ASSERT_NO_THROW(builder2.sign(source_path, output_path, signer));

    // Verify everything made it through.
    auto signed_reader = c2pa::Reader(context, output_path);
    auto signed_parsed = json::parse(signed_reader.json());
    std::string signed_active = signed_parsed["active_manifest"];
    auto& signed_manifest = signed_parsed["manifests"][signed_active];

    EXPECT_EQ(signed_manifest["title"], "renamed-and-converted.png");

    bool claim_generator_found = false;
    if (signed_manifest.contains("claim_generator")) {
        std::string cg = signed_manifest["claim_generator"];
        claim_generator_found = cg.find("final-app") != std::string::npos;
    }
    if (signed_manifest.contains("claim_generator_info")) {
        for (auto& info : signed_manifest["claim_generator_info"]) {
            if (info.contains("name") && info["name"] == "final-app") {
                claim_generator_found = true;
            }
        }
    }
    EXPECT_TRUE(claim_generator_found);

    ASSERT_TRUE(signed_manifest.contains("ingredients"));
    ASSERT_FALSE(signed_manifest["ingredients"].empty());
    EXPECT_EQ(signed_manifest["ingredients"][0]["title"], "parent.jpg");

    ASSERT_TRUE(signed_manifest["ingredients"][0].contains("instance_id"));
    EXPECT_EQ(signed_manifest["ingredients"][0]["instance_id"], "tracking:parent-1");

    bool has_created = false;
    bool has_color_adjustments = false;
    for (auto& assertion : signed_manifest["assertions"]) {
        if (assertion.contains("label") &&
            (assertion["label"] == "c2pa.actions" || assertion["label"] == "c2pa.actions.v2")) {
            for (auto& action : assertion["data"]["actions"]) {
                if (action["action"] == "c2pa.created") {
                    has_created = true;
                }
                if (action["action"] == "c2pa.color_adjustments") {
                    has_color_adjustments = true;
                }
            }
        }
    }
    EXPECT_TRUE(has_created);
    EXPECT_TRUE(has_color_adjustments);
}

TEST_F(BuilderTest, MultiphaseRebuildFromArchiveWithUpdatedProperties)
{
    auto context = c2pa::Context();
    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");

    // Phase 1: initial manifest with initial ingredient and actions.
    json phase1_manifest = {
        {"claim_generator_info", json::array({{{"name", "phase1-app"}, {"version", "0.1.0"}}})},
        {"title", "original-title.jpg"},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions"},
                {"data", {{"actions", json::array({
                    {{"action", "c2pa.created"},
                     {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"}}
                })}}}
            }
        })}
    };

    auto builder1 = c2pa::Builder(context, phase1_manifest.dump());
    builder1.add_ingredient(
        R"({"title": "parent.jpg", "relationship": "parentOf", "instance_id": "tracking:parent-1"})",
        source_path);

    auto archive1_path = get_temp_path("multiphase_archive1.c2pa");
    builder1.to_archive(archive1_path);

    // Phase 2: restore from archive, add a color_adjustments action, archive again.
    auto builder2 = c2pa::Builder(context);
    std::ifstream archive1_stream(archive1_path, std::ios::binary);
    builder2.with_archive(archive1_stream);
    archive1_stream.close();

    builder2.add_action(R"({
        "action": "c2pa.color_adjustments",
        "parameters": {"name": "brightnesscontrast"}
    })");

    auto archive2_path = get_temp_path("multiphase_archive2.c2pa");
    builder2.to_archive(archive2_path);

    // Final phase: read the archive with Reader then rebuild final manifest to use
    auto reader = c2pa::Reader(context, archive2_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& archived_manifest = parsed["manifests"][active];

    auto ingredients = archived_manifest["ingredients"];

    // Merge all c2pa.actions / c2pa.actions.v2 assertions into a single one.
    // add_action() creates separate assertions, so the archive may have duplicates.
    json merged_actions = json::array();
    json other_assertions = json::array();
    for (auto& assertion : archived_manifest["assertions"]) {
        if (assertion.contains("label") &&
            (assertion["label"] == "c2pa.actions" || assertion["label"] == "c2pa.actions.v2")) {
            for (auto& action : assertion["data"]["actions"]) {
                merged_actions.push_back(action);
            }
        } else {
            other_assertions.push_back(assertion);
        }
    }
    json merged_assertions = other_assertions;
    if (!merged_actions.empty()) {
        merged_assertions.push_back({
            {"label", "c2pa.actions"},
            {"data", {{"actions", merged_actions}}}
        });
    }

    json new_manifest = {
        {"claim_generator_info", json::array({{{"name", "final-app"}, {"version", "2.0"}}})},
        {"title", "renamed-and-converted.png"},
        {"ingredients", ingredients},
        {"assertions", merged_assertions}
    };

    auto builder3 = c2pa::Builder(context, new_manifest.dump());

    for (auto& ing : ingredients) {
        if (ing.contains("thumbnail")) {
            std::string id = ing["thumbnail"]["identifier"];
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            reader.get_resource(id, stream);
            stream.seekg(0);
            builder3.add_resource(id, stream);
        }
        if (ing.contains("manifest_data")) {
            std::string id = ing["manifest_data"]["identifier"];
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            reader.get_resource(id, stream);
            stream.seekg(0);
            builder3.add_resource(id, stream);
        }
    }

    auto output_path = get_temp_path("multiphase_rebuild_result.jpg");
    ASSERT_NO_THROW(builder3.sign(source_path, output_path, signer));

    // Verify everything made it through all phases.
    auto signed_reader = c2pa::Reader(context, output_path);
    auto signed_parsed = json::parse(signed_reader.json());
    std::string signed_active = signed_parsed["active_manifest"];
    auto& signed_manifest = signed_parsed["manifests"][signed_active];

    EXPECT_EQ(signed_manifest["title"], "renamed-and-converted.png");

    bool claim_generator_found = false;
    if (signed_manifest.contains("claim_generator")) {
        std::string cg = signed_manifest["claim_generator"];
        claim_generator_found = cg.find("final-app") != std::string::npos;
    }
    if (signed_manifest.contains("claim_generator_info")) {
        for (auto& info : signed_manifest["claim_generator_info"]) {
            if (info.contains("name") && info["name"] == "final-app") {
                claim_generator_found = true;
            }
        }
    }
    EXPECT_TRUE(claim_generator_found);

    ASSERT_TRUE(signed_manifest.contains("ingredients"));
    ASSERT_FALSE(signed_manifest["ingredients"].empty());
    EXPECT_EQ(signed_manifest["ingredients"][0]["title"], "parent.jpg");
    ASSERT_TRUE(signed_manifest["ingredients"][0].contains("instance_id"));
    EXPECT_EQ(signed_manifest["ingredients"][0]["instance_id"], "tracking:parent-1");

    // Verify exactly one actions assertion with both actions inside.
    int actions_assertion_count = 0;
    bool has_created = false;
    bool has_color_adjustments = false;
    for (auto& assertion : signed_manifest["assertions"]) {
        if (assertion.contains("label") &&
            (assertion["label"] == "c2pa.actions" || assertion["label"] == "c2pa.actions.v2")) {
            actions_assertion_count++;
            for (auto& action : assertion["data"]["actions"]) {
                if (action["action"] == "c2pa.created") {
                    has_created = true;
                }
                if (action["action"] == "c2pa.color_adjustments") {
                    has_color_adjustments = true;
                }
            }
        }
    }
    EXPECT_EQ(actions_assertion_count, 1)
        << "There should be exactly one c2pa.actions assertion after merging";
    EXPECT_TRUE(has_created)
        << "c2pa.created from phase 1 should survive multi-phase rebuild";
    EXPECT_TRUE(has_color_adjustments)
        << "c2pa.color_adjustments from phase 2 should survive multi-phase rebuild";
}

TEST_F(BuilderTest, MultiphaseRebuildFromArchiveWithUpdatedProperties2)
{
    auto context = c2pa::Context();
    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");

    // Phase 1: initial manifest with parentOf ingredient and c2pa.created.
    json phase1_manifest = {
        {"claim_generator_info", json::array({{{"name", "phase1-app"}, {"version", "0.1.0"}}})},
        {"title", "original-title.jpg"},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions"},
                {"data", {{"actions", json::array({
                    {{"action", "c2pa.created"},
                     {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"}}
                })}}}
            }
        })}
    };

    auto builder1 = c2pa::Builder(context, phase1_manifest.dump());
    builder1.add_ingredient(
        R"({"title": "parent.jpg", "relationship": "parentOf", "instance_id": "tracking:parent-1"})",
        source_path);

    auto archive1_path = get_temp_path("multiphase2_archive1.c2pa");
    builder1.to_archive(archive1_path);

    // Phase 2: restore, add a componentOf ingredient and a color_adjustments action, archive again.
    auto builder2 = c2pa::Builder(context);
    std::ifstream archive1_stream(archive1_path, std::ios::binary);
    builder2.with_archive(archive1_stream);
    archive1_stream.close();

    builder2.add_ingredient(
        R"({"title": "overlay.png", "relationship": "componentOf", "instance_id": "tracking:overlay-1"})",
        c2pa_test::get_fixture_path("A.jpg"));

    builder2.add_action(R"({
        "action": "c2pa.color_adjustments",
        "parameters": {"name": "brightnesscontrast"}
    })");

    auto archive2_path = get_temp_path("multiphase2_archive2.c2pa");
    builder2.to_archive(archive2_path);

    // Final phase: read archive, rebuild with updated properties, transfer resources, sign.
    auto reader = c2pa::Reader(context, archive2_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& archived_manifest = parsed["manifests"][active];

    auto ingredients = archived_manifest["ingredients"];

    // Merge all c2pa.actions / c2pa.actions.v2 assertions into a single one.
    json merged_actions = json::array();
    json other_assertions = json::array();
    for (auto& assertion : archived_manifest["assertions"]) {
        if (assertion.contains("label") &&
            (assertion["label"] == "c2pa.actions" || assertion["label"] == "c2pa.actions.v2")) {
            for (auto& action : assertion["data"]["actions"]) {
                merged_actions.push_back(action);
            }
        } else {
            other_assertions.push_back(assertion);
        }
    }
    json merged_assertions = other_assertions;
    if (!merged_actions.empty()) {
        merged_assertions.push_back({
            {"label", "c2pa.actions"},
            {"data", {{"actions", merged_actions}}}
        });
    }

    json new_manifest = {
        {"claim_generator_info", json::array({{{"name", "final-app"}, {"version", "2.0"}}})},
        {"title", "renamed-and-converted.png"},
        {"ingredients", ingredients},
        {"assertions", merged_assertions}
    };

    auto builder3 = c2pa::Builder(context, new_manifest.dump());

    for (auto& ing : ingredients) {
        if (ing.contains("thumbnail")) {
            std::string id = ing["thumbnail"]["identifier"];
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            reader.get_resource(id, stream);
            stream.seekg(0);
            builder3.add_resource(id, stream);
        }
        if (ing.contains("manifest_data")) {
            std::string id = ing["manifest_data"]["identifier"];
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            reader.get_resource(id, stream);
            stream.seekg(0);
            builder3.add_resource(id, stream);
        }
    }

    auto output_path = get_temp_path("multiphase2_rebuild_result.jpg");
    ASSERT_NO_THROW(builder3.sign(source_path, output_path, signer));

    // Verify everything from both phases made it through.
    auto signed_reader = c2pa::Reader(context, output_path);
    auto signed_parsed = json::parse(signed_reader.json());
    std::string signed_active = signed_parsed["active_manifest"];
    auto& signed_manifest = signed_parsed["manifests"][signed_active];

    EXPECT_EQ(signed_manifest["title"], "renamed-and-converted.png");

    bool claim_generator_found = false;
    if (signed_manifest.contains("claim_generator")) {
        std::string cg = signed_manifest["claim_generator"];
        claim_generator_found = cg.find("final-app") != std::string::npos;
    }
    if (signed_manifest.contains("claim_generator_info")) {
        for (auto& info : signed_manifest["claim_generator_info"]) {
            if (info.contains("name") && info["name"] == "final-app") {
                claim_generator_found = true;
            }
        }
    }
    EXPECT_TRUE(claim_generator_found);

    ASSERT_TRUE(signed_manifest.contains("ingredients"));
    ASSERT_EQ(signed_manifest["ingredients"].size(), 2u);

    bool found_parent = false;
    bool found_overlay = false;
    for (auto& ing : signed_manifest["ingredients"]) {
        if (ing["title"] == "parent.jpg") {
            found_parent = true;
            ASSERT_TRUE(ing.contains("instance_id"));
            EXPECT_EQ(ing["instance_id"], "tracking:parent-1");
            EXPECT_EQ(ing["relationship"], "parentOf");
        }
        if (ing["title"] == "overlay.png") {
            found_overlay = true;
            ASSERT_TRUE(ing.contains("instance_id"));
            EXPECT_EQ(ing["instance_id"], "tracking:overlay-1");
            EXPECT_EQ(ing["relationship"], "componentOf");
        }
    }
    EXPECT_TRUE(found_parent)
        << "parentOf ingredient from phase 1 should survive";
    EXPECT_TRUE(found_overlay)
        << "componentOf ingredient from phase 2 should survive";

    // Verify exactly one actions assertion with both actions inside.
    int actions_assertion_count = 0;
    bool has_created = false;
    bool has_color_adjustments = false;
    for (auto& assertion : signed_manifest["assertions"]) {
        if (assertion.contains("label") &&
            (assertion["label"] == "c2pa.actions" || assertion["label"] == "c2pa.actions.v2")) {
            actions_assertion_count++;
            for (auto& action : assertion["data"]["actions"]) {
                if (action["action"] == "c2pa.created") {
                    has_created = true;
                }
                if (action["action"] == "c2pa.color_adjustments") {
                    has_color_adjustments = true;
                }
            }
        }
    }
    EXPECT_EQ(actions_assertion_count, 1)
        << "There should be exactly one c2pa.actions assertion after merging";
    EXPECT_TRUE(has_created)
        << "c2pa.created from phase 1 should survive";
    EXPECT_TRUE(has_color_adjustments)
        << "c2pa.color_adjustments from phase 2 should survive";
}

TEST_F(BuilderTest, NonAsciiSourcePathForSign)
{
    // Copy A.jpg to a temp path with a non-ASCII name rather than relying on
    // the fixture file checking out correctly on all platforms (e.g. Windows).
    auto source_path = get_temp_path(u8"source-CÖÄ_-øæå.jpg");
    fs::copy_file(c2pa_test::get_fixture_path("A.jpg"), source_path, fs::copy_options::overwrite_existing);
    auto output_path = get_temp_path(u8"non_ascii_sign_output.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();
    auto builder = c2pa::Builder(manifest);

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(source_path, output_path, signer))
        << "Builder::sign should open non-ASCII source path without throwing";
    EXPECT_FALSE(manifest_data.empty());
    EXPECT_TRUE(fs::exists(output_path));
}

TEST_F(BuilderTest, NonAsciiDestPathForSign)
{
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path(u8"output-CÖÄ_-øæå.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();
    auto builder = c2pa::Builder(manifest);

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(source_path, output_path, signer))
        << "Builder::sign should open non-ASCII destination path without throwing";
    EXPECT_FALSE(manifest_data.empty());
    EXPECT_TRUE(fs::exists(output_path));
}

TEST_F(BuilderTest, NonAsciiPathForAddIngredient)
{
    // Copy A.jpg to a temp path with a non-ASCII name rather than relying on
    // the fixture file checking out correctly on all platforms (e.g. Windows).
    auto ingredient_path = get_temp_path(u8"ingredient-CÖÄ_-øæå.jpg");
    fs::copy_file(c2pa_test::get_fixture_path("A.jpg"), ingredient_path, fs::copy_options::overwrite_existing);
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("non_ascii_ingredient_output.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();
    auto builder = c2pa::Builder(manifest);

    ASSERT_NO_THROW(builder.add_ingredient("{\"title\":\"Non-ASCII Ingredient\"}", ingredient_path))
        << "Builder::add_ingredient should open non-ASCII path without throwing";

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(source_path, output_path, signer));
    EXPECT_FALSE(manifest_data.empty());
}

TEST_F(BuilderTest, NonAsciiPathForAddResource)
{
    // Copy A.jpg to a temp path with a non-ASCII name rather than relying on
    // the fixture file checking out correctly on all platforms (e.g. Windows).
    auto resource_path = get_temp_path(u8"resource-CÖÄ_-øæå.jpg");
    fs::copy_file(c2pa_test::get_fixture_path("A.jpg"), resource_path, fs::copy_options::overwrite_existing);
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("non_ascii_resource_output.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto signer = c2pa_test::create_test_signer();
    auto builder = c2pa::Builder(manifest);

    ASSERT_NO_THROW(builder.add_resource("thumbnail", resource_path))
        << "Builder::add_resource should open non-ASCII path without throwing";

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(source_path, output_path, signer));
    EXPECT_FALSE(manifest_data.empty());
}

TEST_F(BuilderTest, NonAsciiPathForReaderGetResource)
{
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto signed_path = get_temp_path("signed_for_get_resource.jpg");
    // Use a manifest with an explicit thumbnail resource declaration so the
    // resource is embedded in the signed output and can be retrieved via get_resource.
    const std::string manifest = R"({
        "claim_generator_info": [{"name": "c2pa-c test NonAsciiPathForReaderGetResource", "version": "0.1.0"}],
        "thumbnail": {"format": "image/jpeg", "identifier": "thumbnail"},
        "assertions": [{"label": "c2pa.actions", "data": {"actions": [{"action": "c2pa.created", "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"}]}}]
    })";
    auto signer = c2pa_test::create_test_signer();
    {
        auto builder = c2pa::Builder(manifest);
        builder.add_resource("thumbnail", source_path);
        builder.sign(source_path, signed_path, signer);
    }
    ASSERT_TRUE(fs::exists(signed_path));

    // Parse the reader JSON to find the actual embedded thumbnail identifier.
    auto reader = c2pa::Reader(signed_path);
    auto manifest_json = json::parse(reader.json());
    std::string active = manifest_json["active_manifest"];
    std::string thumbnail_id = manifest_json["manifests"][active]["thumbnail"]["identifier"];

    auto output_resource_path = get_temp_path(u8"resource-CÖÄ_-øæå.jpg");
    ASSERT_NO_THROW(reader.get_resource(thumbnail_id, output_resource_path));
    EXPECT_TRUE(fs::exists(output_resource_path));
}

// Sign a file using a Signer from the Builder's Context, then read back
TEST_F(BuilderTest, SignFileWithContextSigner) {
    auto image_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("signed_file_context_signed.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto signer = c2pa_test::create_test_signer();
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(std::move(signer))
        .create_context();

    auto builder = c2pa::Builder(context, manifest);

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(image_path, output_path));
    ASSERT_FALSE(manifest_data.empty());

    auto reader = c2pa::Reader(context, output_path);
    std::string json_result;
    ASSERT_NO_THROW(json_result = reader.json());
}

// Sign a stream using a Signer from the Builder Context
TEST_F(BuilderTest, SignStreamWithContextSigner) {
    auto image_path = c2pa_test::get_fixture_path("A.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto signer = c2pa_test::create_test_signer();
    auto context = c2pa::Context::ContextBuilder()
        .with_signer(std::move(signer))
        .create_context();

    auto builder = c2pa::Builder(context, manifest);

    std::ifstream source(image_path, std::ios::binary);
    ASSERT_TRUE(source.is_open());

    std::stringstream memory_buffer(std::ios::in | std::ios::out | std::ios::binary);
    std::iostream& dest = memory_buffer;

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign("image/jpeg", source, dest));
    source.close();
    ASSERT_FALSE(manifest_data.empty());

    dest.flush();
    dest.seekg(0, std::ios::beg);

    auto reader = c2pa::Reader(context, "image/jpeg", dest);
    std::string json_result;
    ASSERT_NO_THROW(json_result = reader.json());
    EXPECT_TRUE(json_result.find("cawg.training-mining") != std::string::npos);
}

// Sign a stream using a Signer from the Builder Context (settings and signer overload)
TEST_F(BuilderTest, SignWithContextSignerAndSettings) {
    auto image_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("context_signer_settings.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "false");

    auto signer = c2pa_test::create_test_signer();
    c2pa::Context context(settings, std::move(signer));

    auto builder = c2pa::Builder(context, manifest);

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(image_path, output_path));
    ASSERT_FALSE(manifest_data.empty());

    auto reader = c2pa::Reader(context, output_path);
    ASSERT_NO_THROW(reader.json());
}

// Calling sign() without a signer in the context should throw
TEST_F(BuilderTest, SignWithoutContextSignerThrows) {
    auto image_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("no_signer_throws.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, manifest);

    EXPECT_THROW(builder.sign(image_path, output_path), c2pa::C2paException);
}

// Sign a stream using a Signer configured in settings JSON
TEST_F(BuilderTest, SignStreamWithSettingsSigner) {
    auto image_path = c2pa_test::get_fixture_path("A.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto settings = c2pa_test::create_test_settings_with_signer();
    c2pa::Context context(settings);

    auto builder = c2pa::Builder(context, manifest);

    std::ifstream source(image_path, std::ios::binary);
    ASSERT_TRUE(source.is_open());

    std::stringstream memory_buffer(std::ios::in | std::ios::out | std::ios::binary);
    std::iostream& dest = memory_buffer;

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign("image/jpeg", source, dest));
    source.close();
    ASSERT_FALSE(manifest_data.empty());

    dest.flush();
    dest.seekg(0, std::ios::beg);

    auto reader = c2pa::Reader(context, "image/jpeg", dest);
    ASSERT_NO_THROW(reader.json());
}

// Sign a stream using a CallbackSigner stored in Context
TEST_F(BuilderTest, SignStreamWithCallbackSignerInContext) {
    auto image_path = c2pa_test::get_fixture_path("A.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    c2pa::SignerFunc *callback = [](const std::vector<unsigned char> &data)
        -> std::vector<unsigned char> {
        auto private_key = c2pa_test::read_text_file(
            c2pa_test::get_fixture_path("ed25519.pem"));
        const unsigned char *sig_ptr = c2pa_ed25519_sign(
            data.data(), data.size(), private_key.c_str());
        if (!sig_ptr) {
            throw std::runtime_error("c2pa_ed25519_sign failed");
        }
        std::vector<unsigned char> signature(sig_ptr, sig_ptr + 64);
        c2pa_signature_free(sig_ptr);
        return signature;
    };

    auto certs = c2pa_test::read_text_file(
        c2pa_test::get_fixture_path("ed25519.pub"));
    auto signer = c2pa::Signer(
        callback, C2paSigningAlg::Ed25519, certs,
        "http://timestamp.digicert.com");

    auto context = c2pa::Context::ContextBuilder()
        .with_signer(std::move(signer))
        .create_context();

    auto builder = c2pa::Builder(context, manifest);

    std::ifstream source(image_path, std::ios::binary);
    ASSERT_TRUE(source.is_open());

    std::stringstream memory_buffer(std::ios::in | std::ios::out | std::ios::binary);
    std::iostream& dest = memory_buffer;

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign("image/jpeg", source, dest));
    source.close();
    ASSERT_FALSE(manifest_data.empty());

    dest.flush();
    dest.seekg(0, std::ios::beg);

    auto reader = c2pa::Reader(context, "image/jpeg", dest);
    ASSERT_NO_THROW(reader.json());
}


TEST_F(BuilderTest, CreateIntentAddsCreatedAction)
{
    auto signer = c2pa_test::create_test_signer();
    auto image_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("intent_create.jpg");

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, R"({})");
    builder.set_intent(Create, DigitalCapture);

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(image_path, output_path, signer));
    ASSERT_FALSE(manifest_data.empty());

    auto reader = c2pa::Reader(context, output_path);
    auto result = json::parse(reader.json());

    string active_label = result["active_manifest"];
    json active = result["manifests"][active_label];

    bool found_created = false;
    for (const auto& assertion : active["assertions"]) {
        if (!assertion.contains("label")) continue;
        std::string label = assertion["label"];
        if (label.find("c2pa.actions") == std::string::npos) continue;
        for (const auto& action : assertion["data"]["actions"]) {
            if (action["action"] == "c2pa.created") {
                found_created = true;
                // Verify digital source type matches what was set
                std::string dst = action["digitalSourceType"];
                EXPECT_TRUE(dst.find("digitalCapture") != std::string::npos);
            }
        }
    }
    ASSERT_TRUE(found_created) << "Expected c2pa.created action in manifest";
}

TEST_F(BuilderTest, EditIntentAddsOpenedAction)
{
    auto signer = c2pa_test::create_test_signer();
    auto image_path = c2pa_test::get_fixture_path("C.jpg");
    auto output_path = get_temp_path("intent_edit.jpg");

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, R"({})");
    builder.set_intent(Edit);

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(image_path, output_path, signer));
    ASSERT_FALSE(manifest_data.empty());

    auto reader = c2pa::Reader(context, output_path);
    auto result = json::parse(reader.json());

    string active_label = result["active_manifest"];
    json active = result["manifests"][active_label];

    // Verify c2pa.opened action exists
    bool found_opened = false;
    for (const auto& assertion : active["assertions"]) {
        if (!assertion.contains("label")) continue;
        std::string label = assertion["label"];
        if (label.find("c2pa.actions") == std::string::npos) continue;
        for (const auto& action : assertion["data"]["actions"]) {
            if (action["action"] == "c2pa.opened") {
                found_opened = true;
            }
        }
    }
    ASSERT_TRUE(found_opened) << "Expected c2pa.opened action in active manifest";

    // Verify parentOf ingredient exists
    ASSERT_TRUE(active.contains("ingredients"));
    bool found_parent = false;
    for (const auto& ingredient : active["ingredients"]) {
        if (ingredient.contains("relationship") && ingredient["relationship"] == "parentOf") {
            found_parent = true;
        }
    }
    ASSERT_TRUE(found_parent) << "Expected parentOf ingredient to be found in active manifest";
}

TEST_F(BuilderTest, UpdateIntentAddsOpenedAction)
{
    auto signer = c2pa_test::create_test_signer();
    auto image_path = c2pa_test::get_fixture_path("C.jpg");
    auto output_path = get_temp_path("intent_update.jpg");

    auto context = c2pa::Context();
    auto builder = c2pa::Builder(context, R"({})");
    builder.set_intent(Update);

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(image_path, output_path, signer));
    ASSERT_FALSE(manifest_data.empty());

    auto reader = c2pa::Reader(context, output_path);
    auto result = json::parse(reader.json());

    string active_label = result["active_manifest"];
    json active = result["manifests"][active_label];

    // Verify c2pa.opened action exists
    bool found_opened = false;
    for (const auto& assertion : active["assertions"]) {
        if (!assertion.contains("label")) continue;
        std::string label = assertion["label"];
        if (label.find("c2pa.actions") == std::string::npos) continue;
        for (const auto& action : assertion["data"]["actions"]) {
            if (action["action"] == "c2pa.opened") {
                found_opened = true;
            }
        }
    }
    ASSERT_TRUE(found_opened) << "Expected c2pa.opened action in active manifest";

    // Verify a parentOf ingredient exists
    ASSERT_TRUE(active.contains("ingredients"));
    bool found_parent = false;
    for (const auto& ingredient : active["ingredients"]) {
        if (ingredient.contains("relationship") && ingredient["relationship"] == "parentOf") {
            found_parent = true;
        }
    }
    ASSERT_TRUE(found_parent) << "Expected parentOf ingredient to be found in active manifest";
}

TEST_F(BuilderTest, CreateIntentViaContext)
{
    auto signer = c2pa_test::create_test_signer();
    auto image_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("intent_create_ctx.jpg");

    auto context = c2pa::Context(R"({
        "version": 1,
        "builder": {
            "intent": {"create": "digitalCapture"}
        }
    })");
    auto builder = c2pa::Builder(context, R"({})");

    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder.sign(image_path, output_path, signer));
    ASSERT_FALSE(manifest_data.empty());

    auto reader = c2pa::Reader(context, output_path);
    auto result = json::parse(reader.json());

    string active_label = result["active_manifest"];
    json active = result["manifests"][active_label];

    // Verify c2pa.created action exists
    bool found_created = false;
    for (const auto& assertion : active["assertions"]) {
        if (!assertion.contains("label")) continue;
        std::string label = assertion["label"];
        if (label.find("c2pa.actions") == std::string::npos) continue;
        for (const auto& action : assertion["data"]["actions"]) {
            if (action["action"] == "c2pa.created") {
                found_created = true;
            }
        }
    }
    ASSERT_TRUE(found_created) << "Expected c2pa.created action in active manifest";
}

TEST_F(BuilderTest, ArchiveIngredientRoundTripAndReuse)
{
    auto context = c2pa::Context();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");

    // Shared instance_id links the parentOf ingredient to a c2pa.opened action.
    // Thos will make the archive valid
    std::string instance_id = "xmp:iid:archive-roundtrip-0001";
    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions"},
                {"data", {
                    {"actions", json::array({
                        {
                            {"action", "c2pa.opened"},
                            {"parameters", {
                                {"ingredientIds", json::array({instance_id})}
                            }}
                        }
                    })}
                }}
            }
        })}
    };

    // 1. Builder1: add parentOf ingredient linked to c2pa.opened, no signing.
    auto builder1 = c2pa::Builder(context, manifest_json.dump());
    json ingredient = {
        {"title", "A.jpg"},
        {"relationship", "parentOf"},
        {"instance_id", instance_id}
    };
    builder1.add_ingredient(ingredient.dump(), source_path);

    // 2. Archive to .c2pa file.
    auto archive_path = get_temp_path("roundtrip-ingredient.c2pa");
    ASSERT_NO_THROW(builder1.to_archive(archive_path));
    ASSERT_TRUE(fs::exists(archive_path));
    ASSERT_GT(fs::file_size(archive_path), 0u);

    // 3. Read archive back; manifest must parse and carry the ingredient.
    std::ifstream archive_in(archive_path, std::ios::binary);
    c2pa::Reader archive_reader(context, "application/c2pa", archive_in);
    std::string archive_json;
    ASSERT_NO_THROW(archive_json = archive_reader.json());

    auto parsed = json::parse(archive_json);
    ASSERT_TRUE(parsed.contains("active_manifest"));
    std::string active = parsed["active_manifest"];
    auto ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);
    EXPECT_EQ(ingredients[0]["title"], "A.jpg");
    EXPECT_EQ(ingredients[0]["relationship"], "parentOf");

    // 4. Builder2: fresh manifest (training.json) + add the archive as an ingredient.
    auto builder2_manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto builder2 = c2pa::Builder(context, builder2_manifest);
    json ingredient_override = {
        {"title", "A.jpg"},
        {"relationship", "componentOf"}
    };
    ASSERT_NO_THROW(builder2.add_ingredient(ingredient_override.dump(), archive_path));

    // 5. Sign builder2 and validate signed output.
    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("archive_reused_output.jpg");
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder2.sign(source_path, output_path, signer));
    ASSERT_FALSE(manifest_data.empty());

    auto out_reader = c2pa::Reader(context, output_path);
    std::string out_json;
    ASSERT_NO_THROW(out_json = out_reader.json());
    auto out_parsed = json::parse(out_json);
    std::string out_active = out_parsed["active_manifest"];
    auto out_ingredients = out_parsed["manifests"][out_active]["ingredients"];
    ASSERT_EQ(out_ingredients.size(), 1u);
    EXPECT_EQ(out_ingredients[0]["title"], "A.jpg");
    EXPECT_EQ(out_ingredients[0]["relationship"], "componentOf");
}

TEST_F(BuilderTest, ArchiveIngredientWithProvenanceRoundTripAndReuse)
{
    auto context = c2pa::Context();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto ingredient_path = c2pa_test::get_fixture_path("C.jpg");

    // Shared instance_id links the parentOf ingredient to a c2pa.opened action.
    std::string instance_id = "xmp:iid:archive-roundtrip-provenance-0001";

    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions"},
                {"data", {
                    {"actions", json::array({
                        {
                            {"action", "c2pa.opened"},
                            {"parameters", {
                                {"ingredientIds", json::array({instance_id})}
                            }}
                        }
                    })}
                }}
            }
        })}
    };

    // 1. Builder1: add parentOf ingredient (C.jpg is C2PA-signed, has provenance).
    auto builder1 = c2pa::Builder(context, manifest_json.dump());
    json ingredient = {
        {"title", "C.jpg"},
        {"relationship", "parentOf"},
        {"instance_id", instance_id}
    };
    builder1.add_ingredient(ingredient.dump(), ingredient_path);

    // 2. Archive to .c2pa file.
    auto archive_path = get_temp_path("roundtrip-provenance-ingredient.c2pa");
    ASSERT_NO_THROW(builder1.to_archive(archive_path));
    ASSERT_TRUE(fs::exists(archive_path));
    ASSERT_GT(fs::file_size(archive_path), 0u);

    // 3. Read archive back; manifest must parse and carry the ingredient.
    std::ifstream archive_in(archive_path, std::ios::binary);
    c2pa::Reader archive_reader(context, "application/c2pa", archive_in);
    std::string archive_json;
    ASSERT_NO_THROW(archive_json = archive_reader.json());

    auto parsed = json::parse(archive_json);
    ASSERT_TRUE(parsed.contains("active_manifest"));
    std::string active = parsed["active_manifest"];
    auto ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);
    EXPECT_EQ(ingredients[0]["title"], "C.jpg");
    EXPECT_EQ(ingredients[0]["relationship"], "parentOf");

    // 4. Builder2: fresh manifest + add the archive as an ingredient.
    auto builder2_manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto builder2 = c2pa::Builder(context, builder2_manifest);
    json ingredient_override = {
        {"title", "C.jpg"},
        {"relationship", "componentOf"}
    };
    ASSERT_NO_THROW(builder2.add_ingredient(ingredient_override.dump(), archive_path));

    // 5. Sign builder2 and validate signed output.
    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("archive_provenance_reused_output.jpg");
    std::vector<unsigned char> manifest_data;
    ASSERT_NO_THROW(manifest_data = builder2.sign(source_path, output_path, signer));
    ASSERT_FALSE(manifest_data.empty());

    auto out_reader = c2pa::Reader(context, output_path);
    std::string out_json;
    ASSERT_NO_THROW(out_json = out_reader.json());
    auto out_parsed = json::parse(out_json);
    std::string out_active = out_parsed["active_manifest"];
    auto out_ingredients = out_parsed["manifests"][out_active]["ingredients"];
    ASSERT_EQ(out_ingredients.size(), 1u);
    EXPECT_EQ(out_ingredients[0]["title"], "C.jpg");
    EXPECT_EQ(out_ingredients[0]["relationship"], "componentOf");
}

// Extract ingredient from archive, then reuse it.
// write_ingredient_archive per-ingredient -> selective add_ingredient_from_archive.
TEST_F(BuilderTest, ExtractIngredientsFromArchiveAndReuseUsingArchiveApi)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // Build a store with two ingredients, write each to its own ingredient archive.
    auto builder = c2pa::Builder(context, manifest_str);
    builder.add_ingredient(
        json({{"title", "A.jpg"}, {"relationship", "componentOf"}, {"instance_id", "catalog:ingredient-A"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));
    builder.add_ingredient(
        json({{"title", "C.jpg"}, {"relationship", "componentOf"}, {"instance_id", "catalog:ingredient-C"}}).dump(),
        c2pa_test::get_fixture_path("C.jpg"));

    std::stringstream streamA(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream streamC(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(builder.write_ingredient_archive("catalog:ingredient-A", streamA));
    ASSERT_NO_THROW(builder.write_ingredient_archive("catalog:ingredient-C", streamC));

    auto builder2 = c2pa::Builder(context, manifest_str);
    streamA.seekg(0);
    ASSERT_NO_THROW(builder2.add_ingredient_from_archive(streamA));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("extract_reuse_new.jpg");
    ASSERT_NO_THROW(builder2.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u) << "Only ingredient-A should be present";
    EXPECT_EQ(ingredients[0]["title"], "A.jpg");
}

// Link a parentOf ingredient archive to an opened action.
// The ingredient_id passed to write_ingredient_archive survives the round-trip via the
// archive metadata's archive:ingredient_id field, so the signing builder references the
// loaded ingredient by that producer-side id.
TEST_F(BuilderTest, LinkIngredientArchiveParentOfOpenedUsingArchiveApi)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto archive_builder = c2pa::Builder(context, manifest_str);
    archive_builder.add_ingredient(
        json({{"title", "photo.jpg"}, {"relationship", "parentOf"}, {"label", "my-ingredient"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(archive_builder.write_ingredient_archive("my-ingredient", stream));

    auto manifest_json = make_manifest_with_action("c2pa.opened", "my-ingredient",
        "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation");
    auto signing_builder = c2pa::Builder(context, manifest_json.dump());
    stream.seekg(0);
    ASSERT_NO_THROW(signing_builder.add_ingredient_from_archive(stream));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("link_ingredient_archive_parentof_opened.jpg");
    bool linked = verify_ingredient_linked(signing_builder, output_path, signer, "c2pa.opened");
    EXPECT_TRUE(linked);
}

// Link a componentOf ingredient archive to a placed action.
TEST_F(BuilderTest, LinkIngredientArchiveComponentOfPlacedUsingArchiveApi)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto archive_builder = c2pa::Builder(context, manifest_str);
    archive_builder.add_ingredient(
        json({{"title", "photo.jpg"}, {"relationship", "componentOf"}, {"label", "my-ingredient"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(archive_builder.write_ingredient_archive("my-ingredient", stream));

    auto manifest_json = make_manifest_with_action("c2pa.placed", "my-ingredient");
    auto signing_builder = c2pa::Builder(context, manifest_json.dump());
    stream.seekg(0);
    ASSERT_NO_THROW(signing_builder.add_ingredient_from_archive(stream));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("link_ingredient_archive_componentof_placed.jpg");
    bool linked = verify_ingredient_linked(signing_builder, output_path, signer, "c2pa.placed");
    EXPECT_TRUE(linked);
}

// Link same ingredient to 2 different actions
TEST_F(BuilderTest, LinkIngredientArchiveToBothOpenedAndPlacedUsingArchiveApi)
{
    auto settings = c2pa::Settings();
    // This test deliberately links one parentOf ingredient to both c2pa.opened and c2pa.placed.
    // A single ingredient cannot satisfy both parentOf and componentOf relationships at once, and
    // verify-after-sign rejects it. This test verifies the ingredient is linked with the setting disabled.
    settings.set("verify.verify_after_sign", "false");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto archive_builder = c2pa::Builder(context, manifest_str);
    archive_builder.add_ingredient(
        json({{"title", "photo.jpg"}, {"relationship", "parentOf"}, {"label", "shared-ingredient"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(archive_builder.write_ingredient_archive("shared-ingredient", stream));

    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions.v2"},
                {"data", {{"actions", json::array({
                    {
                        {"action", "c2pa.opened"},
                        {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"},
                        {"parameters", {{"ingredientIds", json::array({"shared-ingredient"})}}}
                    },
                    {
                        {"action", "c2pa.placed"},
                        {"parameters", {{"ingredientIds", json::array({"shared-ingredient"})}}}
                    }
                })}}}
            }
        })}
    };

    auto signing_builder = c2pa::Builder(context, manifest_json.dump());
    stream.seekg(0);
    ASSERT_NO_THROW(signing_builder.add_ingredient_from_archive(stream));

    auto signer = c2pa_test::create_test_signer();
    auto source_path = c2pa_test::get_fixture_path("A.jpg");
    auto output_path = get_temp_path("link_ingredient_archive_both.jpg");
    ASSERT_NO_THROW(signing_builder.sign(source_path, output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& manifest = parsed["manifests"][active];

    json opened_action, placed_action;
    bool found_opened = false, found_placed = false;
    for (auto& assertion : manifest["assertions"]) {
        if (assertion["label"] != "c2pa.actions.v2") continue;
        for (auto& action : assertion["data"]["actions"]) {
            if (action["action"] == "c2pa.opened") { opened_action = action; found_opened = true; }
            if (action["action"] == "c2pa.placed") { placed_action = action; found_placed = true; }
        }
    }
    ASSERT_TRUE(found_opened) << "c2pa.opened action not found";
    ASSERT_TRUE(found_placed) << "c2pa.placed action not found";

    ASSERT_TRUE(opened_action.contains("parameters"));
    ASSERT_TRUE(opened_action["parameters"].contains("ingredients"));
    ASSERT_EQ(opened_action["parameters"]["ingredients"].size(), 1u);

    ASSERT_TRUE(placed_action.contains("parameters"));
    ASSERT_TRUE(placed_action["parameters"].contains("ingredients"));
    ASSERT_EQ(placed_action["parameters"]["ingredients"].size(), 1u);

    std::string opened_url = opened_action["parameters"]["ingredients"][0]["url"];
    std::string placed_url = placed_action["parameters"]["ingredients"][0]["url"];
    EXPECT_EQ(opened_url, placed_url) << "Both actions should link the same ingredient archive";
    EXPECT_EQ(opened_url, "self#jumbf=c2pa.assertions/c2pa.ingredient.v3");
}

TEST_F(BuilderTest, LinkIngredientArchiveTwoRelationshipsOpenedParentPlacedComponent)
{
    auto context = c2pa::Context::ContextBuilder().create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // Producer archive #1: ingredient added as parentOf.
    auto producer_parent = c2pa::Builder(context, manifest_str);
    producer_parent.add_ingredient(
        json({{"title", "photo.jpg"}, {"relationship", "parentOf"}, {"label", "opened-ingredient"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));
    std::stringstream parent_stream(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(producer_parent.write_ingredient_archive("opened-ingredient", parent_stream));

    // Producer archive #2: same asset, ingredient kept as componentOf.
    auto producer_component = c2pa::Builder(context, manifest_str);
    producer_component.add_ingredient(
        json({{"title", "photo.jpg"}, {"relationship", "componentOf"}, {"label", "placed-ingredient"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));
    std::stringstream component_stream(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(producer_component.write_ingredient_archive("placed-ingredient", component_stream));

    // Signing builder: opened links the parentOf archive, placed links the componentOf archive.
    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions.v2"},
                {"data", {{"actions", json::array({
                    {
                        {"action", "c2pa.opened"},
                        {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"},
                        {"parameters", {{"ingredientIds", json::array({"opened-ingredient"})}}}
                    },
                    {
                        {"action", "c2pa.placed"},
                        {"parameters", {{"ingredientIds", json::array({"placed-ingredient"})}}}
                    }
                })}}}
            }
        })}
    };

    auto signing_builder = c2pa::Builder(context, manifest_json.dump());
    parent_stream.seekg(0);
    ASSERT_NO_THROW(signing_builder.add_ingredient_from_archive(parent_stream));
    component_stream.seekg(0);
    ASSERT_NO_THROW(signing_builder.add_ingredient_from_archive(component_stream));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("link_archive_two_relationships_opened_placed.jpg");
    ASSERT_NO_THROW(signing_builder.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& manifest = parsed["manifests"][active];

    auto& ingredients = manifest["ingredients"];
    ASSERT_EQ(ingredients.size(), 2u);
    bool found_parent = false, found_component = false;
    for (auto& ing : ingredients) {
        if (ing["relationship"] == "parentOf") found_parent = true;
        if (ing["relationship"] == "componentOf") found_component = true;
    }
    EXPECT_TRUE(found_parent) << "parentOf ingredient (opened archive) should be present";
    EXPECT_TRUE(found_component)
        << "componentOf ingredient (placed archive, relationship overridden) should be present";

    // Both actions present, each linking a single ingredient.
    json opened_action, placed_action;
    bool found_opened = false, found_placed = false;
    for (auto& assertion : manifest["assertions"]) {
        if (assertion["label"] != "c2pa.actions.v2") continue;
        for (auto& action : assertion["data"]["actions"]) {
            if (action["action"] == "c2pa.opened") { opened_action = action; found_opened = true; }
            if (action["action"] == "c2pa.placed") { placed_action = action; found_placed = true; }
        }
    }
    ASSERT_TRUE(found_opened) << "c2pa.opened action not found";
    ASSERT_TRUE(found_placed) << "c2pa.placed action not found";

    ASSERT_TRUE(opened_action["parameters"].contains("ingredients"));
    ASSERT_TRUE(placed_action["parameters"].contains("ingredients"));
    ASSERT_EQ(opened_action["parameters"]["ingredients"].size(), 1u);
    ASSERT_EQ(placed_action["parameters"]["ingredients"].size(), 1u);

    // The two actions link different ingredient archives.
    std::string opened_url = opened_action["parameters"]["ingredients"][0]["url"];
    std::string placed_url = placed_action["parameters"]["ingredients"][0]["url"];
    EXPECT_NE(opened_url, placed_url);
}

// Single ingredient archive, relationship decided at the moment it is added to the signing builder.
TEST_F(BuilderTest, LinkIngredientArchiveRelationshipOverriddenAtAddTime)
{
    auto context = c2pa::Context::ContextBuilder().create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto producer = c2pa::Builder(context, manifest_str);
    producer.add_ingredient(
        json({{"title", "photo.jpg"}, {"relationship", "componentOf"}, {"label", "shared-ingredient"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));
    std::stringstream archive(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(producer.write_ingredient_archive("shared-ingredient", archive));

    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions.v2"},
                {"data", {{"actions", json::array({
                    {
                        {"action", "c2pa.opened"},
                        {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"},
                        {"parameters", {{"ingredientIds", json::array({"opened-ingredient"})}}}
                    },
                    {
                        {"action", "c2pa.placed"},
                        {"parameters", {{"ingredientIds", json::array({"placed-ingredient"})}}}
                    }
                })}}}
            }
        })}
    };

    auto signing_builder = c2pa::Builder(context, manifest_json.dump());

    // Add the same archive, overriding the relationship at add time
    archive.seekg(0);
    ASSERT_NO_THROW(signing_builder.add_ingredient(
        json({{"relationship", "parentOf"}, {"label", "opened-ingredient"}}).dump(),
        "application/c2pa", archive));

    archive.seekg(0);
    ASSERT_NO_THROW(signing_builder.add_ingredient(
        json({{"relationship", "componentOf"}, {"label", "placed-ingredient"}}).dump(),
        "application/c2pa", archive));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("link_archive_relationship_override_at_add.jpg");
    ASSERT_NO_THROW(signing_builder.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& manifest = parsed["manifests"][active];

    // Both overridden relationships are present, from the single source archive.
    auto& ingredients = manifest["ingredients"];
    ASSERT_EQ(ingredients.size(), 2u);
    bool found_parent = false, found_component = false;
    for (auto& ing : ingredients) {
        if (ing["relationship"] == "parentOf") found_parent = true;
        if (ing["relationship"] == "componentOf") found_component = true;
    }
    EXPECT_TRUE(found_parent) << "relationship overridden to parentOf at add time";
    EXPECT_TRUE(found_component) << "relationship overridden to componentOf at add time";

    json opened_action, placed_action;
    bool found_opened = false, found_placed = false;
    for (auto& assertion : manifest["assertions"]) {
        if (assertion["label"] != "c2pa.actions.v2") continue;
        for (auto& action : assertion["data"]["actions"]) {
            if (action["action"] == "c2pa.opened") { opened_action = action; found_opened = true; }
            if (action["action"] == "c2pa.placed") { placed_action = action; found_placed = true; }
        }
    }
    ASSERT_TRUE(found_opened) << "c2pa.opened action not found";
    ASSERT_TRUE(found_placed) << "c2pa.placed action not found";

    ASSERT_TRUE(opened_action["parameters"].contains("ingredients"));
    ASSERT_TRUE(placed_action["parameters"].contains("ingredients"));
    ASSERT_EQ(opened_action["parameters"]["ingredients"].size(), 1u);
    ASSERT_EQ(placed_action["parameters"]["ingredients"].size(), 1u);

    std::string opened_url = opened_action["parameters"]["ingredients"][0]["url"];
    std::string placed_url = placed_action["parameters"]["ingredients"][0]["url"];
    EXPECT_NE(opened_url, placed_url)
        << "the two adds of the same archive should produce distinct linked ingredients";
}

// Catalog pattern: write per-ingredient archives indexed by instance_id,
// then assemble any subset directly via add_ingredient_from_archive.
TEST_F(BuilderTest, IngredientCatalogUsingArchiveApi)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // Build catalog: two ingredient archives indexed by instance_id.
    auto catalog_builder = c2pa::Builder(context, manifest_str);
    catalog_builder.add_ingredient(
        json({{"title", "photo-A.jpg"}, {"relationship", "componentOf"}, {"instance_id", "catalog:ingredient-A"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));
    catalog_builder.add_ingredient(
        json({{"title", "photo-B.jpg"}, {"relationship", "componentOf"}, {"instance_id", "catalog:ingredient-B"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream streamA(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream streamB(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(catalog_builder.write_ingredient_archive("catalog:ingredient-A", streamA));
    ASSERT_NO_THROW(catalog_builder.write_ingredient_archive("catalog:ingredient-B", streamB));

    // Assemble final builder using only ingredient-B from the catalog.
    auto final_builder = c2pa::Builder(context, manifest_str);
    streamB.seekg(0);
    ASSERT_NO_THROW(final_builder.add_ingredient_from_archive(streamB));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("catalog_new.jpg");
    ASSERT_NO_THROW(final_builder.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u) << "Only ingredient-B should be present";
    EXPECT_EQ(ingredients[0]["title"], "photo-B.jpg");
    if (ingredients[0].contains("instance_id")) {
        EXPECT_EQ(ingredients[0]["instance_id"], "catalog:ingredient-B");
    }
}

// Three ingredient archives with distinct ids loaded into one signing builder, with a
// single action linking all three.
TEST_F(BuilderTest, LinkThreeIngredientArchivesDistinctIdsUsingArchiveApi)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto build_archive = [&](const std::string& id, const std::string& title, std::stringstream& stream) {
        auto producer = c2pa::Builder(context, manifest_str);
        producer.add_ingredient(
            json({{"title", title}, {"relationship", "componentOf"}, {"label", id}}).dump(),
            c2pa_test::get_fixture_path("A.jpg"));
        producer.write_ingredient_archive(id, stream);
    };

    std::stringstream archive_a(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream archive_b(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream archive_c(std::ios::in | std::ios::out | std::ios::binary);
    build_archive("ing-a", "Ingredient A", archive_a);
    build_archive("ing-b", "Ingredient B", archive_b);
    build_archive("ing-c", "Ingredient C", archive_c);

    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions.v2"},
                {"data", {{"actions", json::array({
                    created_first_action(),
                    {
                        {"action", "c2pa.placed"},
                        {"parameters", {{"ingredientIds", json::array({"ing-a", "ing-b", "ing-c"})}}}
                    }
                })}}}
            }
        })}
    };

    auto signing_builder = c2pa::Builder(context, manifest_json.dump());
    archive_a.seekg(0);
    archive_b.seekg(0);
    archive_c.seekg(0);
    ASSERT_NO_THROW(signing_builder.add_ingredient_from_archive(archive_a));
    ASSERT_NO_THROW(signing_builder.add_ingredient_from_archive(archive_b));
    ASSERT_NO_THROW(signing_builder.add_ingredient_from_archive(archive_c));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("link_three_ingredient_archives.jpg");
    ASSERT_NO_THROW(signing_builder.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& manifest = parsed["manifests"][active];

    json placed_action;
    bool found_placed = false;
    for (auto& assertion : manifest["assertions"]) {
        if (assertion["label"] != "c2pa.actions.v2") continue;
        for (auto& action : assertion["data"]["actions"]) {
            if (action["action"] == "c2pa.placed") {
                placed_action = action;
                found_placed = true;
            }
        }
    }
    ASSERT_TRUE(found_placed);
    ASSERT_EQ(placed_action["parameters"]["ingredients"].size(), 3u);

    std::set<std::string> urls;
    for (auto& ing : placed_action["parameters"]["ingredients"]) {
        urls.insert(ing["url"].get<std::string>());
    }
    EXPECT_EQ(urls.size(), 3u) << "Three distinct ingredient URLs expected";
    EXPECT_TRUE(urls.count("self#jumbf=c2pa.assertions/c2pa.ingredient.v3"));
    EXPECT_TRUE(urls.count("self#jumbf=c2pa.assertions/c2pa.ingredient.v3__1"));
    EXPECT_TRUE(urls.count("self#jumbf=c2pa.assertions/c2pa.ingredient.v3__2"));
}

// Mix add_ingredient overloads with the dedicated ingredient archive API in the same
// builder. Action links every ingredient by its caller-supplied id regardless
// of how it was added.
TEST_F(BuilderTest, MixIngredientApisLinkByLabel)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto archive_producer = c2pa::Builder(context, manifest_str);
    archive_producer.add_ingredient(
        json({{"title", "via-archive.jpg"}, {"relationship", "componentOf"}, {"label", "via-archive"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));
    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    archive_producer.write_ingredient_archive("via-archive", archive_stream);

    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions.v2"},
                {"data", {{"actions", json::array({
                    created_first_action(),
                    {
                        {"action", "c2pa.placed"},
                        {"parameters", {{"ingredientIds", json::array({"via-add", "via-stream", "via-archive"})}}}
                    }
                })}}}
            }
        })}
    };

    auto signing_builder = c2pa::Builder(context, manifest_json.dump());

    signing_builder.add_ingredient(
        json({{"title", "via-add.jpg"}, {"relationship", "componentOf"}, {"label", "via-add"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));

    std::ifstream stream_src(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(stream_src.good());
    signing_builder.add_ingredient(
        json({{"title", "via-stream.jpg"}, {"relationship", "componentOf"}, {"label", "via-stream"}}).dump(),
        "image/jpeg",
        stream_src);
    stream_src.close();

    archive_stream.seekg(0);
    ASSERT_NO_THROW(signing_builder.add_ingredient_from_archive(archive_stream));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("mix_old_new_apis.jpg");
    ASSERT_NO_THROW(signing_builder.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& manifest = parsed["manifests"][active];

    json placed_action;
    for (auto& assertion : manifest["assertions"]) {
        if (assertion["label"] != "c2pa.actions.v2") continue;
        for (auto& action : assertion["data"]["actions"]) {
            if (action["action"] == "c2pa.placed") placed_action = action;
        }
    }
    ASSERT_FALSE(placed_action.is_null());
    ASSERT_EQ(placed_action["parameters"]["ingredients"].size(), 3u)
        << "All three ingredients should resolve via their caller-supplied ids";
}

TEST_F(BuilderTest, IngredientArchiveFallsBackToInstanceIdWhenNoLabel)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto producer = c2pa::Builder(context, manifest_str);
    producer.add_ingredient(
        json({{"title", "anon.jpg"},
              {"relationship", "componentOf"},
              {"instance_id", "xmp:iid:anon-fixture"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream archive(std::ios::in | std::ios::out | std::ios::binary);
    producer.write_ingredient_archive("xmp:iid:anon-fixture", archive);

    json manifest_json = {
        {"claim_generator_info", json::array({{{"name", "c2pa-test"}, {"version", "1.0"}}})},
        {"assertions", json::array({
            {
                {"label", "c2pa.actions.v2"},
                {"data", {{"actions", json::array({
                    created_first_action(),
                    {
                        {"action", "c2pa.placed"},
                        {"parameters", {{"ingredientIds", json::array({"xmp:iid:anon-fixture"})}}}
                    }
                })}}}
            }
        })}
    };

    auto signing_builder = c2pa::Builder(context, manifest_json.dump());
    archive.seekg(0);
    ASSERT_NO_THROW(signing_builder.add_ingredient_from_archive(archive));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("ingredient_archive_no_label_fallback.jpg");
    EXPECT_TRUE(verify_ingredient_linked(signing_builder, output_path, signer, "c2pa.placed"))
        << "instance_id passed as archive key should link the loaded ingredient to the action";
}

// Empty builder: write_ingredient_archive cannot fabricate an ingredient
// from the id alone. With no prior add_ingredient, the lookup fails.
TEST_F(BuilderTest, WriteIngredientArchiveWithoutAddIngredientThrows)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto builder = c2pa::Builder(context, manifest_str);
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_THROW(builder.write_ingredient_archive("never-added", stream), c2pa::C2paException);
}

// Id mismatch: the id arg must match an id previously supplied via
// add_ingredient's JSON (label or instance_id).
TEST_F(BuilderTest, WriteIngredientArchiveWithUnknownIdThrows)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto builder = c2pa::Builder(context, manifest_str);
    builder.add_ingredient(
        json({{"title", "photo.jpg"}, {"relationship", "componentOf"}, {"label", "real-id"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_THROW(builder.write_ingredient_archive("wrong-id", stream), c2pa::C2paException);
}

// When the builder has many ingredients, write_ingredient_archive
// puts only the requested one in the archive.
TEST_F(BuilderTest, WriteIngredientArchiveContainsOnlyTargetIngredient)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto builder = c2pa::Builder(context, manifest_str);
    builder.add_ingredient(
        json({{"title", "first.jpg"}, {"relationship", "componentOf"}, {"label", "first"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));
    builder.add_ingredient(
        json({{"title", "second.jpg"}, {"relationship", "componentOf"}, {"label", "second"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));
    builder.add_ingredient(
        json({{"title", "third.jpg"}, {"relationship", "componentOf"}, {"label", "third"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream archive(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(builder.write_ingredient_archive("second", archive));

    archive.seekg(0);
    c2pa::Reader reader(context, "application/c2pa", archive);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u) << "Archive should contain only the requested ingredient";
    EXPECT_EQ(ingredients[0]["title"], "second.jpg");
}

// instance_id set on add_ingredient survives write_ingredient_archive →
// add_ingredient_from_archive → signing, and is readable via Reader::json().
TEST_F(BuilderTest, InstanceIdSurvivesWriteIngredientArchiveRoundTripAndSigning)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();

    auto manifest_str = R"({
        "claim_generator_info": [{"name": "test", "version": "1.0"}],
        "assertions": [{"label": "c2pa.actions.v2", "data": {"actions": [
            {"action": "c2pa.created", "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"}
        ]}}]
    })";

    auto producer = c2pa::Builder(context, manifest_str);
    producer.add_ingredient(
        R"({"title": "A.jpg", "relationship": "componentOf", "instance_id": "iid:survival-test-001"})",
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(producer.write_ingredient_archive("iid:survival-test-001", archive_stream));

    auto consumer = c2pa::Builder(context, manifest_str);
    archive_stream.seekg(0);
    ASSERT_NO_THROW(consumer.add_ingredient_from_archive(archive_stream));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("iid_survives_ingredient_archive.jpg");
    ASSERT_NO_THROW(consumer.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);
    ASSERT_TRUE(ingredients[0].contains("instance_id"))
        << "instance_id should survive write_ingredient_archive + add_ingredient_from_archive + sign";
    EXPECT_EQ(ingredients[0]["instance_id"], "iid:survival-test-001");
}

// When neither caller nor XMP provides instance_id, the library auto-generates
// an xmp.iid:<uuid> value so the ingredient assertion is valid.
TEST_F(BuilderTest, InstanceIdAutoGeneratedWhenNotProvided)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();

    auto manifest_str = R"({
        "claim_generator_info": [{"name": "test", "version": "1.0"}],
        "assertions": [{"label": "c2pa.actions.v2", "data": {"actions": [
            {"action": "c2pa.created", "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"}
        ]}}]
    })";

    // Add ingredient with NO instance_id in JSON.
    // A.jpg has no XMP metadata, so the library must generate one.
    auto builder = c2pa::Builder(context, manifest_str);
    builder.add_ingredient(
        R"({"title": "A.jpg", "relationship": "componentOf"})",
        c2pa_test::get_fixture_path("A.jpg"));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("iid_auto_generated.jpg");
    ASSERT_NO_THROW(builder.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);

    // instance_id should be present and start with "xmp.iid:"
    ASSERT_TRUE(ingredients[0].contains("instance_id"))
        << "Library should auto-generate instance_id when none is provided";
    std::string iid = ingredients[0]["instance_id"];
    EXPECT_EQ(iid.substr(0, 8), "xmp.iid:")
        << "Auto-generated instance_id should start with xmp.iid:, got: " << iid;
}

// When add_ingredient JSON omits "relationship", the SDK applies "componentOf" as the default.
TEST_F(BuilderTest, RelationshipDefaultsToComponentOf)
{
    auto context = c2pa::Context::ContextBuilder().create_context();

    auto manifest_str = R"({
        "claim_generator_info": [{"name": "test", "version": "1.0"}],
        "assertions": [{"label": "c2pa.actions.v2", "data": {"actions": [
            {"action": "c2pa.created", "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"}
        ]}}]
    })";

    // Add ingredient with NO relationship in JSON.
    auto builder = c2pa::Builder(context, manifest_str);
    builder.add_ingredient(
        R"({"title": "A.jpg"})",
        c2pa_test::get_fixture_path("A.jpg"));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("relationship_defaults_component_of.jpg");
    ASSERT_NO_THROW(builder.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);
    ASSERT_TRUE(ingredients[0].contains("relationship"));
    EXPECT_EQ(ingredients[0]["relationship"], "componentOf")
        << "Omitting relationship should default to componentOf";
}

// instance_id set on add_ingredient survives to_archive and is
// readable via Reader::json() on the archive.
TEST_F(BuilderTest, InstanceIdSurvivesToArchiveAndReader)
{
    auto context = c2pa::Context::ContextBuilder().create_context();

    auto manifest_str = R"({
        "claim_generator_info": [{"name": "test", "version": "1.0"}]
    })";

    auto builder = c2pa::Builder(context, manifest_str);
    builder.add_ingredient(
        R"({"title": "A.jpg", "relationship": "componentOf", "instance_id": "iid:legacy-survival-001"})",
        c2pa_test::get_fixture_path("A.jpg"));

    auto archive_path = get_temp_path("iid_survives_to_archive.c2pa");
    ASSERT_NO_THROW(builder.to_archive(archive_path));

    auto reader = c2pa::Reader(context, archive_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_FALSE(ingredients.empty());

    bool found = false;
    for (auto& ing : ingredients) {
        if (ing.contains("instance_id") && ing["instance_id"] == "iid:legacy-survival-001") {
            found = true;
            EXPECT_EQ(ing["title"], "A.jpg");
        }
    }
    ASSERT_TRUE(found) << "instance_id should survive to_archive and be readable via Reader";
}

// The ingredient_id passed to write_ingredient_archive is restored as the label
// on the loaded ingredient in the signing builder. This means ingredientIds in
// actions must use the same value that was passed to write_ingredient_archive,
// regardless of whether that value was the label or instance_id at archive-creation time.
TEST_F(BuilderTest, IngredientIdPassedToWriteArchiveRestoredPrefersLabelInSigningBuilder)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();

    auto manifest_str = R"({"claim_generator_info": [{"name": "test", "version": "1.0"}]})";

    // Producer sets both label and instance_id on the ingredient.
    auto producer = c2pa::Builder(context, manifest_str);
    producer.add_ingredient(
        R"({"title": "A.jpg", "relationship": "componentOf",
            "label": "my-label", "instance_id": "iid:label-survival-test"})",
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    // write_ingredient_archive accepts label or instance_id as the lookup key.
    // The value passed here becomes the restored label in the signing builder.
    ASSERT_NO_THROW(producer.write_ingredient_archive("my-label", archive_stream));

    // Signing builder loads the archive.
    // ingredientIds must use "my-label": the value passed to write_ingredient_archive,
    // because that value is restored as the ingredient's label in the signing builder.
    auto signing_manifest = R"({
        "claim_generator_info": [{"name": "test", "version": "1.0"}],
        "assertions": [{
            "label": "c2pa.actions.v2",
            "data": {"actions": [
                {"action": "c2pa.created", "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"},
                {"action": "c2pa.placed",
                "parameters": {"ingredientIds": ["my-label"]}}]}
        }]
    })";

    auto consumer = c2pa::Builder(context, signing_manifest);
    archive_stream.seekg(0);
    ASSERT_NO_THROW(consumer.add_ingredient_from_archive(archive_stream));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("archive_ingredient_id_restored_as_label.jpg");
    ASSERT_NO_THROW(consumer.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);

    // instance_id survives the CBOR assertion round-trip.
    ASSERT_TRUE(ingredients[0].contains("instance_id"));
    EXPECT_EQ(ingredients[0]["instance_id"], "iid:label-survival-test");
}

// When the ingredient has only a label (no instance_id), the label is passed to
// write_ingredient_archive and is restored as the label in the signing builder.
// ingredientIds must use the label value.
TEST_F(BuilderTest, IngredientIdPassedToWriteArchiveRestoredAsLabelUsingLabelOnly)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();

    auto manifest_str = R"({"claim_generator_info": [{"name": "test", "version": "1.0"}]})";

    auto producer = c2pa::Builder(context, manifest_str);
    producer.add_ingredient(
        R"({"title": "A.jpg", "relationship": "componentOf", "label": "only-label"})",
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(producer.write_ingredient_archive("only-label", archive_stream));

    auto signing_manifest = R"({
        "claim_generator_info": [{"name": "test", "version": "1.0"}],
        "assertions": [{
            "label": "c2pa.actions.v2",
            "data": {"actions": [
                {"action": "c2pa.created", "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"},
                {"action": "c2pa.placed",
                "parameters": {"ingredientIds": ["only-label"]}}]}
        }]
    })";

    auto consumer = c2pa::Builder(context, signing_manifest);
    archive_stream.seekg(0);
    ASSERT_NO_THROW(consumer.add_ingredient_from_archive(archive_stream));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("archive_label_only_ingredient.jpg");
    ASSERT_NO_THROW(consumer.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);
    EXPECT_EQ(ingredients[0]["title"], "A.jpg");
}

// When the ingredient has only an instance_id (no label), the instance_id value is
// passed to write_ingredient_archive and is restored as the label in the signing
// builder. ingredientIds must use that instance_id value.
TEST_F(BuilderTest, IngredientIdPassedToWriteArchiveRestoredAsLabelUsingInstanceIdOnly)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();

    auto manifest_str = R"({"claim_generator_info": [{"name": "test", "version": "1.0"}]})";

    auto producer = c2pa::Builder(context, manifest_str);
    producer.add_ingredient(
        R"({"title": "A.jpg", "relationship": "componentOf", "instance_id": "iid:only-instance"})",
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    // No label set, so pass instance_id as the lookup key.
    ASSERT_NO_THROW(producer.write_ingredient_archive("iid:only-instance", archive_stream));

    // ingredientIds uses the instance_id value, same string passed to write_ingredient_archive.
    auto signing_manifest = R"({
        "claim_generator_info": [{"name": "test", "version": "1.0"}],
        "assertions": [{
            "label": "c2pa.actions.v2",
            "data": {"actions": [
                {"action": "c2pa.created", "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"},
                {"action": "c2pa.placed",
                "parameters": {"ingredientIds": ["iid:only-instance"]}}]}
        }]
    })";

    auto consumer = c2pa::Builder(context, signing_manifest);
    archive_stream.seekg(0);
    ASSERT_NO_THROW(consumer.add_ingredient_from_archive(archive_stream));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("archive_instance_id_only_ingredient.jpg");
    ASSERT_NO_THROW(consumer.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);

    // instance_id survives in the CBOR assertion.
    ASSERT_TRUE(ingredients[0].contains("instance_id"));
    EXPECT_EQ(ingredients[0]["instance_id"], "iid:only-instance");
}

// Both label and instance_id set. write_ingredient_archive called with label.
// The label is the lookup key and becomes the restored label in signing builder.
// ingredientIds must use the label value.
TEST_F(BuilderTest, IngredientIdPassedToWriteArchiveRestoredAsLabelBothSetUseLabelForLinking)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();

    auto manifest_str = R"({"claim_generator_info": [{"name": "test", "version": "1.0"}]})";

    auto producer = c2pa::Builder(context, manifest_str);
    producer.add_ingredient(
        R"({"title": "A.jpg", "relationship": "componentOf",
            "label": "lbl:both-set", "instance_id": "iid:both-set"})",
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(producer.write_ingredient_archive("lbl:both-set", archive_stream));

    auto signing_manifest = R"({
        "claim_generator_info": [{"name": "test", "version": "1.0"}],
        "assertions": [{
            "label": "c2pa.actions.v2",
            "data": {"actions": [
                {"action": "c2pa.created", "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"},
                {"action": "c2pa.placed",
                "parameters": {"ingredientIds": ["lbl:both-set"]}}]}
        }]
    })";

    auto consumer = c2pa::Builder(context, signing_manifest);
    archive_stream.seekg(0);
    ASSERT_NO_THROW(consumer.add_ingredient_from_archive(archive_stream));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("archive_both_set_pass_label.jpg");
    ASSERT_NO_THROW(consumer.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);

    // instance_id from the CBOR assertion survives unchanged.
    ASSERT_TRUE(ingredients[0].contains("instance_id"));
    EXPECT_EQ(ingredients[0]["instance_id"], "iid:both-set");
}

// Both label and instance_id set. write_ingredient_archive called with instance_id.
// The instance_id string becomes the restored label in signing builder.
// ingredientIds must use the instance_id value, not the label.
TEST_F(BuilderTest, IngredientIdPassedToWriteArchiveRestoredAsLabelBothSetUseInstanceIdForLinking)
{
    auto settings = c2pa::Settings();
    // builder.generate_c2pa_archive has become default
// settings.set("builder.generate_c2pa_archive", "true");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();

    auto manifest_str = R"({"claim_generator_info": [{"name": "test", "version": "1.0"}]})";

    auto producer = c2pa::Builder(context, manifest_str);
    producer.add_ingredient(
        R"({"title": "A.jpg", "relationship": "componentOf",
            "label": "lbl:both-set2", "instance_id": "iid:both-set2"})",
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    // Pass instance_id — lookup finds it via the instance_id branch of the OR-match.
    ASSERT_NO_THROW(producer.write_ingredient_archive("iid:both-set2", archive_stream));

    // ingredientIds must use "iid:both-set2", the value passed to write_ingredient_archive.
    // Using "lbl:both-set2" here would fail because the restored label is "iid:both-set2".
    auto signing_manifest = R"({
        "claim_generator_info": [{"name": "test", "version": "1.0"}],
        "assertions": [{
            "label": "c2pa.actions.v2",
            "data": {"actions": [
                {"action": "c2pa.created", "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"},
                {"action": "c2pa.placed",
                "parameters": {"ingredientIds": ["iid:both-set2"]}}]}
        }]
    })";

    auto consumer = c2pa::Builder(context, signing_manifest);
    archive_stream.seekg(0);
    ASSERT_NO_THROW(consumer.add_ingredient_from_archive(archive_stream));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("archive_both_set_pass_iid.jpg");
    ASSERT_NO_THROW(consumer.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);
    ASSERT_TRUE(ingredients[0].contains("instance_id"));
    EXPECT_EQ(ingredients[0]["instance_id"], "iid:both-set2");
}

// write_ingredient_archive requires the c2pa (JUMBF) working-store format.
TEST_F(BuilderTest, WriteIngredientArchiveThrowsWhenSettingDisabled)
{
    auto settings = c2pa::Settings();
    settings.set("builder.generate_c2pa_archive", "false");
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(std::move(settings))
        .create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto builder = c2pa::Builder(context, manifest_str);
    builder.add_ingredient(
        json({{"title", "photo.jpg"}, {"relationship", "componentOf"}, {"label", "my-ingredient"}}).dump(),
        c2pa_test::get_fixture_path("A.jpg"));

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_THROW(builder.write_ingredient_archive("my-ingredient", stream), c2pa::C2paException);
}

TEST_F(BuilderTest, ProvenanceSurvivesIngredientArchiveRoundTrip)
{
    auto context = c2pa::Context::ContextBuilder().create_context();
    auto manifest_str = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    // C.jpg is C2PA-signed, so the ingredient records an active manifest.
    auto producer = c2pa::Builder(context, manifest_str);
    producer.add_ingredient(
        json({{"title", "C.jpg"}, {"relationship", "componentOf"}, {"instance_id", "iid:provenance-roundtrip"}}).dump(),
        c2pa_test::get_fixture_path("C.jpg"));

    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(producer.write_ingredient_archive("iid:provenance-roundtrip", archive_stream));

    auto consumer = c2pa::Builder(context, manifest_str);
    archive_stream.seekg(0);
    ASSERT_NO_THROW(consumer.add_ingredient_from_archive(archive_stream));

    auto signer = c2pa_test::create_test_signer();
    auto output_path = get_temp_path("provenance_survives_ingredient_archive.jpg");
    ASSERT_NO_THROW(consumer.sign(c2pa_test::get_fixture_path("A.jpg"), output_path, signer));

    auto reader = c2pa::Reader(context, output_path);
    auto parsed = json::parse(reader.json());
    std::string active = parsed["active_manifest"];
    auto& ingredients = parsed["manifests"][active]["ingredients"];
    ASSERT_EQ(ingredients.size(), 1u);
    EXPECT_EQ(ingredients[0]["title"], "C.jpg");
    ASSERT_TRUE(ingredients[0].contains("instance_id"));
    EXPECT_EQ(ingredients[0]["instance_id"], "iid:provenance-roundtrip");
    EXPECT_TRUE(ingredients[0].contains("active_manifest"))
        << "Ingredient provenance (active_manifest) should survive the archive round-trip";
}

TEST(SignerTest, InvalidCredentialsThrowFromConstructor) {
    EXPECT_THROW(
        c2pa::Signer("Es256", "not a certificate", "not a private key"),
        c2pa::C2paException);
}

TEST_F(BuilderTest, SignSourceStreamWithExceptions) {
    auto image_path = c2pa_test::get_fixture_path("A.jpg");
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto context = std::make_shared<c2pa::Context>();
    auto builder = c2pa::Builder(context, manifest);
    auto signer = c2pa_test::create_test_signer();

    std::ifstream source(image_path, std::ios::binary);
    ASSERT_TRUE(source.is_open());
    source.exceptions(std::ios::failbit | std::ios::badbit);

    std::stringstream memory_buffer(std::ios::in | std::ios::out | std::ios::binary);
    std::iostream& dest = memory_buffer;

    try {
        auto manifest_data = builder.sign("image/jpeg", source, dest, signer);
        EXPECT_FALSE(manifest_data.empty());
    } catch (const c2pa::C2paException&) {
        // An error result is acceptable; crossing the FFI with an exception is not.
    }
}

TEST_F(BuilderTest, ArchiveToFstreamBackedCppOStream) {
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    auto context = std::make_shared<c2pa::Context>();
    auto builder = c2pa::Builder(context, manifest);

    auto archive_path = get_temp_path("archive_fstream_cppostream.bin");
    std::fstream dest(archive_path,
                      std::ios_base::binary | std::ios_base::trunc |
                          std::ios_base::in | std::ios_base::out);
    ASSERT_TRUE(dest.is_open());

    c2pa::CppOStream c_dest(dest);
    ASSERT_EQ(c2pa_builder_to_archive(builder.c2pa_builder(), c_dest.c_stream), 0);
    dest.flush();

    EXPECT_GT(std::filesystem::file_size(archive_path), 0u);
}

// Signing always needs the container type, which decides how it hashes.
class BuilderBlankFormatTest : public BuilderTest, public ::testing::WithParamInterface<std::string> {};

INSTANTIATE_TEST_SUITE_P(BuilderBlankFormatSignTests, BuilderBlankFormatTest,
                         ::testing::ValuesIn(c2pa_test::kBlankFormats));

TEST_P(BuilderBlankFormatTest, SignRejectsBlankFormat) {
    auto signer = c2pa_test::create_test_signer();
    auto builder = make_builder();

    std::ifstream source(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(source.is_open());
    std::stringstream dest(std::ios::in | std::ios::out | std::ios::binary);

    EXPECT_THROW({ builder.sign(GetParam(), source, dest, signer); }, c2pa::C2paException);
}

TEST_F(BuilderTest, SignBlankAndEmptyFormatsReportTheSameMessage) {
    auto signer = c2pa_test::create_test_signer();

    auto collect_message = [&](const std::string& format) {
        auto builder = make_builder();
        std::ifstream source(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
        EXPECT_TRUE(source.is_open());
        std::stringstream dest(std::ios::in | std::ios::out | std::ios::binary);
        try {
            builder.sign(format, source, dest, signer);
            return std::string("<no exception>");
        } catch (const c2pa::C2paException& e) {
            return std::string(e.what());
        }
    };

    const std::string from_empty = collect_message("");
    EXPECT_NE(from_empty, "<no exception>");
    EXPECT_EQ(from_empty, collect_message("   "));
}

TEST_F(BuilderTest, SignToExtensionlessDestinationThrows) {
    auto signer = c2pa_test::create_test_signer();
    auto builder = make_builder();
    fs::path dest = get_temp_path("signed-noext");

    EXPECT_THROW(
        { builder.sign(c2pa_test::get_fixture_path("A.jpg"), dest, signer); },
        c2pa::C2paException);
}

TEST_F(BuilderTest, SignDoesNotTruncateDestinationWhenFormatIsRejected) {
    // The destination opens with trunc, so the format is checked before it.
    auto signer = c2pa_test::create_test_signer();
    auto builder = make_builder();

    fs::path dest = get_temp_path("existing-destination-noext");
    const std::string original = "existing contents that must survive";
    {
        std::ofstream f(dest, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(f.is_open());
        f << original;
    }

    EXPECT_THROW(
        { builder.sign(c2pa_test::get_fixture_path("A.jpg"), dest, signer); },
        c2pa::C2paException);

    ASSERT_TRUE(fs::exists(dest));
    std::ifstream check(dest, std::ios::binary);
    std::stringstream after;
    after << check.rdbuf();
    EXPECT_EQ(after.str(), original);
}

TEST_F(BuilderTest, SignTrimsPaddedFormat) {
    // Trim stray whitespaces in format.
    auto signer = c2pa_test::create_test_signer();
    auto builder = make_builder();

    std::ifstream source(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(source.is_open());
    std::stringstream dest(std::ios::in | std::ios::out | std::ios::binary);

    EXPECT_NO_THROW({ builder.sign(" \t image/jpeg \n ", source, dest, signer); });
}

TEST_F(BuilderTest, SignWithUnsupportedFormatThrows) {
    auto signer = c2pa_test::create_test_signer();
    auto builder = make_builder();

    std::ifstream source(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(source.is_open());
    std::stringstream dest(std::ios::in | std::ios::out | std::ios::binary);

    EXPECT_THROW({ builder.sign("application/zip", source, dest, signer); },
                 c2pa::C2paException);
}

TEST_F(BuilderTest, SignRejectsReadOnlyFormat) {
    // Signing validates against the writable formats.
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

    auto signer = c2pa_test::create_test_signer();
    auto builder = make_builder();
    std::ifstream source(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(source.is_open());
    std::stringstream dest(std::ios::in | std::ios::out | std::ios::binary);

    EXPECT_THROW({ builder.sign(read_only, source, dest, signer); }, c2pa::C2paException);
}

TEST_F(BuilderTest, SignMessageNamesFormatNotPath) {
    auto signer = c2pa_test::create_test_signer();
    auto builder = make_builder();
    fs::path dest = get_temp_path("message-probe-noext");

    try {
        builder.sign(c2pa_test::get_fixture_path("A.jpg"), dest, signer);
        FAIL() << "Expected an extensionless destination to be rejected";
    } catch (const c2pa::C2paException& e) {
        const std::string msg = e.what();
        EXPECT_EQ(msg.find(dest.string()), std::string::npos) << msg;
        EXPECT_EQ(msg.find("message-probe-noext"), std::string::npos) << msg;
    }
}

TEST_F(BuilderTest, AddIngredientAcceptsBlankFormat) {
    auto builder = make_builder();
    const std::string json = R"({"title": "A.jpg", "relationship": "componentOf"})";

    for (const std::string& blank : {std::string(""), std::string("   ")}) {
        std::ifstream src(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
        ASSERT_TRUE(src.is_open());
        EXPECT_NO_THROW({ builder.add_ingredient(json, blank, src); });
    }
}

TEST_F(BuilderTest, AddIngredientAcceptsUnknownFormat) {
    auto builder = make_builder();
    const std::string json = R"({"title": "A.jpg", "relationship": "componentOf"})";

    for (const std::string& format : {std::string("application/zip"), std::string("zzz")}) {
        std::ifstream src(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
        ASSERT_TRUE(src.is_open());
        EXPECT_NO_THROW({ builder.add_ingredient(json, format, src); }) << "format: " << format;
    }
}

TEST_F(BuilderTest, AddIngredientAcceptsReadOnlyFormat) {
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

    auto builder = make_builder();
    std::ifstream src(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(src.is_open());
    EXPECT_NO_THROW({
        builder.add_ingredient(R"({"title": "A.jpg", "relationship": "componentOf"})", read_only, src);
    }) << "read-only format: " << read_only;
}

TEST_F(BuilderTest, AddIngredientFromExtensionlessPath) {
    auto builder = make_builder();
    fs::path ingredient = get_temp_path("ingredient-noext");
    {
        std::ifstream src(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
        ASSERT_TRUE(src.is_open());
        std::ofstream out(ingredient, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << src.rdbuf();
    }

    EXPECT_NO_THROW(
        { builder.add_ingredient(R"({"title": "A.jpg", "relationship": "componentOf"})", ingredient); });
}

TEST_F(BuilderTest, AddIngredientFromPathWithUnknownExtension) {
    auto builder = make_builder();
    fs::path ingredient = get_temp_path("ingredient-unknown.zzz");
    {
        std::ifstream src(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
        ASSERT_TRUE(src.is_open());
        std::ofstream out(ingredient, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << src.rdbuf();
    }

    EXPECT_NO_THROW(
        { builder.add_ingredient(R"({"title": "A.jpg", "relationship": "componentOf"})", ingredient); });
}

TEST_F(BuilderTest, AcceptsSupportedBareExtension) {
    auto signer = c2pa_test::create_test_signer();
    auto builder = make_builder();

    std::ifstream source(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
    ASSERT_TRUE(source.is_open());
    std::stringstream dest(std::ios::in | std::ios::out | std::ios::binary);

    EXPECT_NO_THROW({ builder.sign("jpg", source, dest, signer); });
}

TEST_F(BuilderTest, ArchiveWithEmptyFormatThrows) {
    // A c2pa archive carries no signature that identifies the format it came for/from,
    // so reading one back requires naming the format.
    auto builder = make_builder();
    builder.add_ingredient(R"({"title": "C.jpg", "relationship": "componentOf"})",
                           c2pa_test::get_fixture_path("C.jpg"));

    std::ostringstream archive_out(std::ios::binary);
    ASSERT_NO_THROW(builder.to_archive(archive_out));
    const std::string archive_bytes = archive_out.str();
    ASSERT_FALSE(archive_bytes.empty());

    {   // Control: naming the format works.
        std::istringstream in(archive_bytes, std::ios::binary);
        auto ctx = std::make_shared<c2pa::Context>();
        c2pa::Reader reader(ctx, "application/c2pa", in);
        EXPECT_FALSE(reader.json().empty());
    }
    {   // Same bytes, no format: the container cannot be identified.
        std::istringstream in(archive_bytes, std::ios::binary);
        auto ctx = std::make_shared<c2pa::Context>();
        EXPECT_THROW({ c2pa::Reader reader(ctx, "", in); }, c2pa::C2paException);
    }
}

TEST_F(BuilderTest, SignProducesEquivalentManifestForEverySpelling) {
    auto signer = c2pa_test::create_test_signer();

    for (const char* format :
         {"image/jpeg", "IMAGE/JPEG", "Image/Jpeg", "jpg", "JPG"}) {
        auto builder = make_builder();

        std::ifstream source(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
        ASSERT_TRUE(source.is_open());
        std::stringstream dest(std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_NO_THROW({ builder.sign(format, source, dest, signer); }) << "format: " << format;

        dest.seekg(0);
        auto ctx = std::make_shared<c2pa::Context>();
        c2pa::Reader reader(ctx, "image/jpeg", dest);

        auto store = json::parse(reader.json());
        const auto& active = store["manifests"][store["active_manifest"].get<std::string>()];
        ASSERT_TRUE(active.contains("thumbnail"))
            << "signing with " << format << " dropped the claim thumbnail";
        EXPECT_EQ(active["thumbnail"]["format"], "image/jpeg");

        // The reference must resolve to a real JPEG, not just be present.
        std::stringstream thumbnail(std::ios::in | std::ios::out | std::ios::binary);
        reader.get_resource(active["thumbnail"]["identifier"].get<std::string>(), thumbnail);
        const std::string bytes = thumbnail.str();
        EXPECT_GT(bytes.size(), 1000u)
            << "signing with " << format << " produced an empty claim thumbnail";
        ASSERT_GE(bytes.size(), 3u);
        EXPECT_EQ(bytes.compare(0, 3, "\xff\xd8\xff"), 0)
            << "signing with " << format << " produced a thumbnail that is not a JPEG";
    }
}

TEST_F(BuilderTest, SidecarSignAcceptsUnknownFormat) {
    // A sidecar manifest travels beside the asset, so the container never has
    // to be parsed or rewritten and any format is signable.
    auto signer = c2pa_test::create_test_signer();

    for (const std::string& format : {std::string("application/zip"), std::string("zzz")}) {
        auto builder = make_builder();
        builder.set_no_embed();

        std::ifstream source(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
        ASSERT_TRUE(source.is_open());
        std::stringstream dest(std::ios::in | std::ios::out | std::ios::binary);

        std::vector<unsigned char> manifest;
        ASSERT_NO_THROW({ manifest = builder.sign(format, source, dest, signer); })
            << "format: " << format;
        EXPECT_FALSE(manifest.empty()) << "format: " << format;
    }
}

// The boundary this binding enforces: a blank format is rejected wherever the
// library would otherwise misclassify it, and nothing else is second-guessed.
// Tightening the lax paths or loosening the strict ones fails one of these.

TEST_F(BuilderTest, BlankFormatRejectedWhereItWouldMisclassify) {
    auto signer = c2pa_test::create_test_signer();

    for (const auto& blank : c2pa_test::kBlankFormats) {
        auto builder = make_builder();
        EXPECT_THROW({ builder.needs_placeholder(blank); }, c2pa::C2paException)
            << "needs_placeholder accepted a blank format";
        EXPECT_THROW({ builder.placeholder(blank); }, c2pa::C2paException)
            << "placeholder accepted a blank format";

        std::ifstream source(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
        ASSERT_TRUE(source.is_open());
        std::stringstream dest(std::ios::in | std::ios::out | std::ios::binary);
        EXPECT_THROW({ builder.sign(blank, source, dest, signer); }, c2pa::C2paException)
            << "sign accepted a blank format";
    }
}

TEST_F(BuilderTest, LaxPathsAcceptWhatTheLibraryAccepts) {
    const auto readable = c2pa::Reader::supported_mime_types();
    const auto writable = c2pa::Builder::supported_mime_types();

    std::vector<std::string> formats{"application/zip", "zzz", "", "   "};
    for (const auto& format : readable) {
        if (std::find(writable.begin(), writable.end(), format) == writable.end()) {
            formats.push_back(format);
            break;
        }
    }

    auto builder = make_builder();
    for (const auto& format : formats) {
        std::ifstream source(c2pa_test::get_fixture_path("A.jpg"), std::ios::binary);
        ASSERT_TRUE(source.is_open());
        EXPECT_NO_THROW({
            builder.add_ingredient(R"({"title": "A.jpg", "relationship": "componentOf"})",
                                   format, source);
        }) << "add_ingredient rejected format: " << format;
    }
}
