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
#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>

#include "c2pa.hpp"
#include "include/test_utils.hpp"

namespace fs = std::filesystem;

// Test fixture for context tests with automatic cleanup
class ContextTest : public ::testing::Test {
protected:
    std::vector<fs::path> temp_files;
    bool cleanup_temp_files = true;  // Set to false to keep temp files for debugging

    // Get path for temp context test files in build directory
    fs::path get_temp_path(const std::string& name) {
        fs::path current_dir = fs::path(__FILE__).parent_path();
        fs::path build_dir = current_dir.parent_path() / "build";
        if (!fs::exists(build_dir)) {
            fs::create_directories(build_dir);
        }
        fs::path temp_path = build_dir / ("context-" + name);
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

static fs::path fixture_path(const std::string &name)
{
    return c2pa_test::get_fixture_path(name);
}

static std::string load_fixture(const std::string &name)
{
    return c2pa_test::read_text_file(fixture_path(name));
}

// Can create a context using JSON settings
TEST(Context, ContextFromJsonValid)
{
    std::string json = R"({"settings": {}})";
    c2pa::Context context(json);
    EXPECT_TRUE(context.is_valid());
}

// Can create a context using Settings object
TEST(Context, ContextFromSettingsValid)
{
    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "false");
    c2pa::Context context(settings);
    EXPECT_TRUE(context.is_valid());
}

// Context with invalid JSON throws
TEST(Context, ContextFromJsonInvalidThrows)
{
    EXPECT_THROW(
        { c2pa::Context context("{bad"); },
        c2pa::C2paException
    );
}

// Default context can be used with a Builder
TEST(Context, SettingsDefaultConstruction)
{
    c2pa::Settings settings;
    auto manifest = load_fixture("training.json");
    c2pa::Context context;

    // Should not crash when building with default settings
    auto ctx = std::make_shared<c2pa::Context>();
    EXPECT_NO_THROW({
        c2pa::Builder builder(ctx, manifest);
    });
}

// Can update settings with any valid JSON
TEST(Context, SettingsUpdateJson)
{
    c2pa::Settings settings;
    EXPECT_NO_THROW({
        settings.update(R"({"key": "val"})", "json");
    });
}

// ContextBuilder empty build returns default/empty context
TEST(Context, ContextBuilderEmptyBuild)
{
    auto builder = c2pa::Context::ContextBuilder();
    auto context = builder.create_context();

    EXPECT_TRUE(context.is_valid());
}

// Context move constructor transfers ownership, moved-from has no context
TEST(Context, MoveConstructor)
{
    auto original = c2pa::Context::ContextBuilder().create_context();
    ASSERT_TRUE(original.is_valid());

    c2pa::Context moved_to(std::move(original));
    EXPECT_TRUE(moved_to.is_valid());
    EXPECT_FALSE(original.is_valid());

    // Moved-to context is usable
    auto manifest = load_fixture("training.json");
    auto ctx = std::make_shared<c2pa::Context>(std::move(moved_to));
    EXPECT_NO_THROW({
        c2pa::Builder builder(ctx, manifest);
    });
}

// Context move assignment transfers ownership
TEST(Context, MoveAssignment)
{
    auto a = c2pa::Context::ContextBuilder().create_context();
    auto b = c2pa::Context::ContextBuilder().create_context();
    ASSERT_TRUE(a.is_valid());
    ASSERT_TRUE(b.is_valid());

    a = std::move(b);
    EXPECT_TRUE(a.is_valid());
    EXPECT_FALSE(b.is_valid());
}

// Context move assignment frees existing context when overwriting
TEST(Context, MoveAssignmentOverwrites)
{
    auto a = c2pa::Context::ContextBuilder().create_context();
    auto b = c2pa::Context::ContextBuilder().create_context();
    ASSERT_TRUE(a.is_valid());
    ASSERT_TRUE(b.is_valid());

    a = std::move(b);
    EXPECT_TRUE(a.is_valid());
    EXPECT_FALSE(b.is_valid());
    // Use a to ensure the adopted context works (no double-free of old a)
    auto manifest = load_fixture("training.json");
    auto ctx = std::make_shared<c2pa::Context>(std::move(a));
    EXPECT_NO_THROW({
        c2pa::Builder builder(ctx, manifest);
    });
}

// Helper function to check if thumbnail is present in signed manifest
static bool has_thumbnail(const std::string& manifest_json) {
    auto parsed = nlohmann::json::parse(manifest_json);
    std::string active = parsed["active_manifest"];
    return parsed["manifests"][active].contains("thumbnail");
}

// Helper function to sign with context and return manifest JSON.
static std::string sign_with_context(std::shared_ptr<c2pa::IContextProvider> context, const fs::path& dest_path) {
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto certs = c2pa_test::read_text_file(c2pa_test::get_fixture_path("es256_certs.pem"));
    auto private_key = c2pa_test::read_text_file(c2pa_test::get_fixture_path("es256_private.key"));
    auto asset_path = c2pa_test::get_fixture_path("A.jpg");

    c2pa::Builder builder(context, manifest);
    c2pa::Signer signer("es256", certs, private_key);

    builder.sign(asset_path, dest_path, signer);

    c2pa::Reader reader(context, dest_path);
    auto result = reader.json();

    return result;
}

TEST_F(ContextTest, SetOverridesLastWins) {
    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "true");
    settings.set("builder.thumbnail.enabled", "false");

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder().with_settings(settings).create_context());
    auto manifest_json = sign_with_context(context, get_temp_path("set_overrides_last_wins.jpg"));

    EXPECT_FALSE(has_thumbnail(manifest_json));
}

