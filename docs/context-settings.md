# Configuring the SDK with Context and Settings

This guide explains how to configure the C2PA SDK using `Context` and `Settings`. The configuration controls SDK behavior including verification, trust anchors, thumbnails, signing, and more.

See also:

- [Usage](usage.md): Reading and signing with Reader and Builder
- [Rust SDK settings](https://github.com/contentauth/c2pa-rs/blob/main/docs/settings.md): Shared settings schema and additional JSON examples
- [CAI settings schema reference](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/): Complete schema reference

## Quick start

The simplest way to configure the SDK is to create a `Context` wrapped in a `shared_ptr` and pass it to `Reader` or `Builder`:

```cpp
#include "c2pa.hpp"

// Create a Context with settings
auto context = std::make_shared<c2pa::Context>(R"({
  "version": 1,
  "builder": {
    "claim_generator_info": {"name": "My App", "version": "1.0"},
    "thumbnail": {"enabled": false}
  },
  "verify": {
    "remote_manifest_fetch": false
  }
})");

// Use with Reader or Builder
c2pa::Reader reader(context, "image.jpg");
c2pa::Builder builder(context, manifest_json);
```

For default SDK configuration, just create an empty `Context`:

```cpp
auto context = std::make_shared<c2pa::Context>();  // Uses SDK defaults
c2pa::Reader reader(context, "image.jpg");
```

## Understanding Context and Settings

### What is Context?

`Context` encapsulates SDK configuration that controls how `Reader`, `Builder`, and other components operate:

- **Settings**: Trust configuration, builder behavior, thumbnails, and more
- **Signer configuration**: Optional signing credentials that can be stored for reuse
- **State isolation**: Each `Context` is independent, allowing multiple configurations to coexist

### Why use Context?

`Context` provides explicit, isolated configuration:

- **Makes dependencies explicit**: Configuration is passed directly to `Reader` and `Builder`, not hidden in global state
- **Enables multiple configurations**: Run different configurations simultaneously (e.g., development with test certificates, production with strict validation)
- **Eliminates thread-local state**: No subtle bugs from shared state
- **Simplifies testing**: Isolated configurations without cleanup or interference
- **Improves code clarity**: `Builder(context, manifest)` shows configuration is being used

> [!NOTE]
> The deprecated `c2pa::load_settings(data, format)` still works but you should migrate to `Context`. **Never mix the two approaches**. 
> See [Migrating from deprecated APIs](#migrating-from-deprecated-apis).

### Context lifecycle

- **Non-copyable, moveable**: `Context` can be moved but not copied. After moving, `is_valid()` returns `false` on the source
- **Pass as `shared_ptr`**: `Reader` and `Builder` retain a shared reference to the context, keeping it alive for their lifetime. This is required when using progress callbacks: without it, the callback can fire after the context is destroyed, causing a crash
- **Reusable**: Use the same `shared_ptr<Context>` to create multiple readers and builders

```cpp
auto context = std::make_shared<c2pa::Context>(settings);

// All three use the same configuration
c2pa::Builder builder1(context, manifest1);
c2pa::Builder builder2(context, manifest2);
c2pa::Reader reader(context, "image.jpg");

// context shared_ptr can go out of scope — reader/builder each hold a reference
```

### Settings format

Settings use JSON or TOML format. The schema is shared across all SDK languages (Rust, C/C++, Python). JSON is preferred for C++.

```cpp
// JSON (preferred)
c2pa::Context context(R"({"verify": {"verify_after_sign": true}})");

// TOML
c2pa::Settings settings(R"([verify]
verify_after_sign = true)", "toml");
c2pa::Context context(settings);
```

## Creating a Context

There are several ways to create a `Context`:
- [Use SDK defaults](#use-sdk-defaults)
- [Create from inline JSON](#create-from-inline-json)
- [Create from a Settings object](#create-from-a-settings-object)
- [Create using ContextBuilder](#create-using-contextbuilder)

### Use SDK defaults

For quick prototyping and simple use cases, you can use the SDK defaults like this:

```cpp
c2pa::Context context;  // Uses SDK defaults
```

For information on the defaults, see [Configuring SDK settings - Default configuration](https://opensource.contentauthenticity.org/docs/rust-sdk/docs/context-settings#default-configuration).

### Create from inline JSON

To specify a simple configuration that doesn't need to be shared across the codebase, you can use inline JSON like this:

```cpp
c2pa::Context context(R"({
  "version": 1,
  "verify": {"verify_after_sign": true},
  "builder": {"claim_generator_info": {"name": "My App"}}
})");
```

### Create from a Settings object

To specify a configuration that needs runtime logic or incremental construction, use a Settings object like this:

```cpp
c2pa::Settings settings;
settings.set("builder.thumbnail.enabled", "false");
settings.set("verify.verify_after_sign", "true");
settings.update(R"({"builder": {"claim_generator_info": {"name": "My App"}}})");

c2pa::Context context(settings);
```

### Create using ContextBuilder

To load a configuration from files or combine multiple configuration sources, use ContextBuilder. Don't use if you have a single configuration source, since direct construction is simpler.

```cpp
c2pa::Settings base_settings;
base_settings.set("builder.thumbnail.enabled", "true");

auto context = c2pa::Context::ContextBuilder()
    .with_settings(base_settings)
    .with_json(R"({"verify": {"verify_after_sign": true}})")
    .with_json_settings_file("config/overrides.json")
    .create_context();
```

> [!IMPORTANT]
> Later configuration overrides earlier configuration. In the example above, `overrides.json` will override values from `base_settings` and the inline JSON.

**ContextBuilder methods:**

| Method | Description |
|--------|-------------|
| `with_settings(settings)` | Apply a `Settings` object |
| `with_json(json_string)` | Apply settings from a JSON string |
| `with_json_settings_file(path)` | Load and apply settings from a JSON file |
| `with_signer(signer)` | Store a `Signer` in the context (consumed; used by `Builder::sign` with no explicit signer) |
| `with_progress_callback(callback)` | Register a progress/cancel callback (see [Progress callbacks and cancellation](#progress-callbacks-and-cancellation)) |
| `create_context()` | Build and return the `Context` (consumes the builder) |
| `release()` | Release the raw `C2paContextBuilder*` together with its progress-callback heap owner (see also [Releasing to a custom IContextProvider](#releasing-to-a-custom-icontextprovider)) |

## Progress callbacks and cancellation

You can register a callback on a `Context` to receive progress notifications during signing and reading operations, and to cancel an operation in flight.

### Registering a callback

Use `ContextBuilder::with_progress_callback` to attach a callback before building the context:

```cpp
#include <atomic>

std::atomic<int> phase_count{0};

auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
    .with_progress_callback([&](c2pa::ProgressPhase phase, uint32_t step, uint32_t total) {
        ++phase_count;
        // Return true to continue, false to cancel.
        return true;
    })
    .create_context());

// Pass as shared_ptr so the context stays alive while the callback can fire.
c2pa::Builder builder(context, manifest_json);
builder.sign("source.jpg", "output.jpg", signer);
```

The callback signature is:

```cpp
bool callback(c2pa::ProgressPhase phase, uint32_t step, uint32_t total);
```

- **`phase`**: which stage the SDK is in (see [ProgressPhase values](#progressphase-values) below).
- **`step`**: monotonically increasing counter within the current phase, starting at `1`. Resets to `1` at the start of each new phase. Use as a liveness signal: a rising `step` means the SDK is making forward progress.
- **`total`**: `0` = indeterminate (show a spinner); `1` = single-shot phase; `> 1` = determinate (`step / total` gives a completion fraction).
- **Return value**: return `true` to continue, `false` to request cancellation (same effect as calling `context.cancel()`).

**Do not throw** from the progress callback. Exceptions cannot cross the C/Rust boundary safely; if your callback throws, the wrapper catches it and the operation is aborted as a cancellation (you do not get your exception back at the call site). Use `return false`, `context.cancel()`, or application-side state instead.

> [!IMPORTANT]
> A callback passed to `with_progress_callback` is copied onto the heap and its address is handed to the underlying library. The built-in `Context` owns that heap block and frees it only after the native context is freed. The free order is correct by construction when the callback flows through `create_context()` into a `shared_ptr<Context>`. When the native handle is transferred out of the builder via `release()`, the same contract must be maintained by the caller. See [Releasing to a custom IContextProvider](#releasing-to-a-custom-icontextprovider).

### Cancelling from another thread

You may call `Context::cancel()` from another thread while the same `Context` remains valid and is not being destroyed or moved concurrently with that call. The SDK returns a `C2paException` with an `OperationCancelled` error at the next progress checkpoint:

```cpp
#include <thread>

auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
    .with_progress_callback([](c2pa::ProgressPhase, uint32_t, uint32_t) {
        return true;  // Don't cancel from the callback — use cancel() instead.
    })
    .create_context());

// Kick off a cancel after 500 ms from a background thread.
std::thread cancel_thread([context]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    context->cancel();
});

try {
    c2pa::Builder builder(context, manifest_json);
    builder.sign("large_file.jpg", "output.jpg", signer);
} catch (const c2pa::C2paException& e) {
    // "OperationCancelled" if cancel() fired before signing completed.
}

cancel_thread.join();
```

`cancel()` is safe to call when no operation is in progress: it is a no-op in that case (and a no-op if the `Context` is moved-from).

### ProgressPhase values

| Phase | When emitted |
|-------|-------------|
| `Reading` | Start of a read/verification pass |
| `VerifyingManifest` | Manifest structure is being validated |
| `VerifyingSignature` | COSE signature is being verified |
| `VerifyingIngredient` | An ingredient manifest is being verified |
| `VerifyingAssetHash` | Asset hash is being computed and checked |
| `AddingIngredient` | An ingredient is being embedded |
| `Thumbnail` | A thumbnail is being generated |
| `Hashing` | Asset data is being hashed for signing |
| `Signing` | Claim is being signed |
| `Embedding` | Signed manifest is being embedded into the asset |
| `FetchingRemoteManifest` | A remote manifest URL is being fetched |
| `Writing` | Output is being written |
| `FetchingOCSP` | OCSP certificate status is being fetched |
| `FetchingTimestamp` | A timestamp is being fetched from a TSA |

**Typical phase sequence during signing:**

```
AddingIngredient → Thumbnail → Hashing → Signing → Embedding
```

If `verify_after_sign` is enabled, verification phases follow:

```
→ VerifyingManifest → VerifyingSignature → VerifyingAssetHash → VerifyingIngredient
```

**Typical phase sequence during reading:**

```
Reading → VerifyingManifest → VerifyingSignature → VerifyingAssetHash → VerifyingIngredient
```

### Combining with other settings

`with_progress_callback` chains with other `ContextBuilder` methods:

```cpp
auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
    .with_settings(settings)
    .with_signer(std::move(signer))
    .with_progress_callback([](c2pa::ProgressPhase phase, uint32_t step, uint32_t total) {
        // Update a UI progress bar, log phases, etc.
        return true;
    })
    .create_context());
```

## Common configuration patterns

Common configurations include:

- [Development with test certificates](#development-with-test-certificates)
- [Layered configuration](#layered-configuration)
- [Configuration from environment variables](#configuration-from-environment-variables)
- [Multiple contexts](#multiple-contexts)
- [Temporary contexts](#temporary-contexts)

### Development with test certificates

Trust self-signed or custom CA certificates during development:

```cpp
std::string test_ca = read_file("test-ca.pem");

c2pa::Context dev_context(R"({
  "version": 1,
  "trust": {
    "user_anchors": ")" + test_ca + R"("
  },
  "verify": {
    "remote_manifest_fetch": false,
    "ocsp_fetch": false
  },
  "builder": {
    "claim_generator_info": {"name": "Dev Build", "version": "dev"},
    "thumbnail": {"enabled": false}
  }
})");
```

### Layered configuration

Load base configuration and apply environment-specific overrides:

```cpp
auto context = c2pa::Context::ContextBuilder()
    .with_json_settings_file("config/base.json")
    .with_json_settings_file("config/" + environment + ".json")
    .with_json(R"({
      "builder": {
        "claim_generator_info": {"version": ")" + app_version + R"("}
      }
    })")
    .create_context();
```

### Configuration from environment variables

Adapt configuration based on runtime environment:

```cpp
std::string env = std::getenv("ENVIRONMENT") ? std::getenv("ENVIRONMENT") : "dev";

c2pa::Settings settings;
if (env == "production") {
    settings.update(read_file("config/production.json"), "json");
    settings.set("verify.strict_v1_validation", "true");
} else {
    settings.update(read_file("config/development.json"), "json");
    settings.set("verify.remote_manifest_fetch", "false");
}

c2pa::Context context(settings);
```

### Multiple contexts

Use different `Context` objects for different purposes:

```cpp
c2pa::Context dev_context(dev_settings);
c2pa::Context prod_context(prod_settings);

c2pa::Builder dev_builder(dev_context, manifest);
c2pa::Builder prod_builder(prod_context, manifest);
```

### Temporary contexts

You can wrap a temporary `Context` in a `shared_ptr` inline:

```cpp
c2pa::Reader reader(
    std::make_shared<c2pa::Context>(R"({"verify": {"remote_manifest_fetch": false}})"),
    "image.jpg"
);
```

## Using Context with Reader

`Reader` uses `Context` to control validation, trust configuration, network access, and performance.

> [!IMPORTANT]
> Pass `Context` as a `shared_ptr`. `Reader` retains a shared reference, keeping the context alive for its lifetime. This is required when using progress callbacks.

### Reading from a file

```cpp
auto context = std::make_shared<c2pa::Context>(R"({
  "version": 1,
  "verify": {
    "remote_manifest_fetch": false,
    "ocsp_fetch": false
  }
})");

c2pa::Reader reader(context, "image.jpg");
std::cout << reader.json() << std::endl;
```

### Reading from a stream

```cpp
std::ifstream stream("image.jpg", std::ios::binary);
c2pa::Reader reader(context, "image/jpeg", stream);
std::cout << reader.json() << std::endl;
```

## Using Context with Builder

`Builder` uses `Context` to control manifest creation, signing, thumbnails, and more.

> [!IMPORTANT]
> Pass `Context` as a `shared_ptr`. `Builder` retains a shared reference, keeping the context alive for its lifetime. This is required when using progress callbacks.

```cpp
auto context = std::make_shared<c2pa::Context>(R"({
  "version": 1,
  "builder": {
    "claim_generator_info": {"name": "My App", "version": "1.0"},
    "intent": {"Create": "digitalCapture"}
  }
})");

c2pa::Builder builder(context, manifest_json);

// Pass signer explicitly at signing time
c2pa::Signer signer("es256", certs, private_key);
builder.sign(source_path, output_path, signer);
```

## Settings reference

### Settings API

The `Settings` class provides methods for creating and manipulating configuration:

| Method | Description |
|--------|-------------|
| [`Settings()`](https://contentauth.github.io/c2pa-cpp/da/d96/classc2pa_1_1Settings.html#a70274281f05d59ddcfda4aa21397b896) | Create default settings |
| [`Settings(data, format)`](https://contentauth.github.io/c2pa-cpp/da/d96/classc2pa_1_1Settings.html#a695e6e8c5a8cf16e40d6522af1fc13dd) | Parse settings from a string. Format is `"json"` or `"toml"` |
| [`set(path, json_value)`](https://contentauth.github.io/c2pa-cpp/da/d96/classc2pa_1_1Settings.html#a13810c5df3183aa2b0132e3c7c8edd1c) | Set a value by dot-separated path (e.g., `"verify.verify_after_sign"`). Value must be JSON-encoded. Returns `*this` for chaining |
| [`update(data)`](https://contentauth.github.io/c2pa-cpp/da/d96/classc2pa_1_1Settings.html#a1bd09762fb2e6c4c937c814500826ecc) | Merge JSON configuration (same as `update(data, "json")`) |
| [`update(data, format)`](https://contentauth.github.io/c2pa-cpp/da/d96/classc2pa_1_1Settings.html#a80a12a51569bd89cc18231c7c9c36242) | Merge configuration from a string with specified format |
| [`is_valid()`](https://contentauth.github.io/c2pa-cpp/da/d96/classc2pa_1_1Settings.html#add51c3e2ef459978be035b86803b338e) | Returns `true` if the object is valid (not moved-from) |

> [!NOTE]
> 
> - Settings are **moveable, not copyable**. After moving, `is_valid()` returns `false` on the source.
> - `set()` and `update()` can be chained for sequential configuration.
> - Later calls override earlier ones (last wins).

### Settings object structure

> [!TIP]
> For the complete reference to the Settings object, see [SDK object reference - Settings](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema).

Settings JSON has this top-level structure:

```json
{
  "version": 1,
  "trust": { ... },
  "cawg_trust": { ... },
  "core": { ... },
  "verify": { ... },
  "builder": { ... },
  "signer": { ... },
  "cawg_x509_signer": { ... }
}
```

| Property | Description |
|----------|-------------|
| `version` | Settings format version (must be 1) |
| [`trust`](#trust-configuration) | Certificate trust configuration for C2PA validation |
| [`cawg_trust`](#cawg-trust-configuration) | Certificate trust configuration for CAWG identity assertions |
| [`core`](#core-settings) | Core SDK behavior and performance tuning |
| [`verify`](#verify-settings) | Validation and verification behavior |
| [`builder`](#builder-settings) | Manifest creation and embedding behavior |
| [`signer`](#signer-settings) | C2PA signer configuration |
| [`cawg_x509_signer`](#cawg-x509-signer-configuration) | CAWG identity assertion signer configuration |

The `version` property must be `1`. All other properties are optional.

> [!IMPORTANT]
> If you don't specify a property, the SDK uses the default value. If you specify `null`, the property is explicitly set to null (not the default). This distinction matters when overriding default behavior.

For Boolean values, use JSON `true` and `false`, not the strings `"true"` and `"false"`.

> [!TIP]
> For the complete default settings configuration, see [Rust library - Configuring SDK settings - Default configuration](https://opensource.contentauthenticity.org/docs/rust-sdk/docs/settings#default-configuration).

### Trust configuration

The [`trust` properties](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/#trust) control which certificates are trusted when validating C2PA manifests.

| Property |  Description | Default |
|----------|--------------|---------|
| `trust.user_anchors` |  Additional root certificates (PEM format). Adds custom CAs without replacing built-in trust anchors. Recommended for development. | — |
| `trust.allowed_list` |  Explicitly allowed certificates (PEM format). Trusted regardless of chain validation. Use for development/testing to bypass validation. | — |
| `trust.trust_anchors` |  Default trust anchor root certificates (PEM format). **Replaces** the SDK's built-in trust anchors entirely. | — |
| `trust.trust_config` |  Allowed Extended Key Usage (EKU) OIDs for certificate purposes (e.g., document signing: `1.3.6.1.4.1.311.76.59.1.9`). | — |

#### Using user_anchors

For development, add your test root CA without replacing the SDK's default trust store:

```cpp
std::string test_root_ca = R"(-----BEGIN CERTIFICATE-----
MIICEzCCAcWgAwIBAgIUW4fUnS38162x10PCnB8qFsrQuZgwBQYDK2VwMHcxCzAJ
...
-----END CERTIFICATE-----)";

auto context = std::make_shared<c2pa::Context>(R"({
  "version": 1,
  "trust": {
    "user_anchors": ")" + test_root_ca + R"("
  }
})");

c2pa::Reader reader(context, "signed_asset.jpg");
```

#### Using allowed_list

For quick testing, bypass chain validation by explicitly allowing a specific certificate:

```cpp
std::string test_cert = read_file("test_cert.pem");

c2pa::Settings settings;
settings.update(R"({
  "version": 1,
  "trust": {
    "allowed_list": ")" + test_cert + R"("
  }
})");

auto context = std::make_shared<c2pa::Context>(settings);
c2pa::Reader reader(context, "signed_asset.jpg");
```

#### Loading trust from a file

Suppose `dev_trust_config.json` looks like this:

```json
{
  "version": 1,
  "trust": {
    "user_anchors": "-----BEGIN CERTIFICATE-----\nMIICEzCCA...\n-----END CERTIFICATE-----",
    "trust_config": "1.3.6.1.4.1.311.76.59.1.9\n1.3.6.1.4.1.62558.2.1"
  }
}
```

For the PEM string (for example in `user_anchors` in above example):

- Use literal `\n` (as two-character strings) in JSON for line breaks
- Include the full certificate chain if needed
- Concatenate multiple certificates into a single string

Load in your application:

```cpp
auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
    .with_json_settings_file("dev_trust_config.json")
    .create_context());

c2pa::Reader reader(context, "signed_asset.jpg");
```

### CAWG trust configuration

The `cawg_trust` properties configure CAWG (Creator Assertions Working Group) validation of identity assertions in C2PA manifests. The structure is identical to [`trust`](#trust-configuration).

> [!NOTE]
> CAWG trust settings only apply when processing identity assertions with X.509 certificates. If your workflow doesn't use CAWG identity assertions, these settings have no effect.


### Core settings

The [`core` properties](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/#core) control SDK behavior and performance tuning:

| Property | Description | Default |
|----------|-------------|---------|
| `merkle_tree_chunk_size_in_kb` | Merkle tree chunk size | `null` |
| `merkle_tree_max_proofs` | Maximum merkle tree proofs | `5` |
| `backing_store_memory_threshold_in_mb` | Memory threshold for backing store | `512` |
| `decode_identity_assertions` | Decode identity assertions | `true` |
| `allowed_network_hosts` | Allowed network hosts for SDK requests | `null` |

**Use cases:**

- **Performance tuning for large files**: Set `backing_store_memory_threshold_in_mb` to `2048` or higher for large video files with sufficient RAM
- **Restricted network environments**: Set `allowed_network_hosts` to limit which domains the SDK can contact

### Verify settings

The [`verify` properties](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/#verify) control how the SDK validates C2PA manifests.

The following table lists the key properties (all default to `true`):

| Property | Description | Default |
|----------|-------------|---------|
| `verify_after_reading` | Automatically verify manifests when reading assets | `true` |
| `verify_after_sign` | Automatically verify manifests after signing (recommended) | `true` |
| `verify_trust` | Verify signing certificates against trust anchors | `true` |
| `verify_timestamp_trust` | Verify timestamp authority (TSA) certificates | `true` |
| `remote_manifest_fetch` | Fetch remote manifests referenced in the asset | `true` |
| `ocsp_fetch` | Fetch OCSP responses for certificate validation | `false` |
| `skip_ingredient_conflict_resolution` | Skip ingredient conflict resolution | `false` |
| `strict_v1_validation` | Enable strict C2PA v1 validation | `false` |

> [!WARNING]
> Disabling `verify_trust` or `verify_timestamp_trust` makes verification non-compliant with the C2PA specification. Only modify in controlled environments or with specific requirements.

#### Offline or air-gapped environments

Disable network-dependent features:

```cpp
auto context = std::make_shared<c2pa::Context>(R"({
  "version": 1,
  "verify": {
    "remote_manifest_fetch": false,
    "ocsp_fetch": false
  }
})");

c2pa::Reader reader(context, "signed_asset.jpg");
```

<!-- 
This is to avoid trust errors but errors can be easily missed. 
Removed for now.

**Fast development iteration**

Disable verification for faster iteration:

```cpp
// WARNING: Only use during development, not in production!
c2pa::Settings dev_settings;
dev_settings.set("verify.verify_after_reading", "false");
dev_settings.set("verify.verify_after_sign", "false");

c2pa::Context dev_context(dev_settings);
```

-->

#### Strict validation

Enable all validation features for certification or compliance testing:

```cpp
auto context = std::make_shared<c2pa::Context>(R"({
  "version": 1,
  "verify": {
    "strict_v1_validation": true,
    "ocsp_fetch": true,
    "verify_trust": true,
    "verify_timestamp_trust": true
  }
})");

c2pa::Reader reader(context, "asset_to_validate.jpg");
auto validation_result = reader.json();
```

### Builder settings

The [`builder` settings](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/#buildersettings) control how the SDK creates and embeds C2PA manifests:

- [Builder intent](#builder-intent) to specify the purpose of the claim (for example create or edit).
- [Claim generator information](#claim-generator-information) - Identifies your application in the manifest.
- [Thumbnail settings](#thumbnail-settings)
- Action tracking settings - See [ActionsSettings in the SDK object reference](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema#actionssettings).
- Other builder settings

#### Builder intent

Use the [`builder.intent` setting](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/#builderintent) to specify the purpose of the claim, one of:
- `{"Create": <TYPE>}`: Specifies a new digital creation, where `<TYPE>` is one of the [DigitalSourceType](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/#digitalsourcetype).
- `{"Edit": null}`: An edit of a pre-existing parent asset.
- `{"Update": null}`: A restricted version of the `Edit` type for non-editorial changes.

> [!TIP]
> For more information on intents, see [Intents](https://opensource.contentauthenticity.org/docs/rust-sdk/docs/intents) and [BuilderIntent in the SDK object reference.](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/#builderintent).

**Example: Original digital capture (photos from camera)**

```cpp
c2pa::Context camera_context(R"({
  "version": 1,
  "builder": {
    "intent": {"Create": "digitalCapture"},
    "claim_generator_info": {"name": "Camera App", "version": "1.0"}
  }
})");
```

**Example: Editing existing content**

```cpp
c2pa::Context editor_context(R"({
  "version": 1,
  "builder": {
    "intent": {"Edit": null},
    "claim_generator_info": {"name": "Photo Editor", "version": "2.0"}
  }
})");
```

#### Claim generator information

Identifies your application in the C2PA manifest:

| Property | Description |
|----------|-------------|
| `claim_generator_info.name` | Application name (required, e.g., `"My Photo Editor"`) |
| `claim_generator_info.version` | Application version (recommended, e.g., `"2.1.0"`) |
| `claim_generator_info.icon` | Icon in C2PA format (optional) |
| `claim_generator_info.operating_system` | OS identifier or `"auto"` to auto-detect (optional) |

See [ClaimGeneratorInfoSettings in the SDK object reference](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema#claimgeneratorinfosettings).

**Example:**

```cpp
auto context = std::make_shared<c2pa::Context>(R"({
  "version": 1,
  "builder": {
    "claim_generator_info": {
      "name": "My Photo Editor",
      "version": "2.1.0",
      "operating_system": "auto"
    }
  }
})");

c2pa::Builder builder(context, manifest_json);
```

#### Thumbnail settings

The [`builder.thumbnail`](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/#thumbnailsettings) properties control automatic thumbnail generation:

| Property | Description | Default |
|----------|-------------|---------|
| `enabled` | Enable/disable thumbnail generation | `true` |
| `long_edge` | Maximum size in pixels for the long edge | `1024` |
| `quality` | Quality level: `"low"`, `"medium"`, or `"high"` | `"medium"` |
| `format` | Output format (null for auto-detect) | `null` |
| `prefer_smallest_format` | Prefer smallest format by file size | `true` |
| `ignore_errors` | Continue if thumbnail generation fails | `true` |

**Examples:**

```cpp
// Disable thumbnails for batch processing
c2pa::Context no_thumbnails(R"({
  "builder": {
    "claim_generator_info": {"name": "Batch Processor"},
    "thumbnail": {"enabled": false}
  }
})");

// Customize for mobile (smaller size, lower quality)
c2pa::Context mobile_thumbnails(R"({
  "builder": {
    "thumbnail": {
      "long_edge": 512,
      "quality": "low",
      "prefer_smallest_format": true
    }
  }
})");
```

### Signer settings

The [`signer` properties](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/#signersettings) configure the C2PA signer. Set to `null` if you provide the signer at runtime, or configure as **local** or **remote** in settings.

> [!NOTE]
> In C++, you typically create a `c2pa::Signer` explicitly and pass it to `Builder::sign()`. Settings-based signing is useful when you need the same configuration across multiple operations or when loading from files.

#### Local signer

Use when you have direct access to the private key and certificate. See [signer.local](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/#signerlocal) for all properties.

**Example: Local signer with ES256**

```cpp
std::string config = R"({
  "version": 1,
  "signer": {
    "local": {
      "alg": "es256",
      "sign_cert": "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----",
      "private_key": "-----BEGIN PRIVATE KEY-----\n...\n-----END PRIVATE KEY-----",
      "tsa_url": "http://timestamp.digicert.com"
    }
  }
})";

auto context = std::make_shared<c2pa::Context>(config);
c2pa::Builder builder(context, manifest_json);
builder.sign(source_path, dest_path);  // Uses signer from context
```

#### Remote signer

Use when the private key is on a secure signing service (HSM, cloud KMS). See [signer.remote](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/#signerremote) for all properties.

The signing service receives a POST request with data to sign and must return the signature:

```cpp
c2pa::Context context(R"({
  "version": 1,
  "signer": {
    "remote": {
      "url": "https://signing-service.example.com/sign",
      "alg": "ps256",
      "sign_cert": "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----",
      "tsa_url": "http://timestamp.digicert.com"
    }
  }
})");
```

#### Explicit signer

For full programmatic control, create a `Signer` and pass it to `Builder::sign()`. This is the typical C++ approach:

```cpp
c2pa::Signer signer("es256", certs_pem, private_key_pem, "http://timestamp.digicert.com");
c2pa::Builder builder(context, manifest_json);
builder.sign(source_path, dest_path, signer);
```

The `Context` controls verification and builder options. The signer is used only for the cryptographic signature.

### CAWG X.509 signer configuration

The `cawg_x509_signer` property configures signing for identity assertions. It has the same structure as `signer` (local or remote).

**When to use:** To sign identity assertions separately from the main C2PA claim. When both `signer` and `cawg_x509_signer` are configured, the SDK uses a dual signer:

- Main claim signature from `signer`
- Identity assertions signed with `cawg_x509_signer`

**Example: Dual signer configuration**

```cpp
c2pa::Context context(R"({
  "version": 1,
  "signer": {
    "local": {
      "alg": "es256",
      "sign_cert": "...",
      "private_key": "..."
    }
  },
  "cawg_x509_signer": {
    "local": {
      "alg": "ps256",
      "sign_cert": "...",
      "private_key": "..."
    }
  }
})");
```

## Migrating from deprecated APIs

The SDK introduced Context-based APIs to replace constructors and functions that relied on thread-local state. The older APIs still compile but produce deprecation warnings. This section covers each deprecation that involves passing a `Context`, and explains the `Builder::sign` overloads.

### Quick reference

| Deprecated API | Replacement |
|---|---|
| `load_settings(data, format)` | [`Context` constructors or `ContextBuilder`](#replacing-load_settings) |
| `Reader(format, stream)` | [`Reader(shared_ptr<IContextProvider>, format, stream)`](#adding-a-context-parameter-to-reader-and-builder) |
| `Reader(source_path)` | [`Reader(shared_ptr<IContextProvider>, source_path)`](#adding-a-context-parameter-to-reader-and-builder) |
| `Builder(manifest_json)` | [`Builder(shared_ptr<IContextProvider>, manifest_json)`](#adding-a-context-parameter-to-reader-and-builder) |
| `Reader(IContextProvider&, ...)` | [`Reader(shared_ptr<IContextProvider>, ...)`](#using-shared_ptr-instead-of-reference-for-reader-and-builder) |
| `Builder(IContextProvider&, ...)` | [`Builder(shared_ptr<IContextProvider>, ...)`](#using-shared_ptr-instead-of-reference-for-reader-and-builder) |

### Replacing load_settings

`c2pa::load_settings(data, format)` sets thread-local settings that `Reader` and `Builder` pick up implicitly. Replace it with a `Context` that you pass explicitly.

| Aspect | load_settings (legacy) | Context |
|--------|------------------------|---------|
| Scope | Global / thread-local | Per Reader/Builder, passed explicitly |
| Multiple configs | Awkward (per-thread) | One context per configuration |
| Testing | Shared global state | Isolated contexts per test |

**Deprecated:** 

```cpp
std::ifstream config_file("settings.json");
std::string config((std::istreambuf_iterator<char>(config_file)), std::istreambuf_iterator<char>());
c2pa::load_settings(config, "json");
c2pa::Reader reader("image/jpeg", stream);  // uses thread-local settings
```

**With context API:**

```cpp
c2pa::Context context(settings_json_string);  // or Context(Settings(...))
c2pa::Reader reader(context, "image/jpeg", stream);
```

If you still use `load_settings`, construct `Reader` or `Builder` **without** a context parameter to continue using the thread-local settings. Prefer passing a context for new code.

### Adding a context parameter to Reader and Builder

The following constructors are deprecated because they rely on thread-local settings:

- `Reader(const std::string& format, std::istream& stream)`
- `Reader(const std::filesystem::path& source_path)`
- `Builder(const std::string& manifest_json)`

The migration path is to create a `shared_ptr<Context>` and pass it as the first argument.

**Deprecated:**

```cpp
c2pa::Reader reader("image/jpeg", stream);
c2pa::Reader reader("image.jpg");
c2pa::Builder builder(manifest_json);
```

**With shared_ptr context API:**

```cpp
auto context = std::make_shared<c2pa::Context>();  // or Context(settings) or Context(json)
c2pa::Reader reader(context, "image/jpeg", stream);
c2pa::Reader reader(context, "image.jpg");
c2pa::Builder builder(context, manifest_json);
```

### Using shared_ptr instead of reference for Reader and Builder

The `IContextProvider&` reference overloads are deprecated because they do not extend the lifetime of the context. If the context is destroyed while a `Reader` or `Builder` has a progress callback registered, the callback fires against freed memory, causing a crash.

**Deprecated:**

```cpp
c2pa::Context context;
c2pa::Reader reader(context, "image.jpg");        // reference — context not kept alive
c2pa::Builder builder(context, manifest_json);    // reference — context not kept alive
c2pa::Reader::from_asset(context, "image.jpg");   // reference — context not kept alive
```

**With shared_ptr:**

```cpp
auto context = std::make_shared<c2pa::Context>();
c2pa::Reader reader(context, "image.jpg");
c2pa::Builder builder(context, manifest_json);
c2pa::Reader::from_asset(context, "image.jpg");
```

The `shared_ptr` overloads accept any `shared_ptr<IContextProvider>`, so custom `IContextProvider` implementations work the same way: wrap them in a `shared_ptr` before passing.

#### About IContextProvider

`IContextProvider` is the interface that `Reader` and `Builder` constructors accept. `Context` is the SDK's built-in implementation. The deprecation warnings reference it in the suggested replacement (e.g., `"Use Reader(std::shared_ptr<IContextProvider>, ...)`").

External libraries can implement `IContextProvider` to supply their own context objects. The interface requires a valid `C2paContext*` pointer and an `is_valid()` check. Wrap your implementation in a `shared_ptr` when passing to `Reader` or `Builder`.

> [!IMPORTANT]
> **Progress-callback lifetime in custom providers.** If the native `C2paContext*` was built with a progress callback, the underlying library stores a raw pointer into a heap-owned `ProgressCallbackFunc`. A provider implementation must keep that callback alive for at least as long as the native context. When destroying the provider, the `C2paContext*` must be freed first and the callback storage released after.

#### Releasing to a custom IContextProvider

`Context::ContextBuilder::release()` hands off both the native `C2paContextBuilder*` and the heap-owned progress callback (if any) to the caller. It is the entry point when application code needs to build the native context directly or adopt it into a custom `IContextProvider` implementation.

`ReleasedBuilder` exposes two fields:

- `builder`: the raw `C2paContextBuilder*`. It must be passed to `c2pa_context_builder_build` exactly once.
- `callback_owner`: `std::unique_ptr<c2pa::ProgressCallbackFunc>`; `nullptr` when no progress callback was set. When non-null, it must remain alive at least as long as the native context built from `builder`. If it is destroyed earlier, the library will dereference freed memory on the next progress tick.

When the built-in `Context` is sufficient, `create_context()` is preferred. It performs the same handoff internally and keeps the native handle and callback owner bound together.

### Builder::sign overloads

`Builder::sign` has two kinds of overloads: those that take an explicit `Signer` argument, and those that use the signer stored in the `Builder`'s `Context`.

#### Signing with an explicit Signer

Pass a `Signer` directly when you create signers at runtime or use different signers for different signing operations:

```cpp
c2pa::Signer signer("es256", certs, key, tsa_url);

// Stream-based (preferred)
std::fstream dest("output.jpg", std::ios::in | std::ios::out | std::ios::binary);
builder.sign("image/jpeg", source, dest, signer);

// File-based
builder.sign("source.jpg", "output.jpg", signer);
```

#### Signing with the Context's signer

If a signer is configured in the `Context` (through settings JSON or `ContextBuilder::with_signer()`), you can call `sign` without a `Signer` argument. The context's signer is used automatically. If both a programmatic signer (via `with_signer()`) and a settings-based signer exist, the programmatic signer takes priority.

```cpp
c2pa::Signer signer("es256", certs, key, tsa_url);

auto context = std::make_shared<c2pa::Context>(c2pa::Context::ContextBuilder()
    .with_json(settings_json)
    .with_signer(std::move(signer))  // signer is consumed here
    .create_context());

c2pa::Builder builder(context, manifest_json);

// Stream-based (preferred)
std::fstream dest("output.jpg", std::ios::in | std::ios::out | std::ios::binary);
builder.sign("image/jpeg", source, dest);

// File-based
builder.sign("source.jpg", "output.jpg");
```

This is useful when you want to configure signing once and reuse the same context across multiple builders without passing the signer to each `sign` call.

**Deprecated:**

```cpp
std::ofstream out("output.jpg", std::ios::binary);
builder.sign("image/jpeg", source, out, signer);
```

**With context API:**

```cpp
std::fstream dest("output.jpg", std::ios::in | std::ios::out | std::ios::binary);
builder.sign("image/jpeg", source, dest, signer);
```
