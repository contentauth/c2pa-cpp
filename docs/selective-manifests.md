# Selective manifest construction

You can use `Builder` and `Reader` together to selectively construct manifests&mdash;keeping only the parts you need and omitting the rest. This is useful when you don't want to include all ingredients in a working store (for example, when some ingredient assets are not visible).

This process is best described as *filtering* or *rebuilding* a working store:

1. Read an existing manifest.
2. Choose which elements to retain.
3. Build a new manifest containing only those elements.

A manifest is a signed data structure attached to an asset that records provenance and which source assets (ingredients) contributed to it. It contains assertions (statements about the asset), ingredients (references to other assets), and references to binary resources (such as thumbnails).

Since both `Reader` and `Builder` are **read-only** by design (neither has a `remove()` method), to exclude content you must **read what exists, filter to keep what you need, and create a new** `Builder` **with only that information**. This produces a new `Builder` instance—a "rebuild."

> [!IMPORTANT]
> This process always creates a new `Builder`. The original signed asset and its manifest are never modified, neither is the starting working store. The `Reader` extracts data without side effects, and the `Builder` constructs a new manifest based on extracted data.

## Core concepts

```mermaid
flowchart LR
    A[Signed Asset] -->|Reader| B[JSON + Resources]
    B -->|Filter| C[Filtered Data]
    C -->|new Builder| D[New Builder]
    D -->|sign| E[New Asset]
```



The fundamental workflow is:

1. **Read** the existing manifest with `Reader` to get JSON and binary resources
2. **Identify and filter** the parts to keep (parse the JSON, select and gather elements)
3. **Create a new `Builder`** with only the selected parts based on the applied filtering rules
4. **Sign** the new `Builder` into the output asset

## Reading an existing manifest

Use `Reader` to extract the manifest store JSON and any binary resources (thumbnails, manifest data). The source asset is never modified.

```cpp
c2pa::Context context;
c2pa::Reader reader(context, "image/jpeg", source_stream);

// Get the full manifest store as JSON
std::string store_json = reader.json();
auto parsed = json::parse(store_json);

// Identify the active manifest, which is the current/latest manifest
std::string active = parsed["active_manifest"];
auto manifest = parsed["manifests"][active];

// Access specific parts
auto ingredients  = manifest["ingredients"];
auto assertions   = manifest["assertions"];
auto thumbnail_id = manifest["thumbnail"]["identifier"];
```

### Extracting binary resources

The JSON returned by `reader.json()` only contains string identifiers (JUMBF URIs) for binary data like thumbnails and ingredient manifest stores. Extract the actual binary content by using `get_resource()`:

```cpp
// Extract a thumbnail to a stream
std::stringstream thumb_stream(std::ios::in | std::ios::out | std::ios::binary);
reader.get_resource(thumbnail_id, thumb_stream);

// Or extract to a file
reader.get_resource(thumbnail_id, fs::path("thumbnail.jpg"));
```

## Filtering into a new Builder

Each example below creates a **new `Builder`** from filtered data. The original asset and its manifest store are never modified.

When transferring ingredients from a `Reader` to a new `Builder`, you must transfer both the JSON metadata and the associated binary resources (thumbnails, manifest data). The JSON contains identifiers that reference those resources; the same identifiers must be used when calling `builder.add_resource()`.

> **Transferring binary resources:** For each kept ingredient, call `reader.get_resource(id, stream)` for any `thumbnail` or `manifest_data` it contains, then `builder.add_resource(id, stream)` with the same identifier.

### Keep only specific ingredients

```cpp
c2pa::Context context;
c2pa::Reader reader(context, "image/jpeg", source_stream);
auto parsed = json::parse(reader.json());
std::string active = parsed["active_manifest"];
auto ingredients = parsed["manifests"][active]["ingredients"];

// Filter: keep only ingredients with a specific relationship
json kept_ingredients = json::array();
for (auto& ingredient : ingredients) {
    if (ingredient["relationship"] == "parentOf") {
        kept_ingredients.push_back(ingredient);
    }
}

// Create a new Builder with only the kept ingredients
json new_manifest = json::parse(base_manifest_json);
new_manifest["ingredients"] = kept_ingredients;

c2pa::Builder builder(context, new_manifest.dump());

// Transfer binary resources for kept ingredients (see note above)
for (auto& ingredient : kept_ingredients) {
    if (ingredient.contains("thumbnail")) {
        std::string id = ingredient["thumbnail"]["identifier"];
        std::stringstream s(std::ios::in | std::ios::out | std::ios::binary);
        reader.get_resource(id, s);
        s.seekg(0);
        builder.add_resource(id, s);
    }
    if (ingredient.contains("manifest_data")) {
        std::string id = ingredient["manifest_data"]["identifier"];
        std::stringstream s(std::ios::in | std::ios::out | std::ios::binary);
        reader.get_resource(id, s);
        s.seekg(0);
        builder.add_resource(id, s);
    }
}

// Sign the new Builder into an output asset
builder.sign(source_path, output_path, signer);
```

### Keep only specific assertions

```cpp
auto assertions = parsed["manifests"][active]["assertions"];

json kept_assertions = json::array();
for (auto& assertion : assertions) {
    // Keep training-mining assertions, filter out everything else
    if (assertion["label"] == "c2pa.training-mining") {
        kept_assertions.push_back(assertion);
    }
}

json new_manifest = json::parse(R"({
    "claim_generator_info": [{"name": "an-application", "version": "0.1.0"}]
})");
new_manifest["assertions"] = kept_assertions;

// Create a new Builder with only the filtered assertions
c2pa::Builder builder(context, new_manifest.dump());
builder.sign(source_path, output_path, signer);
```

### Start fresh and preserve provenance

Sometimes all existing assertions and ingredients may need to be discarded but the provenance chain should be maintained nevertheless. This is done by creating a new `Builder` with a new manifest definition and adding the original signed asset as an ingredient using `add_ingredient()`.

The function `add_ingredient()` does not copy the original's assertions into the new manifest. Instead, it stores the original's entire manifest store as opaque binary data inside the ingredient record. This means:

- The new manifest has its own, independent set of assertions
- The original's full manifest is preserved inside the ingredient, so validators can inspect the full provenance history
- The provenance chain is unbroken: anyone reading the new asset can follow the ingredient link back to the original

```mermaid
flowchart TD
    subgraph Original["Original Signed Asset"]
        OA["Assertions: A, B, C"]
        OI["Ingredients: X, Y"]
    end
    subgraph NewBuilder["New Builder"]
        NA["Assertions: (empty or new)"]
        NI["Ingredient: original.jpg (contains full original manifest as binary data)"]
    end
    Original -->|"add_ingredient()"| NI
    NI -.->|"validators can trace back"| Original

    style NA fill:#efe,stroke:#090
    style NI fill:#efe,stroke:#090
```



```cpp
// Create a new Builder with a new definition
c2pa::Builder builder(context);
builder.with_definition(R"({
    "claim_generator_info": [{"name": "an-application", "version": "0.1.0"}],
    "assertions": []
})");

// Add the original as an ingredient to preserve provenance chain.
// add_ingredient() stores the original's manifest as binary data inside the ingredient,
// but does NOT copy the original's assertions into this new manifest.
builder.add_ingredient(R"({"title": "original.jpg", "relationship": "parentOf"})",
                       original_signed_path);
builder.sign(source_path, output_path, signer);
```

## Adding actions to a working store

Actions record what was done to an asset (e.g., color adjustments, cropping, placing content). Use `builder.add_action()` to add them to a working store.

```cpp
builder.add_action(R"({
    "action": "c2pa.color_adjustments",
    "parameters": { "name": "brightnesscontrast" }
})");

builder.add_action(R"({
    "action": "c2pa.filtered",
    "parameters": { "name": "A filter" },
    "description": "Filtering applied"
})");
```

### Action JSON fields


| Field | Required | Description |
| --- | --- | --- |
| `action` | Yes | Action identifier, e.g. `"c2pa.created"`, `"c2pa.opened"`, `"c2pa.placed"`, `"c2pa.color_adjustments"`, `"c2pa.filtered"` |
| `parameters` | No | Free-form object with action-specific data (including `ingredientIds` for linking ingredients, for instance) |
| `description` | No | Human-readable description of what happened |
| `digitalSourceType` | Sometimes, depending on action | URI describing the digital source type (typically for `c2pa.created`) |


### Linking actions to ingredients

