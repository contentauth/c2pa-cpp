# Using the C++ library

To use this library, include the header file in your code as follows:

```cpp
#include "c2pa.hpp"
```

## Read and validate an istream

Use the `Reader` constructor to read C2PA data from a stream. This constructor examines the specified stream for C2PA data in the given format and its return value is a Reader that can be used to extract more information. Exceptions are thrown on errors.

```cpp
  c2pa::Context context;  // or Context(settings) or Context(json_string)
  auto reader = c2pa::Reader(context, <"FORMAT">, <"STREAM">);
```

The parameters are:

- `context` - A `Context` (or any `IContextProvider`) that configures SDK behavior. See [Configuring the SDK with Context and Settings](context-settings.md).
- `<FORMAT>` - A MIME type or file extension for the stream, for example `image/jpeg` or `jpg`; see the [supported file formats](https://github.com/contentauth/c2pa-rs/blob/main/docs/supported-formats.md). Pass an empty string to take the format from the stream's leading bytes instead.
- `<STREAM>` - An open readable iostream.

Passing an empty string (or use a format-less overload of a `Reader`) asks the native core library to guess the format (the read will still fail if the format can't be guessed or is unsupported):

```cpp
c2pa::Context context;
std::ifstream ifs("asset.bin", std::ios::binary);
auto reader = c2pa::Reader(std::make_shared<c2pa::Context>(), ifs);
```

The used stream must support seeking, since it is rewound after inspection. A file path with no extension is read the same way.

Prefer passing the format when you know it. When you do pass one, it is reconciled against the container detected in the leading bytes:

- Both identify the same container: your format is used, keeping the more specific spelling (`dng` stays `dng` rather than widening to `image/tiff`).
- They identify different containers: the detected container wins.
- Detection finds no container: the format is used as given.

If detection detects no container and no format was supplied, the read fails with an unsupported-type error. Note that the format is a hint, not a constraint: the library does not reject a format merely because it is absent from `Reader::supported_mime_types()`, and an unrecognized format on recognizable bytes still reads.

`Builder` always requires an explicit format, because the container type decides how the asset is written and hashed. Signing with a blank format, or to a destination path with no extension, throws.

For example:

```cpp
c2pa::Context context;
std::ifstream ifs("tests/fixtures/C.jpg", std::ios::binary);

// the Reader supports streams or file paths
auto reader = c2pa::Reader(context, "image/jpeg", ifs);

// print out the Manifest Store information
printf("Manifest Store = %s", reader.json())

// write the thumbnail into a file
std::ofstream ofs("test_thumbnail.jpg", std::ios::binary);
reader.get_resource("self#jumbf=c2pa.assertions/c2pa.thumbnail.claim.jpeg", ofs);
ifs.close();
```

## Creating a manifest JSON definition

The manifest JSON string defines the C2PA manifest to add to the file.

A sample JSON manifest is provided in [tests/fixtures/training.json](https://github.com/contentauth/c2pa-cpp/blob/main/tests/fixtures/training.json).

For example:

```cpp
const std::string manifest_json = R"{
    "claim_generator_info": [
      {
        "name": "c2pa-cpp test",
        "version": "0.1"
      }
    ],
    "assertions": [
    {
      "label": "c2pa.training-mining",
      "data": {
        "entries": {
          "c2pa.ai_generative_training": { "use": "notAllowed" },
          "c2pa.ai_inference": { "use": "notAllowed" },
          "c2pa.ai_training": { "use": "notAllowed" },
          "c2pa.data_mining": { "use": "notAllowed" }
        }
      }
    }
  ]
 };
```

## Using settings

The behavior of the SDK can be configured through various [settings](https://opensource.contentauthenticity.org/docs/manifest/json-ref/settings-schema/) loaded from JSON config files or JSON strings directly in the code.

SDK settings are set on the `Context` objects used by the Builder and Reader objects. 
For full details see [Configuring the SDK with Context and Settings](context-settings.md).

> [!NOTE] If you don't specify a value for a property, then the SDK will use the default value. 
> If you specify a value of null, then the property will be set to null, not the default.

### Creating a Context

The `Context` class manages C2PA SDK configuration. There are two ways to create a context: direct construction and using `ContextBuilder`.

#### Direct construction

```cpp
// Default settings
c2pa::Context context;

// From a Settings object
c2pa::Settings settings;
settings.set("builder.thumbnail.enabled", "true");
c2pa::Context context(settings);

// From a JSON configuration string
c2pa::Context context(R"({
  "builder": {
    "thumbnail": {
      "enabled": true
    }
  }
})");
```

#### Using a Context

Contexts are passed by reference to `Builder` and `Reader` constructors. The context is used only at construction; the implementation copies context state into the reader/builder, so the context does not need to outlive them.

```cpp
c2pa::Context context;
c2pa::Builder builder(context, manifest_json);
c2pa::Reader reader(context, "image.jpg");
```

## Creating a Builder

Use the `Builder` constructor to create a `Builder` instance. A `Context` is required as the first parameter.

```cpp
  c2pa::Context context;
  auto builder = c2pa::Builder(context, "<MANIFEST_JSON>");
```

The parameters are:

- `context` - A `Context` (or any `IContextProvider`) that configures SDK behavior. See [Configuring the SDK with Context and Settings](context-settings.md).
- `<MANIFEST_JSON>` - A string in JSON format as described above, defining the manifest to be generated.

For example:

```cpp
  c2pa::Context context;
  auto builder = c2pa::Builder(context, manifest_json);
```

## Creating a Signer

For testing, you can create a signer using any supported algorithm by a `Signer` constructor. For the list of supported signing algorithms, see [Creating and using an X.509 certificate](https://opensource.contentauthenticity.org/docs/c2patool/x_509).

There are multiple constructor forms; this example shows how to create a signer with a public/private key pair.

```cpp
  Signer signer = Signer("<SIGNING_ALG>", "<PUBLIC_CERTS>",  "<PRIVATE_KEY>", "<TIMESTAMP_URL>");
```

The parameters are:
- `<SIGNING_ALG>`- The `C2paSigningAlg` from `c2pa.h` associated with the signing function.
- `<PUBLIC_CERTS>`- A buffer containing the public cert chain in PEM format.
- `<PRIVATE_KEY>`- A buffer containing the private_key in PEM format.
- `<TIMESTAMP_URL>`- An optional parameter containing a URL to a public Time Stamp Authority service.

For example:

```cpp
Signer signer = c2pa::Signer("Es256", certs, private_key, "http://timestamp.digicert.com");
```

> [!WARNING]
> Do not access a private key and certificate directly like this in production  because it's not secure. Instead use a hardware security module (HSM) and optionally a Key Management Service (KMS) to access the key; for example as shown in the [C2PA Python Example](https://github.com/contentauth/c2pa-python-example).

## Signing and embedding a manifest

A media file may contain many manifests in a manifest store. The `active_manifest` property in the manifest store identifies the most recently-added manifest.  For a comprehensive reference to the JSON manifest structure, see the [CAI manifest store reference](https://opensource.contentauthenticity.org/docs/manifest/manifest-ref).

```cpp
  auto manifest_data = builder.sign(image_path, output_path, signer);
```

The parameters are:

- `<SOURCE_ASSET>`- A file path or an istream referencing the asset to sign.
- `<OUTPUT_ASSET>`- A file path or an iostream referencing the asset to generate.
- `<SIGNER>` - A `Signer` instance.

For example:
```cpp
  auto manifest_data = builder.sign("source_asset.jpg", "output_asset.jpg", signer);
```

## CAWG identity

The C++ library can validate [CAWG identity assertions](https://cawg.io/identity/).

## On trust configurations

C2PA maintains two [trust lists](https://opensource.contentauthenticity.org/docs/conformance/trust-lists/)
to verify the authenticity and integrity of Content Credentials attached to digital media: the C2PA trust list and the C2PA time-stamping authority (TSA) trust list. The C2PA trust list is a list of X.509 certificate trust anchors (either root or subordinate certification authorities) that issue certificates to conforming generator products under the C2PA Certificate Policy. The C2PA time-stamping authority (TSA) trust list is a list of X.509 certificate trust anchors (either root or subordinate certification authorities) that issue time-stamp signing certificates to TSAs.

Configure trust lists using the `Settings` and `Context` APIs. Using the `Context` API ensures proper propagation of settings (and trust) to `Builder` and `Reader` objects.

Trust affects manifest validation status: a manifest whose trust chain was verified will be flagged as `Trusted`.

## More examples

The C++ example in [`examples/training.cpp`](https://github.com/contentauth/c2pa-cpp/blob/main/examples/training.cpp) uses the [JSON for Modern C++](https://json.nlohmann.me/) library class.

Build and run the example by entering this `make` command:

```
make examples
```

This example adds the manifest [`tests/fixtures/training.json`](https://github.com/contentauth/c2pa-cpp/blob/main/tests/fixtures/training.json) to the image file [`tests/fixtures/A.jpg`](https://github.com/contentauth/c2pa-cpp/blob/main/tests/fixtures/A.jpg) using the sample private key and certificate in the [`tests/fixtures`](https://github.com/contentauth/c2pa-cpp/tree/main/tests/fixtures) directory.

The example displays some text to standard out that summarizes whether AI training is allowed based on the specified manifest and then saves the resulting image file with attached manifest to `build/examples/training.jpg`.
