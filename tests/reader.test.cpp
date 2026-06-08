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
#include <filesystem>
#include <fstream>

#include "include/test_utils.hpp"

using nlohmann::json;
namespace fs = std::filesystem;

// Test fixture for reader tests with automatic cleanup
class ReaderTest : public ::testing::Test {
protected:
    std::vector<fs::path> temp_files;
    bool cleanup_temp_files = true;  // Set to false to keep temp files for debugging

    // Get path for temp reader test files in build directory
    fs::path get_temp_path(const std::string& name) {
        fs::path current_dir = fs::path(__FILE__).parent_path();
        fs::path build_dir = current_dir.parent_path() / "build";
        if (!fs::exists(build_dir)) {
            fs::create_directories(build_dir);
        }
        fs::path temp_path = build_dir / ("reader-" + name);
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

TEST(Reader, SupportedTypes) {
  auto supported_types = c2pa::Reader::supported_mime_types();
  EXPECT_TRUE(std::find(supported_types.begin(), supported_types.end(), "image/jpeg") != supported_types.end());
  EXPECT_TRUE(std::find(supported_types.begin(), supported_types.end(), "image/png") != supported_types.end());
};

class StreamWithManifestTests
    : public ::testing::TestWithParam<std::tuple<std::string, std::string, std::string>> {
public:
  static void test_stream_with_manifest(const std::string& filename, const std::string& mime_type, const std::string& expected_content) {
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir.parent_path() / "tests" / "fixtures" / filename;
    ASSERT_TRUE(std::filesystem::exists(test_file)) << "Test file does not exist: " << test_file;

    // read the new manifest and display the JSON
    std::ifstream file_stream(test_file, std::ios::binary);
    ASSERT_TRUE(file_stream.is_open()) << "Failed to open file: " << test_file;

    auto reader = c2pa::Reader(mime_type, file_stream);
    auto manifest_store_json = reader.json();
    EXPECT_TRUE(manifest_store_json.find(expected_content) != std::string::npos);
  }
};

INSTANTIATE_TEST_SUITE_P(ReaderStreamWithManifestTests, StreamWithManifestTests,
                         ::testing::Values(
                             // (filename, type or mimetype, expected_content = Title from the manifest)
                             std::make_tuple("video1.mp4", "video/mp4", "My Title"),
                             std::make_tuple("sample1_signed.wav", "wav", "sample1_signed.wav"),
                             std::make_tuple("C.dng", "DNG", "C.jpg")));

TEST_P(StreamWithManifestTests, StreamWithManifest) {
    auto filename = std::get<0>(GetParam());
    auto mime_type = std::get<1>(GetParam());
    auto expected_content = std::get<2>(GetParam());
    test_stream_with_manifest(filename, mime_type, expected_content);
}

TEST_F(ReaderTest, MultipleReadersSameFile)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/C.jpg";
    ASSERT_TRUE(std::filesystem::exists(test_file)) << "Test file does not exist: " << test_file;

    // create multiple readers from the same file
    auto reader1 = c2pa::Reader(test_file);
    auto reader2 = c2pa::Reader(test_file);
    auto reader3 = c2pa::Reader(test_file);

    // all readers should be able to read the manifest independently
    auto manifest1 = reader1.json();
    auto manifest2 = reader2.json();
    auto manifest3 = reader3.json();

    // all manifests should be identical
    EXPECT_EQ(manifest1, manifest2);
    EXPECT_EQ(manifest2, manifest3);
    EXPECT_EQ(manifest1, manifest3);

    // all readers should report the same embedded status
    EXPECT_EQ(reader1.is_embedded(), reader2.is_embedded());
    EXPECT_EQ(reader2.is_embedded(), reader3.is_embedded());

    // all readers should report the same remote URL status
    EXPECT_EQ(reader1.remote_url().has_value(), reader2.remote_url().has_value());
    EXPECT_EQ(reader2.remote_url().has_value(), reader3.remote_url().has_value());

    // verify the manifest
    EXPECT_TRUE(manifest1.find("C.jpg") != std::string::npos);
    EXPECT_TRUE(manifest2.find("C.jpg") != std::string::npos);
    EXPECT_TRUE(manifest3.find("C.jpg") != std::string::npos);
};

TEST_F(ReaderTest, MultipleReadersSameFileUsingContext)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/C.jpg";
    ASSERT_TRUE(std::filesystem::exists(test_file)) << "Test file does not exist: " << test_file;