TEST_F(ContextTest, UpdateOverridesSetJson) {
    auto settings_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("settings/test_settings_no_thumbnail.json"));

    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "true");
    settings.update(settings_json, "json");

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder().with_settings(settings).create_context());
    auto manifest_json = sign_with_context(context, get_temp_path("update_overrides_set_json.jpg"));

    EXPECT_FALSE(has_thumbnail(manifest_json));
}

TEST_F(ContextTest, SetOverridesUpdateJson) {
    auto settings_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("settings/test_settings_no_thumbnail.json"));

    c2pa::Settings settings;
    settings.update(settings_json, "json");
    settings.set("builder.thumbnail.enabled", "true");

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder().with_settings(settings).create_context());
    auto manifest_json = sign_with_context(context, get_temp_path("set_overrides_update_json.jpg"));

    EXPECT_TRUE(has_thumbnail(manifest_json));
}

TEST_F(ContextTest, WithSettingsThenWithJson) {
    auto settings_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("settings/test_settings_no_thumbnail.json"));

    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "true");

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_settings(settings)
        .with_json(settings_json)
        .create_context());

    auto manifest_json = sign_with_context(context, get_temp_path("with_settings_then_json.jpg"));
    EXPECT_FALSE(has_thumbnail(manifest_json));
}

TEST_F(ContextTest, WithJsonThenWithSettings) {
    auto settings_json = c2pa_test::read_text_file(c2pa_test::get_fixture_path("settings/test_settings_with_thumbnail.json"));

    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "false");

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_json(settings_json)
        .with_settings(settings)
        .create_context());

    auto manifest_json = sign_with_context(context, get_temp_path("with_json_then_settings.jpg"));
    EXPECT_FALSE(has_thumbnail(manifest_json));
}

TEST(Context, ContextBuilderMoveConstructor) {
    auto b1 = c2pa::Context::ContextBuilder();
    auto b2 = std::move(b1);

    EXPECT_FALSE(b1.is_valid());
    EXPECT_TRUE(b2.is_valid());

    auto context = b2.create_context();
    EXPECT_TRUE(context.is_valid());
}

TEST(Context, ContextBuilderMoveAssignment) {
    auto b1 = c2pa::Context::ContextBuilder();
    auto b2 = c2pa::Context::ContextBuilder();

    b1 = std::move(b2);

    EXPECT_FALSE(b2.is_valid());
    EXPECT_TRUE(b1.is_valid());

    auto context = b1.create_context();
    EXPECT_TRUE(context.is_valid());
}

TEST(Context, SettingsMoveConstructor) {
    c2pa::Settings s1;
    s1.set("builder.thumbnail.enabled", "false");

    auto s2 = std::move(s1);

    EXPECT_EQ(s1.c_settings(), nullptr);

    // Verify s2 is functional
    auto context = c2pa::Context::ContextBuilder().with_settings(s2).create_context();
    EXPECT_TRUE(context.is_valid());
}

