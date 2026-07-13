# Frequently-asked questions (FAQs)

## When do I use `Reader` versus `Builder`?

**Use a `Reader` when the goal is only to inspect or extract data without creating a new manifest.**

- Validating whether an asset has C2PA credentials
- Displaying provenance information to a user
- Extracting thumbnails for display
- Checking trust status and validation results
- Inspecting ingredient chains

```cpp
c2pa::Reader reader(context, "image.jpg");
auto json = reader.json();               // inspect the manifest
reader.get_resource(thumb_id, stream);   // extract a thumbnail
```

The `Reader` is read-only. It never modifies the source asset.

**Use a `Builder` when creating a manifest from scratch on an asset that has no existing C2PA data, or when intentionally starting with a clean slate.**

- Signing a brand-new asset for the first time
- Adding C2PA credentials to an unsigned asset
- Creating a manifest with all content defined from scratch

```cpp
c2pa::Builder builder(context, manifest_json);
builder.add_ingredient(ingredient_json, source_path);  // add source material
builder.sign(source_path, output_path, signer);
```

Every call to the `Builder` constructor or `Builder::from_archive()` creates a new `Builder`. There is no way to modify an existing signed manifest directly.

### When to use both `Reader` and `Builder` together

**Use both when filtering content from an existing manifest into a new one. The `Reader` extracts data, application code filters it, and a new `Builder` receives only the selected parts.**

- Filtering specific ingredients from a manifest
- Dropping specific assertions while keeping others
- Filtering actions (keeping some, removing others)
- Merging ingredients from multiple signed assets or archives
- Extracting content from an ingredients catalog
- Re-signing with different settings while keeping some original content

```cpp
// Read existing (does not modify the asset)
c2pa::Reader reader(context, "signed.jpg");
auto parsed = json::parse(reader.json());

// Filter what to keep
auto kept = filter(parsed);  // application-specific filtering logic

// Create a new Builder with only the filtered content
c2pa::Builder builder(context, kept.dump());
// ... transfer resources ...
builder.sign(source, output, signer);
```

### Quick reference decision tree

```mermaid
flowchart TD
    Q1{Need to read an existing manifest?}
    Q1 -->|No| USE_B["Use Builder alone (new manifest from scratch)"]
    Q1 -->|Yes| Q2{Need to create a new/modified manifest?}
    Q2 -->|No| USE_R["Use Reader alone (inspect/extract only)"]
    Q2 -->|Yes| USE_BR[Use both Reader + Builder]
    USE_BR --> Q3{What to keep from the existing manifest?}
    Q3 -->|Everything| P1["add_ingredient() with original asset or archive path"]
    Q3 -->|Some parts| P2["1. Read: reader.json() + get_resource() 2. Filter: pick ingredients & actions to keep 3. Build: new Builder with filtered JSON 4. Transfer: .add_resource for kept binaries 5. Sign: builder.sign()"]
    Q3 -->|Nothing| P3["New Builder alone (fresh manifest, no prior provenance)"]
```

## How should I add ingredients?

There are two ways: using `add_ingredient()` and injecting ingredient JSON via `with_definition()`.  The table below summarizes these options.

| Approach | What it does | When to use |
| --- | --- | --- |
| `add_ingredient(json, path)` | Reads the source (a signed asset, an unsigned file, or a `.c2pa` archive), extracts its manifest store automatically, generates a thumbnail | Adding an ingredient where the library should handle extraction. Works with ingredient catalog archives too: pass the archive path and the library extracts the manifest data |
| Inject via `with_definition()` + `add_resource()` | Accepts the ingredient JSON and all binary resources provided manually | Reconstructing from a reader or merging from multiple readers, where the data has already been extracted |

## When to use archives

There are two distinct archive concepts:

- **Builder archives (working store archives)** (`to_archive` / `from_archive` / `with_archive`) serialize the full `Builder` state (manifest definition, resources, ingredients) so it can be resumed or signed later, possibly on a different machine or in a different process. The archive is not yet signed. Use builder archives when:
    - Signing must happen on a different machine (e.g., an HSM server)
    - Checkpointing work-in-progress before signing
    - Transmitting a `Builder` state across a network boundary

- **Ingredient archives** contain the manifest store data (`.c2pa` binary) from ingredients that were added to a `Builder`. When a signed asset is added as an ingredient via `add_ingredient()`, the library extracts and stores its manifest store as `manifest_data` within the ingredient record. When the `Builder` is then serialized via `to_archive()`, these ingredient manifest stores are included. Use ingredient archives when:
    - Building an ingredients catalog for pick-and-choose workflows
    - Preserving provenance history from source assets
    - Transferring ingredient data between `Reader` and `Builder`

See also [Working stores](working-stores.md).

Key consideration for builder archives: `from_archive()` creates a new `Builder` with **default** context settings. If specific settings are needed (e.g., thumbnails disabled), use `with_archive()` on a `Builder` that already has the desired context:

```cpp
// Preserves the caller's context settings
c2pa::Builder builder(my_context);
builder.with_archive(archive_stream);
builder.sign(source, output, signer);
```

## Can a manifest be modified in place?

**No.** C2PA manifests are cryptographically signed. Any modification invalidates the signature. The only way to "modify" a manifest is to create a new `Builder` with the desired changes and sign it. This is by design: it ensures the integrity of the provenance chain.

## What happens to the provenance chain when rebuilding a working store?

When creating a new manifest, the chain is preserved once the original asset is added as an ingredient. The ingredient carries the original asset's manifest data, so validators can trace the full history. If the original is not added as an ingredient, the provenance chain is broken: the new manifest has no link to the original. This might be intentional (starting fresh) or a mistake (losing provenance).

## What replaced `read_file`, `read_ingredient_file`, and `sign_file`?

These file-based functions not attached to an API-exposed object, `read_file`, `read_ingredient_file`, and `sign_file`, were deprecated, then removed. Each legacy function maps to another implementation path using existing APIs on the `Reader` or `Builder` objects:

| Removed function | Equivalent |
| --- | --- |
| `read_file(path)` | `Reader::from_asset(ctx, format, stream)` then `Reader::json()`. Pull binary resources with `Reader::get_resource(uri, dest)`. |
| `read_ingredient_file(path, data_dir)` | `Builder::add_ingredient(ingredient_json, source_path)`, which adds ingredients to the active Builder. To recover the formed ingredient JSON (if needed) and its resources, archive the working store and read it back, or move the ingredient with the dedicated `write_ingredient_archive` / `add_ingredient_from_archive` APIs. There is no single-call replacement; but the full behavior (extract the ingredient JSON, thumbnail, and manifest_data to a directory, then reuse that directory to sign an asset using a Builder) can be reimplemented as shown in examples from [Migrating from `read_ingredient_file`](selective-manifests.md#migrating-from-read_ingredient_file). See also [Extracting ingredients from a working store](selective-manifests.md#extracting-ingredients-from-a-working-store). |
| `sign_file(src, dst, manifest, SignerInfo*, data_dir)` | `Builder::sign(source_path, dest_path, signer)` with a `Signer`. |