    // Create a Context
    auto context = c2pa::Context();

    // create multiple readers from the same file using the context
    auto reader1 = c2pa::Reader(context, test_file);
    auto reader2 = c2pa::Reader(context, test_file);
    auto reader3 = c2pa::Reader(context, test_file);

    // all readers should be able to read the manifest independently
    auto manifest1 = reader1.json();
    auto manifest2 = reader2.json();
    auto manifest3 = reader3.json();

    // all manifests should be identical
    EXPECT_EQ(manifest1, manifest2);
    EXPECT_EQ(manifest2, manifest3);
    EXPECT_EQ(manifest1, manifest3);

    // all readers should report the same embedded status
    EXPECT_EQ(reader1.is_embedded(), reader2.is_embedded());
    EXPECT_EQ(reader2.is_embedded(), reader3.is_embedded());

    // all readers should report the same remote URL status
    EXPECT_EQ(reader1.remote_url().has_value(), reader2.remote_url().has_value());
    EXPECT_EQ(reader2.remote_url().has_value(), reader3.remote_url().has_value());

    // verify the manifest
    EXPECT_TRUE(manifest1.find("C.jpg") != std::string::npos);
    EXPECT_TRUE(manifest2.find("C.jpg") != std::string::npos);
    EXPECT_TRUE(manifest3.find("C.jpg") != std::string::npos);
};

TEST_F(ReaderTest, VideoStreamWithManifestUsingExtension) {
  fs::path current_dir = fs::path(__FILE__).parent_path();
  fs::path test_file = current_dir.parent_path() / "tests" / "fixtures" / "video1.mp4";
  ASSERT_TRUE(std::filesystem::exists(test_file)) << "Test file does not exist: " << test_file;

  // read the new manifest and display the JSON
  std::ifstream file_stream(test_file, std::ios::binary);
  ASSERT_TRUE(file_stream.is_open()) << "Failed to open video file: " << test_file;

  auto reader = c2pa::Reader("mp4", file_stream);
  auto manifest_store_json = reader.json();
  EXPECT_TRUE(manifest_store_json.find("My Title") != std::string::npos);
};

TEST_F(ReaderTest, VideoStreamWithManifestUsingExtensionUsingContext) {
  fs::path current_dir = fs::path(__FILE__).parent_path();
  fs::path test_file = current_dir.parent_path() / "tests" / "fixtures" / "video1.mp4";
  ASSERT_TRUE(std::filesystem::exists(test_file)) << "Test file does not exist: " << test_file;

  // read the new manifest and display the JSON
  std::ifstream file_stream(test_file, std::ios::binary);
  ASSERT_TRUE(file_stream.is_open()) << "Failed to open video file: " << test_file;

  // Create a Context and pass it to the Reader
  auto context = c2pa::Context();
  auto reader = c2pa::Reader(context, "mp4", file_stream);
  auto manifest_store_json = reader.json();
  EXPECT_TRUE(manifest_store_json.find("My Title") != std::string::npos);
};

class FileWithManifestTests
    : public ::testing::TestWithParam<std::tuple<std::string, std::string>> {
public:
  static void test_file_with_manifest(const std::string& filename, const std::string& expected_content) {
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures" / filename;

    // Read the manifest from the file
    auto reader = c2pa::Reader(test_file);
    auto manifest_store_json = reader.json();

    // Simple content checks
    EXPECT_TRUE(manifest_store_json.find(expected_content) != std::string::npos);
  }
};

INSTANTIATE_TEST_SUITE_P(ReaderFileWithManifestTests, FileWithManifestTests,
                         ::testing::Values(
                             // (filename, expected_content = Title from the manifest)
                             std::make_tuple("C.jpg", "C.jpg"),
                             std::make_tuple("video1.mp4", "My Title"),
                             std::make_tuple("sample1_signed.wav", "sample1_signed.wav"),
                             std::make_tuple("C.dng", "C.jpg")));