TEST(Context, SettingsMoveAssignment) {
    c2pa::Settings s1;
    c2pa::Settings s2;
    s2.set("builder.thumbnail.enabled", "true");

    s1 = std::move(s2);

    EXPECT_EQ(s2.c_settings(), nullptr);

    // Verify s1 is functional and can call set()
    EXPECT_NO_THROW(s1.set("builder.thumbnail.enabled", "false"));
}

TEST(Context, ContextBuilderUseAfterMoveThrows) {
    auto b1 = c2pa::Context::ContextBuilder();
    auto b2 = std::move(b1);

    EXPECT_THROW(b1.with_json("{}"), c2pa::C2paException);
}

TEST(Context, UseAfterConsumeThrows) {
    auto builder = c2pa::Context::ContextBuilder();
    auto context = builder.create_context();

    EXPECT_THROW(builder.with_json("{}"), c2pa::C2paException);
}

TEST(Context, DoubleConsumeThrows) {
    auto builder = c2pa::Context::ContextBuilder();
    auto context1 = builder.create_context();

    EXPECT_THROW({
        auto context2 = builder.create_context();
        (void)context2; // Suppress unused variable warning
    }, c2pa::C2paException);
}

// Default constructor creates a valid context
TEST(Context, DirectConstructDefault) {
    c2pa::Context context;
    EXPECT_TRUE(context.is_valid());
    EXPECT_NE(context.c_context(), nullptr);
}

// Constructor with Settings creates a valid context
TEST(Context, DirectConstructWithSettings) {
    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "false");

    c2pa::Context context(settings);
    EXPECT_TRUE(context.is_valid());
}

// Default constructor can be used with Builder
TEST(Context, DirectConstructDefaultWithBuilder) {
    auto manifest = load_fixture("training.json");
    auto context = std::make_shared<c2pa::Context>();
    EXPECT_NO_THROW({
        c2pa::Builder builder(context, manifest);
    });
}

// 1) Direct construction with Settings: sign and verify thumbnail is disabled
TEST_F(ContextTest, DirectConstructSettingsSignVerify) {
    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "false");

    auto context = std::make_shared<c2pa::Context>(settings);
    auto manifest_json = sign_with_context(context, get_temp_path("direct_construct_settings.jpg"));

    EXPECT_FALSE(has_thumbnail(manifest_json));
}

// 2) Direct default construction: sign and verify thumbnail is enabled (default)
TEST_F(ContextTest, DirectConstructDefaultSignVerify) {
    auto context = std::make_shared<c2pa::Context>();
    auto manifest_json = sign_with_context(context, get_temp_path("direct_construct_default.jpg"));

    EXPECT_TRUE(has_thumbnail(manifest_json));
}

// 3) JSON string constructor: sign and verify thumbnail is disabled
TEST_F(ContextTest, JsonConstructorSignVerify) {
    auto context = std::make_shared<c2pa::Context>(R"({"builder": {"thumbnail": {"enabled": false}}})");
    auto manifest_json = sign_with_context(context, get_temp_path("json_constructor.jpg"));

    EXPECT_FALSE(has_thumbnail(manifest_json));
}

// 4) ContextBuilder with Settings: sign and verify thumbnail is disabled
TEST_F(ContextTest, ContextBuilderWithSettingsSignVerify) {
    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "false");

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_settings(settings)
        .create_context());
    auto manifest_json = sign_with_context(context, get_temp_path("builder_with_settings.jpg"));

    EXPECT_FALSE(has_thumbnail(manifest_json));
}

// 5) ContextBuilder with JSON: sign and verify thumbnail is disabled
TEST_F(ContextTest, ContextBuilderWithJsonSignVerify) {
    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_json(R"({"builder": {"thumbnail": {"enabled": false}}})")
        .create_context());
    auto manifest_json = sign_with_context(context, get_temp_path("builder_with_json.jpg"));

    EXPECT_FALSE(has_thumbnail(manifest_json));
}

// 6) ContextBuilder empty (default): sign and verify thumbnail is enabled (default)
TEST_F(ContextTest, ContextBuilderDefaultSignVerify) {
    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder().create_context());
    auto manifest_json = sign_with_context(context, get_temp_path("builder_default.jpg"));

    EXPECT_TRUE(has_thumbnail(manifest_json));
}

// 7) Direct construction with Settings enabling thumbnail: sign and verify
TEST_F(ContextTest, DirectConstructSettingsEnableThumbnailSignVerify) {
    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "true");

    auto context = std::make_shared<c2pa::Context>(settings);
    auto manifest_json = sign_with_context(context, get_temp_path("direct_construct_enable_thumb.jpg"));

    EXPECT_TRUE(has_thumbnail(manifest_json));
}

