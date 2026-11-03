# Release notes

## 26 Oct 2026

The selected native SDK release is c2pa-rs **0.91.7**, including its eMudhra
TLS distrust updates. CMake downloads and the Emscripten example's `curl`
download use the build host's TLS implementation and trust store, not the
SDK's roots. Keep those tools and their CA stores updated separately (including
`ca-certificates` in container builds).

Applications that supply `ContextBuilder::with_http_resolver` are responsible
for their resolver's TLS verification and trust-store updates. In particular,
the Emscripten example delegates HTTP to `emscripten_fetch`, so its HTTPS trust
is controlled by the browser or JavaScript host, not the Rust SDK. Updating the
SDK alone does not update those stores. See the
[eMudhra incident guidance](https://emudhra.we-have-a-problem.com/) for affected
certificate chains and patched trust-store versions.

Use the `c2pa-v0.91.7` tag for the matching native release. Local-source builds
use the checkout supplied through `C2PA_RS_PATH`; they do not select or update
that checkout automatically. Likewise, consumers using `C2PA_C_LIB_OVERRIDE`
must update their replacement native library themselves.

## 3 Mar 2026

This release includes changes relating to trust validation and handling (made in the Rust library [v0.68.0](https://github.com/contentauth/c2pa-rs/releases/tag/c2pa-v0.68.0), see also [trust lists](https://opensource.contentauthenticity.org/docs/conformance/trust-lists) for more details on trust lists).

Additionally, this release adds support for:
- [Configuring the SDK using `Context` and `Settings`](context-settings.md).
- Using [working stores and archives](working-stores.md).
- Using `Builder` and `Reader` together to [selectively construct a manifest by filtering actions and ingredients](selective-manifests.md).

For answers to frequently asked questions about these features, see the [FAQs](faqs.md).

