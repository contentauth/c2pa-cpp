# Release notes

## Unreleased

### Breaking changes

- Removed deprecated `Builder::data_hashed_placeholder`,
  `Builder::sign_data_hashed_embeddable`, and `Builder::format_embeddable`
  for compatibility with c2pa-rs 0.92.0. Use the context-signer workflow in
  [Embeddable manifests](embeddable-api.md#removed-methods).
- Removed deprecated `c2pa::load_settings`. Configure each `Context` using
  `Settings` or `Context::ContextBuilder` instead; settings are no longer
  thread-global.
- Stream wrappers now hold opaque `C2paStream` handles rather than inheriting
  from the C struct. Use their existing `c_stream` member when calling the C
  API, and rebuild C++ consumers against the updated headers.

Non-deprecated method names and signatures are unchanged. Development uses a
local Rust source build; the default prebuilt-library version has not yet
been updated.

## 3 Mar 2026

This release includes changes relating to trust validation and handling (made in the Rust library [v0.68.0](https://github.com/contentauth/c2pa-rs/releases/tag/c2pa-v0.68.0), see also [trust lists](https://opensource.contentauthenticity.org/docs/conformance/trust-lists) for more details on trust lists).

Additionally, this release adds support for:
- [Configuring the SDK using `Context` and `Settings`](context-settings.md).
- Using [working stores and archives](working-stores.md).
- Using `Builder` and `Reader` together to [selectively construct a manifest by filtering actions and ingredients](selective-manifests.md).

For answers to frequently asked questions about these features, see the [FAQs](faqs.md).