// Test with_json_settings_file method: loads settings from file path
TEST_F(ContextTest, ContextBuilderWithJsonSettingsFile) {
    auto settings_path = c2pa_test::get_fixture_path("settings/test_settings_no_thumbnail.json");

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_json_settings_file(settings_path)
        .create_context());

    auto manifest_json = sign_with_context(context, get_temp_path("with_json_settings_file.jpg"));
    EXPECT_FALSE(has_thumbnail(manifest_json));
}

// Test with_json_settings_file with invalid path throws exception
TEST(Context, ContextBuilderWithJsonSettingsFileInvalidPath) {
    auto builder = c2pa::Context::ContextBuilder();
    EXPECT_THROW({
        builder.with_json_settings_file("/nonexistent/path/to/settings.json");
    }, c2pa::C2paException);
}

// Test with_json_settings_file can be chained with other methods
TEST_F(ContextTest, ContextBuilderWithJsonSettingsFileChaining) {
    auto settings_path = c2pa_test::get_fixture_path("settings/test_settings_with_thumbnail.json");

    // File enables thumbnail, then we disable it with set()
    c2pa::Settings override_settings;
    override_settings.set("builder.thumbnail.enabled", "false");

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_json_settings_file(settings_path)
        .with_settings(override_settings)
        .create_context());

    auto manifest_json = sign_with_context(context, get_temp_path("with_json_settings_file_chained.jpg"));
    EXPECT_FALSE(has_thumbnail(manifest_json));
}

// Reader retains a shared reference to the context, keeping it alive for the lifetime of the Reader.
TEST_F(ContextTest, ReaderWorksAfterContextOutOfScope) {
    fs::path signed_path = get_temp_path("reader_after_context_scope.jpg");
    std::unique_ptr<c2pa::Reader> reader;
    {
        auto context = std::make_shared<c2pa::Context>();
        sign_with_context(context, signed_path);
        reader = std::make_unique<c2pa::Reader>(context, signed_path);
        // context goes out of scope here, but reader holds a shared reference
    }
    EXPECT_NO_THROW(reader->json());
}

// ContextBuilder::with_signer can be chained with with_settings
TEST(Context, ContextBuilderWithSettingsAndSigner) {
    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "false");
    auto signer = c2pa_test::create_test_signer();
    auto context = c2pa::Context::ContextBuilder()
        .with_settings(settings)
        .with_signer(std::move(signer))
        .create_context();
    EXPECT_TRUE(context.is_valid());
}

// --- Progress / Cancel API tests ---
// Progress/cancel tests, available since c2pa-rs == 0.78.7.

// Helper: sign a file and return the signed path, using a context with a progress callback.
static fs::path sign_with_progress_context(std::shared_ptr<c2pa::IContextProvider> context, const fs::path& dest) {
    auto manifest   = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
    auto certs      = c2pa_test::read_text_file(c2pa_test::get_fixture_path("es256_certs.pem"));
    auto private_key = c2pa_test::read_text_file(c2pa_test::get_fixture_path("es256_private.key"));
    auto asset_path = c2pa_test::get_fixture_path("A.jpg");

    c2pa::Builder builder(context, manifest);
    c2pa::Signer signer("es256", certs, private_key);
    builder.sign(asset_path, dest, signer);
    return dest;
}

// Callback is invoked at least once during a sign operation.
TEST_F(ContextTest, ProgressCallback_InvokedDuringSigning) {
    std::atomic<int> call_count{0};

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_progress_callback([&](c2pa::ProgressPhase /*phase*/, uint32_t /*step*/, uint32_t /*total*/) {
            ++call_count;
            return true;
        })
        .create_context());

    ASSERT_TRUE(context->is_valid());
    EXPECT_NO_THROW(sign_with_progress_context(context, get_temp_path("progress_signing.jpg")));
    EXPECT_GT(call_count.load(), 0);
}