When an action involves a specific ingredient, the ingredient is linked to the action using `ingredientIds` (in the action's `parameters`), referencing a matching key in the ingredient.

#### How `ingredientIds` resolution works

The SDK matches each value in `ingredientIds` against ingredients using this priority:

1. `label` on the ingredient (primary): if set and non-empty, this is used as the linking key.
2. `instance_id` on the ingredient (fallback): used when `label` is absent or empty.

#### Linking with `label`

The `label` field on an ingredient is the **primary** linking key. Set a `label` on the ingredient and reference it in the action's `ingredientIds`. The label can be any string: it acts as a linking key between the ingredient and the action.

```cpp
c2pa::Context context;

auto manifest_json = R"(
{
    "claim_generator_info": [{ "name": "an-application", "version": "1.0" }],
    "assertions": [
        {
            "label": "c2pa.actions.v2",
            "data": {
                "actions": [
                    {
                        "action": "c2pa.created",
                        "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation"
                    },
                    {
                        "action": "c2pa.placed",
                        "parameters": {
                            "ingredientIds": ["c2pa.ingredient.v3"]
                        }
                    }
                ]
            }
        }
    ]
}
)";

c2pa::Builder builder(context, manifest_json);

// The label on the ingredient matches the value in ingredientIds
auto ingredient_json = R"(
{
    "title": "photo.jpg",
    "format": "image/jpeg",
    "relationship": "componentOf",
    "label": "c2pa.ingredient.v3"
}
)";
builder.add_ingredient(ingredient_json, photo_path);

builder.sign(source_path, output_path, signer);
```

##### Linking multiple ingredients

When linking multiple ingredients, each ingredient needs a unique label.

> [!NOTE]
> The labels used for linking in the working store may not be the exact labels that appear in the signed manifest. They are indicators for the SDK to know which ingredient to link with which action. The SDK assigns final labels during signing.

```cpp
auto manifest_json = R"(
{
    "claim_generator_info": [{ "name": "an-application", "version": "1.0" }],
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

c2pa::Builder builder(context, manifest_json);

// parentOf ingredient linked to c2pa.opened
builder.add_ingredient(R"({
    "title": "original.jpg",
    "format": "image/jpeg",
    "relationship": "parentOf",
    "label": "c2pa.ingredient.v3_1"
})", original_path);

// componentOf ingredient linked to c2pa.placed
builder.add_ingredient(R"({
    "title": "overlay.jpg",
    "format": "image/jpeg",
    "relationship": "componentOf",
    "label": "c2pa.ingredient.v3_2"
})", overlay_path);

builder.sign(source_path, output_path, signer);
```

#### Linking with `instance_id`

When no `label` is set on an ingredient, the SDK matches `ingredientIds` against `instance_id`.

```cpp
c2pa::Context context;

// instance_id is used as the linking identifier and must be unique
std::string instance_id = "xmp:iid:939a4c48-0dff-44ec-8f95-61f52b11618f";

json manifest_json = {
    {"claim_generator_info", json::array({{{"name", "an-application"}, {"version", "1.0"}}})},
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

c2pa::Builder builder(context, manifest_json.dump());

// No label set: instance_id is used as the linking key
json ingredient = {
    {"title", "source_photo.jpg"},
    {"relationship", "parentOf"},
    {"instance_id", instance_id}
};
builder.add_ingredient(ingredient.dump(), source_photo_path);

builder.sign(source_path, output_path, signer);
```

> [!NOTE]
> The `instance_id` can be read back from the ingredient JSON after signing.

#### Reading linked ingredients

After signing, `ingredientIds` is gone. The action's `parameters.ingredients[]` contains hashed JUMBF URIs pointing to ingredient assertions. To match an action to its ingredient, extract the label from the URL:

```cpp
auto reader = c2pa::Reader(context, signed_path);
auto parsed = json::parse(reader.json());
std::string active = parsed["active_manifest"];
auto manifest = parsed["manifests"][active];

// Build a map: label -> ingredient
std::map<std::string, json> label_to_ingredient;
for (auto& ing : manifest["ingredients"]) {
    label_to_ingredient[ing["label"]] = ing;
}

// Match each action to its ingredients by extracting labels from URLs
for (auto& assertion : manifest["assertions"]) {
    if (assertion["label"] == "c2pa.actions.v2") {
        for (auto& action : assertion["data"]["actions"]) {
            if (action.contains("parameters") &&
                action["parameters"].contains("ingredients")) {
                for (auto& ref : action["parameters"]["ingredients"]) {
                    std::string url = ref["url"];
                    std::string label = url.substr(url.rfind('/') + 1);
                    auto& matched = label_to_ingredient[label];
                    // Now the ingredient is available
                }
            }
        }
    }
}
```

#### When to use `label` vs `instance_id`

| Property | `label` | `instance_id` |
| --- | --- | --- |
| **Who controls it** | Caller (any string) | Caller (any string, or from XMP metadata) |
| **Priority for linking** | Primary: checked first | Fallback: used when label is absent/empty |
| **When to use** | JSON-defined manifests where the caller controls the ingredient definition | Programmatic workflows where a stable identifier persisting unchanged across rebuilds is needed (`read_ingredient_file()` is deprecated) |
| **Survives signing** | SDK may reassign the actual assertion label | Unchanged |
| **Stable across rebuilds** | The caller controls the build-time value; the post-signing label may change | Yes, always the same set value |


Use `label` when defining manifests in JSON. Use `instance_id` when working programmatically with ingredients whose identity comes from other sources, or when a stable identifier that persists unchanged across rebuilds is needed.

## Working with archives

A `Builder` represents a **working store**: a manifest that is being assembled but has not yet been signed. Archives serialize this working store (definition + resources) to a `.c2pa` binary format, allowing to save, transfer, or resume the work later. For more background on working stores and archives, see [Working stores](https://opensource.contentauthenticity.org/docs/rust-sdk/docs/working-stores).

There are two distinct types of archives, sharing the same binary format but being conceptually different: builder archives and ingredient archives.

### Builder archives vs. ingredient archives

A **builder archive** is a serialized snapshot of a `Builder` (i.e. of a working store). The term *working store* refers to the unsigned `Builder` itself; the *builder archive* is its serialized `.c2pa` form. The archive contains the manifest definition, all resources, and any ingredients that were added. It is created by `builder.to_archive()` and restored with `Builder::from_archive()` or `builder.with_archive()`.

An **ingredient archive** contains the manifest store from an asset that was added as an ingredient.

The key difference: a builder archive is a work-in-progress (unsigned). An ingredient archive carries the provenance history of a source asset for reuse as an ingredient in other working stores.

### Two ways to produce an ingredient archive

The SDK supports two approaches for producing an ingredient archive. They share the same `.c2pa` binary format and are interchangeable from the consumer side.

| Approach | Entry point | Status |
| --- | --- | --- |
| Dedicated ingredient archive APIs | `add_ingredient` then `write_ingredient_archive(id, stream)` | **Recommended** |
| Read-filter-rebuild pattern | `Builder` + `add_ingredient` + `to_archive`, then `Reader` + manual JSON | **Older pattern** (see [catalog migration guide](#migration-guide-catalog-pattern) and [extraction migration guide](#migration-guide-ingredient-extraction)) |

For the full contract (id resolution, error cases, examples), see [Single-ingredient archive APIs](./working-stores.md#single-ingredient-archive-apis) in the working stores guide.

## The ingredients catalog pattern

An **ingredients catalog** is a collection of archived ingredients that can be selected when constructing a final manifest. Each archive holds ingredients; at build time the caller selects only the ones needed.

```mermaid
flowchart TD
    subgraph Catalog["Ingredients Catalog (archived)"]
        A1["Archive: photos.c2pa (ingredients from photo shoot)"]
        A2["Archive: graphics.c2pa (ingredients from design assets)"]
        A3["Archive: audio.c2pa (ingredients from audio tracks)"]
    end
    subgraph Build["Final Builder"]
        direction TB
        SEL["Pick and choose ingredients from any archive in the catalog"]
        FB["New Builder with selected ingredients only"]
    end
    A1 -->|"select photo_1, photo_3"| SEL
    A2 -->|"select logo"| SEL
    A3 -. "skip (not needed)" .-> X((not used))
    SEL --> FB
    FB -->|sign| OUT[Signed Output Asset]

    style A3 fill:#eee,stroke:#999
    style X fill:#f99,stroke:#c00
```

The catalog can be implemented two ways. The dedicated (ingredient) archives API uses one archive per ingredient.

Alternatively, a single builder archive can hold many ingredients (a multi-ingredient builder archive is still just a builder archive, not a deprecated format), and the read-filter-rebuild *pattern* slices out a subset of ingredients (and resources) from it. The pattern is the older approach; the archive itself is not legacy.

### Dedicated archives API: one ingredient per archive

The producer registers each ingredient on a builder and writes one archive per ingredient, keyed by `instance_id`. The consumer assembles a final builder by loading only the archives it needs via `add_ingredient_from_archive`.

The first argument to `write_ingredient_archive` is the *archive key*: it locates the ingredient on the producer (matched against either `label` or `instance_id`) and becomes the `ingredientIds` value to use on the signing builder. See [Lookup keys and action linking](working-stores.md#lookup-keys-and-action-linking) for the full rules.

> [!NOTE]
> `"relationship": "componentOf"` is shown explicitly below, but `componentOf` is the default the SDK applies when `relationship` is omitted.

Producer side, build the catalog:

```cpp
auto settings = c2pa::Settings();
auto context = c2pa::Context::ContextBuilder()
    .with_settings(std::move(settings))
    .create_context();

auto catalog_builder = c2pa::Builder(context, manifest_json);
catalog_builder.add_ingredient(
    R"({"title": "photo-A.jpg", "relationship": "componentOf", "instance_id": "catalog:ingredient-A"})",
    "photo-A.jpg");
catalog_builder.add_ingredient(
    R"({"title": "photo-B.jpg", "relationship": "componentOf", "instance_id": "catalog:ingredient-B"})",
    "photo-B.jpg");
catalog_builder.add_ingredient(
    R"({"title": "photo-C.jpg", "relationship": "componentOf", "instance_id": "catalog:ingredient-C"})",
    "photo-C.jpg");

// One archive per ingredient, keyed by the instance_id used at registration.
std::stringstream archive_a(std::ios::in | std::ios::out | std::ios::binary);
std::stringstream archive_b(std::ios::in | std::ios::out | std::ios::binary);
std::stringstream archive_c(std::ios::in | std::ios::out | std::ios::binary);
catalog_builder.write_ingredient_archive("catalog:ingredient-A", archive_a);
catalog_builder.write_ingredient_archive("catalog:ingredient-B", archive_b);
catalog_builder.write_ingredient_archive("catalog:ingredient-C", archive_c);
```

Consumer side, pick one archive and load it:

```cpp
auto final_builder = c2pa::Builder(context, manifest_json);
archive_b.seekg(0);
final_builder.add_ingredient_from_archive(archive_b);

final_builder.sign(source_path, output_path, signer);
```

The signed output contains exactly the picked ingredient (`photo-B.jpg` here). `archive_a` stays unused.

A single action can link several ingredients loaded this way. With the three archives from the producer above (`catalog:ingredient-A`, `catalog:ingredient-B`, `catalog:ingredient-C`) loaded into one signing builder, a `c2pa.placed` action that lists all three ids in `ingredientIds` resolves to three distinct ingredient URLs after signing:

```cpp
auto signing_builder = c2pa::Builder(context, R"({
    "claim_generator_info": [{"name": "an-application", "version": "0.1.0"}],
    "assertions": [{
        "label": "c2pa.actions.v2",
        "data": {
            "actions": [{
                "action": "c2pa.placed",
                "parameters": {
                    "ingredientIds": ["catalog:ingredient-A", "catalog:ingredient-B", "catalog:ingredient-C"]
                }
            }]
        }
    }]
})");

archive_a.seekg(0);
archive_b.seekg(0);
archive_c.seekg(0);
signing_builder.add_ingredient_from_archive(archive_a);
signing_builder.add_ingredient_from_archive(archive_b);
signing_builder.add_ingredient_from_archive(archive_c);

signing_builder.sign(source_path, output_path, signer);
```

### Legacy catalog: read-filter-rebuild APIs

> [!NOTE]
> **Legacy approach.** This pattern requires manual JSON parsing and `add_resource` loops to transfer binary data. See [Migration guide](#migration-guide-catalog-pattern) to use the [dedicated ingredient archive APIs](#dedicated-archives-api-one-ingredient-per-archive) instead.

Use this approach when the catalog already exists as a single `.c2pa` builder archive containing many ingredients and you need to pick a subset by reading, filtering, and rebuilding.

```cpp
// Read from a catalog of archived ingredients
c2pa::Context archive_ctx;  // Add settings if needed, e.g. verify options

// Open one archive from the catalog
archive_stream.seekg(0);
c2pa::Reader reader(archive_ctx, "application/c2pa", archive_stream);
auto parsed = json::parse(reader.json());
std::string active = parsed["active_manifest"];
auto available_ingredients = parsed["manifests"][active]["ingredients"];

// Pick only the needed ingredients
json selected = json::array();
for (auto& ingredient : available_ingredients) {
    if (ingredient["title"] == "photo_1.jpg" || ingredient["title"] == "logo.png") {
        selected.push_back(ingredient);
    }
}

// Create a new Builder with selected ingredients
json manifest = json::parse(R"({
    "claim_generator_info": [{"name": "an-application", "version": "0.1.0"}]
})");
manifest["ingredients"] = selected;
c2pa::Builder builder(context, manifest.dump());

// Transfer binary resources for selected ingredients
for (auto& ingredient : selected) {
    if (ingredient.contains("thumbnail")) {
        std::string id = ingredient["thumbnail"]["identifier"];
        std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
        reader.get_resource(id, stream);
        stream.seekg(0);
        builder.add_resource(id, stream);
    }
    if (ingredient.contains("manifest_data")) {
        std::string id = ingredient["manifest_data"]["identifier"];
        std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
        reader.get_resource(id, stream);
        stream.seekg(0);
        builder.add_resource(id, stream);
    }
}

builder.sign(source_path, output_path, signer);
```

#### Migration guide: catalog pattern

Switch to the dedicated ingredient archive APIs: set `instance_id` per ingredient, call `write_ingredient_archive` once per ingredient on the producer, and `add_ingredient_from_archive` on the consumer. No JSON parsing or `add_resource` loops required.

Producer side:

```cpp
auto settings = c2pa::Settings();
auto context = c2pa::Context::ContextBuilder()
    .with_settings(std::move(settings))
    .create_context();

auto catalog_builder = c2pa::Builder(context, manifest_json);
catalog_builder.add_ingredient(
    R"({"title": "photo-A.jpg", "relationship": "componentOf", "instance_id": "catalog:ingredient-A"})",
    "photo-A.jpg");
catalog_builder.add_ingredient(
    R"({"title": "photo-B.jpg", "relationship": "componentOf", "instance_id": "catalog:ingredient-B"})",
    "photo-B.jpg");

std::stringstream archive_a(std::ios::in | std::ios::out | std::ios::binary);
std::stringstream archive_b(std::ios::in | std::ios::out | std::ios::binary);
catalog_builder.write_ingredient_archive("catalog:ingredient-A", archive_a);
catalog_builder.write_ingredient_archive("catalog:ingredient-B", archive_b);
```

Consumer side:

```cpp
auto final_builder = c2pa::Builder(context, manifest_json);
archive_b.seekg(0);
final_builder.add_ingredient_from_archive(archive_b);
final_builder.sign(source_path, output_path, signer);
```

Action linking also changes between the two approaches. Legacy catalog code linked ingredients via `label` set on the signing builder's `add_ingredient` JSON; `instance_id` was not accepted. The dedicated archive API accepts the archive key passed to `write_ingredient_archive`, which can be either `label` or `instance_id`. See [Lookup keys and action linking](working-stores.md#lookup-keys-and-action-linking).

#### Choosing between approaches

The legacy read-filter-rebuild APIs fit when the catalog already exists as one multi-ingredient builder archive and the consumer wants a subset of it. The dedicated ingredient archive APIs fit when ingredients are produced and consumed independently: each archive holds exactly one ingredient, and the call sites stay short. Both produce the same signed output.

### Identifying ingredients in archives

Setting `instance_id` on an ingredient gives it a stable, caller-controlled identifier. This field survives archiving and signing unchanged, so it can locate a specific ingredient in a catalog. The `description` and `informational_URI` fields also survive and can carry additional metadata about the ingredient's origin.

For the dedicated archive methods, `instance_id` is the preferred key: it is the only one of these identifiers observable in the signed manifest. A `label` is a builder-only linking key and does not appear in the signed output (see [Lookup keys and action linking](working-stores.md#lookup-keys-and-action-linking)).

For the legacy load path (`add_ingredient(json, "application/c2pa", archive)`), `instance_id` cannot be used as a linking key in `ingredientIds`; use `label` instead (see [Linking an archived ingredient to an action](#linking-an-archived-ingredient-to-an-action)). For the dedicated `write_ingredient_archive` + `add_ingredient_from_archive` ingredient archive APIs, the archive key can be either `label` or `instance_id` and becomes the `ingredientIds` value (see [Lookup keys and action linking](working-stores.md#lookup-keys-and-action-linking)).

With the dedicated single-ingredient API, `instance_id` also serves as the lookup key passed to `write_ingredient_archive`. Set it on `add_ingredient`, then pass the same value to write the archive:

> [!NOTE]
> A caller-set `instance_id` replaces the ingredient asset's own XMP `instance_id`. Use a value you control (as in `catalog:photo-A` below) when the archive key matters more than preserving the asset's original XMP id.

```cpp
// Producer: register ingredient with instance_id, write its archive.
auto settings = c2pa::Settings();
auto context = c2pa::Context::ContextBuilder()
    .with_settings(std::move(settings))
    .create_context();

auto builder = c2pa::Builder(context, manifest_str);
builder.add_ingredient(
    R"({"title": "photo-A.jpg", "relationship": "componentOf", "instance_id": "catalog:photo-A"})",
    source_path);

std::stringstream archive_a(std::ios::in | std::ios::out | std::ios::binary);
builder.write_ingredient_archive("catalog:photo-A", archive_a);

// Consumer: load this archive directly, no Reader loop required.
auto consumer = c2pa::Builder(context, manifest_json);
archive_a.seekg(0);
consumer.add_ingredient_from_archive(archive_a);
consumer.sign(source_path, output_path, signer);
```

#### Legacy: `to_archive` + Reader loop

> [!NOTE]
> **Legacy approach.** The pattern below archives a multi-ingredient builder and uses a `Reader` loop to find ingredients by `instance_id`.

```cpp
// Set instance_id when adding the ingredient to the archive builder.
auto builder = c2pa::Builder(context, manifest_str);
builder.add_ingredient(
    R"({
        "title": "photo-A.jpg",
        "relationship": "componentOf",
        "instance_id": "catalog:photo-A"
    })",
    source_path);

builder.to_archive("catalog.c2pa");
```

Later, when reading the archive, select ingredients by their `instance_id`:

```cpp
auto reader = c2pa::Reader(context, "catalog.c2pa");
auto parsed = json::parse(reader.json());
std::string active = parsed["active_manifest"];
auto& ingredients = parsed["manifests"][active]["ingredients"];

for (auto& ing : ingredients) {
    if (ing.contains("instance_id") && ing["instance_id"] == "catalog:photo-A") {
        // Do something with the found ingredient...
    }
}
```

### Overriding ingredient properties

When adding an ingredient from an archive or from a file, the JSON passed to `add_ingredient()` can override properties like `title` and `relationship`. This is useful when reusing archived ingredients in a different context:

```cpp
// Override title, relationship, and set a custom instance_id for tracking
json ingredient_override = {
    {"title", "my-custom-title.jpg"},
    {"relationship", "parentOf"},
    {"instance_id", "my-tracking-id:asset-example-id"}
};
builder.add_ingredient(ingredient_override.dump(), signed_asset_path);
```

The `title`, `relationship`, and `instance_id` fields in the provided JSON take priority. The library fills in the rest (thumbnail, manifest_data, format) from the source. This works with signed assets, `.c2pa` archives, or unsigned files.

### Using custom vendor parameters in actions

The C2PA specification allows **vendor-namespaced parameters** on actions using reverse domain notation. These parameters survive signing and can be read back, useful for tagging actions with IDs that support filtering.

```cpp
auto manifest_json = R"(
{
    "claim_generator_info": [{ "name": "an-application", "version": "1.0" }],
    "assertions": [
        {
            "label": "c2pa.actions.v2",
            "data": {
                "actions": [
                    {
                        "action": "c2pa.created",
                        "digitalSourceType": "http://c2pa.org/digitalsourcetype/compositeCapture",
                        "parameters": {
                            "com.mycompany.tool": "my-editor",
                            "com.mycompany.session_id": "session-abc-123"
                        }
                    },
                    {
                        "action": "c2pa.placed",
                        "description": "Placed an image",
                        "parameters": {
                            "com.mycompany.layer_id": "layer-42",
                            "ingredientIds": ["c2pa.ingredient.v3"]
                        }
                    }
                ]
            }
        }
    ]
}
)";
```

After signing, these custom parameters appear alongside the standard fields:

```json
{
    "action": "c2pa.placed",
    "parameters": {
        "com.mycompany.layer_id": "layer-42",
        "ingredients": [{"url": "self#jumbf=c2pa.assertions/c2pa.ingredient.v3"}]
    }
}
```

Custom vendor parameters can be used to filter actions. For example, to find all actions related to a specific layer:

```cpp
for (auto& action : actions) {
    if (action.contains("parameters") &&
        action["parameters"].contains("com.mycompany.layer_id") &&
        action["parameters"]["com.mycompany.layer_id"] == "layer-42") {
        // This action is related to layer-42
    }
}
```

> **Naming convention:** Vendor parameters must use reverse domain notation with period-separated components (e.g., `com.mycompany.tool`, `net.example.session_id`). Some namespaces (e.g., `c2pa` or `cawg`) may be reserved.

### Extracting ingredients from a working store

A way to extract a specific ingredient from a working store is with the dedicated ingredient archive APIs: the producer writes one archive per ingredient with `write_ingredient_archive`, and the consumer loads only what it needs with `add_ingredient_from_archive`. The read-filter-rebuild APIs are the legacy approach.

#### Dedicated ingredient archive APIs

The producer registers each ingredient keyed by `instance_id`, writes one archive per ingredient, and the consumer loads only the needed one.

```cpp
auto settings = c2pa::Settings();
auto context = c2pa::Context::ContextBuilder()
    .with_settings(std::move(settings))
    .create_context();

// Producer: register two ingredients keyed by instance_id, archive each separately.
auto producer = c2pa::Builder(context, manifest_json);
producer.add_ingredient(
    R"({"title": "A.jpg", "relationship": "componentOf", "instance_id": "catalog:ingredient-A"})",
    "A.jpg");
producer.add_ingredient(
    R"({"title": "C.jpg", "relationship": "componentOf", "instance_id": "catalog:ingredient-C"})",
    "C.jpg");

std::stringstream archive_a(std::ios::in | std::ios::out | std::ios::binary);
std::stringstream archive_c(std::ios::in | std::ios::out | std::ios::binary);
producer.write_ingredient_archive("catalog:ingredient-A", archive_a);
producer.write_ingredient_archive("catalog:ingredient-C", archive_c);

// Consumer: load only ingredient-A, no JSON parsing, no add_resource loop.
auto sink = c2pa::Builder(context, manifest_json);
archive_a.seekg(0);
sink.add_ingredient_from_archive(archive_a);

sink.sign(source_path, output_path, signer);
```

The signed output contains exactly the loaded ingredient.

#### Migrating from the removed read_ingredient_file

The removed function `read_ingredient_file(source_path, data_dir)` read an asset, returned a formed ingredient JSON, and wrote the ingredient's binary resources (thumbnail and manifest data) to `data_dir`, so a later signing step could load that directory and embed the ingredient into a carrier. The behavior can be reimplemented with `Builder` and `Reader` objects: form the ingredient, archive it, read it back, write the resources to disk under stable non-colliding names, then reuse the directory to sign.

The legacy API derived each file name from the ingredient's `instance_id` rather than a fixed name (to avoid collisions). That is what lets several ingredients share one output directory without their thumbnail or `manifest_data` files overwriting each other.

```cpp
namespace fs = std::filesystem;

// Map a mime format ("image/jpeg") to a file extension,
// and turn an instance_id into a collision-resistant filename stem.
auto ext_for_format = [](const std::string& mime) {
    std::string ext = mime.substr(mime.find('/') + 1);
    return ext == "jpeg" ? std::string("jpg") : ext;
};
auto uuid_stem = [](const std::string& instance_id) {
    auto colon = instance_id.rfind(':');
    std::string stem = (colon != std::string::npos) ? instance_id.substr(colon + 1)
                                                     : instance_id;
    return stem.empty() ? std::string("ingredient") : stem;
};

// Extract: form the ingredient, archive just it, and write its JSON,
// thumbnail and nested manifest_data (if any) into output_dir.
fs::create_directories(output_dir);
c2pa::Builder builder(context, "{}");
builder.add_ingredient(
    R"({"label": "my-ingredient", "title": "source.jpg", "relationship": "componentOf"})",
    source_path);

std::stringstream archive_buf(std::ios::in | std::ios::out | std::ios::binary);
builder.write_ingredient_archive("my-ingredient", archive_buf);
archive_buf.seekg(0);

c2pa::Reader reader(context, "application/c2pa", archive_buf);
auto store = json::parse(reader.json());
std::string active = store["active_manifest"];
json ingredient = store["manifests"][active]["ingredients"][0];

std::string stem = uuid_stem(ingredient.value("instance_id", std::string()));
if (ingredient.contains("thumbnail")) {
    std::string name = stem + "." + ext_for_format(ingredient["thumbnail"]["format"]);
    reader.get_resource(ingredient["thumbnail"]["identifier"], output_dir / name);
    ingredient["thumbnail"]["identifier"] = name;  // rewrite to the on-disk name
}
if (ingredient.contains("manifest_data")) {
    std::string name = stem + ".c2pa";
    reader.get_resource(ingredient["manifest_data"]["identifier"], output_dir / name);
    ingredient["manifest_data"]["identifier"] = name;
}
ingredient.erase("label");  // drop the archive-only assertion label before reuse
std::ofstream(output_dir / (stem + ".json")) << ingredient.dump(2);

// Reuse: load the extracted ingredient (repeat the block above per ingredient to
// collect several) and sign a carrier, resolving resources from output_dir.
json manifest = {{"ingredients", json::array({ingredient})}};
c2pa::Builder sign_builder(context, manifest.dump());
sign_builder.set_base_path(output_dir.string());  // resolves resources
sign_builder.sign(carrier_path, output_path, signer);
```

To reproduce the full `read_ingredient_file` directory behavior for multiple ingredients, run the extract block once per source and append each resulting `ingredient` object to the `ingredients` array before signing. Deriving the file names from each ingredient's `instance_id` keeps them unique, so the extracted resources for every ingredient coexist in one directory without overwriting each other.

##### Without `set_base_path`

`set_base_path` is deprecated. The equivalent is to register each resource the ingredient JSON references directly on the signing builder with `add_resource`, instead of pointing the builder at a directory. Swap the sign block above for this:

```cpp
// Same extracted `ingredient` objects as above. Register every referenced resource
// (thumbnail, and manifest_data when present) explicitly, then sign.
c2pa::Builder sign_builder(context, manifest.dump());
for (const auto& ingredient : ingredients) {
    std::string thumb_id = ingredient["thumbnail"]["identifier"];
    sign_builder.add_resource(thumb_id, output_dir / thumb_id);
    if (ingredient.contains("manifest_data")) {
        std::string md_id = ingredient["manifest_data"]["identifier"];
        sign_builder.add_resource(md_id, output_dir / md_id);
    }
}
sign_builder.sign(carrier_path, output_path, signer);
```

Both paths produce the same signed result: the same ingredients, with the same resources attached. Register a resource for every `identifier` the ingredient JSON references, or the sign call is missing one; `set_base_path` did this implicitly by resolving names against the directory. `add_resource` also has a stream overload, `add_resource(identifier, stream)`, if the resource is already in memory rather than on disk (see [Dedicated ingredient archive APIs](#dedicated-ingredient-archive-apis) and the `add_resource` loop in the legacy pattern below).

#### Legacy: read-filter-rebuild APIs

> [!NOTE]
> **Legacy approach.** This pattern archives the full working store, then reads it back with `Reader`, filters ingredients in JSON, and transfers binary resources manually.

```mermaid
flowchart TD
    subgraph Step1["Step 1: Build a working store with ingredients"]
        IA["add_ingredient(A.jpg)"] --> B1[Builder]
        IB["add_ingredient(B.jpg)"] --> B1
        B1 -->|"to_archive()"| AR["archive.c2pa"]
    end
    subgraph Step2["Step 2: Extract ingredients from archive"]
        AR -->|"Reader(application/c2pa)"| RD[JSON + resources]
        RD -->|"pick ingredients"| SEL[Selected ingredients]
    end
    subgraph Step3["Step 3: Reuse in a new Builder"]
        SEL -->|"new Builder + add_resource()"| B2[New Builder]
        B2 -->|sign| OUT[Signed Output]
    end
```

**Step 1:** Build a working store and archive it:

```cpp
c2pa::Context context;
c2pa::Builder builder(context, manifest_json);

// Add ingredients to the working store
builder.add_ingredient(R"({"title": "A.jpg", "relationship": "componentOf"})",
                       path_to_A);
builder.add_ingredient(R"({"title": "B.jpg", "relationship": "componentOf"})",
                       path_to_B);

// Save the working store as an archive
std::stringstream archive_stream(std::ios::in | std::ios::out | std::ios::binary);
builder.to_archive(archive_stream);
```

**Step 2:** Read the archive and extract ingredients:

```cpp
// Read the archive (does not modify it)
archive_stream.seekg(0);
c2pa::Reader reader(context, "application/c2pa", archive_stream);
auto parsed = json::parse(reader.json());
std::string active = parsed["active_manifest"];
auto ingredients = parsed["manifests"][active]["ingredients"];
```

**Step 3:** Create a new Builder with the extracted ingredients:

```cpp
// Pick the desired ingredients
json selected = json::array();
for (auto& ingredient : ingredients) {
    if (ingredient["title"] == "A.jpg") {
        selected.push_back(ingredient);
    }
}

// Create a new Builder with only the selected ingredients
json new_manifest = json::parse(base_manifest_json);
new_manifest["ingredients"] = selected;
c2pa::Builder new_builder(context, new_manifest.dump());

// Transfer binary resources for the selected ingredients
for (auto& ingredient : selected) {
    if (ingredient.contains("thumbnail")) {
        std::string id = ingredient["thumbnail"]["identifier"];
        std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
        reader.get_resource(id, stream);
        stream.seekg(0);
        new_builder.add_resource(id, stream);
    }
    if (ingredient.contains("manifest_data")) {
        std::string id = ingredient["manifest_data"]["identifier"];
        std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
        reader.get_resource(id, stream);
        stream.seekg(0);
        new_builder.add_resource(id, stream);
    }
}

new_builder.sign(source_path, output_path, signer);
```

##### Migration guide: ingredient extraction

| Step | Legacy (manual) approach | Current dedicated ingredient archive APIs approach |
| --- | --- | --- |
| Archive | `builder.to_archive(stream)` (full builder) | `builder.write_ingredient_archive(id, stream)` (one ingredient) |
| Load | `Reader` + JSON parse + filter loop + `add_resource` per resource | `builder2.add_ingredient_from_archive(stream)` |
| Setting required | None | `builder.generate_c2pa_archive = "true"` on producer (current default) |

The dedicated ingredient archive APIs require no JSON parsing and no `add_resource` calls. Each archive holds exactly one ingredient.

Action linking differs between the two paths. With the legacy approach, the signing builder must re-assert `label` on its `add_ingredient` JSON to link to an action; `instance_id` is not accepted. With the dedicated ingredient archive APIs, the archive key passed to `write_ingredient_archive` (either `label` or `instance_id` from the producer ingredient) flows through and becomes the `ingredientIds` value. See [Lookup keys and action linking](working-stores.md#lookup-keys-and-action-linking).

### Reading ingredient details from an ingredient archive

An ingredient archive is a serialized `Builder` containing exactly one and only one ingredient (see [Builder archives vs. ingredient archives](#builder-archives-vs-ingredient-archives)). Reading it with `Reader` allows the caller to inspect the ingredient before deciding whether to use it: its thumbnail, whether it carries provenance (e.g. an active manifest), validation status, relationship, etc.

Reading is independent of linking, see [Linking an archived ingredient to an action](#linking-an-archived-ingredient-to-an-action) for how to attach the ingredient to an action without reading it first.

```mermaid
flowchart LR
    IA["ingredient_archive.c2pa"] -->|"Reader(application/c2pa)"| JSON["JSON + resources"]
    JSON --> TH["Thumbnail"]
    JSON --> AM["Active manifest?"]
    JSON --> VS["Validation status"]
    JSON --> REL["Relationship"]
```

```cpp
// Open the ingredient archive
std::ifstream archive_file("ingredient_archive.c2pa", std::ios::binary);
c2pa::Reader reader(context, "application/c2pa", archive_file);
auto parsed = json::parse(reader.json());
std::string active = parsed["active_manifest"];
auto manifest = parsed["manifests"][active];

// An ingredient archive must always contain exactly one ingredient
auto& ingredient = manifest["ingredients"][0];

// Relationship
std::string relationship = ingredient["relationship"];  // e.g. "parentOf", "componentOf", "inputTo"

// Instance ID (optional, set by the caller via add_ingredient or derived from XMP metadata)
std::string instance_id;
if (ingredient.contains("instance_id")) {
    instance_id = ingredient["instance_id"];
}

// Active manifest:
// When present, the ingredient was a signed asset and its manifest label
// points into the top-level "manifests" dictionary.
bool has_provenance = ingredient.contains("active_manifest");
if (has_provenance) {
    std::string ing_manifest_label = ingredient["active_manifest"];
    auto ing_manifest = parsed["manifests"][ing_manifest_label];
    // ing_manifest contains the ingredient's own assertions, actions, etc.
}

// Validation status.
// The top-level "validation_status" array covers the entire manifest store,
// including this ingredient's manifest. An empty or absent array means
// no validation errors were found.
if (parsed.contains("validation_status")) {
    for (auto& status : parsed["validation_status"]) {
        std::cout << status["code"].get<std::string>() << ": "
                  << status["explanation"].get<std::string>() << std::endl;
    }
}

// Thumbnail
if (ingredient.contains("thumbnail")) {
    std::string thumb_id = ingredient["thumbnail"]["identifier"];
    std::stringstream thumb_stream(std::ios::in | std::ios::out | std::ios::binary);
    reader.get_resource(thumb_id, thumb_stream);
    // thumb_stream now contains the thumbnail binary data
}
```

#### Ingredient vs. ingredient archive

A plain ingredient is a source asset (image, video, document) the builder reads at `add_ingredient` time, with `label` (primary) or `instance_id` (fallback) usable as linking keys. An ingredient archive is a `.c2pa` file containing one already-formed ingredient. When the archive is loaded via the legacy `add_ingredient(json, "application/c2pa", archive)` path, the only linking key the action can resolve is the `label` set on the *current* `add_ingredient` call. When loaded via `add_ingredient_from_archive`, the linking key is the archive key passed to `write_ingredient_archive` (either `label` or `instance_id` on the producer).

For a side-by-side comparison, see [Ingredient vs. ingredient archive](working-stores.md#ingredient-vs-ingredient-archive) in the working-stores doc.

#### Linking an archived ingredient to an action

When the archived ingredient is loaded via the legacy `add_ingredient(json, "application/c2pa", archive)` path, linking is **label-driven**: archived ingredients can only be linked to actions using labels. For the dedicated `write_ingredient_archive` + `add_ingredient_from_archive` ingredient archive APIs, the archive key (either `label` or `instance_id`) drives linking. See [Lookup keys and action linking](working-stores.md#lookup-keys-and-action-linking).

To do so, set a `label` on the archived ingredient's JSON passed to `add_ingredient` on the builder, and use that same string in the action's `ingredientIds`.

Reading the archive first is *not* required to link it. `Reader` is only useful when the caller wants to preview the ingredient (thumbnail, provenance, validation status) before deciding whether to use it (see [Reading ingredient details from an ingredient archive](#reading-ingredient-details-from-an-ingredient-archive)).

This section covers the **legacy** load path: producer calls `to_archive`, signing builder calls `add_ingredient(json, "application/c2pa", archive)`. For the dedicated `write_ingredient_archive` + `add_ingredient_from_archive` ingredient archive APIs, the archive key (either `label` or `instance_id`) flows through and becomes the `ingredientIds` value. See [Lookup keys and action linking](working-stores.md#lookup-keys-and-action-linking).

> [!WARNING]
> **For the legacy load path, `instance_id` does not work as a linking key for ingredient archives.** Use `label` instead.
>
> **Labels baked into the archive ingredient at archive-creation time do not carry through as linking keys either.** The label must be re-asserted on the signing builder's `add_ingredient` call so action and archived ingredient properly link.

Labels are build-time linking keys only. A label, as linking key, links ingredients and actions using it together: the label identifies the link. The SDK may reassign the actual label in the signed manifest.

##### Minimal archive to action linking example

Build a manifest whose action references a chosen label, then `add_ingredient` with that label on the signing builder. No `Reader`, no parsing of the archive:

```cpp
c2pa::Context context;

json manifest_json = {
    {"claim_generator_info", json::array({{{"name", "an-application"}, {"version", "1.0"}}})},
    {"assertions", json::array({
        {
            {"label", "c2pa.actions.v2"},
            {"data", {
                {"actions", json::array({
                    {
                        {"action", "c2pa.opened"},
                        {"parameters", {
                            {"ingredientIds", json::array({"archived-ingredient"})}
                        }}
                    }
                })}
            }}
        }
    })}
};

c2pa::Builder builder(context, manifest_json.dump());

// Same label string as in ingredientIds above: that is what links the action.
builder.add_ingredient(
    R"({
        "title": "photo.jpg",
        "relationship": "parentOf",
        "label": "archived-ingredient"
    })",
    archive_path);

// Note that a signing, the SDK may reassign the labels
builder.sign(source_path, output_path, signer);
```

The `add_ingredient` overload that takes a `std::istream` (with `"application/c2pa"` as the format) follows the same rules: the `label` on the ingredient JSON is what links the action:

```cpp
std::ifstream archive_file("ingredient_archive.c2pa", std::ios::binary);
c2pa::Builder builder(context, manifest_json.dump());

builder.add_ingredient(
    R"({
        "title": "photo.jpg",
        "relationship": "parentOf",
        "label": "archived-ingredient"
    })",
    "application/c2pa",
    archive_file);

builder.sign(source_path, output_path, signer);
```

##### Previewing the archive before linking

If you want to inspect the ingredient archive (e.g. to decide whether to use it, or to copy a `title` from it), open it with `Reader` first, then add it as an ingredient. The Reader step is independent of the linking: the link is still established by the `label` on the signing builder's `add_ingredient` call.

```cpp
std::ifstream archive_file("ingredient_archive.c2pa", std::ios::binary);
c2pa::Reader reader(context, "application/c2pa", archive_file);
auto parsed = json::parse(reader.json());
std::string active = parsed["active_manifest"];
auto& ingredient = parsed["manifests"][active]["ingredients"][0];

// Inspect ingredient (thumbnail, validation_status, active_manifest, etc.)
// before deciding to use it. See "Reading ingredient details from an ingredient archive".

c2pa::Builder builder(context, manifest_json.dump());

archive_file.seekg(0);
builder.add_ingredient(
    json({
        {"title", ingredient["title"]},
        {"relationship", "parentOf"},
        {"label", "archived-ingredient"}  // The linking key: chosen here, not read from the archive.
    }).dump(),
    "application/c2pa",
    archive_file);

builder.sign(source_path, output_path, signer);
```

##### Linking with the dedicated archive API

The same linking flow works when the ingredient is loaded with `add_ingredient_from_archive`. The id used at write time on the producer (passed as the first argument to `write_ingredient_archive`) becomes the linking key on the signing builder. Reference that same id in `ingredientIds`.

Producer side:

```cpp
auto settings = c2pa::Settings();
auto context = c2pa::Context::ContextBuilder()
    .with_settings(std::move(settings))
    .create_context();

auto archive_builder = c2pa::Builder(context, manifest_json);
archive_builder.add_ingredient(
    R"({"title": "photo.jpg", "relationship": "parentOf", "label": "my-ingredient"})",
    "photo.jpg");

std::stringstream archive(std::ios::in | std::ios::out | std::ios::binary);
archive_builder.write_ingredient_archive("my-ingredient", archive);
```

Signing side, link `my-ingredient` to `c2pa.opened`:

```cpp
auto signing_builder = c2pa::Builder(context, R"({
    "claim_generator_info": [{"name": "an-application", "version": "1.0"}],
    "assertions": [{
        "label": "c2pa.actions.v2",
        "data": {
            "actions": [{
                "action": "c2pa.opened",
                "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCreation",
                "parameters": { "ingredientIds": ["my-ingredient"] }
            }]
        }
    }]
})");

archive.seekg(0);
signing_builder.add_ingredient_from_archive(archive);

signing_builder.sign(source_path, output_path, signer);
```

The same id can appear in `ingredientIds` of more than one action. A `c2pa.opened` and a `c2pa.placed` action that both list `my-ingredient` resolve to the same ingredient URL after signing.

For `c2pa.placed`, the relationship on the producing builder is `componentOf` instead of `parentOf`. Otherwise the linking pattern is identical.

A signing builder can mix the dedicated API with the existing `add_ingredient(json, source)` overloads in the same build. Linking by id works the same regardless of how each ingredient reached the builder. For example, an action that lists `via-add`, `via-stream`, `via-archive` in `ingredientIds` resolves to three distinct ingredient URLs when one ingredient is added by file, one by stream, and one by ingredient archive.

#### Troubleshooting linking errors

A common signing-time error when linking ingredients is:

```text
Builder.sign failure: Other: assertion-specific error:
Action ingredientId not found: <some id>
```

Causes and potential fixes to investigate:

| Symptom | Cause | Fix |
| --- | --- | --- |
| `Action ingredientId not found: xmp:iid:...` (or any `instance_id` value) | `instance_id` was used as the linking key for an ingredient archive. | Assign a `label` on the signing builder's `add_ingredient` JSON, and use that label in `ingredientIds`. |
| `Action ingredientId not found: <label>` where the label was set only when building the archive | Labels baked into an archive ingredient do not carry through as linking keys. | Re-assert the same `label` in the signing builder's `add_ingredient` JSON when calling `add_ingredient`. |
| `Action ingredientId not found: <label>` where the label is on `add_ingredient` but the action references a different string | Typo or mismatch between `ingredientIds[i]` and the `label` field on the ingredient. | Make the two strings identical, as the string is a linking key. |
| Sign succeeds but the action's `parameters.ingredients` array is empty in the signed output | The action was kept during a filter/rebuild but the corresponding ingredient was not, or was not linked. | See [Filtering actions that reference ingredients](#filtering-actions-that-reference-ingredients) to keep the ingredient and its binary resources alongside the action. Verify that the linking of ingredients and actions uses the correct JSON attributes. |

For the linking rules, see [Linking an archived ingredient to an action](#linking-an-archived-ingredient-to-an-action) above.

### Merging multiple working stores

In some cases you may need to merge ingredients from multiple working stores (builder archives) into a single `Builder`. This should be a **fallback strategy**—the recommended practice is to maintain a single active working store and add ingredients incrementally (archived ingredient catalogs help with this). Merging is available when multiple working stores must be consolidated.

When merging from multiple sources, resource identifier URIs can collide. Rename identifiers with a unique suffix when needed. Use two passes: (1) collect ingredients with collision handling, build the manifest, create the builder; (2) re-read each archive and transfer resources (use original ID for `get_resource()`, renamed ID for `add_resource()` when collisions occurred).

When each source contributes one ingredient, the dedicated single-ingredient API sidesteps this resource-identifier collision case: each archive holds one and only one ingredient, and `add_ingredient_from_archive` registers it cleanly on the consuming builder. See [Single-ingredient archive APIs](./working-stores.md#single-ingredient-archive-apis) in the working stores guide. The two-pass approach below remains the right tool when sources hold multiple ingredients each and a full merge is required.

```cpp
std::set<std::string> used_ids;
int suffix_counter = 0;
json all_ingredients = json::array();
std::vector<std::pair<std::istream*, size_t>> archive_info;  // (stream, ingredient count)

// Pass 1: Collect ingredients, renaming IDs on collision
for (auto& archive_stream : archives) {
    archive_stream.seekg(0);
    c2pa::Reader reader(archive_ctx, "application/c2pa", archive_stream);
    auto parsed = json::parse(reader.json());
    auto ingredients = parsed["manifests"][parsed["active_manifest"]]["ingredients"];

    for (auto& ingredient : ingredients) {
        for (const char* key : {"thumbnail", "manifest_data"}) {
            if (!ingredient.contains(key)) continue;
            std::string id = ingredient[key]["identifier"];
            if (used_ids.count(id)) {
                ingredient[key]["identifier"] = id + "__" + std::to_string(++suffix_counter);
            }
            used_ids.insert(ingredient[key]["identifier"].get<std::string>());
        }
        all_ingredients.push_back(ingredient);
    }
    archive_info.emplace_back(&archive_stream, ingredients.size());
}

json manifest = json::parse(R"({
    "claim_generator_info": [{"name": "an-application", "version": "0.1.0"}]
})");
manifest["ingredients"] = all_ingredients;
c2pa::Builder builder(context, manifest.dump());

// Pass 2: Transfer resources (match by ingredient index)
size_t idx = 0;
for (auto& [stream, count] : archive_info) {
    stream->seekg(0);
    c2pa::Reader reader(archive_ctx, "application/c2pa", *stream);
    auto parsed = json::parse(reader.json());
    auto orig = parsed["manifests"][parsed["active_manifest"]]["ingredients"];

    for (size_t i = 0; i < count; ++i) {
        auto& o = orig[i];
        auto& m = all_ingredients[idx++];
        for (const char* key : {"thumbnail", "manifest_data"}) {
            if (!o.contains(key)) continue;
            std::stringstream s(std::ios::in | std::ios::out | std::ios::binary);
            reader.get_resource(o[key]["identifier"].get<std::string>(), s);
            s.seekg(0);
            builder.add_resource(m[key]["identifier"].get<std::string>(), s);
        }
    }
}

builder.sign(source_path, output_path, signer);
```

## Retrieving actions from a working store

Actions are stored in the `c2pa.actions.v2` assertion. Use `Reader` to extract them from a signed asset or an archived Builder.

### Reading actions

```cpp
c2pa::Context context;
c2pa::Reader reader(context, "image/jpeg", source_stream);
auto parsed = json::parse(reader.json());
std::string active = parsed["active_manifest"];
auto assertions = parsed["manifests"][active]["assertions"];

// Find the actions assertion
for (auto& assertion : assertions) {
    if (assertion["label"] == "c2pa.actions.v2") {
        auto actions = assertion["data"]["actions"];
        for (auto& action : actions) {
            std::cout << "Action: " << action["action"] << std::endl;
            if (action.contains("description")) {
                std::cout << "  Description: " << action["description"] << std::endl;
            }
        }
    }
}
```

### Reading actions from an archive

Use the same approach with format `"application/c2pa"` and an archive stream:

```cpp
std::ifstream archive_file("builder_archive.c2pa", std::ios::binary);
c2pa::Reader reader(context, "application/c2pa", archive_file);
// Then parse and iterate assertions as in the example above
```

### Understanding the manifest tree

The `Reader` returns a manifest store—a dictionary of manifests keyed by label (a URN like `contentauth:urn:uuid:...`). Conceptually it forms a tree: each manifest has assertions and ingredients; ingredients with `manifest_data` carry their own manifest store, which can have its own ingredients and assertions recursively. The `active_manifest` key indicates the root.

```mermaid
flowchart TD
    subgraph Store["Manifest Store"]
        M1["Active Manifest\n- assertions (including c2pa.actions.v2)\n- ingredients"]
        M2["Ingredient A's manifest\n- its own c2pa.actions.v2\n- its own ingredients"]
        M3["Ingredient B's manifest\n- its own c2pa.actions.v2"]
    end
    M1 -->|"ingredient A has manifest_data"| M2
    M1 -->|"ingredient B has manifest_data"| M3
    M1 -.-|"ingredient C has no manifest_data"| M5["Ingredient C\n(unsigned asset, no provenance)"]
    M2 -->|"may have its own ingredients..."| M4["...deeper in the tree"]

    style M5 fill:#eee,stroke:#999,stroke-dasharray: 5 5
```



Not every ingredient has provenance. An unsigned asset added as an ingredient has `title`, `format`, and `relationship`, but no `manifest_data` and no entry in the `"manifests"` dictionary. Walking the tree reveals the full provenance chain: what each actor did at each step, including actions performed and ingredients used.

**To walk the tree and find actions at each level:**

```cpp
auto parsed = json::parse(reader.json());
std::string active = parsed["active_manifest"];
auto active_manifest = parsed["manifests"][active];

// Read the active manifest's actions
for (auto& assertion : active_manifest["assertions"]) {
    if (assertion["label"] == "c2pa.actions.v2") {
        std::cout << "Active manifest actions:" << std::endl;
        for (auto& action : assertion["data"]["actions"]) {
            std::cout << "  " << action["action"].get<std::string>() << std::endl;
        }
    }
}

// Walk into each ingredient's manifest
for (auto& ingredient : active_manifest["ingredients"]) {
    std::cout << "Ingredient: " << ingredient["title"].get<std::string>() << std::endl;

    // If this ingredient has its own manifest (it was a signed asset),
    // its manifest label is in "active_manifest"
    if (ingredient.contains("active_manifest")) {
        std::string ing_manifest_label = ingredient["active_manifest"];
        if (parsed["manifests"].contains(ing_manifest_label)) {
            auto ing_manifest = parsed["manifests"][ing_manifest_label];

            // This ingredient's manifest has its own actions
            for (auto& assertion : ing_manifest["assertions"]) {
                if (assertion["label"] == "c2pa.actions.v2") {
                    std::cout << "  Ingredient's actions:" << std::endl;
                    for (auto& action : assertion["data"]["actions"]) {
                        std::cout << "    " << action["action"].get<std::string>() << std::endl;
                    }
                }
            }

            // And its own ingredients (deeper in the tree)...
        }
    } else {
        // This ingredient has no manifest of its own (it was an unsigned asset).
        // It still has a title, format, and relationship, but no manifest_data,
        // no actions, and no deeper provenance chain.
        std::cout << "  (no content credentials)" << std::endl;
    }
}
```

## Filtering actions 

To remove actions, use the same read–filter–rebuild pattern: **read, pick the ones to keep, create a new Builder**.

```mermaid
flowchart TD
    SA["Signed Asset with 3 actions: opened, placed, filtered"] -->|Reader| JSON[Parse JSON]
    JSON -->|"Keep only opened + placed"| FILT[Filtered actions]
    FILT -->|"New Builder with 2 actions"| NB[New Builder]
    NB -->|sign| OUT["New with 2 actions only: opened, placed"]
```



### Basic action filtering

When filtering, remember that the first action must remain `c2pa.created` or `c2pa.opened` for the manifest to be valid. If the first action is removed, a new one must be added.

```cpp
c2pa::Context context;
c2pa::Reader reader(context, "image/jpeg", source_stream);
auto parsed = json::parse(reader.json());
std::string active = parsed["active_manifest"];
auto manifest = parsed["manifests"][active];

// Filter actions: keep c2pa.created/c2pa.opened (mandatory) and c2pa.placed, drop the rest
json kept_actions = json::array();
for (auto& assertion : manifest["assertions"]) {
    if (assertion["label"] == "c2pa.actions.v2") {
        for (auto& action : assertion["data"]["actions"]) {
            std::string action_type = action["action"];
            if (action_type == "c2pa.created" || action_type == "c2pa.opened" ||
                action_type == "c2pa.placed") {
                kept_actions.push_back(action);
            }
            // Skip c2pa.filtered, c2pa.color_adjustments, etc.
        }
    }
}

// Build a new manifest with only the kept actions
json new_manifest = json::parse(R"({
    "claim_generator_info": [{"name": "an-application", "version": "1.0"}]
})");

if (!kept_actions.empty()) {
    new_manifest["assertions"] = json::array({
        {
            {"label", "c2pa.actions"},
            {"data", {{"actions", kept_actions}}}
        }
    });
}

c2pa::Builder builder(context, new_manifest.dump());
builder.sign(source_path, output_path, signer);
```

### Filtering actions that reference ingredients

Some actions reference ingredients (via `parameters.ingredients[].url` after signing). If keeping an action that references an ingredient, **the corresponding ingredient and its binary resources must also be kept**. If an ingredient is dropped, any actions that reference it must also be dropped (or updated).

#### `c2pa.opened` action

The `c2pa.opened` action is special because it must be the first action and it references the asset that was opened (the `parentOf` ingredient). When filtering:

- **Always keep `c2pa.opened` or `c2pa.created`**: it is required for a valid manifest
- **Keep the ingredient it references**: the `parentOf` ingredient linked via its `parameters.ingredients[].url`
- Removing the ingredient that `c2pa.opened` points to will make the manifest invalid

#### `c2pa.placed` action

The `c2pa.placed` action references a `componentOf` ingredient that was composited into the asset. When filtering:

- If keeping `c2pa.placed`, keep the ingredient it references
- If the ingredient is dropped, also drop the `c2pa.placed` action
- If `c2pa.placed` is not required: it can safely be removed (and the ingredient it references, if it is the only reference)

#### Example

The code below provides an example of filtering with linked ingredients.

```cpp
c2pa::Context context;
c2pa::Reader reader(context, "image/jpeg", source_stream);
auto parsed = json::parse(reader.json());
std::string active = parsed["active_manifest"];
auto manifest = parsed["manifests"][active];

// Filter actions and track which ingredients are needed
json kept_actions = json::array();
std::set<std::string> needed_ingredient_labels;

for (auto& assertion : manifest["assertions"]) {
    if (assertion["label"] == "c2pa.actions.v2") {
        for (auto& action : assertion["data"]["actions"]) {
            std::string action_type = action["action"];

            // Always keep c2pa.opened/c2pa.created (required for valid manifest)
            // Keep c2pa.placed (optional -- kept here as an example)
            // Drop everything else
            bool keep = (action_type == "c2pa.opened" ||
                         action_type == "c2pa.created" ||
                         action_type == "c2pa.placed");

            if (keep) {
                kept_actions.push_back(action);

                // Track which ingredients this action needs
                if (action.contains("parameters") &&
                    action["parameters"].contains("ingredients")) {
                    for (auto& ing_ref : action["parameters"]["ingredients"]) {
                        std::string url = ing_ref["url"];
                        std::string label = url.substr(url.rfind('/') + 1);
                        needed_ingredient_labels.insert(label);
                    }
                }
            }
        }
    }
}

// Keep only the ingredients that are referenced by kept actions
json kept_ingredients = json::array();
for (auto& ingredient : manifest["ingredients"]) {
    if (ingredient.contains("label") &&
        needed_ingredient_labels.count(ingredient["label"])) {
        kept_ingredients.push_back(ingredient);
    }
}

// Build the new manifest with filtered actions and matching ingredients
json new_manifest = json::parse(R"({
    "claim_generator_info": [{"name": "an-application", "version": "1.0"}]
})");
new_manifest["ingredients"] = kept_ingredients;
if (!kept_actions.empty()) {
    new_manifest["assertions"] = json::array({
        {
            {"label", "c2pa.actions"},
            {"data", {{"actions", kept_actions}}}
        }
    });
}

c2pa::Builder builder(context, new_manifest.dump());

// Transfer binary resources for kept ingredients
for (auto& ingredient : kept_ingredients) {
    if (ingredient.contains("thumbnail")) {
        std::string id = ingredient["thumbnail"]["identifier"];
        std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
        reader.get_resource(id, stream);
        stream.seekg(0);
        builder.add_resource(id, stream);
    }
    if (ingredient.contains("manifest_data")) {
        std::string id = ingredient["manifest_data"]["identifier"];
        std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
        reader.get_resource(id, stream);
        stream.seekg(0);
        builder.add_resource(id, stream);
    }
}

builder.sign(source_path, output_path, signer);
```

> [!NOTE]
> When copying ingredient JSON objects from a reader, they keep their `label` field. Since the action URLs reference ingredients by label, the links resolve correctly as long as ingredients are not renamed or reindexed. If ingredients are re-added via `add_ingredient()` (which generates new labels), the action URLs will also need to be updated.

## Controlling manifest embedding

By default, `sign()` embeds the manifest directly inside the output asset file.

### Remove the manifest from the asset entirely

Use `set_no_embed()` so the signed asset contains no embedded manifest. The manifest bytes are returned from `sign()` and can be stored separately (as a sidecar file, on a server, etc.):

```mermaid
flowchart LR
    subgraph Default["Default (embedded)"]
        A1[Output Asset] --- A2[Image data + C2PA manifest]
    end

    subgraph NoEmbed["With set_no_embed()"]
        B1[Output Asset] ~~~ B2[Manifest bytes with store as sidecar or uploaded to server]
    end
```



```cpp
c2pa::Builder builder(context, manifest_json);
builder.set_no_embed();
builder.set_remote_url("<<URI/URL to remote storage of manifest bytes>>");

auto manifest_bytes = builder.sign("image/jpeg", source, dest, signer);
// manifest_bytes contains the full manifest store
// Upload manifest_bytes to the remote URL
// The output asset has no embedded manifest
```

### Checking manifest location on a Reader

After opening an asset with `Reader`, use `is_embedded()` to check whether the manifest is embedded in the asset or stored remotely. If the manifest is remote, `remote_url()` returns the URL it was fetched from (the URL set via `set_remote_url()` at signing time).

```cpp
c2pa::Reader reader(context, "image/jpeg", dest);

if (reader.is_embedded()) {
    std::cout << "Manifest is embedded in the asset." << std::endl;
} else {
    std::cout << "Manifest is not embedded." << std::endl;
    auto url = reader.remote_url();
    if (url.has_value()) {
        std::cout << "Remote manifest URL: " << url.value() << std::endl;
    }
}
```

## Complete workflow diagram

```mermaid
flowchart TD
    subgraph Step1["Step 1: READ"]
        SA[Signed Asset] -->|Reader| RD["reader.json() -- full manifest JSON\nreader.get_resource(id, stream) -- binary"]
    end

    subgraph Step2["Step 2: FILTER"]
        RD --> FI[Parse JSON]
        FI --> F1[Pick ingredients to keep]
        FI --> F3[Pick actions to keep]
        FI --> F4[Ensure kept actions' ingredients are also kept]
        FI --> F5["Ensure c2pa.created/opened is still the first action"]
        F1 & F3 & F4 & F5 --> FM[Build new manifest JSON with only filtered items]
    end

    subgraph Step3["Step 3: BUILD new Builder"]
        FM --> BLD["new Builder with context and filtered_json"]
        BLD --> AR[".add_resource for each kept binary resource"]
        AR --> AI[".add_ingredient to add original as parent (optional)"]
        AI --> AA[".add_action to record new actions (optional)"]
    end

    subgraph Step4["Step 4: SIGN"]
        AA --> SIGN["builder.sign(source, output, signer)"]
        SIGN --> OUT[Output asset with new manifest containing only filtered content]
    end
```