TEST_P(FileWithManifestTests, FileWithManifest) {
    auto filename = std::get<0>(GetParam());
    auto expected_content = std::get<1>(GetParam());
    test_file_with_manifest(filename, expected_content);
}

TEST_F(ReaderTest, ImageFileWithManifestMultipleCalls)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/C.jpg";

    // read the new manifest and display the JSON
    auto reader = c2pa::Reader(test_file);
    auto manifest_store_json = reader.json();
    EXPECT_TRUE(manifest_store_json.find("C.jpg") != std::string::npos);

    auto manifest_store_json_2 = reader.json();
    EXPECT_TRUE(manifest_store_json_2.find("C.jpg") != std::string::npos);

    auto manifest_store_json_3 = reader.json();
    EXPECT_TRUE(manifest_store_json_3.find("C.jpg") != std::string::npos);
};

TEST_F(ReaderTest, FileNoManifest)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/A.jpg";
    EXPECT_THROW({ auto reader = c2pa::Reader(test_file); }, c2pa::C2paException);
};

TEST_F(ReaderTest, FromAssetNoManifestReturnsNullopt)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/A.jpg";
    auto context = c2pa::Context();
    auto reader = c2pa::Reader::from_asset(context, test_file);
    EXPECT_FALSE(reader.has_value());
}

TEST_F(ReaderTest, FromAssetWithManifestReturnsReader)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/C.jpg";
    auto context = c2pa::Context();
    auto reader = c2pa::Reader::from_asset(context, test_file);
    ASSERT_TRUE(reader.has_value());
    EXPECT_TRUE(reader->json().find("C.jpg") != std::string::npos);
}

TEST_F(ReaderTest, FromAssetStreamNoManifestReturnsNullopt)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/A.jpg";
    std::ifstream stream(test_file, std::ios::binary);
    ASSERT_TRUE(stream);
    auto context = c2pa::Context();
    auto reader = c2pa::Reader::from_asset(context, "image/jpeg", stream);
    EXPECT_FALSE(reader.has_value());
}

TEST_F(ReaderTest, FromAssetStreamWithManifestReturnsReader)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/C.jpg";
    std::ifstream stream(test_file, std::ios::binary);
    ASSERT_TRUE(stream);
    auto context = c2pa::Context();
    auto reader = c2pa::Reader::from_asset(context, "image/jpeg", stream);
    ASSERT_TRUE(reader.has_value());
    EXPECT_TRUE(reader->json().find("C.jpg") != std::string::npos);
}

TEST_F(ReaderTest, FromAssetEmptyFileStillThrows)
{
    fs::path empty_file = get_temp_path("from_asset_empty");
    {
        std::ofstream f(empty_file, std::ios::binary);
        ASSERT_TRUE(f);
    }
    auto context = c2pa::Context();
    EXPECT_THROW(
        {
            (void)c2pa::Reader::from_asset(context, empty_file);
        },
        c2pa::C2paException);
}

class RemoteUrlTests
    : public ::testing::TestWithParam<std::tuple<std::string, bool>> {
public:
  static c2pa::Reader reader_from_fixture(const std::string &file_name) {
    auto current_dir = fs::path(__FILE__).parent_path();
    auto fixture = current_dir / "../tests/fixtures" / file_name;
    std::ifstream stream(fixture, std::ios::binary);
    return { "image/jpeg", stream  };
  }
};

INSTANTIATE_TEST_SUITE_P(ReaderRemoteUrlTests, RemoteUrlTests,
                         ::testing::Values(
                             // (fixture filename, is_remote_manifest)
                             std::make_tuple("cloud.jpg", true),
                             std::make_tuple("C.jpg", false)));

TEST_P(RemoteUrlTests, RemoteUrl) {
    auto reader = reader_from_fixture(std::get<0>(GetParam()));
    auto expected_is_remote = std::get<1>(GetParam());
    EXPECT_EQ(reader.remote_url().has_value(), expected_is_remote);
}

TEST_P(RemoteUrlTests, IsEmbeddedTest) {
    auto reader = reader_from_fixture(std::get<0>(GetParam()));
    auto expected_is_remote = std::get<1>(GetParam());
    EXPECT_EQ(reader.is_embedded(), !expected_is_remote);
}