// Callback is invoked at least once during a read operation.
TEST_F(ContextTest, ProgressCallback_InvokedDuringReading) {
    // First sign a file without a callback so we have something to read.
    {
        auto sign_ctx = std::make_shared<c2pa::Context>();
        sign_with_progress_context(sign_ctx, get_temp_path("progress_read_src.jpg"));
    }

    std::atomic<int> call_count{0};

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_progress_callback([&](c2pa::ProgressPhase /*phase*/, uint32_t /*step*/, uint32_t /*total*/) {
            ++call_count;
            return true;
        })
        .create_context());

    ASSERT_TRUE(context->is_valid());
    EXPECT_NO_THROW({
        c2pa::Reader reader(context, get_temp_path("progress_read_src.jpg"));
        (void)reader.json();
    });
    EXPECT_GT(call_count.load(), 0);
}

// Callback receives non-negative step and total values.
TEST_F(ContextTest, ProgressCallback_StepAndTotalValues) {
    bool saw_valid_step = false;

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_progress_callback([&](c2pa::ProgressPhase /*phase*/, uint32_t step, uint32_t total) {
            // step is 1-based when total > 0; both may be 0 for indeterminate phases.
            if (total > 0) {
                EXPECT_GE(step, 1u);
                EXPECT_LE(step, total);
                saw_valid_step = true;
            }
            return true;
        })
        .create_context());

    ASSERT_TRUE(context->is_valid());
    EXPECT_NO_THROW(sign_with_progress_context(context, get_temp_path("progress_step_total.jpg")));
    EXPECT_TRUE(saw_valid_step);
}

// Returning false from the callback causes the operation to be cancelled.
TEST_F(ContextTest, ProgressCallback_ReturnFalseCancels) {
    // Cancel on the very first callback invocation.
    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_progress_callback([](c2pa::ProgressPhase /*phase*/, uint32_t /*step*/, uint32_t /*total*/) {
            return false;  // request cancellation
        })
        .create_context());

    ASSERT_TRUE(context->is_valid());
    EXPECT_THROW(
        sign_with_progress_context(context, get_temp_path("progress_cancel_false.jpg")),
        c2pa::C2paException
    );
}

// Context::cancel() called before an operation prevents that operation from completing.
TEST_F(ContextTest, ProgressCallback_CancelMethodAbortsOperation) {
    // ctx is declared before the lambda so the lambda can capture it by reference.
    // It is assigned after construction since the context must be built first.
    std::shared_ptr<c2pa::Context> ctx;
    bool cancel_called = false;

    ctx = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_progress_callback([&](c2pa::ProgressPhase /*phase*/, uint32_t /*step*/, uint32_t /*total*/) {
            if (!cancel_called) {
                cancel_called = true;
                ctx->cancel();
            }
            return true;  // continue returning true; cancellation is handled via cancel()
        })
        .create_context());

    ASSERT_TRUE(ctx->is_valid());
    EXPECT_THROW(
        sign_with_progress_context(ctx, get_temp_path("progress_cancel_method.jpg")),
        c2pa::C2paException
    );
}

// Context::cancel() is safe to call on a context without a callback.
TEST(Context, CancelWithoutCallback_IsNoOp) {
    c2pa::Context context;
    ASSERT_TRUE(context.is_valid());
    EXPECT_NO_THROW(context.cancel());
}

// with_progress_callback can be chained with with_settings.
TEST_F(ContextTest, ProgressCallback_ChainWithSettings) {
    std::atomic<int> call_count{0};

    c2pa::Settings settings;
    settings.set("builder.thumbnail.enabled", "false");

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_settings(settings)
        .with_progress_callback([&](c2pa::ProgressPhase /*phase*/, uint32_t /*step*/, uint32_t /*total*/) {
            ++call_count;
            return true;
        })
        .create_context());

    ASSERT_TRUE(context->is_valid());
    auto manifest_json = sign_with_context(context, get_temp_path("progress_chain_settings.jpg"));
    EXPECT_GT(call_count.load(), 0);
    EXPECT_FALSE(has_thumbnail(manifest_json));
}

// Context with a progress callback can be move-constructed; callback is still valid.
TEST_F(ContextTest, ProgressCallback_SurvivesContextMove) {
    std::atomic<int> call_count{0};

    auto original = c2pa::Context::ContextBuilder()
        .with_progress_callback([&](c2pa::ProgressPhase /*phase*/, uint32_t /*step*/, uint32_t /*total*/) {
            ++call_count;
            return true;
        })
        .create_context();

    auto moved_to = std::make_shared<c2pa::Context>(std::move(original));
    EXPECT_FALSE(original.is_valid());
    ASSERT_TRUE(moved_to->is_valid());

    EXPECT_NO_THROW(sign_with_progress_context(moved_to, get_temp_path("progress_move.jpg")));
    EXPECT_GT(call_count.load(), 0);
}

