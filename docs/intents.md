# Using Builder intents

_Intents_ enable validation, add actions required by the C2PA specification, and help prevent invalid operations when using a `Builder`. Intents are about the operation (create, edit, or update) executed on the source asset.

## Why use intents?

Without intents, you have to manually construct the correct manifest structure: adding the required actions (`c2pa.created` or `c2pa.opened` as the first action per the specification), setting digital source types, managing ingredients, and linking actions to ingredients. Getting any of this wrong produces a non-compliant manifest.

With intents, the caller declares *what to do* and `Builder` handles the rest.

For example, without intents you have to manually wire up actions and make sure ingredients are properly linked to actions. This is especially important for `parentOf` ingredient relationships with the `c2pa.opened` action.

```cpp
#include "c2pa.hpp"

c2pa::Context context;
c2pa::Builder builder(context, R"({
    "assertions": [
        {
            "label": "c2pa.actions",
            "data": {
                "actions": [
                    {
                        "action": "c2pa.created",
                        "digitalSourceType": "http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia"
                    }
                ]
            }
        }
    ]
})");
builder.sign(source_path, output_path, signer);
```

But with intents, `Builder` generates the actions automatically; for example:

```cpp
#include "c2pa.hpp"

c2pa::Context context;
c2pa::Builder builder(context, R"({})");
builder.set_intent(Create, TrainedAlgorithmicMedia);
builder.sign(source_path, output_path, signer);
```

Both of these code snippets produce the same signed manifest. But with intents, `Builder` validates the setup and fills in the required structure.

## Setting the intent

You can set the intent on a `Builder` instance by:

- [Using Context](#using-context)
- [Using `set_intent` on the `Builder`](#using-set_intent-on-builder)

Don't set intent using the deprecated `load_settings()` function. For existing code, see [Context and Settings - Migrating from deprecated APIs](context-settings.md#migrating-from-deprecated-apis).

### Using Context

Pass the intent through a `Context` object when creating a `Builder`. This keeps intent configuration alongside other builder settings such as `claim_generator_info` and `thumbnail`.

```cpp
c2pa::Context context(R"({
    "version": 1,
    "builder": {
        "intent": {"Create": "digitalCapture"},
        "claim_generator_info": {"name": "My App", "version": "0.1.0"}
    }
})");

c2pa::Builder builder(context, R"({})");
builder.sign(source_path, output_path, signer);
```

You can reuse the same `Context` across multiple `Builder` instances, ensuring consistent configuration:

```cpp
c2pa::Context context(R"({
    "version": 1,
    "builder": {
        "intent": "edit",
        "claim_generator_info": {"name": "Batch Editor"}
    }
})");

for (const auto& path : image_paths) {
    c2pa::Builder builder(context, R"({})");
    builder.sign(path, output_path(path), signer);
}
```

### Using set_intent on Builder

Call `set_intent` directly on a `Builder` instance for one-off operations or when you determine the intent at runtime. For example:

```cpp
c2pa::Context context;
c2pa::Builder builder(context, R"({})");
builder.set_intent(Create, TrainedAlgorithmicMedia);
builder.sign(source_path, output_path, signer);
```

### Intent precedence

When you configure an intent in multiple places, the most specific setting takes precedence.
Calling `set_intent` on a `Builder` instance takes precedence over all other sources.

```mermaid
flowchart TD
    Check{Was set_intent called
    on the Builder?}
    Check --> |Yes| UseSetIntent["Use set_intent value"]
    Check --> |No| CheckCtx{Was a Context with
    builder.intent provided?}
    CheckCtx --> |Yes| UseCtx["Use Context intent"]
    CheckCtx --> |No| CheckGlobal{Was load_settings called
    with builder.intent?}
    CheckGlobal --> |Yes| UseGlobal["Use global intent
    (deprecated)"]
    CheckGlobal --> |No| NoIntent["No intent set.
    Caller must define actions
    manually in manifest JSON."]
```

## How intents relate to the source stream

The intent operates on the source passed to `sign()`, not on any ingredient added via `add_ingredient`.

The following diagram shows what happens at sign time for each intent:

```mermaid
flowchart LR
    subgraph CREATE
        S1[source stream] --> B1[Builder]
        B1 --> O1[signed output]
        B1 -. adds .-> A1["c2pa.created action
        + digital source type"]
    end
```

```mermaid
flowchart LR
    subgraph EDIT
        S2[source stream] --> B2[Builder]
        B2 --> O2[signed output]
        S2 -. auto-created as .-> P2[parentOf ingredient]
        P2 --> B2
        B2 -. adds .-> A2["c2pa.opened action
        linked to parent"]
    end
```

```mermaid
flowchart LR
    subgraph UPDATE
        S3[source stream] --> B3[Builder]
        B3 --> O3[signed output]
        S3 -. auto-created as .-> P3[parentOf ingredient]
        P3 --> B3
        B3 -. adds .-> A3["c2pa.opened action
        linked to parent"]
        B3 -. restricts .-> R3[content must not change]
    end
```

For `Edit` and `Update` intents, `Builder` looks at the source stream. If the caller has not manually added a `parentOf` ingredient, `Builder` automatically creates one from that stream (and adds the needed action). The source stream *becomes* the parent ingredient. If the caller has already manually added a `parentOf` ingredient (via `add_ingredient`), `Builder` uses that one instead and does not automatically create one from the source.

### How intent relates to add_ingredient

The `Builder` intent controls what `Builder` does with the source stream (source asset) at sign time. The `add_ingredient` method adds other ingredients explicitly. These are separate concerns.

```mermaid
flowchart TD
    Intent["Intent
    (via Context, set_intent,
    or load_settings)"] --> Q{Intent type?}
    Q --> |CREATE| CreateFlow["No parent allowed
    Source stream is new content"]
    Q --> |EDIT or UPDATE| EditFlow{Was a parentOf ingredient
    added via add_ingredient?}
    EditFlow --> |No| Auto["Builder auto-creates
    parentOf from source stream"]
    EditFlow --> |Yes| Manual["Builder uses the
    manually-added parent"]
    Auto --> Opened["Builder adds c2pa.opened
    action linked to parent"]
    Manual --> Opened
    CreateFlow --> Created["Builder adds c2pa.created
    action + digital source type"]

    AddIngredient["add_ingredient()"] --> IngType{relationship?}
    IngType --> |parentOf| ParentIng["Overrides auto-parent
    for EDIT/UPDATE"]
    IngType --> |componentOf| CompIng["Additional ingredient
    not affected by intent"]
    ParentIng --> EditFlow
```

## Including the header

The `C2paBuilderIntent` and `C2paDigitalSourceType` enums are available through the `c2pa.hpp` header file:

```cpp
#include "c2pa.hpp"
```

Enum values are in the global namespace:
- `C2paBuilderIntent`: Create, Edit, or Update
- `C2paDigitalSourceType`: Empty, DigitalCapture, TrainedAlgorithmicMedia, and so on.

### Using set_intent

Use the `Builder` [`set_intent`](https://contentauth.github.io/c2pa-cpp/da/db7/classc2pa_1_1Builder.html#ac3ca980a43f44c9349ac0d6de50a088c) method to specify the intent:

```cpp
void Builder::set_intent(
    C2paBuilderIntent intent,
    C2paDigitalSourceType digital_source_type = Empty
);
```

Where:
- `intent` is one of the [intent types](#intent-types).
- `digital_source_type` is one of the [`C2paDigitalSourceType` values](#c2padigitalsourcetype) that describe how the asset was made. Required for the `Create` intent. Defaults to `Empty`.


### Intent types

Intent types can be any `C2paBuilderIntent` values: 

| Intent   | Operation | Parent ingredient  | Auto-generated action  |
|----------|-----------|--------------------|------------------------|
| `Create` | Brand-new content | Must *not* have one | `c2pa.created` |
| `Edit`   | Modifying existing content | Automatically created from the source stream if not provided | `c2pa.opened` (linked to parent) |
| `Update` | Metadata-only changes | Automatically created from the source stream if not provided | `c2pa.opened` (linked to parent) |

### C2paDigitalSourceType

| Enum value | Description |
| --- | --- |
| `Empty` | No source type specified. The default value. |
| `DigitalCapture` | Captured from a real-world source using a digital device |
| `TrainedAlgorithmicMedia` | Created by a trained algorithm (e.g., generative AI) |
| `DigitalCreation` | Created digitally (e.g., drawing software) |
| `CompositeWithTrainedAlgorithmicMedia` | Composite that includes trained algorithmic media |
| `AlgorithmicallyEnhanced` | Enhanced by an algorithm |
| `ScreenCapture` | Captured from a screen |
| `VirtualRecording` | Recorded from a virtual environment |
| `Composite` | Composed from multiple sources |
| `CompositeCapture` | Composite of captured sources |
| `CompositeSynthetic` | Composite of synthetic sources |
| `DataDrivenMedia` | Generated from data |
| `AlgorithmicMedia` | Created by an algorithm |
| `HumanEdits` | Human-edited content |
| `ComputationalCapture` | Captured with computational processing |
| `NegativeFilm` | Scanned from negative film |
| `PositiveFilm` | Scanned from positive film |
| `Print` | Scanned from a print |
| `TrainedAlgorithmicData` | Data created by a trained algorithm |

## Choosing the right intent

```mermaid
flowchart TD
    Start([Start]) --> HasParent{Does the asset have
    prior history?}
    HasParent --> |No| IsNew[Brand-new content]
    IsNew --> CREATE["Use Create
    + C2paDigitalSourceType"]
    HasParent --> |Yes| ContentChanged{Will the content
    itself change?}
    ContentChanged --> |Yes| EDIT[Use Edit]
    ContentChanged --> |No, metadata only| UPDATE[Use Update]
    ContentChanged --> |Need full manual control| MANUAL["Skip intents.
    Define actions and ingredients
    directly in manifest JSON."]
```

## Create intent

Use the `Create` intent when the asset has no prior history. Specify a `C2paDigitalSourceType` to describe how the asset was produced. `Builder` will:

- Add a `c2pa.created` action with the specified digital source type.
- Reject the operation if a `parentOf` ingredient exists.

### Example: New digital creation

Using `Context`:

```cpp
c2pa::Context context(R"({
    "version": 1,
    "builder": {"intent": {"Create": "digitalCreation"}}
})");

c2pa::Builder builder(context, R"({})");
builder.sign(source_path, output_path, signer);
```

Using `set_intent`:

```cpp
c2pa::Context context;
c2pa::Builder builder(context, R"({})");
builder.set_intent(Create, DigitalCreation);
builder.sign(source_path, output_path, signer);
```

### Example: Marking AI-generated content

```cpp
c2pa::Context context(R"({
    "version": 1,
    "builder": {"intent": {"Create": "trainedAlgorithmicMedia"}}
})");

c2pa::Builder builder(context, R"({})");
builder.sign(source_path, output_path, signer);
```

### Example: Create with additional manifest metadata

A `Context` and a manifest definition can be combined. The `Context` handles the intent; the manifest definition provides additional metadata and assertions:

```cpp
c2pa::Context context(R"({
    "version": 1,
    "builder": {
        "intent": {"Create": "digitalCapture"},
        "claim_generator_info": {"name": "an_app", "version": "0.1.0"}
    }
})");

std::string manifest_def = R"({
    "title": "My New Image",
    "assertions": [
        {
            "label": "cawg.training-mining",
            "data": {
                "entries": {
                    "cawg.ai_inference": {"use": "notAllowed"},
                    "cawg.ai_generative_training": {"use": "notAllowed"}
                }
            }
        }
    ]
})";

c2pa::Builder builder(context, manifest_def);
builder.sign(source_path, output_path, signer);
```

## Edit intent

Use the `Edit` intent when you modify an existing asset. With this intent, `Builder`:

1. Checks whether the caller has already added a `parentOf` ingredient. If not, it automatically creates one from the source stream passed to `sign()`.
2. Adds a `c2pa.opened` action linked to the parent ingredient.

No `digital_source_type` parameter is needed.

### Example: Editing an asset

Using `Context`:

```cpp
c2pa::Context context(R"({
    "version": 1,
    "builder": {"intent": "edit"}
})");

c2pa::Builder builder(context, R"({})");
// The Builder reads "original.jpg" as the parent ingredient,
// then writes the new manifest into "edited.jpg"
builder.sign("original.jpg", "edited.jpg", signer);
```

Using `set_intent`:

```cpp
c2pa::Context context;
c2pa::Builder builder(context, R"({})");
builder.set_intent(Edit);
builder.sign("original.jpg", "edited.jpg", signer);
```

The resulting manifest contains one ingredient with `relationship: "parentOf"` pointing to `original.jpg` and a `c2pa.opened` action referencing that ingredient. If the source file already has a C2PA manifest, the ingredient preserves the full provenance chain.

### Example: Editing with a manually-added parent

To control the parent ingredient's metadata (for example, to set a title or use a different source), add it explicitly:

```cpp
c2pa::Context context(R"({
    "version": 1,
    "builder": {"intent": "edit"}
})");

c2pa::Builder builder(context, R"({})");

std::ifstream original("original.jpg", std::ios::binary);
builder.add_ingredient(
    R"({"title": "Original Photo", "relationship": "parentOf"})",
    "image/jpeg",
    original
);

builder.sign("canvas.jpg", "edited.jpg", signer);
```

### Example: Editing with additional component ingredients

A parent ingredient can be combined with component or input ingredients. The intent creates the `c2pa.opened` action for the parent; additional actions can reference components (`componentOf`) or inputs (`inputTo`):

```cpp
c2pa::Context context(R"({
    "version": 1,
    "builder": {"intent": "edit"}
})");

std::string manifest_def = R"({
    "assertions": [
        {
            "label": "c2pa.actions.v2",
            "data": {
                "actions": [
                    {
                        "action": "c2pa.placed",
                        "parameters": {"ingredientIds": ["overlay_label"]}
                    }
                ]
            }
        }
    ]
})";

c2pa::Builder builder(context, manifest_def);

// The Builder auto-creates a parent from the source stream
// and generates a c2pa.opened action for it.

// Add a component ingredient manually.
std::ifstream overlay("overlay.png", std::ios::binary);
builder.add_ingredient(
    R"({
        "title": "overlay.png",
        "relationship": "componentOf",
        "label": "overlay_label"
    })",
    "image/png",
    overlay
);

builder.sign("original.jpg", "composite.jpg", signer);
```

## Update intent

Use the `Update` intent for metadata-only changes, where the asset content itself does not change. This is a restricted form of the `Edit` intent that:

- Allows exactly one ingredient (the parent).
- Does not allow changes to the parent's hashed content.
- Produces a more compact manifest than `Edit`.

As with the `Edit` intent, `Builder` automatically creates a parent ingredient from the source stream if the caller has not provided one.

### Example: Adding metadata to a signed asset

Using `Context`:

```cpp
c2pa::Context context(R"({
    "version": 1,
    "builder": {"intent": "update"}
})");

c2pa::Builder builder(context, R"({})");
builder.sign("signed_asset.jpg", "updated_asset.jpg", signer);
```

Using `set_intent`:

```cpp
c2pa::Context context;
c2pa::Builder builder(context, R"({})");
builder.set_intent(Update);
builder.sign("signed_asset.jpg", "updated_asset.jpg", signer);
```