TEST_F(ReaderTest, HasManifestUtf8Path) {
    auto current_dir = fs::path(__FILE__).parent_path();
    #ifdef _WIN32
      auto test_file = current_dir.parent_path() / "tests" / "fixtures" / L"CÖÄ_.jpg";
    #else
      auto test_file = current_dir.parent_path() / "tests" / "fixtures" / "CÖÄ_.jpg";
    #endif
    ASSERT_TRUE(std::filesystem::exists(test_file)) << "Test file does not exist: " << test_file;

    std::ifstream stream(test_file, std::ios::binary);
    auto reader = c2pa::Reader("image/jpeg", stream);

    EXPECT_FALSE(reader.remote_url());
    EXPECT_TRUE(reader.is_embedded());
}

TEST_F(ReaderTest, HasManifestUtf8PathUsingContext) {
    auto current_dir = fs::path(__FILE__).parent_path();
    #ifdef _WIN32
      auto test_file = current_dir.parent_path() / "tests" / "fixtures" / L"CÖÄ_.jpg";
    #else
      auto test_file = current_dir.parent_path() / "tests" / "fixtures" / "CÖÄ_.jpg";
    #endif
    ASSERT_TRUE(std::filesystem::exists(test_file)) << "Test file does not exist: " << test_file;

    std::ifstream stream(test_file, std::ios::binary);

    // Create a Context and pass it to the Reader
    auto context = c2pa::Context();
    auto reader = c2pa::Reader(context, "image/jpeg", stream);

    EXPECT_FALSE(reader.remote_url());
    EXPECT_TRUE(reader.is_embedded());
}

TEST_F(ReaderTest, FileNotFound)
{
    try
    {
        auto reader = c2pa::Reader("foo/xxx.xyz");
        FAIL() << "Expected std::system_error";
    }
    catch (const std::system_error &e)
    {
        EXPECT_TRUE(std::string(e.what()).find("Failed to open file") != std::string::npos);
    }
    catch (...)
    {
        FAIL() << "Expected std::system_error for file not found";
    }
};

TEST_F(ReaderTest, StreamClosed)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/C.jpg";
    ASSERT_TRUE(std::filesystem::exists(test_file)) << "Test file does not exist: " << test_file;

    // create a stream and close it before creating the reader
    std::ifstream file_stream(test_file, std::ios::binary);
    ASSERT_TRUE(file_stream.is_open()) << "Failed to open file: " << test_file;
    file_stream.close(); // Close the stream before creating reader

    // attempt to create reader with closed stream should throw exception
    EXPECT_THROW({
        auto reader = c2pa::Reader("image/jpeg", file_stream);
    }, c2pa::C2paException);
};

TEST_F(ReaderTest, ReadManifestWithTrustConfiguredJsonSettings)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path signed_image_path = current_dir / "../tests/fixtures/for_trusted_read.jpg";

    // Trust is based on a chain of trusted certificates. When signing, we may need to know
    // if the ingredients are trusted at time of signing, so we benefit from having a context
    // already configured with that trust to use with our Builder and Reader.
    fs::path settings_path = current_dir / "../tests/fixtures/settings/test_settings_example.json";
    auto settings = c2pa_test::read_text_file(settings_path);
    auto trusted_context = c2pa::Context(settings);

    // When reading, the Reader also needs to know about trust, to determine the manifest validation state
    // If there is a valid trust chain, the manifest will be in validation_state Trusted.
    auto reader = c2pa::Reader(trusted_context, signed_image_path);
    std::string read_json_manifest;
    ASSERT_NO_THROW(read_json_manifest = reader.json());
    ASSERT_FALSE(read_json_manifest.empty());

    json parsed_manifest_json = json::parse(read_json_manifest);

    ASSERT_TRUE(parsed_manifest_json["validation_state"] == "Trusted");
}

TEST_F(ReaderTest, ReaderFromIStreamWithContext)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path signed_path = current_dir / "../tests/fixtures/sample1_signed.wav";

    if (!std::filesystem::exists(signed_path))
    {
        GTEST_SKIP() << "Fixture not found: " << signed_path;
    }

    auto context = c2pa::Context();
    std::ifstream stream(signed_path, std::ios::binary);
    ASSERT_TRUE(stream) << "Failed to open " << signed_path;

    c2pa::Reader reader(context, "audio/wav", stream);
    std::string json_result;
    ASSERT_NO_THROW(json_result = reader.json());
    ASSERT_FALSE(json_result.empty());
}