// ContextBuilder with a progress callback can be move-constructed; callback is transferred.
TEST_F(ContextTest, ProgressCallback_SurvivesBuilderMove) {
    std::atomic<int> call_count{0};

    c2pa::Context::ContextBuilder b1;
    b1.with_progress_callback([&](c2pa::ProgressPhase /*phase*/, uint32_t /*step*/, uint32_t /*total*/) {
        ++call_count;
        return true;
    });

    auto b2 = std::move(b1);
    EXPECT_FALSE(b1.is_valid());
    ASSERT_TRUE(b2.is_valid());

    auto context = std::make_shared<c2pa::Context>(b2.create_context());
    ASSERT_TRUE(context->is_valid());

    EXPECT_NO_THROW(sign_with_progress_context(context, get_temp_path("progress_builder_move.jpg")));
    EXPECT_GT(call_count.load(), 0);
}

// Builder keeps the Context alive to use progress callbacks
TEST_F(ContextTest, BuilderKeepsContextAlive) {
    std::atomic<int> call_count{0};

    c2pa::Builder builder = [&]() {
        auto ctx = std::make_shared<c2pa::Context>(
            c2pa::Context::ContextBuilder()
                .with_progress_callback([&](c2pa::ProgressPhase, uint32_t, uint32_t) {
                    ++call_count;
                    return true;
                })
                .create_context()
        );
        auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));
        return c2pa::Builder(ctx, manifest);
        // ctx goes out of scope here
    }();

    auto certs       = c2pa_test::read_text_file(c2pa_test::get_fixture_path("es256_certs.pem"));
    auto private_key = c2pa_test::read_text_file(c2pa_test::get_fixture_path("es256_private.key"));
    c2pa::Signer signer("es256", certs, private_key);

    EXPECT_NO_THROW(builder.sign(c2pa_test::get_fixture_path("A.jpg"), get_temp_path("shared_ptr_builder.jpg"), signer));
    EXPECT_GT(call_count.load(), 0);
}

// Reader keeps the Context alive via shared_ptr.
TEST_F(ContextTest, ReaderKeepsContextAlive) {
    // Sign a file first so we have something to read.
    {
        auto sign_ctx = std::make_shared<c2pa::Context>();
        sign_with_progress_context(sign_ctx, get_temp_path("shared_ptr_reader_src.jpg"));
    }

    std::atomic<int> call_count{0};

    c2pa::Reader reader = [&]() {
        auto ctx = std::make_shared<c2pa::Context>(
            c2pa::Context::ContextBuilder()
                .with_progress_callback([&](c2pa::ProgressPhase, uint32_t, uint32_t) {
                    ++call_count;
                    return true;
                })
                .create_context()
        );
        return c2pa::Reader(ctx, get_temp_path("shared_ptr_reader_src.jpg"));
        // ctx goes out of scope, but reader holds a copy
    }();

    EXPECT_NO_THROW((void)reader.json());
    EXPECT_GT(call_count.load(), 0);
}

// Move-constructing a Builder transfers the shared context reference.
TEST_F(ContextTest, SharedPtrContextMoveTransfersOwnership) {
    auto ctx = std::make_shared<c2pa::Context>();
    auto manifest = c2pa_test::read_text_file(c2pa_test::get_fixture_path("training.json"));

    c2pa::Builder b1(ctx, manifest);
    EXPECT_EQ(ctx.use_count(), 2);

    c2pa::Builder b2 = std::move(b1);
    EXPECT_EQ(ctx.use_count(), 2);  // b1 released, b2 took over
}

// Reader::from_asset works with shared_ptr context.
TEST_F(ContextTest, SharedPtrContext_FromAsset) {
    // Sign a file first so we have something with C2PA data.
    {
        auto sign_ctx = std::make_shared<c2pa::Context>();
        sign_with_progress_context(sign_ctx, get_temp_path("shared_ptr_from_asset_src.jpg"));
    }

    auto ctx = std::make_shared<c2pa::Context>();
    auto reader = c2pa::Reader::from_asset(ctx, get_temp_path("shared_ptr_from_asset_src.jpg"));
    EXPECT_TRUE(reader.has_value());
    EXPECT_EQ(ctx.use_count(), 2);  // ctx + reader
}