TEST_F(ReaderTest, EmptyFileReturnsError)
{
    fs::path empty_file = get_temp_path("empty_error_handling_test");
    {
        std::ofstream f(empty_file, std::ios::binary);
        ASSERT_TRUE(f) << "Failed to create empty test file";
    }
    EXPECT_THROW(
        {
            c2pa::Reader reader(empty_file);
        },
        c2pa::C2paException);
}

TEST_F(ReaderTest, TruncatedFileReturnsError)
{
    fs::path truncated_file = get_temp_path("truncated_error_handling_test");
    {
        std::ofstream f(truncated_file, std::ios::binary);
        ASSERT_TRUE(f);
        f.write("\xff\xd8\xff", 3);
    }
    EXPECT_THROW(
        {
            c2pa::Reader reader(truncated_file);
        },
        c2pa::C2paException);
}

TEST(ReaderErrorHandling, UnsupportedMimeTypeReturnsError)
{
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/C.jpg";
    ASSERT_TRUE(std::filesystem::exists(test_file)) << "Test file does not exist: " << test_file;
    std::ifstream stream(test_file, std::ios::binary);
    ASSERT_TRUE(stream);
    EXPECT_THROW(
        {
            c2pa::Reader reader("application/x-unsupported-c2pa-test", stream);
        },
        c2pa::C2paException);
}

TEST(ReaderErrorHandling, EmptyStreamBehavesTheSameWithAndWithoutContext)
{
    std::stringstream empty_stream1, empty_stream2;
    std::string format = "image/jpeg";

    // Without context
    EXPECT_THROW({
        c2pa::Reader reader(format, empty_stream1);
    }, c2pa::C2paException);

    // With context
    auto ctx = c2pa::Context();
    EXPECT_THROW({
        c2pa::Reader reader(ctx, format, empty_stream2);
    }, c2pa::C2paException);
}

TEST(ReaderErrorHandling, NonExistentFileBehavesTheSameWithAndWithoutContext)
{
    fs::path nonexistent = "/nonexistent/path/to/file.jpg";

    // Without context
    EXPECT_THROW({
        c2pa::Reader reader(nonexistent);
    }, std::system_error);

    // With context
    auto ctx = c2pa::Context();
    EXPECT_THROW({
        c2pa::Reader reader(ctx, nonexistent);
    }, std::system_error);
}

TEST(ReaderErrorHandling, InvalidStreamBehavesTheSameWithAndWithoutContext)
{
    // Create truncated/invalid JPEG data
    std::vector<uint8_t> bad_data = {0xFF, 0xD8, 0xFF}; // Incomplete JPEG
    std::string data_str(bad_data.begin(), bad_data.end());
    std::stringstream stream1(data_str);
    std::stringstream stream2(data_str);
    std::string format = "image/jpeg";

    bool without_context_throws = false;
    bool with_context_throws = false;

    try {
        c2pa::Reader reader(format, stream1);
    } catch (...) {
        without_context_throws = true;
    }

    try {
        auto ctx = c2pa::Context();
        c2pa::Reader reader(ctx, format, stream2);
    } catch (...) {
        with_context_throws = true;
    }

    EXPECT_EQ(without_context_throws, with_context_throws)
        << "Both Reader constructors should behave the same for invalid streams";
}

TEST(ReaderErrorHandling, FailedReaderConstructionWithAndWithoutContext)
{
    std::string format = "image/jpeg";

    for (int i = 0; i < 100; i++) {
        std::stringstream stream1, stream2;

        // Without context
        try {
            c2pa::Reader reader(format, stream1);
        } catch (...) {
            // Expected to fail on empty stream
        }

        // With context
        try {
            auto ctx = c2pa::Context();
            c2pa::Reader reader(ctx, format, stream2);
        } catch (...) {
            // Expected to fail on empty stream
        }
    }
}