// ContextBuilder::release() must carry the progress-callback owner
// alongside the raw native builder handle.
TEST_F(ContextTest, ContextBuilderReleaseCarriesCallbackOwner) {
    std::atomic<int> call_count{0};

    // Minimal external IContextProvider that adopts the ReleasedBuilder
    // pieces and preserves the required destruction order.
    class AdoptingProvider : public c2pa::IContextProvider {
    public:
        AdoptingProvider(c2pa::Context::ContextBuilder::ReleasedBuilder rb) {
            if (!rb.builder) {
                throw c2pa::C2paException("AdoptingProvider: null builder");
            }
            ctx_ = c2pa_context_builder_build(rb.builder);
            if (!ctx_) {
                throw c2pa::C2paException("AdoptingProvider: build failed");
            }
            callback_owner_ = std::move(rb.callback_owner);
        }
        ~AdoptingProvider() noexcept override {
            if (ctx_) {
                c2pa_free(ctx_);
            }
        }
        AdoptingProvider(const AdoptingProvider&) = delete;
        AdoptingProvider& operator=(const AdoptingProvider&) = delete;

        C2paContext* c_context() const noexcept override { return ctx_; }
        bool is_valid() const noexcept override { return ctx_ != nullptr; }

    private:
        C2paContext* ctx_ = nullptr;
        std::unique_ptr<c2pa::ProgressCallbackFunc> callback_owner_;
    };

    auto rb = c2pa::Context::ContextBuilder()
        .with_progress_callback([&](c2pa::ProgressPhase, uint32_t, uint32_t) {
            ++call_count;
            return true;
        })
        .release();
    ASSERT_NE(rb.builder, nullptr);
    ASSERT_NE(rb.callback_owner, nullptr);

    auto provider = std::make_shared<AdoptingProvider>(std::move(rb));
    ASSERT_TRUE(provider->is_valid());

    EXPECT_NO_THROW(sign_with_progress_context(provider, get_temp_path("release_adopting.jpg")));
    EXPECT_GT(call_count.load(), 0);
}

// A ContextBuilder that has a progress callback set but is never
// turned into a Context must destruct cleanly.
TEST_F(ContextTest, ContextBuilderDroppedAfterCallbackNoUAF) {
    std::atomic<int> call_count{0};
    {
        c2pa::Context::ContextBuilder b;
        b.with_progress_callback([&](c2pa::ProgressPhase, uint32_t, uint32_t) {
            ++call_count;
            return true;
        });
        // Intentionally no create_context(), let b destruct here.
    }

    SUCCEED();
}

// Ccreate_context() must transfer callback ownership to the new
// Context atomically.
TEST_F(ContextTest, ContextBuilderCreateContextAtomicHandoff) {
    std::atomic<int> call_count{0};

    auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
        .with_progress_callback([&](c2pa::ProgressPhase, uint32_t, uint32_t) {
            ++call_count;
            return true;
        })
        .create_context());

    ASSERT_TRUE(context->is_valid());
    EXPECT_NO_THROW(sign_with_progress_context(context, get_temp_path("atomic_handoff.jpg")));
    EXPECT_GT(call_count.load(), 0);
}

// Context move-assignment must free the destination's native context
// before releasing its old callback storage, and must leave the survivor
// with a valid callback bound to its new context.
TEST_F(ContextTest, ContextMoveAssignWithCallbackPreservesInvocation) {
    std::atomic<int> lhs{0};
    std::atomic<int> rhs{0};

    c2pa::Context a = c2pa::Context::ContextBuilder()
        .with_progress_callback([&](c2pa::ProgressPhase, uint32_t, uint32_t) {
            ++lhs;
            return true;
        })
        .create_context();

    c2pa::Context b = c2pa::Context::ContextBuilder()
        .with_progress_callback([&](c2pa::ProgressPhase, uint32_t, uint32_t) {
            ++rhs;
            return true;
        })
        .create_context();

    a = std::move(b);
    EXPECT_FALSE(b.is_valid());
    ASSERT_TRUE(a.is_valid());

    auto sp = std::make_shared<c2pa::Context>(std::move(a));
    EXPECT_NO_THROW(sign_with_progress_context(sp, get_temp_path("move_assign_survivor.jpg")));

    EXPECT_EQ(lhs.load(), 0);
    EXPECT_GT(rhs.load(), 0);
}