TEST(ReaderErrorHandling, ErrorMessagesWithAndWithoutContext)
{
    std::stringstream empty_stream1, empty_stream2;
    std::string format = "image/jpeg";

    // Without context
    try {
        c2pa::Reader reader(format, empty_stream1);
        FAIL() << "Should have thrown";
    } catch (const c2pa::C2paException& e) {
        std::string msg = e.what();
        EXPECT_FALSE(msg.empty()) << "Error message should be present";
    }

    // With context
    try {
        auto ctx = c2pa::Context();
        c2pa::Reader reader(ctx, format, empty_stream2);
        FAIL() << "Should have thrown";
    } catch (const c2pa::C2paException& e) {
        std::string msg = e.what();
        EXPECT_FALSE(msg.empty()) << "Error message should be present with context API";
    }
}

TEST_F(ReaderTest, GetResourceToStream) {
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/C.jpg";

    c2pa::Reader reader(test_file);
    auto manifest_json = reader.json();
    auto parsed = json::parse(manifest_json);

    std::string active = parsed["active_manifest"];
    auto manifest = parsed["manifests"][active];

    // Extract thumbnail assertion URI
    std::string thumbnail_uri = "self#jumbf=c2pa.assertions/c2pa.thumbnail.claim.jpeg";

    std::ostringstream output;
    auto byte_count = reader.get_resource(thumbnail_uri, output);

    EXPECT_GT(byte_count, 0);
    EXPECT_FALSE(output.str().empty());
}

TEST_F(ReaderTest, GetResourceToFilePath) {
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/C.jpg";
    fs::path output_file = get_temp_path("thumbnail_test_output.jpg");

    c2pa::Reader reader(test_file);
    auto manifest_json = reader.json();
    auto parsed = json::parse(manifest_json);

    std::string active = parsed["active_manifest"];
    auto manifest = parsed["manifests"][active];

    // Extract thumbnail assertion URI
    std::string thumbnail_uri = "self#jumbf=c2pa.assertions/c2pa.thumbnail.claim.jpeg";

    auto byte_count = reader.get_resource(thumbnail_uri, output_file);

    EXPECT_GT(byte_count, 0);
    EXPECT_TRUE(fs::exists(output_file));
    EXPECT_GT(fs::file_size(output_file), 0);
}

TEST_F(ReaderTest, GetResourceInvalidUriThrows) {
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/C.jpg";

    c2pa::Reader reader(test_file);

    std::ostringstream output;
    EXPECT_THROW(reader.get_resource("nonexistent_uri", output), c2pa::C2paException);
}

TEST_F(ReaderTest, GetResourceWithInvalidUri) {
    fs::path current_dir = fs::path(__FILE__).parent_path();
    fs::path test_file = current_dir / "../tests/fixtures/C.jpg";

    c2pa::Reader reader(test_file);

    std::ostringstream output;
    EXPECT_THROW(reader.get_resource("invalid://nonexistent", output), c2pa::C2paException);
}

TEST_F(ReaderTest, ReadArchive)
{
    // Build a manifest with ingredients and archive it to re-read it back with a Reader
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto builder = c2pa::Builder(manifest);

    std::string ingredient1_json = R"({"title": "A.jpg", "relationship": "parentOf"})";
    builder.add_ingredient(ingredient1_json, c2pa_test::get_fixture_path("A.jpg"));

    std::string ingredient2_json = R"({"title": "C.jpg", "relationship": "componentOf"})";
    builder.add_ingredient(ingredient2_json, c2pa_test::get_fixture_path("C.jpg"));

    std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_NO_THROW(builder.to_archive(archive_stream));

    // Read back the archive with a Reader
    archive_stream.seekg(0);
    auto context = c2pa::Context();
    auto reader = c2pa::Reader(context, "application/c2pa", archive_stream);
    auto json_result = reader.json();

    auto parsed = json::parse(json_result);
    EXPECT_TRUE(parsed.contains("active_manifest"));
    EXPECT_TRUE(parsed.contains("manifests"));

    std::string active = parsed["active_manifest"];
    EXPECT_FALSE(active.empty());

    auto active_manifest = parsed["manifests"][active];
    EXPECT_TRUE(active_manifest.contains("ingredients"));
    EXPECT_EQ(active_manifest["ingredients"].size(), 2);
}
